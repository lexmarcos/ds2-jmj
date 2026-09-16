/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_TravelWatchHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include "ThirdParty/detours/src/detours.h"
#endif

namespace
{
#if defined(_WIN32) && defined(_M_X64)

    // Version 1.03 Calibrations 2.02.
    constexpr size_t kEntityDtorOffset = 0x3b9ea0;
    constexpr uint8_t kEntityDtorPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x05 };
    constexpr size_t kComponentFreeOffset = 0x3f6300;
    constexpr uint8_t kComponentFreePrologue[] = { 0x40, 0x53, 0x57, 0x41, 0x55, 0x41, 0x57 };
    constexpr size_t kMapObjectsOffset = 0x1c5dd0;
    constexpr uint8_t kMapObjectsPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56 };
    constexpr size_t kMapCharactersOffset = 0x247650;
    constexpr uint8_t kMapCharactersPrologue[] = { 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };

    // MapEntity: the owner of the map at +0x28 (its map id at +0x08), the kind
    // at +0xa2. A MapModelComponent carries its entity at +0x08 and the id of
    // the component it registered at +0xc0.
    constexpr size_t kEntityOwner = 0x28;
    constexpr size_t kOwnerMap = 0x08;
    constexpr size_t kEntityKind = 0xa2;
    constexpr size_t kComponentEntity = 0x08;
    constexpr size_t kComponentRegistered = 0xc8;
    constexpr size_t kComponentId = 0xc0;
    constexpr size_t kEntityAllocator = 0x20;

    // A character's per-frame update, FUN_1403152f0(chr, delta): its model
    // component at chr+0xf0 (slot +0x30 of it is FUN_1403f4f60, the pre-draw),
    // its physics at chr+0x100, its roles at chr+0xb0. The guest's game closed
    // twice inside that pre-draw (15/09 +0x3f4fac and +0x3f510f, 16/09
    // +0x3f687a): the component's memory had been freed with a heap - its
    // +0xd0 held allocator metadata, not a pointer - while the character was
    // still being updated. Before the update runs, the component and the
    // three objects it points at (+0x40 the model instance, +0xc8 the frame
    // registration, +0xd0 the follower) are checked; a character whose
    // component is gone is written down with everything that names it, and
    // skipped for that frame.
    constexpr size_t kChrUpdateOffset = 0x3152f0;
    constexpr uint8_t kChrUpdatePrologue[] = { 0x40, 0x53, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00 };
    constexpr size_t kChrModel = 0xf0;
    constexpr size_t kChrType = 0x54;              // 2: another player's copy
    constexpr size_t kChrRoles = 0xb0;
    constexpr size_t kRole = 0x3c;
    constexpr size_t kChrPosition = 0x90;
    constexpr size_t kChrPhysics = 0x100;
    constexpr size_t kPhysicsContact = 0x10;
    constexpr size_t kContactHandle = 0xe0;
    constexpr size_t kModelInstance = 0x40;
    constexpr size_t kModelFollower = 0xd0;

    // The heaps, the way FUN_1408389e0 finds the one a pointer belongs to:
    // the manager at 0x141627ac0, +0x488 a pointer to {begin, end} of entries
    // of 0x18 bytes, {heap, low, high}. A map part brings its own; when the
    // part goes, its entry goes, and everything allocated from it is freed at
    // once - whatever still points there is what the crashes read.
    constexpr size_t kHeapManagerGlobal = 0x1627ac0;
    constexpr size_t kHeapRanges = 0x488;
    constexpr size_t kHeapEntry = 0x18;
    constexpr int kMaxHeaps = 512;
    constexpr int kGoneRing = 64;

    // The per-frame broadcast of every registered component:
    // FUN_14040d2e0(registry, delta, first bucket, count) walks the buckets at
    // `registry+0x10 + bucket*0x10` and calls slot +0x30 of each node's owner
    // (the node sits at `owner+0x20`, prev at +0x00 and next at +0x08, the way
    // FUN_14040d2b0 unlinks one). Measured 15/09 on the guest, seconds after a
    // travel: a node whose owner had already been freed - its vftable stamped
    // by the allocator (`00b010..`) - was still in the list, and the call
    // through it closed the game (+0x40d33b). Nothing in the four doors below
    // had let that owner go, so whoever frees it does not unlink it.
    //
    // So the walk is done here instead, with one question asked of each node
    // before it is called: is the owner's vftable still inside the module? A
    // node that fails is unlinked and written down, and the game goes on.
    constexpr size_t kBroadcastOffset = 0x40d2e0;
    constexpr uint8_t kBroadcastPrologue[] = { 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x20 };
    constexpr size_t kRegistryCurrent = 0x208;
    constexpr size_t kBucketStride = 0x10;
    constexpr size_t kNodeFromOwner = 0x20;
    constexpr size_t kNodeNext = 0x08;
    constexpr size_t kUpdateSlot = 0x30;
    constexpr uintptr_t kModuleSpan = 0x2000000;
    constexpr uint32_t kNodeGuard = 200000;   // a bucket is never this long

    constexpr size_t kFrames = 6;
    constexpr uint64_t kMaxLines = 4000;   // one travel is a few hundred

    using EntityDtor_p = void*(*)(void* Entity, uint32_t Flags);
    using ComponentFree_p = void(*)(void* Component, char Flag);
    using ByMap_p = void(*)(void* Object, int32_t MapIndex);
    using Broadcast_p = void(*)(void* Registry, void* Argument, uint32_t First, int32_t Count);
    using Update_p = void(*)(void* Owner, void* Argument);
    using ChrUpdate_p = void(*)(void* Chr, float* Delta);
    ChrUpdate_p s_original_chr_update = nullptr;

    struct HeapRange
    {
        uintptr_t Heap = 0;
        uintptr_t Low = 0;
        uintptr_t High = 0;
    };
    // Game thread only.
    HeapRange s_heaps[kMaxHeaps];
    int s_heap_count = 0;
    bool s_heaps_known = false;
    ULONGLONG s_heaps_polled = 0;
    struct GoneHeap
    {
        HeapRange Range;
        ULONGLONG At = 0;
    };
    GoneHeap s_gone[kGoneRing];
    int s_gone_next = 0;
    std::atomic<uint64_t> s_skipped_characters{ 0 };
    uintptr_t s_last_skipped_chr = 0;
    ULONGLONG s_last_skipped_at = 0;

    EntityDtor_p s_original_entity = nullptr;
    ComponentFree_p s_original_component = nullptr;
    ByMap_p s_original_objects = nullptr;
    ByMap_p s_original_characters = nullptr;
    Broadcast_p s_original_broadcast = nullptr;
    std::atomic<uint64_t> s_skipped{ 0 };

    uintptr_t s_base = 0;
    std::filesystem::path s_log_path;
    std::mutex s_log_mutex;
    std::atomic<ULONGLONG> s_open_until{ 0 };
    std::atomic<uint64_t> s_lines{ 0 };
    std::atomic<bool> s_ready{ false };

    std::string Clock()
    {
        SYSTEMTIME Now;
        GetLocalTime(&Now);
        return StringFormat("%02u:%02u:%02u.%03u", Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds);
    }

    void Append(const std::string& Text)
    {
        std::scoped_lock Lock(s_log_mutex);
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream)
        {
            Stream << Text;
        }
    }

    void Note(const std::string& Text);

    bool WindowOpen()
    {
        const ULONGLONG Until = s_open_until.load();
        return Until != 0 && GetTickCount64() < Until && s_lines.load() < kMaxLines;
    }

    // The return addresses, as module offsets, so a line can be read against
    // the binary the way the crash log's are.
    std::string Stack()
    {
        void* Frames[kFrames] = {};
        const USHORT Count = RtlCaptureStackBackTrace(1, (ULONG)kFrames, Frames, nullptr);
        std::string Out;
        for (USHORT i = 0; i < Count; ++i)
        {
            const uintptr_t At = (uintptr_t)Frames[i];
            if (At >= s_base && At < s_base + 0x2000000)
            {
                Out += StringFormat(" +0x%zx", (size_t)(At - s_base));
            }
        }
        return Out;
    }

    // Reading a dying object can fault; a watcher must never be the thing that
    // closes the game.
    bool Peek(uintptr_t At, void* Out, size_t Length)
    {
        __try
        {
            memcpy(Out, (const void*)At, Length);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The map an entity belongs to, and its kind. 0xffffffff when unreadable.
    void DescribeEntity(uintptr_t Entity, uint32_t& Map, uint16_t& Kind)
    {
        Map = 0xffffffff;
        Kind = 0xffff;
        uintptr_t Owner = 0;
        if (Entity != 0 && Peek(Entity + kEntityOwner, &Owner, sizeof(Owner)) && Owner != 0)
        {
            uint32_t Id = 0;
            if (Peek(Owner + kOwnerMap, &Id, sizeof(Id)))
            {
                Map = Id;
            }
        }
        uint16_t Value = 0;
        if (Entity != 0 && Peek(Entity + kEntityKind, &Value, sizeof(Value)))
        {
            Kind = Value;
        }
    }

    // A pointer the game could have handed out: user space, and not a value
    // the allocator stamped over a freed block (those carry bits above 47).
    bool PointerShape(uintptr_t P)
    {
        return P >= 0x10000 && (P >> 47) == 0;
    }

    bool InModule(uintptr_t At)
    {
        return At >= s_base && At < s_base + kModuleSpan;
    }

    // Null, or a live object: pointer-shaped, readable, vftable in the module.
    bool ObjectOrNull(uintptr_t P)
    {
        uintptr_t Vftable = 0;
        return P == 0 || (PointerShape(P) && Peek(P, &Vftable, sizeof(Vftable)) && InModule(Vftable) && (Vftable & 7) == 0);
    }

    // The heap table as it is now. False when it cannot be read.
    bool ReadHeaps(HeapRange* Out, int& Count)
    {
        uintptr_t Manager = 0, Vector = 0, Begin = 0, End = 0;
        Count = 0;
        if (!Peek(s_base + kHeapManagerGlobal, &Manager, sizeof(Manager)) || Manager == 0 ||
            !Peek(Manager + kHeapRanges, &Vector, sizeof(Vector)) || Vector == 0 ||
            !Peek(Vector, &Begin, sizeof(Begin)) || !Peek(Vector + 8, &End, sizeof(End)) ||
            Begin == 0 || End < Begin || (End - Begin) % kHeapEntry != 0)
        {
            return false;
        }
        const size_t Many = (End - Begin) / kHeapEntry;
        if (Many > (size_t)kMaxHeaps)
        {
            return false;
        }
        for (size_t i = 0; i < Many; ++i)
        {
            uintptr_t Entry[3] = {};
            if (!Peek(Begin + i * kHeapEntry, Entry, sizeof(Entry)))
            {
                return false;
            }
            Out[Count].Heap = Entry[0];
            Out[Count].Low = Entry[1];
            Out[Count].High = Entry[2];
            ++Count;
        }
        return true;
    }

    // Once a frame: which heaps went since the last look. Written down only
    // while the window is open; remembered always, so a dead character can be
    // matched to the heap that took its component.
    void PollHeaps()
    {
        const ULONGLONG Now = GetTickCount64();
        if (Now - s_heaps_polled < 15)
        {
            return;
        }
        s_heaps_polled = Now;
        HeapRange Next[kMaxHeaps];
        int Count = 0;
        if (!ReadHeaps(Next, Count))
        {
            return;
        }
        if (s_heaps_known)
        {
            for (int i = 0; i < s_heap_count; ++i)
            {
                bool Still = false;
                for (int k = 0; k < Count && !Still; ++k)
                {
                    Still = Next[k].Heap == s_heaps[i].Heap && Next[k].Low == s_heaps[i].Low;
                }
                if (!Still)
                {
                    s_gone[s_gone_next] = { s_heaps[i], Now };
                    s_gone_next = (s_gone_next + 1) % kGoneRing;
                    if (WindowOpen())
                    {
                        Note(StringFormat("heap %p [%p..%p, %zu KB] sumiu", (void*)s_heaps[i].Heap, (void*)s_heaps[i].Low,
                            (void*)s_heaps[i].High, (size_t)((s_heaps[i].High - s_heaps[i].Low) / 1024)));
                    }
                }
            }
            if (WindowOpen())
            {
                for (int k = 0; k < Count; ++k)
                {
                    bool Was = false;
                    for (int i = 0; i < s_heap_count && !Was; ++i)
                    {
                        Was = s_heaps[i].Heap == Next[k].Heap && s_heaps[i].Low == Next[k].Low;
                    }
                    if (!Was)
                    {
                        Note(StringFormat("heap %p [%p..%p, %zu KB] criado", (void*)Next[k].Heap, (void*)Next[k].Low,
                            (void*)Next[k].High, (size_t)((Next[k].High - Next[k].Low) / 1024)));
                    }
                }
            }
        }
        memcpy(s_heaps, Next, sizeof(HeapRange) * (size_t)Count);
        s_heap_count = Count;
        s_heaps_known = true;
    }

    // Which heap an address sits in: a live one, or one that went (and when).
    std::string WhoseHeap(uintptr_t At)
    {
        for (int i = 0; i < s_heap_count; ++i)
        {
            if (At >= s_heaps[i].Low && At < s_heaps[i].High)
            {
                return StringFormat("heap vivo %p [%p..%p]", (void*)s_heaps[i].Heap, (void*)s_heaps[i].Low, (void*)s_heaps[i].High);
            }
        }
        const ULONGLONG Now = GetTickCount64();
        for (int i = 0; i < kGoneRing; ++i)
        {
            const GoneHeap& G = s_gone[i];
            if (G.At != 0 && At >= G.Range.Low && At < G.Range.High)
            {
                return StringFormat("heap %p [%p..%p] que sumiu ha %llu ms", (void*)G.Range.Heap, (void*)G.Range.Low,
                    (void*)G.Range.High, (unsigned long long)(Now - G.At));
            }
        }
        return "heap desconhecido";
    }

    void Note(const std::string& Text)
    {
        s_lines.fetch_add(1);
        Append(StringFormat("%s  t%lu  %s%s\n", Clock().c_str(), GetCurrentThreadId(), Text.c_str(), Stack().c_str()));
    }

    void* EntityDtorHook(void* Entity, uint32_t Flags)
    {
        if (WindowOpen())
        {
            uint32_t Map = 0;
            uint16_t Kind = 0;
            DescribeEntity((uintptr_t)Entity, Map, Kind);
            Note(StringFormat("MapEntity %p destruida (mapa %08x, tipo %u, flags %u)", Entity, Map, (unsigned)Kind, Flags));
        }
        return s_original_entity(Entity, Flags);
    }

    void ComponentFreeHook(void* Component, char Flag)
    {
        if (WindowOpen())
        {
            uintptr_t Entity = 0, Registered = 0;
            uint32_t Map = 0xffffffff, Id = 0;
            uint16_t Kind = 0;
            if (Peek((uintptr_t)Component + kComponentEntity, &Entity, sizeof(Entity)))
            {
                DescribeEntity(Entity, Map, Kind);
            }
            Peek((uintptr_t)Component + kComponentRegistered, &Registered, sizeof(Registered));
            Peek((uintptr_t)Component + kComponentId, &Id, sizeof(Id));
            uintptr_t Allocator = 0;
            if (Entity != 0)
            {
                Peek(Entity + kEntityAllocator, &Allocator, sizeof(Allocator));
            }
            Note(StringFormat("MapModelComponent %p solto (entidade %p, mapa %08x, tipo %u, +0xc8 %p, id %u, flag %d, heap %p)",
                Component, (void*)Entity, Map, (unsigned)Kind, (void*)Registered, Id, (int)Flag, (void*)Allocator));
        }
        s_original_component(Component, Flag);
    }

    void MapObjectsHook(void* Object, int32_t MapIndex)
    {
        if (WindowOpen())
        {
            Note(StringFormat("objetos do mapa de indice %d desmontados (%p)", MapIndex, Object));
        }
        s_original_objects(Object, MapIndex);
    }

    void MapCharactersHook(void* Object, int32_t MapIndex)
    {
        if (WindowOpen())
        {
            Note(StringFormat("personagens do mapa de indice %d desmontados (%p)", MapIndex, Object));
        }
        s_original_characters(Object, MapIndex);
    }

    // The character's model component and what it points at, checked before
    // the game touches them. `Why` names the first thing wrong.
    bool ModelHealthy(uintptr_t Chr, uintptr_t& Component, uintptr_t Fields[3], const char*& Why)
    {
        Component = 0;
        Fields[0] = Fields[1] = Fields[2] = 0;
        if (!Peek(Chr + kChrModel, &Component, sizeof(Component)))
        {
            Why = "personagem ilegivel";
            return false;
        }
        if (Component == 0)
        {
            return true;   // a character without a model is the game's business
        }
        if (!ObjectOrNull(Component))
        {
            Why = "o componente de modelo (+0xf0) nao e um objeto vivo";
            return false;
        }
        if (!Peek(Component + kModelInstance, &Fields[0], sizeof(Fields[0])) ||
            !Peek(Component + kComponentRegistered, &Fields[1], sizeof(Fields[1])) ||
            !Peek(Component + kModelFollower, &Fields[2], sizeof(Fields[2])))
        {
            Why = "componente ilegivel";
            return false;
        }
        if (!ObjectOrNull(Fields[0]))
        {
            Why = "a instancia do modelo (+0x40) foi liberada";
            return false;
        }
        if (!ObjectOrNull(Fields[1]))
        {
            Why = "o registro no quadro (+0xc8) foi liberado";
            return false;
        }
        if (!ObjectOrNull(Fields[2]))
        {
            Why = "o seguidor (+0xd0) foi liberado";
            return false;
        }
        return true;
    }

    // Everything that names a character, read with care.
    std::string DescribeCharacter(uintptr_t Chr, uintptr_t Component)
    {
        uintptr_t Vftable = 0, Roles = 0, Physics = 0, Contact = 0, Entity = 0;
        uint8_t Type = 0xff, Role = 0xff;
        float At[3] = {};
        uint32_t Handle = 0, Map = 0xffffffff;
        uint16_t Kind = 0xffff;
        Peek(Chr, &Vftable, sizeof(Vftable));
        Peek(Chr + kChrType, &Type, 1);
        if (Peek(Chr + kChrRoles, &Roles, sizeof(Roles)) && PointerShape(Roles))
        {
            Peek(Roles + kRole, &Role, 1);
        }
        Peek(Chr + kChrPosition, At, sizeof(At));
        if (Peek(Chr + kChrPhysics, &Physics, sizeof(Physics)) && PointerShape(Physics) &&
            Peek(Physics + kPhysicsContact, &Contact, sizeof(Contact)) && PointerShape(Contact))
        {
            Peek(Contact + kContactHandle, &Handle, sizeof(Handle));
        }
        if (Component != 0 && Peek(Component + kComponentEntity, &Entity, sizeof(Entity)) && PointerShape(Entity))
        {
            DescribeEntity(Entity, Map, Kind);
        }
        return StringFormat("personagem %p (vftable +0x%zx, tipo %u, papel %u, em (%.2f, %.2f, %.2f), contato %08x, indice de mapa %d; entidade %p mapa %08x tipo %u)",
            (void*)Chr, InModule(Vftable) ? (size_t)(Vftable - s_base) : (size_t)0, (unsigned)Type, (unsigned)Role,
            At[0], At[1], At[2], Handle, (int)((Handle >> 4) & 0x3f), (void*)Entity, Map, (unsigned)Kind);
    }

    void ChrUpdateHook(void* Chr, float* Delta)
    {
        PollHeaps();
        uintptr_t Component = 0, Fields[3] = {};
        const char* Why = "";
        if (ModelHealthy((uintptr_t)Chr, Component, Fields, Why))
        {
            s_original_chr_update(Chr, Delta);
            return;
        }
        // Skipped, and said once a second per character.
        const ULONGLONG Now = GetTickCount64();
        const uint64_t Count = s_skipped_characters.fetch_add(1);
        if (Count < 200 && ((uintptr_t)Chr != s_last_skipped_chr || Now - s_last_skipped_at >= 1000))
        {
            s_last_skipped_chr = (uintptr_t)Chr;
            s_last_skipped_at = Now;
            Append(StringFormat("%s  t%lu  UPDATE PULADO: %s; %s; componente %p (+0x40 %p, +0xc8 %p, +0xd0 %p); o componente esta no %s%s\n",
                Clock().c_str(), GetCurrentThreadId(), Why, DescribeCharacter((uintptr_t)Chr, Component).c_str(), (void*)Component,
                (void*)Fields[0], (void*)Fields[1], (void*)Fields[2], WhoseHeap(Component).c_str(), Stack().c_str()));
            s_lines.fetch_add(1);
        }
    }

    // A node whose owner is gone: unlinked the way FUN_14040d2b0 does, so the
    // next walk of this bucket never sees it again.
    void Unlink(uintptr_t Node)
    {
        uintptr_t Prev = 0, Next = 0;
        if (!Peek(Node, &Prev, sizeof(Prev)) || !Peek(Node + kNodeNext, &Next, sizeof(Next)) ||
            Prev == 0 || Next == 0)
        {
            return;
        }
        __try
        {
            *(uintptr_t*)(Prev + kNodeNext) = Next;
            *(uintptr_t*)Next = Prev;
            *(uintptr_t*)Node = Node;
            *(uintptr_t*)(Node + kNodeNext) = Node;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    bool OwnerAlive(uintptr_t Owner)
    {
        uintptr_t Vftable = 0;
        return Peek(Owner, &Vftable, sizeof(Vftable)) && Vftable >= s_base &&
            Vftable < s_base + kModuleSpan && (Vftable & 7) == 0;
    }

    void BroadcastHook(void* Registry, void* Argument, uint32_t First, int32_t Count)
    {
        const uintptr_t Reg = (uintptr_t)Registry;
        if (Reg == 0 || Count <= 0)
        {
            if (Reg != 0)
            {
                *(uint32_t*)(Reg + kRegistryCurrent) = 0xffffffff;
            }
            return;
        }
        for (uint32_t Bucket = First; Count > 0; --Count, ++Bucket)
        {
            *(uint32_t*)(Reg + kRegistryCurrent) = Bucket;
            const uintptr_t End = Reg + 8 + (uintptr_t)Bucket * kBucketStride;
            uintptr_t Node = *(uintptr_t*)(Reg + 0x10 + (uintptr_t)Bucket * kBucketStride);
            for (uint32_t Guard = 0; Node != End && Node != 0 && Guard < kNodeGuard; ++Guard)
            {
                const uintptr_t Owner = Node - kNodeFromOwner;
                uintptr_t Next = 0;
                if (!Peek(Node + kNodeNext, &Next, sizeof(Next)))
                {
                    break;
                }
                if (!OwnerAlive(Owner))
                {
                    Unlink(Node);
                    if (s_skipped.fetch_add(1) < 40)
                    {
                        Note(StringFormat("no morto na lista do quadro: dono %p sem vftable do jogo; desliguei da lista", (void*)Owner));
                    }
                    Node = Next;
                    continue;
                }
                const Update_p Update = (Update_p)(*(void***)Owner)[kUpdateSlot / sizeof(void*)];
                Update((void*)Owner, Argument);
                // The game re-reads the node's own next here, which is how a
                // handler that removes the node after it gets away with it;
                // the one captured before the call is the fallback.
                uintptr_t After = 0;
                Node = OwnerAlive(Owner) && Peek(Node + kNodeNext, &After, sizeof(After)) && After != 0 ? After : Next;
            }
        }
        *(uint32_t*)(Reg + kRegistryCurrent) = 0xffffffff;
    }

    bool Matches(uintptr_t Address, const uint8_t* Expected, size_t Length)
    {
        return memcmp((const void*)Address, Expected, Length) == 0;
    }

#endif
}

bool DS2_TravelWatchHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    const uintptr_t Base = (uintptr_t)injector.GetBaseAddress();
    if (!Matches(Base + kBroadcastOffset, kBroadcastPrologue, sizeof(kBroadcastPrologue)) ||
        !Matches(Base + kEntityDtorOffset, kEntityDtorPrologue, sizeof(kEntityDtorPrologue)) ||
        !Matches(Base + kComponentFreeOffset, kComponentFreePrologue, sizeof(kComponentFreePrologue)) ||
        !Matches(Base + kMapObjectsOffset, kMapObjectsPrologue, sizeof(kMapObjectsPrologue)) ||
        !Matches(Base + kMapCharactersOffset, kMapCharactersPrologue, sizeof(kMapCharactersPrologue)))
    {
        Error("[DS2TravelWatch] o codigo de uma das quatro portas nao e o esperado; nao instalado");
        return false;
    }

    s_base = Base;
    s_log_path = injector.GetDllPath() / "DS2_TravelWatch.log";
    s_original_entity = (EntityDtor_p)(Base + kEntityDtorOffset);
    s_original_component = (ComponentFree_p)(Base + kComponentFreeOffset);
    s_original_objects = (ByMap_p)(Base + kMapObjectsOffset);
    s_original_characters = (ByMap_p)(Base + kMapCharactersOffset);
    s_original_broadcast = (Broadcast_p)(Base + kBroadcastOffset);
    const bool ChrUpdate = Matches(Base + kChrUpdateOffset, kChrUpdatePrologue, sizeof(kChrUpdatePrologue));
    s_original_chr_update = ChrUpdate ? (ChrUpdate_p)(Base + kChrUpdateOffset) : nullptr;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_broadcast, BroadcastHook);
    if (ChrUpdate)
    {
        DetourAttach(&(PVOID&)s_original_chr_update, ChrUpdateHook);
    }
    DetourAttach(&(PVOID&)s_original_entity, EntityDtorHook);
    DetourAttach(&(PVOID&)s_original_component, ComponentFreeHook);
    DetourAttach(&(PVOID&)s_original_objects, MapObjectsHook);
    DetourAttach(&(PVOID&)s_original_characters, MapCharactersHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        s_original_entity = nullptr;
        Error("[DS2TravelWatch] nao consegui instalar os detours");
        return false;
    }

    s_ready.store(true);
    Append(StringFormat("%s  === ds2os vigia da viagem: quem destroi o que depois de chegar; update de personagem %s ===\n", Clock().c_str(),
        ChrUpdate ? "guardado" : "sem guarda (codigo inesperado)"));
    Log("[DS2TravelWatch] pronto; so escreve enquanto uma viagem estiver aberta");
#endif
    return true;
}

void DS2_TravelWatchHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    s_ready.store(false);
    s_open_until.store(0);
    if (s_original_entity != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_broadcast, BroadcastHook);
        if (s_original_chr_update != nullptr)
        {
            DetourDetach(&(PVOID&)s_original_chr_update, ChrUpdateHook);
        }
        DetourDetach(&(PVOID&)s_original_entity, EntityDtorHook);
        DetourDetach(&(PVOID&)s_original_component, ComponentFreeHook);
        DetourDetach(&(PVOID&)s_original_objects, MapObjectsHook);
        DetourDetach(&(PVOID&)s_original_characters, MapCharactersHook);
        DetourTransactionCommit();
        s_original_entity = nullptr;
    }
#endif
}

const char* DS2_TravelWatchHook::GetName()
{
    return "DS2 Travel Watch";
}

namespace DS2_TravelWatch
{
    void Open(uint32_t Milliseconds, const char* Why)
    {
#if defined(_WIN32) && defined(_M_X64)
        if (!s_ready.load())
        {
            return;
        }
        s_lines.store(0);
        s_open_until.store(GetTickCount64() + Milliseconds);
        Append(StringFormat("%s  --- janela aberta por %u ms: %s ---\n", Clock().c_str(), Milliseconds,
            Why != nullptr ? Why : ""));
#else
        (void)Milliseconds;
        (void)Why;
#endif
    }
}
