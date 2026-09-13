/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_DeathInterceptHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#include "ThirdParty/detours/src/detours.h"
#endif

namespace
{
#ifdef _WIN32

    // Version 1.03 Calibrations 2.02, verified before anything is written.
    //
    // Slot +0x20 of ChrDeadActionCtrl, `void(ctrl, float delta)`, once a frame
    // for every character. The only function here that is ever held.
    constexpr size_t kUpdateOffset = 0x13c720;
    constexpr uint8_t kUpdateBytes[] = { 0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0x51, 0x08 };

    // Slot +0x10, `void(ctrl)`. Reads the same byte and fires the same
    // notifications, but never moves the state. Watched only.
    constexpr size_t kReplicaOffset = 0x13c3b0;
    constexpr uint8_t kReplicaBytes[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30 };

    // `void(ctrl, char kind)`, reached by a tail jump from `FUN_14030eb20`, a
    // virtual in five vftables. Kind 1 or 2 fires every consequence of a death
    // in one call and parks the controller in state 3. Watched only.
    constexpr size_t kInstantOffset = 0x13c500;
    constexpr uint8_t kInstantBytes[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x8d, 0x42, 0xff };

    constexpr size_t kContextOffset = 0x16148f0;
    constexpr size_t kLocalCharacter = 0xd0;   // ctx+0xd0, rebuilt on every load

    // ChrDeadActionCtrl
    constexpr size_t kCtrlCharacter = 0x08;
    constexpr size_t kCtrlState = 0x10;        // 0 alive, 1 waiting, 2 dying, 3 done
    constexpr size_t kCtrlNotified = 0x34;     // FUN_14013d430 already ran
    constexpr size_t kCtrlRewarded = 0x38;     // FUN_14013d560 already ran

    // The character
    constexpr size_t kCharacterData = 0xb8;
    constexpr size_t kHp = 0x168;
    constexpr size_t kHpMax = 0x174;           // after hollowing; +0x170 is the base

    // *(chr+0xb8)
    constexpr size_t kFallBits = 0x4c0;
    constexpr size_t kStateBits = 0x4c8;
    constexpr size_t kDeferred = 0x5fc;        // nonzero: the controller does not look
    constexpr size_t kPending = 0x759;
    constexpr size_t kParams = 0x75c;          // through +0x76d
    constexpr size_t kParamsLength = 0x12;

    // What the controller's own tail keeps clear while it sits in state 0.
    constexpr uint64_t kDyingBits = 0x4000 | 0x8000;

    // What a death by falling leaves in +0x4c0, none of it undone by the byte:
    //   bit 9   FUN_140372e20 and the landing damage: this character fell dead
    //   bit 51  FUN_14036fdf0, touching a death volume of the map (zone kinds
    //           1, 2, 5, 6); while it stands, FUN_140372620 calls the fall
    //           death on every frame the character is off the ground
    //   bit 52  the same, for zone kinds 3, 4, 7, 8
    constexpr uint64_t kFallFamily = 0x200 | 0x8000000000000 | 0x10000000000000;

    // The same touch asks the camera for FallDeadCameraOperator: a request of
    // type 7 sets CameraManager+0x450, and the manager's update pushes a type 5
    // request while that byte is set and pops it, by the id in +0x454, once it
    // is not. Nothing clears it short of a reload, and with the camera looking
    // at the character from where it fell, the stick moves it next to nothing
    // - which read as a lock on the controls, and was only the camera.
    constexpr size_t kCameraManager = 0x20;            // ctx+0x20
    constexpr size_t kCameraManagerVftable = 0x10f45a8;
    constexpr size_t kCameraFallWanted = 0x450;        // byte

    // The step-1 teleport (docs/DS2_SEAMLESS_COOP.md): the game's own copies,
    // the velocity tracker, and the Havok body, which is the one that counts.
    constexpr size_t kActions = 0xe0;                  // chr+0xe0, PlayerActionCtrl
    constexpr size_t kActionsFall = 0xb0;              // its fall controller (FUN_140372620)
    constexpr size_t kFallInAir = 0x08;                // byte
    constexpr size_t kFallGrounded = 0x20;             // last position on the ground
    constexpr size_t kMotion = 0xf8;
    constexpr size_t kPhysics = 0x100;
    constexpr size_t kPhysicsProxy = 0x320;            // hkpCharacterRigidBody
    constexpr size_t kProxyBody = 0x20;                // hkpRigidBody
    constexpr size_t kRigidBodyVftable = 0x1126578;
    constexpr float kBodyAboveFeet = 0.05f;

    // The last bonfire, found the way the respawn finds it (step 2).
    constexpr size_t kBonfireRecord = 0x70;            // ctx+0x70
    constexpr size_t kRecordId = 0x16c;
    constexpr size_t kRecordList = 0x58;
    constexpr size_t kListFirst = 0x08;
    constexpr size_t kNodeObject = 0x08;
    constexpr size_t kNodeNext = 0x60;
    constexpr size_t kObjectKind = 0xa2;               // 1 or 5: the short component path
    constexpr size_t kObjectComponents = 0xb8;
    constexpr size_t kComponentsReaction = 0x20;       // MapObjReactionComponent
    constexpr size_t kReactionId = 0xe0;
    constexpr size_t kObjectAxisZ = 0x60;
    constexpr size_t kObjectTranslation = 0x70;
    constexpr float kSpawnBehind = 1.1f;

    // Frames to wait for the fall controller to say the character is down.
    constexpr uint32_t kRecoveryRetryFrames = 30;
    constexpr uint32_t kRecoveryGiveUpFrames = 300;

    // What a death costs, applied with the game's own functions (step 5, all
    // measured on 13/09 against a death the game carried out itself).
    //
    // Souls: the "YOU DIED" sequence reaches FUN_14026af40 through NetSvrManager
    // slot +0xe0. It moves the carried souls into the bloodstain record of
    // NetSvrBloodstainManager (+0x2c has one, +0x30 souls, +0x34 map, position,
    // angle and cell, taken from the last safe position, so a fall leaves it at
    // the edge), zeroes PlayerParam+0xec and marks the record done (+0x2d).
    // After the reload, slot +0x28 (FUN_14026b0d0) clears the mark and puts the
    // bloodstain in the world: a type 10 sign of BloodstainSetCtrl.
    constexpr size_t kRecordSoulsOffset = 0x26af40;
    constexpr uint8_t kRecordSoulsBytes[] = { 0x40, 0x53, 0x57, 0x48, 0x83, 0xec, 0x58 };
    constexpr size_t kSpawnBloodstainOffset = 0x26b0d0;
    constexpr uint8_t kSpawnBloodstainBytes[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x80, 0x79, 0x2c, 0x00 };
    // Hollowing: FUN_14037dcc0, on the frame the controller enters state 2,
    // calls FUN_140202c30(PlayerParam, *(data+0x76d)), which adds a param row's
    // delta to the hollow level at PlayerParam+0x1ac - unless FUN_14031c850 or
    // the flags below say this death carries no penalty.
    constexpr size_t kHollowOffset = 0x202c30;
    constexpr uint8_t kHollowBytes[] = { 0x48, 0x85, 0xc9, 0x74, 0x5f, 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57 };
    constexpr size_t kNoPenaltyOffset = 0x31c850;
    constexpr uint8_t kNoPenaltyBytes[] = { 0x48, 0x83, 0xec, 0x28, 0x4c, 0x8b, 0xd9, 0x45, 0x33, 0xd2 };
    constexpr size_t kNotAPlayerOffset = 0x16f740;
    constexpr uint8_t kNotAPlayerBytes[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0xd1 };
    // Estus: the rest at a bonfire refills it through FUN_1401ac370(inventory).
    constexpr size_t kRefillEstusOffset = 0x1ac370;
    constexpr uint8_t kRefillEstusBytes[] = { 0x48, 0x8b, 0x49, 0x10, 0xe9, 0xe7, 0xae, 0xff, 0xff };

    constexpr size_t kPlayerParam = 0x490;             // chr+0x490
    constexpr size_t kParamSouls = 0xec;
    constexpr size_t kParamHollow = 0x1ac;             // byte, 0 to 32
    constexpr size_t kDeathKind = 0x76d;               // *(chr+0xb8), byte
    constexpr uint64_t kNoPenaltyBit = 0x80000000000000;       // +0x4c8
    constexpr uint64_t kNoHollowBit = 0x400000000000000;       // +0x4b8
    constexpr size_t kEffectBits = 0x4b8;

    constexpr size_t kInventoryHolder = 0xa8;          // ctx+0xa8
    constexpr size_t kHolderInventory = 0x10;

    constexpr size_t kNetGlobalOffset = 0x1616cf8;     // *(global)+0x30 is NetSvrManager
    constexpr size_t kNetManager = 0x30;
    constexpr size_t kNetBloodstains = 0x90;
    constexpr size_t kBloodstainManagerVftable = 0x10d21c8;
    constexpr size_t kBloodstainDone = 0x2d;

    constexpr size_t kSignsHolder = 0x90;              // ctx+0x90
    constexpr size_t kSignsHolderInner = 0x68;
    constexpr size_t kSignsSetCtrl = 0x28;             // BloodstainSetCtrl
    constexpr size_t kSetCtrlVftable = 0x10caed8;
    constexpr size_t kSetCtrlInterface = 0x28;         // IBloodstainSetCtrl
    constexpr size_t kInterfaceVftable = 0x10caf28;
    constexpr size_t kInterfaceRemove = 0x20;          // void(iface, entry*)
    constexpr size_t kSetCount = 0x18;                 // uint32(set)
    constexpr size_t kSetEntry = 0x10;                 // entry*(set, index)
    constexpr size_t kSignSets[] = { 0x18, 0x20 };
    constexpr uint32_t kSoulsBloodstainType = 10;

    enum Mode : int
    {
        Observe = 0,
        Cancel = 1,
        Respawn = 2,
    };

    using RecordSouls_p = void(*)(void* Manager, uint64_t* Out);
    using SpawnBloodstain_p = uint8_t(*)(void* Manager);
    using Hollow_p = void(*)(void* PlayerParam, int Kind);
    using Check_p = uint64_t(*)(void* Object);
    using RefillEstus_p = void(*)(void* Inventory);
    using SetCount_p = uint32_t(*)(void* Set);
    using SetEntry_p = uint32_t*(*)(void* Set, uint32_t Index);
    using RemoveSign_p = void(*)(void* Interface, uint32_t* Entry);

    RecordSouls_p s_record_souls = nullptr;
    SpawnBloodstain_p s_spawn_bloodstain = nullptr;
    Hollow_p s_hollow = nullptr;
    Check_p s_no_penalty = nullptr;
    Check_p s_not_a_player = nullptr;
    RefillEstus_p s_refill_estus = nullptr;

    using Update_p = void(*)(void* Ctrl, float Delta);
    using Replica_p = void(*)(void* Ctrl);
    using Instant_p = void(*)(void* Ctrl, char Kind);

    Update_p s_original_update = nullptr;
    Replica_p s_original_replica = nullptr;
    Instant_p s_original_instant = nullptr;

    uintptr_t s_base = 0;

    std::atomic<int> s_mode{ Observe };
    std::atomic<uint64_t> s_seen{ 0 };
    std::atomic<uint64_t> s_cancelled{ 0 };
    std::atomic<uint64_t> s_unexplained{ 0 };
    std::atomic<uint64_t> s_instant{ 0 };
    std::atomic<uint64_t> s_replica_calls{ 0 };
    std::atomic<uint64_t> s_recovered{ 0 };
    std::atomic<uint64_t> s_recovery_failed{ 0 };
    std::atomic<uint64_t> s_respawns{ 0 };

    // Touched only from the game's thread, inside the detours.
    void* s_local_ctrl = nullptr;
    uint8_t s_local_state = 0xff;
    uint64_t s_streak = 0;
    ULONGLONG s_last_cancel_ms = 0;

    struct Recovery
    {
        bool Active = false;
        uint32_t Frames = 0;
        float Target[3] = {};
        const char* Where = "";
        const char* Why = "";
    };
    Recovery s_recovery;

    struct Watched
    {
        void* Ctrl;
        uint32_t Signature;
    };
    Watched s_replicas[16] = {};

    std::atomic<bool> s_running{ false };
    std::thread s_thread;

    std::mutex s_log_mutex;
    std::filesystem::path s_log_path;
    std::filesystem::path s_request_path;

    void Append(const std::string& Text)
    {
        std::scoped_lock Lock(s_log_mutex);
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream)
        {
            Stream << Text;
        }
    }

    // Wall clock, so a line can be set beside the server's log.
    std::string Clock()
    {
        SYSTEMTIME Now;
        GetLocalTime(&Now);
        return StringFormat("%02u:%02u:%02u.%03u", Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds);
    }

    std::string Hex(const uint8_t* Bytes, size_t Length)
    {
        std::string Out;
        for (size_t i = 0; i < Length; ++i)
        {
            Out += StringFormat("%02x", Bytes[i]);
        }
        return Out;
    }

    void* LocalCharacter()
    {
        const uintptr_t Context = *(const uintptr_t*)(s_base + kContextOffset);
        return Context == 0 ? nullptr : *(void**)(Context + kLocalCharacter);
    }

    // Guarded access for everything past the character itself: map objects,
    // the camera and the Havok body can all be gone mid-load. Functions of
    // their own, because MSVC refuses __try where objects need unwinding.
    bool ReadBytes(uintptr_t Address, void* Out, size_t Length)
    {
        __try
        {
            memcpy(Out, (const void*)Address, Length);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool WriteBytes(uintptr_t Address, const void* In, size_t Length)
    {
        __try
        {
            memcpy((void*)Address, In, Length);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadPointer(uintptr_t Address, uintptr_t& Out)
    {
        Out = 0;
        return ReadBytes(Address, &Out, sizeof(Out)) && Out != 0;
    }

    uintptr_t CameraManager()
    {
        uintptr_t Context = 0, Manager = 0, Vftable = 0;
        if (!ReadPointer(s_base + kContextOffset, Context) ||
            !ReadPointer(Context + kCameraManager, Manager) ||
            !ReadPointer(Manager, Vftable) || Vftable != s_base + kCameraManagerVftable)
        {
            return 0;
        }
        return Manager;
    }

    bool CameraWantsFallDead()
    {
        const uintptr_t Manager = CameraManager();
        uint8_t Wanted = 0;
        return Manager != 0 && ReadBytes(Manager + kCameraFallWanted, &Wanted, 1) && Wanted != 0;
    }

    uintptr_t FallController(uint8_t* Chr)
    {
        uintptr_t Actions = 0, Fall = 0;
        return ReadPointer((uintptr_t)Chr + kActions, Actions) && ReadPointer(Actions + kActionsFall, Fall) ? Fall : 0;
    }

    // The spawn point of the bonfire in the respawn record, if that bonfire is
    // in the loaded map: translation - 1.1 * Z axis of its map object, which is
    // where the game itself put the character (0.000 m, measured on 13/09).
    bool FindBonfireSpawn(float Out[3], uint32_t& Id)
    {
        uintptr_t Context = 0, Record = 0, List = 0, Node = 0;
        Id = 0;
        if (!ReadPointer(s_base + kContextOffset, Context) ||
            !ReadPointer(Context + kBonfireRecord, Record) ||
            !ReadBytes(Record + kRecordId, &Id, sizeof(Id)) ||
            !ReadPointer(Record + kRecordList, List) ||
            !ReadPointer(List + kListFirst, Node))
        {
            return false;
        }

        for (int i = 0; i < 256 && Node != 0; ++i)
        {
            uintptr_t Object = 0, Components = 0, Reaction = 0, IdAt = 0;
            uint8_t Kind = 0;
            uint32_t NodeId = 0;
            if (ReadPointer(Node + kNodeObject, Object) &&
                ReadBytes(Object + kObjectKind, &Kind, 1) && (Kind == 1 || Kind == 5) &&
                ReadPointer(Object + kObjectComponents, Components) &&
                ReadPointer(Components + kComponentsReaction, Reaction) &&
                ReadPointer(Reaction + kReactionId, IdAt) &&
                ReadBytes(IdAt, &NodeId, sizeof(NodeId)) && NodeId == Id)
            {
                float Axis[4] = {}, Translation[4] = {};
                if (!ReadBytes(Object + kObjectAxisZ, Axis, sizeof(Axis)) ||
                    !ReadBytes(Object + kObjectTranslation, Translation, sizeof(Translation)))
                {
                    return false;
                }
                for (int k = 0; k < 3; ++k)
                {
                    Out[k] = Translation[k] - kSpawnBehind * Axis[k];
                }
                return true;
            }

            uintptr_t Next = 0;
            if (!ReadBytes(Node + kNodeNext, &Next, sizeof(Next)))
            {
                break;
            }
            Node = Next;
        }
        return false;
    }

    // XYZ only, every w left alone; the order is the one that worked by hand.
    bool TeleportLocal(uint8_t* Chr, const float Target[3])
    {
        uintptr_t Motion = 0, Physics = 0, Proxy = 0, Body = 0, Vftable = 0;
        if (!ReadPointer((uintptr_t)Chr + kMotion, Motion) ||
            !ReadPointer((uintptr_t)Chr + kPhysics, Physics) ||
            !ReadPointer(Physics + kPhysicsProxy, Proxy) ||
            !ReadPointer(Proxy + kProxyBody, Body) ||
            !ReadPointer(Body, Vftable) || Vftable != s_base + kRigidBodyVftable)
        {
            return false;
        }

        const float Feet[3] = { Target[0], Target[1], Target[2] };
        const float Centre[3] = { Target[0], Target[1] + kBodyAboveFeet, Target[2] };
        const uint8_t Still[16] = {};
        return WriteBytes((uintptr_t)Chr + 0x90, Feet, sizeof(Feet)) &&
            WriteBytes((uintptr_t)Chr + 0xa0, Feet, sizeof(Feet)) &&
            WriteBytes(Physics + 0x80, Feet, sizeof(Feet)) &&
            WriteBytes(Motion + 0x50, Feet, sizeof(Feet)) &&
            WriteBytes(Physics + 0x60, Still, sizeof(Still)) &&
            WriteBytes(Physics + 0x70, Still, sizeof(Still)) &&
            WriteBytes(Body + 0x250, Centre, sizeof(Centre)) &&
            WriteBytes(Body + 0x260, Centre, sizeof(Centre)) &&
            WriteBytes(Body + 0x1b0, Centre, sizeof(Centre)) &&
            WriteBytes(Body + 0x1c0, Centre, sizeof(Centre)) &&
            WriteBytes(Body + 0x1a0, Centre, sizeof(Centre)) &&
            WriteBytes(Physics + 0x1c0, Centre, sizeof(Centre));
    }

    // Calls into the game, each behind its own __try so a fault inside comes
    // back as false instead of taking the process. Functions of their own for
    // the same reason as ReadBytes.
    bool CallRecordSouls(uintptr_t Manager, uint64_t& Out)
    {
        __try
        {
            s_record_souls((void*)Manager, &Out);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallSpawnBloodstain(uintptr_t Manager)
    {
        __try
        {
            s_spawn_bloodstain((void*)Manager);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallHollow(uintptr_t PlayerParam, int Kind)
    {
        __try
        {
            s_hollow((void*)PlayerParam, Kind);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallCheck(Check_p Check, uintptr_t Object, bool& Result)
    {
        __try
        {
            Result = (Check((void*)Object) & 0xff) != 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallRefillEstus(uintptr_t Inventory)
    {
        __try
        {
            s_refill_estus((void*)Inventory);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallSetCount(uintptr_t Set, uint32_t& Count)
    {
        __try
        {
            const uintptr_t Vftable = *(const uintptr_t*)Set;
            Count = (*(SetCount_p*)(Vftable + kSetCount))((void*)Set);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallSetEntry(uintptr_t Set, uint32_t Index, uint32_t*& Entry)
    {
        __try
        {
            const uintptr_t Vftable = *(const uintptr_t*)Set;
            Entry = (*(SetEntry_p*)(Vftable + kSetEntry))((void*)Set, Index);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallRemoveSign(uintptr_t Interface, uint32_t* Entry)
    {
        __try
        {
            const uintptr_t Vftable = *(const uintptr_t*)Interface;
            (*(RemoveSign_p*)(Vftable + kInterfaceRemove))((void*)Interface, Entry);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    uintptr_t BloodstainManager()
    {
        uintptr_t Global = 0, Net = 0, Manager = 0, Vftable = 0;
        if (!ReadPointer(s_base + kNetGlobalOffset, Global) ||
            !ReadPointer(Global + kNetManager, Net) ||
            !ReadPointer(Net + kNetBloodstains, Manager) ||
            !ReadPointer(Manager, Vftable) || Vftable != s_base + kBloodstainManagerVftable)
        {
            return 0;
        }
        return Manager;
    }

    uintptr_t BloodstainSetCtrl()
    {
        uintptr_t Context = 0, Holder = 0, Inner = 0, SetCtrl = 0, Vftable = 0, InterfaceVftable = 0;
        if (!ReadPointer(s_base + kContextOffset, Context) ||
            !ReadPointer(Context + kSignsHolder, Holder) ||
            !ReadPointer(Holder + kSignsHolderInner, Inner) ||
            !ReadPointer(Inner + kSignsSetCtrl, SetCtrl) ||
            !ReadPointer(SetCtrl, Vftable) || Vftable != s_base + kSetCtrlVftable ||
            !ReadPointer(SetCtrl + kSetCtrlInterface, InterfaceVftable) ||
            InterfaceVftable != s_base + kInterfaceVftable)
        {
            return 0;
        }
        return SetCtrl;
    }

    // Without a reload nothing clears the bloodstain of the previous death, and
    // FUN_14020e4e0 only evicts when the set is full; so the old souls
    // bloodstain goes first, the way the reload would have taken it.
    int RemoveSoulsBloodstains(uintptr_t SetCtrl)
    {
        uint32_t* Found[16] = {};
        size_t Count = 0;
        for (size_t Offset : kSignSets)
        {
            uintptr_t Set = 0;
            uint32_t Size = 0;
            if (!ReadPointer(SetCtrl + Offset, Set) || !CallSetCount(Set, Size))
            {
                continue;
            }
            for (uint32_t i = 0; i < Size && i < 4096 && Count < 16; ++i)
            {
                uint32_t* Entry = nullptr;
                uint32_t Handle = 0, Flags = 0;
                if (CallSetEntry(Set, i, Entry) && Entry != nullptr &&
                    ReadBytes((uintptr_t)Entry, &Handle, sizeof(Handle)) &&
                    ReadBytes((uintptr_t)Entry + 0x14, &Flags, sizeof(Flags)) &&
                    (int32_t)Flags < 0 && (Handle & 0xf) == kSoulsBloodstainType)
                {
                    Found[Count++] = Entry;
                }
            }
        }

        int Removed = 0;
        for (size_t i = 0; i < Count; ++i)
        {
            if (CallRemoveSign(SetCtrl + kSetCtrlInterface, Found[i]))
            {
                ++Removed;
            }
        }
        return Removed;
    }

    int CountSoulsBloodstains(uintptr_t SetCtrl)
    {
        int Count = 0;
        for (size_t Offset : kSignSets)
        {
            uintptr_t Set = 0;
            uint32_t Size = 0;
            if (!ReadPointer(SetCtrl + Offset, Set) || !CallSetCount(Set, Size))
            {
                continue;
            }
            for (uint32_t i = 0; i < Size && i < 4096; ++i)
            {
                uint32_t* Entry = nullptr;
                uint32_t Handle = 0, Flags = 0;
                if (CallSetEntry(Set, i, Entry) && Entry != nullptr &&
                    ReadBytes((uintptr_t)Entry, &Handle, sizeof(Handle)) &&
                    ReadBytes((uintptr_t)Entry + 0x14, &Flags, sizeof(Flags)) &&
                    (int32_t)Flags < 0 && (Handle & 0xf) == kSoulsBloodstainType)
                {
                    ++Count;
                }
            }
        }
        return Count;
    }

    // Everything a death costs except the reload, in the order the game pays
    // it: souls into the bloodstain record while the character still stands
    // where it died (the record takes the last safe position), hollowing, the
    // bloodstain put in the world, and the Estus the respawn refills.
    void ApplyDeathCosts(uint8_t* Chr, uint8_t* Data)
    {
        const uintptr_t Player = (uintptr_t)Chr;
        uintptr_t Param = 0;
        const bool HaveParam = ReadPointer(Player + kPlayerParam, Param);

        uint32_t SoulsBefore = 0;
        uint8_t HollowBefore = 0;
        if (HaveParam)
        {
            ReadBytes(Param + kParamSouls, &SoulsBefore, sizeof(SoulsBefore));
            ReadBytes(Param + kParamHollow, &HollowBefore, 1);
        }

        // Souls first.
        std::string Souls = "sem gerenciador de manchas";
        const uintptr_t Manager = BloodstainManager();
        if (Manager != 0)
        {
            uint8_t Done = 1;
            ReadBytes(Manager + kBloodstainDone, &Done, 1);
            if (Done != 0)
            {
                Souls = "registro ja marcado nesta carga; almas ficam";
            }
            else
            {
                uint64_t Out = 0;
                const bool Called = CallRecordSouls(Manager, Out);
                Souls = StringFormat("%s: %u almas para a mancha, %u perdidas da anterior",
                    Called ? "registradas" : "FALHOU", (uint32_t)(Out >> 32), (uint32_t)Out);
            }
        }

        // Hollowing, with the checks FUN_14037dcc0 makes before it.
        std::string Hollow = "sem PlayerParam";
        if (HaveParam)
        {
            bool Exempt = false, NotPlayer = false;
            uint64_t StateBits = 0, EffectBits = 0;
            ReadBytes((uintptr_t)Data + kStateBits, &StateBits, sizeof(StateBits));
            ReadBytes((uintptr_t)Data + kEffectBits, &EffectBits, sizeof(EffectBits));
            const bool Checked = CallCheck(s_no_penalty, (uintptr_t)Data, Exempt) &&
                CallCheck(s_not_a_player, Player, NotPlayer);
            if (!Checked)
            {
                Hollow = "checagens FALHARAM; sem hollow";
            }
            else if (Exempt || (StateBits & kNoPenaltyBit) != 0 || NotPlayer || (EffectBits & kNoHollowBit) != 0)
            {
                Hollow = StringFormat("isento (penalidade=%d especial=%d bits=%d/%d)",
                    Exempt ? 1 : 0, NotPlayer ? 1 : 0, (StateBits & kNoPenaltyBit) != 0 ? 1 : 0,
                    (EffectBits & kNoHollowBit) != 0 ? 1 : 0);
            }
            else
            {
                const int Kind = (int)(int8_t)Data[kDeathKind];
                const bool Called = CallHollow(Param, Kind);
                uint8_t HollowAfter = HollowBefore;
                ReadBytes(Param + kParamHollow, &HollowAfter, 1);
                Hollow = StringFormat("%s: nivel %u -> %u", Called ? "aplicado" : "FALHOU", HollowBefore, HollowAfter);
            }
        }

        // The old bloodstain out, the new one in.
        std::string Bloodstain = "sem BloodstainSetCtrl";
        const uintptr_t SetCtrl = BloodstainSetCtrl();
        if (Manager != 0 && SetCtrl != 0)
        {
            const int Removed = RemoveSoulsBloodstains(SetCtrl);
            const bool Spawned = CallSpawnBloodstain(Manager);
            Bloodstain = StringFormat("%d antiga(s) removida(s), nova %s, agora %d no mundo",
                Removed, Spawned ? "criada" : "FALHOU", CountSoulsBloodstains(SetCtrl));
        }

        // Estus, as the respawn at a bonfire refills it.
        std::string Estus = "sem inventario";
        uintptr_t Context = 0, Holder = 0, Inventory = 0;
        if (ReadPointer(s_base + kContextOffset, Context) &&
            ReadPointer(Context + kInventoryHolder, Holder) &&
            ReadPointer(Holder + kHolderInventory, Inventory))
        {
            Estus = CallRefillEstus(Inventory) ? "recarregado" : "FALHOU";
        }

        uint32_t SoulsAfter = SoulsBefore;
        if (HaveParam)
        {
            ReadBytes(Param + kParamSouls, &SoulsAfter, sizeof(SoulsAfter));
        }
        Append(StringFormat("%s  custos da morte: almas %u -> %u (%s); hollow %s; manchas: %s; estus %s\n",
            Clock().c_str(), SoulsBefore, SoulsAfter, Souls.c_str(), Hollow.c_str(), Bloodstain.c_str(), Estus.c_str()));
    }

    // A fall that was refused still leaves the character in the air, in a
    // death volume, with the fall camera. Out of the air first; the flags only
    // once the fall controller agrees the character is down, or the next frame
    // in the air is another fall death.
    void StartRecovery(uint8_t* Chr, const char* Why)
    {
        Recovery Next;
        Next.Active = true;
        Next.Why = Why;
        uint32_t Id = 0;
        if (FindBonfireSpawn(Next.Target, Id))
        {
            Next.Where = "fogueira do registro";
        }
        else
        {
            const uintptr_t Fall = FallController(Chr);
            if (Fall == 0 || !ReadBytes(Fall + kFallGrounded, Next.Target, sizeof(Next.Target)))
            {
                ++s_recovery_failed;
                Append(StringFormat("%s  %s: sem fogueira %08x no mapa e sem a ultima posicao no chao; nada a fazer\n",
                    Clock().c_str(), Why, Id));
                return;
            }
            Next.Where = "ultima posicao no chao";
        }

        s_recovery = Next;
        const bool Moved = TeleportLocal(Chr, s_recovery.Target);
        Append(StringFormat("%s  %s: levando para %s (%.3f, %.3f, %.3f) id=%08x %s\n",
            Clock().c_str(), Why, s_recovery.Where, s_recovery.Target[0], s_recovery.Target[1], s_recovery.Target[2],
            Id, Moved ? "teleportado" : "TELEPORTE FALHOU"));
    }

    void ContinueRecovery(uint8_t* Chr, uint8_t* Data)
    {
        ++s_recovery.Frames;

        const uintptr_t Fall = FallController(Chr);
        uint8_t InAir = 1;
        const bool Down = Fall != 0 && ReadBytes(Fall + kFallInAir, &InAir, 1) && InAir == 0;
        if (!Down)
        {
            if (s_recovery.Frames >= kRecoveryGiveUpFrames)
            {
                ++s_recovery_failed;
                s_recovery.Active = false;
                Append(StringFormat("%s  %s: %u quadros e o personagem nao pousou; desisto\n",
                    Clock().c_str(), s_recovery.Why, s_recovery.Frames));
            }
            else if (s_recovery.Frames % kRecoveryRetryFrames == 0)
            {
                TeleportLocal(Chr, s_recovery.Target);
            }
            return;
        }

        uint64_t Bits = 0;
        const bool HadBits = ReadBytes((uintptr_t)Data + kFallBits, &Bits, sizeof(Bits));
        const uint64_t Before = Bits;
        Bits &= ~kFallFamily;
        if (HadBits && Bits != Before)
        {
            WriteBytes((uintptr_t)Data + kFallBits, &Bits, sizeof(Bits));
        }

        // Clearing the byte is the whole job: the manager pops its own request.
        bool Camera = false;
        if (const uintptr_t Manager = CameraManager())
        {
            uint8_t Wanted = 0;
            if (ReadBytes(Manager + kCameraFallWanted, &Wanted, 1) && Wanted != 0)
            {
                const uint8_t Clear = 0;
                Camera = WriteBytes(Manager + kCameraFallWanted, &Clear, 1);
            }
        }

        // Last, so the maximum already carries this death's hollowing.
        int32_t Hp = 0, Max = 0;
        if (ReadBytes((uintptr_t)Chr + kHpMax, &Max, sizeof(Max)) && Max > 0)
        {
            ReadBytes((uintptr_t)Chr + kHp, &Hp, sizeof(Hp));
            WriteBytes((uintptr_t)Chr + kHp, &Max, sizeof(Max));
        }

        ++s_recovered;
        s_recovery.Active = false;
        Append(StringFormat("%s  %s concluido em %u quadros: +0x4c0 %016llx -> %016llx, camera de queda %s, hp %d -> %d\n",
            Clock().c_str(), s_recovery.Why, s_recovery.Frames, (unsigned long long)Before, (unsigned long long)Bits,
            Camera ? "desligada" : "nao estava ligada", Hp, Max));
    }

    // The parameters the controller would have copied: who killed (a handle
    // FUN_14017b4f0 resolves), the flags that suppress single consequences,
    // and the cause that picks the timing row (10 for HP).
    // Cause 90 comes with bit 0x200 of +0x4c0, and that death leaves more
    // behind than the byte.
    std::string DescribeParams(const uint8_t* Data)
    {
        const uint8_t* P = Data + kParams;
        return StringFormat("matador=%08x flags=%08x causa=%u +0x4c0=%016llx bruto=%s",
            *(const uint32_t*)P,
            *(const uint32_t*)(P + 0x04),
            *(const uint32_t*)(P + 0x0c),
            (unsigned long long)*(const uint64_t*)(Data + kFallBits),
            Hex(P, kParamsLength).c_str());
    }

    void UpdateHook(void* Ctrl, float Delta)
    {
        uint8_t* Bytes = (uint8_t*)Ctrl;
        void* Character = *(void**)(Bytes + kCtrlCharacter);
        if (Character == nullptr || Character != LocalCharacter())
        {
            s_original_update(Ctrl, Delta);
            return;
        }

        uint8_t* Chr = (uint8_t*)Character;
        uint8_t* Data = *(uint8_t**)(Chr + kCharacterData);
        const uint8_t Before = Bytes[kCtrlState];

        if (Ctrl != s_local_ctrl)
        {
            s_local_ctrl = Ctrl;
            s_local_state = Before;
            s_recovery.Active = false;
            Append(StringFormat("%s  controlador do jogador local %p, personagem %p, estado %u\n",
                Clock().c_str(), Ctrl, Character, Before));
        }

        if (s_recovery.Active && Data != nullptr)
        {
            ContinueRecovery(Chr, Data);
        }

        // The same three tests the controller makes, in its order. The HP
        // source runs earlier in the frame, so the byte is already there.
        const bool Pending = Before == 0 && Data != nullptr
            && *(const int32_t*)(Data + kDeferred) == 0
            && Data[kPending] != 0;

        if (Pending)
        {
            const int32_t Hp = *(const int32_t*)(Chr + kHp);
            const int32_t Max = *(const int32_t*)(Chr + kHpMax);
            const std::string Params = DescribeParams(Data);

            const int Mode = s_mode.load();
            if (Mode == Cancel || Mode == Respawn)
            {
                // Read before anything is cleared: a fall leaves its marks in
                // +0x4c0 and in the camera, not in the parameters.
                const bool Fell = (*(const uint64_t*)(Data + kFallBits) & kFallFamily) != 0 || CameraWantsFallDead();

                // One death, one bill. While a recovery runs, a byte that comes
                // back is the same death still being held: a fall zeroes the HP
                // on every frame the character is in the air.
                const bool NewDeath = !s_recovery.Active;
                if (Mode == Respawn && NewDeath)
                {
                    ApplyDeathCosts(Chr, Data);
                }

                // Never leave the byte set: with it cleared and the HP back,
                // the source has nothing to say next frame.
                Data[kPending] = 0;
                if (Max > 0)
                {
                    *(int32_t*)(Chr + kHp) = Max;
                }
                *(uint64_t*)(Data + kStateBits) &= ~kDyingBits;

                // A cancel on every frame means something keeps killing and
                // the HP did not hold; say so without filling the disk. The
                // limit is per run of consecutive cancels, so the first cancel
                // after a long one is still written down.
                const uint64_t Count = ++s_cancelled;
                const ULONGLONG Now = GetTickCount64();
                if (Now - s_last_cancel_ms > 1000)
                {
                    s_streak = 0;
                }
                s_last_cancel_ms = Now;
                const uint64_t InStreak = ++s_streak;
                if (InStreak <= 20 || InStreak % 300 == 0)
                {
                    Append(StringFormat("%s  morte CANCELADA #%llu (seguida %llu)%s hp=%d -> %d %s\n",
                        Clock().c_str(), (unsigned long long)Count, (unsigned long long)InStreak,
                        Fell ? " queda" : "", Hp, Max, Params.c_str()));
                }

                if (NewDeath && (Mode == Respawn || Fell))
                {
                    if (Mode == Respawn)
                    {
                        ++s_respawns;
                    }
                    StartRecovery(Chr, Mode == Respawn ? "renascer" : "queda");
                }
                return;
            }

            ++s_seen;
            Append(StringFormat("%s  morte vista hp=%d max=%d %s\n",
                Clock().c_str(), Hp, Max, Params.c_str()));
        }

        s_original_update(Ctrl, Delta);

        const uint8_t After = Bytes[kCtrlState];
        if (After != s_local_state)
        {
            // Leaving state 0 with no byte beforehand means FUN_14013cc30 set
            // it inside the call - character flags or animation event 0x19 -
            // and that death went past the check above.
            const bool Unexplained = Before == 0 && After != 0 && !Pending;
            if (Unexplained)
            {
                ++s_unexplained;
            }
            Append(StringFormat("%s  estado do controlador %u -> %u%s\n",
                Clock().c_str(), s_local_state, After,
                Unexplained ? "  SEM +0x759 ANTES: veio de FUN_14013cc30 e nao foi interceptada" : ""));
            s_local_state = After;
        }
    }

    void ReplicaHook(void* Ctrl)
    {
        ++s_replica_calls;
        if (Ctrl != nullptr)
        {
            const uint8_t* Bytes = (const uint8_t*)Ctrl;
            const void* Character = *(void**)(Bytes + kCtrlCharacter);

            // Dereference only where the original does.
            uint8_t Pending = 0;
            if (Character != nullptr && Bytes[kCtrlState] == 0)
            {
                const uint8_t* Data = *(const uint8_t**)((const uint8_t*)Character + kCharacterData);
                Pending = Data == nullptr ? 0 : Data[kPending];
            }

            const uint32_t Signature = (uint32_t)Bytes[kCtrlState]
                | ((uint32_t)Pending << 8)
                | ((uint32_t)Bytes[kCtrlNotified] << 16)
                | ((uint32_t)Bytes[kCtrlRewarded] << 24);

            Watched* Slot = nullptr;
            bool Known = false;
            for (Watched& Entry : s_replicas)
            {
                if (Entry.Ctrl == Ctrl)
                {
                    Slot = &Entry;
                    Known = true;
                    break;
                }
                if (Slot == nullptr && Entry.Ctrl == nullptr)
                {
                    Slot = &Entry;
                }
            }

            if (Slot != nullptr && (!Known || Slot->Signature != Signature))
            {
                Slot->Ctrl = Ctrl;
                Slot->Signature = Signature;
                Append(StringFormat("%s  slot +0x10 controlador=%p personagem=%p local=%d estado=%u +0x759=%u notificado=%u recompensado=%u de=+0x%zx\n",
                    Clock().c_str(), Ctrl, Character, Character != nullptr && Character == LocalCharacter() ? 1 : 0,
                    Bytes[kCtrlState], Pending, Bytes[kCtrlNotified], Bytes[kCtrlRewarded],
                    (size_t)((uintptr_t)_ReturnAddress() - s_base)));
            }
        }

        s_original_replica(Ctrl);
    }

    void InstantHook(void* Ctrl, char Kind)
    {
        if ((uint8_t)(Kind - 1) < 2 && Ctrl != nullptr)
        {
            const void* Character = *(void**)((uint8_t*)Ctrl + kCtrlCharacter);
            const bool Local = Character != nullptr && Character == LocalCharacter();
            const uint64_t Count = ++s_instant;
            if (Local || Count <= 30)
            {
                Append(StringFormat("%s  morte instantanea tipo=%d controlador=%p personagem=%p local=%d de=+0x%zx\n",
                    Clock().c_str(), (int)Kind, Ctrl, Character, Local ? 1 : 0,
                    (size_t)((uintptr_t)_ReturnAddress() - s_base)));
            }
        }

        s_original_instant(Ctrl, Kind);
    }

    bool BytesMatch(uintptr_t Address, const uint8_t* Expected, size_t Length)
    {
        return memcmp((const void*)Address, Expected, Length) == 0;
    }

    void Apply(const std::string& Line)
    {
        std::istringstream Parts(Line);
        std::string Verb;
        Parts >> Verb;

        if (Verb == "observe")
        {
            s_mode.store(Observe);
            Append(StringFormat("%s  === modo: observar ===\n", Clock().c_str()));
        }
        else if (Verb == "cancel")
        {
            s_mode.store(Cancel);
            Append(StringFormat("%s  === modo: cancelar a morte do jogador local ===\n", Clock().c_str()));
        }
        else if (Verb == "respawn")
        {
            s_mode.store(Respawn);
            Append(StringFormat("%s  === modo: renascer na fogueira, pagando a morte ===\n", Clock().c_str()));
        }
        else if (Verb == "status")
        {
            const int Mode = s_mode.load();
            Append(StringFormat("%s  === modo %s: vistas=%llu canceladas=%llu renascimentos=%llu recuperacoes=%llu recuperacoes_falhas=%llu sem_+0x759=%llu instantaneas=%llu chamadas_slot_+0x10=%llu ===\n",
                Clock().c_str(), Mode == Respawn ? "renascer" : (Mode == Cancel ? "cancelar" : "observar"),
                (unsigned long long)s_seen.load(), (unsigned long long)s_cancelled.load(),
                (unsigned long long)s_respawns.load(),
                (unsigned long long)s_recovered.load(), (unsigned long long)s_recovery_failed.load(),
                (unsigned long long)s_unexplained.load(), (unsigned long long)s_instant.load(),
                (unsigned long long)s_replica_calls.load()));
        }
        else if (!Verb.empty())
        {
            Append(StringFormat("%s  === nao entendi: %s ===\n", Clock().c_str(), Line.c_str()));
        }
    }

    void Run()
    {
        while (s_running.load())
        {
            std::error_code Error;
            if (std::filesystem::exists(s_request_path, Error))
            {
                std::ifstream Stream(s_request_path);
                std::string Line;
                while (std::getline(Stream, Line))
                {
                    if (!Line.empty())
                    {
                        Apply(Line);
                    }
                }
                Stream.close();
                std::filesystem::remove(s_request_path, Error);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

#endif
}

bool DS2_DeathInterceptHook::Install(Injector& injector)
{
#ifdef _WIN32
    s_base = (uintptr_t)injector.GetBaseAddress();

    struct { size_t Offset; const uint8_t* Bytes; size_t Length; const char* Name; } Checks[] = {
        { kUpdateOffset, kUpdateBytes, sizeof(kUpdateBytes), "controlador da morte" },
        { kReplicaOffset, kReplicaBytes, sizeof(kReplicaBytes), "slot +0x10" },
        { kInstantOffset, kInstantBytes, sizeof(kInstantBytes), "morte instantanea" },
        { kRecordSoulsOffset, kRecordSoulsBytes, sizeof(kRecordSoulsBytes), "almas para a mancha" },
        { kSpawnBloodstainOffset, kSpawnBloodstainBytes, sizeof(kSpawnBloodstainBytes), "mancha no mundo" },
        { kHollowOffset, kHollowBytes, sizeof(kHollowBytes), "hollow" },
        { kNoPenaltyOffset, kNoPenaltyBytes, sizeof(kNoPenaltyBytes), "morte sem penalidade" },
        { kNotAPlayerOffset, kNotAPlayerBytes, sizeof(kNotAPlayerBytes), "personagem especial" },
        { kRefillEstusOffset, kRefillEstusBytes, sizeof(kRefillEstusBytes), "estus" },
    };
    for (const auto& Check : Checks)
    {
        if (!BytesMatch(s_base + Check.Offset, Check.Bytes, Check.Length))
        {
            Error("[DS2_DeathInterceptHook] %s em +0x%zx nao e o esperado; recusando",
                Check.Name, Check.Offset);
            return false;
        }
    }

    s_log_path = injector.GetDllPath() / "DS2_Death.log";
    s_request_path = injector.GetDllPath() / "DS2_Death.req";

    s_original_update = (Update_p)(s_base + kUpdateOffset);
    s_original_replica = (Replica_p)(s_base + kReplicaOffset);
    s_original_instant = (Instant_p)(s_base + kInstantOffset);
    s_record_souls = (RecordSouls_p)(s_base + kRecordSoulsOffset);
    s_spawn_bloodstain = (SpawnBloodstain_p)(s_base + kSpawnBloodstainOffset);
    s_hollow = (Hollow_p)(s_base + kHollowOffset);
    s_no_penalty = (Check_p)(s_base + kNoPenaltyOffset);
    s_not_a_player = (Check_p)(s_base + kNotAPlayerOffset);
    s_refill_estus = (RefillEstus_p)(s_base + kRefillEstusOffset);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_update, UpdateHook);
    DetourAttach(&(PVOID&)s_original_replica, ReplicaHook);
    DetourAttach(&(PVOID&)s_original_instant, InstantHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        Error("[DS2_DeathInterceptHook] nao consegui instalar os detours");
        return false;
    }

    s_running.store(true);
    s_thread = std::thread(Run);

    Append(StringFormat("%s  === ds2os morte: modo observar ===\n", Clock().c_str()));
    Log("[DS2_DeathInterceptHook] pronto; escreva cancel em DS2_Death.req para segurar a morte");
#endif
    return true;
}

void DS2_DeathInterceptHook::Uninstall()
{
#ifdef _WIN32
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }

    if (s_original_update != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_update, UpdateHook);
        DetourDetach(&(PVOID&)s_original_replica, ReplicaHook);
        DetourDetach(&(PVOID&)s_original_instant, InstantHook);
        DetourTransactionCommit();
        s_original_update = nullptr;
    }
#endif
}

const char* DS2_DeathInterceptHook::GetName()
{
    return "DS2 Death Intercept";
}
