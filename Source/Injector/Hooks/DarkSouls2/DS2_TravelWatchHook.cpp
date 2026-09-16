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

    // The per-frame update of a MapModelComponent, FUN_1403f4f60(component,
    // delta), the pre-draw the CharacterManager runs for a character's model.
    // The guest's game closed three times inside it (15/09 +0x3f4fac and
    // +0x3f510f, 16/09 +0x3f687a): the component's memory had been freed with
    // a heap - its +0xd0 held allocator metadata, not a pointer - while the
    // character it belongs to was still being updated. Before the update runs,
    // the component and the three objects it points at (+0x40 the model
    // instance, +0xc8 the frame registration, +0xd0 the follower) are checked;
    // a component that is gone is written down with everything that names its
    // owner, and skipped for that frame. (A first version guarded the
    // character's update, FUN_1403152f0, through chr+0xf0 - which is not this
    // component, and every character was skipped. 16/09.)
    constexpr size_t kModelUpdateOffset = 0x3f4f60;
    constexpr uint8_t kModelUpdatePrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x81, 0xec, 0x90, 0x00, 0x00, 0x00 };
    constexpr size_t kPlayerCtrlVftable = 0x10e4bb8;
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

    // The other two per-frame jobs of a MapModelComponent, besides the
    // pre-draw: FUN_1403f41d0(component, arg) is the post-physics task the
    // CharacterManager runs, and it calls slot +0x18 of the object **embedded**
    // at component+0x50. That embedded object's vftable is what was corrupt on
    // 16/09 (+0x3f4230, `call *0x18(%rax)` with rax = 00b54001410e86d8).
    constexpr size_t kPostPhysicsOffset = 0x3f41d0;
    constexpr uint8_t kPostPhysicsPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0x49, 0x08 };
    constexpr size_t kEmbedded = 0x50;          // an embedded object: its vftable sits here

    // FUN_1401cbf20(owner): walks the list at owner+0x18, calling slot 0 of
    // each node and following node[1] and node[2]. It died on a freed node on
    // 15/09 (+0x1cbf40). A lookup that faults answers "not found" instead of
    // closing the game.
    // FUN_14040cea0(registry, node, free): takes a component out of the
    // frame's lists. It opens by calling slot +0x28 of the node and then walks
    // the list at registry+0x18, and it is reached from a dozen places, most
    // of them outside any teardown this file already guards - which is where
    // the guest died on 16/09 (+0x40cee3, and +0x40d2c7 in the unlink it
    // calls). A fault here means the component stays registered: it leaks,
    // and the game lives.
    // FUN_140354e80(task, arg): the generic task runner. It calls the work,
    // `task[2](task[1], arg, task+3)`, and then the completion, virtual slot 0
    // of the task itself. The CharacterManager's post-physics work for one
    // character goes through here (FUN_140359e80 builds those task items), and
    // that is where the **host** closed twice on 16/09 with the same stack both
    // times: `+0x36f846` and `+0xbd15c5`, reading a pointer that held float
    // data instead. Running the work under __try and the completion **always**
    // means one character loses a frame of animation rather than everyone
    // losing the session, and the task's own bookkeeping still balances.
    constexpr size_t kTaskRunOffset = 0x354e80;
    constexpr uint8_t kTaskRunPrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x4c, 0x8d, 0x41, 0x18, 0x48, 0x8b, 0x49, 0x08, 0xff, 0x53, 0x10 };

    constexpr size_t kUnregisterOffset = 0x40cea0;
    constexpr uint8_t kUnregisterPrologue[] = { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0xc0, 0x00, 0x00, 0x00, 0x48, 0x89, 0x6c, 0x24, 0x10 };

    constexpr size_t kListLookupOffset = 0x1cbf20;
    constexpr uint8_t kListLookupPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8 };

    constexpr size_t kFrames = 6;
    constexpr uint64_t kMaxLines = 4000;   // one travel is a few hundred

    using EntityDtor_p = void*(*)(void* Entity, uint32_t Flags);
    using ComponentFree_p = void(*)(void* Component, char Flag);
    using ByMap_p = void(*)(void* Object, int32_t MapIndex);
    using Broadcast_p = void(*)(void* Registry, void* Argument, uint32_t First, int32_t Count);
    using Update_p = void(*)(void* Owner, void* Argument);
    using ModelUpdate_p = void(*)(void* Component, float* Delta);
    ModelUpdate_p s_original_model_update = nullptr;
    using PostPhysics_p = void(*)(void* Component, void* Argument);
    PostPhysics_p s_original_post_physics = nullptr;
    using ListLookup_p = void*(*)(void* Owner);
    ListLookup_p s_original_lookup = nullptr;
    using Unregister_p = void(*)(void* Registry, void* Node, char Free);
    Unregister_p s_original_unregister = nullptr;
    using TaskRun_p = void(*)(void** Task, void* Argument);
    TaskRun_p s_original_task_run = nullptr;
    using TaskWork_p = void(*)(void* Owner, void* Argument, void* Info);
    using TaskDone_p = void(*)(void* Task, int32_t Flag);
    std::atomic<uint64_t> s_caught_task{ 0 };
    std::atomic<uint64_t> s_corrupt_seen{ 0 };
    uintptr_t s_corrupt_last = 0;     // game thread only
    std::atomic<uint64_t> s_skipped_physics{ 0 };
    std::atomic<uint64_t> s_caught{ 0 };

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
    bool GuardedModelUpdate(ModelUpdate_p Fn, void* Component, float* Delta);
    bool GuardedPostPhysics(PostPhysics_p Fn, void* Component, void* Argument);
    bool GuardedFree(ComponentFree_p Fn, void* Component, char Flag);
    void* GuardedLookup(ListLookup_p Fn, void* Owner, bool* Ok);
    bool GuardedUnregister(Unregister_p Fn, void* Registry, void* Node, char Free);
    bool GuardedTaskWork(TaskWork_p Fn, void* Owner, void* Argument, void* Info);

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
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
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
        if (!GuardedFree(s_original_component, Component, Flag))
        {
            const uint64_t Count = s_caught.fetch_add(1);
            if (Count < 40)
            {
                Append(StringFormat("%s  t%lu  FALHA APARADA soltando o MapModelComponent %p; deixo vazar em vez de fechar o jogo%s\n",
                    Clock().c_str(), GetCurrentThreadId(), Component, Stack().c_str()));
            }
        }
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

    // The model component and what it points at, checked before the game
    // touches them. `Why` names the first thing wrong.
    bool ModelHealthy(uintptr_t Component, uintptr_t Fields[3], const char*& Why)
    {
        Fields[0] = Fields[1] = Fields[2] = 0;
        if (Component == 0 || !ObjectOrNull(Component))
        {
            Why = "o componente de modelo nao e um objeto vivo";
            return false;
        }
        // Nothing else is predicted here. A first version also demanded that
        // +0x40, +0xc8, +0xd0 and the vftable embedded at +0x50 all look like
        // live objects, and on 16/09 that threw away **600 updates a boot** of
        // perfectly ordinary map objects: those fields legitimately hold
        // things that are not vftable'd objects. Guessing which field is
        // healthy is how you break the game while trying to save it. What is
        // left is the one thing that cannot be argued with - the component
        // itself - and a fault inside the game is caught below instead.
        Peek(Component + kModelInstance, &Fields[0], sizeof(Fields[0]));
        Peek(Component + kComponentRegistered, &Fields[1], sizeof(Fields[1]));
        Peek(Component + kModelFollower, &Fields[2], sizeof(Fields[2]));
        return true;
    }

    // A fault inside the game is answered by skipping the job, never by
    // closing the game. No C++ objects live in these: __try cannot sit in a
    // function that unwinds.
    bool GuardedModelUpdate(ModelUpdate_p Fn, void* Component, float* Delta)
    {
        __try
        {
            Fn(Component, Delta);
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    bool GuardedPostPhysics(PostPhysics_p Fn, void* Component, void* Argument)
    {
        __try
        {
            Fn(Component, Argument);
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    bool GuardedFree(ComponentFree_p Fn, void* Component, char Flag)
    {
        __try
        {
            Fn(Component, Flag);
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    void* GuardedLookup(ListLookup_p Fn, void* Owner, bool* Ok)
    {
        __try
        {
            void* Result = Fn(Owner);
            *Ok = true;
            return Result;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            *Ok = false;
            return nullptr;
        }
    }

    bool GuardedUnregister(Unregister_p Fn, void* Registry, void* Node, char Free)
    {
        __try
        {
            Fn(Registry, Node, Free);
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    bool GuardedTaskWork(TaskWork_p Fn, void* Owner, void* Argument, void* Info)
    {
        __try
        {
            Fn(Owner, Argument, Info);
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    // The task runner, rebuilt so the work can fail without taking the
    // completion with it. Anything that does not look like a task is handed
    // straight back to the game.
    void TaskRunHook(void** Task, void* Argument)
    {
        uintptr_t Vftable = 0, Work = 0, Owner = 0;
        if (Task == nullptr || !Peek((uintptr_t)Task, &Vftable, sizeof(Vftable)) || !InModule(Vftable) ||
            !Peek((uintptr_t)(Task + 2), &Work, sizeof(Work)) || !InModule(Work) ||
            !Peek((uintptr_t)(Task + 1), &Owner, sizeof(Owner)))
        {
            s_original_task_run(Task, Argument);
            return;
        }
        if (!GuardedTaskWork((TaskWork_p)Work, (void*)Owner, Argument, (void*)(Task + 3)))
        {
            const uint64_t Count = s_caught_task.fetch_add(1);
            if (Count < 60)
            {
                Append(StringFormat("%s  t%lu  FALHA APARADA na tarefa %p (trabalho +0x%zx, dono %p); o quadro dela e pulado e a conclusao segue%s\n",
                    Clock().c_str(), GetCurrentThreadId(), (void*)Task, (size_t)(Work - s_base), (void*)Owner, Stack().c_str()));
            }
        }
        const TaskDone_p Done = (TaskDone_p)((void**)Vftable)[0];
        Done((void*)Task, 0);
    }

    void UnregisterHook(void* Registry, void* Node, char Free)
    {
        if (!GuardedUnregister(s_original_unregister, Registry, Node, Free))
        {
            const uint64_t Count = s_caught.fetch_add(1);
            if (Count < 60)
            {
                Append(StringFormat("%s  t%lu  FALHA APARADA tirando o no %p da lista do quadro; fica registrado em vez de fechar o jogo%s\n",
                    Clock().c_str(), GetCurrentThreadId(), Node, Stack().c_str()));
            }
        }
    }

    // Everything that names the owner of a component, read with care: the
    // entity at +0x08, which for a character is the character itself (the
    // fields after the entity's are only meaningful then).
    std::string DescribeOwner(uintptr_t Component)
    {
        uintptr_t Chr = 0, Vftable = 0, Roles = 0, Physics = 0, Contact = 0, Entity = 0;
        uint8_t Type = 0xff, Role = 0xff;
        float At[3] = {};
        uint32_t Handle = 0, Map = 0xffffffff;
        uint16_t Kind = 0xffff;
        if (!Peek(Component + kComponentEntity, &Chr, sizeof(Chr)) || !PointerShape(Chr))
        {
            return StringFormat("entidade %p ilegivel", (void*)Chr);
        }
        Entity = Chr;
        DescribeEntity(Entity, Map, Kind);
        Peek(Chr, &Vftable, sizeof(Vftable));
        uintptr_t Allocator = 0;
        Peek(Entity + kEntityAllocator, &Allocator, sizeof(Allocator));
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
        return StringFormat("entidade %p (vftable +0x%zx%s, mapa %08x, tipo %u, heap %p; como personagem: tipo %u, papel %u, em (%.2f, %.2f, %.2f), contato %08x, indice de mapa %d)",
            (void*)Entity, InModule(Vftable) ? (size_t)(Vftable - s_base) : (size_t)0,
            Vftable == s_base + kPlayerCtrlVftable ? " PlayerCtrl" : "", Map, (unsigned)Kind, (void*)Allocator, (unsigned)Type, (unsigned)Role,
            At[0], At[1], At[2], Handle, (int)((Handle >> 4) & 0x3f));
    }

    // A component holding a word that **cannot be an address on this machine**:
    // anything with a bit set above bit 47. Every corruption sample of 15 and
    // 16/09 has exactly that shape, the low half of the pointer still right and
    // the top written over (`00b54001410e86d8`, `000b0010e81d77b0`).
    //
    // A first version demanded instead that `+0x50` hold a vftable **of this
    // module**, and that was simply wrong: the field legitimately points
    // outside it, and the very same heap address turns up in dozens of
    // components at once. Forty false positives in one run, 16/09. The bit-47
    // test has no opinion about what a field means; it only says the word is
    // not an address, and a real pointer never trips it.
    //
    // This only **writes down** the address; it changes nothing and skips
    // nothing. Its whole job is to hand a live, already-corrupted address to
    // the page watch of DS2_TraceHook (`wp` in DS2_Trace.req) while the object
    // still exists, because a watch armed after the fact cannot recover the
    // write that already happened. An earlier version of this file tried to
    // *act* on guesses like this one and threw away six hundred good updates a
    // boot; detection and action are kept apart on purpose.
    void NoteIfCorrupt(uintptr_t Component)
    {
        if (Component == 0)
        {
            return;
        }
        static const size_t Fields[] = { kEmbedded, kModelInstance, kComponentRegistered, kModelFollower };
        for (const size_t At : Fields)
        {
            uintptr_t Value = 0;
            if (!Peek(Component + At, &Value, sizeof(Value)) || Value == 0 || (Value >> 47) == 0)
            {
                continue;
            }
            if (Component == s_corrupt_last)
            {
                return;   // the same one every frame says nothing new
            }
            s_corrupt_last = Component;
            if (s_corrupt_seen.fetch_add(1) < 40)
            {
                Append(StringFormat("%s  t%lu  COMPONENTE CORROMPIDO %p campo +0x%zx vale %016llx; pagina %p; %s\n",
                    Clock().c_str(), GetCurrentThreadId(), (void*)Component, At, (unsigned long long)Value,
                    (void*)(Component & ~(uintptr_t)0xfff), DescribeOwner(Component).c_str()));
            }
            return;
        }
    }

    void ModelUpdateHook(void* Component, float* Delta)
    {
        NoteIfCorrupt((uintptr_t)Component);
        PollHeaps();
        uintptr_t Fields[3] = {};
        const char* Why = "";
        if (ModelHealthy((uintptr_t)Component, Fields, Why))
        {
            if (!GuardedModelUpdate(s_original_model_update, Component, Delta))
            {
                const uint64_t Caught = s_caught.fetch_add(1);
                if (Caught < 40)
                {
                    Append(StringFormat("%s  t%lu  FALHA APARADA no pre-desenho do componente %p; pulo o quadro%s\n",
                        Clock().c_str(), GetCurrentThreadId(), Component, Stack().c_str()));
                }
            }
            return;
        }
        // Skipped, and said once a second per component.
        const ULONGLONG Now = GetTickCount64();
        const uint64_t Count = s_skipped_characters.fetch_add(1);
        if (Count < 400 && ((uintptr_t)Component != s_last_skipped_chr || Now - s_last_skipped_at >= 1000))
        {
            s_last_skipped_chr = (uintptr_t)Component;
            s_last_skipped_at = Now;
            Append(StringFormat("%s  t%lu  PRE-DESENHO PULADO: %s; componente %p (+0x40 %p, +0xc8 %p, +0xd0 %p) no %s; %s%s\n",
                Clock().c_str(), GetCurrentThreadId(), Why, Component, (void*)Fields[0], (void*)Fields[1], (void*)Fields[2],
                WhoseHeap((uintptr_t)Component).c_str(), DescribeOwner((uintptr_t)Component).c_str(), Stack().c_str()));
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
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }
    }

    bool OwnerAlive(uintptr_t Owner)
    {
        uintptr_t Vftable = 0;
        return Peek(Owner, &Vftable, sizeof(Vftable)) && Vftable >= s_base &&
            Vftable < s_base + kModuleSpan && (Vftable & 7) == 0;
    }

    // The post-physics task of a component, the twin of the pre-draw: the same
    // component is checked, and a fault is skipped instead of fatal.
    void PostPhysicsHook(void* Component, void* Argument)
    {
        NoteIfCorrupt((uintptr_t)Component);
        uintptr_t Fields[3] = {};
        const char* Why = "";
        if (!ModelHealthy((uintptr_t)Component, Fields, Why))
        {
            const uint64_t Count = s_skipped_physics.fetch_add(1);
            if (Count < 200)
            {
                Append(StringFormat("%s  t%lu  POS-FISICA PULADA: %s; componente %p no %s; %s%s\n",
                    Clock().c_str(), GetCurrentThreadId(), Why, Component, WhoseHeap((uintptr_t)Component).c_str(),
                    DescribeOwner((uintptr_t)Component).c_str(), Stack().c_str()));
                s_lines.fetch_add(1);
            }
            return;
        }
        if (!GuardedPostPhysics(s_original_post_physics, Component, Argument))
        {
            const uint64_t Caught = s_caught.fetch_add(1);
            if (Caught < 40)
            {
                Append(StringFormat("%s  t%lu  FALHA APARADA na pos-fisica do componente %p; pulo o quadro%s\n",
                    Clock().c_str(), GetCurrentThreadId(), Component, Stack().c_str()));
            }
        }
    }

    // A lookup that walks a list of nodes calling a virtual on each: a fault
    // there answers "not found".
    void* ListLookupHook(void* Owner)
    {
        bool Ok = false;
        void* Result = GuardedLookup(s_original_lookup, Owner, &Ok);
        if (!Ok)
        {
            const uint64_t Caught = s_caught.fetch_add(1);
            if (Caught < 40)
            {
                Append(StringFormat("%s  t%lu  FALHA APARADA na busca em lista de %p; respondo nao encontrado%s\n",
                    Clock().c_str(), GetCurrentThreadId(), Owner, Stack().c_str()));
            }
        }
        return Result;
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
    const bool ChrUpdate = Matches(Base + kModelUpdateOffset, kModelUpdatePrologue, sizeof(kModelUpdatePrologue));
    s_original_model_update = ChrUpdate ? (ModelUpdate_p)(Base + kModelUpdateOffset) : nullptr;
    const bool PostPhysics = Matches(Base + kPostPhysicsOffset, kPostPhysicsPrologue, sizeof(kPostPhysicsPrologue));
    s_original_post_physics = PostPhysics ? (PostPhysics_p)(Base + kPostPhysicsOffset) : nullptr;
    const bool ListLookup = Matches(Base + kListLookupOffset, kListLookupPrologue, sizeof(kListLookupPrologue));
    s_original_lookup = ListLookup ? (ListLookup_p)(Base + kListLookupOffset) : nullptr;
    const bool Unregister = Matches(Base + kUnregisterOffset, kUnregisterPrologue, sizeof(kUnregisterPrologue));
    s_original_unregister = Unregister ? (Unregister_p)(Base + kUnregisterOffset) : nullptr;
    const bool TaskRun = Matches(Base + kTaskRunOffset, kTaskRunPrologue, sizeof(kTaskRunPrologue));
    s_original_task_run = TaskRun ? (TaskRun_p)(Base + kTaskRunOffset) : nullptr;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_broadcast, BroadcastHook);
    if (ChrUpdate)
    {
        DetourAttach(&(PVOID&)s_original_model_update, ModelUpdateHook);
    }
    if (PostPhysics)
    {
        DetourAttach(&(PVOID&)s_original_post_physics, PostPhysicsHook);
    }
    if (ListLookup)
    {
        DetourAttach(&(PVOID&)s_original_lookup, ListLookupHook);
    }
    if (Unregister)
    {
        DetourAttach(&(PVOID&)s_original_unregister, UnregisterHook);
    }
    if (TaskRun)
    {
        DetourAttach(&(PVOID&)s_original_task_run, TaskRunHook);
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
    Append(StringFormat("%s  === ds2os vigia da viagem: quem destroi o que depois de chegar; pre-desenho do modelo %s ===\n", Clock().c_str(),
        ChrUpdate ? "guardado" : "sem guarda (codigo inesperado)"));
    Append(StringFormat("%s  === pos-fisica %s, busca em lista %s, listas do quadro refeitas a cada quadro ===\n", Clock().c_str(),
        PostPhysics ? "guardada" : "sem guarda (codigo inesperado)", ListLookup ? "guardada" : "sem guarda (codigo inesperado)"));
    Append(StringFormat("%s  === executor de tarefa %s ===\n", Clock().c_str(),
        TaskRun ? "guardado" : "sem guarda (codigo inesperado)"));
    Append(StringFormat("%s  === saida da lista do quadro %s ===\n", Clock().c_str(),
        Unregister ? "guardada" : "sem guarda (codigo inesperado)"));
    // A version of 16/09 also rebuilt all 32 buckets of the frame's registry
    // every frame, on the theory that something frees a node without taking it
    // out of the list. In more than forty legs **no bucket ever needed
    // rebuilding**: the lists were always whole, so that was not the mechanism,
    // and the sweep is gone. It also wrote through a registry pointer cached
    // from an earlier frame, which is a way to corrupt memory while claiming to
    // protect it.
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
        if (s_original_model_update != nullptr)
        {
            DetourDetach(&(PVOID&)s_original_model_update, ModelUpdateHook);
        }
        if (s_original_post_physics != nullptr)
        {
            DetourDetach(&(PVOID&)s_original_post_physics, PostPhysicsHook);
        }
        if (s_original_lookup != nullptr)
        {
            DetourDetach(&(PVOID&)s_original_lookup, ListLookupHook);
        }
        if (s_original_unregister != nullptr)
        {
            DetourDetach(&(PVOID&)s_original_unregister, UnregisterHook);
        }
        if (s_original_task_run != nullptr)
        {
            DetourDetach(&(PVOID&)s_original_task_run, TaskRunHook);
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
