/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_BonfireInSessionHook.h"
#include "Injector/Hooks/DarkSouls2/DS2_CoopChannelHook.h"
#include "Injector/Hooks/DarkSouls2/DS2_RespawnInSessionHook.h"
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
    constexpr const wchar_t* kTravelQuestion = L"The host wants to travel to another bonfire. Travel together?";
    constexpr const wchar_t* kTravelDeclined = L"Travel canceled: a player declined.";
    constexpr const wchar_t* kTravelNoAnswer = L"Travel canceled: not every player answered.";

    // A Yes/No box the way FeSubStateCommonWindow opens one (FUN_140104db0):
    // FUN_1404fe1c0(frontend, text, yes, no, 1, 1, 1, 1) returns its number
    // (+0x324); FUN_140500440(frontend, n) says it closed, FUN_1404ff940
    // (frontend, n) which button (2 and 5 are the second, "No"), and
    // FUN_1404ff2e0 / FUN_1404fe960 (frontend, 0) put it away.
    constexpr size_t kChoiceOffset = 0x4fe1c0;
    constexpr uint8_t kChoicePrologue[] = { 0x40, 0x53, 0x48, 0x81, 0xec, 0xc0, 0x00, 0x00, 0x00, 0x0f, 0xb6, 0x84, 0x24, 0x08, 0x01 };
    constexpr size_t kClosedOffset = 0x500440;
    constexpr size_t kButtonOffset = 0x4ff940;
    constexpr uint8_t kByNumberPrologue[] = { 0x48, 0x8b, 0x81, 0xf0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x14, 0x85, 0xd2, 0x7e, 0x08 };
    constexpr size_t kCloseOffset = 0x4ff2e0;
    constexpr size_t kReleaseOffset = 0x4fe960;
    constexpr uint8_t kCloseByNumberPrologue[] = { 0x48, 0x8b, 0xc1, 0x48, 0x8b, 0x89, 0xf0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x11, 0x85, 0xd2 };
    constexpr size_t kDialogNumber = 0x324;
    constexpr int kYesText = 100;
    constexpr int kNoText = 0x65;
    constexpr ULONGLONG kVoteTimeoutMs = 30000;
    // A held travel leaves the bonfire menu open and invisible, with the rest
    // job waiting on it (measured 15/09: queue state 10, rest state 2, the host
    // sat with no menu). Canceling closes it the way the game cancels the
    // bonfire menu in a session: FUN_1401994e0(*(*(ctx+0x70)+0x50)).
    // The travel itself, *(*(ctx+0x70)+0x70), starts with the 0x38-byte warp
    // request (map at +0x08) and keeps its phase at +0x40. Picking a bonfire
    // puts it in phase 1 (FUN_140184bd0(travel, 1)); FUN_140184a10, its update,
    // then starts the load transition (FUN_1404815c0: HUD and menus put away,
    // the session told to wait) and asks for the warp. Holding the warp itself
    // was too late: with the transition started and no warp, the host was left
    // sitting with no menu for good (15/09). Holding phase 1 is before all of
    // it, and FUN_140184bd0(travel, 0) is the game's own way to drop a travel.
    constexpr size_t kTravelUpdateOffset = 0x184a10;
    constexpr uint8_t kTravelUpdatePrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0x0d, 0xd0, 0xfe, 0x48, 0x01 };
    constexpr size_t kTravelResetOffset = 0x184bd0;
    constexpr uint8_t kTravelResetPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0x0d, 0x0c, 0xfd, 0x48, 0x01 };
    constexpr size_t kTravelPhase = 0x40;
    constexpr size_t kTravelMap = 0x08;
    constexpr int32_t kTravelPicked = 1;
    constexpr size_t kMenuCancelOffset = 0x1994e0;
    constexpr uint8_t kMenuCancelPrologue[] = { 0x48, 0x8b, 0x05, 0x09, 0xb4, 0x47, 0x01, 0x48, 0x83, 0xb8, 0xe0, 0x22, 0x00, 0x00, 0x00 };
    constexpr size_t kEventManager = 0x70;
    // Choosing a destination in the travel menu writes it into the respawn
    // record (*(ctx+0x70): +0x164 map, +0x168 type, +0x16c id) before the warp
    // is asked for: a canceled travel left Chico's record on Heide's Ruin while
    // he stood at The Far Fire (15/09). The record is kept when the rest
    // starts and put back when a travel is canceled.
    constexpr size_t kRecordFields = 0x164;
    constexpr size_t kMenuQueue = 0x50;
    constexpr ULONGLONG kLeaveSettleMs = 1500;
    constexpr ULONGLONG kLeaveGiveUpMs = 20000;
    constexpr const wchar_t* kTravelStuck = L"Travel canceled: a player could not leave the session.";

    // The guest's session, NetSummonJoinMultiplayCtrl (vftable 0x1410d7bd8),
    // playing in state 7 (+0xf8). A nonzero +0x120 makes its state-7 handler
    // (FUN_1402c3830) end the session with reason 3 on the next frame: the
    // same end the guest got when a host travelled on 15/09 (from +0x2c385c),
    // armed 1 -> 0 with the penalty points unchanged.
    constexpr size_t kJoinCtrlVftable = 0x10d7bd8;
    constexpr size_t kJoinState = 0xf8;
    constexpr int32_t kJoinPlaying = 7;
    constexpr size_t kJoinLeave = 0x120;

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
    using Choice_p = int32_t(*)(void* FrontEnd, const wchar_t* Text, const wchar_t* Yes, const wchar_t* No, uint8_t A, uint8_t B, uint8_t C, uint8_t D);
    using ByNumber_p = uint64_t(*)(void* FrontEnd, int32_t Number);
    using CloseByNumber_p = void(*)(void* FrontEnd, int32_t Number);
    Choice_p s_choice = nullptr;
    ByNumber_p s_closed = nullptr;
    ByNumber_p s_button = nullptr;
    CloseByNumber_p s_close = nullptr;
    CloseByNumber_p s_release = nullptr;
    bool s_votes_ready = false;
    uint8_t s_record_at_rest[12] = {};
    bool s_record_kept = false;
    using MenuCancel_p = void(*)(void* Queue);
    MenuCancel_p s_menu_cancel = nullptr;
    using TravelUpdate_p = void(*)(void* Travel, float Delta);
    TravelUpdate_p s_original_travel = nullptr;
    using TravelReset_p = void(*)(void* Travel, int32_t Phase);
    TravelReset_p s_travel_reset = nullptr;

    // Host side, game thread only.
    struct HeldTravel
    {
        bool Active = false;
        bool Pass = false;
        void* Travel = nullptr;
        uint32_t Map = 0;
        uint32_t Vote = 0;
        size_t Guests = 0;
        ULONGLONG Since = 0;
        bool Leaving = false;
        ULONGLONG LeaveSince = 0;
        ULONGLONG GuestsGone = 0;
    };
    HeldTravel s_travel;
    uint32_t s_vote_counter = 0;

    // Guest side, game thread only.
    struct OpenVote
    {
        bool Active = false;
        uint32_t Vote = 0;
        int32_t Number = 0;
    };
    OpenVote s_open_vote;
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
            uintptr_t Context = 0, Events = 0;
            s_record_kept = ReadPointer(s_base + kGameGlobal, Context) && Context != 0 &&
                ReadPointer(Context + kEventManager, Events) && Events != 0;
            for (size_t i = 0; s_record_kept && i < sizeof(s_record_at_rest); ++i)
            {
                s_record_kept = ReadByte(Events + kRecordFields + i, s_record_at_rest[i]);
            }
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

    void* FrontEndOrNull()
    {
        uintptr_t Context = 0, FrontEnd = 0;
        if (ReadPointer(s_base + kGameGlobal, Context) && Context != 0 &&
            ReadPointer(Context + kFrontEnd, FrontEnd) && FrontEnd != 0)
        {
            return (void*)FrontEnd;
        }
        return nullptr;
    }

    void RestoreRecord()
    {
        uintptr_t Context = 0, Events = 0;
        if (!s_record_kept || !ReadPointer(s_base + kGameGlobal, Context) || Context == 0 ||
            !ReadPointer(Context + kEventManager, Events) || Events == 0)
        {
            return;
        }
        uint8_t Now[12] = {};
        for (size_t i = 0; i < sizeof(Now); ++i)
        {
            if (!ReadByte(Events + kRecordFields + i, Now[i]))
            {
                return;
            }
        }
        if (memcmp(Now, s_record_at_rest, sizeof(Now)) != 0)
        {
            memcpy((void*)(Events + kRecordFields), s_record_at_rest, sizeof(s_record_at_rest));
            uint32_t Map = 0, Id = 0;
            memcpy(&Map, s_record_at_rest, 4);
            memcpy(&Id, s_record_at_rest + 8, 4);
            Append(StringFormat("host: registro de renascimento devolvido para %08x/%08x\n", Map, Id));
        }
    }

    void TravelUpdateHook(void* Travel, float Delta)
    {
        if (Travel != nullptr && s_votes_ready && *(const int32_t*)((const uint8_t*)Travel + kTravelPhase) == kTravelPicked &&
            OwnsTheWorld())
        {
            if (s_travel.Pass)
            {
                s_original_travel(Travel, Delta);
                if (*(const int32_t*)((const uint8_t*)Travel + kTravelPhase) != kTravelPicked)
                {
                    s_travel.Pass = false;
                }
                return;
            }
            if (s_travel.Active)
            {
                return;   // held while the vote runs
            }
            const size_t Guests = DS2_CoopChannel::GuestCount();
            if (Guests != 0)
            {
                s_travel = HeldTravel();
                s_travel.Active = true;
                s_travel.Travel = Travel;
                memcpy(&s_travel.Map, (const uint8_t*)Travel + kTravelMap, sizeof(s_travel.Map));
                s_travel.Vote = ++s_vote_counter;
                s_travel.Guests = Guests;
                s_travel.Since = GetTickCount64();
                DS2_CoopChannel::SendHostEvent(DS2_CoopChannel::HostEvent::TravelVote, s_travel.Map, s_travel.Vote);
                Append(StringFormat("host: viagem para o mapa %08x segurada antes da transicao; votacao %u com %zu convidado(s)\n",
                    s_travel.Map, s_travel.Vote, Guests));
                return;
            }
        }
        s_original_travel(Travel, Delta);
    }

    void DropTravel()
    {
        if (s_travel.Travel != nullptr && s_travel_reset != nullptr &&
            *(const int32_t*)((const uint8_t*)s_travel.Travel + kTravelPhase) == kTravelPicked)
        {
            s_travel_reset(s_travel.Travel, 0);
        }
    }

    void CloseHeldMenu()
    {
        DropTravel();
        RestoreRecord();
        uintptr_t Context = 0, Events = 0, Queue = 0;
        if (s_menu_cancel != nullptr && ReadPointer(s_base + kGameGlobal, Context) && Context != 0 &&
            ReadPointer(Context + kEventManager, Events) && Events != 0 &&
            ReadPointer(Events + kMenuQueue, Queue) && Queue != 0)
        {
            s_menu_cancel((void*)Queue);
        }
    }

    void ShowMessage(const wchar_t* Text)
    {
        if (void* FrontEnd = FrontEndOrNull())
        {
            s_dialog(FrontEnd, Text, s_text(kTitleCategory, kTitleId), 1, 1);
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
        Matches(Base + kTextOffset, kTextPrologue, sizeof(kTextPrologue)))
    {
        s_votes_ready = Matches(Base + kChoiceOffset, kChoicePrologue, sizeof(kChoicePrologue)) &&
            Matches(Base + kClosedOffset, kByNumberPrologue, sizeof(kByNumberPrologue)) &&
            Matches(Base + kButtonOffset, kByNumberPrologue, sizeof(kByNumberPrologue)) &&
            Matches(Base + kCloseOffset, kCloseByNumberPrologue, sizeof(kCloseByNumberPrologue)) &&
            Matches(Base + kReleaseOffset, kCloseByNumberPrologue, sizeof(kCloseByNumberPrologue)) &&
            Matches(Base + kMenuCancelOffset, kMenuCancelPrologue, sizeof(kMenuCancelPrologue)) &&
            Matches(Base + kTravelUpdateOffset, kTravelUpdatePrologue, sizeof(kTravelUpdatePrologue)) &&
            Matches(Base + kTravelResetOffset, kTravelResetPrologue, sizeof(kTravelResetPrologue));
        s_travel_reset = (TravelReset_p)(Base + kTravelResetOffset);
        s_original_travel = (TravelUpdate_p)(Base + kTravelUpdateOffset);
        s_menu_cancel = (MenuCancel_p)(Base + kMenuCancelOffset);
        s_choice = (Choice_p)(Base + kChoiceOffset);
        s_closed = (ByNumber_p)(Base + kClosedOffset);
        s_button = (ByNumber_p)(Base + kButtonOffset);
        s_close = (CloseByNumber_p)(Base + kCloseOffset);
        s_release = (CloseByNumber_p)(Base + kReleaseOffset);
        s_original_rest = (RestStart_p)(Base + kRestStartOffset);
        s_original_reset = (WorldReset_p)(Base + kWorldResetOffset);
        s_dialog = (Dialog_p)(Base + kDialogOffset);
        s_text = (Text_p)(Base + kTextOffset);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)s_original_rest, RestStartHook);
        DetourAttach(&(PVOID&)s_original_reset, WorldResetHook);
        if (s_votes_ready)
        {
            DetourAttach(&(PVOID&)s_original_travel, TravelUpdateHook);
        }
        if (DetourTransactionCommit() == NO_ERROR)
        {
            s_events_ready.store(true);
            Append(StringFormat("=== ds2os fogueira em sessao: descanso, reinicio do mundo, aviso e votacao de viagem (%s) ===\n",
                s_votes_ready ? "votacao pronta" : "sem votacao: a caixa sim/nao nao e a esperada"));
        }
        else
        {
            s_original_rest = nullptr;
            s_original_reset = nullptr;
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
        size_t Yes = 0, No = 0;
        DS2_CoopChannel::GuestAnswers(s_travel.Vote, Yes, No);
        const size_t Guests = DS2_CoopChannel::GuestCount();
        if (No > 0)
        {
            s_travel.Active = false;
            CloseHeldMenu();
            ShowMessage(kTravelDeclined);
            Append(StringFormat("host: votacao %u recusada (%zu sim, %zu nao); viagem cancelada\n", s_travel.Vote, Yes, No));
        }
        else if (s_travel.Leaving)
        {
            if (Guests != 0)
            {
                s_travel.GuestsGone = 0;
                if (Now - s_travel.LeaveSince > kLeaveGiveUpMs)
                {
                    s_travel.Active = false;
                    CloseHeldMenu();
                    ShowMessage(kTravelStuck);
                    Append(StringFormat("host: votacao %u: %zu convidado(s) ainda na sessao depois de %llu ms; viagem cancelada\n",
                        s_travel.Vote, Guests, (unsigned long long)(Now - s_travel.LeaveSince)));
                }
            }
            else if (s_travel.GuestsGone == 0)
            {
                s_travel.GuestsGone = Now;
            }
            else if (Now - s_travel.GuestsGone >= kLeaveSettleMs)
            {
                s_travel.Active = false;
                s_travel.Pass = true;
                Append(StringFormat("host: convidados fora da sessao em %llu ms; viagem para %08x liberada\n",
                    (unsigned long long)(s_travel.GuestsGone - s_travel.LeaveSince), s_travel.Map));
            }
        }
        else if (Guests == 0 || Yes >= Guests)
        {
            s_travel.Leaving = true;
            s_travel.LeaveSince = Now;
            DS2_CoopChannel::SendHostEvent(DS2_CoopChannel::HostEvent::TravelLeave);
            Append(StringFormat("host: votacao %u aprovada (%zu sim de %zu) em %llu ms; convidados saem da sessao\n",
                s_travel.Vote, Yes, Guests, (unsigned long long)(Now - s_travel.Since)));
        }
        else if (Now - s_travel.Since > kVoteTimeoutMs)
        {
            s_travel.Active = false;
            CloseHeldMenu();
            ShowMessage(kTravelNoAnswer);
            Append(StringFormat("host: votacao %u sem resposta de todos (%zu sim de %zu); viagem cancelada\n", s_travel.Vote, Yes, Guests));
        }
    }

    if (s_open_vote.Active)
    {
        void* FrontEnd = FrontEndOrNull();
        const bool Ours = FrontEnd != nullptr && *(const int32_t*)((const uint8_t*)FrontEnd + kDialogNumber) == s_open_vote.Number;
        if (!Ours || (uint8_t)s_closed(FrontEnd, s_open_vote.Number) != 0)
        {
            const uint64_t Button = Ours ? s_button(FrontEnd, s_open_vote.Number) : 2;
            const bool Yes = Ours && (uint32_t)Button != 2 && (uint32_t)Button != 5;
            if (Ours)
            {
                s_close(FrontEnd, 0);
                s_release(FrontEnd, 0);
            }
            s_open_vote.Active = false;
            DS2_CoopChannel::SendGuestAnswer(s_open_vote.Vote, Yes);
            Append(StringFormat("convidado: votacao %u respondida %s (botao %llu%s)\n", s_open_vote.Vote, Yes ? "sim" : "nao",
                (unsigned long long)Button, Ours ? "" : ", a caixa foi trocada"));
        }
    }

    if (OwnsTheWorld())
    {
        return;
    }
    DS2_CoopChannel::Bonfire Said;
    if (DS2_CoopChannel::TakeHostEvent(DS2_CoopChannel::HostEvent::TravelVote, Said) && s_votes_ready)
    {
        void* FrontEnd = FrontEndOrNull();
        if (FrontEnd == nullptr)
        {
            DS2_CoopChannel::SendGuestAnswer(Said.Id, false);
            Append(StringFormat("convidado: votacao %u sem frontend; respondo nao\n", Said.Id));
        }
        else
        {
            s_open_vote.Active = true;
            s_open_vote.Vote = Said.Id;
            s_open_vote.Number = s_choice(FrontEnd, kTravelQuestion, s_text(0, kYesText), s_text(0, kNoText), 1, 1, 1, 1);
            Append(StringFormat("convidado: o host quer viajar para o mapa %08x; votacao %u aberta (caixa %d)\n",
                Said.Map, Said.Id, s_open_vote.Number));
        }
    }
    if (DS2_CoopChannel::TakeHostEvent(DS2_CoopChannel::HostEvent::TravelLeave, Said))
    {
        const uintptr_t Session = (uintptr_t)DS2_RespawnInSession_PlayingSession();
        uintptr_t Vftable = 0;
        uint8_t State[4] = {};
        const bool Playing = Session != 0 && ReadPointer(Session, Vftable) && Vftable == s_base + kJoinCtrlVftable &&
            ReadByte(Session + kJoinState, State[0]) && ReadByte(Session + kJoinState + 1, State[1]) &&
            ReadByte(Session + kJoinState + 2, State[2]) && ReadByte(Session + kJoinState + 3, State[3]);
        int32_t StateValue = 0;
        memcpy(&StateValue, State, sizeof(StateValue));
        if (Playing && StateValue == kJoinPlaying)
        {
            int32_t* Leave = (int32_t*)(Session + kJoinLeave);
            const int32_t Before = *Leave;
            if (Before == 0)
            {
                *Leave = 1;
            }
            Append(StringFormat("convidado: votacao aprovada; saio da sessao para o host viajar (sessao %p, +0x120 %d -> %d)\n",
                (void*)Session, Before, *Leave));
        }
        else
        {
            Append(StringFormat("convidado: pedido de saida para a viagem, mas nao ha sessao jogando (sessao %p, vftable %s, estado %d)\n",
                (void*)Session, Vftable == s_base + kJoinCtrlVftable ? "certa" : "outra", StateValue));
        }
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
        if (s_votes_ready)
        {
            DetourDetach(&(PVOID&)s_original_travel, TravelUpdateHook);
        }
        DetourTransactionCommit();
        s_original_rest = nullptr;
        s_original_reset = nullptr;
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
