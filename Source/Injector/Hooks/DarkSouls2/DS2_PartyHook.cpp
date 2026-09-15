/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_PartyHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include "ThirdParty/detours/src/detours.h"
#endif

namespace
{
#ifdef _WIN32

    // Version 1.03 Calibrations 2.02, both verified before anything is written.
    //
    // The NetSvrSummonSignManager is reached the way the game's own getter
    // does (FUN_1405132a0: *(*0x141616cf8 + 0x30) is the NetSvrManager), then
    // its field +0x78, and it is believed only with its vftable. Measured the
    // same on both instances, 14/09.
    //
    //   +0x2a1410  "place my sign": maps the type through the player's state
    //              (FUN_14029c9b0), then FUN_1402a2780, which
    //              checks again, builds the cell and matching parameters,
    //              calls NetSvrSummonSignInterface::CreateSummonSign and keeps
    //              the sign at manager+0x18 (placed) / +0x24 (handle) / +0x40
    //              (type). A second call replaces the sign, as a second use of
    //              the soapstone does.
    //
    // Traced 14/09: a real White Sign Soapstone use reached CreateSummonSign
    // from +0x2a2b98 (inside FUN_1402a2780), and the evaluation passes type 1.
    constexpr size_t kPlaceOffset = 0x2a1410;
    constexpr uint8_t kPlacePrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x20, 0x56, 0x57 };

    // SummonSignSetCtrl's update, called every frame on host and guest alike
    // (a re-armed breakpoint hit it in every half-second window on both). The
    // orders run here, so they run on the game's thread. The evaluation the
    // soapstone uses (+0x29fff0) was the first choice and never runs on a
    // player without a soapstone in hand.
    constexpr size_t kTickOffset = 0x2139d0;
    constexpr uint8_t kTickPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };

    constexpr size_t kNetSvrGlobal = 0x1616cf8;
    constexpr size_t kNetSvrField = 0x30;
    constexpr size_t kSummonSignField = 0x78;
    constexpr size_t kSummonSignVftable = 0x10d61f8;

    constexpr size_t kPlacedOffset = 0x18;   // byte
    constexpr size_t kHandleOffset = 0x24;   // uint32
    constexpr size_t kTypeOffset = 0x40;     // byte

    using SignMethod_p = void(*)(void* Manager, uint64_t Type);
    using Tick_p = void(*)(void* Self);
    Tick_p s_original_tick = nullptr;
    SignMethod_p s_place = nullptr;
    uintptr_t s_base = 0;

    constexpr int kNothing = -1;
    std::atomic<void*> s_manager{ nullptr };
    std::atomic<int> s_pending_place{ kNothing };
    std::atomic<bool> s_pending_status{ false };
    std::atomic<bool> s_running{ false };
    std::thread s_thread;

    std::filesystem::path s_log_path;
    std::filesystem::path s_request_path;

    void Append(const std::string& Text)
    {
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream)
        {
            Stream << Text;
        }
    }

    std::string Clock()
    {
        SYSTEMTIME Now;
        GetLocalTime(&Now);
        return StringFormat("%02u:%02u:%02u.%03u", Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds);
    }

    std::string Describe(void* Manager)
    {
        const uint8_t* Bytes = (const uint8_t*)Manager;
        uint32_t Handle = 0;
        memcpy(&Handle, Bytes + kHandleOffset, sizeof(Handle));
        return StringFormat("placa %s, alca %08x, tipo %u", Bytes[kPlacedOffset] != 0 ? "posta" : "nenhuma",
            Handle, (unsigned)Bytes[kTypeOffset]);
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

    void* ResolveManager()
    {
        uintptr_t Global = 0, NetSvr = 0, Manager = 0, Vftable = 0;
        if (!ReadPointer(s_base + kNetSvrGlobal, Global) || Global == 0) { return nullptr; }
        if (!ReadPointer(Global + kNetSvrField, NetSvr) || NetSvr == 0) { return nullptr; }
        if (!ReadPointer(NetSvr + kSummonSignField, Manager) || Manager == 0) { return nullptr; }
        if (!ReadPointer(Manager, Vftable) || Vftable != s_base + kSummonSignVftable) { return nullptr; }
        return (void*)Manager;
    }

    // Runs on the game's thread, every frame.
    void TickHook(void* Self)
    {
        s_original_tick(Self);

        if (s_pending_place.load() == kNothing && !s_pending_status.load())
        {
            return;
        }
        void* Manager = ResolveManager();
        s_manager.store(Manager);
        if (Manager == nullptr)
        {
            // Stays pending: the manager exists once the player is online.
            return;
        }

        const int Wanted = s_pending_place.exchange(kNothing);
        if (Wanted != kNothing)
        {
            const std::string Before = Describe(Manager);
            s_place(Manager, (uint64_t)(uint8_t)Wanted);
            Append(StringFormat("%s  pedido: placa tipo %d; antes: %s; depois: %s\n",
                Clock().c_str(), Wanted, Before.c_str(), Describe(Manager).c_str()));
        }
        if (s_pending_status.exchange(false))
        {
            Append(StringFormat("%s  === status: manager %p, %s ===\n", Clock().c_str(), Manager, Describe(Manager).c_str()));
        }
    }

    bool BytesMatch(uintptr_t Address, const uint8_t* Expected, size_t Length)
    {
        return memcmp((const void*)Address, Expected, Length) == 0;
    }

    void Run()
    {
        while (s_running.load())
        {
            std::error_code Error;
            if (std::filesystem::exists(s_request_path, Error))
            {
                std::ifstream Stream(s_request_path);
                std::string Line;
                while (std::getline(Stream, Line))
                {
                    int Type = 0;
                    if (sscanf(Line.c_str(), "placa %d", &Type) == 1 && Type >= 0 && Type < 0x14)
                    {
                        s_pending_place.store(Type);
                        Append(StringFormat("%s  === pedido de placa tipo %d, no proximo quadro ===\n", Clock().c_str(), Type));
                    }
                    else if (Line.rfind("status", 0) == 0)
                    {
                        s_pending_status.store(true);
                        if (ResolveManager() == nullptr)
                        {
                            Append(StringFormat("%s  === status: manager nao resolvido ainda (fica pendente) ===\n", Clock().c_str()));
                        }
                    }
                    else if (!Line.empty())
                    {
                        Append(StringFormat("%s  === pedido desconhecido: %s ===\n", Clock().c_str(), Line.c_str()));
                    }
                }
                Stream.close();
                std::filesystem::remove(s_request_path, Error);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

#endif
}

void* DS2_PartyHook_SignManager()
{
#ifdef _WIN32
    return s_base == 0 ? nullptr : ResolveManager();
#else
    return nullptr;
#endif
}

bool DS2_PartyHook::Install(Injector& injector)
{
#ifdef _WIN32
    const uintptr_t Base = (uintptr_t)injector.GetBaseAddress();
    const uintptr_t Tick = Base + kTickOffset;
    const uintptr_t Place = Base + kPlaceOffset;

    if (!BytesMatch(Tick, kTickPrologue, sizeof(kTickPrologue)) || !BytesMatch(Place, kPlacePrologue, sizeof(kPlacePrologue)))
    {
        Error("[DS2_PartyHook] o codigo em +0x%zx ou +0x%zx nao e o esperado; recusando", kTickOffset, kPlaceOffset);
        return false;
    }
    s_base = Base;

    s_log_path = injector.GetDllPath() / "DS2_Party.log";
    s_request_path = injector.GetDllPath() / "DS2_Party.req";

    s_original_tick = (Tick_p)Tick;
    s_place = (SignMethod_p)Place;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_tick, TickHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        Error("[DS2_PartyHook] nao consegui instalar o detour");
        return false;
    }

    s_running.store(true);
    s_thread = std::thread(Run);

    Append(StringFormat("%s  === ds2os party ===\n", Clock().c_str()));
    Log("[DS2_PartyHook] pronto; DS2_Party.req aceita 'placa <tipo>' e 'status'");
#endif
    return true;
}

void DS2_PartyHook::Uninstall()
{
#ifdef _WIN32
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }
    if (s_original_tick != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_tick, TickHook);
        DetourTransactionCommit();
        s_original_tick = nullptr;
    }
#endif
}

const char* DS2_PartyHook::GetName()
{
    return "DS2 Party";
}
