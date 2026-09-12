/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_PhantomFogHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"

#include <cstdint>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
#if defined(_WIN32) && defined(_M_X64)

    // In FUN_1401d1330, the white door's init:
    //
    //   +0x1d1362  call 0x140513270         ; [FeManager + 0x3b8]
    //   +0x1d136f  call 0x14025ea40         ; -> eax, the phantom type   <- here
    //   +0x1d1374  jmp  +0x1d1378
    //   +0x1d1376  mov  eax,edi             ; the path that already stamps 0
    //   +0x1d137b  mov  [rbx+0x80],eax      ; the stamp
    //
    // The branch above already has a "stamp zero" path, for when the frontend
    // has no player object yet, so zero is a value the rest of the init is
    // written to accept. Replacing the call with `xor eax,eax` takes that path's
    // value without taking its branch, and the three NOPs keep the following
    // instruction where the jump expects it.
    constexpr size_t kPatchOffset = 0x1d136f;
    constexpr uint8_t kPatchBytes[] = { 0x31, 0xC0, 0x90, 0x90, 0x90 };       // xor eax,eax ; nop ; nop ; nop
    constexpr uint8_t kExpectedBytes[] = { 0xE8, 0xCC, 0xD6, 0x08, 0x00 };    // call 0x14025ea40

    uintptr_t s_address = 0;
    uint8_t s_original[sizeof(kPatchBytes)] = {};
    bool s_installed = false;

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

#endif
}

bool DS2_PhantomFogHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    s_address = (uintptr_t)injector.GetBaseAddress() + kPatchOffset;

    // Refuse rather than guess: a different build means these bytes belong to
    // something else, and writing anyway would corrupt it.
    uint8_t Found[sizeof(kExpectedBytes)] = {};
    memcpy(Found, (const void*)s_address, sizeof(Found));
    if (memcmp(Found, kExpectedBytes, sizeof(kExpectedBytes)) != 0)
    {
        Error("[DS2PhantomFog] esperava %02x %02x %02x %02x %02x em +0x%zx, achou %02x %02x %02x %02x %02x; nao aplicado.",
            kExpectedBytes[0], kExpectedBytes[1], kExpectedBytes[2], kExpectedBytes[3], kExpectedBytes[4],
            (size_t)kPatchOffset, Found[0], Found[1], Found[2], Found[3], Found[4]);
        s_address = 0;
        return false;
    }

    memcpy(s_original, (const void*)s_address, sizeof(s_original));

    if (!WriteCode(s_address, kPatchBytes, sizeof(kPatchBytes)))
    {
        Error("[DS2PhantomFog] nao foi possivel escrever em +0x%zx.", (size_t)kPatchOffset);
        s_address = 0;
        return false;
    }

    s_installed = true;
    Log("[DS2PhantomFog] portas de nevoa nascem como dono do mundo (+0x%zx).", (size_t)kPatchOffset);
#endif
    return true;
}

void DS2_PhantomFogHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    if (s_installed && s_address != 0)
    {
        WriteCode(s_address, s_original, sizeof(s_original));
        s_installed = false;
    }
#endif
}

const char* DS2_PhantomFogHook::GetName()
{
    return "DS2 Phantom Fog";
}
