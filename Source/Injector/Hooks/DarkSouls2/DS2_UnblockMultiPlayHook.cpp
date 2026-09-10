/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_UnblockMultiPlayHook.h"
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

    // FUN_1403c0890 answers "is multiplay allowed where the player is". It
    // looks up a per-map record by packed map id and wants a non-zero first
    // byte with [block+0x16] == 0. Majula's +0x16 is already zero, so it is
    // the per-map record that is missing: Majula simply has no multiplay data
    // in that table.
    //
    //   +0x3c0890  mov [rsp+0x18],rbx     <- overwritten
    //   +0x3c0895  push rsi
    //   +0x3c0896  sub  rsp,0x20
    //
    // Replaced by `mov al,1; ret`. Three bytes over a five byte instruction is
    // safe here because the ret leaves before the remainder is ever reached.
    //
    // Its single caller is a per-frame updater that edge-tracks the result: on
    // a false edge it raises both counters at +8 and +9 of the object at
    // [[0x141616cf8+0x18]], and on a true edge it lowers them. Returning true
    // therefore drains them through the game's own logic within a frame, which
    // is what makes this safe to leave in place across a world transition.
    constexpr size_t kPatchOffset = 0x3c0890;
    constexpr uint8_t kPatchBytes[] = { 0xB0, 0x01, 0xC3 };            // mov al,1 ; ret
    constexpr uint8_t kExpectedBytes[] = { 0x48, 0x89, 0x5C };         // mov [rsp+0x18],rbx

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

#endif
}

bool DS2_UnblockMultiPlayHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    s_address = (uintptr_t)injector.GetBaseAddress() + kPatchOffset;

    // Refuse rather than guess. If the bytes are not what this patch was
    // written against, the build has moved and writing anyway would corrupt
    // whatever is there now.
    uint8_t Found[sizeof(kExpectedBytes)] = {};
    memcpy(Found, (const void*)s_address, sizeof(Found));
    if (memcmp(Found, kExpectedBytes, sizeof(kExpectedBytes)) != 0)
    {
        Error("[DS2UnblockMultiPlay] esperava %02x %02x %02x em +0x%zx, achou %02x %02x %02x; nao aplicado.",
            kExpectedBytes[0], kExpectedBytes[1], kExpectedBytes[2], (size_t)kPatchOffset,
            Found[0], Found[1], Found[2]);
        s_address = 0;
        return false;
    }

    memcpy(s_original, (const void*)s_address, sizeof(s_original));

    if (!WriteCode(s_address, kPatchBytes, sizeof(kPatchBytes)))
    {
        Error("[DS2UnblockMultiPlay] nao foi possivel escrever em +0x%zx.", (size_t)kPatchOffset);
        s_address = 0;
        return false;
    }

    s_installed = true;
    Log("[DS2UnblockMultiPlay] bloqueio de multiplayer removido em +0x%zx (multiplayer em Majula).",
        (size_t)kPatchOffset);
#endif
    return true;
}

void DS2_UnblockMultiPlayHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    if (s_installed && s_address != 0)
    {
        WriteCode(s_address, s_original, sizeof(s_original));
        s_installed = false;
    }
#endif
}

const char* DS2_UnblockMultiPlayHook::GetName()
{
    return "DS2 Unblock MultiPlay";
}
