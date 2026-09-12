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

    // `FUN_1401d24a0` is the door's decision: it takes the door's kind and the
    // value at `this + 0x80` and returns the state the door should be in.
    // Every branch that matters turns on that field being non-zero —
    //
    //   kind 0:  return (this->0x80 != 0) ? 4 : 0
    //   kind 3:  return (this->0x80 != 0) ? 5 : 0
    //   kind 2:  the closed state only when it is 0
    //
    // — and measured on a running game, `+0x80` is 0 while the player is alone
    // in a world and 5 from the moment a phantom is in it, on the host's client
    // as much as the guest's.
    //
    // It is written in two places, both as `call FUN_14025ea40` followed by
    // `mov [rbx+0x80],eax`:
    //
    //   +0x1d136f  in the door's init    (FUN_1401d1330)
    //   +0x1d195f  in its update         (FUN_1401d1920), every frame
    //
    // The first attempt patched only the init, and the update wrote 5 back on
    // the next frame. Both become `xor eax,eax`.
    //
    // The mode byte at `this + 0x85` moves with a session too, and pinning it
    // changed nothing; it is left alone here so this experiment says one thing.
    struct Site
    {
        size_t Offset;
        uint8_t Expected[5];
    };

    constexpr Site kSites[] = {
        { 0x1d136f, { 0xE8, 0xCC, 0xD6, 0x08, 0x00 } },
        { 0x1d195f, { 0xE8, 0xDC, 0xD0, 0x08, 0x00 } },
    };
    constexpr uint8_t kPatchBytes[] = { 0x31, 0xC0, 0x90, 0x90, 0x90 };   // xor eax,eax ; nop ; nop ; nop

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
    Log("[DS2PhantomFog] portas de nevoa nascem e seguem como mundo proprio (+0x%zx, +0x%zx).",
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
