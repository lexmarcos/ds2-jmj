/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_BonfireInSessionHook.h"
#include "Injector/Hooks/DarkSouls2/DS2_CoopChannelHook.h"
#include "Injector/Hooks/DarkSouls2/DS2_SeamlessCoopHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "Shared/Core/Utils/Strings.h"

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#include "ThirdParty/detours/src/detours.h"
#endif

namespace
{
#if defined(_WIN32) && defined(_M_X64)

    // Version 1.03 Calibrations 2.02 (docs/DS2_SEAMLESS_COOP_TASKS.md, M8).
    //
    //   +0x25f690  FUN_14025f690(session): FUN_14025ed80 - 1 < 2
    //   +0x1cb9d9  the return address of its call in FUN_1401cb950 (the rest),
    //              followed by `test al,al ; jne` to message 0x453
    //   +0x199c2e  the return address of its call in FUN_140199a70 (the menu
    //              queue, state 10), followed by `test al,al ; je` past the
    //              cancel
    //   +0x17ee9d  FUN_14017ed90, rest job state 2: `je +0x17eeb8` after
    //              FUN_14025ea40; taken always, the menu is not cancelled
    constexpr size_t kSessionUpOffset = 0x25f690;
    constexpr uint8_t kSessionUpPrologue[] = { 0x48, 0x83, 0xec, 0x28, 0xe8, 0xe7, 0xf6, 0xff, 0xff, 0xff, 0xc8, 0x83, 0xf8, 0x01 };
    constexpr size_t kRestReturn = 0x1cb9d9;
    constexpr uint8_t kRestAfter[] = { 0x84, 0xc0, 0x75, 0x28 };
    constexpr size_t kQueueReturn = 0x199c2e;
    constexpr uint8_t kQueueAfter[] = { 0x84, 0xc0, 0x74, 0x08 };
    constexpr size_t kJobBranch = 0x17ee9d;
    constexpr uint8_t kJobExpected[] = { 0x74, 0x19 };
    constexpr uint8_t kJobPatch[] = { 0xeb, 0x19 };

    // The rest, host side (measured 15/09, docs/DS2_SEAMLESS_COOP_TASKS.md M8):
    //
    //   +0x17dc40  FUN_14017dc40(EventBonfireManager, bonfire id): starts the
    //              rest, returns 1 when it did (state 0 -> 1)
    //   +0x17fd70  FUN_14017fd70(): the world reset of a rest, run on state
    //              1 -> 2; enemy generators (FUN_140417210), map objects
    //              (FUN_1403c27f0) and the event manager (FUN_14044f880). Takes
    //              nothing, reads the globals.
    //
    // Measured without this: an enemy killed in the host's world came back on
    // the host when it rested and stayed dead on the guest.
    constexpr size_t kRestStartOffset = 0x17dc40;
    constexpr uint8_t kRestStartPrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x83, 0x79, 0x38, 0x00, 0x48, 0x8b, 0xd9 };
    constexpr size_t kWorldResetOffset = 0x17fd70;
    constexpr uint8_t kWorldResetPrologue[] = { 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x05, 0x75, 0x4b, 0x49, 0x01, 0x48, 0x8b, 0x48, 0x40 };

    // A message box with text of our own, the way FUN_1402d6540 shows the
    // network errors: FUN_1404fe2a0(*(ctx+0x22e0), text, title, 1, 1), the
    // title from FUN_140503620(0, 0xcc).
    constexpr size_t kDialogOffset = 0x4fe2a0;
    constexpr uint8_t kDialogPrologue[] = { 0x40, 0x53, 0x48, 0x81, 0xec, 0xb0, 0x00, 0x00, 0x00, 0x0f, 0xb6, 0x84, 0x24, 0xe0 };
    constexpr size_t kTextOffset = 0x503620;
    constexpr uint8_t kTextPrologue[] = { 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x41, 0x56 };
    constexpr size_t kFrontEnd = 0x22e0;
    constexpr int kTitleCategory = 0;
    constexpr int kTitleId = 0xcc;
    constexpr const wchar_t* kRestNotice = L"A player is resting at a bonfire.";
    constexpr const wchar_t* kTravelNotice = L"The host is travelling. You will join again at the destination.";
    constexpr const wchar_t* kTravelCanceled = L"Travel canceled: other players are still in your world.";

    // The guest's in-session update (NetSummonJoinMultiplayCtrl, state 7):
    // a nonzero +0x120 makes it end the session with reason 3 on its next
    // frame, the same end a host that travelled caused on 15/09, which cost the
    // guest no penalty (armed 1 -> 0, points 40 -> 40).
    constexpr size_t kGuestUpdateOffset = 0x2c3830;
    constexpr uint8_t kGuestUpdatePrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x30, 0x83, 0xb9, 0x20, 0x01, 0x00, 0x00, 0x00 };
    constexpr size_t kGuestLeaveFlag = 0x120;
    constexpr ULONGLONG kTravelSettleMs = 1500;
    constexpr ULONGLONG kTravelGiveUpMs = 20000;

    // The local character's role: *(*0x1416148f0 + 0xd0) -> +0xb0 -> +0x3c.
    constexpr size_t kGameGlobal = 0x16148f0;
    constexpr size_t kLocalCharacter = 0xd0;
    constexpr size_t kRoles = 0xb0;
    constexpr size_t kRole = 0x3c;

    using SessionUp_p = uint64_t(*)(void* Session);
    SessionUp_p s_original = nullptr;
    using RestStart_p = uint64_t(*)(void* Manager, int32_t Bonfire);
    RestStart_p s_original_rest = nullptr;
    using WorldReset_p = void(*)();
    WorldReset_p s_original_reset = nullptr;
    using Dialog_p = uint32_t(*)(void* FrontEnd, const wchar_t* Text, const wchar_t* Title, uint8_t A, uint8_t B);
    Dialog_p s_dialog = nullptr;
    using Text_p = const wchar_t*(*)(int Category, int Id);
    Text_p s_text = nullptr;
    bool s_replaying = false;   // game thread only
    using GuestUpdate_p = void(*)(void* Ctrl, float Delta);
    GuestUpdate_p s_original_guest_update = nullptr;
    std::atomic<bool> s_leave_requested{ false };
    struct HeldTravel
    {
        bool Active = false;
        void* Context = nullptr;
        uint8_t Request[0x38] = {};
        uint8_t Flag = 0;
        ULONGLONG Since = 0;
        ULONGLONG GuestsGone = 0;
    };
    HeldTravel s_travel;   // game thread only
    std::atomic<bool> s_events_ready{ false };
    std::filesystem::path s_log_path;
    std::mutex s_log_mutex;
    uintptr_t s_base = 0;
    bool s_job_patched = false;
    std::atomic<uint64_t> s_answered{ 0 };

    bool ReadByte(uintptr_t At, uint8_t& Out)
    {
        __try
        {
            Out = *(const uint8_t*)At;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadPointer(uintptr_t At, uintptr_t& Out)
    {
        __try
        {
            Out = *(const uintptr_t*)At;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool OwnsTheWorld()
    {
        uintptr_t Context = 0, Character = 0, Roles = 0;
        uint8_t Role = 0xff;
        return ReadPointer(s_base + kGameGlobal, Context) && Context != 0 &&
            ReadPointer(Context + kLocalCharacter, Character) && Character != 0 &&
            ReadPointer(Character + kRoles, Roles) && Roles != 0 &&
            ReadByte(Roles + kRole, Role) && Role == 0;
    }

    void Append(const std::string& Text)
    {
        std::scoped_lock Lock(s_log_mutex);
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream)
        {
            SYSTEMTIME Now;
            GetLocalTime(&Now);
            Stream << StringFormat("%02u:%02u:%02u.%03u  ", Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds) << Text;
        }
    }

    uint64_t RestStartHook(void* Manager, int32_t Bonfire)
    {
        const uint64_t Started = s_original_rest(Manager, Bonfire);
        if ((uint8_t)Started != 0 && OwnsTheWorld())
        {
            DS2_CoopChannel::SendHostEvent(DS2_CoopChannel::HostEvent::RestStarted);
            Append(StringFormat("host: descanso na fogueira %08x; aviso para a sessao\n", (uint32_t)Bonfire));
        }
        return Started;
    }

    void WorldResetHook()
    {
        s_original_reset();
        if (!s_replaying && OwnsTheWorld())
        {
            DS2_CoopChannel::SendHostEvent(DS2_CoopChannel::HostEvent::WorldReset);
            Append("host: o mundo foi reiniciado pelo descanso; pedido para a sessao\n");
        }
    }

    void GuestUpdateHook(void* Ctrl, float Delta)
    {
        if (s_leave_requested.exchange(false) && Ctrl != nullptr)
        {
            int32_t* Flag = (int32_t*)((uint8_t*)Ctrl + kGuestLeaveFlag);
            const int32_t Before = *Flag;
            if (Before == 0)
            {
                *Flag = 1;
            }
            Append(StringFormat("convidado: saindo da sessao para o host viajar (+0x120 %d -> %d)\n", Before, *Flag));
        }
        s_original_guest_update(Ctrl, Delta);
    }

    void ShowMessage(const wchar_t* Text)
    {
        uintptr_t Context = 0, FrontEnd = 0;
        if (ReadPointer(s_base + kGameGlobal, Context) && Context != 0 &&
            ReadPointer(Context + kFrontEnd, FrontEnd) && FrontEnd != 0)
        {
            s_dialog((void*)FrontEnd, Text, s_text(kTitleCategory, kTitleId), 1, 1);
        }
    }

    uint64_t SessionUpHook(void* Session)
    {
        const uintptr_t Caller = (uintptr_t)_ReturnAddress();
        const uint64_t Answer = s_original(Session);
        if ((uint8_t)Answer != 0 && (Caller == s_base + kRestReturn || Caller == s_base + kQueueReturn) && OwnsTheWorld())
        {
            s_answered.fetch_add(1, std::memory_order_relaxed);
            return Answer & ~(uint64_t)0xff;
        }
        return Answer;
    }

    bool WriteCode(uintptr_t Address, const uint8_t* From, size_t Length)
    {
        DWORD Previous = 0;
        if (!VirtualProtect((void*)Address, Length, PAGE_EXECUTE_READWRITE, &Previous))
        {
            return false;
        }
        memcpy((void*)Address, From, Length);
        FlushInstructionCache(GetCurrentProcess(), (void*)Address, Length);
        DWORD Ignored = 0;
        VirtualProtect((void*)Address, Length, Previous, &Ignored);
        return true;
    }

    bool Matches(uintptr_t Address, const uint8_t* Expected, size_t Length)
    {
        return memcmp((const void*)Address, Expected, Length) == 0;
    }

#endif
}

bool DS2_BonfireInSessionHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    const uintptr_t Base = (uintptr_t)injector.GetBaseAddress();
    if (!Matches(Base + kSessionUpOffset, kSessionUpPrologue, sizeof(kSessionUpPrologue)) ||
        !Matches(Base + kRestReturn, kRestAfter, sizeof(kRestAfter)) ||
        !Matches(Base + kQueueReturn, kQueueAfter, sizeof(kQueueAfter)) ||
        !Matches(Base + kJobBranch, kJobExpected, sizeof(kJobExpected)))
    {
        Error("[DS2BonfireInSession] o codigo de uma das tres travas nao e o esperado; nao aplicado");
        return false;
    }
    s_base = Base;

    s_original = (SessionUp_p)(Base + kSessionUpOffset);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original, SessionUpHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        s_original = nullptr;
        Error("[DS2BonfireInSession] nao consegui instalar o detour");
        return false;
    }

    if (!WriteCode(Base + kJobBranch, kJobPatch, sizeof(kJobPatch)))
    {
        Error("[DS2BonfireInSession] nao consegui escrever em +0x%zx", (size_t)kJobBranch);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original, SessionUpHook);
        DetourTransactionCommit();
        s_original = nullptr;
        return false;
    }
    s_job_patched = true;

    // The guest's half: optional, the rest in session works without it.
    s_log_path = injector.GetDllPath() / "DS2_Bonfire.log";
    if (Matches(Base + kRestStartOffset, kRestStartPrologue, sizeof(kRestStartPrologue)) &&
        Matches(Base + kWorldResetOffset, kWorldResetPrologue, sizeof(kWorldResetPrologue)) &&
        Matches(Base + kDialogOffset, kDialogPrologue, sizeof(kDialogPrologue)) &&
        Matches(Base + kTextOffset, kTextPrologue, sizeof(kTextPrologue)) &&
        Matches(Base + kGuestUpdateOffset, kGuestUpdatePrologue, sizeof(kGuestUpdatePrologue)))
    {
        s_original_guest_update = (GuestUpdate_p)(Base + kGuestUpdateOffset);
        s_original_rest = (RestStart_p)(Base + kRestStartOffset);
        s_original_reset = (WorldReset_p)(Base + kWorldResetOffset);
        s_dialog = (Dialog_p)(Base + kDialogOffset);
        s_text = (Text_p)(Base + kTextOffset);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)s_original_rest, RestStartHook);
        DetourAttach(&(PVOID&)s_original_reset, WorldResetHook);
        DetourAttach(&(PVOID&)s_original_guest_update, GuestUpdateHook);
        if (DetourTransactionCommit() == NO_ERROR)
        {
            s_events_ready.store(true);
            Append("=== ds2os fogueira em sessao: descanso, reinicio do mundo e aviso ===\n");
        }
        else
        {
            s_original_rest = nullptr;
            s_original_reset = nullptr;
            s_original_guest_update = nullptr;
            Error("[DS2BonfireInSession] nao consegui instalar o aviso e o reinicio do convidado");
        }
    }
    else
    {
        Error("[DS2BonfireInSession] o codigo do descanso, do reinicio ou da caixa de mensagem nao e o esperado; so o host descansa");
    }
    Log("[DS2BonfireInSession] o dono do mundo descansa em fogueira com a sessao de pe");
#endif
    return true;
}

bool DS2_BonfireInSession_HoldTravel(void* Context, const uint8_t* Request, size_t Size, uint8_t Flag)
{
#if defined(_WIN32) && defined(_M_X64)
    if (!s_events_ready.load() || s_original_guest_update == nullptr || Size != sizeof(s_travel.Request) || !OwnsTheWorld())
    {
        return false;
    }
    if (s_travel.Active)
    {
        return true;
    }
    const size_t Guests = DS2_CoopChannel::GuestCount();
    if (Guests == 0)
    {
        return false;
    }
    s_travel = HeldTravel();
    s_travel.Active = true;
    s_travel.Context = Context;
    memcpy(s_travel.Request, Request, sizeof(s_travel.Request));
    s_travel.Flag = Flag;
    s_travel.Since = GetTickCount64();
    DS2_CoopChannel::SendHostEvent(DS2_CoopChannel::HostEvent::TravelLeave);
    Append(StringFormat("host: viagem segurada com %zu convidado(s); pedido para sairem\n", Guests));
    return true;
#else
    return false;
#endif
}

void DS2_BonfireInSession_Tick()
{
#if defined(_WIN32) && defined(_M_X64)
    if (!s_events_ready.load())
    {
        return;
    }
    const ULONGLONG Now = GetTickCount64();
    if (s_travel.Active)
    {
        const size_t Guests = DS2_CoopChannel::GuestCount();
        if (Guests != 0)
        {
            s_travel.GuestsGone = 0;
            if (Now - s_travel.Since > kTravelGiveUpMs)
            {
                s_travel.Active = false;
                ShowMessage(kTravelCanceled);
                Append(StringFormat("host: viagem cancelada; %zu convidado(s) ainda na sessao depois de %llu ms\n",
                    Guests, (unsigned long long)(Now - s_travel.Since)));
            }
        }
        else if (s_travel.GuestsGone == 0)
        {
            s_travel.GuestsGone = Now;
        }
        else if (Now - s_travel.GuestsGone >= kTravelSettleMs)
        {
            s_travel.Active = false;
            HeldTravel Travel = s_travel;
            const uint8_t Accepted = DS2_SeamlessCoop_ReplayWarp(Travel.Context, Travel.Request, sizeof(Travel.Request), Travel.Flag);
            Append(StringFormat("host: convidados fora ha %llu ms; viagem retomada, aceita=%u\n",
                (unsigned long long)(Now - Travel.GuestsGone), (unsigned)Accepted));
        }
    }
    if (OwnsTheWorld())
    {
        return;
    }
    DS2_CoopChannel::Bonfire Said;
    if (DS2_CoopChannel::TakeHostEvent(DS2_CoopChannel::HostEvent::TravelLeave, Said))
    {
        s_leave_requested.store(true);
        ShowMessage(kTravelNotice);
        Append(StringFormat("convidado: o host vai viajar (pedido ha %llu ms); saio da sessao\n", (unsigned long long)Said.AgeMs));
    }
    if (DS2_CoopChannel::TakeHostEvent(DS2_CoopChannel::HostEvent::RestStarted, Said))
    {
        ShowMessage(kRestNotice);
        Append(StringFormat("convidado: o host descansou na fogueira %08x (mapa %08x, ha %llu ms); aviso mostrado\n",
            Said.Id, Said.Map, (unsigned long long)Said.AgeMs));
    }
    if (DS2_CoopChannel::TakeHostEvent(DS2_CoopChannel::HostEvent::WorldReset, Said))
    {
        s_replaying = true;
        s_original_reset();
        s_replaying = false;
        Append(StringFormat("convidado: mundo do host reiniciado aqui tambem (pedido ha %llu ms)\n", (unsigned long long)Said.AgeMs));
    }
#endif
}

void DS2_BonfireInSessionHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    s_events_ready.store(false);
    if (s_original_rest != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_rest, RestStartHook);
        DetourDetach(&(PVOID&)s_original_reset, WorldResetHook);
        DetourDetach(&(PVOID&)s_original_guest_update, GuestUpdateHook);
        DetourTransactionCommit();
        s_original_rest = nullptr;
        s_original_reset = nullptr;
        s_original_guest_update = nullptr;
    }
    if (s_job_patched)
    {
        WriteCode(s_base + kJobBranch, kJobExpected, sizeof(kJobExpected));
        s_job_patched = false;
    }
    if (s_original != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original, SessionUpHook);
        DetourTransactionCommit();
        s_original = nullptr;
    }
#endif
}

const char* DS2_BonfireInSessionHook::GetName()
{
    return "DS2 Bonfire In Session";
}
