/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_SessionSlotsHook.h"
#include "Injector/Hooks/DarkSouls2/DS2_SessionSlotPatchTable.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"

#include <cstdint>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
#if defined(_WIN32) && defined(_M_X64)

    constexpr size_t kPatchCount = sizeof(kDS2SessionSlotPatches) / sizeof(kDS2SessionSlotPatches[0]);

    bool s_installed = false;
    uintptr_t s_base = 0;

    // .text is not writable, so the page is opened for exactly as long as the
    // copy takes.
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

bool DS2_SessionSlotsHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    s_base = (uintptr_t)injector.GetBaseAddress();

    // Check everything first. The table describes one coherent change of
    // layout across two objects; applying part of it would leave the game
    // reading its players out of two different places.
    int Mismatched = 0;
    for (size_t i = 0; i < kPatchCount; i++)
    {
        const DS2_SessionSlotPatch& Patch = kDS2SessionSlotPatches[i];
        uint8_t Found[sizeof(Patch.Expected)] = {};
        memcpy(Found, (const void*)(s_base + Patch.Offset), Patch.Length);

        if (memcmp(Found, Patch.Expected, Patch.Length) != 0)
        {
            if (Mismatched < 4)
            {
                Error("[DS2SessionSlots] +0x%x (%s) nao confere: esperava %02x %02x %02x, achou %02x %02x %02x.",
                    Patch.Offset, Patch.Note,
                    Patch.Expected[0], Patch.Expected[1], Patch.Expected[2],
                    Found[0], Found[1], Found[2]);
            }
            Mismatched++;
        }
    }

    if (Mismatched > 0)
    {
        Error("[DS2SessionSlots] %d de %zu posicoes nao conferem; nada aplicado. A tabela foi feita para a versao 1.03, Calibrations 2.02.",
            Mismatched, kPatchCount);
        s_base = 0;
        return false;
    }

    for (size_t i = 0; i < kPatchCount; i++)
    {
        const DS2_SessionSlotPatch& Patch = kDS2SessionSlotPatches[i];
        if (!WriteCode(s_base + Patch.Offset, Patch.Patched, Patch.Length))
        {
            Error("[DS2SessionSlots] nao foi possivel escrever em +0x%x (%s); revertendo.",
                Patch.Offset, Patch.Note);

            for (size_t j = 0; j < i; j++)
            {
                const DS2_SessionSlotPatch& Done = kDS2SessionSlotPatches[j];
                WriteCode(s_base + Done.Offset, Done.Expected, Done.Length);
            }

            s_base = 0;
            return false;
        }
    }

    s_installed = true;
    Log("[DS2SessionSlots] %zu posicoes aplicadas: sessao de %d jogadores contando o host.",
        kPatchCount, kDS2SessionSlotRemotePlayers + 1);
#endif
    return true;
}

void DS2_SessionSlotsHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    if (!s_installed || s_base == 0)
    {
        return;
    }

    for (size_t i = 0; i < kPatchCount; i++)
    {
        const DS2_SessionSlotPatch& Patch = kDS2SessionSlotPatches[i];
        WriteCode(s_base + Patch.Offset, Patch.Expected, Patch.Length);
    }

    s_installed = false;
#endif
}

const char* DS2_SessionSlotsHook::GetName()
{
    return "DS2 Session Slots";
}
