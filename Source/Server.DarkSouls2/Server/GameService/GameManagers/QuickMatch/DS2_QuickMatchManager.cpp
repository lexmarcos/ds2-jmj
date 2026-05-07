/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Server/GameService/GameManagers/QuickMatch/DS2_QuickMatchManager.h"
#include "Server/GameService/DS2_PlayerState.h"
#include "Server/GameService/GameClient.h"
#include "Server/GameService/GameService.h"
#include "Server/Streams/Frpg2ReliableUdpMessage.h"
#include "Server/Streams/Frpg2ReliableUdpMessageStream.h"
#include "Server/Streams/DS2_Frpg2ReliableUdpMessage.h"

#include "Config/RuntimeConfig.h"
#include "Server/Server.h"

#include "Config/BuildConfig.h"
#include "Server/GameService/Utils/DS2_PvpDebug.h"

#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/File.h"
#include "Shared/Core/Utils/Strings.h"
#include "Shared/Core/Utils/DiffTracker.h"

DS2_QuickMatchManager::DS2_QuickMatchManager(Server* InServerInstance, GameService* InGameServiceInstance)
    : ServerInstance(InServerInstance)
    , GameServiceInstance(InGameServiceInstance)
{
}

void DS2_QuickMatchManager::OnLostPlayer(GameClient* Client)
{
    for (auto Iter = Matches.begin(); Iter != Matches.end(); /* empty */)
    {
        std::shared_ptr<Match> Match = *Iter;

        if (Match->HostPlayerId == Client->GetPlayerState().GetPlayerId())
        {
            LogS(Client->GetName().c_str(), "Unregistered quick match hosted by player %u, as player has disconnected.", Match->HostPlayerId);
            DS2PvpDebug::LogEvent(ServerInstance, Client, "SessionCleanup",
                "cleanup=quick_match host_profile_id=%u mode=%s request_area_id=%u cell_id=%u",
                Match->HostPlayerId,
                DS2PvpDebug::QuickMatchModeName(Match->GameMode),
                (uint32_t)Match->AreaId,
                (uint32_t)Match->CellId);
            Iter = Matches.erase(Iter);
        }
        else
        {
            Iter++;
        }
    }
}

void DS2_QuickMatchManager::Poll()
{
}

MessageHandleResult DS2_QuickMatchManager::OnMessageRecieved(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestSearchQuickMatch))
    {
        return Handle_RequestSearchQuickMatch(Client, Message);
    }
    else if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestUnregisterQuickMatch))
    {
        return Handle_RequestUnregisterQuickMatch(Client, Message);
    }
    else if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestUpdateQuickMatch))
    {
        return Handle_RequestUpdateQuickMatch(Client, Message);
    }
    else if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestJoinQuickMatch))
    {
        return Handle_RequestJoinQuickMatch(Client, Message);
    }
    else if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestRejectQuickMatch))
    {
        return Handle_RequestRejectQuickMatch(Client, Message);
    }
    else if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestRegisterQuickMatch))
    {
        return Handle_RequestRegisterQuickMatch(Client, Message);
    }

    return MessageHandleResult::Unhandled;
}

bool DS2_QuickMatchManager::CanMatchWith(GameClient* Client, const DS2_Frpg2RequestMessage::RequestSearchQuickMatch& Request, const std::shared_ptr<Match>& Match)
{
    auto& Player = Client->GetPlayerStateType<DS2_PlayerState>();
    const RuntimeConfig& Config = ServerInstance->GetConfig();

    // Can't match with self.
    if (Client->GetPlayerState().GetPlayerId() == Match->HostPlayerId)
    {
        return false;
    }

    // Matches requested game mode.
    if (Match->GameMode != Request.mode())
    {
        return false;
    }

    // Matches requested map.
    if (Request.cell_id() != (uint32_t)Match->CellId ||
        Request.online_area_id() != (uint32_t)Match->AreaId)
    {
        return false;
    }

    // Check for matchmaking match.
    return Config.DS2_ArenaMatchingParameters.CheckMatch(Match->MatchingParams.soul_memory(), Request.matching_parameter().soul_memory(), Request.matching_parameter().name_engraved_ring() > 0);

#if 0

    // Can match with the hosts level.
    const RuntimeConfig& Config = ServerInstance->GetConfig();
    const RuntimeConfigMatchingParameters* MatchingParams = &Config.UndeadMatchMatchingParameters;

    if (!MatchingParams->CheckMatch(
        Match->MatchingParams.soul_level(), Match->MatchingParams.weapon_level(),
        Request.matching_parameter().soul_level(), Request.matching_parameter().weapon_level(),
        Match->MatchingParams.password().size() > 0
    ))
    {
        return false;
    }

    // Check passwords match.
    if (Match->MatchingParams.password() != Request.matching_parameter().password())
    {
        return false;
    }
#endif

    return true;
}

std::shared_ptr<DS2_QuickMatchManager::Match> DS2_QuickMatchManager::GetMatchByHost(uint32_t HostPlayerId)
{
    for (std::shared_ptr<Match>& Iter : Matches)
    {
        if (Iter->HostPlayerId == HostPlayerId)
        {
            return Iter;
        }
    }
    return nullptr;
}

MessageHandleResult DS2_QuickMatchManager::Handle_RequestSearchQuickMatch(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    DS2_Frpg2RequestMessage::RequestSearchQuickMatch* Request = (DS2_Frpg2RequestMessage::RequestSearchQuickMatch*)Message.Protobuf.get();
    DS2_Frpg2RequestMessage::RequestSearchQuickMatchResponse Response;
    
    int ResultCount = 0;
    for (std::shared_ptr<Match>& Iter : Matches)
    {
        if (!CanMatchWith(Client, *Request, Iter))
        {
            continue;
        }

        DS2_Frpg2RequestMessage::QuickMatchData* Result = Response.add_matches();
        Result->set_player_id(Iter->HostPlayerId);
        Result->set_player_steam_id(Iter->HostPlayerSteamId);
        Result->set_cell_id((uint32_t)Iter->CellId);
        Result->mutable_matching_parameter()->CopyFrom(Iter->MatchingParams);
        Result->set_online_area_id((uint32_t)Iter->AreaId);
        Result->set_mode(Iter->GameMode);

        ResultCount++;
    }

    LogS(Client->GetName().c_str(), "RequestSearchQuickMatch: Found %i matches.", ResultCount);
    DS2PvpDebug::LogEvent(ServerInstance, Client, "QuickMatchSearchRequest",
        "mode=%s request_area_id=%lld cell_id=%lld max_results=%lld result_count=%d soul_memory=%u name_engraved_ring=%u",
        DS2PvpDebug::QuickMatchModeName(Request->mode()),
        (long long)Request->online_area_id(),
        (long long)Request->cell_id(),
        (long long)Request->max_results(),
        ResultCount,
        Request->matching_parameter().soul_memory(),
        Request->matching_parameter().name_engraved_ring());
    
    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send RequestCountRankingDataResponse response.");
        return MessageHandleResult::Error;
    }

    return MessageHandleResult::Handled;
}

MessageHandleResult DS2_QuickMatchManager::Handle_RequestRegisterQuickMatch(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    DS2_Frpg2RequestMessage::RequestRegisterQuickMatch* Request = (DS2_Frpg2RequestMessage::RequestRegisterQuickMatch*)Message.Protobuf.get();
    DS2_Frpg2RequestMessage::RequestRegisterQuickMatchResponse Response;

    std::shared_ptr<Match> NewMatch = std::make_shared<Match>();
    NewMatch->HostPlayerId = Client->GetPlayerState().GetPlayerId();
    NewMatch->HostPlayerSteamId = Client->GetPlayerState().GetSteamId();
    NewMatch->GameMode = Request->mode();
    NewMatch->MatchingParams = Request->matching_parameter();
    NewMatch->CellId = Request->cell_id();
    NewMatch->AreaId = (DS2_OnlineAreaId)Request->online_area_id();
    NewMatch->HasStarted = false;

    LogS(Client->GetName().c_str(), "RequestRegisterQuickMatch: Hosting new match.");
    DS2PvpDebug::LogEvent(ServerInstance, Client, "QuickMatchRegisterRequest",
        "mode=%s request_area_id=%lld cell_id=%lld soul_memory=%u name_engraved_ring=%u",
        DS2PvpDebug::QuickMatchModeName(Request->mode()),
        (long long)Request->online_area_id(),
        (long long)Request->cell_id(),
        Request->matching_parameter().soul_memory(),
        Request->matching_parameter().name_engraved_ring());
    
    Matches.push_back(NewMatch);

    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send RequestRegisterQuickMatchResponse response.");
        return MessageHandleResult::Error;
    }

    if (ServerInstance->GetConfig().SendDiscordNotice_QuickMatch)
    {
        std::string ModeName = "";
        switch (NewMatch->GameMode)
        {
        case DS2_Frpg2RequestMessage::QuickMatchGameMode::QuickMatchGameMode_Blue:          ModeName = "Blue Sentinel";              break;
        case DS2_Frpg2RequestMessage::QuickMatchGameMode::QuickMatchGameMode_Brotherhood:   ModeName = "Brotherhood of Blood";    break;
        }

        ServerInstance->SendDiscordNotice(Client->shared_from_this(), DiscordNoticeType::UndeadMatch,
            StringFormat("Started a public '%s' undead match.", ModeName.c_str()),
            0,
            {
                { "Soul Memory", std::to_string(Client->GetPlayerState().GetSoulMemory()), true },
            }
        );
    }

    return MessageHandleResult::Handled;
}

MessageHandleResult DS2_QuickMatchManager::Handle_RequestUnregisterQuickMatch(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    DS2_Frpg2RequestMessage::RequestUnregisterQuickMatch* Request = (DS2_Frpg2RequestMessage::RequestUnregisterQuickMatch*)Message.Protobuf.get();
    DS2_Frpg2RequestMessage::RequestUnregisterQuickMatchResponse Response;

    for (auto Iter = Matches.begin(); Iter != Matches.end(); /* empty */)
    {
        std::shared_ptr<Match> Match = *Iter;
        if (          Match->HostPlayerId == Client->GetPlayerState().GetPlayerId() &&
                      Match->GameMode == Request->mode() &&
                      Match->CellId == Request->cell_id() &&
            (uint32_t)Match->AreaId == Request->online_area_id())
        {
            LogS(Client->GetName().c_str(), "RequestUnregisterQuickMatch: Unregistered quick match hosted by self.", Match->HostPlayerId);
            DS2PvpDebug::LogEvent(ServerInstance, Client, "QuickMatchUnregisterRequest",
                "mode=%s request_area_id=%lld cell_id=%lld host_profile_id=%u",
                DS2PvpDebug::QuickMatchModeName(Request->mode()),
                (long long)Request->online_area_id(),
                (long long)Request->cell_id(),
                Match->HostPlayerId);
            Iter = Matches.erase(Iter);
        }
        else
        {
            Iter++;
        }
    }

    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send RequestUnregisterQuickMatchResponse response.");
        return MessageHandleResult::Error;
    }

    return MessageHandleResult::Handled;
}

MessageHandleResult DS2_QuickMatchManager::Handle_RequestUpdateQuickMatch(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    DS2_Frpg2RequestMessage::RequestUpdateQuickMatch* Request = (DS2_Frpg2RequestMessage::RequestUpdateQuickMatch*)Message.Protobuf.get();

    // Not sure we really need to do anything with this. It just keeps the match alive?
    DS2PvpDebug::LogEvent(ServerInstance, Client, "QuickMatchUpdateRequest",
        "mode=%s request_area_id=%lld cell_id=%lld",
        DS2PvpDebug::QuickMatchModeName(Request->mode()),
        (long long)Request->online_area_id(),
        (long long)Request->cell_id());

    DS2_Frpg2RequestMessage::RequestUpdateQuickMatchResponse Response;
    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send RequestUpdateQuickMatchResponse response.");
        return MessageHandleResult::Error;
    }

    return MessageHandleResult::Handled;
}

MessageHandleResult DS2_QuickMatchManager::Handle_RequestJoinQuickMatch(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    ServerDatabase& Database = ServerInstance->GetDatabase();
    DS2_PlayerState& Player = Client->GetPlayerStateType<DS2_PlayerState>();

    DS2_Frpg2RequestMessage::RequestJoinQuickMatch* Request = (DS2_Frpg2RequestMessage::RequestJoinQuickMatch*)Message.Protobuf.get();

    bool bSuccess = false;

    DS2PvpDebug::LogEvent(ServerInstance, Client, "QuickMatchJoinRequest",
        "mode=%s host_profile_id=%lld request_area_id=%lld cell_id=%lld",
        DS2PvpDebug::QuickMatchModeName(Request->mode()),
        (long long)Request->player_id(),
        (long long)Request->online_area_id(),
        (long long)Request->cell_id());

    std::shared_ptr<GameClient> HostClient = GameServiceInstance->FindClientByPlayerId(Request->player_id());
    if (HostClient)
    {
        std::shared_ptr<Match> ExistingMatch = GetMatchByHost(Request->player_id());    
        if (ExistingMatch)
        {        
            LogS(Client->GetName().c_str(), "RequestJoinQuickMatch: Attempting to join match hosted by %s", HostClient->GetName().c_str());

            DS2_Frpg2RequestMessage::PushRequestJoinQuickMatch PushMessage;
            PushMessage.set_push_message_id(DS2_Frpg2RequestMessage::PushID_PushRequestJoinQuickMatch);
            PushMessage.set_player_id(Player.GetPlayerId());
            PushMessage.set_player_steam_id(Player.GetSteamId());
            PushMessage.set_online_area_id((uint32_t)ExistingMatch->AreaId);
            PushMessage.set_cell_id((uint32_t)ExistingMatch->CellId);
            PushMessage.set_mode(ExistingMatch->GameMode);

            if (!HostClient->MessageStream->Send(&PushMessage))
            {
                WarningS(Client->GetName().c_str(), "Failed to send PushRequestJoinQuickMatch to host of quick match.");
                bSuccess = false;
            }
            else
            {
                bSuccess = true;
                DS2PvpDebug::LogEvent(ServerInstance, Client, "QuickMatchJoinForwarded",
                    "mode=%s host_profile_id=%u host_steam_id_masked=%s host_character_id=%d request_area_id=%u cell_id=%u",
                    DS2PvpDebug::QuickMatchModeName(ExistingMatch->GameMode),
                    HostClient->GetPlayerState().GetPlayerId(),
                    DS2PvpDebug::MaskIdentifier(HostClient->GetPlayerState().GetSteamId()).c_str(),
                    HostClient->GetPlayerState().GetCharacterId(),
                    (uint32_t)ExistingMatch->AreaId,
                    (uint32_t)ExistingMatch->CellId);
            }
        }
    }

    if (!bSuccess)
    {
        DS2PvpDebug::LogEvent(ServerInstance, Client, "QuickMatchJoinRejected",
            "mode=%s host_profile_id=%lld host_found=%u request_area_id=%lld cell_id=%lld",
            DS2PvpDebug::QuickMatchModeName(Request->mode()),
            (long long)Request->player_id(),
            HostClient ? 1 : 0,
            (long long)Request->online_area_id(),
            (long long)Request->cell_id());

        DS2_Frpg2RequestMessage::PushRequestRejectQuickMatch PushMessage;
        PushMessage.set_push_message_id(DS2_Frpg2RequestMessage::PushID_PushRequestRejectQuickMatch);
        PushMessage.set_player_id(Request->player_id());
        PushMessage.set_player_steam_id(HostClient ? HostClient->GetPlayerState().GetSteamId() : "");
        PushMessage.set_online_area_id(Request->online_area_id());
        PushMessage.set_cell_id(Request->cell_id());
        PushMessage.set_mode(Request->mode());
        PushMessage.set_unknown_7(0);
     
        if (!Client->MessageStream->Send(&PushMessage))
        {
            WarningS(Client->GetName().c_str(), "Failed to send PushRequestRejectQuickMatch to player attempting to join quick match.");
            bSuccess = false;
        }

        return MessageHandleResult::Handled;
    }

    DS2_Frpg2RequestMessage::RequestJoinQuickMatchResponse Response;
    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send RequestJoinQuickMatchResponse response.");
        return MessageHandleResult::Error;
    }

    return MessageHandleResult::Handled;
}

MessageHandleResult DS2_QuickMatchManager::Handle_RequestRejectQuickMatch(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    PlayerState& Player = Client->GetPlayerState();

    DS2_Frpg2RequestMessage::RequestRejectQuickMatch* Request = (DS2_Frpg2RequestMessage::RequestRejectQuickMatch*)Message.Protobuf.get();

    std::shared_ptr<GameClient> TargetClient = GameServiceInstance->FindClientByPlayerId(Request->player_id());
    if (TargetClient)
    {
        LogS(Client->GetName().c_str(), "RequestRejectQuickMatch: Rejecting join request from %s", TargetClient->GetName().c_str());
        DS2PvpDebug::LogEvent(ServerInstance, Client, "QuickMatchRejectRequest",
            "mode=%s target_profile_id=%u target_steam_id_masked=%s target_character_id=%d request_area_id=%lld cell_id=%lld reason=%lld",
            DS2PvpDebug::QuickMatchModeName(Request->mode()),
            TargetClient->GetPlayerState().GetPlayerId(),
            DS2PvpDebug::MaskIdentifier(TargetClient->GetPlayerState().GetSteamId()).c_str(),
            TargetClient->GetPlayerState().GetCharacterId(),
            (long long)Request->online_area_id(),
            (long long)Request->cell_id(),
            (long long)Request->unknown_5());

        DS2_Frpg2RequestMessage::PushRequestRejectQuickMatch PushMessage;
        PushMessage.set_push_message_id(DS2_Frpg2RequestMessage::PushID_PushRequestRejectQuickMatch);
        PushMessage.set_player_id(Player.GetPlayerId());
        PushMessage.set_player_steam_id(Player.GetSteamId());
        PushMessage.set_online_area_id(Request->online_area_id());
        PushMessage.set_cell_id(Request->cell_id());
        PushMessage.set_mode(Request->mode());
        PushMessage.set_unknown_7(Request->unknown_5());

        if (!TargetClient->MessageStream->Send(&PushMessage))
        {
            WarningS(Client->GetName().c_str(), "Failed to send PushRequestRejectQuickMatch to target of quick match join.");
        }
    }
    else
    {
        DS2PvpDebug::LogEvent(ServerInstance, Client, "QuickMatchRejectRequest",
            "mode=%s target_profile_id=%lld target_found=0 request_area_id=%lld cell_id=%lld reason=%lld",
            DS2PvpDebug::QuickMatchModeName(Request->mode()),
            (long long)Request->player_id(),
            (long long)Request->online_area_id(),
            (long long)Request->cell_id(),
            (long long)Request->unknown_5());
    }

    DS2_Frpg2RequestMessage::RequestRejectQuickMatchResponse Response;
    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send RequestRejectQuickMatchResponse response.");
        return MessageHandleResult::Error;
    }

    return MessageHandleResult::Handled;
}

std::string DS2_QuickMatchManager::GetName()
{
    return "Quick Match";
}
