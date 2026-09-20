/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_NetSyncGuardHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
#if defined(_WIN32) && defined(_M_X64)

    // 24 bytes at +0x517840, ending exactly where the original `call` begins
    // at +0x517858, so that call and the whole epilogue are left alone.
    //
    //   test rcx,rcx                 48 85 c9
    //   je   +0x51786e               74 29        <- the epilogue that returns false
    //   mov  r8,[rcx+0x10]           4c 8b 41 10
    //   cmp  [rcx+0x18],ebx          39 59 18
    //   jle  +0x51786e               7e 20
    //   lea  rcx,[rsp+0x30]          48 8d 4c 24 30
    //   mov  rdx,[r8+rbx*8]          49 8b 14 d8
    //   nop                          90
    constexpr size_t kPatchOffset = 0x517840;
    constexpr uint8_t kPatchBytes[] = {
        0x48, 0x85, 0xC9,
        0x74, 0x29,
        0x4C, 0x8B, 0x41, 0x10,
        0x39, 0x59, 0x18,
        0x7E, 0x20,
        0x48, 0x8D, 0x4C, 0x24, 0x30,
        0x49, 0x8B, 0x14, 0xD8,
        0x90,
    };

    //   movzwl eax,bx                0f b7 c3
    //   mov    r8,[rcx+0x10]         4c 8b 41 10
    //   cmp    [rcx+0x18],eax        39 41 18
    //   jle    +0x51786e             7e 22
    //   movzwl edx,bx                0f b7 d3
    //   lea    rcx,[rsp+0x30]        48 8d 4c 24 30
    //   mov    rdx,[r8+rdx*8]        49 8b 14 d0
    constexpr uint8_t kExpectedBytes[] = {
        0x0F, 0xB7, 0xC3,
        0x4C, 0x8B, 0x41, 0x10,
        0x39, 0x41, 0x18,
        0x7E, 0x22,
        0x0F, 0xB7, 0xD3,
        0x48, 0x8D, 0x4C, 0x24, 0x30,
        0x49, 0x8B, 0x14, 0xD0,
    };

    static_assert(sizeof(kPatchBytes) == sizeof(kExpectedBytes),
        "the guard has to end where the original call begins");

    uintptr_t s_address = 0;
    uint8_t s_original[sizeof(kPatchBytes)] = {};
    bool s_installed = false;

    // Writes into .text, which is not writable, so the page is opened for
    // exactly as long as the copy takes.
    bool WriteCode(uintptr_t Address, const void* From, size_t Length)
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

    std::string Hex(const uint8_t* Bytes, size_t Length)
    {
        std::string Out;
        char Pair[4] = {};
        for (size_t Index = 0; Index < Length; Index++)
        {
            snprintf(Pair, sizeof(Pair), "%02x", Bytes[Index]);
            if (Index > 0)
            {
                Out += ' ';
            }
            Out += Pair;
        }
        return Out;
    }

#endif
}

bool DS2_NetSyncGuardHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    s_address = (uintptr_t)injector.GetBaseAddress() + kPatchOffset;

    // Refuse rather than guess. A build that moved would be corrupted by
    // writing 24 bytes over whatever is there now.
    uint8_t Found[sizeof(kExpectedBytes)] = {};
    memcpy(Found, (const void*)s_address, sizeof(Found));
    if (memcmp(Found, kExpectedBytes, sizeof(kExpectedBytes)) != 0)
    {
        Error("[DS2NetSyncGuard] esperava %s em +0x%zx, achou %s; nao aplicado.",
            Hex(kExpectedBytes, sizeof(kExpectedBytes)).c_str(), (size_t)kPatchOffset,
            Hex(Found, sizeof(Found)).c_str());
        s_address = 0;
        return false;
    }

    memcpy(s_original, (const void*)s_address, sizeof(s_original));

    if (!WriteCode(s_address, kPatchBytes, sizeof(kPatchBytes)))
    {
        Error("[DS2NetSyncGuard] nao foi possivel escrever em +0x%zx.", (size_t)kPatchOffset);
        s_address = 0;
        return false;
    }

    s_installed = true;
    Log("[DS2NetSyncGuard] a tabela de personagens do mapa passa a ser conferida antes de lida em +0x%zx "
        "(a rede parava de matar o jogo quando um mapa era solto).", (size_t)kPatchOffset);
#endif
    return true;
}

void DS2_NetSyncGuardHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    if (s_installed && s_address != 0)
    {
        WriteCode(s_address, s_original, sizeof(s_original));
        s_installed = false;
    }
#endif
}

const char* DS2_NetSyncGuardHook::GetName()
{
    return "DS2 Net Sync Guard";
}
