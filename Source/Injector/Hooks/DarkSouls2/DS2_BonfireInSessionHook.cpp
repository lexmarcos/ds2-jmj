/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_BonfireInSessionHook.h"
#include "Injector/Hooks/DarkSouls2/DS2_CoopChannelHook.h"
#include "Injector/Hooks/DarkSouls2/DS2_DeathInterceptHook.h"
#include "Injector/Hooks/DarkSouls2/DS2_RespawnInSessionHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "Shared/Core/Utils/Strings.h"

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

    // The rest, host side (measured 15/09, docs/DS2_SEAMLESS_COOP_TASKS.md M8):
    //
    //   +0x17dc40  FUN_14017dc40(EventBonfireManager, bonfire id): starts the
    //              rest, returns 1 when it did (state 0 -> 1)
    //   +0x17fd70  FUN_14017fd70(): the world reset of a rest, run on state
    //              1 -> 2; enemy generators (FUN_140417210), map objects
    //              (FUN_1403c27f0) and the event manager (FUN_14044f880). Takes
    //              nothing, reads the globals.
    //
    // Measured without this: an enemy killed in the host's world came back on
    // the host when it rested and stayed dead on the guest.
    constexpr size_t kRestStartOffset = 0x17dc40;
    constexpr uint8_t kRestStartPrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x83, 0x79, 0x38, 0x00, 0x48, 0x8b, 0xd9 };
    constexpr size_t kWorldResetOffset = 0x17fd70;
    constexpr uint8_t kWorldResetPrologue[] = { 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x05, 0x75, 0x4b, 0x49, 0x01, 0x48, 0x8b, 0x48, 0x40 };

    // A message box with text of our own, the way FUN_1402d6540 shows the
    // network errors: FUN_1404fe2a0(*(ctx+0x22e0), text, title, 1, 1), the
    // title from FUN_140503620(0, 0xcc).
    constexpr size_t kDialogOffset = 0x4fe2a0;
    constexpr uint8_t kDialogPrologue[] = { 0x40, 0x53, 0x48, 0x81, 0xec, 0xb0, 0x00, 0x00, 0x00, 0x0f, 0xb6, 0x84, 0x24, 0xe0 };
    constexpr size_t kTextOffset = 0x503620;
    constexpr uint8_t kTextPrologue[] = { 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x41, 0x56 };
    constexpr size_t kFrontEnd = 0x22e0;
    constexpr int kTitleCategory = 0;
    constexpr int kTitleId = 0xcc;
    constexpr const wchar_t* kTravelDeclined = L"Travel canceled: a player declined.";
    constexpr const wchar_t* kTravelNoAnswer = L"Travel canceled: not every player answered.";
    constexpr const wchar_t* kTravelBusy = L"Travel canceled: another travel vote is running.";

    // Names for the travel question, from the game's own text: bonfires in
    // category 0x12 by bonfire id (FUN_1400d5800, the travel list), areas in
    // category 5 by area id (FUN_14002f630), the area id being the map's
    // first two numbers: 0x0a1f0000 (m10_31) is 10310000, read from the travel
    // list's own entries on 15/09.
    constexpr int kBonfireNames = 0x12;
    constexpr int kAreaNames = 5;

    // The bonfire table, EventBonfireManager = *(*(ctx+0x70)+0x58):
    //   FUN_14017c110(&index, id) + FUN_14017c230(&index, &map): a bonfire's map
    //   FUN_14017e6f0(manager, id): lit in the world the player stands in
    //   *(manager+8) the loaded MapObjBonfireComponents, next at +0x60, entity at +0x08
    constexpr size_t kBonfireIndexOffset = 0x17c110;
    constexpr uint8_t kBonfireIndexPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x8b, 0x05, 0xd4, 0x87, 0x49, 0x01, 0x48, 0x8b, 0xd9 };
    constexpr size_t kBonfireMapOffset = 0x17c230;
    constexpr uint8_t kBonfireMapPrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x05, 0xb3, 0x86, 0x49, 0x01, 0x48, 0x8b, 0xda };
    constexpr size_t kBonfireLitOffset = 0x17e6f0;
    constexpr uint8_t kBonfireLitPrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8, 0xc2, 0x0a, 0x00, 0x00 };
    // The bonfire table itself: a pointer at +0x20, the count at +0x28, one
    // entry every 0x18 bytes with the id first (ushort) and one lit byte per
    // world after it; the world to read is the manager's own +0x44 (0 at
    // home, 1 in someone else's world). Measured 15/09 with the two games
    // side by side: the guest's column had only the bonfires of the map it
    // had loaded in the host's world, which is why its travel list was short.
    constexpr size_t kBonfireTable = 0x20;
    constexpr size_t kBonfireCount = 0x28;
    constexpr size_t kBonfireColumn = 0x44;
    constexpr size_t kBonfireEntry = 0x18;
    constexpr size_t kBonfireLitByte = 0x02;
    constexpr ULONGLONG kLitEveryMs = 1000;

    // The bonfire menu's own cancel, the one the game uses to close it when a
    // session comes up mid-rest: FUN_1401994e0(*(*(ctx+0x70)+0x50)), which
    // needs the menu queue in state 10 and leaves the job to stand the
    // character up. Measured 15/09 live: the travel list closed in under a
    // second and the session stayed verified.
    constexpr size_t kMenuCancelOffset = 0x1994e0;
    constexpr uint8_t kMenuCancelPrologue[] = { 0x48, 0x8b, 0x05, 0x09, 0xb4, 0x47, 0x01, 0x48, 0x83, 0xb8, 0xe0, 0x22, 0x00, 0x00, 0x00 };
    // Time for the character to stand up before it is taken anywhere.
    constexpr ULONGLONG kStandUpMs = 2000;

    // The curtain the game's own travel puts up while it loads, FUN_140483250:
    // `ctx+0x1178` (which stops the action prompts and the death timer),
    // FUN_140b06270(*(0x1416751f8)+0x80, 1) (the world's drawing off), the
    // HUD hidden (FUN_1404ffef0(frontend, 0xffdffbff), slot +0x40 of every
    // HUD group) and the loading screen itself opened (FUN_1405014b0: event
    // 0x67 to the front-end object 0x4c5c574, the black screen with the area's
    // name). FUN_140482d50 takes it down in the reverse order: FUN_1404ffde0
    // (frontend, mask) shows the HUD, FUN_1404ff310 sends the loading screen
    // 0x65, FUN_1404fe920 drops the count. With only the first two (15/09)
    // the guest saw the sky's clear colour, the map's pieces coming in and
    // the whole HUD (screenshots of 16/09); the loading screen is what makes
    // it black.
    constexpr size_t kCurtainOffset = 0xb06270;
    constexpr uint8_t kCurtainPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x80, 0x79, 0x08, 0x00 };
    constexpr size_t kRenderGlobal = 0x16751f8;
    constexpr size_t kRenderSwitch = 0x80;
    constexpr size_t kLoadingFlag = 0x1178;
    constexpr size_t kLoadingOpenOffset = 0x5014b0;
    constexpr uint8_t kLoadingOpenPrologue[] = { 0x48, 0x8b, 0x89, 0xf0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x0f, 0x85, 0x10, 0x13, 0xb5, 0xff };
    constexpr size_t kLoadingCloseOffset = 0x4ff310;
    constexpr uint8_t kLoadingClosePrologue[] = { 0x48, 0x8b, 0x89, 0xf0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x0f, 0x85, 0xf0, 0x2d, 0xb5, 0xff };
    constexpr size_t kHudHideOffset = 0x4ffef0;
    constexpr uint8_t kHudHidePrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20 };
    constexpr size_t kHudShowOffset = 0x4ffde0;
    constexpr uint8_t kHudShowPrologue[] = { 0x48, 0x89, 0x6c, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9, 0x0f, 0x03, 0x00, 0x00, 0x00 };
    constexpr size_t kHudDropOffset = 0x4fe920;
    constexpr uint8_t kHudDropPrologue[] = { 0x80, 0xb9, 0x0f, 0x03, 0x00, 0x00, 0x00, 0x75, 0x12, 0x8b, 0x81, 0x1c, 0x03, 0x00, 0x00 };
    constexpr uint32_t kHudMask = 0xffdffbff;
    // Never leave the screen black: the curtain comes down anyway after this.
    constexpr ULONGLONG kCurtainGiveUpMs = 25000;
    // A moment more after arriving, so the map left behind goes away behind it.
    constexpr ULONGLONG kCurtainHoldMs = 1200;

    constexpr size_t kBonfireManager = 0x58;
    constexpr size_t kBonfireList = 0x08;
    constexpr size_t kBonfireNext = 0x60;
    constexpr size_t kComponentEntity = 0x08;
    constexpr size_t kEntityPosition = 0x70;

    // A travel the game itself starts, the way FUN_14017fdb0 does once a
    // bonfire is picked: FUN_1401843b0(&request, id, 2) builds it,
    // FUN_140184830(*(*(ctx+0x70)+0x70), &request) starts it, and
    // FUN_14044fe30(*(ctx+0x70), {map, 0, spawn}) makes it the respawn point.
    constexpr size_t kTravelBuildOffset = 0x1843b0;
    constexpr uint8_t kTravelBuildPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x60 };
    constexpr size_t kTravelStartOffset = 0x184830;
    constexpr uint8_t kTravelStartPrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x60, 0x8b, 0x02, 0x48, 0x8b, 0xd9, 0x89, 0x01 };
    constexpr size_t kRecordSetOffset = 0x44fe30;
    constexpr uint8_t kRecordSetPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x60, 0x83, 0x7a, 0x04, 0x01 };
    constexpr size_t kTravelObject = 0x70;
    constexpr size_t kTravelBonfireOffset = 0xd4eb0;   // FUN_1400d4eb0(list): the bonfire id under the cursor
    constexpr uint8_t kTravelBonfirePrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8, 0x82, 0xd2, 0xf4, 0xff };

    // A guest never gets "Rest at bonfire": an event script asks query 130602
    // (FUN_140513440, "in a session as a guest") and stops there - measured
    // 15/09 with the esd spy, the guest's script evaluated nothing else. The
    // script is a plain EventEzStateCtrl evaluated by FUN_14045c6a0, with no
    // pointer to the bonfire in it, so the answer is "no" when the local
    // player is a white phantom standing within 3 m of a loaded bonfire.
    constexpr size_t kInnerScriptOffset = 0x45c6a0;
    constexpr uint8_t kInnerScriptPrologue[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24 };
    constexpr int32_t kQueryIsGuest = 130602;
    constexpr size_t kCharacterPosition = 0x90;
    constexpr float kNearBonfire = 3.0f;

    // The prompt itself is refused in FUN_140453ce0, the event action entries:
    // an entry of type 13 or 14 is dropped while the context says the player
    // is in someone else's world (ctx vftable +0x58, FUN_1405135f0), before
    // the distance is even checked. Type 14 is "Rest at bonfire": the guest's
    // entry at a lit bonfire was 0x0e, and with the gate's `jne` gone that
    // entry became the prompt, the guest sat and healed 400 -> 914 (15/09).
    // Type 13 is left behind the gate (most likely lighting an unlit bonfire,
    // not measured): `sub eax,0xd ; cmp eax,1 ; ja` becomes `cmp eax,0`, so
    // only 13 reaches the gate. Patched only while the local player is a
    // white phantom, so an invader still gets nothing.
    //
    //   +0x453db0  mov eax,[rbx+0x8c] ; sub eax,0xd ; cmp eax,<1> ; ja +0x453dde
    //   +0x453dd1  call FUN_1405135f0 ; test al,al ; jne +0x45401c
    constexpr size_t kPromptGate = 0x453dbb;
    constexpr uint8_t kPromptGateExpected[] = { 0x01 };
    constexpr uint8_t kPromptGatePatch[] = { 0x00 };
    constexpr size_t kPromptGateBeforeAt = 0x453db0;
    constexpr uint8_t kPromptGateBefore[] = { 0x8b, 0x83, 0x8c, 0x00, 0x00, 0x00, 0x83, 0xe8, 0x0d, 0x83, 0xf8 };
    constexpr size_t kPromptGateAfterAt = 0x453dbc;
    constexpr uint8_t kPromptGateAfter[] = { 0x77, 0x20 };
    constexpr size_t kPromptGateCallAt = 0x453dd1;
    constexpr uint8_t kPromptGateCall[] = { 0xe8, 0x1a, 0xf8, 0x0b, 0x00, 0x84, 0xc0, 0x0f, 0x85, 0x3e, 0x02, 0x00, 0x00 };

    // A Yes/No box the way FeSubStateCommonWindow opens one (FUN_140104db0):
    // FUN_1404fe1c0(frontend, text, yes, no, 1, 1, 1, 1) returns its number
    // (+0x324); FUN_140500440(frontend, n) says it closed, FUN_1404ff940
    // (frontend, n) which button (2 and 5 are the second, "No"), and
    // FUN_1404ff2e0 / FUN_1404fe960 (frontend, 0) put it away.
    constexpr size_t kChoiceOffset = 0x4fe1c0;
    constexpr uint8_t kChoicePrologue[] = { 0x40, 0x53, 0x48, 0x81, 0xec, 0xc0, 0x00, 0x00, 0x00, 0x0f, 0xb6, 0x84, 0x24, 0x08, 0x01 };
    constexpr size_t kClosedOffset = 0x500440;
    constexpr size_t kButtonOffset = 0x4ff940;
    constexpr uint8_t kByNumberPrologue[] = { 0x48, 0x8b, 0x81, 0xf0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x14, 0x85, 0xd2, 0x7e, 0x08 };
    constexpr size_t kCloseOffset = 0x4ff2e0;
    constexpr size_t kReleaseOffset = 0x4fe960;
    constexpr uint8_t kCloseByNumberPrologue[] = { 0x48, 0x8b, 0xc1, 0x48, 0x8b, 0x89, 0xf0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x11, 0x85, 0xd2 };
    constexpr size_t kDialogNumber = 0x324;
    constexpr int kYesText = 100;
    constexpr int kNoText = 0x65;
    constexpr ULONGLONG kVoteTimeoutMs = 30000;
    // Picking a bonfire in the travel list: FeGroupTestBonfireTransitionList
    // (vftable 0x1410ba868) slot +0x80, FUN_1400d5170(list). It writes the
    // destination into the bonfire job (+0x68, mark +0x67), and from there the
    // character plays the travel animation, the load starts and the warp is
    // asked for - a road whose only way out is the load: holding the warp, and
    // then the travel's phase 1 (FUN_140184a10), both left the host frozen in
    // the travel pose (15/09). So the vote happens before the pick is let
    // through, with the list still open.
    constexpr size_t kPickOffset = 0xd5170;
    constexpr uint8_t kPickPrologue[] = { 0x40, 0x56, 0x48, 0x83, 0xec, 0x60, 0x48, 0x8b, 0x05, 0xd3, 0xca, 0x50, 0x01 };
    constexpr size_t kTravelListVftable = 0x10ba868;
    constexpr size_t kPickSlot = 0x80;
    constexpr size_t kEventManager = 0x70;
    constexpr size_t kQueueState = 0x54;
    constexpr int32_t kQueueBonfireMenu = 10;
    constexpr size_t kMenuQueue = 0x50;
    constexpr ULONGLONG kLeaveSettleMs = 1500;
    constexpr ULONGLONG kLeaveGiveUpMs = 20000;
    constexpr const wchar_t* kTravelStuck = L"Travel canceled: a player could not leave the session.";

    // The guest's session, NetSummonJoinMultiplayCtrl (vftable 0x1410d7bd8),
    // playing in state 7 (+0xf8). A nonzero +0x120 makes its state-7 handler
    // (FUN_1402c3830) end the session with reason 3 on the next frame: the
    // same end the guest got when a host travelled on 15/09 (from +0x2c385c),
    // armed 1 -> 0 with the penalty points unchanged.
    constexpr size_t kJoinCtrlVftable = 0x10d7bd8;
    constexpr size_t kJoinState = 0xf8;
    constexpr int32_t kJoinPlaying = 7;
    constexpr size_t kJoinLeave = 0x120;

    // The local character's role: *(*0x1416148f0 + 0xd0) -> +0xb0 -> +0x3c.
    constexpr size_t kGameGlobal = 0x16148f0;
    constexpr size_t kLocalCharacter = 0xd0;
    constexpr size_t kRoles = 0xb0;
    constexpr size_t kRole = 0x3c;
    constexpr uint8_t kRoleOwner = 0;
    constexpr uint8_t kRoleWhitePhantom = 1;

    // Why a travel vote ended without travelling, as TravelCanceled carries it.
    enum class Cancel : uint32_t
    {
        Declined = 1,
        NoAnswer = 2,
        NotLit = 3,
        Busy = 4,
        Stuck = 5,
    };

    using SessionUp_p = uint64_t(*)(void* Session);
    SessionUp_p s_original = nullptr;
    using RestStart_p = uint64_t(*)(void* Manager, int32_t Bonfire);
    RestStart_p s_original_rest = nullptr;
    using WorldReset_p = void(*)();
    WorldReset_p s_original_reset = nullptr;
    using Dialog_p = uint32_t(*)(void* FrontEnd, const wchar_t* Text, const wchar_t* Title, uint8_t A, uint8_t B);
    Dialog_p s_dialog = nullptr;
    using Text_p = const wchar_t*(*)(int Category, int Id);
    Text_p s_text = nullptr;
    bool s_replaying = false;   // game thread only
    using Choice_p = int32_t(*)(void* FrontEnd, const wchar_t* Text, const wchar_t* Yes, const wchar_t* No, uint8_t A, uint8_t B, uint8_t C, uint8_t D);
    using ByNumber_p = uint64_t(*)(void* FrontEnd, int32_t Number);
    using CloseByNumber_p = void(*)(void* FrontEnd, int32_t Number);
    Choice_p s_choice = nullptr;
    ByNumber_p s_closed = nullptr;
    ByNumber_p s_button = nullptr;
    CloseByNumber_p s_close = nullptr;
    CloseByNumber_p s_release = nullptr;
    bool s_votes_ready = false;
    using Pick_p = void(*)(void* List);
    Pick_p s_original_pick = nullptr;
    using BonfireIndex_p = uint32_t*(*)(uint32_t* Out, uint16_t Id);
    using BonfireMap_p = uint32_t*(*)(uint32_t* Index, uint32_t* Out);
    using BonfireLit_p = uint8_t(*)(void* Manager, uint32_t Id);
    using TravelBuild_p = void*(*)(uint8_t* Request, uint16_t Id, uint32_t Reason);
    using TravelStart_p = void(*)(void* Travel, uint8_t* Request);
    using RecordSet_p = void(*)(void* Record, int32_t* Fields);
    using TravelBonfire_p = uint16_t(*)(void* List);
    using MenuCancel_p = void(*)(void* Queue);
    using Curtain_p = void(*)(void* Switch, char On);
    using FrontEndOnly_p = void(*)(void* FrontEnd);
    using FrontEndMask_p = void(*)(void* FrontEnd, uint32_t Mask);
    FrontEndOnly_p s_loading_open = nullptr;
    FrontEndOnly_p s_loading_close = nullptr;
    FrontEndMask_p s_hud_hide = nullptr;
    FrontEndMask_p s_hud_show = nullptr;
    FrontEndOnly_p s_hud_drop = nullptr;
    bool s_loading_screen = false;   // the loading screen is up, by us
    using Script_p = uint64_t(*)(void* This, uint32_t* Out, void** Arguments, void* P4);
    BonfireIndex_p s_bonfire_index = nullptr;
    BonfireMap_p s_bonfire_map = nullptr;
    BonfireLit_p s_bonfire_lit = nullptr;
    TravelBuild_p s_travel_build = nullptr;
    TravelStart_p s_travel_start = nullptr;
    RecordSet_p s_record_set = nullptr;
    TravelBonfire_p s_travel_bonfire = nullptr;
    MenuCancel_p s_menu_cancel = nullptr;
    Curtain_p s_curtain = nullptr;
    bool s_curtain_up = false;
    ULONGLONG s_curtain_since = 0;
    ULONGLONG s_curtain_down_at = 0;
    Script_p s_original_inner_script = nullptr;
    bool s_guest_rest_ready = false;
    std::atomic<uint64_t> s_prompts_opened{ 0 };

    // Host side, game thread only.
    struct HeldTravel
    {
        bool Active = false;
        bool Pass = false;
        void* List = nullptr;          // the host's own list, when the host picked
        uint64_t Proposer = 0;         // the guest who picked, otherwise
        uint16_t Bonfire = 0;
        uint32_t Map = 0;
        bool HostAnswered = false;
        bool HostYes = false;
        uint32_t Vote = 0;
        size_t Guests = 0;
        ULONGLONG Since = 0;
        bool Leaving = false;
        ULONGLONG LeaveSince = 0;
        ULONGLONG GuestsGone = 0;
    };
    HeldTravel s_travel;
    uint32_t s_vote_counter = 0;

    // The travel itself, once everyone agreed: each machine takes its own
    // player to the bonfire without a warp, so nobody leaves the session.
    struct Go
    {
        bool Active = false;
        uint32_t Map = 0;
        uint16_t Bonfire = 0;
        ULONGLONG At = 0;
    };
    Go s_go;
    // Travelling without leaving the session is off by default (16/09): it
    // works on the host and solo, and on a guest it still closes the game
    // seconds after arriving - memory of the map heap freed while live
    // components point at it (docs/DS2_SEAMLESS_COOP_TASKS.md, M8). Until that
    // is understood the vote falls back to the shape that was measured stable:
    // the guests leave the session legally, the host travels with the game's
    // own travel, and the party puts everyone back together. `DS2_Bonfire.req`
    // takes `junta liga` / `junta desliga` to try the seamless one.
    bool s_together = false;
    // The host travels first and calls the guests only once it is standing in
    // the new map: both machines bringing a map in at the same instant closed
    // both games twice (15/09), once inside the CharacterManager and once on
    // the frame the old map was let go.
    struct CallGuests
    {
        bool Active = false;
        uint32_t Map = 0;
        uint16_t Bonfire = 0;
        ULONGLONG Ready = 0;
        ULONGLONG Since = 0;
    };
    CallGuests s_call;
    constexpr ULONGLONG kSettledMs = 1500;
    constexpr ULONGLONG kCallGiveUpMs = 40000;
    ULONGLONG s_job_unpatched_at = 0;
    constexpr ULONGLONG kJobUnpatchMs = 1500;
    ULONGLONG s_lit_tick = 0;
    uint8_t s_lit_applied_count = 0;
    uint32_t s_lit_applied[3] = {};

    // Guest side, game thread only.
    struct OpenVote
    {
        bool Active = false;
        bool Host = false;
        uint32_t Vote = 0;
        int32_t Number = 0;
    };
    OpenVote s_open_vote;

    // Guest side, game thread only: what this guest proposed and answered.
    struct Proposal
    {
        bool Active = false;
        uint16_t Bonfire = 0;
        ULONGLONG Since = 0;
    };
    Proposal s_proposal;
    uint32_t s_answered_vote = 0;
    bool s_answered_yes = false;

    // Text handed to the game's message boxes; they are kept, not copied.
    wchar_t s_question[512] = {};
    wchar_t s_message[512] = {};
    std::atomic<bool> s_events_ready{ false };
    std::filesystem::path s_log_path;
    std::filesystem::path s_request_path;
    ULONGLONG s_request_tick = 0;
    std::mutex s_log_mutex;
    uintptr_t s_base = 0;
    bool s_job_patched = false;
    bool s_prompt_patched = false;
    bool s_prompt_gate_broken = false;
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

    void Append(const std::string& Text);
    void ShowMessage(const wchar_t* Text);
    void WorldResetHook();
    bool WriteCode(uintptr_t Address, const uint8_t* From, size_t Length);

    // 0xff without a local character.
    uint8_t LocalRole()
    {
        uintptr_t Context = 0, Character = 0, Roles = 0;
        uint8_t Role = 0xff;
        if (ReadPointer(s_base + kGameGlobal, Context) && Context != 0 &&
            ReadPointer(Context + kLocalCharacter, Character) && Character != 0 &&
            ReadPointer(Character + kRoles, Roles) && Roles != 0 &&
            ReadByte(Roles + kRole, Role))
        {
            return Role;
        }
        return 0xff;
    }

    bool OwnsTheWorld()
    {
        return LocalRole() == kRoleOwner;
    }

    bool IsWhitePhantom()
    {
        return LocalRole() == kRoleWhitePhantom;
    }

    uintptr_t BonfireManager()
    {
        uintptr_t Context = 0, Events = 0, Manager = 0;
        if (ReadPointer(s_base + kGameGlobal, Context) && Context != 0 &&
            ReadPointer(Context + kEventManager, Events) && Events != 0 &&
            ReadPointer(Events + kBonfireManager, Manager))
        {
            return Manager;
        }
        return 0;
    }

    // 0xffffffff when the bonfire is not in the table.
    uint32_t MapOfBonfire(uint16_t Id)
    {
        if (s_bonfire_index == nullptr || BonfireManager() == 0)
        {
            return 0xffffffff;
        }
        uint32_t Index = 0, Map = 0xffffffff;
        s_bonfire_index(&Index, Id);
        s_bonfire_map(&Index, &Map);
        return Map;
    }

    const wchar_t* TextOrEmpty(int Category, int Id)
    {
        const wchar_t* Text = s_text != nullptr ? s_text(Category, Id) : nullptr;
        __try
        {
            if (Text == nullptr || Text[0] == 0 || Text[0] > 0xffff || Text[0] == L'?')
            {
                return L"";
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return L"";
        }
        return Text;
    }

    // "Heide's Ruin (Heide's Tower of Flame)", or a plain "another bonfire".
    std::wstring PlaceName(uint16_t Bonfire, uint32_t Map)
    {
        const wchar_t* Name = TextOrEmpty(kBonfireNames, Bonfire);
        const int Area = Map == 0xffffffff || Map == 0 ? 0 : (int)(((Map >> 24) & 0xff) * 1000000 + ((Map >> 16) & 0xff) * 10000);
        const wchar_t* AreaName = Area != 0 ? TextOrEmpty(kAreaNames, Area) : L"";
        std::wstring Out = Name[0] != 0 ? Name : L"another bonfire";
        if (AreaName[0] != 0)
        {
            Out += L" (";
            Out += AreaName;
            Out += L")";
        }
        return Out;
    }

    std::string Narrow(const std::wstring& Text)
    {
        std::string Out;
        for (wchar_t C : Text)
        {
            Out += C < 0x80 ? (char)C : '?';
        }
        return Out;
    }

    bool HostHasLit(uint16_t Bonfire)
    {
        const uintptr_t Manager = BonfireManager();
        return s_bonfire_lit != nullptr && Manager != 0 && (s_bonfire_lit((void*)Manager, Bonfire) & 1) != 0;
    }

    // The bonfire table: where it is, how many entries, and which lit column
    // this machine reads.
    // The table is written into, so it is checked before it is believed: the
    // count in range, the column one of the two, and the ids of every entry
    // strictly ascending and nonzero, which is what the game's own binary
    // search over this table needs. A manager caught half built would fail
    // here instead of sending 77 byte writes into the heap.
    bool BonfireTable(uintptr_t& Table, uint32_t& Count, uint8_t& Column)
    {
        const uintptr_t Manager = BonfireManager();
        uint8_t Col = 0;
        uint32_t Many = 0;
        uintptr_t At = 0;
        if (Manager == 0 || !ReadPointer(Manager + kBonfireTable, At) || At == 0 ||
            !ReadByte(Manager + kBonfireColumn, Col) || Col > 3)
        {
            return false;
        }
        memcpy(&Many, (const void*)(Manager + kBonfireCount), sizeof(Many));
        if (Many < 8 || Many > DS2_CoopChannel::kMaxLitBonfires)
        {
            return false;
        }
        uint16_t Last = 0;
        for (uint32_t i = 0; i < Many; ++i)
        {
            uint16_t Id = 0;
            memcpy(&Id, (const void*)(At + i * kBonfireEntry), sizeof(Id));
            if (Id == 0 || Id <= Last)
            {
                return false;
            }
            Last = Id;
        }
        Table = At;
        Count = Many;
        Column = Col;
        return true;
    }

    // The host says which bonfires it has lit, so a guest's travel list is
    // the host's world and not the two maps it happens to have loaded.
    void PublishLitBonfires()
    {
        uintptr_t Table = 0;
        uint32_t Count = 0;
        uint8_t Column = 0;
        if (!BonfireTable(Table, Count, Column))
        {
            return;
        }
        uint32_t Bits[3] = {};
        for (uint32_t i = 0; i < Count; ++i)
        {
            uint8_t Lit = 0;
            if (ReadByte(Table + i * kBonfireEntry + kBonfireLitByte + Column, Lit) && (Lit & 1) != 0)
            {
                Bits[i / 32] |= 1u << (i % 32);
            }
        }
        DS2_CoopChannel::PublishLit((uint8_t)Count, Bits);
    }

    void ApplyHostLitBonfires()
    {
        uint8_t Count = 0;
        uint32_t Bits[3] = {};
        uint64_t AgeMs = 0;
        if (!DS2_CoopChannel::HostLit(Count, Bits, AgeMs))
        {
            return;
        }
        uintptr_t Table = 0;
        uint32_t Mine = 0;
        uint8_t Column = 0;
        // Never while a map is coming in: that is when a half built manager
        // could be read, and there is nothing to gain from the hurry.
        if (DS2_DeathIntercept::Moving() || !BonfireTable(Table, Mine, Column) || Column == 0 || Mine != Count)
        {
            return;
        }
        uint32_t Written = 0;
        for (uint32_t i = 0; i < Count; ++i)
        {
            const uint8_t Want = (Bits[i / 32] >> (i % 32)) & 1;
            const uintptr_t At = Table + i * kBonfireEntry + kBonfireLitByte + Column;
            uint8_t Now = 0;
            if (ReadByte(At, Now) && (Now & 1) != Want)
            {
                const uint8_t Next = (uint8_t)((Now & ~1u) | Want);
                memcpy((void*)At, &Next, 1);
                ++Written;
            }
        }
        const bool Changed = Count != s_lit_applied_count || memcmp(Bits, s_lit_applied, sizeof(Bits)) != 0;
        if (Written != 0 || Changed)
        {
            s_lit_applied_count = Count;
            memcpy(s_lit_applied, Bits, sizeof(Bits));
            Append(StringFormat("convidado: %u fogueira(s) da tabela alinhada(s) com o mundo do host (%u entradas, coluna %u, ha %llu ms)\n",
                Written, Count, (unsigned)Column, (unsigned long long)AgeMs));
        }
    }

    // Is a bonfire menu open here (the queue in state 10)?
    bool BonfireMenuOpen()
    {
        uintptr_t Context = 0, Events = 0, Queue = 0;
        int32_t State = 0;
        if (!ReadPointer(s_base + kGameGlobal, Context) || Context == 0 ||
            !ReadPointer(Context + kEventManager, Events) || Events == 0 ||
            !ReadPointer(Events + kMenuQueue, Queue) || Queue == 0)
        {
            return false;
        }
        memcpy(&State, (const void*)(Queue + kQueueState), sizeof(State));
        return State == kQueueBonfireMenu;
    }

    // The bonfire menu is closed by the game, not by us: calling
    // FUN_1401994e0 from this tick closed it on a host and killed a guest
    // twice (15/09, c0000005 writing to 0 inside the menu teardown, on the
    // frame of the call). What the game itself does, and what was measured
    // live, is the rest job cancelling the menu because a session is up: so
    // the job's branch patch comes off for a moment and the job does it.
    // True when there was a menu to close.
    bool CloseBonfireMenu()
    {
        if (!BonfireMenuOpen())
        {
            return false;
        }
        if (s_job_patched && WriteCode(s_base + kJobBranch, kJobExpected, sizeof(kJobExpected)))
        {
            s_job_patched = false;
            s_job_unpatched_at = GetTickCount64();
            Append("menu da fogueira: a trava do job sai por um instante para o jogo fechar o menu\n");
        }
        return true;
    }

    // Put the job's branch back once the menu is gone (or after a second).
    void KeepJobPatch(ULONGLONG Now)
    {
        if (s_job_patched || s_job_unpatched_at == 0)
        {
            return;
        }
        if (BonfireMenuOpen() && Now - s_job_unpatched_at < kJobUnpatchMs)
        {
            return;
        }
        if (WriteCode(s_base + kJobBranch, kJobPatch, sizeof(kJobPatch)))
        {
            s_job_patched = true;
            Append(StringFormat("menu da fogueira: trava do job de volta depois de %llu ms\n",
                (unsigned long long)(Now - s_job_unpatched_at)));
        }
        s_job_unpatched_at = 0;
    }

    // Whether the local character stands within a few metres of a loaded bonfire.
    bool NearBonfire()
    {
        const uintptr_t Manager = BonfireManager();
        uintptr_t Context = 0, Character = 0, Component = 0;
        if (Manager == 0 || !ReadPointer(s_base + kGameGlobal, Context) || Context == 0 ||
            !ReadPointer(Context + kLocalCharacter, Character) || Character == 0 ||
            !ReadPointer(Manager + kBonfireList, Component))
        {
            return false;
        }
        float Me[3] = {};
        memcpy(Me, (const void*)(Character + kCharacterPosition), sizeof(Me));
        for (int Guard = 0; Component != 0 && Guard < 64; ++Guard)
        {
            uintptr_t Entity = 0;
            if (!ReadPointer(Component + kComponentEntity, Entity))
            {
                return false;
            }
            if (Entity != 0)
            {
                float At[3] = {};
                memcpy(At, (const void*)(Entity + kEntityPosition), sizeof(At));
                const float Dx = At[0] - Me[0], Dy = At[1] - Me[1], Dz = At[2] - Me[2];
                if (Dx * Dx + Dy * Dy + Dz * Dz <= kNearBonfire * kNearBonfire)
                {
                    return true;
                }
            }
            if (!ReadPointer(Component + kBonfireNext, Component))
            {
                return false;
            }
        }
        return false;
    }

    // No C++ objects here: __try cannot sit in a function that unwinds.
    bool AnswerNotGuest(uint32_t* Out, void** Arguments)
    {
        __try
        {
            if (Out != nullptr && Arguments != nullptr && Out[0] != 0)
            {
                using Id_p = int32_t(*)(void*);
                const int32_t Id = ((Id_p)((*(void***)Arguments)[1]))(Arguments);
                if (Id == kQueryIsGuest && IsWhitePhantom() && NearBonfire())
                {
                    Out[0] = 0;
                    return true;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return false;
    }

    uint64_t InnerScriptHook(void* This, uint32_t* Out, void** Arguments, void* P4)
    {
        const uint64_t Result = s_original_inner_script(This, Out, Arguments, P4);
        if (AnswerNotGuest(Out, Arguments) && s_prompts_opened.fetch_add(1) == 0)
        {
            Append("convidado: perto da fogueira, a pergunta 'sou convidado?' do script respondeu nao\n");
        }
        return Result;
    }

    // The loading curtain, up and down. True when it moved.
    bool Curtain(bool Up)
    {
        uintptr_t Context = 0, Render = 0, Switch = 0;
        if (s_curtain == nullptr || !ReadPointer(s_base + kGameGlobal, Context) || Context == 0 ||
            !ReadPointer(s_base + kRenderGlobal, Render) || Render == 0 ||
            !ReadPointer(Render + kRenderSwitch, Switch) || Switch == 0)
        {
            return false;
        }
        const uint8_t Flag = Up ? 1 : 0;
        uintptr_t FrontEnd = 0;
        const bool Screen = s_loading_open != nullptr && ReadPointer(Context + kFrontEnd, FrontEnd) && FrontEnd != 0;
        if (Up)
        {
            memcpy((void*)(Context + kLoadingFlag), &Flag, 1);
            s_curtain((void*)Switch, 1);
            if (Screen && !s_loading_screen)
            {
                s_hud_hide((void*)FrontEnd, kHudMask);
                s_loading_open((void*)FrontEnd);
                s_loading_screen = true;
            }
        }
        else
        {
            if (Screen && s_loading_screen)
            {
                s_hud_show((void*)FrontEnd, kHudMask);
                s_loading_close((void*)FrontEnd);
                s_hud_drop((void*)FrontEnd);
            }
            s_loading_screen = false;
            s_curtain((void*)Switch, 0);
            memcpy((void*)(Context + kLoadingFlag), &Flag, 1);
        }
        s_curtain_up = Up;
        s_curtain_since = GetTickCount64();
        Append(Up ? StringFormat("tela de carregamento: subiu%s\n", Screen ? " (com a tela do jogo)" : " (so o desenho do mundo)")
                  : "tela de carregamento: desceu\n");
        return true;
    }

    // Called every frame: the curtain comes down once this machine's player
    // has arrived and stood still for a moment, and always before 25 s.
    void KeepCurtain(ULONGLONG Now)
    {
        if (!s_curtain_up)
        {
            return;
        }
        const bool Arrived = !s_go.Active && !DS2_DeathIntercept::Moving();
        if (!Arrived)
        {
            s_curtain_down_at = 0;
            if (Now - s_curtain_since > kCurtainGiveUpMs)
            {
                Append("tela de carregamento: a viagem nao terminou a tempo; desco assim mesmo\n");
                Curtain(false);
            }
            return;
        }
        if (s_curtain_down_at == 0)
        {
            s_curtain_down_at = Now + kCurtainHoldMs;
        }
        else if (Now >= s_curtain_down_at)
        {
            s_curtain_down_at = 0;
            Curtain(false);
        }
    }

    // The host's respawn record, moved to the bonfire everyone travels to,
    // the way the game's own travel does it: the request the travel builds
    // carries the map at +0x08 and the spawn point at +0x18.
    void SetRecord(uint16_t Bonfire)
    {
        uintptr_t Context = 0, Events = 0;
        if (s_travel_build == nullptr || s_record_set == nullptr ||
            !ReadPointer(s_base + kGameGlobal, Context) || Context == 0 ||
            !ReadPointer(Context + kEventManager, Events) || Events == 0 || MapOfBonfire(Bonfire) == 0xffffffff)
        {
            return;
        }
        uint8_t Request[0x40] = {};
        s_travel_build(Request, Bonfire, 2);
        int32_t Fields[3] = {};
        memcpy(&Fields[0], Request + 0x08, 4);
        memcpy(&Fields[2], Request + 0x18, 4);
        s_record_set((void*)Events, Fields);
        Append(StringFormat("registro de renascimento na fogueira %04x (mapa %08x, ponto %08x)\n", (unsigned)Bonfire,
            (uint32_t)Fields[0], (uint32_t)Fields[2]));
    }

    // Close whatever bonfire menu is open here and take this machine's player
    // to the bonfire once it is on its feet.
    void StartGo(uint32_t Map, uint16_t Bonfire)
    {
        const bool Closed = CloseBonfireMenu();
        if (!s_curtain_up)
        {
            Curtain(true);
        }
        s_go = Go();
        s_go.Active = true;
        s_go.Map = Map;
        s_go.Bonfire = Bonfire;
        // The host goes first and the guests a moment later: two machines
        // forcing a map in at the same instant is the shape the two crashes
        // of 15/09 had (docs/DS2_SEAMLESS_COOP_TASKS.md, M8).
        s_go.At = GetTickCount64() + (Closed || !OwnsTheWorld() ? kStandUpMs : 0);
        (void)s_menu_cancel;
        Append(StringFormat("viagem para a fogueira %04x (mapa %08x)%s\n", (unsigned)Bonfire, Map,
            Closed ? "; menu da fogueira fechado, esperando levantar" : ""));
    }

    // The host's own travel, started by the game's functions. False when the
    // bonfire is not in the table.
    bool StartTravel(uint16_t Bonfire)
    {
        uintptr_t Context = 0, Events = 0, Travel = 0;
        if (s_travel_build == nullptr || !ReadPointer(s_base + kGameGlobal, Context) || Context == 0 ||
            !ReadPointer(Context + kEventManager, Events) || Events == 0 ||
            !ReadPointer(Events + kTravelObject, Travel) || Travel == 0 || MapOfBonfire(Bonfire) == 0xffffffff)
        {
            return false;
        }
        uint8_t Request[0x40] = {};
        s_travel_build(Request, Bonfire, 2);
        int32_t Fields[3] = {};
        memcpy(&Fields[0], Request + 0x08, 4);
        memcpy(&Fields[2], Request + 0x18, 4);
        s_travel_start((void*)Travel, Request);
        s_record_set((void*)Events, Fields);
        Append(StringFormat("host: viagem iniciada para a fogueira %04x (mapa %08x, ponto %08x)\n", (unsigned)Bonfire,
            (uint32_t)Fields[0], (uint32_t)Fields[2]));
        return true;
    }

    void Append(const std::string& Text)
    {
        std::scoped_lock Lock(s_log_mutex);
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream)
        {
            SYSTEMTIME Now;
            GetLocalTime(&Now);
            Stream << StringFormat("%02u:%02u:%02u.%03u  ", Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds) << Text;
        }
    }

    uint64_t RestStartHook(void* Manager, int32_t Bonfire)
    {
        const uint64_t Started = s_original_rest(Manager, Bonfire);
        if ((uint8_t)Started != 0 && IsWhitePhantom())
        {
            DS2_CoopChannel::SendGuestEvent(DS2_CoopChannel::GuestEvent::RestStarted, MapOfBonfire((uint16_t)Bonfire), (uint32_t)Bonfire);
            Append(StringFormat("convidado: descanso na fogueira %08x no mundo do host; aviso ao host\n", (uint32_t)Bonfire));
        }
        if ((uint8_t)Started != 0 && OwnsTheWorld())
        {
            DS2_CoopChannel::SendHostEvent(DS2_CoopChannel::HostEvent::RestStarted);
            Append(StringFormat("host: descanso na fogueira %08x; aviso para a sessao\n", (uint32_t)Bonfire));
        }
        return Started;
    }

    void WorldResetHook()
    {
        if (!s_replaying && IsWhitePhantom())
        {
            // The host resets its world and the reset comes back to every
            // guest, this one included: resetting here too would do it twice.
            Append("convidado: o reinicio do meu descanso fica com o host\n");
            return;
        }
        s_original_reset();
        if (!s_replaying && OwnsTheWorld())
        {
            DS2_CoopChannel::SendHostEvent(DS2_CoopChannel::HostEvent::WorldReset);
            Append("host: o mundo foi reiniciado pelo descanso; pedido para a sessao\n");
        }
    }

    void* FrontEndOrNull()
    {
        uintptr_t Context = 0, FrontEnd = 0;
        if (ReadPointer(s_base + kGameGlobal, Context) && Context != 0 &&
            ReadPointer(Context + kFrontEnd, FrontEnd) && FrontEnd != 0)
        {
            return (void*)FrontEnd;
        }
        return nullptr;
    }

    void OpenQuestion(uint32_t Vote, bool AsHost, const std::wstring& Place, bool GuestProposed)
    {
        void* FrontEnd = FrontEndOrNull();
        if (FrontEnd == nullptr)
        {
            if (AsHost)
            {
                s_travel.HostAnswered = true;
                s_travel.HostYes = false;
            }
            else
            {
                DS2_CoopChannel::SendGuestAnswer(Vote, false);
            }
            Append(StringFormat("votacao %u sem frontend; resposta nao\n", Vote));
            return;
        }
        _snwprintf_s(s_question, _TRUNCATE, GuestProposed ? L"A player wants to travel to %ls. Travel together?"
                                                          : L"The host wants to travel to %ls. Travel together?", Place.c_str());
        s_open_vote = OpenVote();
        s_open_vote.Active = true;
        s_open_vote.Host = AsHost;
        s_open_vote.Vote = Vote;
        s_open_vote.Number = s_choice(FrontEnd, s_question, s_text(0, kYesText), s_text(0, kNoText), 1, 1, 1, 1);
        Append(StringFormat("%s: votacao %u aberta para %s (caixa %d)\n", AsHost ? "host" : "convidado", Vote,
            Narrow(Place).c_str(), s_open_vote.Number));
    }

    void BeginVote(void* List, uint64_t Proposer, uint16_t Bonfire, uint32_t Map)
    {
        const size_t Guests = DS2_CoopChannel::GuestCount();
        s_travel = HeldTravel();
        s_travel.Active = true;
        s_travel.List = List;
        s_travel.Proposer = Proposer;
        s_travel.Bonfire = Bonfire;
        s_travel.Map = Map;
        s_travel.HostAnswered = Proposer == 0;   // the host picked: its pick is its yes
        s_travel.HostYes = Proposer == 0;
        s_travel.Vote = ++s_vote_counter;
        s_travel.Guests = Guests;
        s_travel.Since = GetTickCount64();
        const uint32_t Carried = s_travel.Vote | (Proposer != 0 ? 0x80000000u : 0u);
        DS2_CoopChannel::SendHostEvent(DS2_CoopChannel::HostEvent::TravelVote, Map, Carried, (int32_t)Bonfire);
        const std::wstring Place = PlaceName(Bonfire, Map);
        Append(StringFormat("host: votacao %u para %s (fogueira %04x, mapa %08x), %s, %zu convidado(s)\n", s_travel.Vote,
            Narrow(Place).c_str(), (unsigned)Bonfire, Map, Proposer != 0 ? "proposta por um convidado" : "escolhida pelo host", Guests));
        if (Proposer != 0)
        {
            OpenQuestion(s_travel.Vote, true, Place, true);
        }
    }

    void TellCanceled(Cancel Why, const wchar_t* Text)
    {
        DS2_CoopChannel::SendHostEvent(DS2_CoopChannel::HostEvent::TravelCanceled, s_travel.Map, (uint32_t)Why, (int32_t)s_travel.Bonfire);
        ShowMessage(Text);
    }

    void PickHook(void* List)
    {
        if (List == nullptr || !s_votes_ready)
        {
            s_original_pick(List);
            return;
        }
        const uint8_t Role = LocalRole();
        if (Role == kRoleOwner)
        {
            if (s_travel.Pass)
            {
                s_travel.Pass = false;
                s_original_pick(List);
                return;
            }
            if (s_travel.Active)
            {
                return;   // a vote is running; the pick waits for it
            }
            if (DS2_CoopChannel::GuestCount() != 0)
            {
                const uint16_t Bonfire = s_travel_bonfire(List);
                BeginVote(List, 0, Bonfire, MapOfBonfire(Bonfire));
                return;
            }
        }
        else if (Role == kRoleWhitePhantom)
        {
            // A guest's pick never travels by itself: it is a proposal to
            // the host, who travels if everyone agrees.
            const ULONGLONG Now = GetTickCount64();
            if (s_proposal.Active && Now - s_proposal.Since < kVoteTimeoutMs + 5000)
            {
                return;
            }
            const uint16_t Bonfire = s_travel_bonfire(List);
            const uint32_t Map = MapOfBonfire(Bonfire);
            s_proposal = Proposal();
            s_proposal.Active = true;
            s_proposal.Bonfire = Bonfire;
            s_proposal.Since = Now;
            DS2_CoopChannel::SendGuestEvent(DS2_CoopChannel::GuestEvent::TravelPropose, Map, Bonfire);
            Append(StringFormat("convidado: proponho viajar para %s (fogueira %04x, mapa %08x)\n", Narrow(PlaceName(Bonfire, Map)).c_str(),
                (unsigned)Bonfire, Map));
            return;
        }
        s_original_pick(List);
    }

    // The list the vote was opened from, if it is still what it was: the
    // bonfire menu open (menu queue state 10) and the object still a travel
    // list. The host may have backed out while the others answered.
    bool ListStillOpen(void* List)
    {
        uintptr_t Context = 0, Events = 0, Queue = 0, Vftable = 0;
        uint8_t State[4] = {};
        if (List == nullptr || !ReadPointer(s_base + kGameGlobal, Context) || Context == 0 ||
            !ReadPointer(Context + kEventManager, Events) || Events == 0 ||
            !ReadPointer(Events + kMenuQueue, Queue) || Queue == 0)
        {
            return false;
        }
        for (size_t i = 0; i < sizeof(State); ++i)
        {
            if (!ReadByte(Queue + kQueueState + i, State[i]))
            {
                return false;
            }
        }
        int32_t QueueState = 0;
        memcpy(&QueueState, State, sizeof(QueueState));
        return QueueState == kQueueBonfireMenu && ReadPointer((uintptr_t)List, Vftable) && Vftable == s_base + kTravelListVftable;
    }

    void ShowMessage(const wchar_t* Text)
    {
        if (void* FrontEnd = FrontEndOrNull())
        {
            s_dialog(FrontEnd, Text, s_text(kTitleCategory, kTitleId), 1, 1);
        }
    }

    uint64_t SessionUpHook(void* Session)
    {
        const uintptr_t Caller = (uintptr_t)_ReturnAddress();
        const uint64_t Answer = s_original(Session);
        const uint8_t Role = (uint8_t)Answer != 0 && (Caller == s_base + kRestReturn || Caller == s_base + kQueueReturn) ? LocalRole() : 0xff;
        if (Role == kRoleOwner || (Role == kRoleWhitePhantom && s_guest_rest_ready))
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

    // The guest's half: optional, the rest in session works without it.
    s_log_path = injector.GetDllPath() / "DS2_Bonfire.log";
    s_request_path = injector.GetDllPath() / "DS2_Bonfire.req";
    if (Matches(Base + kRestStartOffset, kRestStartPrologue, sizeof(kRestStartPrologue)) &&
        Matches(Base + kWorldResetOffset, kWorldResetPrologue, sizeof(kWorldResetPrologue)) &&
        Matches(Base + kDialogOffset, kDialogPrologue, sizeof(kDialogPrologue)) &&
        Matches(Base + kTextOffset, kTextPrologue, sizeof(kTextPrologue)))
    {
        s_votes_ready = Matches(Base + kChoiceOffset, kChoicePrologue, sizeof(kChoicePrologue)) &&
            Matches(Base + kClosedOffset, kByNumberPrologue, sizeof(kByNumberPrologue)) &&
            Matches(Base + kButtonOffset, kByNumberPrologue, sizeof(kByNumberPrologue)) &&
            Matches(Base + kCloseOffset, kCloseByNumberPrologue, sizeof(kCloseByNumberPrologue)) &&
            Matches(Base + kReleaseOffset, kCloseByNumberPrologue, sizeof(kCloseByNumberPrologue)) &&
            Matches(Base + kPickOffset, kPickPrologue, sizeof(kPickPrologue)) &&
            *(const uintptr_t*)(Base + kTravelListVftable + kPickSlot) == Base + kPickOffset &&
            Matches(Base + kBonfireIndexOffset, kBonfireIndexPrologue, sizeof(kBonfireIndexPrologue)) &&
            Matches(Base + kBonfireMapOffset, kBonfireMapPrologue, sizeof(kBonfireMapPrologue)) &&
            Matches(Base + kBonfireLitOffset, kBonfireLitPrologue, sizeof(kBonfireLitPrologue)) &&
            Matches(Base + kTravelBuildOffset, kTravelBuildPrologue, sizeof(kTravelBuildPrologue)) &&
            Matches(Base + kTravelStartOffset, kTravelStartPrologue, sizeof(kTravelStartPrologue)) &&
            Matches(Base + kRecordSetOffset, kRecordSetPrologue, sizeof(kRecordSetPrologue)) &&
            Matches(Base + kTravelBonfireOffset, kTravelBonfirePrologue, sizeof(kTravelBonfirePrologue)) &&
            Matches(Base + kMenuCancelOffset, kMenuCancelPrologue, sizeof(kMenuCancelPrologue));
        // DS2_TraceHook's esd spy may have detoured it first (a jmp); Detours chains.
        s_guest_rest_ready = (Matches(Base + kInnerScriptOffset, kInnerScriptPrologue, sizeof(kInnerScriptPrologue)) ||
                              *(const uint8_t*)(Base + kInnerScriptOffset) == 0xe9) &&
            Matches(Base + kBonfireIndexOffset, kBonfireIndexPrologue, sizeof(kBonfireIndexPrologue)) &&
            Matches(Base + kPromptGateBeforeAt, kPromptGateBefore, sizeof(kPromptGateBefore)) &&
            Matches(Base + kPromptGate, kPromptGateExpected, sizeof(kPromptGateExpected)) &&
            Matches(Base + kPromptGateAfterAt, kPromptGateAfter, sizeof(kPromptGateAfter)) &&
            Matches(Base + kPromptGateCallAt, kPromptGateCall, sizeof(kPromptGateCall));
        s_bonfire_index = (BonfireIndex_p)(Base + kBonfireIndexOffset);
        s_bonfire_map = (BonfireMap_p)(Base + kBonfireMapOffset);
        s_bonfire_lit = (BonfireLit_p)(Base + kBonfireLitOffset);
        s_travel_build = (TravelBuild_p)(Base + kTravelBuildOffset);
        s_travel_start = (TravelStart_p)(Base + kTravelStartOffset);
        s_record_set = (RecordSet_p)(Base + kRecordSetOffset);
        s_travel_bonfire = (TravelBonfire_p)(Base + kTravelBonfireOffset);
        s_menu_cancel = (MenuCancel_p)(Base + kMenuCancelOffset);
        s_curtain = Matches(Base + kCurtainOffset, kCurtainPrologue, sizeof(kCurtainPrologue))
            ? (Curtain_p)(Base + kCurtainOffset) : nullptr;
        if (Matches(Base + kLoadingOpenOffset, kLoadingOpenPrologue, sizeof(kLoadingOpenPrologue)) &&
            Matches(Base + kLoadingCloseOffset, kLoadingClosePrologue, sizeof(kLoadingClosePrologue)) &&
            Matches(Base + kHudHideOffset, kHudHidePrologue, sizeof(kHudHidePrologue)) &&
            Matches(Base + kHudShowOffset, kHudShowPrologue, sizeof(kHudShowPrologue)) &&
            Matches(Base + kHudDropOffset, kHudDropPrologue, sizeof(kHudDropPrologue)))
        {
            s_loading_open = (FrontEndOnly_p)(Base + kLoadingOpenOffset);
            s_loading_close = (FrontEndOnly_p)(Base + kLoadingCloseOffset);
            s_hud_hide = (FrontEndMask_p)(Base + kHudHideOffset);
            s_hud_show = (FrontEndMask_p)(Base + kHudShowOffset);
            s_hud_drop = (FrontEndOnly_p)(Base + kHudDropOffset);
        }
        else
        {
            Error("[DS2BonfireInSession] a tela de carregamento do jogo nao tem o codigo esperado; a cortina so desliga o desenho");
        }
        s_original_inner_script = (Script_p)(Base + kInnerScriptOffset);
        s_original_pick = (Pick_p)(Base + kPickOffset);
        s_choice = (Choice_p)(Base + kChoiceOffset);
        s_closed = (ByNumber_p)(Base + kClosedOffset);
        s_button = (ByNumber_p)(Base + kButtonOffset);
        s_close = (CloseByNumber_p)(Base + kCloseOffset);
        s_release = (CloseByNumber_p)(Base + kReleaseOffset);
        s_original_rest = (RestStart_p)(Base + kRestStartOffset);
        s_original_reset = (WorldReset_p)(Base + kWorldResetOffset);
        s_dialog = (Dialog_p)(Base + kDialogOffset);
        s_text = (Text_p)(Base + kTextOffset);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)s_original_rest, RestStartHook);
        DetourAttach(&(PVOID&)s_original_reset, WorldResetHook);
        if (s_votes_ready)
        {
            DetourAttach(&(PVOID&)s_original_pick, PickHook);
        }
        if (s_guest_rest_ready)
        {
            DetourAttach(&(PVOID&)s_original_inner_script, InnerScriptHook);
        }
        if (DetourTransactionCommit() == NO_ERROR)
        {
            s_events_ready.store(true);
            Append(StringFormat("=== ds2os fogueira em sessao: descanso, reinicio do mundo, aviso, votacao de viagem (%s), descanso do convidado (%s) ===\n",
                s_votes_ready ? "pronta" : "codigo inesperado", s_guest_rest_ready ? "pronto" : "codigo inesperado"));
        }
        else
        {
            s_original_rest = nullptr;
            s_original_reset = nullptr;
            Error("[DS2BonfireInSession] nao consegui instalar o aviso e o reinicio do convidado");
        }
    }
    else
    {
        Error("[DS2BonfireInSession] o codigo do descanso, do reinicio ou da caixa de mensagem nao e o esperado; so o host descansa");
    }
    Log("[DS2BonfireInSession] o dono do mundo descansa em fogueira com a sessao de pe");
#endif
    return true;
}

void DS2_BonfireInSession_Tick()
{
#if defined(_WIN32) && defined(_M_X64)
    if (!s_events_ready.load())
    {
        return;
    }
    const ULONGLONG Now = GetTickCount64();
    DS2_CoopChannel::Bonfire Said;

    // The bonfire prompt's guest gate is open exactly while this player is a
    // white phantom; each write checks the bytes it replaces.
    if (s_guest_rest_ready && !s_prompt_gate_broken)
    {
        const bool Want = IsWhitePhantom();
        if (Want != s_prompt_patched)
        {
            const uint8_t* From = Want ? kPromptGateExpected : kPromptGatePatch;
            const uint8_t* To = Want ? kPromptGatePatch : kPromptGateExpected;
            if (Matches(s_base + kPromptGate, From, sizeof(kPromptGateExpected)) &&
                WriteCode(s_base + kPromptGate, To, sizeof(kPromptGateExpected)))
            {
                s_prompt_patched = Want;
                Append(Want ? "convidado: a trava do 'Rest at bonfire' para convidados foi aberta\n"
                            : "a trava do 'Rest at bonfire' para convidados foi restaurada\n");
            }
            else
            {
                s_prompt_gate_broken = true;
                Append("a trava do 'Rest at bonfire' nao tem os bytes esperados; o convidado nao descansa\n");
            }
        }
    }

    KeepJobPatch(Now);
    KeepCurtain(Now);

    // The host arrived: now the guests may come.
    if (s_call.Active)
    {
        const bool Settled = !s_go.Active && !DS2_DeathIntercept::Moving();
        if (!Settled)
        {
            s_call.Ready = 0;
            if (Now - s_call.Since > kCallGiveUpMs)
            {
                s_call.Active = false;
                DS2_CoopChannel::SendHostEvent(DS2_CoopChannel::HostEvent::TravelGo, s_call.Map, s_call.Bonfire);
                Append(StringFormat("host: %llu ms e ainda nao cheguei na fogueira %04x; chamo os convidados assim mesmo\n",
                    (unsigned long long)(Now - s_call.Since), (unsigned)s_call.Bonfire));
            }
        }
        else if (s_call.Ready == 0)
        {
            s_call.Ready = Now + kSettledMs;
        }
        else if (Now >= s_call.Ready)
        {
            s_call.Active = false;
            DS2_CoopChannel::SendHostEvent(DS2_CoopChannel::HostEvent::TravelGo, s_call.Map, s_call.Bonfire);
            Append(StringFormat("host: cheguei na fogueira %04x em %llu ms; os convidados podem vir\n",
                (unsigned)s_call.Bonfire, (unsigned long long)(Now - s_call.Since)));
        }
    }

    if (s_go.Active && Now >= s_go.At)
    {
        s_go.Active = false;
        DS2_DeathIntercept::GoToBonfire(s_go.Map, s_go.Bonfire);
        Append(StringFormat("indo para a fogueira %04x (mapa %08x) sem warp e sem sair da sessao\n",
            (unsigned)s_go.Bonfire, s_go.Map));
    }

    // `DS2_Bonfire.req`: `ir <mapa hex> <fogueira hex>` takes this machine's
    // player to that bonfire the way a travel does, with no session and no
    // vote - the control for a travel that closed the game. `votar <mapa hex>
    // <fogueira hex>` on the host starts the vote itself, without the travel
    // list, so the whole road (vote, host first, guests called) can be
    // measured from the request file.
    if (Now - s_request_tick >= 500)
    {
        s_request_tick = Now;
        std::error_code Error;
        if (!s_request_path.empty() && std::filesystem::exists(s_request_path, Error))
        {
            std::ifstream Stream(s_request_path);
            std::string Line;
            while (std::getline(Stream, Line))
            {
                unsigned Map = 0, Bonfire = 0;
                if (Line.rfind("junta", 0) == 0)
                {
                    s_together = Line.find("liga") != std::string::npos && Line.find("desliga") == std::string::npos;
                    Append(StringFormat("pedido: viagem junta (sem sair da sessao) %s\n", s_together ? "ligada" : "desligada"));
                }
                else if (sscanf_s(Line.c_str(), "ir %x %x", &Map, &Bonfire) == 2)
                {
                    Append(StringFormat("pedido: ir para a fogueira %04x do mapa %08x (alcancavel: %s)\n", Bonfire, Map,
                        DS2_DeathIntercept::MapReachable(Map) ? "sim" : "nao"));
                    StartGo(Map, (uint16_t)Bonfire);
                }
                else if (sscanf_s(Line.c_str(), "votar %x %x", &Map, &Bonfire) == 2)
                {
                    // The host starts the vote as if it had picked that bonfire
                    // in its travel list, with no list open: what the vote
                    // does after a yes from everyone is the same.
                    if (!OwnsTheWorld() || !s_votes_ready)
                    {
                        Append("pedido: votar, mas este jogador nao e o dono do mundo (ou a votacao nao esta pronta)\n");
                    }
                    else if (s_travel.Active)
                    {
                        Append("pedido: votar, mas ja ha uma votacao em andamento\n");
                    }
                    else
                    {
                        Append(StringFormat("pedido: votacao para a fogueira %04x do mapa %08x\n", Bonfire, Map));
                        BeginVote(nullptr, 0, (uint16_t)Bonfire, Map);
                    }
                }
            }
            Stream.close();
            std::filesystem::remove(s_request_path, Error);
        }
    }

    if (Now - s_lit_tick >= kLitEveryMs)
    {
        s_lit_tick = Now;
        if (OwnsTheWorld())
        {
            PublishLitBonfires();
        }
        else if (IsWhitePhantom())
        {
            ApplyHostLitBonfires();
        }
    }

    if (OwnsTheWorld())
    {
        if (DS2_CoopChannel::TakeGuestEvent(DS2_CoopChannel::GuestEvent::RestStarted, Said))
        {
            const uint32_t Low = (uint32_t)Said.From;
            DS2_CoopChannel::SendHostEvent(DS2_CoopChannel::HostEvent::RestStarted, Said.Map, Said.Id, (int32_t)Low);
            Append(StringFormat("host: o convidado %016llx descansou na fogueira %04x; reinicio o mundo para todos\n",
                (unsigned long long)Said.From, Said.Id));
            WorldResetHook();
        }
        if (DS2_CoopChannel::TakeGuestEvent(DS2_CoopChannel::GuestEvent::TravelPropose, Said))
        {
            const uint16_t Bonfire = (uint16_t)Said.Id;
            const uint32_t Map = MapOfBonfire(Bonfire);
            if (s_travel.Active)
            {
                const HeldTravel Running = s_travel;
                s_travel.Map = Said.Map;
                s_travel.Bonfire = Bonfire;
                DS2_CoopChannel::SendHostEvent(DS2_CoopChannel::HostEvent::TravelCanceled, Said.Map, (uint32_t)Cancel::Busy, (int32_t)Bonfire);
                s_travel.Map = Running.Map;
                s_travel.Bonfire = Running.Bonfire;
                Append(StringFormat("host: proposta de %016llx recusada: votacao %u em andamento\n", (unsigned long long)Said.From, s_travel.Vote));
            }
            else if (!HostHasLit(Bonfire) || Map == 0xffffffff)
            {
                DS2_CoopChannel::SendHostEvent(DS2_CoopChannel::HostEvent::TravelCanceled, Said.Map, (uint32_t)Cancel::NotLit, (int32_t)Bonfire);
                Append(StringFormat("host: proposta de %016llx recusada: a fogueira %04x nao esta acesa no meu mundo\n",
                    (unsigned long long)Said.From, (unsigned)Bonfire));
            }
            else
            {
                BeginVote(nullptr, Said.From, Bonfire, Map);
            }
        }
    }

    if (s_travel.Active)
    {
        size_t Yes = 0, No = 0;
        DS2_CoopChannel::GuestAnswers(s_travel.Vote, Yes, No);
        const size_t Guests = DS2_CoopChannel::GuestCount();
        if (No > 0 || (s_travel.HostAnswered && !s_travel.HostYes))
        {
            s_travel.Active = false;
            TellCanceled(Cancel::Declined, kTravelDeclined);
            Append(StringFormat("host: votacao %u recusada (%zu sim, %zu nao, host %s); viagem cancelada\n", s_travel.Vote, Yes, No,
                s_travel.HostYes ? "sim" : "nao"));
        }
        else if (s_travel.Leaving)
        {
            if (Guests != 0)
            {
                s_travel.GuestsGone = 0;
                if (Now - s_travel.LeaveSince > kLeaveGiveUpMs)
                {
                    s_travel.Active = false;
                    TellCanceled(Cancel::Stuck, kTravelStuck);
                    Append(StringFormat("host: votacao %u: %zu convidado(s) ainda na sessao depois de %llu ms; viagem cancelada\n",
                        s_travel.Vote, Guests, (unsigned long long)(Now - s_travel.LeaveSince)));
                }
            }
            else if (s_travel.GuestsGone == 0)
            {
                s_travel.GuestsGone = Now;
            }
            else if (Now - s_travel.GuestsGone >= kLeaveSettleMs)
            {
                s_travel.Active = false;
                const bool Open = s_travel.Proposer == 0 && ListStillOpen(s_travel.List);
                Append(StringFormat("host: convidados fora da sessao em %llu ms; %s\n",
                    (unsigned long long)(s_travel.GuestsGone - s_travel.LeaveSince),
                    Open ? "a escolha na lista segue" : "a viagem comeca por aqui"));
                if (Open)
                {
                    s_travel.Pass = true;
                    PickHook(s_travel.List);
                }
                else if (!StartTravel(s_travel.Bonfire))
                {
                    ShowMessage(kTravelStuck);
                    Append(StringFormat("host: a fogueira %04x nao esta na tabela; nao viajei\n", (unsigned)s_travel.Bonfire));
                }
            }
        }
        else if (s_travel.HostAnswered && (Guests == 0 || Yes >= Guests))
        {
            // Everyone goes to the bonfire on its own machine, without a
            // warp and without leaving the session; the old way - guests out
            // of the session, the host's own travel, the party putting them
            // back together a minute later - is what is left when the map
            // cannot be brought in beside this one.
            const bool Together = s_together && DS2_DeathIntercept::MapReachable(s_travel.Map);
            if (Together)
            {
                s_travel.Active = false;
                SetRecord(s_travel.Bonfire);
                StartGo(s_travel.Map, s_travel.Bonfire);
                s_call = CallGuests();
                s_call.Active = true;
                s_call.Map = s_travel.Map;
                s_call.Bonfire = s_travel.Bonfire;
                s_call.Since = Now;
                Append(StringFormat("host: votacao %u aprovada (%zu sim de %zu, host sim) em %llu ms; vou primeiro para a fogueira %04x e chamo os convidados ao chegar\n",
                    s_travel.Vote, Yes, Guests, (unsigned long long)(Now - s_travel.Since), (unsigned)s_travel.Bonfire));
            }
            else
            {
                s_travel.Leaving = true;
                s_travel.LeaveSince = Now;
                DS2_CoopChannel::SendHostEvent(DS2_CoopChannel::HostEvent::TravelLeave);
                Append(StringFormat("host: votacao %u aprovada (%zu sim de %zu, host sim) em %llu ms; %s, convidados saem da sessao\n",
                    s_travel.Vote, Yes, Guests, (unsigned long long)(Now - s_travel.Since),
                    s_together ? StringFormat("o mapa %08x nao pode ser trazido", s_travel.Map).c_str() : "viagem junta desligada"));
            }
        }
        else if (Now - s_travel.Since > kVoteTimeoutMs)
        {
            s_travel.Active = false;
            if (s_open_vote.Active && s_open_vote.Host)
            {
                if (void* FrontEnd = FrontEndOrNull())
                {
                    s_close(FrontEnd, 0);
                    s_release(FrontEnd, 0);
                }
                s_open_vote.Active = false;
            }
            TellCanceled(Cancel::NoAnswer, kTravelNoAnswer);
            Append(StringFormat("host: votacao %u sem resposta de todos (%zu sim de %zu, host %s); viagem cancelada\n", s_travel.Vote,
                Yes, Guests, s_travel.HostAnswered ? "respondeu" : "nao respondeu"));
        }
    }

    if (s_open_vote.Active)
    {
        void* FrontEnd = FrontEndOrNull();
        const bool Ours = FrontEnd != nullptr && *(const int32_t*)((const uint8_t*)FrontEnd + kDialogNumber) == s_open_vote.Number;
        if (!Ours || (uint8_t)s_closed(FrontEnd, s_open_vote.Number) != 0)
        {
            const uint64_t Button = Ours ? s_button(FrontEnd, s_open_vote.Number) : 2;
            const bool Yes = Ours && (uint32_t)Button != 2 && (uint32_t)Button != 5;
            if (Ours)
            {
                s_close(FrontEnd, 0);
                s_release(FrontEnd, 0);
            }
            s_open_vote.Active = false;
            if (s_open_vote.Host)
            {
                if (s_travel.Active && s_travel.Vote == s_open_vote.Vote)
                {
                    s_travel.HostAnswered = true;
                    s_travel.HostYes = Yes;
                }
            }
            else
            {
                DS2_CoopChannel::SendGuestAnswer(s_open_vote.Vote, Yes);
                s_answered_vote = s_open_vote.Vote;
                s_answered_yes = Yes;
            }
            Append(StringFormat("%s: votacao %u respondida %s (botao %llu%s)\n", s_open_vote.Host ? "host" : "convidado", s_open_vote.Vote,
                Yes ? "sim" : "nao", (unsigned long long)Button, Ours ? "" : ", a caixa foi trocada"));
        }
    }

    if (OwnsTheWorld())
    {
        return;
    }
    if (DS2_CoopChannel::TakeHostEvent(DS2_CoopChannel::HostEvent::TravelVote, Said) && s_votes_ready)
    {
        const uint32_t Vote = Said.Id & 0x7fffffff;
        const bool GuestProposed = (Said.Id & 0x80000000) != 0;
        const uint16_t Bonfire = (uint16_t)Said.Type;
        if (GuestProposed && s_proposal.Active && s_proposal.Bonfire == Bonfire)
        {
            // Our own proposal: our pick was our yes.
            s_proposal.Active = false;
            DS2_CoopChannel::SendGuestAnswer(Vote, true);
            s_answered_vote = Vote;
            s_answered_yes = true;
            Append(StringFormat("convidado: votacao %u e a minha proposta; respondo sim\n", Vote));
        }
        else
        {
            s_answered_vote = 0;
            OpenQuestion(Vote, false, PlaceName(Bonfire, Said.Map), GuestProposed);
        }
    }
    if (DS2_CoopChannel::TakeHostEvent(DS2_CoopChannel::HostEvent::TravelCanceled, Said))
    {
        const Cancel Why = (Cancel)Said.Id;
        const uint16_t Bonfire = (uint16_t)Said.Type;
        const bool Mine = s_proposal.Active && s_proposal.Bonfire == Bonfire;
        // A question still open vanishes here; its player is told why.
        const bool WasAsked = s_open_vote.Active && !s_open_vote.Host;
        if (WasAsked)
        {
            if (void* FrontEnd = FrontEndOrNull())
            {
                s_close(FrontEnd, 0);
                s_release(FrontEnd, 0);
            }
            s_open_vote.Active = false;
        }
        bool Show = false;
        if (Why == Cancel::NotLit && Mine)
        {
            _snwprintf_s(s_message, _TRUNCATE, L"Travel canceled: the host has not lit %ls.", PlaceName(Bonfire, Said.Map).c_str());
            Show = true;
        }
        else if (Why == Cancel::Busy && Mine)
        {
            wcsncpy_s(s_message, kTravelBusy, _TRUNCATE);
            Show = true;
        }
        else if (Why == Cancel::Declined || Why == Cancel::NoAnswer || Why == Cancel::Stuck)
        {
            wcsncpy_s(s_message, Why == Cancel::Declined ? kTravelDeclined : Why == Cancel::NoAnswer ? kTravelNoAnswer : kTravelStuck, _TRUNCATE);
            Show = Mine || s_answered_yes || WasAsked;
        }
        if (Mine)
        {
            s_proposal.Active = false;
        }
        if (Show)
        {
            ShowMessage(s_message);
        }
        Append(StringFormat("convidado: viagem cancelada pelo host (motivo %u, fogueira %04x)%s\n", (unsigned)Why, (unsigned)Bonfire,
            Show ? "; aviso mostrado" : ""));
    }
    if (DS2_CoopChannel::TakeHostEvent(DS2_CoopChannel::HostEvent::TravelGo, Said))
    {
        const uint16_t Bonfire = (uint16_t)Said.Id;
        if (s_open_vote.Active && !s_open_vote.Host)
        {
            if (void* FrontEnd = FrontEndOrNull())
            {
                s_close(FrontEnd, 0);
                s_release(FrontEnd, 0);
            }
            s_open_vote.Active = false;
        }
        s_proposal.Active = false;
        if (DS2_DeathIntercept::MapReachable(Said.Map))
        {
            Append(StringFormat("convidado: viagem aprovada para a fogueira %04x (mapa %08x); vou junto\n",
                (unsigned)Bonfire, Said.Map));
            StartGo(Said.Map, Bonfire);
        }
        else
        {
            _snwprintf_s(s_message, _TRUNCATE, L"Travel canceled: %ls could not be reached from here.",
                PlaceName(Bonfire, Said.Map).c_str());
            ShowMessage(s_message);
            Append(StringFormat("convidado: viagem para a fogueira %04x (mapa %08x), mas o mapa nao pode ser trazido aqui\n",
                (unsigned)Bonfire, Said.Map));
        }
    }
    if (DS2_CoopChannel::TakeHostEvent(DS2_CoopChannel::HostEvent::TravelLeave, Said))
    {
        const uintptr_t Session = (uintptr_t)DS2_RespawnInSession_PlayingSession();
        uintptr_t Vftable = 0;
        uint8_t State[4] = {};
        const bool Playing = Session != 0 && ReadPointer(Session, Vftable) && Vftable == s_base + kJoinCtrlVftable &&
            ReadByte(Session + kJoinState, State[0]) && ReadByte(Session + kJoinState + 1, State[1]) &&
            ReadByte(Session + kJoinState + 2, State[2]) && ReadByte(Session + kJoinState + 3, State[3]);
        int32_t StateValue = 0;
        memcpy(&StateValue, State, sizeof(StateValue));
        if (Playing && StateValue == kJoinPlaying)
        {
            int32_t* Leave = (int32_t*)(Session + kJoinLeave);
            const int32_t Before = *Leave;
            if (Before == 0)
            {
                *Leave = 1;
            }
            Append(StringFormat("convidado: votacao aprovada; saio da sessao para o host viajar (sessao %p, +0x120 %d -> %d)\n",
                (void*)Session, Before, *Leave));
        }
        else
        {
            Append(StringFormat("convidado: pedido de saida para a viagem, mas nao ha sessao jogando (sessao %p, vftable %s, estado %d)\n",
                (void*)Session, Vftable == s_base + kJoinCtrlVftable ? "certa" : "outra", StateValue));
        }
    }
    if (DS2_CoopChannel::TakeHostEvent(DS2_CoopChannel::HostEvent::RestStarted, Said) &&
        (uint32_t)Said.Type != (uint32_t)DS2_CoopChannel::SelfSteamId())
    {
        Append(StringFormat("convidado: um jogador descansou na fogueira %08x (mapa %08x, ha %llu ms)\n",
            Said.Id, Said.Map, (unsigned long long)Said.AgeMs));
    }
    if (DS2_CoopChannel::TakeHostEvent(DS2_CoopChannel::HostEvent::WorldReset, Said))
    {
        s_replaying = true;
        s_original_reset();
        s_replaying = false;
        Append(StringFormat("convidado: mundo do host reiniciado aqui tambem (pedido ha %llu ms)\n", (unsigned long long)Said.AgeMs));
    }
#endif
}

void DS2_BonfireInSessionHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    s_events_ready.store(false);
    if (s_original_rest != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_rest, RestStartHook);
        DetourDetach(&(PVOID&)s_original_reset, WorldResetHook);
        if (s_votes_ready)
        {
            DetourDetach(&(PVOID&)s_original_pick, PickHook);
        }
        if (s_guest_rest_ready)
        {
            DetourDetach(&(PVOID&)s_original_inner_script, InnerScriptHook);
        }
        DetourTransactionCommit();
        s_original_rest = nullptr;
        s_original_reset = nullptr;
    }
    if (s_curtain_up)
    {
        Curtain(false);
    }
    if (s_prompt_patched && Matches(s_base + kPromptGate, kPromptGatePatch, sizeof(kPromptGatePatch)))
    {
        WriteCode(s_base + kPromptGate, kPromptGateExpected, sizeof(kPromptGateExpected));
        s_prompt_patched = false;
    }
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
