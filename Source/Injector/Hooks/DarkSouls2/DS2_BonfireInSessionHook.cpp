/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_BonfireInSessionHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#include "ThirdParty/detours/src/detours.h"
#endif

namespace
{
#if defined(_WIN32) && defined(_M_X64)

    // Version 1.03 Calibrations 2.02 (docs/DS2_SEAMLESS_COOP_TASKS.md, M8).
    //
    //   +0x25f690  FUN_14025f690(session): FUN_14025ed80 - 1 < 2
    //   +0x1cb9d9  the return address of its call in FUN_1401cb950 (the rest),
    //              followed by `test al,al ; jne` to message 0x453
    //   +0x199c2e  the return address of its call in FUN_140199a70 (the menu
    //              queue, state 10), followed by `test al,al ; je` past the
    //              cancel
    //   +0x17ee9d  FUN_14017ed90, rest job state 2: `je +0x17eeb8` after
    //              FUN_14025ea40; taken always, the menu is not cancelled
    constexpr size_t kSessionUpOffset = 0x25f690;
    constexpr uint8_t kSessionUpPrologue[] = { 0x48, 0x83, 0xec, 0x28, 0xe8, 0xe7, 0xf6, 0xff, 0xff, 0xff, 0xc8, 0x83, 0xf8, 0x01 };
    constexpr size_t kRestReturn = 0x1cb9d9;
    constexpr uint8_t kRestAfter[] = { 0x84, 0xc0, 0x75, 0x28 };
    constexpr size_t kQueueReturn = 0x199c2e;
    constexpr uint8_t kQueueAfter[] = { 0x84, 0xc0, 0x74, 0x08 };
    constexpr size_t kJobBranch = 0x17ee9d;
    constexpr uint8_t kJobExpected[] = { 0x74, 0x19 };
    constexpr uint8_t kJobPatch[] = { 0xeb, 0x19 };

    // The local character's role: *(*0x1416148f0 + 0xd0) -> +0xb0 -> +0x3c.
    constexpr size_t kGameGlobal = 0x16148f0;
    constexpr size_t kLocalCharacter = 0xd0;
    constexpr size_t kRoles = 0xb0;
    constexpr size_t kRole = 0x3c;

    using SessionUp_p = uint64_t(*)(void* Session);
    SessionUp_p s_original = nullptr;
    uintptr_t s_base = 0;
    bool s_job_patched = false;
    std::atomic<uint64_t> s_answered{ 0 };

    bool ReadByte(uintptr_t At, uint8_t& Out)
    {
        __try
        {
            Out = *(const uint8_t*)At;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadPointer(uintptr_t At, uintptr_t& Out)
    {
        __try
        {
            Out = *(const uintptr_t*)At;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool OwnsTheWorld()
    {
        uintptr_t Context = 0, Character = 0, Roles = 0;
        uint8_t Role = 0xff;
        return ReadPointer(s_base + kGameGlobal, Context) && Context != 0 &&
            ReadPointer(Context + kLocalCharacter, Character) && Character != 0 &&
            ReadPointer(Character + kRoles, Roles) && Roles != 0 &&
            ReadByte(Roles + kRole, Role) && Role == 0;
    }

    uint64_t SessionUpHook(void* Session)
    {
        const uintptr_t Caller = (uintptr_t)_ReturnAddress();
        const uint64_t Answer = s_original(Session);
        if ((uint8_t)Answer != 0 && (Caller == s_base + kRestReturn || Caller == s_base + kQueueReturn) && OwnsTheWorld())
        {
            s_answered.fetch_add(1, std::memory_order_relaxed);
            return Answer & ~(uint64_t)0xff;
        }
        return Answer;
    }

    bool WriteCode(uintptr_t Address, const uint8_t* From, size_t Length)
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

    bool Matches(uintptr_t Address, const uint8_t* Expected, size_t Length)
    {
        return memcmp((const void*)Address, Expected, Length) == 0;
    }

#endif
}

bool DS2_BonfireInSessionHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    const uintptr_t Base = (uintptr_t)injector.GetBaseAddress();
    if (!Matches(Base + kSessionUpOffset, kSessionUpPrologue, sizeof(kSessionUpPrologue)) ||
        !Matches(Base + kRestReturn, kRestAfter, sizeof(kRestAfter)) ||
        !Matches(Base + kQueueReturn, kQueueAfter, sizeof(kQueueAfter)) ||
        !Matches(Base + kJobBranch, kJobExpected, sizeof(kJobExpected)))
    {
        Error("[DS2BonfireInSession] o codigo de uma das tres travas nao e o esperado; nao aplicado");
        return false;
    }
    s_base = Base;

    s_original = (SessionUp_p)(Base + kSessionUpOffset);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original, SessionUpHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        s_original = nullptr;
        Error("[DS2BonfireInSession] nao consegui instalar o detour");
        return false;
    }

    if (!WriteCode(Base + kJobBranch, kJobPatch, sizeof(kJobPatch)))
    {
        Error("[DS2BonfireInSession] nao consegui escrever em +0x%zx", (size_t)kJobBranch);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original, SessionUpHook);
        DetourTransactionCommit();
        s_original = nullptr;
        return false;
    }
    s_job_patched = true;
    Log("[DS2BonfireInSession] o dono do mundo descansa em fogueira com a sessao de pe");
#endif
    return true;
}

void DS2_BonfireInSessionHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    if (s_job_patched)
    {
        WriteCode(s_base + kJobBranch, kJobExpected, sizeof(kJobExpected));
        s_job_patched = false;
    }
    if (s_original != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original, SessionUpHook);
        DetourTransactionCommit();
        s_original = nullptr;
    }
#endif
}

const char* DS2_BonfireInSessionHook::GetName()
{
    return "DS2 Bonfire In Session";
}
