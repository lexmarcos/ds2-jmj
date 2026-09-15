/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

#include <cstdint>

// A channel of the mod's own between the machines of a session, and the one
// thing it carries today: the bonfire the host respawns at.
//
// A guest in the host's world has only its own respawn record (`*(ctx+0x70)`:
// +0x164 map, +0x168 type, +0x16c id), which holds the last bonfire of its own
// world, and the game writes none for a guest: lighting (`FUN_1401caf50`) and
// resting (`FUN_1401cb950`) record the bonfire only for the local player, and
// only when the context's slot +0x58 says it is not in someone else's world.
// The host's record exists on the host's machine alone.
//
// The session between the players is Steam P2P, and the game uses one channel
// of it: every `SteamNetworking()` call in the binary passes channel 0
// (`FUN_140a75800` asks and `FUN_140a73de0` reads with 0, `FUN_140a7a410` and
// `FUN_140a76d90` send with 0). A packet on another channel travels over the
// same P2P session and waits on the other machine, untouched by the game, for
// whoever reads that channel. No server, no new connection.
//
// `FUN_140a75800` is slot +0x108 of `DLNRD::SteamSessionLight` (vftable
// `0x1411b1058`), the session's poll, run by the session manager's thread. The
// members are the vector at +0x68..+0x70, each a `SteamSessionMemberLight` with
// its CSteamID at +0xc8; the game looks the sender of a packet up there. Which
// of them is the host the game decides itself when it adds a member
// (`FUN_140a72740`): +0xad is set when the member is the lobby's owner, and the
// debug log calls it "Host". The detour lets the poll run, then reads this
// channel and, on the host, speaks.
//
// The host, when it is also the owner of the world it stands in (role 0),
// announces the map, type and id of its record to every other member every two
// seconds, and at once when they change. A guest keeps the last announcement
// that came from the host of its session. Role 0 alone is not enough: a guest
// on its way in still owns its own world for a few seconds, and announced its
// own bonfire five times before arriving (14/09). The network thread never
// reads the game's world: the game's thread publishes the local role and record
// once a frame (DS2_DeathInterceptHook), and the death hook asks for the host's
// bonfire when a guest dies.
//
// `DS2_Channel.req`: `status` writes what the channel has seen to
// `DS2_Channel.log`.
class DS2_CoopChannelHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};

namespace DS2_CoopChannel
{
    struct Bonfire
    {
        uint32_t Map = 0;
        int32_t Type = 0;
        uint32_t Id = 0;
        uint64_t From = 0;      // the host's SteamID64
        uint64_t AgeMs = 0;
    };

    // From the game's thread: who the local player is, and what its respawn
    // record holds. Role 0xff when there is no local player.
    void PublishLocal(uint8_t Role, uint32_t Map, int32_t Type, uint32_t Id);

    // The bonfire the host of this session last announced: false unless it
    // came in the last 30 s from someone who is the host of a session now.
    bool HostBonfire(Bonfire& Out);

    // Things the host tells its guests once, at the moment they happen. The
    // host queues one from the game's thread; the next poll sends it to every
    // other member, with the host's respawn record, if the host owns the world
    // and has guests (otherwise it is dropped). A guest takes each received
    // event once.
    enum class HostEvent : uint8_t
    {
        RestStarted = 0,   // the host sat at a bonfire
        WorldReset = 1,    // the rest reset the host's world (FUN_14017fd70)
        TravelVote = 2,    // the host asks to travel; Id carries the vote number
        TravelFollow = 3,  // the host travelled; Map/Id are its destination
    };
    constexpr uint8_t kHostEventCount = 4;

    // Other members of a session this machine hosts, seen in the last few
    // seconds. 0 when it hosts nothing.
    size_t GuestCount();
    // With Map/Id nonzero they replace the host's record in the event.
    void SendHostEvent(HostEvent Event, uint32_t Map = 0, uint32_t Id = 0);
    bool TakeHostEvent(HostEvent Event, Bonfire& Out);

    // A guest's answer to the host's vote, sent to the host of its session by
    // the next poll. The host counts the answers it received for one vote.
    void SendGuestAnswer(uint32_t Vote, bool Yes);
    void GuestAnswers(uint32_t Vote, size_t& Yes, size_t& No);
}
