/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_ForceMultiPlayZoneHook.h"
#include "Injector/Config/RuntimeConfig.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"
#include "Shared/Platform/Platform.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
#if defined(_WIN32) && defined(_M_X64)

    // In FUN_140250dc0, ebx has just been loaded with the zone the player is
    // standing in and nothing has read it yet:
    //
    //   +0x250e58  mov ebx,[rcx+0x20]    the zone
    //   +0x250e5b  cmp ebx,[rsi+0x10]    <- we break here
    //   +0x250e6d  test ebx,ebx          zero or less means no zone
    //
    // Everything downstream takes the zone from ebx, so replacing it here is
    // enough; there is no second copy to keep in step.
    constexpr size_t kPatchOffset = 0x250e5b;
    constexpr uint8_t kExpectedBytes[] = { 0x3b, 0x5e, 0x10 };  // cmp ebx,[rsi+0x10]

    constexpr uint8_t kBreakpointOpcode = 0xCC;
    constexpr DWORD64 kTrapFlag = 0x100;
    constexpr double kLogIntervalSeconds = 30.0;

    std::mutex s_log_mutex;
    std::mutex s_state_mutex;
    std::unordered_map<DWORD, bool> s_pending_steps;
    bool s_breakpoint_present = false;

    std::atomic_bool s_installed{false};
    std::atomic_uint64_t s_hits{0};
    std::atomic_uint64_t s_substitutions{0};
    std::atomic<double> s_last_log{-1.0};
    PVOID s_handler = nullptr;
    uintptr_t s_address = 0;
    uint8_t s_original = 0;
    int s_forced_zone = 0;

    void Append(const std::string& Text)
    {
        std::scoped_lock lock(s_log_mutex);
        std::filesystem::path Path = Injector::Instance().GetDllPath() / "DS2_MultiPlayZone.log";
        std::ofstream Stream(Path, std::ios::app);
        if (Stream.is_open())
        {
            Stream << Text;
        }
    }

    bool WriteByte(uintptr_t Address, uint8_t Value)
    {
        DWORD Previous = 0;
        if (!VirtualProtect((void*)Address, 1, PAGE_EXECUTE_READWRITE, &Previous))
        {
            return false;
        }
        *(uint8_t*)Address = Value;
        FlushInstructionCache(GetCurrentProcess(), (void*)Address, 1);
        DWORD Ignored = 0;
        VirtualProtect((void*)Address, 1, Previous, &Ignored);
        return true;
    }

    LONG CALLBACK ForceHandler(EXCEPTION_POINTERS* Exception)
    {
        CONTEXT* Context = Exception->ContextRecord;
        const DWORD Code = Exception->ExceptionRecord->ExceptionCode;
        const DWORD ThreadId = GetCurrentThreadId();

        // Wine does not always leave Rip past the int3 the way Windows does, so
        // the address in the exception record counts too. Missing our own
        // breakpoint means the game dies on an int3 nobody claimed.
        const uintptr_t Rip = (uintptr_t)Context->Rip;
        const uintptr_t Candidate = Rip > 0 ? Rip - 1 : 0;
        const uintptr_t Reported = (uintptr_t)Exception->ExceptionRecord->ExceptionAddress;

        if (Code == EXCEPTION_BREAKPOINT && (Candidate == s_address || Reported == s_address))
        {
            {
                std::scoped_lock lock(s_state_mutex);
                if (!s_breakpoint_present)
                {
                    return EXCEPTION_CONTINUE_SEARCH;
                }
                WriteByte(s_address, s_original);
                s_breakpoint_present = false;
                s_pending_steps[ThreadId] = true;
            }

            s_hits++;

            const int Zone = (int)(int32_t)Context->Rbx;
            if (Zone <= 0)
            {
                Context->Rbx = (DWORD64)(uint32_t)s_forced_zone;
                s_substitutions++;

                const double Now = GetSeconds();
                if (s_last_log.load() < 0.0 || Now - s_last_log.load() >= kLogIntervalSeconds)
                {
                    s_last_log.store(Now);
                    Append(StringFormat(
                        "time=%.3f event=DS2ForceZone result=substituted was=%d now=%d hits=%llu subs=%llu\n",
                        Now,
                        Zone,
                        s_forced_zone,
                        (unsigned long long)s_hits.load(),
                        (unsigned long long)s_substitutions.load()));
                    Log("[DS2ForceZone] zona %d -> %d", Zone, s_forced_zone);
                }
            }

            Context->Rip = s_address;
            Context->EFlags |= (DWORD)kTrapFlag;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (Code == EXCEPTION_SINGLE_STEP)
        {
            std::scoped_lock lock(s_state_mutex);
            auto Pending = s_pending_steps.find(ThreadId);
            if (Pending == s_pending_steps.end())
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            s_pending_steps.erase(Pending);
            Context->EFlags &= ~(DWORD)kTrapFlag;
            if (WriteByte(s_address, kBreakpointOpcode))
            {
                s_breakpoint_present = true;
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

#endif
}

bool DS2_ForceMultiPlayZoneHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    const RuntimeConfig& Config = injector.GetConfig();
    if (!Config.DS2ForceMultiPlayZone)
    {
        return true;
    }

    s_forced_zone = Config.DS2ForcedZoneId;
    if (s_forced_zone <= 0)
    {
        Error("[DS2ForceZone] DS2ForcedZoneId precisa ser positivo; nao instalei");
        return true;
    }

    s_address = (uintptr_t)injector.GetBaseAddress() + kPatchOffset;

    uint8_t Found[sizeof(kExpectedBytes)] = {};
    memcpy(Found, (const void*)s_address, sizeof(Found));
    if (memcmp(Found, kExpectedBytes, sizeof(Found)) != 0)
    {
        Append(StringFormat(
            "time=%.3f event=DS2ForceZone result=signature_mismatch offset=0x%zx found=%02x%02x%02x\n",
            GetSeconds(),
            kPatchOffset,
            Found[0],
            Found[1],
            Found[2]));
        Error("[DS2ForceZone] assinatura nao confere em +0x%zx; nao instalei", kPatchOffset);
        return true;
    }

    s_original = Found[0];
    s_handler = AddVectoredExceptionHandler(1, ForceHandler);
    if (s_handler == nullptr)
    {
        Error("[DS2ForceZone] nao consegui registrar o handler");
        return true;
    }

    if (!WriteByte(s_address, kBreakpointOpcode))
    {
        RemoveVectoredExceptionHandler(s_handler);
        s_handler = nullptr;
        Error("[DS2ForceZone] nao consegui escrever o breakpoint");
        return true;
    }

    {
        std::scoped_lock lock(s_state_mutex);
        s_breakpoint_present = true;
    }
    s_installed.store(true);

    Append(StringFormat(
        "time=%.3f event=DS2ForceZone result=installed address=0x%016llx offset=0x%zx zone=%d\n",
        GetSeconds(),
        (unsigned long long)s_address,
        kPatchOffset,
        s_forced_zone));
    Log("[DS2ForceZone] forcando zona %d onde nao houver nenhuma", s_forced_zone);
    return true;
#else
    Warning("[DS2ForceZone] requer Windows x64; nao instalado.");
    return true;
#endif
}

void DS2_ForceMultiPlayZoneHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    if (s_installed.exchange(false))
    {
        std::scoped_lock lock(s_state_mutex);
        if (s_breakpoint_present)
        {
            WriteByte(s_address, s_original);
            s_breakpoint_present = false;
        }
    }
    if (s_handler != nullptr)
    {
        RemoveVectoredExceptionHandler(s_handler);
        s_handler = nullptr;
    }
#endif
}

const char* DS2_ForceMultiPlayZoneHook::GetName()
{
    return "DS2 Force MultiPlay Zone";
}
