/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include <string>
#include <filesystem>
#include "ThirdParty/nlohmann/json.hpp"

// Configuration saved and loaded at runtime by the server from a configuration file.
class RuntimeConfig
{
public:

    // Name of server being joined.
    std::string ServerName = "Dark Souls 3 Server";

    // Hostname of server being joined.
    std::string ServerHostname = "";

    // Public key of server being joined.
    std::string ServerPublicKey = "";

    // Type of game we are being injected into.
    std::string ServerGameType = "";

    // Login port to connect to on server.
    int ServerPort = 50050;

    // If we should use seperate saves from the retail ones.
    bool EnableSeperateSaveFiles = true;

    // DS2-only: patch the client-side phantom session timer so PvP sessions are
    // not ended by it. Disabled by default.
    bool DS2PatchPhantomTimers = false;

    // Value written to the active session timer when the patch is enabled.
    double DS2PhantomTimerSeconds = 4000.0;

    // DS2-only: raise the number of players a session can hold from six to
    // twelve. The six is the size of two fixed per-player arrays inside the
    // game's own objects, so this grows those objects and moves the arrays;
    // see docs/DS2_SESSION_SLOTS.md. Never tested with more than two players,
    // because two Steam accounts on one machine cannot produce a third.
    // Disabled by default.
    bool DS2ExpandSessionSlots = false;

    // DS2-only exploratory probe: finds where the game keeps the id of the
    // area the player is in. Writes DS2_AreaProbe.log and changes nothing.
    bool DS2ProbeArea = false;

    // With the probe on, watch reads of the address it finds using a hardware
    // watchpoint, and report which instructions touch it.
    bool DS2WatchAreaReads = false;

    // Address of the current area id, if already known. Skips the scan, which
    // otherwise needs a trip between two areas to identify it. Ignored unless
    // it currently holds a known area id.
    std::string DS2AreaAddress = "";

    // DS2-only exploratory probe: reports the multiplay zone the player is in
    // and the permissions it carries. Writes DS2_MultiPlayZone.log.
    bool DS2ProbeMultiPlayZone = false;

    // DS2-only: make the game believe the player is always inside a multiplay
    // zone, so summon signs and invasions work where they normally cannot.
    bool DS2ForceMultiPlayZone = false;

    // Zone substituted when the game reports none. 103110 is Heide's Tower of
    // Flame, measured as a zone that permits summoning.
    int DS2ForcedZoneId = 103110;

public:

    bool Save(const std::filesystem::path& Path);
    bool Load(const std::filesystem::path& Path);
    bool Serialize(nlohmann::json& Json, bool Loading);

};
