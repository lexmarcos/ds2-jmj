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

    // FUN_14017b240, `void*(obj)`: walks the list at obj+0x18, node to node by
    // +0x10, and calls slot 0 of each node's vftable. The same shape as the
    // one above and the same death: on 18/09 the guest faulted at +0x17b266
    // with the node's vftable reading 4254670941466334, which is not a
    // pointer at all but two floats - the block had been freed and handed to
    // something that keeps positions in it.
    //
    // This one is only tidied, not replaced: the list is cut at the first
    // rotten node and the game's own function then does the work.
    constexpr size_t kFindOffset = 0x17b240;
    constexpr uint8_t kFindBytes[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9 };
    constexpr size_t kFindHead = 0x18;
    constexpr size_t kFindNext = 0x10;

    // FUN_1401729a0 is the twin of the one above, byte for byte the same shape
    // on the same list. Measured 18/09: with FUN_14017b240 guarded its own site
    // never came back and the death moved here, at +0x1729c6, with the node's
    // vftable reading 00002817410ec488 - the low half a real address in the
    // game's image and the top half somebody else's.
    //
    // The object in hand was a live MapEntity (its vftable is +0x10e7b68, and
    // the class name sits in plain text right after it). So the map is not what
    // went: one component of an entity that is still alive was freed and left
    // linked in the entity's own list.
    constexpr size_t kFindTwinOffset = 0x1729a0;
    constexpr uint8_t kFindTwinBytes[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9 };

    using FindFn = void*(__fastcall*)(uintptr_t);
    FindFn s_original_find = nullptr;
    FindFn s_original_find_twin = nullptr;

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

    // Walks a list of objects and cuts it at the first one that is no longer
    // an object of the game. Returns how many it kept.
    int Tidy(uintptr_t Head, size_t NextAt, uintptr_t Owner, const char* Which)
    {
        uintptr_t Link = Head;
        uintptr_t Node = 0;
        int Kept = 0;
        if (!ReadPointer(Link, Node))
        {
            return 0;
        }
        while (Node != 0 && Kept < 4096)
        {
            uintptr_t Vftable = 0, Slot = 0;
            if (!Sound(Node, Vftable) || !ReadPointer(Vftable, Slot) || Slot < s_base || Slot >= s_end)
            {
                WritePointer(Link, 0);
                const uint64_t Count = s_cut.fetch_add(1);
                if (Count < 20)
                {
                    Log("[DS2PartNotifyGuard] %s: o no %p da lista de %p nao e mais um objeto do jogo (tabela virtual %p); a lista foi cortada ai.",
                        Which, (void*)Node, (void*)Owner, (void*)Vftable);
                }
                return Kept;
            }
            Link = Node + NextAt;
            if (!ReadPointer(Link, Node))
            {
                return Kept;
            }
            ++Kept;
        }
        return Kept;
    }

    void* __fastcall FindHook(uintptr_t Obj)
    {
        Tidy(Obj + kFindHead, kFindNext, Obj, "busca de componente");
        return s_original_find(Obj);
    }

    // Detours, one target at a time, with the error in hand.
    bool Attach(PVOID* Original, PVOID Hook, size_t Offset)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        const LONG Attached = DetourAttach(Original, Hook);
        if (Attached != NO_ERROR)
        {
            // Closed with a commit rather than an abort: the abort is not in
            // every build of Detours this tree compiles against, and a commit
            // with nothing pending is harmless.
            DetourTransactionCommit();
            Error("[DS2PartNotifyGuard] nao consegui desviar +0x%zx: DetourAttach deu %ld.", Offset, Attached);
            return false;
        }
        const LONG Committed = DetourTransactionCommit();
        if (Committed != NO_ERROR)
        {
            Error("[DS2PartNotifyGuard] nao consegui desviar +0x%zx: o commit deu %ld.", Offset, Committed);
            return false;
        }
        return true;
    }

    void* __fastcall FindTwinHook(uintptr_t Obj)
    {
        Tidy(Obj + kFindHead, kFindNext, Obj, "busca de componente (gemea)");
        return s_original_find_twin(Obj);
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

    const uintptr_t Find = s_base + kFindOffset;
    uint8_t FoundFind[sizeof(kFindBytes)] = {};
    memcpy(FoundFind, (const void*)Find, sizeof(FoundFind));
    if (memcmp(FoundFind, kFindBytes, sizeof(kFindBytes)) != 0)
    {
        Error("[DS2PartNotifyGuard] o prologo em +0x%zx nao e o esperado; nao aplicado.", (size_t)kFindOffset);
        return false;
    }

    const uintptr_t Twin = s_base + kFindTwinOffset;
    uint8_t FoundTwin[sizeof(kFindTwinBytes)] = {};
    memcpy(FoundTwin, (const void*)Twin, sizeof(FoundTwin));
    if (memcmp(FoundTwin, kFindTwinBytes, sizeof(kFindTwinBytes)) != 0)
    {
        Error("[DS2PartNotifyGuard] o prologo em +0x%zx nao e o esperado; nao aplicado.", (size_t)kFindTwinOffset);
        return false;
    }

    // One transaction each, and both return values checked.
    //
    // All three went in one transaction at first, and the third silently did
    // not take: the commit answered NO_ERROR, the log line said three lists
    // were guarded, and reading the live bytes on 18/09 showed a jmp at the
    // first two and the untouched prologue at the third. The crash then landed
    // in the function nobody was watching. A commit that returns NO_ERROR is
    // not a receipt for every attach in it.
    s_original = (NotifyFn)Address;
    s_original_find = (FindFn)Find;
    s_original_find_twin = (FindFn)Twin;
    if (!Attach((PVOID*)&s_original, NotifyHook, kNotifyOffset) ||
        !Attach((PVOID*)&s_original_find, FindHook, kFindOffset) ||
        !Attach((PVOID*)&s_original_find_twin, FindTwinHook, kFindTwinOffset))
    {
        Uninstall();
        return false;
    }

    // Proof, not a report: the first byte of each target is now a jmp.
    const size_t Targets[] = { kNotifyOffset, kFindOffset, kFindTwinOffset };
    for (const size_t At : Targets)
    {
        const uint8_t First = *(const uint8_t*)(s_base + At);
        if (First != 0xE9 && First != 0xEB)
        {
            Error("[DS2PartNotifyGuard] +0x%zx nao foi desviado (primeiro byte %02x); nao aplicado.",
                (size_t)At, First);
            Uninstall();
            return false;
        }
    }

    Log("[DS2PartNotifyGuard] as tres listas passam a ser conferidas antes de andadas (+0x%zx, +0x%zx e +0x%zx).",
        (size_t)kNotifyOffset, (size_t)kFindOffset, (size_t)kFindTwinOffset);
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
        if (s_original_find != nullptr)
        {
            DetourDetach(&(PVOID&)s_original_find, FindHook);
            s_original_find = nullptr;
        }
        if (s_original_find_twin != nullptr)
        {
            DetourDetach(&(PVOID&)s_original_find_twin, FindTwinHook);
            s_original_find_twin = nullptr;
        }
        DetourTransactionCommit();
        s_original = nullptr;
    }
#endif
}

const char* DS2_PartNotifyGuardHook::GetName()
{
    return "DS2 Part Notify Guard";
}
