/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_RematchHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"

#include <atomic>
#include <chrono>
#include <cstdint>
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

    // Version 1.03 Calibrations 2.02. Both were found by tracing a real
    // summon, and both are verified byte for byte before anything is written.
    constexpr size_t kSummonOffset = 0x2a14c0;
    constexpr uint8_t kSummonBytes[] = { 0x48, 0x83, 0xec, 0x28, 0x8b, 0x02 };

    constexpr size_t kAddSignOffset = 0x213160;
    constexpr uint8_t kAddSignBytes[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c };


    using Summon_p = void(*)(void* Manager, uint32_t* Handle);
    Summon_p s_original_summon = nullptr;

    // Eleven parameters, and every one of them is forwarded. A detour that
    // takes fewer would read the caller's stack arguments from the wrong
    // place, which is the quiet way to corrupt a game.
    using AddSign_p = uint32_t*(*)(void* Self, uint32_t* OutHandle, uint8_t Type, void* P4,
        uint32_t P5, uint32_t P6, void* P7, void* P8, uint8_t P9, uint32_t P10, void* P11);
    AddSign_p s_original_add_sign = nullptr;


    std::atomic<void*> s_manager{ nullptr };
    std::atomic<bool> s_pending{ false };
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

    void SummonHook(void* Manager, uint32_t* Handle)
    {
        // The only place the manager pointer is handed out. Remembering it is
        // the whole reason this detour exists; the summon itself is the
        // player's own and is passed straight through.
        s_manager.store(Manager);
        if (Handle != nullptr)
        {
            Append(StringFormat("  o jogador invocou a placa %08x; revanche ligada\n", *Handle));
        }

        // Arming here is safe because of an interlock the game already has: a
        // phantom cannot place a sign while it is in somebody else's world, so
        // a sign arriving is proof the previous duel is over. Which makes "the
        // player summoned once" the honest trigger, and saves guessing at a
        // session-end path that the host does not even run — the guest's
        // teardown was tried first and never fired here.
        s_pending.store(true);
        s_original_summon(Manager, Handle);
    }


    uint32_t* AddSignHook(void* Self, uint32_t* OutHandle, uint8_t Type, void* P4,
        uint32_t P5, uint32_t P6, void* P7, void* P8, uint8_t P9, uint32_t P10, void* P11)
    {
        uint32_t* Result = s_original_add_sign(Self, OutHandle, Type, P4, P5, P6, P7, P8, P9, P10, P11);

        if (!s_pending.load() || OutHandle == nullptr || *OutHandle == 0)
        {
            return Result;
        }

        void* Manager = s_manager.load();
        if (Manager == nullptr)
        {
            Append("  revanche pedida, mas ninguem invocou ainda: sem o manager nao da\n");
            return Result;
        }

        // Stays armed: the next sign from the same pair is the next rematch,
        // and that is the point. Writing "0" to the request file turns it off.
        Append(StringFormat("  revanche: invocando a placa %08x que acabou de chegar\n", *OutHandle));
        s_original_summon(Manager, OutHandle);
        return Result;
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
                std::string Contents;
                {
                    std::ifstream Stream(s_request_path);
                    std::getline(Stream, Contents);
                }
                std::filesystem::remove(s_request_path, Error);

                const bool Wanted = Contents.find('0') != 0;
                s_pending.store(Wanted);
                Append(Wanted
                    ? "=== revanche armada a mao ===\n"
                    : "=== revanche desligada a mao ===\n");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

#endif
}

bool DS2_RematchHook::Install(Injector& injector)
{
#ifdef _WIN32
    const uintptr_t Base = (uintptr_t)injector.GetBaseAddress();
    const uintptr_t Summon = Base + kSummonOffset;
    const uintptr_t AddSign = Base + kAddSignOffset;

    if (!BytesMatch(Summon, kSummonBytes, sizeof(kSummonBytes)))
    {
        Error("[DS2_RematchHook] o codigo em +0x%zx nao e o esperado; recusando", kSummonOffset);
        return false;
    }
    if (!BytesMatch(AddSign, kAddSignBytes, sizeof(kAddSignBytes)))
    {
        Error("[DS2_RematchHook] o codigo em +0x%zx nao e o esperado; recusando", kAddSignOffset);
        return false;
    }


    s_log_path = injector.GetDllPath() / "DS2_Rematch.log";
    s_request_path = injector.GetDllPath() / "DS2_Rematch.req";

    s_original_summon = (Summon_p)Summon;
    s_original_add_sign = (AddSign_p)AddSign;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_summon, SummonHook);
    DetourAttach(&(PVOID&)s_original_add_sign, AddSignHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        Error("[DS2_RematchHook] nao consegui instalar os detours");
        return false;
    }

    s_running.store(true);
    s_thread = std::thread(Run);

    Append("=== ds2os revanche ===\n");
    Log("[DS2_RematchHook] pronto; escreva em DS2_Rematch.req para armar uma revanche");
#endif
    return true;
}

void DS2_RematchHook::Uninstall()
{
#ifdef _WIN32
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }

    if (s_original_summon != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_summon, SummonHook);
        DetourDetach(&(PVOID&)s_original_add_sign, AddSignHook);
        DetourTransactionCommit();
        s_original_summon = nullptr;
        s_original_add_sign = nullptr;
    }
#endif
}

const char* DS2_RematchHook::GetName()
{
    return "DS2 Rematch";
}
