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

    // DS2-only exploratory probe: finds where the game keeps the id of the
    // area the player is in. Writes DS2_AreaProbe.log and changes nothing.
    bool DS2ProbeArea = false;

    // With the probe on, watch reads of the address it finds using a hardware
    // watchpoint, and report which instructions touch it.
    bool DS2WatchAreaReads = false;

public:

    bool Save(const std::filesystem::path& Path);
    bool Load(const std::filesystem::path& Path);
    bool Serialize(nlohmann::json& Json, bool Loading);

};
