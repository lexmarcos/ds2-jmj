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

    // The session's own teardown: the state machine reaches state 8, this runs
    // once, sends the leave and moves to state 9. On the host it is the moment
    // the guest is gone — which is the moment a rematch becomes wanted.
    constexpr size_t kSessionEndOffset = 0x2c3900;
    constexpr uint8_t kSessionEndBytes[] = { 0x40, 0x55, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xb1 };

    using Summon_p = void(*)(void* Manager, uint32_t* Handle);
    Summon_p s_original_summon = nullptr;

    // Eleven parameters, and every one of them is forwarded. A detour that
    // takes fewer would read the caller's stack arguments from the wrong
    // place, which is the quiet way to corrupt a game.
    using AddSign_p = uint32_t*(*)(void* Self, uint32_t* OutHandle, uint8_t Type, void* P4,
        uint32_t P5, uint32_t P6, void* P7, void* P8, uint8_t P9, uint32_t P10, void* P11);
    AddSign_p s_original_add_sign = nullptr;

    using SessionEnd_p = void(*)(void* Session);
    SessionEnd_p s_original_session_end = nullptr;

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
            Append(StringFormat("  o jogador invocou a placa %08x\n", *Handle));
        }
        s_original_summon(Manager, Handle);
    }

    void SessionEndHook(void* Session)
    {
        s_original_session_end(Session);

        // Any end arms a rematch, not only a death. The reason lives in the
        // session object and could be read here, but a duel that ends because
        // the guest walked out is just as much a rematch as one that ends in a
        // kill — and reading the wrong field to be clever would be worse than
        // taking both.
        s_pending.store(true);
        Append("  a sessao acabou; revanche armada\n");
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

        // Spend the rematch whether or not the summon takes, so a sign that
        // cannot be summoned does not leave the hook firing on every sign that
        // arrives afterwards.
        s_pending.store(false);
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
                std::filesystem::remove(s_request_path, Error);
                s_pending.store(true);
                Append("=== revanche armada; sai na proxima placa que chegar ===\n");
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

    const uintptr_t SessionEnd = Base + kSessionEndOffset;
    if (!BytesMatch(SessionEnd, kSessionEndBytes, sizeof(kSessionEndBytes)))
    {
        Error("[DS2_RematchHook] o codigo em +0x%zx nao e o esperado; recusando", kSessionEndOffset);
        return false;
    }

    s_log_path = injector.GetDllPath() / "DS2_Rematch.log";
    s_request_path = injector.GetDllPath() / "DS2_Rematch.req";

    s_original_summon = (Summon_p)Summon;
    s_original_add_sign = (AddSign_p)AddSign;
    s_original_session_end = (SessionEnd_p)SessionEnd;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_summon, SummonHook);
    DetourAttach(&(PVOID&)s_original_add_sign, AddSignHook);
    DetourAttach(&(PVOID&)s_original_session_end, SessionEndHook);
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
        DetourDetach(&(PVOID&)s_original_session_end, SessionEndHook);
        DetourTransactionCommit();
        s_original_summon = nullptr;
        s_original_add_sign = nullptr;
        s_original_session_end = nullptr;
    }
#endif
}

const char* DS2_RematchHook::GetName()
{
    return "DS2 Rematch";
}
