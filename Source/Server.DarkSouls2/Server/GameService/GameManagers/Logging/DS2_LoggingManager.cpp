/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Server/GameService/GameManagers/Logging/DS2_LoggingManager.h"
#include "Server/GameService/GameService.h"
#include "Server/GameService/GameClient.h"
#include "Server/Streams/Frpg2ReliableUdpMessage.h"
#include "Server/Streams/Frpg2ReliableUdpMessageStream.h"
#include "Server/Streams/DS2_Frpg2ReliableUdpMessage.h"
#include "Server/GameService/Utils/DS2_GameIds.h"
#include "Server.DarkSouls2/Protobuf/DS2_Protobufs.h"

#include "Config/RuntimeConfig.h"
#include "Server/Server.h"
#include "Server/Database/ServerDatabase.h"
#include "Server/GameService/Utils/DS2_PvpDebug.h"

#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"

#include <algorithm>

namespace
{
    constexpr double kKillDeathEvidenceWindowSeconds = 60.0;
    constexpr double kDisconnectEvidenceWindowSeconds = 30.0;
    constexpr double kPairedLeaveWindowSeconds = 20.0;
    constexpr double kTimerCandidateMinSeconds = 11.5 * 60.0;
    constexpr double kTimerCandidateMaxSeconds = 13.5 * 60.0;
    constexpr size_t kMaxRecentSessionEvents = 8;
}

DS2_LoggingManager::DS2_LoggingManager(Server* InServerInstance)
    : ServerInstance(InServerInstance)
{
}

std::string DS2_LoggingManager::MakeSessionTraceKey(GameClient* Client, long long RemoteProfileId, const char* Role)
{
    PlayerState& Player = Client->GetPlayerState();
    return StringFormat("%u:%lld:%s", Player.GetPlayerId(), RemoteProfileId, Role);
}

unsigned long long DS2_LoggingManager::FindExistingSessionIdForPair(uint32_t LocalProfileId, long long RemoteProfileId)
{
    if (RemoteProfileId <= 0)
    {
        return 0;
    }

    for (const auto& Pair : ActiveSessionTraces)
    {
        const ActiveSessionTrace& Trace = Pair.second;
        if ((long long)Trace.LocalProfileId == RemoteProfileId &&
            Trace.RemoteProfileId == (long long)LocalProfileId)
        {
            return Trace.SessionId;
        }
    }

    return 0;
}

void DS2_LoggingManager::AppendTraceEvent(ActiveSessionTrace& Trace, const SessionEventTrace& Event)
{
    Trace.RecentEvents.push_back(Event);
    if (Trace.RecentEvents.size() > kMaxRecentSessionEvents)
    {
        Trace.RecentEvents.erase(Trace.RecentEvents.begin());
    }

    if (Event.Name == "KillPlayer")
    {
        Trace.LastKillAt = Event.Time;
    }
    else if (Event.Name == "Death")
    {
        Trace.LastDeathAt = Event.Time;
    }
    else if (Event.Name == "DisconnectSession")
    {
        Trace.LastDisconnectAt = Event.Time;
    }
}

void DS2_LoggingManager::AppendClientEvent(uint32_t PlayerId, const SessionEventTrace& Event)
{
    ClientEventTrace& Trace = RecentClientEvents[PlayerId];
    Trace.RecentEvents.push_back(Event);
    if (Trace.RecentEvents.size() > kMaxRecentSessionEvents)
    {
        Trace.RecentEvents.erase(Trace.RecentEvents.begin());
    }

    if (Event.Name == "KillPlayer")
    {
        Trace.LastKillAt = Event.Time;
    }
    else if (Event.Name == "Death")
    {
        Trace.LastDeathAt = Event.Time;
    }
    else if (Event.Name == "DisconnectSession")
    {
        Trace.LastDisconnectAt = Event.Time;
    }
}

unsigned long long DS2_LoggingManager::TrackSessionStart(GameClient* Client, long long RemoteProfileId, long long Field2, long long Field3, long long Field4, const char* Role)
{
    PlayerState& Player = Client->GetPlayerState();
    double Now = GetSeconds();

    ActiveSessionTrace Trace;
    Trace.SessionId = FindExistingSessionIdForPair(Player.GetPlayerId(), RemoteProfileId);
    if (Trace.SessionId == 0)
    {
        Trace.SessionId = NextSessionTraceId++;
    }

    Trace.StartedAt = Now;
    Trace.LocalProfileId = Player.GetPlayerId();
    Trace.LocalCharacterId = Player.GetCharacterId();
    Trace.StartedAreaId = Player.GetCurrentAreaId();
    Trace.CurrentAreaId = Player.GetCurrentAreaId();
    Trace.RemoteProfileId = RemoteProfileId;
    Trace.Field2 = Field2;
    Trace.Field3 = Field3;
    Trace.Field4 = Field4;
    Trace.Role = Role;

    SessionEventTrace Event;
    Event.Time = Now;
    Event.Name = Trace.Role == "session" ? "JoinSession" : "JoinGuestPlayer";
    Event.RemoteProfileId = RemoteProfileId;
    Event.Field1 = RemoteProfileId;
    Event.Field2 = Field2;
    Event.Field3 = Field3;
    Event.Field4 = Field4;

    AppendTraceEvent(Trace, Event);
    AppendClientEvent(Player.GetPlayerId(), Event);

    ActiveSessionTraces[MakeSessionTraceKey(Client, RemoteProfileId, Role)] = Trace;

    return Trace.SessionId;
}

DS2_LoggingManager::ActiveSessionTrace DS2_LoggingManager::PopSessionTrace(GameClient* Client, long long RemoteProfileId, const char* Role, bool& Found)
{
    std::string Key = MakeSessionTraceKey(Client, RemoteProfileId, Role);
    auto Iter = ActiveSessionTraces.find(Key);
    if (Iter == ActiveSessionTraces.end())
    {
        Found = false;
        return {};
    }

    Found = true;
    ActiveSessionTrace Result = Iter->second;
    ActiveSessionTraces.erase(Iter);
    return Result;
}

uint32_t DS2_LoggingManager::TrackSessionEvent(GameClient* Client, const char* EventName, long long RemoteProfileId, long long Field1, long long Field2, long long Field3, long long Field4, long long Field5)
{
    PlayerState& Player = Client->GetPlayerState();
    double Now = GetSeconds();

    SessionEventTrace Event;
    Event.Time = Now;
    Event.Name = EventName;
    Event.RemoteProfileId = RemoteProfileId;
    Event.Field1 = Field1;
    Event.Field2 = Field2;
    Event.Field3 = Field3;
    Event.Field4 = Field4;
    Event.Field5 = Field5;

    AppendClientEvent(Player.GetPlayerId(), Event);

    uint32_t MatchedSessionCount = 0;
    for (auto& Pair : ActiveSessionTraces)
    {
        ActiveSessionTrace& Trace = Pair.second;
        if (Trace.LocalProfileId != Player.GetPlayerId())
        {
            continue;
        }

        if (RemoteProfileId != 0 && Trace.RemoteProfileId != 0 && Trace.RemoteProfileId != RemoteProfileId)
        {
            continue;
        }

        Trace.CurrentAreaId = Player.GetCurrentAreaId();
        AppendTraceEvent(Trace, Event);
        MatchedSessionCount++;
    }

    return MatchedSessionCount;
}

std::string DS2_LoggingManager::FormatRecentEvents(const std::vector<SessionEventTrace>& Events, double Now)
{
    if (Events.empty())
    {
        return "none";
    }

    std::string Result;
    for (const SessionEventTrace& Event : Events)
    {
        if (!Result.empty())
        {
            Result += "|";
        }

        Result += StringFormat("%s:%.3f", Event.Name.c_str(), Now - Event.Time);
    }

    return Result;
}

std::string DS2_LoggingManager::BuildEvidenceFlags(bool TraceFound, bool RecentKill, bool RecentDeath, bool RecentDisconnect, bool TimerCandidate, bool PairedRemoteLeave, bool ClientDisconnected) const
{
    std::vector<std::string> Flags;
    Flags.push_back(TraceFound ? "trace_found" : "trace_missing");

    if (RecentKill)
    {
        Flags.push_back("recent_kill");
    }
    if (RecentDeath)
    {
        Flags.push_back("recent_death");
    }
    if (RecentDisconnect)
    {
        Flags.push_back("recent_disconnect_session");
    }
    if (TimerCandidate)
    {
        Flags.push_back("duration_in_timer_window");
    }
    if (PairedRemoteLeave)
    {
        Flags.push_back("paired_remote_leave");
    }
    if (ClientDisconnected)
    {
        Flags.push_back("client_disconnected");
    }

    std::string Result;
    for (const std::string& Flag : Flags)
    {
        if (!Result.empty())
        {
            Result += "|";
        }
        Result += Flag;
    }

    return Result.empty() ? "none" : Result;
}

const char* DS2_LoggingManager::ClassifyLeave(bool RecentKill, bool RecentDeath, bool RecentDisconnect, bool TimerCandidate, bool PairedRemoteLeave, bool ClientDisconnected) const
{
    if (ClientDisconnected)
    {
        return "client_disconnected";
    }
    if (RecentDisconnect)
    {
        return "disconnect_session";
    }
    if (RecentKill || RecentDeath)
    {
        return "killed_or_death_related";
    }
    if (PairedRemoteLeave)
    {
        return "paired_remote_leave";
    }
    if (TimerCandidate)
    {
        return "timer_candidate";
    }

    return "manual_or_unknown";
}

bool DS2_LoggingManager::FindPairedRemoteLeave(uint32_t LocalProfileId, long long RemoteProfileId, double Now, double& Delta, unsigned long long& PairedSessionId, unsigned long long& PairedLeaveOrder) const
{
    if (RemoteProfileId <= 0)
    {
        return false;
    }

    for (auto Iter = RecentLeaves.rbegin(); Iter != RecentLeaves.rend(); ++Iter)
    {
        const RecentLeaveTrace& Leave = *Iter;
        double CandidateDelta = Now - Leave.Time;
        if (CandidateDelta > kPairedLeaveWindowSeconds)
        {
            break;
        }

        if ((long long)Leave.LocalProfileId == RemoteProfileId &&
            Leave.RemoteProfileId == (long long)LocalProfileId)
        {
            Delta = CandidateDelta;
            PairedSessionId = Leave.SessionId;
            PairedLeaveOrder = Leave.LeaveOrder;
            return true;
        }
    }

    return false;
}

void DS2_LoggingManager::RememberLeave(uint32_t LocalProfileId, long long RemoteProfileId, const char* Role, unsigned long long SessionId, unsigned long long LeaveOrder, const char* ReasonHint, double Now)
{
    RecentLeaves.erase(
        std::remove_if(
            RecentLeaves.begin(),
            RecentLeaves.end(),
            [Now](const RecentLeaveTrace& Leave)
            {
                return Now - Leave.Time > kPairedLeaveWindowSeconds;
            }),
        RecentLeaves.end());

    RecentLeaveTrace Leave;
    Leave.Time = Now;
    Leave.SessionId = SessionId;
    Leave.LeaveOrder = LeaveOrder;
    Leave.LocalProfileId = LocalProfileId;
    Leave.RemoteProfileId = RemoteProfileId;
    Leave.Role = Role;
    Leave.ReasonHint = ReasonHint;
    RecentLeaves.push_back(Leave);
}

void DS2_LoggingManager::LogLeaveTrace(GameClient* Client, const char* EventType, const char* Role, const ActiveSessionTrace& Trace, bool TraceFound, long long Field1, long long Field2, long long Field3, long long Field4, bool ClientDisconnected)
{
    PlayerState& Player = Client->GetPlayerState();
    double Now = GetSeconds();
    uint32_t LocalProfileId = Player.GetPlayerId();
    long long RemoteProfileId = Field1;
    double Duration = TraceFound ? (Now - Trace.StartedAt) : -1.0;

    double LastKillAt = TraceFound ? Trace.LastKillAt : -1.0;
    double LastDeathAt = TraceFound ? Trace.LastDeathAt : -1.0;
    double LastDisconnectAt = TraceFound ? Trace.LastDisconnectAt : -1.0;

    auto ClientEventsIter = RecentClientEvents.find(LocalProfileId);
    if (ClientEventsIter != RecentClientEvents.end())
    {
        const ClientEventTrace& ClientTrace = ClientEventsIter->second;
        double EarliestRelevantEventAt = TraceFound ? Trace.StartedAt : 0.0;
        if (ClientTrace.LastKillAt >= EarliestRelevantEventAt)
        {
            LastKillAt = std::max(LastKillAt, ClientTrace.LastKillAt);
        }
        if (ClientTrace.LastDeathAt >= EarliestRelevantEventAt)
        {
            LastDeathAt = std::max(LastDeathAt, ClientTrace.LastDeathAt);
        }
        if (ClientTrace.LastDisconnectAt >= EarliestRelevantEventAt)
        {
            LastDisconnectAt = std::max(LastDisconnectAt, ClientTrace.LastDisconnectAt);
        }
    }

    double RecentKillDelta = LastKillAt >= 0.0 ? Now - LastKillAt : -1.0;
    double RecentDeathDelta = LastDeathAt >= 0.0 ? Now - LastDeathAt : -1.0;
    double RecentDisconnectDelta = LastDisconnectAt >= 0.0 ? Now - LastDisconnectAt : -1.0;

    bool RecentKill = RecentKillDelta >= 0.0 && RecentKillDelta <= kKillDeathEvidenceWindowSeconds;
    bool RecentDeath = RecentDeathDelta >= 0.0 && RecentDeathDelta <= kKillDeathEvidenceWindowSeconds;
    bool RecentDisconnect = RecentDisconnectDelta >= 0.0 && RecentDisconnectDelta <= kDisconnectEvidenceWindowSeconds;
    bool TimerCandidate = TraceFound &&
                          Duration >= kTimerCandidateMinSeconds &&
                          Duration <= kTimerCandidateMaxSeconds &&
                          !RecentKill &&
                          !RecentDeath &&
                          !RecentDisconnect &&
                          !ClientDisconnected;

    double PairedRemoteLeaveDelta = -1.0;
    unsigned long long PairedSessionId = 0;
    unsigned long long PairedLeaveOrder = 0;
    bool PairedRemoteLeave = !ClientDisconnected && FindPairedRemoteLeave(LocalProfileId, RemoteProfileId, Now, PairedRemoteLeaveDelta, PairedSessionId, PairedLeaveOrder);

    const char* ReasonHint = ClassifyLeave(RecentKill, RecentDeath, RecentDisconnect, TimerCandidate, PairedRemoteLeave, ClientDisconnected);
    std::string EvidenceFlags = BuildEvidenceFlags(TraceFound, RecentKill, RecentDeath, RecentDisconnect, TimerCandidate, PairedRemoteLeave, ClientDisconnected);
    unsigned long long LeaveOrder = NextLeaveOrder++;

    const std::vector<SessionEventTrace>* RecentEvents = TraceFound ? &Trace.RecentEvents : nullptr;
    if (RecentEvents == nullptr && ClientEventsIter != RecentClientEvents.end())
    {
        RecentEvents = &ClientEventsIter->second.RecentEvents;
    }

    std::string RecentEventsText = RecentEvents != nullptr ? FormatRecentEvents(*RecentEvents, Now) : "none";

    DS2PvpDebug::LogEvent(ServerInstance, Client, EventType,
        "leave_reason_hint=%s evidence_flags=%s session_id=%llu session_duration=%.3f recent_kill_delta=%.3f recent_death_delta=%.3f recent_disconnect_delta=%.3f leave_order=%llu paired_remote_leave_delta=%.3f paired_session_id=%llu paired_leave_order=%llu remote_profile_id=%lld field_1=%lld field_2=%lld field_3=%lld field_4=%lld session_role=%s trace_state=%s local_profile_id=%u local_character_id=%d started_area_id=%u current_area_id=%u tracked_current_area_id=%u start_field_2=%lld start_field_3=%lld start_field_4=%lld recent_events=%s",
        ReasonHint,
        EvidenceFlags.c_str(),
        TraceFound ? Trace.SessionId : 0,
        Duration,
        RecentKillDelta,
        RecentDeathDelta,
        RecentDisconnectDelta,
        LeaveOrder,
        PairedRemoteLeaveDelta,
        PairedSessionId,
        PairedLeaveOrder,
        RemoteProfileId,
        Field1,
        Field2,
        Field3,
        Field4,
        Role,
        TraceFound ? "known" : "unknown",
        TraceFound ? Trace.LocalProfileId : LocalProfileId,
        TraceFound ? Trace.LocalCharacterId : Player.GetCharacterId(),
        TraceFound ? Trace.StartedAreaId : 0,
        Player.GetCurrentAreaId(),
        TraceFound ? Trace.CurrentAreaId : Player.GetCurrentAreaId(),
        TraceFound ? Trace.Field2 : 0,
        TraceFound ? Trace.Field3 : 0,
        TraceFound ? Trace.Field4 : 0,
        RecentEventsText.c_str());

    RememberLeave(LocalProfileId, RemoteProfileId, Role, TraceFound ? Trace.SessionId : 0, LeaveOrder, ReasonHint, Now);
}

MessageHandleResult DS2_LoggingManager::OnMessageRecieved(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestNotifyBuyItem))
    {
        return Handle_RequestNotifyBuyItem(Client, Message);
    }
    else if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestNotifyDeath))
    {
        return Handle_RequestNotifyDeath(Client, Message);
    }
    else if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestNotifyDisconnectSession))
    {
        return Handle_RequestNotifyDisconnectSession(Client, Message);
    }
    else if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestNotifyJoinGuestPlayer))
    {
        return Handle_RequestNotifyJoinGuestPlayer(Client, Message);
    }
    else if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestNotifyJoinSession))
    {
        return Handle_RequestNotifyJoinSession(Client, Message);
    }
    else if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestNotifyKillEnemy))
    {
        return Handle_RequestNotifyKillEnemy(Client, Message);
    }
    else if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestNotifyKillPlayer))
    {
        return Handle_RequestNotifyKillPlayer(Client, Message);
    }
    else if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestNotifyLeaveGuestPlayer))
    {
        return Handle_RequestNotifyLeaveGuestPlayer(Client, Message);
    }
    else if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestNotifyLeaveSession))
    {
        return Handle_RequestNotifyLeaveSession(Client, Message);
    }
    else if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestNotifyMirrorKnight))
    {
        return Handle_RequestNotifyMirrorKnight(Client, Message);
    }
    else if (Message.Header.IsType(DS2_Frpg2ReliableUdpMessageType::RequestNotifyOfflineDeathCount))
    {
        return Handle_RequestNotifyOfflineDeathCount(Client, Message);
    }
        
    return MessageHandleResult::Unhandled;
}

void DS2_LoggingManager::OnLostPlayer(GameClient* Client)
{
    PlayerState& Player = Client->GetPlayerState();
    uint32_t LocalProfileId = Player.GetPlayerId();

    for (auto Iter = ActiveSessionTraces.begin(); Iter != ActiveSessionTraces.end(); /* empty */)
    {
        ActiveSessionTrace Trace = Iter->second;
        if (Trace.LocalProfileId != LocalProfileId)
        {
            ++Iter;
            continue;
        }

        LogLeaveTrace(
            Client,
            "SessionTraceCleanup",
            Trace.Role.c_str(),
            Trace,
            true,
            Trace.RemoteProfileId,
            Trace.Field2,
            Trace.Field3,
            Trace.Field4,
            true);

        Iter = ActiveSessionTraces.erase(Iter);
    }

    RecentClientEvents.erase(LocalProfileId);
}

MessageHandleResult DS2_LoggingManager::Handle_RequestNotifyBuyItem(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    ServerDatabase& Database = ServerInstance->GetDatabase();
    PlayerState& Player = Client->GetPlayerState();

    DS2_Frpg2RequestMessage::RequestNotifyBuyItem* Request = (DS2_Frpg2RequestMessage::RequestNotifyBuyItem*)Message.Protobuf.get();

    std::string StatisticKey = StringFormat("Item/TotalPurchased/Id=%u", Request->item_id());
    Database.AddGlobalStatistic(StatisticKey, Request->quantity());
    Database.AddPlayerStatistic(StatisticKey, Player.GetPlayerId(), Request->quantity());

    std::string TotalStatisticKey = StringFormat("Item/TotalPurchased");
    Database.AddGlobalStatistic(TotalStatisticKey, Request->quantity());
    Database.AddPlayerStatistic(TotalStatisticKey, Player.GetPlayerId(), Request->quantity());

    DS2_Frpg2RequestMessage::EmptyResponse Response;
    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send EmptyResponse response.");
        return MessageHandleResult::Error;
    }

    return MessageHandleResult::Handled;
}

MessageHandleResult DS2_LoggingManager::Handle_RequestNotifyDeath(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    ServerDatabase& Database = ServerInstance->GetDatabase();
    PlayerState& Player = Client->GetPlayerState();

    DS2_Frpg2RequestMessage::RequestNotifyDeath* Request = (DS2_Frpg2RequestMessage::RequestNotifyDeath*)Message.Protobuf.get();

    uint32_t MatchedSessionCount = TrackSessionEvent(
        Client,
        "Death",
        0,
        (long long)Request->online_area_id(),
        (long long)Request->cell_id(),
        (long long)Request->field_3(),
        (long long)Request->field_4(),
        (long long)Request->field_5());

    DS2PvpDebug::LogEvent(ServerInstance, Client, "Death",
        "online_area_id=%u cell_id=%u field_3=%lld field_4=%lld field_5=%lld field_6=%lld field_7=%lld payload_size=%u matched_session_count=%u",
        Request->online_area_id(),
        Request->cell_id(),
        (long long)Request->field_3(),
        (long long)Request->field_4(),
        (long long)Request->field_5(),
        (long long)Request->field_6(),
        (long long)Request->field_7(),
        (uint32_t)Request->field_8().size(),
        MatchedSessionCount);

    std::string TotalStatisticKey = StringFormat("Player/TotalDeaths");
    Database.AddGlobalStatistic(TotalStatisticKey, 1);
    Database.AddPlayerStatistic(TotalStatisticKey, Player.GetPlayerId(), 1);

    // TODO: Implement the rest of things from ds3.

    DS2_Frpg2RequestMessage::EmptyResponse Response;
    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send EmptyResponse response.");
        return MessageHandleResult::Error;
    }

    return MessageHandleResult::Handled;
}

MessageHandleResult DS2_LoggingManager::Handle_RequestNotifyDisconnectSession(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    DS2_Frpg2RequestMessage::RequestNotifyDisconnectSession* Request = (DS2_Frpg2RequestMessage::RequestNotifyDisconnectSession*)Message.Protobuf.get();

    uint32_t MatchedSessionCount = TrackSessionEvent(
        Client,
        "DisconnectSession",
        (long long)Request->field_1(),
        (long long)Request->field_1(),
        0,
        0,
        0,
        0);

    DS2PvpDebug::LogEvent(ServerInstance, Client, "DisconnectSession",
        "field_1=%lld remote_profile_id=%lld matched_session_count=%u",
        (long long)Request->field_1(),
        (long long)Request->field_1(),
        MatchedSessionCount);

    // Note: I don't think we really care about this log. We get most of this during the summon flow.

    DS2_Frpg2RequestMessage::EmptyResponse Response;
    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send EmptyResponse response.");
        return MessageHandleResult::Error;
    }

    return MessageHandleResult::Handled;
}

MessageHandleResult DS2_LoggingManager::Handle_RequestNotifyJoinGuestPlayer(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    DS2_Frpg2RequestMessage::RequestNotifyJoinGuestPlayer* Request = (DS2_Frpg2RequestMessage::RequestNotifyJoinGuestPlayer*)Message.Protobuf.get();

    unsigned long long SessionId = TrackSessionStart(
        Client,
        (long long)Request->field_1(),
        (long long)Request->field_2(),
        (long long)Request->field_3(),
        (long long)Request->field_4(),
        "guest_player");

    DS2PvpDebug::LogEvent(ServerInstance, Client, "JoinGuestPlayer",
        "session_id=%llu remote_profile_id=%lld field_1=%lld field_2=%lld field_3=%lld field_4=%lld field_5=%lld field_6=%lld request_area_id=%lld cell_id=%lld payload_size=%u session_role=guest_player trace_state=started",
        SessionId,
        (long long)Request->field_1(),
        (long long)Request->field_1(),
        (long long)Request->field_2(),
        (long long)Request->field_3(),
        (long long)Request->field_4(),
        (long long)Request->field_5(),
        (long long)Request->field_6(),
        (long long)Request->field_7(),
        (long long)Request->field_8(),
        (uint32_t)Request->field_9().size());

    // Note: I don't think we really care about this log. We get most of this during the summon flow.

    DS2_Frpg2RequestMessage::EmptyResponse Response;
    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send EmptyResponse response.");
        return MessageHandleResult::Error;
    }

    return MessageHandleResult::Handled;
}

MessageHandleResult DS2_LoggingManager::Handle_RequestNotifyJoinSession(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    DS2_Frpg2RequestMessage::RequestNotifyJoinSession* Request = (DS2_Frpg2RequestMessage::RequestNotifyJoinSession*)Message.Protobuf.get();

    unsigned long long SessionId = TrackSessionStart(
        Client,
        (long long)Request->field_1(),
        (long long)Request->field_2(),
        (long long)Request->field_3(),
        (long long)Request->field_4(),
        "session");

    DS2PvpDebug::LogEvent(ServerInstance, Client, "JoinSession",
        "session_id=%llu remote_profile_id=%lld field_1=%lld field_2=%lld field_3=%lld field_4=%lld session_role=session trace_state=started",
        SessionId,
        (long long)Request->field_1(),
        (long long)Request->field_1(),
        (long long)Request->field_2(),
        (long long)Request->field_3(),
        (long long)Request->field_4());

    // Note: I don't think we really care about this log. We get most of this during the summon flow.

    DS2_Frpg2RequestMessage::EmptyResponse Response;
    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send EmptyResponse response.");
        return MessageHandleResult::Error;
    }

    return MessageHandleResult::Handled;
}

MessageHandleResult DS2_LoggingManager::Handle_RequestNotifyKillEnemy(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    ServerDatabase& Database = ServerInstance->GetDatabase();
    PlayerState& Player = Client->GetPlayerState();

    DS2_Frpg2RequestMessage::RequestNotifyKillEnemy* Request = (DS2_Frpg2RequestMessage::RequestNotifyKillEnemy*)Message.Protobuf.get();

    int EnemyCount = 0;
    for (int i = 0; i < Request->enemy_count_size(); i++)
    {
        const DS2_Frpg2RequestMessage::RequestNotifyKillEnemy_Enemy_count& EnemyInfo = Request->enemy_count(i);

        std::string StatisticKey = StringFormat("Enemies/TotalKilled/Id=%u", EnemyInfo.enemy_id());
        Database.AddGlobalStatistic(StatisticKey, EnemyInfo.enemy_count());
        Database.AddPlayerStatistic(StatisticKey, Player.GetPlayerId(), EnemyInfo.enemy_count());

        EnemyCount += EnemyInfo.enemy_count();
    }

    std::string TotalStatisticKey = StringFormat("Enemies/TotalKilled");
    Database.AddGlobalStatistic(TotalStatisticKey, EnemyCount);
    Database.AddPlayerStatistic(TotalStatisticKey, Player.GetPlayerId(), EnemyCount);

    DS2_Frpg2RequestMessage::EmptyResponse Response;
    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send EmptyResponse response.");
        return MessageHandleResult::Error;
    }

    return MessageHandleResult::Handled;
}

MessageHandleResult DS2_LoggingManager::Handle_RequestNotifyKillPlayer(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    DS2_Frpg2RequestMessage::RequestNotifyKillPlayer* Request = (DS2_Frpg2RequestMessage::RequestNotifyKillPlayer*)Message.Protobuf.get();

    uint32_t MatchedSessionCount = TrackSessionEvent(
        Client,
        "KillPlayer",
        (long long)Request->field_2(),
        (long long)Request->field_1(),
        (long long)Request->field_2(),
        (long long)Request->field_3(),
        (long long)Request->field_4(),
        (long long)Request->field_5());

    DS2PvpDebug::LogEvent(ServerInstance, Client, "KillPlayer",
        "field_1=%lld remote_profile_id=%lld field_2=%lld field_3=%lld field_4=%lld field_5=%lld matched_session_count=%u",
        (long long)Request->field_1(),
        (long long)Request->field_2(),
        (long long)Request->field_2(),
        (long long)Request->field_3(),
        (long long)Request->field_4(),
        (long long)Request->field_5(),
        MatchedSessionCount);

    // Note: I don't think we really care about this log. We get most of this during the summon flow.

    DS2_Frpg2RequestMessage::EmptyResponse Response;
    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send EmptyResponse response.");
        return MessageHandleResult::Error;
    }

    return MessageHandleResult::Handled;
}

MessageHandleResult DS2_LoggingManager::Handle_RequestNotifyLeaveGuestPlayer(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    DS2_Frpg2RequestMessage::RequestNotifyLeaveGuestPlayer* Request = (DS2_Frpg2RequestMessage::RequestNotifyLeaveGuestPlayer*)Message.Protobuf.get();

    bool Found = false;
    ActiveSessionTrace Trace = PopSessionTrace(Client, (long long)Request->field_1(), "guest_player", Found);
    LogLeaveTrace(
        Client,
        "LeaveGuestPlayer",
        "guest_player",
        Trace,
        Found,
        (long long)Request->field_1(),
        (long long)Request->field_2(),
        (long long)Request->field_3(),
        (long long)Request->field_4(),
        false);

    // Note: I don't think we really care about this log. We get most of this during the summon flow.

    DS2_Frpg2RequestMessage::EmptyResponse Response;
    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send EmptyResponse response.");
        return MessageHandleResult::Error;
    }

    return MessageHandleResult::Handled;
}

MessageHandleResult DS2_LoggingManager::Handle_RequestNotifyLeaveSession(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    ServerDatabase& Database = ServerInstance->GetDatabase();
    PlayerState& Player = Client->GetPlayerState();

    DS2_Frpg2RequestMessage::RequestNotifyLeaveSession* Request = (DS2_Frpg2RequestMessage::RequestNotifyLeaveSession*)Message.Protobuf.get();

    bool Found = false;
    ActiveSessionTrace Trace = PopSessionTrace(Client, (long long)Request->field_1(), "session", Found);
    LogLeaveTrace(
        Client,
        "LeaveSession",
        "session",
        Trace,
        Found,
        (long long)Request->field_1(),
        (long long)Request->field_2(),
        (long long)Request->field_3(),
        (long long)Request->field_4(),
        false);

    std::string TypeStatisticKey = StringFormat("Player/TotalMultiplaySessions");
    Database.AddGlobalStatistic(TypeStatisticKey, 1);
    Database.AddPlayerStatistic(TypeStatisticKey, Player.GetPlayerId(), 1);

    DS2_Frpg2RequestMessage::EmptyResponse Response;
    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send EmptyResponse response.");
        return MessageHandleResult::Error;
    }

    return MessageHandleResult::Handled;
}

MessageHandleResult DS2_LoggingManager::Handle_RequestNotifyMirrorKnight(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    DS2_Frpg2RequestMessage::RequestNotifyMirrorKnight* Request = (DS2_Frpg2RequestMessage::RequestNotifyMirrorKnight*)Message.Protobuf.get();

    // Note: I don't think we really care about this log. We get most of this during the summon flow.

    DS2_Frpg2RequestMessage::EmptyResponse Response;
    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send EmptyResponse response.");
        return MessageHandleResult::Error;
    }

    return MessageHandleResult::Handled;
}

MessageHandleResult DS2_LoggingManager::Handle_RequestNotifyOfflineDeathCount(GameClient* Client, const Frpg2ReliableUdpMessage& Message)
{
    ServerDatabase& Database = ServerInstance->GetDatabase();
    PlayerState& Player = Client->GetPlayerState();

    DS2_Frpg2RequestMessage::RequestNotifyOfflineDeathCount* Request = (DS2_Frpg2RequestMessage::RequestNotifyOfflineDeathCount*)Message.Protobuf.get();

    std::string TotalStatisticKey = StringFormat("Player/TotalDeaths");
    Database.AddGlobalStatistic(TotalStatisticKey, Request->count());
    Database.AddPlayerStatistic(TotalStatisticKey, Player.GetPlayerId(), Request->count());

    DS2_Frpg2RequestMessage::EmptyResponse Response;
    if (!Client->MessageStream->Send(&Response, &Message))
    {
        WarningS(Client->GetName().c_str(), "Disconnecting client as failed to send EmptyResponse response.");
        return MessageHandleResult::Error;
    }

    return MessageHandleResult::Handled;
}

std::string DS2_LoggingManager::GetName()
{
    return "Logging";
}
