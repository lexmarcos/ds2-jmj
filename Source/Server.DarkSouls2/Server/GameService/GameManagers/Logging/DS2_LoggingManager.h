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

#include <string>
#include <unordered_map>
#include <vector>

struct Frpg2ReliableUdpMessage;
class Server;

// Handles telemetry messages sent by the client, usually this logs things
// like item usage, game settngs, match results, etc.

class DS2_LoggingManager
    : public GameManager
{
public:    
    DS2_LoggingManager(Server* InServerInstance);

    virtual MessageHandleResult OnMessageRecieved(GameClient* Client, const Frpg2ReliableUdpMessage& Message) override;

    virtual void OnLostPlayer(GameClient* Client) override;

    virtual std::string GetName() override;

protected:
    MessageHandleResult Handle_RequestNotifyBuyItem(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestNotifyDeath(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestNotifyDisconnectSession(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestNotifyJoinGuestPlayer(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestNotifyJoinSession(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestNotifyKillEnemy(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestNotifyKillPlayer(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestNotifyLeaveGuestPlayer(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestNotifyLeaveSession(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestNotifyMirrorKnight(GameClient* Client, const Frpg2ReliableUdpMessage& Message);
    MessageHandleResult Handle_RequestNotifyOfflineDeathCount(GameClient* Client, const Frpg2ReliableUdpMessage& Message);

private:
    struct SessionEventTrace
    {
        double Time = 0.0;
        std::string Name;
        long long RemoteProfileId = 0;
        long long Field1 = 0;
        long long Field2 = 0;
        long long Field3 = 0;
        long long Field4 = 0;
        long long Field5 = 0;
    };

    struct ActiveSessionTrace
    {
        unsigned long long SessionId = 0;
        double StartedAt = 0.0;
        uint32_t LocalProfileId = 0;
        int LocalCharacterId = -1;
        uint32_t StartedAreaId = 0;
        uint32_t CurrentAreaId = 0;
        long long RemoteProfileId = 0;
        long long Field2 = 0;
        long long Field3 = 0;
        long long Field4 = 0;
        double LastKillAt = -1.0;
        double LastDeathAt = -1.0;
        double LastDisconnectAt = -1.0;
        std::string Role;
        std::vector<SessionEventTrace> RecentEvents;
    };

    struct ClientEventTrace
    {
        double LastKillAt = -1.0;
        double LastDeathAt = -1.0;
        double LastDisconnectAt = -1.0;
        std::vector<SessionEventTrace> RecentEvents;
    };

    struct RecentLeaveTrace
    {
        double Time = 0.0;
        unsigned long long SessionId = 0;
        unsigned long long LeaveOrder = 0;
        uint32_t LocalProfileId = 0;
        long long RemoteProfileId = 0;
        std::string Role;
        std::string ReasonHint;
    };

    std::string MakeSessionTraceKey(GameClient* Client, long long RemoteProfileId, const char* Role);
    unsigned long long FindExistingSessionIdForPair(uint32_t LocalProfileId, long long RemoteProfileId);
    unsigned long long TrackSessionStart(GameClient* Client, long long RemoteProfileId, long long Field2, long long Field3, long long Field4, const char* Role);
    ActiveSessionTrace PopSessionTrace(GameClient* Client, long long RemoteProfileId, const char* Role, bool& Found);
    void AppendTraceEvent(ActiveSessionTrace& Trace, const SessionEventTrace& Event);
    void AppendClientEvent(uint32_t PlayerId, const SessionEventTrace& Event);
    uint32_t TrackSessionEvent(GameClient* Client, const char* EventName, long long RemoteProfileId, long long Field1, long long Field2, long long Field3, long long Field4, long long Field5);
    std::string FormatRecentEvents(const std::vector<SessionEventTrace>& Events, double Now);
    std::string BuildEvidenceFlags(bool TraceFound, bool RecentKill, bool RecentDeath, bool RecentDisconnect, bool TimerCandidate, bool PairedRemoteLeave, bool ClientDisconnected) const;
    const char* ClassifyLeave(bool RecentKill, bool RecentDeath, bool RecentDisconnect, bool TimerCandidate, bool PairedRemoteLeave, bool ClientDisconnected) const;
    bool FindPairedRemoteLeave(uint32_t LocalProfileId, long long RemoteProfileId, double Now, double& Delta, unsigned long long& PairedSessionId, unsigned long long& PairedLeaveOrder) const;
    void RememberLeave(uint32_t LocalProfileId, long long RemoteProfileId, const char* Role, unsigned long long SessionId, unsigned long long LeaveOrder, const char* ReasonHint, double Now);
    void LogLeaveTrace(GameClient* Client, const char* EventType, const char* Role, const ActiveSessionTrace& Trace, bool TraceFound, long long Field1, long long Field2, long long Field3, long long Field4, bool ClientDisconnected);

    Server* ServerInstance;
    std::unordered_map<std::string, ActiveSessionTrace> ActiveSessionTraces;
    std::unordered_map<uint32_t, ClientEventTrace> RecentClientEvents;
    std::vector<RecentLeaveTrace> RecentLeaves;
    unsigned long long NextSessionTraceId = 1;
    unsigned long long NextLeaveOrder = 1;

};
