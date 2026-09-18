/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_PartNotifyGuardHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include "ThirdParty/detours/src/detours.h"
#endif

namespace
{
#if defined(_WIN32) && defined(_M_X64)

    // FUN_1403f3ae0, `void(obj, uint32, uint8)`.
    constexpr size_t kNotifyOffset = 0x3f3ae0;
    constexpr uint8_t kNotifyBytes[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10 };
    constexpr size_t kListHead = 0x38;

    using NotifyFn = void(__fastcall*)(uintptr_t, uint32_t, uint8_t);
    NotifyFn s_original = nullptr;

    uintptr_t s_base = 0;
    uintptr_t s_end = 0;
    std::atomic<uint64_t> s_cut{ 0 };

    bool ReadPointer(uintptr_t Address, uintptr_t& Out)
    {
        __try
        {
            Out = *(const uintptr_t*)Address;
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    bool WritePointer(uintptr_t Address, uintptr_t Value)
    {
        __try
        {
            *(uintptr_t*)Address = Value;
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    // A listener is worth calling only if its vftable is a pointer into the
    // game's own image. Everything the game registers here has one; a freed
    // block that something else has written over does not.
    bool Sound(uintptr_t Node, uintptr_t& Vftable)
    {
        if (Node == 0 || (Node & 7) != 0)
        {
            return false;
        }
        if (!ReadPointer(Node, Vftable))
        {
            return false;
        }
        return Vftable >= s_base && Vftable < s_end && (Vftable & 7) == 0;
    }

    void __fastcall NotifyHook(uintptr_t Obj, uint32_t Arg, uint8_t Flag)
    {
        uintptr_t Link = Obj + kListHead;
        uintptr_t Node = 0;
        if (!ReadPointer(Link, Node))
        {
            return;
        }

        while (Node != 0)
        {
            uintptr_t Vftable = 0;
            if (!Sound(Node, Vftable))
            {
                // Cut here and keep the cut: what follows is only reachable
                // through this node, and next frame would walk it again.
                WritePointer(Link, 0);
                const uint64_t Count = s_cut.fetch_add(1);
                if (Count < 20)
                {
                    Log("[DS2PartNotifyGuard] no %p da lista de %p nao e mais um objeto do jogo (tabela virtual %p); a lista foi cortada ai.",
                        (void*)Node, (void*)Obj, (void*)Vftable);
                }
                return;
            }

            uintptr_t Slot = 0;
            if (!ReadPointer(Vftable + 8, Slot) || Slot < s_base || Slot >= s_end)
            {
                WritePointer(Link, 0);
                const uint64_t Count = s_cut.fetch_add(1);
                if (Count < 20)
                {
                    Log("[DS2PartNotifyGuard] o no %p aponta para fora do jogo (%p); a lista foi cortada ai.",
                        (void*)Node, (void*)Slot);
                }
                return;
            }

            ((void(__fastcall*)(uintptr_t, uintptr_t, uint32_t, uint8_t))Slot)(Node, Obj, Arg, Flag);

            // The list is re-read after the call, the way the game does it:
            // a listener may take itself out while it runs.
            Link = Node + 8;
            if (!ReadPointer(Link, Node))
            {
                return;
            }
        }
    }

#endif
}

bool DS2_PartNotifyGuardHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    s_base = (uintptr_t)injector.GetBaseAddress();

    // The image's own headers, so nothing has to be linked for it.
    const IMAGE_DOS_HEADER* Dos = (const IMAGE_DOS_HEADER*)s_base;
    const IMAGE_NT_HEADERS64* Nt = (const IMAGE_NT_HEADERS64*)(s_base + Dos->e_lfanew);
    if (Dos->e_magic != IMAGE_DOS_SIGNATURE || Nt->Signature != IMAGE_NT_SIGNATURE ||
        Nt->OptionalHeader.SizeOfImage == 0)
    {
        Error("[DS2PartNotifyGuard] nao consegui medir a imagem do jogo; nao aplicado.");
        return false;
    }
    s_end = s_base + Nt->OptionalHeader.SizeOfImage;

    const uintptr_t Address = s_base + kNotifyOffset;
    uint8_t Found[sizeof(kNotifyBytes)] = {};
    memcpy(Found, (const void*)Address, sizeof(Found));
    if (memcmp(Found, kNotifyBytes, sizeof(kNotifyBytes)) != 0)
    {
        Error("[DS2PartNotifyGuard] o prologo em +0x%zx nao e o esperado; nao aplicado.", (size_t)kNotifyOffset);
        return false;
    }

    s_original = (NotifyFn)Address;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original, NotifyHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        Error("[DS2PartNotifyGuard] nao consegui instalar o detour em +0x%zx.", (size_t)kNotifyOffset);
        s_original = nullptr;
        return false;
    }

    Log("[DS2PartNotifyGuard] a lista de ouvintes de parte passa a ser conferida antes de chamada (+0x%zx).",
        (size_t)kNotifyOffset);
#endif
    return true;
}

void DS2_PartNotifyGuardHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    if (s_original != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original, NotifyHook);
        DetourTransactionCommit();
        s_original = nullptr;
    }
#endif
}

const char* DS2_PartNotifyGuardHook::GetName()
{
    return "DS2 Part Notify Guard";
}
