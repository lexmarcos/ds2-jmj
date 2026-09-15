/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_HollowSummonHook.h"
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

    // FUN_1402a1980 decides, every frame from SummonSignSetCtrl, whether a sign
    // of a given type can be used by the local player. For the types whose
    // role row in 0x1410c0050 is 2 or 3 it refuses when the local player is
    // hollow:
    //
    //   +0x2a1b1e  call FUN_1402ab0e0      ; *(ctx+0xd0)->+0x490->+0x1ac mapped
    //   +0x2a1b23  test al,al              ;   by FUN_140203db0: hollow?
    //   +0x2a1b25  jne  +0x2a1b47          ; yes -> return false
    //   +0x2a1b27  call FUN_1402aabf0      ; the other refusal (state 8) stays
    //
    // The call becomes `xor eax,eax` and a three-byte nop, so the filter reads
    // "not hollow" and falls through to its remaining checks. FUN_1402ab0e0 is
    // left alone: the same getter reports the player's status to the server
    // (FUN_140297f20, case 8) and gates items in FUN_1402a1bf0, neither of
    // which this is about.
    //
    // Found with a read watch on PlayerParam+0x1ac (`wpr`) while a hollow host
    // stood by a delivered sign; poked live on 14/09 it brought the prompt
    // back, and the summon reached the server with both players hollow.
    constexpr size_t kPatchOffset = 0x2a1b1e;
    constexpr uint8_t kPatchBytes[] = { 0x31, 0xC0, 0x0F, 0x1F, 0x00 };      // xor eax,eax ; nop dword [rax]
    constexpr uint8_t kExpectedBytes[] = { 0xE8, 0xBD, 0x95, 0x00, 0x00 };   // call FUN_1402ab0e0

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

bool DS2_HollowSummonHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    s_address = (uintptr_t)injector.GetBaseAddress() + kPatchOffset;

    // A relative call's bytes move with any rebuild of the game; refuse rather
    // than write over whatever is there now.
    uint8_t Found[sizeof(kExpectedBytes)] = {};
    memcpy(Found, (const void*)s_address, sizeof(Found));
    if (memcmp(Found, kExpectedBytes, sizeof(kExpectedBytes)) != 0)
    {
        Error("[DS2HollowSummon] esperava e8 bd 95 00 00 em +0x%zx, achou %02x %02x %02x %02x %02x; nao aplicado.",
            (size_t)kPatchOffset, Found[0], Found[1], Found[2], Found[3], Found[4]);
        s_address = 0;
        return false;
    }

    memcpy(s_original, (const void*)s_address, sizeof(s_original));
    if (!WriteCode(s_address, kPatchBytes, sizeof(kPatchBytes)))
    {
        Error("[DS2HollowSummon] nao foi possivel escrever em +0x%zx.", (size_t)kPatchOffset);
        s_address = 0;
        return false;
    }

    s_installed = true;
    Log("[DS2HollowSummon] placas de invocacao visiveis para o host hollow (+0x%zx).", (size_t)kPatchOffset);
#endif
    return true;
}

void DS2_HollowSummonHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    if (s_installed && s_address != 0)
    {
        WriteCode(s_address, s_original, sizeof(s_original));
        s_installed = false;
    }
#endif
}

const char* DS2_HollowSummonHook::GetName()
{
    return "DS2 Hollow Summon";
}
