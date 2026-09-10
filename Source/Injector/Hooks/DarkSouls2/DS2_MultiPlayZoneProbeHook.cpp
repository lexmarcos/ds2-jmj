/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_MultiPlayZoneProbeHook.h"
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

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
#if defined(_WIN32) && defined(_M_X64)

    // FUN_140250dc0 keeps the player's multiplay permissions. At this offset it
    // has just looked the zone up, and:
    //
    //   ebx        the zone id, non-positive when outside every zone
    //   edi        the permission byte the zone carries
    //   [rsi+0x20] the permissions currently in force
    //
    // which is everything needed to see how Heide and Majula differ.
    constexpr size_t kProbeOffset = 0x250e93;
    constexpr uint8_t kExpectedBytes[] = { 0x8b, 0x56, 0x20 };  // mov edx,[rsi+0x20]

    constexpr uint8_t kBreakpointOpcode = 0xCC;
    constexpr DWORD64 kTrapFlag = 0x100;

    std::mutex s_log_mutex;
    std::atomic_bool s_installed{false};
    std::atomic_uint64_t s_hits{0};
    PVOID s_handler = nullptr;
    uintptr_t s_address = 0;
    uint8_t s_original = 0;
    std::mutex s_step_mutex;
    std::atomic_bool s_step_pending{false};

    // Only a change is worth a line; this runs every frame.
    std::atomic_int s_last_zone{-999};
    std::atomic_uint s_last_permissions{0xffffffff};

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

    LONG CALLBACK ProbeHandler(EXCEPTION_POINTERS* Exception)
    {
        CONTEXT* Context = Exception->ContextRecord;
        const DWORD Code = Exception->ExceptionRecord->ExceptionCode;

        if (Code == EXCEPTION_BREAKPOINT && (uintptr_t)Context->Rip == s_address + 1)
        {
            s_hits++;

            const int Zone = (int)Context->Rbx;
            const uint32_t Permissions = (uint32_t)Context->Rdi;
            uint32_t InForce = 0;
            memcpy(&InForce, (const void*)(Context->Rsi + 0x20), sizeof(InForce));

            if (Zone != s_last_zone.exchange(Zone) ||
                Permissions != s_last_permissions.exchange(Permissions))
            {
                Append(StringFormat(
                    "time=%.3f event=DS2MultiPlayZone zone=%d permissions=0x%02x in_force=0x%02x hits=%llu\n",
                    GetSeconds(),
                    Zone,
                    Permissions,
                    InForce,
                    (unsigned long long)s_hits.load()));
                Log("[DS2Zone] zona=%d permissoes=0x%02x (em vigor 0x%02x)", Zone, Permissions, InForce);
            }

            // Step past the restored instruction, then put the breakpoint back.
            Context->Rip = s_address;
            WriteByte(s_address, s_original);
            Context->EFlags |= (DWORD)kTrapFlag;
            s_step_pending.store(true);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (Code == EXCEPTION_SINGLE_STEP && s_step_pending.load())
        {
            s_step_pending.store(false);
            Context->EFlags &= ~(DWORD)kTrapFlag;
            WriteByte(s_address, kBreakpointOpcode);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

#endif
}

bool DS2_MultiPlayZoneProbeHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    if (!injector.GetConfig().DS2ProbeMultiPlayZone)
    {
        return true;
    }

    s_address = (uintptr_t)injector.GetBaseAddress() + kProbeOffset;

    // Refuse to patch anything that is not the instruction this was written
    // against; a different game build would land somewhere arbitrary.
    uint8_t Found[sizeof(kExpectedBytes)] = {};
    memcpy(Found, (const void*)s_address, sizeof(Found));
    if (memcmp(Found, kExpectedBytes, sizeof(Found)) != 0)
    {
        Append(StringFormat(
            "time=%.3f event=DS2MultiPlayZone result=signature_mismatch offset=0x%zx found=%02x%02x%02x\n",
            GetSeconds(),
            kProbeOffset,
            Found[0],
            Found[1],
            Found[2]));
        Error("[DS2Zone] assinatura nao confere em +0x%zx; nao instalei", kProbeOffset);
        return true;
    }

    s_original = Found[0];
    s_handler = AddVectoredExceptionHandler(1, ProbeHandler);
    if (s_handler == nullptr)
    {
        Error("[DS2Zone] nao consegui registrar o handler");
        return true;
    }

    if (!WriteByte(s_address, kBreakpointOpcode))
    {
        RemoveVectoredExceptionHandler(s_handler);
        s_handler = nullptr;
        Error("[DS2Zone] nao consegui escrever o breakpoint");
        return true;
    }

    s_installed.store(true);
    Append(StringFormat(
        "time=%.3f event=DS2MultiPlayZone result=installed address=0x%016llx offset=0x%zx\n",
        GetSeconds(),
        (unsigned long long)s_address,
        kProbeOffset));
    Log("[DS2Zone] observando a zona em +0x%zx; ande entre areas", kProbeOffset);
    return true;
#else
    Warning("[DS2Zone] requer Windows x64; nao instalado.");
    return true;
#endif
}

void DS2_MultiPlayZoneProbeHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    if (s_installed.exchange(false))
    {
        WriteByte(s_address, s_original);
    }
    if (s_handler != nullptr)
    {
        RemoveVectoredExceptionHandler(s_handler);
        s_handler = nullptr;
    }
#endif
}

const char* DS2_MultiPlayZoneProbeHook::GetName()
{
    return "DS2 MultiPlay Zone Probe";
}
