/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_PhantomActionHook.h"
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

    // FUN_140453b30 builds an EventKeyGuideCtrl's 24-bit "who may use this
    // action" mask out of the map object's own row:
    //
    //   +0x453b45  add  al,al          ; row[0x1a] << 1
    //   +0x453b47  or   al,0x1         ; bit 0, always on
    //   +0x453b49  mov  [rcx],al
    //
    // Bit 1 of that byte is the one `FUN_140453760` tests for role 1, the
    // white phantom. Setting it here turns the prompt on for every object with
    // a state machine. See the header for how it was found and measured.
    constexpr size_t kPatchOffset = 0x453b47;
    constexpr uint8_t kPatchBytes[] = { 0x0C, 0x03 };      // or al,0x3
    constexpr uint8_t kExpectedBytes[] = { 0x0C, 0x01 };   // or al,0x1

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

bool DS2_PhantomActionHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    s_address = (uintptr_t)injector.GetBaseAddress() + kPatchOffset;

    // One wrong byte here writes into the middle of an instruction; refuse
    // instead, the way DS2_UnblockMultiPlayHook does.
    uint8_t Found[sizeof(kExpectedBytes)] = {};
    memcpy(Found, (const void*)s_address, sizeof(Found));
    if (memcmp(Found, kExpectedBytes, sizeof(kExpectedBytes)) != 0)
    {
        Error("[DS2PhantomAction] expected 0c 01 at +0x%zx, found %02x %02x; not applied.",
            (size_t)kPatchOffset, Found[0], Found[1]);
        s_address = 0;
        return false;
    }

    memcpy(s_original, (const void*)s_address, sizeof(s_original));
    if (!WriteCode(s_address, kPatchBytes, sizeof(kPatchBytes)))
    {
        Error("[DS2PhantomAction] could not write at +0x%zx.", (size_t)kPatchOffset);
        s_address = 0;
        return false;
    }

    s_installed = true;
    Log("[DS2PhantomAction] a white phantom may use a map object's action (+0x%zx).", (size_t)kPatchOffset);
#endif
    return true;
}

void DS2_PhantomActionHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    if (s_installed && s_address != 0)
    {
        WriteCode(s_address, s_original, sizeof(s_original));
        s_installed = false;
    }
#endif
}

const char* DS2_PhantomActionHook::GetName()
{
    return "DS2 Phantom Action";
}
