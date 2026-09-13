/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_SeamlessSessionHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
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

    // Version 1.03 Calibrations 2.02, verified before anything is written.
    // Slot +0x30 of the session: "end this session, and here is why".
    constexpr size_t kEndSessionOffset = 0x2c2f20;
    constexpr uint8_t kEndSessionBytes[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20 };

    // Fields of the session object that the teardown itself reads.
    constexpr size_t kRoleOffset = 0xd8;    // byte, the index into the role table
    constexpr size_t kStateOffset = 0xf8;   // int, what FUN_1402c3630 switches on
    constexpr size_t kReasonOffset = 0x1cc; // int, written by this very function

    // Reasons are small; a bitmask is enough and costs no allocation on the
    // game's thread.
    constexpr uint32_t kMaxReason = 32;

    using EndSession_p = void(*)(void* Session, uint32_t Reason);
    EndSession_p s_original_end = nullptr;

    uintptr_t s_base = 0;

    std::atomic<uint32_t> s_blocked{ 0 };
    std::atomic<int> s_role{ -1 };
    std::atomic<bool> s_running{ false };
    std::thread s_thread;

    std::filesystem::path s_log_path;
    std::filesystem::path s_request_path;

    // The other half of the multiplayer machine, and the one this hook was
    // blind to for a week.
    //
    // The binary carries RTTI, and the classes name themselves:
    //
    //   NetJoinMultiplayCtrl   -> NetSummonJoinMultiplayCtrl    the guest
    //   NetAcceptMultiplayCtrl -> NetSummonAcceptMultiplayCtrl  the host
    //
    // Everything above watches the guest's: `FUN_1402c2f20` is its "end the
    // session" and `FUN_1402c3630` its per-frame dispatcher. The host runs a
    // different object with a different layout - state at +0x150 rather than
    // +0xf8 - and a different dispatcher, so it was never going to show up in
    // this log. "The host asks for nothing when the guest dies" was a fact
    // about the instrumentation, not about the game.
    //
    // This detour fixes that: it is the host's per-frame switch, so it says
    // what the host's state is, when it changes, and - by falling silent -
    // when the controller stops being ticked at all.
    constexpr size_t kHostTickOffset = 0x2bddb0;
    constexpr uint8_t kHostTickBytes[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x0f, 0x29, 0x74, 0x24, 0x20 };
    constexpr size_t kHostState = 0x150;

    using HostTick_p = void(*)(void* Ctrl, float Delta);
    HostTick_p s_original_host_tick = nullptr;

    void* s_host_ctrl = nullptr;
    uint32_t s_host_state = 0xffffffff;
    uint64_t s_host_ticks = 0;

    void Append(const std::string& Text)
    {
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream)
        {
            Stream << Text;
        }
    }

    void EndSessionHook(void* Session, uint32_t Reason)
    {
        const uintptr_t From = s_base == 0 ? 0 : (uintptr_t)_ReturnAddress() - s_base;

        unsigned Role = 0xff;
        int State = -1;
        int Previous = -1;
        if (Session != nullptr)
        {
            const uint8_t* Bytes = (const uint8_t*)Session;
            Role = *(const uint8_t*)(Bytes + kRoleOffset);
            State = *(const int*)(Bytes + kStateOffset);
            Previous = *(const int*)(Bytes + kReasonOffset);
        }

        const bool Refuse =
            Reason < kMaxReason &&
            (s_blocked.load() & (1u << Reason)) != 0 &&
            (s_role.load() < 0 || s_role.load() == (int)Role);

        Append(StringFormat(
            "  fim de sessao %s sessao=%p papel=%u estado=%d motivo=%u motivo_anterior=%d de=+0x%zx\n",
            Refuse ? "RECUSADO" : "pedido", Session, Role, State, Reason, Previous, (size_t)From));

        if (Refuse)
        {
            return;
        }

        s_original_end(Session, Reason);
    }

    void HostTickHook(void* Ctrl, float Delta)
    {
        // One line per change, never one per frame: this runs sixty times a
        // second and the interesting thing is the shape of the timeline, not
        // its length.
        if (Ctrl != s_host_ctrl)
        {
            s_host_ctrl = Ctrl;
            s_host_state = 0xffffffff;
            Append(StringFormat("  host: controlador %p apos %llu quadros\n",
                Ctrl, (unsigned long long)s_host_ticks));
        }

        const uint32_t State = Ctrl == nullptr
            ? 0xffffffff
            : *(const uint32_t*)((const uint8_t*)Ctrl + kHostState);

        if (State != s_host_state)
        {
            s_host_state = State;
            Append(StringFormat("  host: estado -> %u (0x%x) no quadro %llu\n",
                State, State, (unsigned long long)s_host_ticks));
        }

        ++s_host_ticks;
        s_original_host_tick(Ctrl, Delta);
    }

    bool BytesMatch(uintptr_t Address, const uint8_t* Expected, size_t Length)
    {
        return memcmp((const void*)Address, Expected, Length) == 0;
    }

    void Apply(const std::string& Line)
    {
        std::istringstream Parts(Line);
        std::string Verb;
        Parts >> Verb;

        if (Verb == "clear")
        {
            s_blocked.store(0);
            Append("=== nada mais e recusado ===\n");
            return;
        }
        if (Verb == "role")
        {
            std::string Which;
            Parts >> Which;
            if (Which == "any" || Which.empty())
            {
                s_role.store(-1);
                Append("=== recusa vale para qualquer papel ===\n");
            }
            else
            {
                s_role.store(atoi(Which.c_str()));
                Append(StringFormat("=== recusa so para o papel %d ===\n", s_role.load()));
            }
            return;
        }
        if (Verb == "status")
        {
            Append(StringFormat("=== recusando a mascara %08x, papel %d ===\n",
                s_blocked.load(), s_role.load()));
            return;
        }

        if (Verb == "block" || Verb == "allow")
        {
            std::string Which;
            Parts >> Which;
            const long Reason = strtol(Which.c_str(), nullptr, 0);
            if (Reason < 0 || Reason >= (long)kMaxReason)
            {
                Append(StringFormat("=== motivo %s fora da faixa ===\n", Which.c_str()));
                return;
            }

            const uint32_t Bit = 1u << (uint32_t)Reason;
            if (Verb == "block")
            {
                s_blocked.fetch_or(Bit);
                Append(StringFormat("=== o motivo %ld passa a ser recusado ===\n", Reason));
            }
            else
            {
                s_blocked.fetch_and(~Bit);
                Append(StringFormat("=== o motivo %ld volta a passar ===\n", Reason));
            }
        }
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
                    if (!Line.empty())
                    {
                        Apply(Line);
                    }
                }
                Stream.close();
                std::filesystem::remove(s_request_path, Error);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

#endif
}

bool DS2_SeamlessSessionHook::Install(Injector& injector)
{
#ifdef _WIN32
    s_base = (uintptr_t)injector.GetBaseAddress();
    const uintptr_t EndSession = s_base + kEndSessionOffset;

    if (!BytesMatch(EndSession, kEndSessionBytes, sizeof(kEndSessionBytes)))
    {
        Error("[DS2_SeamlessSessionHook] o codigo em +0x%zx nao e o esperado; recusando", kEndSessionOffset);
        return false;
    }

    s_log_path = injector.GetDllPath() / "DS2_Session.log";
    s_request_path = injector.GetDllPath() / "DS2_Session.req";

    const uintptr_t HostTick = s_base + kHostTickOffset;
    if (!BytesMatch(HostTick, kHostTickBytes, sizeof(kHostTickBytes)))
    {
        Error("[DS2_SeamlessSessionHook] o codigo em +0x%zx nao e o esperado; recusando", kHostTickOffset);
        return false;
    }

    s_original_end = (EndSession_p)EndSession;
    s_original_host_tick = (HostTick_p)HostTick;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_end, EndSessionHook);
    DetourAttach(&(PVOID&)s_original_host_tick, HostTickHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        Error("[DS2_SeamlessSessionHook] nao consegui instalar o detour");
        return false;
    }

    s_running.store(true);
    s_thread = std::thread(Run);

    Append("=== ds2os sessao ===\n");
    Log("[DS2_SeamlessSessionHook] pronto; escreva em DS2_Session.req para recusar um fim de sessao");
#endif
    return true;
}

void DS2_SeamlessSessionHook::Uninstall()
{
#ifdef _WIN32
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }

    if (s_original_end != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_end, EndSessionHook);
        DetourDetach(&(PVOID&)s_original_host_tick, HostTickHook);
        DetourTransactionCommit();
        s_original_end = nullptr;
    }
#endif
}

const char* DS2_SeamlessSessionHook::GetName()
{
    return "DS2 Seamless Session";
}
