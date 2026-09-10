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
#include "Protobuf/DS2_Protobufs.h"

#include <memory>

struct Frpg2ReliableUdpMessage;
class Server;
class GameService;

// Handles client requests for invading other games.

class DS2_BreakInManager
    : public GameManager
{
public:    
    DS2_BreakInManager(Server* InServerInstance, GameService* InGameServiceInstance);

    virtual MessageHandleResult OnMessageReceived(GameClient* Client, const Frpg2ReliableUdpMessage& Message) override;

    virtual std::string GetName() override;

    virtual void OnLostPlayer(GameClient* Client) override;

    virtual void Poll() override;

protected:
    bool CanMatchWith(const DS2_Frpg2RequestMessage::MatchingParameter& Client, const std::shared_ptr<GameClient>& Match, DS2_Frpg2RequestMessage::BreakInType Type);

    MessageHandleResult Handle_RequestGetBreakInTargetList(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestBreakInTarget(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestRejectBreakInTarget(GameClient* Client, const Frpg2ReliableUdpMessage& Message);

private:
    // Fires an invasion straight from a request file, skipping the item and
    // the target list. This exists so the one open question, whether a client
    // accepts the invasion push where the game allows no online activity, can
    // be asked without first solving how the invader gets an orb.
    void PollDebugInvadeRequest();

private:
    Server* ServerInstance;
    GameService* GameServiceInstance;

    double NextDebugPollTime = 0.0;

};