/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_DeathInterceptHook.h"
#include "Injector/Hooks/DarkSouls2/DS2_CoopChannelHook.h"
#include "Injector/Hooks/DarkSouls2/DS2_BackreadHook.h"
#include "Injector/Hooks/DarkSouls2/DS2_TravelWatchHook.h"
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
    constexpr size_t kFallGrounded = 0x20;             // last position on the ground, where a fall is measured from
    constexpr size_t kMotion = 0xf8;
    constexpr size_t kPhysics = 0x100;
    constexpr size_t kPhysicsProxy = 0x320;            // hkpCharacterRigidBody
    constexpr size_t kProxyBody = 0x20;                // hkpRigidBody
    constexpr size_t kRigidBodyVftable = 0x1126578;
    constexpr float kBodyAboveFeet = 0.05f;

    // The last bonfire, found the way the respawn finds it (step 2).
    constexpr size_t kBonfireRecord = 0x70;            // ctx+0x70
    constexpr size_t kRecordMap = 0x164;               // then +0x168 type, +0x16c id
    constexpr size_t kRecordId = 0x16c;
    constexpr size_t kRecordList = 0x58;
    constexpr size_t kListFirst = 0x08;
    constexpr size_t kNodeObject = 0x08;
    constexpr size_t kNodeNext = 0x60;
    constexpr size_t kObjectKind = 0xa2;               // 1 or 5: the short component path
    constexpr size_t kObjectComponents = 0xb8;
    constexpr size_t kComponentsReaction = 0x20;       // MapObjReactionComponent
    constexpr size_t kReactionId = 0xe0;
    constexpr size_t kObjectMap = 0x28;                // *(*(obj+0x28)+8), FUN_1403ba320
    constexpr size_t kMapId = 0x08;
    constexpr size_t kObjectAxisZ = 0x60;
    constexpr size_t kObjectTranslation = 0x70;
    constexpr float kSpawnBehind = 1.1f;

    // Frames to wait for the fall controller to say the character is down.
    constexpr uint32_t kRecoveryRetryFrames = 30;
    constexpr uint32_t kRecoveryGiveUpFrames = 300;

    // A bonfire in a map that is not loaded (step 8). The map is brought in
    // beside the current one without a warp (DS2_BackreadHook), the character
    // waits where it stands, and goes once the bonfire is in the list and its
    // map is loaded; the map is let go once the character stands on it.
    constexpr uint32_t kLoadPollFrames = 10;
    constexpr uint32_t kLoadGiveUpFrames = 1800;       // 30 s at 60 frames a second
    constexpr uint32_t kSettleGiveUpFrames = 600;
    // Two maps can overlap: Heide's first bonfire stands where Majula's sea
    // rocks are (measured 14/09). Landed on the old map's ground, the character
    // is sent to the bonfire again while the old map's parts go.
    constexpr uint32_t kSettleRetryFrames = 90;
    constexpr uint8_t kRemotePlayerCopy = 2;           // chr+0x54
    // Once there, the ground of that map still has to come in, and the fall
    // controller re-teleports every 30 frames until the character lands.
    constexpr uint32_t kOtherMapGiveUpFrames = 900;
    constexpr uint8_t kMapLoaded = 5;                  // MapAreaCtrlOwner+0x1e8
    constexpr size_t kMapManager = 0x38;               // ctx+0x38
    constexpr size_t kMapStreamer = 0x08;
    constexpr size_t kStreamerPart = 0x28;             // the part the player was last on (FUN_1403dc8e0)
    constexpr size_t kPartOwner = 0x28;                // MapEntity -> MapAreaCtrlOwner
    constexpr size_t kOwnerMap = 0x08;
    // The map a character stands in, the way FUN_140312ba0 finds it: the
    // physics contact `*(chr+0x100)+0x10`, whose handle at +0xe0 has the kind
    // in the low nibble (7 a map hit, 1 a map object), the map index in bits
    // 4..9 and, for a hit, the index of the collision in bits 10 and up.
    // Other players' maps are kept on this machine (DS2_BackreadHook) for a
    // few seconds after they were last seen there.
    constexpr size_t kPhysicsContact = 0x10;
    constexpr size_t kContactHandle = 0xe0;
    constexpr uint32_t kKeepOtherPlayerMs = 5000;

    // With the parts around them, not whole: what the game would have in for a
    // player standing there, and no more, since maps share coordinates.
    // FUN_140312ba0 resolves the contact to a map entity, a part when its kind
    // (+0xa2) is 2, which is how FUN_1403be060 finds the player's. A part
    // carries the parts to have in around it (`*(*(part+0x30)+0x70)` points at
    // 128 bits), the sets the streamer ORs together for the cells near the
    // player (FUN_1403da960), and its map index is `*(*(part+0x28)+0xc)`
    // (FUN_1403ba380). Measured 14/09 solo, with `keep`: Majula kept whole
    // did not reach Heide's first bonfire (a respawn there stood on Heide,
    // contact 0xc7), and kept with the set of its bonfire's part (bit 37) it
    // stayed loaded, forced, with only that part.
    constexpr size_t kPartUnderOffset = 0x312ba0;
    constexpr uint8_t kPartUnderBytes[] = { 0x48, 0x8b, 0x81, 0x00, 0x01, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x27 };
    constexpr size_t kEntityKind = 0xa2;               // byte
    constexpr uint8_t kEntityPart = 2;
    constexpr size_t kPartInfo = 0x30;
    constexpr size_t kPartSet = 0x70;                  // -> 4 x uint32
    constexpr size_t kOwnerIndex = 0x0c;
    // MapAreaCtrlOwner. The parts mask below is read through this owner and
    // then OR'd, by DS2_BackreadHook, into the real map owner's own mask
    // fields. Sixteen bytes of part bits taken from a stale chain would ask
    // the streamer for parts a map does not have, so the chain has to prove
    // what it is rather than be assumed (16/09).
    constexpr size_t kMapOwnerVftable = 0x10e87f0;

    // The character's physics body, and the Havok body hanging off it.
    //
    //   chr+0x100            ChrPhysicsCtrl
    //     +0x40              PXCharacterRigidBody (vftable 0x1411e42a8)
    //       +0x110           hkpRigidBody         (vftable 0x141126578)
    //         +0x18          the object the animation chain reads
    //
    // Measured on 16/09 with the page watch of DS2_TraceHook, which is what
    // finally named this: the host's repeating close read exactly that path
    // (`FUN_140bd15b0`), and the write that landed on `hkpRigidBody+0x18` was
    // a `rep movsb` of 0x1d28 bytes from FUN_1404e00d0 - a function that
    // **allocates** a buffer and copies into it. So the Havok body's memory
    // had been handed out again while the character still pointed at it: a use
    // after free with the address reused, not a stray write. That also
    // explains why the garbage kept changing shape, from floats to UTF-16 text
    // from a file path.
    //
    // The cure is the game's own: `FUN_140bd1500` releases both Havok fields
    // and **writes 0** into them, and every reader tests for 0 first
    // (`FUN_140bd15b0` returns straight away on a null +0x110). So a body that
    // is provably gone is simply dropped, and the game takes the null in its
    // stride. Nothing is freed here: the block is already somebody else's.
    constexpr size_t kChrPhysics = 0x100;
    constexpr size_t kPhysicsRigidBody = 0x40;
    constexpr size_t kRigidBodyHavok = 0x110;
    constexpr size_t kHavokInner = 0x18;
    constexpr size_t kPxRigidBodyVftable = 0x11e42a8;

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
    // The hollow level alone changes nothing: the maximum HP multiplier
    // (FUN_140202820) is 1.0 while *(chr+0xb0)+0x3e says human, and the model
    // keeps its human look. After a load, the player update calls
    // FUN_1402026e0(chr), which turns the level into that state (0 human, 1
    // hollow, 2 past a param row's threshold), changes the model, marks
    // PlayerParam and works the maximum out again (FUN_140202ca0). It is the
    // opposite of what the Human Effigy calls, FUN_140203d50. Found by the
    // effective maximum staying at 915 with hollow 1 (13/09).
    constexpr size_t kHollowStateOffset = 0x2026e0;
    constexpr uint8_t kHollowStateBytes[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x81, 0x90, 0x04, 0x00, 0x00 };
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

    // Who pays, in a session (step 6, read on 13/09). The death sequence is
    // EventResult slot +0x20 (FUN_14018f830), built from a param row picked by
    // the role; for a death of type 1 its step FUN_14018fbc0 moves the souls
    // only when a session manager and NetSvrManager exist and FUN_14018fd70
    // agrees: the bonfire record's +0x1b9 is clear, and either the role is not
    // a guest's or its role param says +0x2e == 1. The role is the byte at
    // *(chr+0xb0)+0x3c, and a 16-byte table per role says, in its second byte,
    // what kind of guest it is (0 the owner of the world, 1 for the white
    // phantom roles 1 and 3).
    constexpr size_t kChrRoles = 0xb0;
    constexpr size_t kRole = 0x3c;
    constexpr uint8_t kWorldOwnerRole = 0;
    constexpr size_t kRoleTableOffset = 0x10c0050;
    constexpr size_t kRoleRows = 0x14;
    constexpr size_t kRoleRowSize = 0x10;
    constexpr size_t kRoleGuestKind = 0x01;
    constexpr size_t kSessionManager = 0x22f0;         // ctx+0x22f0
    constexpr size_t kRecordNoSouls = 0x1b9;           // *(ctx+0x70), byte
    constexpr size_t kRoleParamOffset = 0x16f540;      // param row*(role)
    constexpr uint8_t kRoleParamBytes[] = { 0x48, 0x8b, 0x05, 0xa9, 0x53, 0x4a, 0x01, 0x8b, 0xd1, 0x48, 0x8b, 0x48, 0x18 };
    constexpr size_t kRoleParamGuestPays = 0x2e;       // byte, 1: this guest loses its souls
    // FUN_14026b0d0 puts the bloodstain in the world only when the context's
    // slot +0x58 says no; nothing is removed unless it will.
    constexpr size_t kContextNoBloodstain = 0x58;

    // The rest of FUN_14037dcc0, the function that hollows. Its first check
    // is not in the decompiler's view: `call 0x14016f7d0` is a jump into
    // obfuscated code, `bool(chr)`, nonzero meaning no hollowing. Its second,
    // `call 0x140203be0`, the same kind of jump, picks the branch for the
    // player of this machine: FUN_140203ad0 adds the death to PlayerParam
    // +0x104+role*8 and +0x1a4, FUN_1401ac240 breaks the protection ring that
    // was worn (it unequips the four item ids and hands back the broken one),
    // and a menu of ctx+0x22e0 that was open is closed. The other branch is
    // for somebody else's character dying in this world.
    constexpr size_t kHollowExemptOffset = 0x16f7d0;
    constexpr uint8_t kHollowExemptBytes[] = { 0xe9, 0x2e, 0x53, 0xea, 0xff };
    constexpr size_t kLocalBranchOffset = 0x203be0;
    constexpr uint8_t kLocalBranchBytes[] = { 0xe9, 0x7b, 0x56, 0x36, 0x00 };
    constexpr size_t kDeathCounterOffset = 0x203ad0;
    constexpr uint8_t kDeathCounterBytes[] = { 0x48, 0x85, 0xc9, 0x74, 0x17, 0x48, 0x8b, 0x81, 0xb0, 0x00, 0x00, 0x00 };
    constexpr size_t kRingBreakOffset = 0x1ac240;
    constexpr uint8_t kRingBreakBytes[] = { 0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0xe8, 0xb5, 0xf3, 0xff, 0xff };
    constexpr size_t kMenuOpenOffset = 0x500900;
    constexpr uint8_t kMenuOpenBytes[] = { 0x48, 0x8b, 0x89, 0x10, 0x01, 0x00, 0x00, 0x48, 0x85, 0xc9 };
    constexpr size_t kMenuCloseOffset = 0x4fecd0;
    constexpr uint8_t kMenuCloseBytes[] = { 0x48, 0x8b, 0x81, 0x10, 0x01, 0x00, 0x00, 0x48, 0x85, 0xc0 };
    constexpr size_t kFrontEnd = 0x22e0;               // ctx+0x22e0
    constexpr size_t kParamDeaths = 0x1a4;             // PlayerParam, int

    // The bloodstain other players see. NetSvrBloodstainManager's own update
    // (slot +0x38) starts the job on the first frame the local character has
    // HP 0 and bit 0x4000 of +0x4c8 - the frame this hook never lets happen -
    // through FUN_14026c0b0, or FUN_14026c1b0 for a death of kind 3 (turned to
    // stone). Both check FUN_14026bc50 themselves; the job runs five seconds
    // later (FUN_14026bd10) and sends RequestCreateBloodstain only when
    // FUN_14019f520 finds, in the ghost recorder's last 16 frames, one marked
    // 0x1000 - a frame recorded with the character dead. Measured on 13/09:
    // called from here, the job runs and sends nothing, because no such frame
    // was ever recorded. Off by default until that frame is understood.
    constexpr size_t kOnlineStainOffset = 0x26c0b0;
    constexpr uint8_t kOnlineStainBytes[] = { 0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xf9, 0xe8, 0x92, 0xfb, 0xff, 0xff };
    constexpr size_t kOnlineStatueOffset = 0x26c1b0;
    constexpr uint8_t kOnlineStatueBytes[] = { 0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xf9, 0xe8, 0x92, 0xfa, 0xff, 0xff };
    constexpr int8_t kDeathKindStone = 3;

    // "YOU DIED", the way EventResult shows it (FUN_1401909c0). The death's
    // param row, id role + type * 100 in the bonfire record's table
    // (FUN_14044ed10, type*100 + 99 when the role has none), holds an FE type
    // in its first byte, and the ten ints at 0x1410c3580 turn it into a banner
    // for FUN_1405012e0(*(ctx+0x22e0), id). Read live on 13/09: row 100 (the
    // owner of the world) and row 199 (a white phantom) both say FE 1, banner
    // 3. Banner 3 also hides the HUD (+0x46c of *(frontend+0xd8)) and nothing
    // but a load puts it back: FUN_1404fffb0 sets +0x468, which the HUD's
    // update (FUN_140507360) takes as "show everything again". The hook calls
    // it once FUN_140500b10 says the front end is no longer busy. Measured in
    // a session on 14/09, host and phantom: the banner, the HUD hidden, and
    // the HUD back 203 frames later.
    constexpr size_t kParamRowOffset = 0x44ed10;
    constexpr uint8_t kParamRowBytes[] = { 0x48, 0x8b, 0x81, 0x50, 0x01, 0x00, 0x00, 0x48, 0x85, 0xc0 };
    constexpr size_t kBannerOffset = 0x5012e0;
    constexpr uint8_t kBannerBytes[] = { 0x48, 0x8b, 0x89, 0xd8, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9 };
    constexpr size_t kFrontEndBusyOffset = 0x500b10;
    constexpr uint8_t kFrontEndBusyBytes[] = { 0x48, 0x8b, 0x89, 0xd8, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x75, 0x03 };
    constexpr size_t kHudResetOffset = 0x4fffb0;
    constexpr uint8_t kHudResetBytes[] = { 0xc7, 0x81, 0x1c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x89, 0xd8 };
    constexpr size_t kFeTableOffset = 0x10c3580;       // int32[10]
    constexpr uint32_t kFeTypes = 10;
    constexpr int32_t kBannerNeedsCheck = 9;           // FUN_1401909c0 may swap it; not used by deaths
    constexpr size_t kFrontEndHud = 0xd8;
    constexpr size_t kHudHidden = 0x46c;               // int
    constexpr int kDeathEvent = 1;                     // FUN_140191df0, the local player's death
    constexpr int kDeathEventGuestKind3 = 0x1b;        // the same, for a role whose guest kind is 3
    constexpr uint32_t kBannerMinFrames = 30;
    constexpr uint32_t kBannerGiveUpFrames = 60 * 30;

    // The other machine's copy of a player. Measured on 13/09 with Chico
    // summoned into Samuel's world: Chico's death was refused on his machine
    // and he stood at the bonfire, but the HP 0 had already gone out, and
    // Samuel's copy of him took it through its own controller - state 0 -> 2,
    // "Phantom Chico has been vanquished", RequestNotifyKillEnemy - and was
    // never seen again. A player's death belongs to the machine that plays
    // the character; the copy's is refused here the same way.
    constexpr size_t kPlayerCtrlVftable = 0x10e4bb8;   // PlayerCtrl, local and remote alike
    constexpr size_t kChrType = 0x54;                  // byte, indexes the 5-byte table at 0x1410bfff0

    // Parts of the bill that can be switched off from DS2_Death.req, so a
    // session test can take one out without a build.
    enum Feature : uint32_t
    {
        FeatureSouls = 1 << 0,
        FeatureHollow = 1 << 1,
        FeatureCounter = 1 << 2,
        FeatureRing = 1 << 3,
        FeatureOnlineStain = 1 << 4,
        FeatureEstus = 1 << 5,
        FeatureBanner = 1 << 6,
        FeatureRemote = 1 << 7,
        FeatureHostBonfire = 1 << 8,
        FeatureOtherMap = 1 << 9,
    };
    struct FeatureName
    {
        const char* Name;
        uint32_t Bit;
    };
    constexpr FeatureName kFeatures[] = {
        { "almas", FeatureSouls },
        { "hollow", FeatureHollow },
        { "contador", FeatureCounter },
        { "anel", FeatureRing },
        { "mancha_online", FeatureOnlineStain },
        { "estus", FeatureEstus },
        { "banner", FeatureBanner },
        { "copias", FeatureRemote },
        { "fogueira_do_host", FeatureHostBonfire },
        { "outro_mapa", FeatureOtherMap },
    };

    enum Mode : int
    {
        Observe = 0,
        Cancel = 1,
        Respawn = 2,
    };

    using RecordSouls_p = void(*)(void* Manager, uint64_t* Out);
    using SpawnBloodstain_p = uint8_t(*)(void* Manager);
    using Hollow_p = void(*)(void* PlayerParam, int Kind);
    using HollowState_p = void(*)(void* Character);
    using Check_p = uint64_t(*)(void* Object);
    using RefillEstus_p = void(*)(void* Inventory);
    using SetCount_p = uint32_t(*)(void* Set);
    using SetEntry_p = uint32_t*(*)(void* Set, uint32_t Index);
    using RemoveSign_p = void(*)(void* Interface, uint32_t* Entry);

    using RoleParam_p = uintptr_t(*)(int Role);
    using Action_p = void(*)(void* Object);
    using Global_p = void(*)();
    using ContextCheck_p = uint64_t(*)(void* Context);
    using ParamRow_p = uintptr_t(*)(void* Record, uint32_t Id);
    using Banner_p = void(*)(void* FrontEnd, uint32_t Banner);

    RecordSouls_p s_record_souls = nullptr;
    SpawnBloodstain_p s_spawn_bloodstain = nullptr;
    Hollow_p s_hollow = nullptr;
    HollowState_p s_hollow_state = nullptr;
    Check_p s_no_penalty = nullptr;
    Check_p s_not_a_player = nullptr;
    RefillEstus_p s_refill_estus = nullptr;
    RoleParam_p s_role_param = nullptr;
    Check_p s_hollow_exempt = nullptr;
    Check_p s_local_branch = nullptr;
    Action_p s_death_counter = nullptr;
    Global_p s_ring_break = nullptr;
    Check_p s_menu_open = nullptr;
    Action_p s_menu_close = nullptr;
    Action_p s_online_stain = nullptr;
    Action_p s_online_statue = nullptr;
    ParamRow_p s_param_row = nullptr;
    Banner_p s_banner = nullptr;
    Check_p s_front_end_busy = nullptr;
    Action_p s_hud_reset = nullptr;
    using PartUnder_p = uintptr_t(*)(void* Chr);
    PartUnder_p s_part_under = nullptr;

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
    std::atomic<uint64_t> s_remote_transitions{ 0 };
    std::atomic<uint64_t> s_remote_refused{ 0 };
    std::atomic<uint32_t> s_features{ FeatureSouls | FeatureHollow | FeatureCounter | FeatureRing | FeatureEstus | FeatureBanner | FeatureRemote | FeatureHostBonfire | FeatureOtherMap };

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
        // A travel: the HP is not given back, and there was no death.
        bool KeepHp = false;
        // Waiting for the bonfire's map to come in.
        bool Loading = false;
        uint32_t LoadMap = 0;
        uint32_t LoadId = 0;
        uint32_t LoadFrames = 0;
        // The destination is not asked for until the target budget has room
        // for it (DS2_Backread::Targets): first the holds go, then, if that
        // is not enough, the map being left.
        bool WaitRoom = false;
        ULONGLONG RoomSince = 0;
        uint32_t RoomCost = 0;
        int32_t RoomSource = -1;
        bool SourceAsked = false;
        ULONGLONG RoomAt = 0;
    };
    Recovery s_recovery;

    // The travel budget. The TargetManager holds 2048 entries for every
    // loaded map together, and the game ends the process when it is full:
    // 19/09, Majula held (313) + Frozen Eleum Loyce (1392) + the next
    // destination (794) killed the host. A travel's peak is the session's map
    // + the map left + the destination, so before asking for the destination
    // the budget is read, and when it would not fit the 30 s holds go first
    // (the map left is not held) and then, if that is still not enough, the
    // map left is taken down behind the black screen, as a loading screen
    // would, before the destination is asked for.
    constexpr ULONGLONG kRoomHoldsMs = 3000;      // time for the holds to go
    // A map's release goes on on the game's worker threads after its owner
    // reads 0: measured 19/09, Brume Tower asked for 16 ms after Eleum Loyce
    // reached 0 killed the host on a worker thread (+0x833655, under
    // +0x96ab09 / +0xb4ed3b) 77 ms later. So the room has to stay made this
    // long before the destination is asked for.
    constexpr ULONGLONG kRoomSettleMs = 2000;
    // The map left goes only once nobody stands in it, and a guest's copy
    // holds it 5 s after it has gone (kKeepOtherPlayerMs).
    constexpr ULONGLONG kRoomGiveUpMs = 30000;    // then load anyway, and say so
    bool s_travel_tight = false;
    std::atomic<bool> s_park_pending{ false };
    bool s_parked = false;
    // Where this machine's player waits while a map is taken down; another
    // player's copy standing near it has left the map too.
    float s_park_spot[3] = {};
    bool s_park_spot_valid = false;
    uint32_t s_park_map = 0;
    constexpr float kParkNear = 40.0f;
    uint64_t s_keep_skipped = 0;

    // A fall in the first seconds after a travel ended is the travel's, not a
    // death. Measured 18/09 22:29: the guest stood on Iron Keep's collision
    // at Threshold Bridge, the travel ended, and half a second later the
    // character fell through and the death was billed and respawned. The
    // ground it first touched was not the ground that stayed.
    constexpr ULONGLONG kAfterTravelMs = 3000;
    ULONGLONG s_travel_done_ms = 0;
    float s_travel_target[3] = {};

    // A travel asked for by DS2_BonfireInSessionHook, from the game's thread;
    // taken by the local player's next frame.
    std::atomic<bool> s_go_pending{ false };
    std::atomic<uint32_t> s_go_map{ 0 };
    std::atomic<uint32_t> s_go_id{ 0 };
    std::atomic<bool> s_go_moving{ false };
    // How the travel ended; see DS2_DeathIntercept::Outcome.
    std::atomic<uint8_t> s_travel_outcome{ 0 };
    // The map the travel started in, kept a while after the arrival: letting
    // it unload right behind a travel killed the guest twice (15/09, a freed
    // pointer in a list walked by FUN_1401cbf20 and in the pre-draw task),
    // and it only ever happened with the other player's copy in that map.
    int32_t s_travel_from = -1;
    // Both maps of a travel are held by the travel itself, from the moment it
    // starts until well after everyone has landed - not by whichever map the
    // other player's copy happens to be standing on, which goes stale the
    // moment that copy moves (measured 15/09: the keep for the copy's map
    // arrived after the guest had already left it).
    constexpr uint32_t kTravelHoldMs = 30000;
    // What the watcher writes down around a travel.
    constexpr uint32_t kWatchStartMs = 12000;
    constexpr uint32_t kWatchLandedMs = 8000;

    // After the jump to another map: holding that map until the character
    // stands on it, then letting go.
    struct Settle
    {
        bool Active = false;
        uint32_t Map = 0;
        uint32_t Frames = 0;
        float Target[3] = {};
        uint32_t Retries = 0;
    };
    Settle s_settle;

    // The map this machine's player last stood in as the owner of its world,
    // and how many frames a guest arriving elsewhere has waited for the host's
    // bonfire (0: not waiting).
    uint64_t s_bodies_dropped = 0;    // game thread only
    uint32_t s_owner_map = 0;
    uint32_t s_arrival_frames = 0;
    uint8_t s_last_local_role = 0xff;
    constexpr uint32_t kArrivalGiveUpFrames = 30 * 60;
    constexpr uint32_t kArrivalSettleFrames = 30;

    // Waiting for the banner to end, to give the HUD back.
    struct BannerWait
    {
        bool Active = false;
        uint32_t Frames = 0;
        uint32_t Banner = 0;
    };
    BannerWait s_banner_wait;

    // Consecutive refusals for other players' copies, to keep the log short.
    uint64_t s_remote_streak = 0;
    ULONGLONG s_remote_last_ms = 0;

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

    // The record of the last bonfire: map, type and id.
    bool ReadRecord(uint32_t& Map, int32_t& Type, uint32_t& Id)
    {
        uintptr_t Context = 0, Record = 0;
        int32_t Fields[3] = {};
        if (!ReadPointer(s_base + kContextOffset, Context) ||
            !ReadPointer(Context + kBonfireRecord, Record) ||
            !ReadBytes(Record + kRecordMap, Fields, sizeof(Fields)))
        {
            return false;
        }
        Map = (uint32_t)Fields[0];
        Type = Fields[1];
        Id = (uint32_t)Fields[2];
        return true;
    }

    // The map of the part the player last stood on, as the streamer keeps it.
    uint32_t CurrentMap()
    {
        uintptr_t Context = 0, Manager = 0, Streamer = 0, Part = 0, Owner = 0;
        uint32_t Map = 0;
        if (ReadPointer(s_base + kContextOffset, Context) &&
            ReadPointer(Context + kMapManager, Manager) &&
            ReadPointer(Manager + kMapStreamer, Streamer) &&
            ReadPointer(Streamer + kStreamerPart, Part) &&
            ReadPointer(Part + kPartOwner, Owner))
        {
            ReadBytes(Owner + kOwnerMap, &Map, sizeof(Map));
        }
        return Map;
    }

    int32_t MapIndexUnder(uint8_t* Chr);

    // Whether the character stands on Map. The streamer's last part says so
    // most of the time; after a fall on arrival it stayed null for the whole
    // settle (18/09 21:54, Threshold Bridge) while the contact under the feet,
    // collision on index 7, named Iron Keep. Either one is enough.
    bool StandsOn(uint8_t* Chr, uint32_t Map)
    {
        if (CurrentMap() == Map)
        {
            return true;
        }
        const int32_t Under = MapIndexUnder(Chr);
        return Under >= 0 && Under == DS2_Backread::IndexOf(Map);
    }

    // The handle of what the character stands on, raw: its kind in the low
    // nibble, and for collision (7) or a map object (1) the map index in bits
    // 4..9. False in the air.
    bool ContactHandle(uint8_t* Chr, uint32_t& Handle)
    {
        uintptr_t Physics = 0, Contact = 0;
        Handle = 0;
        return ReadPointer((uintptr_t)Chr + kPhysics, Physics) &&
            ReadPointer(Physics + kPhysicsContact, Contact) &&
            ReadBytes(Contact + kContactHandle, &Handle, sizeof(Handle));
    }

    int32_t MapIndexUnder(uint8_t* Chr)
    {
        uint32_t Handle = 0;
        if (!ContactHandle(Chr, Handle))
        {
            return -1;
        }
        const uint32_t Kind = Handle & 0xf;
        return Kind == 7 || Kind == 1 ? (int32_t)((Handle >> 4) & 0x3f) : -1;
    }

    uintptr_t CallPartUnder(uint8_t* Chr)
    {
        __try
        {
            return s_part_under(Chr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // The map index of the part a character stands on and the parts around
    // it. False in the air, on a map object, or with no parts to say.
    bool PartsUnder(uint8_t* Chr, int32_t& Index, uint32_t Mask[4])
    {
        const uintptr_t Part = CallPartUnder(Chr);
        uint8_t Kind = 0;
        uintptr_t Owner = 0, Info = 0, Set = 0, OwnerVftable = 0;
        Index = -1;
        return Part != 0 &&
            ReadBytes(Part + kEntityKind, &Kind, 1) && Kind == kEntityPart &&
            ReadPointer(Part + kPartOwner, Owner) &&
            ReadPointer(Owner, OwnerVftable) && OwnerVftable == s_base + kMapOwnerVftable &&
            ReadBytes(Owner + kOwnerIndex, &Index, sizeof(Index)) && Index >= 0 && Index <= 0x3f &&
            ReadPointer(Part + kPartInfo, Info) &&
            ReadPointer(Info + kPartSet, Set) &&
            ReadBytes(Set, Mask, 4 * sizeof(uint32_t)) &&
            (Mask[0] | Mask[1] | Mask[2] | Mask[3]) != 0;
    }

    // Where the character stands and on what, for the log.
    std::string DescribeFooting(uint8_t* Chr)
    {
        float Feet[3] = {};
        uint32_t Handle = 0;
        const bool HaveFeet = ReadBytes((uintptr_t)Chr + 0x90, Feet, sizeof(Feet));
        const bool HaveHandle = ContactHandle(Chr, Handle);
        return StringFormat("em (%s), contato %s", HaveFeet ? StringFormat("%.3f, %.3f, %.3f", Feet[0], Feet[1], Feet[2]).c_str() : "?",
            HaveHandle ? StringFormat("%08x", Handle).c_str() : "nenhum");
    }

    uint8_t RoleOf(uint8_t* Chr)
    {
        uintptr_t Roles = 0;
        uint8_t Role = 0xff;
        if (ReadPointer((uintptr_t)Chr + kChrRoles, Roles))
        {
            ReadBytes(Roles + kRole, &Role, 1);
        }
        return Role;
    }

    // The spawn point of a bonfire of the loaded map: translation - 1.1 * Z
    // axis of its map object, which is where the game itself put the character
    // (0.000 m, measured on 13/09). The map is compared too, the way
    // FUN_1401caf50 records it (`*(*(obj+0x28)+8)`, 0x0a1f0000 for all three
    // of Heide, read on 14/09): an id alone is only an object of some map.
    // A place to stand while the map left is taken down: the spawn of the
    // first loaded bonfire whose map is neither of these two. In a session
    // that is the session's map, which is never let go.
    bool FindParking(uint32_t NotMap, uint32_t NorMap, float Out[3], uint32_t& Map)
    {
        uintptr_t Context = 0, Record = 0, List = 0, Node = 0;
        if (!ReadPointer(s_base + kContextOffset, Context) ||
            !ReadPointer(Context + kBonfireRecord, Record) ||
            !ReadPointer(Record + kRecordList, List) ||
            !ReadPointer(List + kListFirst, Node))
        {
            return false;
        }
        for (int i = 0; i < 256 && Node != 0; ++i)
        {
            uintptr_t Object = 0, MapAt = 0;
            uint8_t Kind = 0;
            uint32_t NodeMap = 0;
            if (ReadPointer(Node + kNodeObject, Object) &&
                ReadBytes(Object + kObjectKind, &Kind, 1) && (Kind == 1 || Kind == 5) &&
                ReadPointer(Object + kObjectMap, MapAt) &&
                ReadBytes(MapAt + kMapId, &NodeMap, sizeof(NodeMap)) && NodeMap != 0 &&
                NodeMap != NotMap && NodeMap != NorMap)
            {
                float Axis[4] = {}, Translation[4] = {};
                if (ReadBytes(Object + kObjectAxisZ, Axis, sizeof(Axis)) &&
                    ReadBytes(Object + kObjectTranslation, Translation, sizeof(Translation)))
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        Out[k] = Translation[k] - kSpawnBehind * Axis[k];
                    }
                    Map = NodeMap;
                    return true;
                }
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

    bool FindBonfireSpawn(uint32_t Map, uint32_t Id, float Out[3])
    {
        uintptr_t Context = 0, Record = 0, List = 0, Node = 0;
        if (!ReadPointer(s_base + kContextOffset, Context) ||
            !ReadPointer(Context + kBonfireRecord, Record) ||
            !ReadPointer(Record + kRecordList, List) ||
            !ReadPointer(List + kListFirst, Node))
        {
            return false;
        }

        for (int i = 0; i < 256 && Node != 0; ++i)
        {
            uintptr_t Object = 0, Components = 0, Reaction = 0, IdAt = 0, MapAt = 0;
            uint8_t Kind = 0;
            uint32_t NodeId = 0, NodeMap = 0;
            if (ReadPointer(Node + kNodeObject, Object) &&
                ReadBytes(Object + kObjectKind, &Kind, 1) && (Kind == 1 || Kind == 5) &&
                ReadPointer(Object + kObjectComponents, Components) &&
                ReadPointer(Components + kComponentsReaction, Reaction) &&
                ReadPointer(Reaction + kReactionId, IdAt) &&
                ReadBytes(IdAt, &NodeId, sizeof(NodeId)) && NodeId == Id &&
                ReadPointer(Object + kObjectMap, MapAt) &&
                ReadBytes(MapAt + kMapId, &NodeMap, sizeof(NodeMap)) && NodeMap == Map)
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

    // Once a frame, for the channel: who the local player is and what its
    // record holds. The host announces it; see DS2_CoopChannelHook.h.
    void PublishLocal(uint8_t* Chr)
    {
        uint32_t Map = 0, Id = 0;
        int32_t Type = 0;
        if (ReadRecord(Map, Type, Id))
        {
            DS2_CoopChannel::PublishLocal(RoleOf(Chr), Map, Type, Id);
        }
    }

    // XYZ only, every w left alone; the order is the one that worked by hand.
    //
    // The fall controller's ground position goes along. A landing is measured
    // from it (FUN_140372560: `*(fall+0x24)` minus the height now), and the
    // game's own position request (`*(chr+0xc8)`, bit 0 of +0xfc) moves it too,
    // in FUN_140372620. Measured 14/09: a respawn 24.5 m below the death, whose
    // ground came in a few frames after the character, landed as a fall from
    // the death's height and killed the character a second time (cause 60).
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
        const bool Moved = WriteBytes((uintptr_t)Chr + 0x90, Feet, sizeof(Feet)) &&
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
        const uintptr_t Fall = FallController(Chr);
        if (Moved && Fall != 0)
        {
            WriteBytes(Fall + kFallGrounded, Feet, sizeof(Feet));
        }
        return Moved;
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

    bool CallHollowState(uintptr_t Character)
    {
        __try
        {
            s_hollow_state((void*)Character);
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

    bool CallRoleParam(int Role, uintptr_t& Row)
    {
        __try
        {
            Row = s_role_param(Role);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallAction(Action_p Action, uintptr_t Object)
    {
        __try
        {
            Action((void*)Object);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallGlobal(Global_p Action)
    {
        __try
        {
            Action();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallContextCheck(uintptr_t Context, size_t Slot, bool& Result)
    {
        __try
        {
            const uintptr_t Vftable = *(const uintptr_t*)Context;
            Result = ((*(ContextCheck_p*)(Vftable + Slot))((void*)Context) & 0xff) != 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallParamRow(uintptr_t Record, uint32_t Id, uintptr_t& Row)
    {
        __try
        {
            Row = s_param_row((void*)Record, Id);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool CallBanner(uintptr_t FrontEnd, uint32_t Banner)
    {
        __try
        {
            s_banner((void*)FrontEnd, Banner);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool Enabled(uint32_t Bit)
    {
        return (s_features.load() & Bit) != 0;
    }

    uintptr_t FrontEnd()
    {
        uintptr_t Context = 0, Object = 0;
        return ReadPointer(s_base + kContextOffset, Context) && ReadPointer(Context + kFrontEnd, Object) ? Object : 0;
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
    // it and behind the game's own gates for who pays what: souls into the
    // bloodstain record while the character still stands where it died (the
    // record takes the last safe position), hollowing, the counters and the
    // ring of the player of this machine, the bloodstain put in the world and
    // the one other players see, and the Estus the respawn refills.
    void ApplyDeathCosts(uint8_t* Chr, uint8_t* Data)
    {
        const uintptr_t Player = (uintptr_t)Chr;
        uintptr_t Param = 0, Context = 0, Roles = 0;
        const bool HaveParam = ReadPointer(Player + kPlayerParam, Param);
        const bool HaveContext = ReadPointer(s_base + kContextOffset, Context);

        uint8_t Role = 0, GuestKind = 0;
        if (ReadPointer(Player + kChrRoles, Roles))
        {
            ReadBytes(Roles + kRole, &Role, 1);
        }
        ReadBytes(s_base + kRoleTableOffset + (Role < kRoleRows ? Role : 0) * kRoleRowSize + kRoleGuestKind, &GuestKind, 1);

        uint32_t SoulsBefore = 0;
        uint8_t HollowBefore = 0;
        int32_t DeathsBefore = 0;
        if (HaveParam)
        {
            ReadBytes(Param + kParamSouls, &SoulsBefore, sizeof(SoulsBefore));
            ReadBytes(Param + kParamHollow, &HollowBefore, 1);
            ReadBytes(Param + kParamDeaths, &DeathsBefore, sizeof(DeathsBefore));
        }

        // Souls, if FUN_14018fbc0 and FUN_14018fd70 would move them.
        std::string Souls;
        bool Recorded = false;
        const uintptr_t Manager = BloodstainManager();
        uintptr_t Session = 0, Record = 0;
        if (!Enabled(FeatureSouls))
        {
            Souls = "desligado";
        }
        else if (Manager == 0)
        {
            Souls = "sem gerenciador de manchas";
        }
        else if (!HaveContext || !ReadPointer(Context + kSessionManager, Session))
        {
            Souls = "sem gerenciador de sessao; o jogo nao cobra";
        }
        else if (!ReadPointer(Context + kBonfireRecord, Record))
        {
            Souls = "sem registro da fogueira";
        }
        else
        {
            uint8_t NoSouls = 1, GuestPays = 0, Done = 1;
            uintptr_t RoleRow = 0;
            ReadBytes(Record + kRecordNoSouls, &NoSouls, 1);
            const bool GuestChecked = GuestKind == 0 ||
                (CallRoleParam(Role, RoleRow) && RoleRow != 0 && ReadBytes(RoleRow + kRoleParamGuestPays, &GuestPays, 1));
            ReadBytes(Manager + kBloodstainDone, &Done, 1);
            if (NoSouls != 0)
            {
                Souls = "o registro da fogueira diz sem almas (+0x1b9); ficam";
            }
            else if (!GuestChecked || (GuestKind != 0 && GuestPays != 1))
            {
                Souls = StringFormat("convidado que nao paga (param do papel %s, +0x2e=%u); ficam",
                    RoleRow != 0 ? "lido" : "AUSENTE", GuestPays);
            }
            else if (Done != 0)
            {
                Souls = "registro ja marcado nesta carga; ficam";
            }
            else
            {
                uint64_t Out = 0;
                const bool Called = CallRecordSouls(Manager, Out);
                uint8_t DoneAfter = 0;
                ReadBytes(Manager + kBloodstainDone, &DoneAfter, 1);
                Recorded = Called && DoneAfter != 0;
                Souls = StringFormat("%s: %u almas para a mancha, %u perdidas da anterior",
                    !Called ? "FALHOU" : (Recorded ? "registradas" : "recusadas pelo jogo"),
                    (uint32_t)(Out >> 32), (uint32_t)Out);
            }
        }

        // Hollowing, with the five checks FUN_14037dcc0 makes before it.
        std::string Hollow = "desligado";
        if (Enabled(FeatureHollow) && !HaveParam)
        {
            Hollow = "sem PlayerParam";
        }
        else if (Enabled(FeatureHollow))
        {
            bool Exempt = false, Obscure = false, NotPlayer = false;
            uint64_t StateBits = 0, EffectBits = 0;
            ReadBytes((uintptr_t)Data + kStateBits, &StateBits, sizeof(StateBits));
            ReadBytes((uintptr_t)Data + kEffectBits, &EffectBits, sizeof(EffectBits));
            const bool Checked = CallCheck(s_no_penalty, (uintptr_t)Data, Exempt) &&
                CallCheck(s_hollow_exempt, Player, Obscure) &&
                CallCheck(s_not_a_player, Player, NotPlayer);
            if (!Checked)
            {
                Hollow = "checagens FALHARAM; sem hollow";
            }
            else if (Exempt || (StateBits & kNoPenaltyBit) != 0 || Obscure || NotPlayer || (EffectBits & kNoHollowBit) != 0)
            {
                Hollow = StringFormat("isento (penalidade=%d ofuscada=%d especial=%d bits=%d/%d)",
                    Exempt ? 1 : 0, Obscure ? 1 : 0, NotPlayer ? 1 : 0, (StateBits & kNoPenaltyBit) != 0 ? 1 : 0,
                    (EffectBits & kNoHollowBit) != 0 ? 1 : 0);
            }
            else
            {
                const int Kind = (int)(int8_t)Data[kDeathKind];
                const bool Called = CallHollow(Param, Kind);
                uint8_t HollowAfter = HollowBefore;
                ReadBytes(Param + kParamHollow, &HollowAfter, 1);
                int32_t MaxBefore = 0, MaxAfter = 0;
                ReadBytes(Player + kHpMax, &MaxBefore, sizeof(MaxBefore));
                const bool Recomputed = Called && CallHollowState(Player);
                ReadBytes(Player + kHpMax, &MaxAfter, sizeof(MaxAfter));
                Hollow = StringFormat("%s: nivel %u -> %u, hp maximo %d -> %d%s", Called ? "aplicado" : "FALHOU",
                    HollowBefore, HollowAfter, MaxBefore, MaxAfter, Recomputed ? "" : " (recalculo FALHOU)");
            }
        }

        // The branch for the player of this machine.
        std::string Mine;
        bool Local = false;
        if (!CallCheck(s_local_branch, Player, Local))
        {
            Mine = "checagem FALHOU";
        }
        else if (!Local)
        {
            Mine = "o jogo diz que nao e o jogador desta maquina; nada";
        }
        else
        {
            if (Enabled(FeatureCounter) && HaveParam)
            {
                const bool Counted = CallAction(s_death_counter, Player);
                int32_t DeathsAfter = DeathsBefore;
                ReadBytes(Param + kParamDeaths, &DeathsAfter, sizeof(DeathsAfter));
                Mine += StringFormat("mortes %d -> %d%s", DeathsBefore, DeathsAfter, Counted ? "" : " (FALHOU)");
            }
            else
            {
                Mine += "contador desligado";
            }

            if (Enabled(FeatureRing))
            {
                Mine += CallGlobal(s_ring_break) ? ", anel conferido" : ", anel FALHOU";
            }

            uintptr_t FrontEnd = 0;
            bool Open = false;
            if (HaveContext && ReadPointer(Context + kFrontEnd, FrontEnd) &&
                CallCheck(s_menu_open, FrontEnd, Open) && Open)
            {
                Mine += CallAction(s_menu_close, FrontEnd) ? ", menu fechado" : ", menu FALHOU";
            }
        }

        // The old bloodstain out and the new one in, only for souls that moved
        // now, and only where FUN_14026b0d0 would put one.
        std::string Bloodstain = "nenhuma alma registrada; manchas intocadas";
        if (Recorded)
        {
            const uintptr_t SetCtrl = BloodstainSetCtrl();
            bool Refused = true;
            if (SetCtrl == 0)
            {
                Bloodstain = "sem BloodstainSetCtrl";
            }
            else if (!CallContextCheck(Context, kContextNoBloodstain, Refused))
            {
                Bloodstain = "checagem do contexto (+0x58) FALHOU";
            }
            else if (Refused)
            {
                // Still clears the mark, as the reload would.
                CallSpawnBloodstain(Manager);
                Bloodstain = "o contexto recusa mancha aqui (+0x58); manchas intocadas";
            }
            else
            {
                const int Removed = RemoveSoulsBloodstains(SetCtrl);
                const bool Spawned = CallSpawnBloodstain(Manager);
                Bloodstain = StringFormat("%d antiga(s) removida(s), nova %s, agora %d no mundo",
                    Removed, Spawned ? "criada" : "FALHOU", CountSoulsBloodstains(SetCtrl));
            }
        }

        std::string Online = "desligado";
        if (Enabled(FeatureOnlineStain))
        {
            const bool Stone = (int8_t)Data[kDeathKind] == kDeathKindStone;
            Online = Manager == 0 ? "sem gerenciador"
                : (CallAction(Stone ? s_online_statue : s_online_stain, Manager) ? (Stone ? "pedida (estatua)" : "pedida") : "FALHOU");
        }

        std::string Estus = "desligado";
        uintptr_t Holder = 0, Inventory = 0;
        if (Enabled(FeatureEstus))
        {
            Estus = !(HaveContext && ReadPointer(Context + kInventoryHolder, Holder) && ReadPointer(Holder + kHolderInventory, Inventory))
                ? "sem inventario" : (CallRefillEstus(Inventory) ? "recarregado" : "FALHOU");
        }

        uint32_t SoulsAfter = SoulsBefore;
        if (HaveParam)
        {
            ReadBytes(Param + kParamSouls, &SoulsAfter, sizeof(SoulsAfter));
        }
        Append(StringFormat("%s  custos da morte (papel %u, convidado %u): almas %u -> %u (%s); hollow %s; deste jogador: %s; manchas: %s; mancha online %s; estus %s\n",
            Clock().c_str(), Role, GuestKind, SoulsBefore, SoulsAfter, Souls.c_str(), Hollow.c_str(), Mine.c_str(),
            Bloodstain.c_str(), Online.c_str(), Estus.c_str()));
    }

    std::string DescribeParams(const uint8_t* Data);

    // The banner the death sequence would have shown for this role.
    void ShowDeathBanner(uint8_t* Chr)
    {
        uintptr_t Context = 0, Record = 0, Roles = 0, Row = 0;
        uint8_t Role = 0, GuestKind = 0, FeType = 0xff;
        const uintptr_t Front = FrontEnd();
        if (!ReadPointer(s_base + kContextOffset, Context) || !ReadPointer(Context + kBonfireRecord, Record) || Front == 0)
        {
            Append(StringFormat("%s  banner: sem registro ou front end\n", Clock().c_str()));
            return;
        }
        if (ReadPointer((uintptr_t)Chr + kChrRoles, Roles))
        {
            ReadBytes(Roles + kRole, &Role, 1);
        }
        ReadBytes(s_base + kRoleTableOffset + (Role < kRoleRows ? Role : 0) * kRoleRowSize + kRoleGuestKind, &GuestKind, 1);

        const int Type = GuestKind == 3 ? kDeathEventGuestKind3 : kDeathEvent;
        uint32_t Id = (uint32_t)((int8_t)Role + Type * 100);
        if (!CallParamRow(Record, Id, Row) || Row == 0)
        {
            Id = (uint32_t)(Type * 100 + 99);
            CallParamRow(Record, Id, Row);
        }
        int32_t Banner = -1;
        if (Row != 0 && ReadBytes(Row, &FeType, 1) && FeType < kFeTypes)
        {
            ReadBytes(s_base + kFeTableOffset + FeType * sizeof(int32_t), &Banner, sizeof(Banner));
        }
        if (Banner < 0 || Banner == kBannerNeedsCheck)
        {
            Append(StringFormat("%s  banner: linha %u tipo FE %u, nenhum banner (%d)\n", Clock().c_str(), Id, FeType, Banner));
            return;
        }

        const bool Shown = CallBanner(Front, (uint32_t)Banner);
        s_banner_wait.Active = Shown;
        s_banner_wait.Frames = 0;
        s_banner_wait.Banner = (uint32_t)Banner;
        Append(StringFormat("%s  banner: linha %u tipo FE %u -> banner %d %s\n", Clock().c_str(), Id, FeType, Banner,
            Shown ? "mostrado" : "FALHOU"));
    }

    // Once the front end is done, the HUD the banner hid comes back.
    void ContinueBanner()
    {
        ++s_banner_wait.Frames;
        if (s_banner_wait.Frames < kBannerMinFrames)
        {
            return;
        }
        const uintptr_t Front = FrontEnd();
        if (Front == 0)
        {
            s_banner_wait.Active = false;
            return;
        }
        bool Busy = false;
        const bool GiveUp = s_banner_wait.Frames >= kBannerGiveUpFrames;
        if (!GiveUp && CallCheck(s_front_end_busy, Front, Busy) && Busy)
        {
            return;
        }

        uintptr_t Hud = 0;
        int32_t Hidden = 0;
        const bool HaveHud = ReadPointer(Front + kFrontEndHud, Hud) && ReadBytes(Hud + kHudHidden, &Hidden, sizeof(Hidden));
        const bool Reset = HaveHud && Hidden != 0 && CallAction(s_hud_reset, Front);
        s_banner_wait.Active = false;
        Append(StringFormat("%s  banner %u acabou em %u quadros%s: HUD %s\n", Clock().c_str(), s_banner_wait.Banner,
            s_banner_wait.Frames, GiveUp ? " (desisti de esperar)" : "",
            !HaveHud ? "ilegivel" : (Hidden == 0 ? "ja visivel" : (Reset ? "devolvido" : "FALHOU"))));
    }

    // Somebody else's character, with its death pending on this machine.
    void RefuseRemoteDeath(void* Ctrl, uint8_t* Chr, uint8_t* Data)
    {
        const int32_t Hp = *(const int32_t*)(Chr + kHp);
        const int32_t Max = *(const int32_t*)(Chr + kHpMax);
        const std::string Params = DescribeParams(Data);

        Data[kPending] = 0;
        if (Hp < 1 && Max > 0)
        {
            // The owner's next update brings the real value.
            *(int32_t*)(Chr + kHp) = Max;
        }
        *(uint64_t*)(Data + kStateBits) &= ~kDyingBits;

        const uint64_t Count = ++s_remote_refused;
        const ULONGLONG Now = GetTickCount64();
        if (Now - s_remote_last_ms > 1000)
        {
            s_remote_streak = 0;
        }
        s_remote_last_ms = Now;
        const uint64_t InStreak = ++s_remote_streak;
        if (InStreak <= 10 || InStreak % 300 == 0)
        {
            uintptr_t Roles = 0;
            uint8_t Role = 0xff;
            if (ReadPointer((uintptr_t)Chr + kChrRoles, Roles))
            {
                ReadBytes(Roles + kRole, &Role, 1);
            }
            Append(StringFormat("%s  morte da copia RECUSADA #%llu (seguida %llu) controlador %p personagem %p tipo %u papel %u hp=%d -> %d %s\n",
                Clock().c_str(), (unsigned long long)Count, (unsigned long long)InStreak, Ctrl, Chr, Chr[kChrType], Role, Hp,
                *(const int32_t*)(Chr + kHp), Params.c_str()));
        }
    }

    // A fall that was refused still leaves the character in the air, in a
    // death volume, with the fall camera. Out of the air first; the flags only
    // once the fall controller agrees the character is down, or the next frame
    // in the air is another fall death.
    // A travel: the bonfire is named, there is no record to fall back on and
    // no death to pay for. The machine is the respawn's (StartRecovery and
    // what follows it), measured between Heide and Majula in both directions.
    void StartTravel(uint8_t* Chr, uint32_t Map, uint32_t Id)
    {
        if (s_parked)
        {
            s_parked = false;
            DS2_Backread::Unfocus();
        }
        s_park_spot_valid = false;
        if (s_recovery.Loading || s_settle.Active)
        {
            DS2_Backread::Unfocus();
            DS2_Backread::Release();
            s_settle.Active = false;
        }

        Recovery Next;
        Next.Active = true;
        Next.Why = "viagem";
        Next.KeepHp = true;
        s_travel_from = MapIndexUnder(Chr);
        // On a map object the contact carries no map index (kind 1 does,
        // anything else not): measured 19/09 leaving Eleum Loyce, -1 here
        // meant the map left could not be taken down and the host died on a
        // full TargetManager. The streamer's current map says it instead.
        if (s_travel_from < 0)
        {
            s_travel_from = DS2_Backread::IndexOf(CurrentMap());
        }
        uint64_t InUse = 0;
        uint8_t DestState = 0;
        uint32_t DestMask[4] = {};
        const bool DestIn = DS2_Backread::Query(Map, DestState, DestMask) && DestState == 5;
        const uint32_t Cost = DestIn ? 0 : DS2_Backread::TargetCost(Map);
        const bool Read = DS2_Backread::Targets(InUse);
        s_travel_tight = Read && InUse + Cost > DS2_Backread::TargetLimit();
        if (s_travel_tight)
        {
            DS2_Backread::DropKeeps();
            // Dropped unless this machine hosts the session with guests.
            DS2_CoopChannel::SendHostEvent(DS2_CoopChannel::HostEvent::TravelPark, Map, Id, 0);
            Append(StringFormat("%s  budget: %llu targets in use + %u for %08x > %llu; holds dropped, the map left is not held\n",
                Clock().c_str(), (unsigned long long)InUse, Cost, Map, (unsigned long long)DS2_Backread::TargetLimit()));
        }
        else if (s_travel_from >= 0)
        {
            DS2_Backread::KeepIndex(s_travel_from, kTravelHoldMs);
        }
        DS2_TravelWatch::Open(kWatchStartMs, "viagem comecou");

        // Only a bonfire of the map under the player is jumped to straight
        // away. A bonfire of another map may be in the list already - that
        // map is kept loaded for the other player - but with only the parts
        // around that player: measured 15/09, the guest landed at Heide's
        // bonfire with no ground under it and fell to its death. So any other
        // map goes through the hold and the focus, which bring its parts in.
        const uint32_t Here = CurrentMap();
        if (Map == Here && FindBonfireSpawn(Map, Id, Next.Target))
        {
            Next.Where = "fogueira da viagem";
        }
        else
        {
            uint8_t State = 0;
            uint32_t Mask[4] = {};
            const uintptr_t Fall = FallController(Chr);
            if (Map == Here || !DS2_Backread::Query(Map, State, Mask) ||
                Fall == 0 || !ReadBytes(Fall + kFallGrounded, Next.Target, sizeof(Next.Target)))
            {
                ++s_recovery_failed;
                s_go_moving.store(false);
                s_travel_outcome.store((uint8_t)DS2_DeathIntercept::Outcome::Failed);
                Append(StringFormat("%s  viagem: a fogueira %08x do mapa %08x nao esta na lista e o mapa nao pode ser trazido; nao viajei\n",
                    Clock().c_str(), Id, Map));
                return;
            }
            // Every part. A version of 16/09 asked for none, on the theory
            // that the force byte loads the map and the focus brings the
            // ground - six travels alone went through, so it looked right.
            // It was not: measured with two players, the owner sits at state 0
            // for ever with an empty mask (`estado 0 forcado 1 quer 1`, every
            // mask zero). **The force byte says "keep this map", not "load
            // it"; what loads a map is having a part asked for.** Alone it
            // only seemed to work because the player had just come from that
            // map and the streamer still wanted parts of it. The cost of the
            // mistake was quiet: the travel gave up after 30 s, wrote
            // "viagem concluido" anyway and left the character where it stood,
            // so a run of thirty legs looked clean while the guest had really
            // travelled five times.
            const uint32_t Every[4] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff };
            if (s_travel_tight)
            {
                Next.WaitRoom = true;
                Next.RoomSince = GetTickCount64();
                Next.RoomCost = Cost;
                Next.RoomSource = s_travel_from;
            }
            else
            {
                DS2_Backread::Request(Map, Every);
            }
            Next.Loading = true;
            Next.LoadMap = Map;
            Next.LoadId = Id;
            Next.Where = "ultima posicao no chao, esperando o mapa da viagem";
        }

        s_recovery = Next;
        const bool Moved = TeleportLocal(Chr, s_recovery.Target);
        Append(StringFormat("%s  viagem: levando para %s (%.3f, %.3f, %.3f) mapa=%08x id=%08x %s\n",
            Clock().c_str(), s_recovery.Where, s_recovery.Target[0], s_recovery.Target[1], s_recovery.Target[2],
            Map, Id, Moved ? "teleportado" : "TELEPORTE FALHOU"));
    }

    void StartRecovery(uint8_t* Chr, const char* Why)
    {
        Recovery Next;
        Next.Active = true;
        Next.Why = Why;

        uint32_t Map = 0, Id = 0;
        int32_t Type = 0;
        const bool HaveRecord = ReadRecord(Map, Type, Id);

        // A guest's own record holds a bonfire of its own world, and the game
        // writes none for it in the host's: it goes where the host would.
        std::string Host;
        const uint8_t Role = RoleOf(Chr);
        if (Role != kWorldOwnerRole && Enabled(FeatureHostBonfire))
        {
            DS2_CoopChannel::Bonfire Said;
            if (!DS2_CoopChannel::HostBonfire(Said))
            {
                Host = StringFormat("; papel %u, nenhum anuncio do host", Role);
            }
            else if (FindBonfireSpawn(Said.Map, Said.Id, Next.Target))
            {
                Next.Where = "fogueira do host";
                Map = Said.Map;
                Type = Said.Type;
                Id = Said.Id;
                Host = StringFormat("; papel %u, anunciada por %016llx ha %llu ms", Role,
                    (unsigned long long)Said.From, (unsigned long long)Said.AgeMs);
            }
            else
            {
                Host = StringFormat("; papel %u, a fogueira do host (mapa %08x id %08x) nao esta no mapa carregado",
                    Role, Said.Map, Said.Id);
            }
        }

        if (Next.Where[0] == '\0')
        {
            // A guest whose host announced a bonfire that is not loaded waits
            // for that bonfire's map; everyone else tries its own record first.
            DS2_CoopChannel::Bonfire Said;
            const bool FromHost = Role != kWorldOwnerRole && Enabled(FeatureHostBonfire) &&
                DS2_CoopChannel::HostBonfire(Said) && Said.Map != 0;
            const bool LoadHost = FromHost && Enabled(FeatureOtherMap);

            if (!LoadHost && HaveRecord && FindBonfireSpawn(Map, Id, Next.Target))
            {
                Next.Where = "fogueira do registro";
            }
            else
            {
                uint32_t Wanted = 0;
                if (LoadHost)
                {
                    Wanted = Said.Map;
                    Map = Said.Map;
                    Type = Said.Type;
                    Id = Said.Id;
                }
                else if (Enabled(FeatureOtherMap) && HaveRecord)
                {
                    Wanted = Map;
                }
                uint8_t OwnerState = 0;
                uint32_t OwnerMask[4] = {};
                const bool Known = Wanted != 0 && Wanted != CurrentMap() && DS2_Backread::Query(Wanted, OwnerState, OwnerMask);

                const uintptr_t Fall = FallController(Chr);
                if (Fall == 0 || !ReadBytes(Fall + kFallGrounded, Next.Target, sizeof(Next.Target)))
                {
                    ++s_recovery_failed;
                    Append(StringFormat("%s  %s: sem a fogueira %08x/%08x no mapa e sem a ultima posicao no chao; nada a fazer%s\n",
                        Clock().c_str(), Why, Map, Id, Host.c_str()));
                    return;
                }
                Next.Where = "ultima posicao no chao";
                if (Known)
                {
                    const uint32_t Every[4] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff };
                    DS2_Backread::Request(Wanted, Every);
                    Next.Loading = true;
                    Next.LoadMap = Wanted;
                    Next.LoadId = Id;
                    Next.Where = "ultima posicao no chao, esperando o mapa da fogueira";
                    s_settle.Active = false;
                }
            }
        }

        s_recovery = Next;
        const bool Moved = TeleportLocal(Chr, s_recovery.Target);
        Append(StringFormat("%s  %s: levando para %s (%.3f, %.3f, %.3f) mapa=%08x tipo=%d id=%08x %s%s\n",
            Clock().c_str(), Why, s_recovery.Where, s_recovery.Target[0], s_recovery.Target[1], s_recovery.Target[2],
            Map, Type, Id, Moved ? "teleportado" : "TELEPORTE FALHOU", Host.c_str()));
    }

    // The bonfire's map is coming in. Once it is loaded and the bonfire is in
    // the list, the character goes there; the map stays held until it stands.
    // True on the frame the character was sent.
    // Waiting for the target budget to have room for the destination.
    // True while still waiting.
    bool ContinueRoom(uint8_t* Chr)
    {
        const ULONGLONG Now = GetTickCount64();
        const ULONGLONG Waited = Now - s_recovery.RoomSince;
        uint64_t InUse = 0;
        const bool Read = DS2_Backread::Targets(InUse);
        const bool Fits = !Read || InUse + s_recovery.RoomCost <= DS2_Backread::TargetLimit();
        const bool SourceBusy = s_recovery.SourceAsked && !DS2_Backread::Unloaded(s_recovery.RoomSource);
        if (!Fits || SourceBusy)
        {
            s_recovery.RoomAt = 0;
        }
        else if (s_recovery.RoomAt == 0)
        {
            s_recovery.RoomAt = Now;
        }
        const bool Settled = s_recovery.RoomAt != 0 && Now - s_recovery.RoomAt >= kRoomSettleMs;
        if (Settled || Waited > kRoomGiveUpMs)
        {
            const uint32_t Every[4] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff };
            DS2_Backread::Request(s_recovery.LoadMap, Every);
            s_recovery.WaitRoom = false;
            s_recovery.LoadFrames = 0;
            Append(StringFormat("%s  budget: %s after %llu ms (%llu targets in use, %u for %08x); destination asked for\n",
                Clock().c_str(), Fits ? "room made" : "NO ROOM, loading anyway", (unsigned long long)Waited,
                (unsigned long long)InUse, s_recovery.RoomCost, s_recovery.LoadMap));
            return false;
        }
        if (!s_recovery.SourceAsked && Waited > kRoomHoldsMs && s_recovery.RoomSource >= 0)
        {
            s_recovery.SourceAsked = true;
            const uint32_t Source = DS2_Backread::MapAt(s_recovery.RoomSource);
            // The game never takes down the map under the player (measured
            // 19/09: 15 s of an unreachable, unforced Eleum Loyce and it
            // stayed at state 5), so the character goes somewhere else first:
            // a bonfire of another loaded map, the session's in practice.
            float Park[3] = {};
            uint32_t ParkMap = 0;
            const bool Parked = FindParking(Source, s_recovery.LoadMap, Park, ParkMap);
            if (Parked)
            {
                memcpy(s_park_spot, Park, sizeof(Park));
                s_park_spot_valid = true;
                s_park_map = ParkMap;
                memcpy(s_recovery.Target, Park, sizeof(Park));
                TeleportLocal(Chr, s_recovery.Target);
                // The streamer keeps the map of the last part the player
                // stood on; told the player stands here, it lets the map left
                // go (measured 19/09: teleported alone, Eleum Loyce stayed).
                DS2_Backread::Focus(ParkMap, Park);
            }
            DS2_Backread::Unload(s_recovery.RoomSource);
            Append(StringFormat("%s  budget: still %llu targets in use + %u; the map left (%08x [%d]) goes before the destination, %s\n",
                Clock().c_str(), (unsigned long long)InUse, s_recovery.RoomCost, Source, s_recovery.RoomSource,
                Parked ? StringFormat("the character waits at a bonfire of %08x", ParkMap).c_str()
                       : "with nowhere else to stand"));
        }
        return true;
    }

    bool ContinueLoading(uint8_t* Chr)
    {
        if (s_recovery.WaitRoom && ContinueRoom(Chr))
        {
            return false;
        }
        ++s_recovery.LoadFrames;
        if (s_recovery.LoadFrames % kLoadPollFrames != 0)
        {
            return false;
        }

        uint8_t State = 0;
        uint32_t Mask[4] = {};
        float Spawn[3] = {};
        const bool Loaded = DS2_Backread::Query(s_recovery.LoadMap, State, Mask) && State == kMapLoaded;
        if (Loaded && FindBonfireSpawn(s_recovery.LoadMap, s_recovery.LoadId, Spawn))
        {
            // Its ground comes from the nav cell of where the player stands;
            // in the air there is none, so the streamer is told the bonfire's.
            DS2_Backread::Focus(s_recovery.LoadMap, Spawn);
            memcpy(s_recovery.Target, Spawn, sizeof(Spawn));
            s_recovery.Where = "fogueira em outro mapa";
            s_recovery.Loading = false;
            s_recovery.Frames = 0;
            s_settle.Active = true;
            s_settle.Map = s_recovery.LoadMap;
            s_settle.Frames = 0;
            s_settle.Retries = 0;
            memcpy(s_settle.Target, Spawn, sizeof(Spawn));
            const bool Moved = TeleportLocal(Chr, s_recovery.Target);
            Append(StringFormat("%s  %s: o mapa %08x carregou em %u quadros; levando para a fogueira %08x (%.3f, %.3f, %.3f) %s\n",
                Clock().c_str(), s_recovery.Why, s_recovery.LoadMap, s_recovery.LoadFrames, s_recovery.LoadId,
                Spawn[0], Spawn[1], Spawn[2], Moved ? "teleportado" : "TELEPORTE FALHOU"));
            return true;
        }

        if (s_recovery.LoadFrames >= kLoadGiveUpFrames)
        {
            DS2_Backread::Unfocus();
            DS2_Backread::Release();
            s_recovery.Loading = false;
            s_recovery.Frames = 0;
            // The map never came. The recovery goes on to stand the character
            // up where it is, and **that** used to look like an arrival: the
            // completion below sets Arrived whenever no settle is running, so
            // on 16/09 the host announced "cheguei na fogueira" after thirty
            // seconds of not loading anything, and the group was called into a
            // map nobody was in. A travel that never left is a failure.
            if (s_go_moving.load())
            {
                s_travel_outcome.store((uint8_t)DS2_DeathIntercept::Outcome::Failed);
            }
            Append(StringFormat("%s  %s: %u quadros e o mapa %08x nao trouxe a fogueira %08x (estado %u); fica na ultima posicao no chao\n",
                Clock().c_str(), s_recovery.Why, s_recovery.LoadFrames, s_recovery.LoadMap, s_recovery.LoadId, State));
        }
        return false;
    }

    // Holding the map the character jumped to until the streamer has it under
    // the character's feet, so letting go unloads only the map left behind.
    void ContinueSettle(uint8_t* Chr)
    {
        ++s_settle.Frames;
        const uint32_t Current = CurrentMap();
        const bool Arrived = StandsOn(Chr, s_settle.Map);
        if (!Arrived && s_settle.Frames < kSettleGiveUpFrames)
        {
            // Standing, but on another map's ground.
            if (Current != 0 && s_settle.Frames % kSettleRetryFrames == 0)
            {
                ++s_settle.Retries;
                const std::string Where = DescribeFooting(Chr);
                const bool Moved = TeleportLocal(Chr, s_settle.Target);
                Append(StringFormat("%s  pisando no mapa %08x e nao no %08x (%s); de novo para a fogueira (%u) %s\n",
                    Clock().c_str(), Current, s_settle.Map, Where.c_str(), s_settle.Retries, Moved ? "teleportado" : "TELEPORTE FALHOU"));
            }
            return;
        }
        DS2_Backread::Unfocus();
        DS2_Backread::Release();
        // The map arrived at is held by the travel too, now that the contact
        // under the character says which index it is (right after the jump it
        // still answers the map left behind).
        const int32_t Landed = MapIndexUnder(Chr);
        if (Arrived && Landed >= 0)
        {
            DS2_Backread::KeepIndex(Landed, kTravelHoldMs);
        }
        s_settle.Active = false;
        s_travel_outcome.store((uint8_t)(Arrived ? DS2_DeathIntercept::Outcome::Arrived
                                                 : DS2_DeathIntercept::Outcome::Failed));
        Append(StringFormat("%s  mapa %08x solto depois de %u quadros: %s (%s)\n", Clock().c_str(), s_settle.Map, s_settle.Frames,
            Arrived ? "o personagem esta nele" : StringFormat("desisti, o mapa atual e %08x", Current).c_str(),
            DescribeFooting(Chr).c_str()));
    }

    // A guest told to wait at the session's map while the host makes room.
    void ContinuePark(uint8_t* Chr)
    {
        if (!s_park_pending.exchange(false))
        {
            return;
        }
        float Park[3] = {};
        uint32_t ParkMap = 0;
        const uint32_t Here = CurrentMap();
        if (!FindParking(Here, 0, Park, ParkMap))
        {
            Append(StringFormat("%s  park: no bonfire of another loaded map; staying in %08x\n", Clock().c_str(), Here));
            return;
        }
        const bool Moved = TeleportLocal(Chr, Park);
        DS2_Backread::Focus(ParkMap, Park);
        s_parked = true;
        Append(StringFormat("%s  park: left %08x for a bonfire of %08x (%.3f, %.3f, %.3f) %s, while the host makes room\n",
            Clock().c_str(), Here, ParkMap, Park[0], Park[1], Park[2], Moved ? "teleportado" : "TELEPORTE FALHOU"));
    }

    void ContinueRecovery(uint8_t* Chr, uint8_t* Data)
    {
        // Not a frame to ask whether the character is down: the fall controller
        // has not run since it was sent, and still answers for the place it
        // left. Measured 14/09, the recovery ended on that answer, and the fall
        // that followed was billed as a death of its own.
        if (s_recovery.Loading && ContinueLoading(Chr))
        {
            return;
        }
        ++s_recovery.Frames;

        const uintptr_t Fall = FallController(Chr);
        uint8_t InAir = 1;
        // On a travel to another map, "down" counts only on that map's ground.
        // Measured 18/09 21:29: sent to Threshold Bridge, the recovery ended one
        // frame after the jump on the contact it still had in Majula, the
        // bridge had no floor yet, and the fall 0.8 s later was billed as a
        // real death (hollowing, respawn in Majula). Kept open, that fall is
        // the travel's own, and the retry below puts the character back.
        const bool OnTarget = !s_settle.Active || StandsOn(Chr, s_settle.Map);
        const bool Down = OnTarget && Fall != 0 && ReadBytes(Fall + kFallInAir, &InAir, 1) && InAir == 0;
        if (!Down)
        {
            const uint32_t GiveUp = s_settle.Active ? kOtherMapGiveUpFrames : kRecoveryGiveUpFrames;
            if (s_recovery.Frames >= GiveUp && !s_recovery.Loading)
            {
                ++s_recovery_failed;
                s_recovery.Active = false;
                if (s_go_moving.load())
                {
                    s_travel_outcome.store((uint8_t)DS2_DeathIntercept::Outcome::Failed);
                }
                s_go_moving.store(false);
                Append(StringFormat("%s  %s: %u quadros e o personagem nao pousou; desisto\n",
                    Clock().c_str(), s_recovery.Why, s_recovery.Frames));
            }
            else if (s_recovery.Frames % kRecoveryRetryFrames == 0)
            {
                TeleportLocal(Chr, s_recovery.Target);
            }
            return;
        }

        if (s_recovery.Loading)
        {
            return;
        }

        // Down, but the landing may have zeroed the HP on this very frame: the
        // HP source runs before this and the pending byte is checked after, so
        // ending here would make that death look new, and it was billed as one
        // (18/09 22:24 and 22:36, Threshold Bridge: hollowing and a respawn in
        // Majula right after "cheguei"). The cancel below still sees the
        // recovery and treats it as the same death; the next frame ends it.
        int32_t HpNow = 0;
        if (Data[kPending] != 0 || (ReadBytes((uintptr_t)Chr + kHp, &HpNow, sizeof(HpNow)) && HpNow <= 0))
        {
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

        // Last, so the maximum already carries this death's hollowing. A
        // travel pays nothing and heals nothing.
        int32_t Hp = 0, Max = 0;
        if (!s_recovery.KeepHp && ReadBytes((uintptr_t)Chr + kHpMax, &Max, sizeof(Max)) && Max > 0)
        {
            ReadBytes((uintptr_t)Chr + kHp, &Hp, sizeof(Hp));
            WriteBytes((uintptr_t)Chr + kHp, &Max, sizeof(Max));
        }

        ++s_recovered;
        s_recovery.Active = false;
        if (s_go_moving.exchange(false))
        {
            s_travel_done_ms = GetTickCount64();
            memcpy(s_travel_target, s_recovery.Target, sizeof(s_travel_target));
            // A bonfire of the map already under the character has no settle
            // to run, so this is the arrival. Every other travel is judged by
            // ContinueSettle, on the physics contact.
            // Only when nothing has decided yet: a give-up above has already
            // written Failed, and a settle still running will decide later.
            uint8_t Expected = (uint8_t)DS2_DeathIntercept::Outcome::Moving;
            if (!s_settle.Active)
            {
                s_travel_outcome.compare_exchange_strong(Expected, (uint8_t)DS2_DeathIntercept::Outcome::Arrived);
            }
            if (s_travel_from >= 0 && !s_travel_tight)
            {
                DS2_Backread::KeepIndex(s_travel_from, kTravelHoldMs);
            }
            DS2_TravelWatch::Open(kWatchLandedMs, "viagem chegou");
            Append(StringFormat("%s  viagem: o mapa de indice %d de onde sai fica %u ms; o de chegada e segurado quando o personagem pisar nele\n",
                Clock().c_str(), s_travel_from, kTravelHoldMs));
            s_travel_from = -1;
        }
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

    // Somebody else's controller moving. In a session this is the question of
    // step 6: whether the other machine's copy of a player who never died here
    // dies anyway, from what it receives.
    void NoteRemoteTransition(void* Ctrl, void* Character, uint8_t Before, uint8_t After)
    {
        const uint64_t Count = ++s_remote_transitions;
        if (Count > 400)
        {
            return;
        }
        uintptr_t Vftable = 0, Roles = 0;
        uint8_t Role = 0xff, Pending = 0;
        int32_t Hp = 0;
        uintptr_t Data = 0;
        ReadPointer((uintptr_t)Character, Vftable);
        if (ReadPointer((uintptr_t)Character + kChrRoles, Roles))
        {
            ReadBytes(Roles + kRole, &Role, 1);
        }
        ReadBytes((uintptr_t)Character + kHp, &Hp, sizeof(Hp));
        if (ReadPointer((uintptr_t)Character + kCharacterData, Data))
        {
            ReadBytes(Data + kPending, &Pending, 1);
        }
        uint8_t Type = 0xff;
        ReadBytes((uintptr_t)Character + kChrType, &Type, 1);
        Append(StringFormat("%s  outro controlador %p personagem %p (vftable +0x%zx, tipo %u, papel %u) estado %u -> %u hp=%d +0x759=%u\n",
            Clock().c_str(), Ctrl, Character, Vftable != 0 ? (size_t)(Vftable - s_base) : 0, Type, Role, Before, After, Hp, Pending));
    }

    // Could this word be an address at all? Anything with a bit set above bit
    // 47 could not have come from the allocator.
    bool AddressShaped(uintptr_t Value)
    {
        return Value >= 0x10000 && (Value >> 47) == 0;
    }

    // Drops the character's Havok body if it is provably not there any more.
    // True when one was dropped.
    bool DropDeadRigidBody(uint8_t* Chr)
    {
        uintptr_t Physics = 0, Body = 0, Havok = 0, Vftable = 0, Inner = 0;
        if (!ReadPointer((uintptr_t)Chr + kChrPhysics, Physics) ||
            !ReadPointer(Physics + kPhysicsRigidBody, Body) ||
            !ReadPointer(Body, Vftable) || Vftable != s_base + kPxRigidBodyVftable)
        {
            return false;   // not the shape this knows about: leave it alone
        }
        uint8_t Raw[sizeof(uintptr_t)] = {};
        if (!ReadBytes(Body + kRigidBodyHavok, Raw, sizeof(Raw)))
        {
            return false;
        }
        memcpy(&Havok, Raw, sizeof(Havok));
        if (Havok == 0)
        {
            return false;   // already the state the game itself leaves behind
        }

        // Three ways of being gone, in the order they cost: the pointer is not
        // an address; what it points at has no vftable of this module; or the
        // field the animation chain reads through holds something that is not
        // an address either. The third is the one that catches a block already
        // handed out to somebody else, because such a block can perfectly well
        // carry a believable vftable again.
        const char* Why = nullptr;
        uintptr_t HavokVftable = 0;
        if (!AddressShaped(Havok))
        {
            Why = "o ponteiro nao e um endereco";
        }
        else if (!ReadPointer(Havok, HavokVftable) || HavokVftable < s_base ||
                 HavokVftable >= s_base + 0x2000000 || (HavokVftable & 7) != 0)
        {
            Why = "o corpo nao tem vftable do jogo";
        }
        else if (ReadPointer(Havok + kHavokInner, Inner) && Inner != 0 && !AddressShaped(Inner))
        {
            Why = "o +0x18 do corpo guarda algo que nao e endereco";
        }
        if (Why == nullptr)
        {
            return false;
        }

        const uintptr_t Zero = 0;
        if (!WriteBytes(Body + kRigidBodyHavok, &Zero, sizeof(Zero)))
        {
            return false;
        }
        if (++s_bodies_dropped <= 40)
        {
            Append(StringFormat("%s  corpo rigido morto largado: personagem %p, PXCharacterRigidBody %p, corpo %016llx (%s); o jogo trata o nulo\n",
                Clock().c_str(), (void*)Chr, (void*)Body, (unsigned long long)Havok, Why));
        }
        return true;
    }

    void UpdateHook(void* Ctrl, float Delta)
    {
        uint8_t* Bytes = (uint8_t*)Ctrl;
        void* Character = *(void**)(Bytes + kCtrlCharacter);

        // Every player character, local or another player's copy: the host
        // died on its own and the guest on the copy, so both are checked.
        {
            uintptr_t ChrVftable = 0;
            if (Character != nullptr && ReadPointer((uintptr_t)Character, ChrVftable) &&
                ChrVftable == s_base + kPlayerCtrlVftable)
            {
                DropDeadRigidBody((uint8_t*)Character);
            }
        }
        if (Character == nullptr || Character != LocalCharacter())
        {
            // The map another player stands in must not unload under its
            // copy here, whatever this machine's player does.
            // Held even while this machine travels: letting the other
            // player's map go during a travel killed the guest inside the
            // CharacterManager (15/09, c0000005 at +0x3f4fac, a character
            // already freed - the same crash the host's warp used to give).
            if (Character != nullptr && Enabled(FeatureOtherMap) &&
                *(const uintptr_t*)Character == s_base + kPlayerCtrlVftable &&
                ((const uint8_t*)Character)[kChrType] == kRemotePlayerCopy)
            {
                int32_t Index = -1;
                uint32_t Parts[4] = {};
                // While a map is being taken down for a travel, a copy that
                // already stands by the parking bonfire does not hold it: its
                // contact can go on naming the map it left (measured 19/09,
                // leaving Eleum Loyce for Brume Tower, the map stayed kept for
                // 27 s with both players waiting in Majula).
                //
                // Nor is a map that is not loaded here: a copy cannot stand
                // on it, and keeping it loads it. Measured 19/09: 25 ms after
                // Eleum Loyce reached state 0, the copy's stale contact kept
                // index 36, the game started loading it again, and the host
                // died on a worker thread (+0x833655) 45 ms later.
                const int32_t Going = DS2_Backread::Unloading();
                float Where[3] = {};
                bool AtPark = false;
                if (s_park_spot_valid && ReadBytes((uintptr_t)Character + 0x90, Where, sizeof(Where)))
                {
                    const float Dx = Where[0] - s_park_spot[0], Dy = Where[1] - s_park_spot[1], Dz = Where[2] - s_park_spot[2];
                    AtPark = Dx * Dx + Dy * Dy + Dz * Dz <= kParkNear * kParkNear;
                }
                const int32_t Under = MapIndexUnder((uint8_t*)Character);
                uint8_t UnderState = 0;
                uint32_t UnderMask[4] = {};
                const uint32_t UnderMap = Under >= 0 ? DS2_Backread::MapAt(Under) : 0;
                const bool UnderLoaded = UnderMap != 0 && DS2_Backread::Query(UnderMap, UnderState, UnderMask) && UnderState != 0;
                if (Under >= 0 && !UnderLoaded)
                {
                    // Not loaded here; nothing to hold.
                }
                else if (AtPark && Under >= 0 && (Under == Going || DS2_Backread::IndexOf(s_park_map) != Under))
                {
                    if (s_keep_skipped++ % 300 == 0)
                    {
                        Append(StringFormat("%s  park: the other player's copy waits by the parking bonfire; map [%d] is not held for it\n",
                            Clock().c_str(), Going));
                    }
                }
                else if (PartsUnder((uint8_t*)Character, Index, Parts))
                {
                    DS2_Backread::KeepIndex(Index, kKeepOtherPlayerMs, Parts);
                }
                else if ((Index = MapIndexUnder((uint8_t*)Character)) >= 0)
                {
                    DS2_Backread::KeepIndex(Index, kKeepOtherPlayerMs);
                }
            }

            // Another player's copy, with a death pending: the same three
            // tests as for the local player, and only for PlayerCtrl.
            const int RemoteMode = s_mode.load();
            if (Character != nullptr && (RemoteMode == Cancel || RemoteMode == Respawn) && Enabled(FeatureRemote) &&
                Bytes[kCtrlState] == 0 && *(const uintptr_t*)Character == s_base + kPlayerCtrlVftable)
            {
                uint8_t* RemoteData = *(uint8_t**)((uint8_t*)Character + kCharacterData);
                if (RemoteData != nullptr && *(const int32_t*)(RemoteData + kDeferred) == 0 && RemoteData[kPending] != 0)
                {
                    RefuseRemoteDeath(Ctrl, (uint8_t*)Character, RemoteData);
                    return;
                }
            }

            const uint8_t StateBefore = Bytes[kCtrlState];
            s_original_update(Ctrl, Delta);
            const uint8_t StateAfter = Bytes[kCtrlState];
            if (Character != nullptr && StateAfter != StateBefore)
            {
                NoteRemoteTransition(Ctrl, Character, StateBefore, StateAfter);
            }
            return;
        }

        uint8_t* Chr = (uint8_t*)Character;
        uint8_t* Data = *(uint8_t**)(Chr + kCharacterData);
        const uint8_t Before = Bytes[kCtrlState];
        PublishLocal(Chr);
        const bool NewController = Ctrl != s_local_ctrl;

        if (Ctrl != s_local_ctrl)
        {
            s_local_ctrl = Ctrl;
            s_local_state = Before;
            // A new local controller means the game rebuilt the world (a
            // warp, a return home, a load). Nothing the travel left may carry
            // over. Measured 19/09: a guest parked at Majula's bonfire for a
            // travel, the host died, and the guest went home to Heide with the
            // streamer still focused on Majula's bonfire - characters and
            // signs floating in a grey void, Heide's ground never built; an
            // `unfocus` drew it at once. The focus lives in the injector, so no
            // world rebuild resets it.
            DS2_Backread::Unfocus();
            DS2_Backread::Release();
            // Keeps and a pending unload are by owner index, and a load builds
            // a new streamer whose indices mean other maps.
            DS2_Backread::DropKeeps();
            DS2_Backread::CancelUnload();
            s_parked = false;
            s_park_pending.store(false);
            s_park_spot_valid = false;
            s_recovery = Recovery();
            s_settle.Active = false;
            s_banner_wait.Active = false;
            Append(StringFormat("%s  controlador do jogador local %p, personagem %p, estado %u\n",
                Clock().c_str(), Ctrl, Character, Before));
        }

        // Joining a host in another map lands the guest on its own sign
        // converted into the host's map - for Majula into Heide, empty space
        // (measured 15/09, M3). The host's bonfire is known by then, so a guest
        // arriving from another map goes to it at once, without waiting to
        // fall; a fall was the only rescue before, and a point inside the
        // ground would not have fallen.
        {
            const uint8_t RoleNow = RoleOf(Chr);
            if (RoleNow == kWorldOwnerRole)
            {
                const uint32_t Map = CurrentMap();
                if (Map != 0)
                {
                    s_owner_map = Map;
                }
                s_arrival_frames = 0;
            }
            else if (s_last_local_role == kWorldOwnerRole && s_owner_map != 0 && Enabled(FeatureHostBonfire))
            {
                // The controller survives the join (measured 15/09: no new
                // local controller across it), so the role change is the sign.
                s_arrival_frames = 1;
            }
            s_last_local_role = RoleNow;
            (void)NewController;

            if (s_arrival_frames > 0 && RoleNow != kWorldOwnerRole && !s_recovery.Active && Data != nullptr)
            {
                ++s_arrival_frames;
                const uint32_t Map = CurrentMap();
                DS2_CoopChannel::Bonfire Said;
                const bool Announced = DS2_CoopChannel::HostBonfire(Said) && Said.Map != 0;
                if (Announced && Said.Map == s_owner_map)
                {
                    s_arrival_frames = 0;   // the host is in the map the guest came from: nothing converted
                }
                else if (Announced && (Map == Said.Map || Map == 0) && s_arrival_frames > kArrivalSettleFrames)
                {
                    // The landing point is empty space, so there is no part
                    // under the guest and the streamer says map 0 (measured
                    // 15/09): no footing and a host elsewhere is the arrival.
                    s_arrival_frames = 0;
                    Append(StringFormat("%s  chegada de outro mapa: vim de %08x, o host esta em %08x, sob os pes %08x\n",
                        Clock().c_str(), s_owner_map, Said.Map, Map));
                    StartRecovery(Chr, "chegada de outro mapa");
                }
                else if (s_arrival_frames > kArrivalGiveUpFrames)
                {
                    s_arrival_frames = 0;
                    Append(StringFormat("%s  chegada de outro mapa: sem anuncio da fogueira do host no mapa %08x em %u quadros; fica onde chegou\n",
                        Clock().c_str(), Map, kArrivalGiveUpFrames));
                }
            }
        }

        if (!s_recovery.Active)
        {
            ContinuePark(Chr);
        }

        if (s_go_pending.load() && Data != nullptr && !s_recovery.Active)
        {
            s_go_pending.store(false);
            StartTravel(Chr, s_go_map.load(), s_go_id.load());
        }

        if (s_recovery.Active && Data != nullptr)
        {
            ContinueRecovery(Chr, Data);
        }
        if (s_banner_wait.Active)
        {
            ContinueBanner();
        }
        if (s_settle.Active)
        {
            ContinueSettle(Chr);
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
                const bool AfterTravel = NewDeath && Fell && s_travel_done_ms != 0 &&
                    GetTickCount64() - s_travel_done_ms < kAfterTravelMs;
                if (Mode == Respawn && NewDeath && !AfterTravel)
                {
                    ApplyDeathCosts(Chr, Data);
                    if (Enabled(FeatureBanner))
                    {
                        ShowDeathBanner(Chr);
                    }
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

                if (AfterTravel)
                {
                    Recovery Next;
                    Next.Active = true;
                    Next.Why = "queda depois da viagem";
                    Next.Where = "destino da viagem";
                    memcpy(Next.Target, s_travel_target, sizeof(Next.Target));
                    s_recovery = Next;
                    const bool Moved = TeleportLocal(Chr, s_recovery.Target);
                    Append(StringFormat("%s  fall %llu ms after the travel ended: the travel's, not a death; back to (%.3f, %.3f, %.3f) %s\n",
                        Clock().c_str(), (unsigned long long)(GetTickCount64() - s_travel_done_ms),
                        s_travel_target[0], s_travel_target[1], s_travel_target[2], Moved ? "teleportado" : "TELEPORTE FALHOU"));
                    s_travel_done_ms = 0;
                }
                else if (NewDeath && (Mode == Respawn || Fell))
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
        else if (Verb == "feature")
        {
            std::string Name, State;
            Parts >> Name >> State;
            bool Known = false;
            for (const auto& Entry : kFeatures)
            {
                if (Name == Entry.Name && (State == "on" || State == "off"))
                {
                    Known = true;
                    if (State == "on")
                    {
                        s_features.fetch_or(Entry.Bit);
                    }
                    else
                    {
                        s_features.fetch_and(~Entry.Bit);
                    }
                }
            }
            Append(StringFormat("%s  === %s: %s ===\n", Clock().c_str(),
                Known ? "cobranca" : "nao entendi", Line.c_str()));
        }
        else if (Verb == "status")
        {
            const int Mode = s_mode.load();
            std::string Features;
            for (const auto& Entry : kFeatures)
            {
                Features += StringFormat(" %s=%d", Entry.Name, Enabled(Entry.Bit) ? 1 : 0);
            }
            Append(StringFormat("%s  === modo %s: vistas=%llu canceladas=%llu renascimentos=%llu recuperacoes=%llu recuperacoes_falhas=%llu sem_+0x759=%llu instantaneas=%llu chamadas_slot_+0x10=%llu outros_controladores=%llu copias_recusadas=%llu;%s ===\n",
                Clock().c_str(), Mode == Respawn ? "renascer" : (Mode == Cancel ? "cancelar" : "observar"),
                (unsigned long long)s_seen.load(), (unsigned long long)s_cancelled.load(),
                (unsigned long long)s_respawns.load(),
                (unsigned long long)s_recovered.load(), (unsigned long long)s_recovery_failed.load(),
                (unsigned long long)s_unexplained.load(), (unsigned long long)s_instant.load(),
                (unsigned long long)s_replica_calls.load(), (unsigned long long)s_remote_transitions.load(),
                (unsigned long long)s_remote_refused.load(), Features.c_str()));
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
        { kHollowStateOffset, kHollowStateBytes, sizeof(kHollowStateBytes), "estado de hollow" },
        { kNoPenaltyOffset, kNoPenaltyBytes, sizeof(kNoPenaltyBytes), "morte sem penalidade" },
        { kNotAPlayerOffset, kNotAPlayerBytes, sizeof(kNotAPlayerBytes), "personagem especial" },
        { kRefillEstusOffset, kRefillEstusBytes, sizeof(kRefillEstusBytes), "estus" },
        { kRoleParamOffset, kRoleParamBytes, sizeof(kRoleParamBytes), "param do papel" },
        { kHollowExemptOffset, kHollowExemptBytes, sizeof(kHollowExemptBytes), "checagem ofuscada do hollow" },
        { kLocalBranchOffset, kLocalBranchBytes, sizeof(kLocalBranchBytes), "ramo do jogador desta maquina" },
        { kDeathCounterOffset, kDeathCounterBytes, sizeof(kDeathCounterBytes), "contador de mortes" },
        { kRingBreakOffset, kRingBreakBytes, sizeof(kRingBreakBytes), "anel de protecao" },
        { kMenuOpenOffset, kMenuOpenBytes, sizeof(kMenuOpenBytes), "menu aberto" },
        { kMenuCloseOffset, kMenuCloseBytes, sizeof(kMenuCloseBytes), "fechar menu" },
        { kOnlineStainOffset, kOnlineStainBytes, sizeof(kOnlineStainBytes), "mancha online" },
        { kOnlineStatueOffset, kOnlineStatueBytes, sizeof(kOnlineStatueBytes), "mancha online de estatua" },
        { kParamRowOffset, kParamRowBytes, sizeof(kParamRowBytes), "linha do param da morte" },
        { kBannerOffset, kBannerBytes, sizeof(kBannerBytes), "banner" },
        { kFrontEndBusyOffset, kFrontEndBusyBytes, sizeof(kFrontEndBusyBytes), "front end ocupado" },
        { kHudResetOffset, kHudResetBytes, sizeof(kHudResetBytes), "devolver o HUD" },
        { kPartUnderOffset, kPartUnderBytes, sizeof(kPartUnderBytes), "parte sob um personagem" },
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
    s_hollow_state = (HollowState_p)(s_base + kHollowStateOffset);
    s_no_penalty = (Check_p)(s_base + kNoPenaltyOffset);
    s_not_a_player = (Check_p)(s_base + kNotAPlayerOffset);
    s_refill_estus = (RefillEstus_p)(s_base + kRefillEstusOffset);
    s_role_param = (RoleParam_p)(s_base + kRoleParamOffset);
    s_hollow_exempt = (Check_p)(s_base + kHollowExemptOffset);
    s_local_branch = (Check_p)(s_base + kLocalBranchOffset);
    s_death_counter = (Action_p)(s_base + kDeathCounterOffset);
    s_ring_break = (Global_p)(s_base + kRingBreakOffset);
    s_menu_open = (Check_p)(s_base + kMenuOpenOffset);
    s_menu_close = (Action_p)(s_base + kMenuCloseOffset);
    s_online_stain = (Action_p)(s_base + kOnlineStainOffset);
    s_online_statue = (Action_p)(s_base + kOnlineStatueOffset);
    s_param_row = (ParamRow_p)(s_base + kParamRowOffset);
    s_banner = (Banner_p)(s_base + kBannerOffset);
    s_front_end_busy = (Check_p)(s_base + kFrontEndBusyOffset);
    s_hud_reset = (Action_p)(s_base + kHudResetOffset);
    s_part_under = (PartUnder_p)(s_base + kPartUnderOffset);

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

namespace DS2_DeathIntercept
{
    bool MapReachable(uint32_t Map)
    {
#if defined(_WIN32) && defined(_M_X64)
        if (Map == 0)
        {
            return false;
        }
        if (Map == CurrentMap())
        {
            return true;
        }
        uint8_t State = 0;
        uint32_t Mask[4] = {};
        return DS2_Backread::Query(Map, State, Mask);
#else
        (void)Map;
        return false;
#endif
    }

    void GoToBonfire(uint32_t Map, uint32_t Id)
    {
#if defined(_WIN32) && defined(_M_X64)
        s_travel_outcome.store((uint8_t)Outcome::Moving);
        s_go_map.store(Map);
        s_go_id.store(Id);
        s_go_moving.store(true);
        s_go_pending.store(true);
#else
        (void)Map;
        (void)Id;
#endif
    }

    void Park()
    {
#if defined(_WIN32) && defined(_M_X64)
        s_park_pending.store(true);
#endif
    }

    bool Moving()
    {
#if defined(_WIN32) && defined(_M_X64)
        return s_travel_outcome.load() == (uint8_t)Outcome::Moving;
#else
        return false;
#endif
    }

    Outcome TravelOutcome()
    {
#if defined(_WIN32) && defined(_M_X64)
        return (Outcome)s_travel_outcome.load();
#else
        return Outcome::Idle;
#endif
    }
}
