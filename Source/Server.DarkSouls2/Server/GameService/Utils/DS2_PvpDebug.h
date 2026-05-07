/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Server/GameService/GameClient.h"
#include "Server/Server.h"
#include "Server.DarkSouls2/Protobuf/DS2_Protobufs.h"

#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"
#include "Shared/Platform/Platform.h"

#include <algorithm>
#include <string>

namespace DS2PvpDebug
{
    inline bool IsEnabled(Server* ServerInstance)
    {
        return ServerInstance != nullptr &&
               ServerInstance->GetGameType() == GameType::DarkSouls2 &&
               ServerInstance->GetConfig().DS2PvpDebugLogging;
    }

    inline std::string MaskIdentifier(const std::string& Value)
    {
        if (Value.empty())
        {
            return "<empty>";
        }
        if (Value.size() <= 4)
        {
            return "****";
        }

        return std::string(Value.size() - 4, '*') + Value.substr(Value.size() - 4);
    }

    inline const char* SignTypeName(uint32_t Type)
    {
        switch (Type)
        {
        case DS2_Frpg2RequestMessage::SignType_WhiteSoapstone:
            return "WhiteSoapstone";
        case DS2_Frpg2RequestMessage::SignType_SmallWhiteSoapstone:
            return "SmallWhiteSoapstone";
        case DS2_Frpg2RequestMessage::SignType_RedSoapstone:
            return "RedSoapstone";
        case DS2_Frpg2RequestMessage::SignType_Dragon:
            return "DragonEye";
        case DS2_Frpg2RequestMessage::SignType_MirrorKnight:
            return "MirrorKnight";
        default:
            return "Unknown";
        }
    }

    inline const char* BreakInTypeName(DS2_Frpg2RequestMessage::BreakInType Type)
    {
        switch (Type)
        {
        case DS2_Frpg2RequestMessage::BreakInType_RedEyeOrb:
            return "RedEyeOrb";
        case DS2_Frpg2RequestMessage::BreakInType_BlueEyeOrb:
            return "BlueEyeOrb";
        default:
            return "Unknown";
        }
    }

    inline const char* QuickMatchModeName(DS2_Frpg2RequestMessage::QuickMatchGameMode Mode)
    {
        switch (Mode)
        {
        case DS2_Frpg2RequestMessage::QuickMatchGameMode_Blue:
            return "Blue";
        case DS2_Frpg2RequestMessage::QuickMatchGameMode_Brotherhood:
            return "Brotherhood";
        default:
            return "Unknown";
        }
    }

    template <typename... Args>
    inline void LogEvent(Server* ServerInstance, GameClient* Client, const char* EventType, const char* DetailFormat = "", Args... Arguments)
    {
        if (!IsEnabled(ServerInstance))
        {
            return;
        }

        std::string Details;
        if (DetailFormat != nullptr && DetailFormat[0] != '\0')
        {
            Details = StringFormat(DetailFormat, Arguments...);
        }

        uint32_t ProfileId = 0;
        int CharacterId = -1;
        uint32_t AreaId = 0;
        std::string SteamId = "<none>";
        std::string Source = "DS2PvP";

        if (Client != nullptr)
        {
            PlayerState& Player = Client->GetPlayerState();
            ProfileId = Player.GetPlayerId();
            CharacterId = Player.GetCharacterId();
            AreaId = Player.GetCurrentAreaId();
            SteamId = MaskIdentifier(Player.GetSteamId());
            Source = Client->GetName();
        }

        LogS(Source.c_str(),
            "[DS2PvpDebug] monotonic_ts=%.3f event_type=%s steam_id_masked=%s profile_id=%u character_id=%d area_id=%u %s",
            GetSeconds(),
            EventType,
            SteamId.c_str(),
            ProfileId,
            CharacterId,
            AreaId,
            Details.c_str());
    }
}
