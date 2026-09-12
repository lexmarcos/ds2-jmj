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

    // A fog wall keeps a mode byte at `this + 0x85`, and everything it does
    // hangs on it: `0x14` zeroes the door's timer and leaves it alone, `0x0a`
    // runs the timer up. Measured on a running game, every door reads `0x14`
    // while the player is alone in a world and `0x0a` from the moment a phantom
    // is in it — on the host's client as well as the guest's, which is the half
    // the first attempt at this got wrong.
    //
    // The byte is written in two places, both as `call FUN_1403f2d30` followed
    // by `mov [rbx+0x85],al`:
    //
    //   +0x1d1381  in the door's init   (FUN_1401d1330)
    //   +0x1d1931  in its update        (FUN_1401d1920), every frame
    //
    // Patching only the init does nothing, because the update writes it again.
    // Both become `mov al,0x14`, so a door is always in the mode it has when
    // nobody is visiting.
    //
    // `FUN_1403f2d30` itself is left alone: six other gimmicks call it, and
    // this is meant to change fog walls, not everything that asks that
    // question.
    struct Site
    {
        size_t Offset;
        uint8_t Expected[5];
    };

    constexpr Site kSites[] = {
        { 0x1d1381, { 0xE8, 0xAA, 0x19, 0x22, 0x00 } },
        { 0x1d1931, { 0xE8, 0xFA, 0x13, 0x22, 0x00 } },
    };
    constexpr uint8_t kPatchBytes[] = { 0xB0, 0x14, 0x90, 0x90, 0x90 };   // mov al,0x14 ; nop ; nop ; nop

    uintptr_t s_addresses[2] = {};
    uint8_t s_original[2][sizeof(kPatchBytes)] = {};
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
    const uintptr_t Base = (uintptr_t)injector.GetBaseAddress();

    // Check both sites before writing either. Half a patch is worse than none:
    // the init would lie and the update would correct it every frame, which is
    // exactly the failure this replaces.
    for (size_t Index = 0; Index < 2; Index++)
    {
        const uintptr_t Address = Base + kSites[Index].Offset;
        uint8_t Found[sizeof(kPatchBytes)] = {};
        memcpy(Found, (const void*)Address, sizeof(Found));
        if (memcmp(Found, kSites[Index].Expected, sizeof(Found)) != 0)
        {
            Error("[DS2PhantomFog] esperava %02x %02x %02x %02x %02x em +0x%zx, achou %02x %02x %02x %02x %02x; nao aplicado.",
                kSites[Index].Expected[0], kSites[Index].Expected[1], kSites[Index].Expected[2],
                kSites[Index].Expected[3], kSites[Index].Expected[4], (size_t)kSites[Index].Offset,
                Found[0], Found[1], Found[2], Found[3], Found[4]);
            return false;
        }
    }

    for (size_t Index = 0; Index < 2; Index++)
    {
        s_addresses[Index] = Base + kSites[Index].Offset;
        memcpy(s_original[Index], (const void*)s_addresses[Index], sizeof(kPatchBytes));
        if (!WriteCode(s_addresses[Index], kPatchBytes, sizeof(kPatchBytes)))
        {
            Error("[DS2PhantomFog] nao foi possivel escrever em +0x%zx.", (size_t)kSites[Index].Offset);
            Uninstall();
            return false;
        }
    }

    s_installed = true;
    Log("[DS2PhantomFog] portas de nevoa presas no modo 0x14 (+0x%zx, +0x%zx).",
        (size_t)kSites[0].Offset, (size_t)kSites[1].Offset);
#endif
    return true;
}

void DS2_PhantomFogHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    for (size_t Index = 0; Index < 2; Index++)
    {
        if (s_addresses[Index] != 0)
        {
            WriteCode(s_addresses[Index], s_original[Index], sizeof(kPatchBytes));
            s_addresses[Index] = 0;
        }
    }
    s_installed = false;
#endif
}

const char* DS2_PhantomFogHook::GetName()
{
    return "DS2 Phantom Fog";
}
