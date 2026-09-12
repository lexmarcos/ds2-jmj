/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Server/GameService/GameManager.h"
#include "Server/GameService/Utils/OnlineAreaPool.h"
#include "Server/Database/DatabaseTypes.h"
#include "Server.DarkSouls2/Protobuf/DS2_Protobufs.h"
#include "Server.DarkSouls2/Server/GameService/Utils/DS2_GameIds.h"
#include "Server.DarkSouls2/Server/GameService/Utils/DS2_CellAndAreaId.h"

#include <unordered_map>

struct Frpg2ReliableUdpMessage;
class Server;
class GameService;

// Handles all client requests to do with matchmaking
// Placing and retrieving summon signs.

class DS2_SignManager
    : public GameManager
{
public:    
    DS2_SignManager(Server* InServerInstance, GameService* InGameServiceInstance);

    virtual MessageHandleResult OnMessageReceived(GameClient* Client, const Frpg2ReliableUdpMessage& Message) override;

    virtual std::string GetName() override;
    virtual void Poll() override;

    virtual void OnLostPlayer(GameClient* Client) override;

    size_t GetLiveCount() { return LiveCache.GetTotalEntries(); }

protected:
    bool CanMatchWith(const DS2_Frpg2RequestMessage::MatchingParameter& Client, const DS2_Frpg2RequestMessage::MatchingParameter& Match, uint32_t SignType);

    void RemoveSignAndNotifyAware(const std::shared_ptr<SummonSign>& Sign);

    MessageHandleResult Handle_RequestGetSignList(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestCreateSign(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestRemoveSign(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestUpdateSign(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestSummonSign(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestRejectSign(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestGetRightMatchingArea(GameClient* Client, const Frpg2ReliableUdpMessage& Message);

    // Sends the summon push for a sign as if the remembered host had walked up
    // to it and pressed the button. Returns why it could not, for the log.
    bool ReplaySummon(uint32_t OwnerPlayerId, std::string& OutReason);

    // Tells the host a visitor is arriving, the way a covenant invasion does.
    // The point of it is which side opens the session: an invasion push goes to
    // the *host* and the host reaches for its peer, which is the half a
    // replayed summon is missing.
    bool PushVisitToHost(uint32_t OwnerPlayerId, uint32_t VisitType, std::string& OutReason);

    void PollRematchRequest();

private:
    Server* ServerInstance;
    GameService* GameServiceInstance;

    OnlineAreaPool<DS2_CellAndAreaId, SummonSign> LiveCache;

    uint32_t NextSignId = 1000;

    // Throttles the sticky-sign diagnostic, which would otherwise print on
    // every sign list poll. Keyed on player id.
    std::unordered_map<uint32_t, double> LastStickyLogTime;

    // The last summon of each sign owner, kept so the same duel can be started
    // again. The player struct is the summoning host's own blob, which the
    // server never builds and can only repeat: it arrives in RequestSummonSign
    // and is handed to the phantom's client untouched.
    struct RememberedSummon
    {
        uint32_t HostPlayerId = 0;
        std::string HostSteamId;
        std::string PlayerStruct;
        double Time = 0.0;
    };
    std::unordered_map<uint32_t, RememberedSummon> LastSummonOfOwner;

    double NextRematchPollTime = 0.0;

};