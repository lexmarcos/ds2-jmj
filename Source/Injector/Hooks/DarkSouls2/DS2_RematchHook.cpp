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
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
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

    // Who the rematch is with. A rematch re-summons the opponent of the last
    // duel, not whichever sign shows up next: with three or more players a
    // stranger's sign — red, white, anything — would otherwise be summoned
    // into the host's world unasked.
    //
    // AddSign's sixth argument is the sign owner's player id and the third the
    // sign type (measured 14/09 on a red sign: entry +0x20 sign id 1006, +0x24
    // player 3, the type byte 7; a white sign arrives as type 1). The summon
    // only sees a handle, so every arriving sign is remembered by handle.
    struct SeenSign
    {
        uint32_t Handle = 0;
        uint32_t PlayerId = 0;
        uint8_t Type = 0;
    };
    constexpr size_t kSeenSigns = 64;
    SeenSign s_seen[kSeenSigns];
    size_t s_seen_next = 0;
    std::mutex s_seen_mutex;

    constexpr uint32_t kNoOpponent = 0xffffffff;
    std::atomic<uint32_t> s_opponent_player{ kNoOpponent };
    std::atomic<uint32_t> s_opponent_type{ 0 };

    void RememberSign(uint32_t Handle, uint32_t PlayerId, uint8_t Type)
    {
        std::scoped_lock Lock(s_seen_mutex);
        for (SeenSign& Seen : s_seen)
        {
            if (Seen.Handle == Handle)
            {
                Seen.PlayerId = PlayerId;
                Seen.Type = Type;
                return;
            }
        }
        s_seen[s_seen_next] = { Handle, PlayerId, Type };
        s_seen_next = (s_seen_next + 1) % kSeenSigns;
    }

    bool LookUpSign(uint32_t Handle, SeenSign& Out)
    {
        std::scoped_lock Lock(s_seen_mutex);
        for (const SeenSign& Seen : s_seen)
        {
            if (Seen.Handle != 0 && Seen.Handle == Handle)
            {
                Out = Seen;
                return true;
            }
        }
        return false;
    }
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
        SeenSign Summoned;
        if (Handle != nullptr && LookUpSign(*Handle, Summoned))
        {
            s_opponent_player.store(Summoned.PlayerId);
            s_opponent_type.store(Summoned.Type);
            Append(StringFormat("  o jogador invocou a placa %08x (jogador %u, tipo %u); revanche ligada com ele\n",
                *Handle, Summoned.PlayerId, (unsigned)Summoned.Type));
        }
        else
        {
            // Without an owner there is nobody to rematch with, and summoning
            // "the next sign" is exactly what must not happen.
            s_opponent_player.store(kNoOpponent);
            Append(StringFormat("  o jogador invocou a placa %08x, que nao passou pelo AddSign; revanche sem oponente\n",
                Handle == nullptr ? 0u : *Handle));
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

        // Every sign that reaches the cache, whether or not a rematch is
        // armed. This hook was written for red signs and never fired for a
        // white one, and a hook that only speaks when it acts cannot say
        // whether it was never called or called and declined.
        Append(StringFormat("  placa recebida: tipo=%u jogador=%u alca=%08x armado=%u\n",
            (unsigned)Type, P6, OutHandle == nullptr ? 0u : *OutHandle,
            (unsigned)s_pending.load()));

        if (OutHandle == nullptr || *OutHandle == 0)
        {
            return Result;
        }
        RememberSign(*OutHandle, P6, Type);

        if (!s_pending.load())
        {
            return Result;
        }

        const uint32_t Opponent = s_opponent_player.load();
        if (Opponent == kNoOpponent)
        {
            Append(StringFormat("  placa %08x (jogador %u, tipo %u) chegou, mas a revanche nao tem oponente; ignorada\n",
                *OutHandle, P6, (unsigned)Type));
            return Result;
        }
        if (P6 != Opponent || (uint32_t)Type != s_opponent_type.load())
        {
            Append(StringFormat("  placa %08x e do jogador %u tipo %u, a revanche e com o jogador %u tipo %u; ignorada\n",
                *OutHandle, P6, (unsigned)Type, Opponent, s_opponent_type.load()));
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
        Append(StringFormat("  revanche: invocando a placa %08x do jogador %u, que acabou de chegar\n", *OutHandle, P6));
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

                // "alvo <player id> <type>" names the opponent by hand, which is
                // also how a sign from somebody else is tested with two accounts.
                unsigned Player = 0;
                unsigned SignType = 0;
                if (sscanf(Contents.c_str(), "alvo %u %u", &Player, &SignType) == 2)
                {
                    s_opponent_player.store(Player);
                    s_opponent_type.store(SignType);
                    s_pending.store(true);
                    Append(StringFormat("=== revanche armada a mao com o jogador %u tipo %u ===\n", Player, SignType));
                }
                else
                {
                    const bool Wanted = Contents.find('0') != 0;
                    s_pending.store(Wanted);
                    Append(Wanted
                        ? (s_opponent_player.load() == kNoOpponent
                            ? "=== revanche armada a mao, sem oponente: nenhuma placa sera invocada ===\n"
                            : "=== revanche armada a mao ===\n")
                        : "=== revanche desligada a mao ===\n");
                }
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
