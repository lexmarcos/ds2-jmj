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
    // NetSvrSummonSignManager, two neighbouring methods with the same shape
    // (manager, sign type byte):
    //
    //   +0x29fff0  "could this sign be placed now": maps the type through the
    //              player's state (FUN_14029c9b0) and runs the usability check
    //              FUN_1402a1bf0. The item code calls it every frame, from
    //              +0x1a9176, with the manager in rcx — which is how the
    //              manager is found here, and why this is the game thread.
    //   +0x2a1410  "place it": the same mapping, then FUN_1402a2780, which
    //              checks again, builds the cell and matching parameters,
    //              calls NetSvrSummonSignInterface::CreateSummonSign and keeps
    //              the sign at manager+0x18 (placed) / +0x24 (handle) / +0x40
    //              (type). A second call replaces the sign, as a second use of
    //              the soapstone does.
    //
    // Traced 14/09: a real White Sign Soapstone use reached CreateSummonSign
    // from +0x2a2b98 (inside FUN_1402a2780), and the evaluation passes type 1.
    constexpr size_t kCanPlaceOffset = 0x29fff0;
    constexpr size_t kPlaceOffset = 0x2a1410;
    constexpr uint8_t kPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x20, 0x56, 0x57 };

    constexpr size_t kPlacedOffset = 0x18;   // byte
    constexpr size_t kHandleOffset = 0x24;   // uint32
    constexpr size_t kTypeOffset = 0x40;     // byte

    using SignMethod_p = void(*)(void* Manager, uint64_t Type);
    SignMethod_p s_original_can_place = nullptr;
    SignMethod_p s_place = nullptr;

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

    // Runs on the game's thread, every frame.
    void CanPlaceHook(void* Manager, uint64_t Type)
    {
        s_original_can_place(Manager, Type);
        s_manager.store(Manager);

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
                        if (s_manager.load() == nullptr)
                        {
                            Append(StringFormat("%s  === status: o manager ainda nao passou pelo hook ===\n", Clock().c_str()));
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

bool DS2_PartyHook::Install(Injector& injector)
{
#ifdef _WIN32
    const uintptr_t Base = (uintptr_t)injector.GetBaseAddress();
    const uintptr_t CanPlace = Base + kCanPlaceOffset;
    const uintptr_t Place = Base + kPlaceOffset;

    if (!BytesMatch(CanPlace, kPrologue, sizeof(kPrologue)) || !BytesMatch(Place, kPrologue, sizeof(kPrologue)))
    {
        Error("[DS2_PartyHook] o codigo em +0x%zx ou +0x%zx nao e o esperado; recusando", kCanPlaceOffset, kPlaceOffset);
        return false;
    }

    s_log_path = injector.GetDllPath() / "DS2_Party.log";
    s_request_path = injector.GetDllPath() / "DS2_Party.req";

    s_original_can_place = (SignMethod_p)CanPlace;
    s_place = (SignMethod_p)Place;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_can_place, CanPlaceHook);
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
    if (s_original_can_place != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_can_place, CanPlaceHook);
        DetourTransactionCommit();
        s_original_can_place = nullptr;
    }
#endif
}

const char* DS2_PartyHook::GetName()
{
    return "DS2 Party";
}
