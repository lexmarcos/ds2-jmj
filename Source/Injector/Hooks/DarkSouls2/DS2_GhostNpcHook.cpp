/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_GhostNpcHook.h"
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

    // Both sites are inside FUN_140312dd0, the character's Initialize, and
    // both test the kind table at 0x1410bfff0. See the header.
    //
    //   +0x31335e  cmpb $0x3,0x1(%rcx,%rax,1)
    //   +0x313363  jne  +0x313385          ; -> the host's path
    //   ...
    //   +0x31337c  movl $0xf,0x48(%rax)    ; the ghost draw type
    //
    //   +0x3141cd  cmpb $0x3,0x1(%rdx,%r12,1)
    //   +0x3141d3  jne  +0x3141e2          ; -> past the collision override
    //   +0x3141d5  cmpb $0x0,0x2(%rdx,%r12,1)
    //   +0x3141de  cmove %r14d,%ecx        ; force "drop the collision bit"
    //
    // Turning each `jne` into a `jmp` takes the branch the host's kinds
    // already take. One byte each, the opcode.
    struct Patch
    {
        size_t Offset;
        uint8_t Expected;
        uint8_t Replacement;
        const char* What;
    };
    constexpr Patch kPatches[] = {
        { 0x313363, 0x75, 0xEB, "draw" },
        { 0x3141d3, 0x75, 0xEB, "collision" },
    };
    constexpr size_t kPatchCount = sizeof(kPatches) / sizeof(kPatches[0]);

    uintptr_t s_address[kPatchCount] = {};
    uint8_t s_original[kPatchCount] = {};
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

bool DS2_GhostNpcHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    const uintptr_t Base = (uintptr_t)injector.GetBaseAddress();

    // Every byte is checked before any of them is written, so a game that is
    // not this build is left exactly as it was.
    for (size_t i = 0; i < kPatchCount; ++i)
    {
        const uintptr_t At = Base + kPatches[i].Offset;
        uint8_t Found = 0;
        memcpy(&Found, (const void*)At, 1);
        if (Found != kPatches[i].Expected)
        {
            Error("[DS2GhostNpc] expected %02x at +0x%zx (%s), found %02x; nothing applied.",
                kPatches[i].Expected, kPatches[i].Offset, kPatches[i].What, Found);
            return false;
        }
    }

    for (size_t i = 0; i < kPatchCount; ++i)
    {
        s_address[i] = Base + kPatches[i].Offset;
        memcpy(&s_original[i], (const void*)s_address[i], 1);
        if (!WriteCode(s_address[i], &kPatches[i].Replacement, 1))
        {
            Error("[DS2GhostNpc] could not write at +0x%zx (%s).", kPatches[i].Offset, kPatches[i].What);
            for (size_t j = 0; j < i; ++j)
            {
                WriteCode(s_address[j], &s_original[j], 1);
            }
            return false;
        }
    }

    s_installed = true;
    Log("[DS2GhostNpc] a guest's NPCs are drawn and collide as the host's (+0x%zx, +0x%zx).",
        kPatches[0].Offset, kPatches[1].Offset);
#endif
    return true;
}

void DS2_GhostNpcHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    if (s_installed)
    {
        for (size_t i = 0; i < kPatchCount; ++i)
        {
            if (s_address[i] != 0)
            {
                WriteCode(s_address[i], &s_original[i], 1);
            }
        }
        s_installed = false;
    }
#endif
}

const char* DS2_GhostNpcHook::GetName()
{
    return "DS2 Ghost NPC";
}
