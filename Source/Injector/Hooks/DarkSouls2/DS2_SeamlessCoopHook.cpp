/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_SeamlessCoopHook.h"
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
#include <intrin.h>
#include "ThirdParty/detours/src/detours.h"
#endif

namespace
{
#ifdef _WIN32

    // Version 1.03 Calibrations 2.02, both verified byte for byte before a
    // detour is written or a call is made.
    //
    // The warp entry: the virtual function the whole game reaches through slot
    // +0x40 of the global context. Traced live from an ordinary death.
    constexpr size_t kWarpOffset = 0x1c2a80;
    constexpr uint8_t kWarpBytes[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57 };

    // "Send the player to where they last rested." Builds the request from the
    // respawn record and calls the warp itself.
    constexpr size_t kRespawnOffset = 0x44fde0;
    constexpr uint8_t kRespawnBytes[] = { 0x48, 0x83, 0xec, 0x68, 0x48, 0x8d, 0x54, 0x24, 0x20 };

    // The respawn record lives on the same context the warp is called with.
    constexpr size_t kRespawnRecordOffset = 0x70;

    // What the warp entry reads out of the request before it queues it.
    constexpr uint32_t kReasonBonfire = 1;
    constexpr uint32_t kReasonSession = 4;

    // Motive 4 alone is not "go home": the same motive carries a guest into
    // the host's world. The game separates the two on the third argument, and
    // so does this hook - see the note on WarpHook.
    constexpr uint8_t kForced = 0;

    // 0x38 bytes, laid out by the game's own builder at 0x14044ed40.
    struct WarpRequest
    {
        uint32_t Kind;        // +0x00  which family of destination
        uint32_t Reason;      // +0x04  why the warp is happening
        uint32_t Map;         // +0x08  map id, 0x0a1f0000 for Majula
        uint32_t Unknown0c;   // +0x0c  left at -1 by the respawn builder
        uint32_t Unknown10;   // +0x10
        uint8_t Flavour;      // +0x14  3 on every path measured so far
        uint8_t Pad15[3];
        uint32_t Spawn;       // +0x18  spawn point inside the map
        uint32_t Rest[7];     // +0x1c  only written by the copy path
    };
    static_assert(sizeof(WarpRequest) == 0x38, "the warp request is 0x38 bytes");

    // It returns a byte, and the caller reads it: the session state machine
    // goes to state 0x13 when the warp is refused (0x1402c2ee6). A detour
    // declared void would hand that caller whatever happened to be in al.
    using Warp_p = uint8_t(*)(void* Context, WarpRequest* Request, uint8_t Flag);
    Warp_p s_original_warp = nullptr;

    using Respawn_p = void(*)(void* Record);
    Respawn_p s_respawn = nullptr;

    uintptr_t s_base = 0;

    // On, and what it is worth depends on who died - measured both ways:
    //
    //   red invader   the game already asks for the bonfire. No change.
    //   co-op phantom the game asks to be put back where he stood when he was
    //                 summoned (kind 0, a position). This replaces it.
    //
    // Writing "0" to DS2_Seamless.req leaves the log on and stops the
    // substitution, which is how the two halves of that measurement were
    // taken.
    std::atomic<bool> s_redirect{ true };
    std::atomic<bool> s_running{ false };
    std::atomic<bool> s_inside{ false };
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

    void Describe(const char* What, const WarpRequest* Request, uint8_t Flag, uintptr_t From)
    {
        if (Request == nullptr)
        {
            Append(StringFormat("  %s pedido vazio de +0x%zx\n", What, (size_t)From));
            return;
        }

        // The whole request goes in as well. Half of these 0x38 bytes are
        // still unnamed, and a line that only prints the named half cannot
        // answer a question nobody has asked yet.
        std::string Raw;
        const uint8_t* Bytes = (const uint8_t*)Request;
        for (size_t Index = 0; Index < sizeof(WarpRequest); ++Index)
        {
            Raw += StringFormat("%02x", Bytes[Index]);
        }

        Append(StringFormat(
            "  %s motivo=%u forca=%u tipo=%u mapa=%08x ponto=%08x [+0x0c]=%08x [+0x10]=%08x sabor=%u de=+0x%zx cru=%s\n",
            What, Request->Reason, (unsigned)Flag, Request->Kind, Request->Map, Request->Spawn,
            Request->Unknown0c, Request->Unknown10, (unsigned)Request->Flavour, (size_t)From,
            Raw.c_str()));
    }

    uint8_t WarpHook(void* Context, WarpRequest* Request, uint8_t Flag)
    {
        const uintptr_t From = s_base == 0 ? 0 : (uintptr_t)_ReturnAddress() - s_base;

        // The replacement warp goes through this same entry, so without a
        // guard the hook would describe its own work as if the game had asked
        // for it, and a mistake in the test below would recurse.
        const bool Reentrant = s_inside.load();

        Describe(Reentrant ? "(reentrada)" : "warp", Request, Flag, From);

        // The third argument is the whole test. The entry point reads it
        // itself: motive 4 skips the permission gate only when the argument is
        // zero, and that is the forced return home the session teardown asks
        // for. Motive 4 with the argument set to one is the ordinary request
        // that carries a guest *into* a host's world - redirecting that one
        // cancels the summon, which is how this was found.
        if (!Reentrant && s_redirect.load() && Request != nullptr &&
            Request->Reason == kReasonSession && Flag == kForced &&
            Context != nullptr && s_respawn != nullptr)
        {
            void* Record = *(void**)((uint8_t*)Context + kRespawnRecordOffset);
            if (Record != nullptr)
            {
                Append("  co-op: em vez de voltar para o proprio mundo, ultima fogueira\n");
                s_inside.store(true);
                s_respawn(Record);
                s_inside.store(false);
                return 1;
            }

            Append("  co-op: sem registro de renascimento; deixando o warp original passar\n");
        }

        return s_original_warp(Context, Request, Flag);
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
                s_redirect.store(Wanted);
                Append(Wanted
                    ? "=== redirecionamento ligado ===\n"
                    : "=== redirecionamento desligado, so registro ===\n");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

#endif
}

bool DS2_SeamlessCoopHook::Install(Injector& injector)
{
#ifdef _WIN32
    s_base = (uintptr_t)injector.GetBaseAddress();
    const uintptr_t Warp = s_base + kWarpOffset;
    const uintptr_t Respawn = s_base + kRespawnOffset;

    if (!BytesMatch(Warp, kWarpBytes, sizeof(kWarpBytes)))
    {
        Error("[DS2_SeamlessCoopHook] o codigo em +0x%zx nao e o esperado; recusando", kWarpOffset);
        return false;
    }
    if (!BytesMatch(Respawn, kRespawnBytes, sizeof(kRespawnBytes)))
    {
        Error("[DS2_SeamlessCoopHook] o codigo em +0x%zx nao e o esperado; recusando", kRespawnOffset);
        return false;
    }

    s_log_path = injector.GetDllPath() / "DS2_Seamless.log";
    s_request_path = injector.GetDllPath() / "DS2_Seamless.req";

    s_original_warp = (Warp_p)Warp;
    s_respawn = (Respawn_p)Respawn;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_warp, WarpHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        Error("[DS2_SeamlessCoopHook] nao consegui instalar o detour");
        return false;
    }

    s_running.store(true);
    s_thread = std::thread(Run);

    Append("=== ds2os co-op seamless ===\n");
    Log("[DS2_SeamlessCoopHook] pronto; escreva 0 em DS2_Seamless.req para so registrar");
#endif
    return true;
}

void DS2_SeamlessCoopHook::Uninstall()
{
#ifdef _WIN32
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }

    if (s_original_warp != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_warp, WarpHook);
        DetourTransactionCommit();
        s_original_warp = nullptr;
    }
#endif
}

const char* DS2_SeamlessCoopHook::GetName()
{
    return "DS2 Seamless Coop";
}
