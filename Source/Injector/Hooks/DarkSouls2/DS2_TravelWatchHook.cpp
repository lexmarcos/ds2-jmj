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

    // FUN_1403f4f10(component, delta): the **other** per-frame job of a
    // MapModelComponent, and the one the guest has actually been closing in.
    // It is 0x43 bytes long and disjoint from the pre-draw above - the PE
    // exception directory gives 0x3f4f10..0x3f4f53 and 0x3f4f60..0x3f527e -
    // and it does two things:
    //
    //     mov  0x40(%rcx),%rcx     the model instance
    //     test %rcx,%rcx
    //     je   +0x3f4f34           the game itself tolerates null here
    //     mov  (%rcx),%rax         <- +0x3f4f2b, where the guest closes
    //     call *0x8(%rax)
    //     mov  0xd8(%rbx),%rcx     and then the second follower, if any
    //     call FUN_1401caaf0
    //
    // Until 16/09 this file recorded that crash as happening in FUN_1403f4f60
    // and counted it as a dependency that **escaped** the pre-draw guard. It
    // never escaped anything: the two are separate functions, nothing in
    // either .text section calls this one directly, and it is reached as a
    // virtual method - its only pointer in the image sits at 0x1410eb588, a
    // slot of the vftable that carries the name "MapModelComponent" right
    // after it at 0x1410eb5c0, and whose slot at 0x1410eb598 is the
    // FUN_1403f6300 release this file already hooks. So the pre-draw's __try
    // was never on the stack while this ran. The guard was in the wrong
    // place, the same mistake this file already made once with FUN_1403152f0.
    //
    // Two neighbouring slots of that table are where this goes next, because
    // whoever owns the model instance has to be found before anything is
    // written: 0x1410eb578 -> FUN_1403f4c20, which reacts to a backread
    // change and calls virtual release/load, and 0x1410eb580 ->
    // FUN_1403f4ce0, which tears a registration down.
    //
    // What this hook does is only what the pre-draw's does: run the job under
    // __try, and on a fault write down everything that names the object at
    // +0x40 while it is still there. It deliberately does **not** write null
    // into +0x40 by analogy with DropDeadRigidBody. That the game null-checks
    // the field here proves this consumer's contract and nothing about the
    // others - FUN_1403f4f60 reads the same field - and for the Havok body
    // the null path was read in the callee first. That reading has not been
    // done here.
    // The entity's component list, which is not the frame registry's. Read in
    // Ghidra on 16/09 from the attach, FUN_14040cca0(entity, components, n):
    // it links each node into `entity+0x18`, follows `node+0x10` to the end,
    // and writes the entity into `node+0x08`. FUN_1403f4ce0 shows where a
    // MapModelComponent keeps that node - it passes `base+0x60` - and
    // FUN_14040cea0 shows the rest of the node: `+0x18` the registry index,
    // `+0x00` its own vftable, whose slot `+0x28` is the teardown.
    //
    // This is the list the 74 copies of GetComponent<T> walk.
    constexpr size_t kNodeInEntityNext = 0x10;    // the next node
    constexpr size_t kEntityComponents = 0x18;    // the list head, inside the entity
    constexpr uint32_t kListGuard = 4096;         // a component list is never this long

    constexpr size_t kModelTickOffset = 0x3f4f10;
    constexpr uint8_t kModelTickPrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0x49, 0x40 };
    constexpr size_t kModelSecond = 0xd8;   // the second follower, read at +0x3f4f34

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
    using ModelTick_p = void(*)(void* Component, float* Delta);
    ModelTick_p s_original_model_tick = nullptr;
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
    std::atomic<uint64_t> s_caught_tick{ 0 };   // faults inside FUN_1403f4f10 alone
    // The detach window and what it did. Kept apart from the guard counters:
    // a guard firing is a failure, a detach is the fix doing its job.
    std::atomic<uint64_t> s_heap_unknown{ 0 };
    std::atomic<uint64_t> s_cut{ 0 };            // rotten links cut out of an entity's list
    std::atomic<uint64_t> s_same_heap{ 0 };      // component and entity in one heap: the ordinary case
    std::atomic<uint64_t> s_cross_heap{ 0 };     // measured: this has never once been anything but zero
    // Which entities were swept and when. Small on purpose: a travel has a
    // handful of entities passing these hooks, not hundreds.
    struct SweptEntity
    {
        uintptr_t Entity = 0;
        ULONGLONG At = 0;
    };
    constexpr int kSweptRing = 32;
    constexpr ULONGLONG kSweepEveryMs = 250;
    SweptEntity s_swept[kSweptRing];   // game thread only, like s_bodies_dropped
    int s_swept_next = 0;
    // The heaps that existed when the travel began, which is before the
    // destination map has one. Without this, "the component is in another heap
    // than its entity" would also be true of a component the character has
    // just bound to the **destination**, and tearing one of those off a live
    // character is a new bug wearing the old one's clothes. What this does not
    // separate is a third map's heap that was already there and is not dying;
    // a component of one of those attached to a travelling character would
    // still be detached. Not solved here, and not seen either.
    std::atomic<uint64_t> s_skipped_tick{ 0 };

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
    bool GuardedModelTick(ModelTick_p Fn, void* Component, float* Delta);
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
    // Which live heap a pointer belongs to, as an index into s_heaps, or -1.
    // The same question WhoseHeap answers in words, for code that has to
    // compare two of them.
    bool HeapIndexOf(uintptr_t At, int& Index)
    {
        Index = -1;
        for (int i = 0; i < s_heap_count; ++i)
        {
            if (At >= s_heaps[i].Low && At < s_heaps[i].High)
            {
                Index = i;
                return true;
            }
        }
        return false;
    }

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

    bool GuardedModelTick(ModelTick_p Fn, void* Component, float* Delta)
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
                // `Task[1]` is the manager that owns the task; the thing the
                // work is **about** is `Task[3]` (FUN_140359e80 writes the
                // character there). That is the address worth pointing a page
                // watch at, so it is printed on its own, with its page.
                uintptr_t Subject = 0;
                Peek((uintptr_t)(Task + 3), &Subject, sizeof(Subject));
                Append(StringFormat("%s  t%lu  FALHA APARADA na tarefa %p (trabalho +0x%zx, dono %p) SUJEITO %p pagina %p; o quadro dela e pulado e a conclusao segue%s\n",
                    Clock().c_str(), GetCurrentThreadId(), (void*)Task, (size_t)(Work - s_base), (void*)Owner,
                    (void*)Subject, (void*)(Subject & ~(uintptr_t)0xfff), Stack().c_str()));
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
        // **Only +0x50**, and only the bit-47 test. Both halves of that matter,
        // and each was learned by getting it wrong on 16/09:
        //
        //  - the field has to be one that is provably a pointer. `+0x50` is:
        //    the game loads it and calls through it (`call *0x18(%rax)`).
        //    `+0x40`, `+0xc8` and `+0xd0` are **not** pointers in many kinds of
        //    component, and watching them reported UTF-16 text from a file path
        //    (`005c003200470053` is "SG2\\"), the float 1.0
        //    (`3f80000000000001`) and a pair of coordinates
        //    (`c1ed107bc204b5c5`) as if they were damage;
        //  - the test has to be "this is not an address", not "this is not a
        //    vftable of my module". `+0x50` legitimately points outside the
        //    module, and demanding otherwise reported forty live components at
        //    once.
        {
            const size_t At = kEmbedded;
            uintptr_t Value = 0;
            if (!Peek(Component + At, &Value, sizeof(Value)) || Value == 0 || (Value >> 47) == 0)
            {
                return;
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

    // The one write the sweep makes, on its own so that __try has no C++
    // object to unwind past - the rule this file states and that the sweep
    // broke on its first build (C2712).
    bool WritePointer(uintptr_t At, uintptr_t Value)
    {
        __try
        {
            *(uintptr_t*)At = Value;
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    // The list of components an entity keeps, swept for a link that is no
    // longer a link.
    //
    // This is where the guest died on 16/09 at +0x17b260, inside a
    // GetComponent<T>: `mov (%rbx),%rdx` with rbx = 00b540ffe83053f0, bit 47
    // set, which no processor will follow. Which of the two things that value
    // is - the `next` field itself written over, or the first qword of a node
    // that was freed and stamped by the allocator, read one instruction
    // earlier - is **not** settled by what was captured, and the same stamp
    // appeared in the host's rigid-body crash where it was the freed block.
    // The sweep does not need to know: a link that is not an address and a
    // node with no vftable of this game are both cut. The first walk that
    // reaches either one dies, and there are 74 copies of that walk.
    //
    // BroadcastHook has done exactly this for the frame's registry since
    // 15/09, and it is the reason those lists stopped killing the game. The
    // entity's list never got the same treatment, which is why the crash
    // moved here instead of stopping.
    //
    // The repair keeps the tail. A freed block is still mapped - the stamp is
    // in its first qword, not a hole in the address space - so the `next` it
    // carries can usually still be read, and the damaged node is spliced out
    // the way the game's own detach splices one: the previous node's `next`,
    // or the entity's own head, is pointed at whatever came after. Only when
    // that tail cannot be read either, or is itself not a node, is the list
    // ended there - and the log says which of the two happened. Cutting the
    // tail off by default would throw away every live component behind the
    // damage, and a character here carries fourteen of them.
    //
    // Nothing else is written, and a list that is whole is left exactly as it
    // was.
    void SweepEntityList(uintptr_t Entity)
    {
        uintptr_t At = 0;
        if (!Peek(Entity + kEntityComponents, &At, sizeof(At)))
        {
            return;
        }
        uintptr_t Previous = 0;     // 0 means the head lives in the entity
        for (uint32_t Guard = 0; Guard < kListGuard; ++Guard)
        {
            if (At == 0)
            {
                return;             // a whole list, ending the way it should
            }
            uintptr_t Vftable = 0;
            const bool Shaped = PointerShape(At) && (At & 7) == 0;
            const bool Reads = Shaped && Peek(At, &Vftable, sizeof(Vftable));
            if (Shaped && Reads && InModule(Vftable) && (Vftable & 7) == 0)
            {
                uintptr_t Next = 0;
                if (!Peek(At + kNodeInEntityNext, &Next, sizeof(Next)))
                {
                    return;
                }
                Previous = At;
                At = Next;
                continue;
            }
            // Past here the link is not something anyone can follow. What
            // comes after it is saved if it can be.
            const uintptr_t Where = Previous == 0 ? Entity + kEntityComponents : Previous + kNodeInEntityNext;
            uintptr_t Tail = 0;
            bool KeptTail = false;
            if (Shaped && Peek(At + kNodeInEntityNext, &Tail, sizeof(Tail)))
            {
                if (Tail == 0)
                {
                    KeptTail = true;    // it really was the last one
                }
                else
                {
                    uintptr_t TailVftable = 0;
                    KeptTail = PointerShape(Tail) && (Tail & 7) == 0 &&
                        Peek(Tail, &TailVftable, sizeof(TailVftable)) && InModule(TailVftable) &&
                        (TailVftable & 7) == 0;
                }
            }
            if (!KeptTail)
            {
                Tail = 0;
            }
            const uint64_t Count = s_cut.fetch_add(1);
            if (Count < 80)
            {
                Append(StringFormat("%s  t%lu  ELO PODRE na lista da entidade %p: %p %s; costuro %p (%s) com %p%s\n",
                    Clock().c_str(), GetCurrentThreadId(), (void*)Entity, (void*)At,
                    !Shaped ? "nao e nem endereco" : (!Reads ? "nao da para ler" : "sem vftable do jogo"),
                    (void*)Where, Previous == 0 ? "a cabeca, na entidade" : "o no anterior", (void*)Tail,
                    KeptTail ? "" : " (a cauda tambem estava perdida: a lista acaba aqui)"));
                s_lines.fetch_add(1);
            }
            WritePointer(Where, Tail);
            // The list moved under the walk, so it is walked again from where
            // it is now; the guard bounds the whole thing either way.
            At = Tail;
            continue;
        }
    }

    // Each entity at most once every kSweepEveryMs, because the same handful
    // of components come past these hooks every frame and walking their
    // entity's whole list each time would be a cost with no new answer.
    void SweepEntitySoon(uintptr_t Entity)
    {
        const ULONGLONG Now = GetTickCount64();
        for (int i = 0; i < kSweptRing; ++i)
        {
            if (s_swept[i].Entity == Entity)
            {
                if (Now - s_swept[i].At < kSweepEveryMs)
                {
                    return;
                }
                s_swept[i].At = Now;
                SweepEntityList(Entity);
                return;
            }
        }
        s_swept[s_swept_next].Entity = Entity;
        s_swept[s_swept_next].At = Now;
        s_swept_next = (s_swept_next + 1) % kSweptRing;
        SweepEntityList(Entity);
    }


    // What a travel does with each component that comes past the per-frame
    // hooks: sweep the list of the entity that owns it, and count where the
    // two live.
    //
    // There used to be a detach here as well. Read in Ghidra on 16/09, the
    // game always detaches before it frees - FUN_14040cea0 calls the
    // component's teardown, takes it out of entity+0x18 and out of the frame
    // registry, and only then hands the block back - so a dead node in a list
    // could only come from memory disappearing without that running, which is
    // what a map part's heap does when it dies whole. From that the rule
    // followed: a component in a different heap from its entity is one about
    // to be freed under it, and the travel cut those loose before letting the
    // origin map go.
    //
    // **The rule was false, and the counters below are what proved it.** Over
    // twelve legs on 16/09 the two instances saw 6017963 and 7035751
    // components, and **every one of them** was in the same heap as its
    // entity. Not one cross-heap link exists to cut. The detach was removed
    // rather than left in: code that cannot act, sitting where a fix belongs,
    // reads as a fix to whoever comes next.
    //
    // The counting stays, because it is what turned "nothing happened" into
    // "the rule does not apply" - a silent no-op and an idle correct fix write
    // the same log line, and that cost a whole cycle to find out.
    void TendComponent(uintptr_t Component)
    {
        // The sweep is **not** gated on WindowOpen(): that also stops at
        // kMaxLines, which is a cap on writing, not on protecting. Measured
        // 17/09: a native travel left a dead node in an entity's list and the
        // host died on it **four minutes later**, at +0x1e5c36, in one of the
        // 74 GetComponent<T> copies, reading `0x3f800000` - the float 1.0 -
        // where a vftable belongs. A window that closes on a log cap would
        // have let exactly that through.
        const ULONGLONG Until = s_open_until.load();
        const bool Travelling = Until != 0 && GetTickCount64() < Until;
        if (Component == 0 || !Travelling)
        {
            return;
        }
        uintptr_t Entity = 0;
        if (!Peek(Component + kComponentEntity, &Entity, sizeof(Entity)) || Entity == 0 || !ObjectOrNull(Entity))
        {
            return;
        }
        SweepEntitySoon(Entity);
        int Mine = -1, Theirs = -1;
        if (!HeapIndexOf(Component, Mine) || !HeapIndexOf(Entity, Theirs))
        {
            s_heap_unknown.fetch_add(1);
            return;
        }
        (Mine == Theirs ? s_same_heap : s_cross_heap).fetch_add(1);
    }

    void ModelUpdateHook(void* Component, float* Delta)
    {
        NoteIfCorrupt((uintptr_t)Component);
        PollHeaps();
        TendComponent((uintptr_t)Component);
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

    // The two objects FUN_1403f4f10 reaches through, described after a fault
    // while they are still there: the value of each field, whether its page
    // can be read at all, and whether what +0x40 points at still carries a
    // vftable of this module. This is the question the page watch needs
    // answered - which object died, and whose heap it came from - and it is
    // the whole reason the guard writes anything.
    std::string DescribeModelInstance(uintptr_t Component)
    {
        uintptr_t Model = 0, Second = 0, Vftable = 0;
        const bool Have = Peek(Component + kModelInstance, &Model, sizeof(Model));
        Peek(Component + kModelSecond, &Second, sizeof(Second));
        const bool Readable = Have && Model != 0 && PointerShape(Model) &&
            Peek(Model, &Vftable, sizeof(Vftable));
        const char* State = !Have ? "o componente nao le" :
            Model == 0 ? "nulo, e o jogo pularia" :
            !PointerShape(Model) ? "nao tem forma de endereco" :
            !Readable ? "pagina ilegivel" : "legivel";
        std::string About = StringFormat("+0x40 %p (%s)", (void*)Model, State);
        if (Readable)
        {
            About += StringFormat(", vftable %016llx%s, %s", (unsigned long long)Vftable,
                InModule(Vftable) ? " do modulo" : " FORA DO MODULO", WhoseHeap(Model).c_str());
        }
        return About + StringFormat("; +0xd8 %p", (void*)Second);
    }

    // The twin of the pre-draw, on the function that actually faults: same
    // health check, same skip, same guard. A fault here is not a fix and not
    // a cure - it is one character losing a frame instead of everyone losing
    // the session, and a line naming the object to arm the page watch on.
    void ModelTickHook(void* Component, float* Delta)
    {
        NoteIfCorrupt((uintptr_t)Component);
        PollHeaps();
        TendComponent((uintptr_t)Component);
        uintptr_t Fields[3] = {};
        const char* Why = "";
        if (!ModelHealthy((uintptr_t)Component, Fields, Why))
        {
            const uint64_t Count = s_skipped_tick.fetch_add(1);
            if (Count < 200)
            {
                Append(StringFormat("%s  t%lu  TIQUE DO MODELO PULADO: %s; componente %p no %s; %s%s\n",
                    Clock().c_str(), GetCurrentThreadId(), Why, Component, WhoseHeap((uintptr_t)Component).c_str(),
                    DescribeOwner((uintptr_t)Component).c_str(), Stack().c_str()));
                s_lines.fetch_add(1);
            }
            return;
        }
        if (GuardedModelTick(s_original_model_tick, Component, Delta))
        {
            return;
        }
        s_caught.fetch_add(1);
        const uint64_t Caught = s_caught_tick.fetch_add(1);
        if (Caught < 40)
        {
            Append(StringFormat("%s  t%lu  FALHA APARADA no tique do modelo (FUN_1403f4f10) do componente %p; %s; o componente no %s; %s%s\n",
                Clock().c_str(), GetCurrentThreadId(), Component, DescribeModelInstance((uintptr_t)Component).c_str(),
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
        PollHeaps();
        TendComponent((uintptr_t)Component);
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
                // ...but a node that was taken out points at **itself**:
                // FUN_14040d2b0 ends with `*link = link`. Re-reading that as
                // the next one walks in place until kNodeGuard runs out,
                // 200000 updates in a single frame. The one captured before
                // the call is the way out, and this matters now that the
                // detach below can remove the node the loop is standing on.
                uintptr_t After = 0;
                Node = OwnerAlive(Owner) && Peek(Node + kNodeNext, &After, sizeof(After)) &&
                    After != 0 && After != Node ? After : Next;
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
    const bool ChrTick = Matches(Base + kModelTickOffset, kModelTickPrologue, sizeof(kModelTickPrologue));
    s_original_model_tick = ChrTick ? (ModelTick_p)(Base + kModelTickOffset) : nullptr;
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
    if (ChrTick)
    {
        DetourAttach(&(PVOID&)s_original_model_tick, ModelTickHook);
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
    Append(StringFormat("%s  === tique do modelo (FUN_1403f4f10, onde o convidado fechava) %s ===\n", Clock().c_str(),
        ChrTick ? "guardado" : "SEM GUARDA (codigo inesperado)"));
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
        if (s_original_model_tick != nullptr)
        {
            DetourDetach(&(PVOID&)s_original_model_tick, ModelTickHook);
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
        // The first Open of a travel - the second one, on landing, only
        // extends the window - starts the sweep's ring empty, so an entity
        // address reused by a new entity cannot suppress a sweep for a whole
        // leg.
        const ULONGLONG Now = GetTickCount64();
        const ULONGLONG Had = s_open_until.load();
        if (Had == 0 || Now >= Had)
        {
            PollHeaps();
            for (int i = 0; i < kSweptRing; ++i)
            {
                s_swept[i] = SweptEntity();
            }
            s_swept_next = 0;
        }
        s_lines.store(0);
        s_open_until.store(Now + Milliseconds);
        Append(StringFormat("%s  --- janela aberta por %u ms: %s ---\n", Clock().c_str(), Milliseconds,
            Why != nullptr ? Why : ""));
        // The running totals, written at both ends of every leg because this
        // is called when a travel starts and again when it lands. Until now
        // these counters only gated log lines - each capped at 40 or 200 - so
        // there was no way to read "did a guard fire on this leg" without
        // trusting a cap that had already been reached. A leg is clean when
        // this line reads the same at both ends of it.
        Append(StringFormat("%s  --- contadores: aparadas %llu (tique %llu), tarefas %llu, pre-desenho pulado %llu, tique pulado %llu, pos-fisica pulada %llu, nos pulados %llu, corrompidos %llu ---\n",
            Clock().c_str(),
            (unsigned long long)s_caught.load(), (unsigned long long)s_caught_tick.load(),
            (unsigned long long)s_caught_task.load(), (unsigned long long)s_skipped_characters.load(),
            (unsigned long long)s_skipped_tick.load(), (unsigned long long)s_skipped_physics.load(),
            (unsigned long long)s_skipped.load(), (unsigned long long)s_corrupt_seen.load()));
        Append(StringFormat("%s  --- componentes vistos: %llu no mesmo heap da entidade, %llu em heap alheio, %llu com heap desconhecido ---\n",
            Clock().c_str(), (unsigned long long)s_same_heap.load(), (unsigned long long)s_cross_heap.load(),
            (unsigned long long)s_heap_unknown.load()));
        Append(StringFormat("%s  --- elos podres cortados de listas de entidade: %llu ---\n", Clock().c_str(),
            (unsigned long long)s_cut.load()));
#else
        (void)Milliseconds;
        (void)Why;
#endif
    }

}
