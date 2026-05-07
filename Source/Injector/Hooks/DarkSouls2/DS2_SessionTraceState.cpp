/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_SessionTraceState.h"
#include "Shared/Core/Utils/Strings.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace DS2_SessionTraceState
{
namespace
{
    constexpr double kKillDeathEvidenceWindowSeconds = 30.0;
    constexpr double kDisconnectEvidenceWindowSeconds = 30.0;

    struct ClientSessionEvent
    {
        double Time = 0.0;
        size_t SequenceId = 0;
        uint32_t ThreadId = 0;
        std::string ClassName;
        std::string StackSignature;
    };

    struct ClientSessionTrace
    {
        size_t SessionId = 0;
        std::string Role;
        long long RemoteProfileId = 0;
        long long Field1 = 0;
        long long Field2 = 0;
        long long Field3 = 0;
        long long Field4 = 0;
        double StartedAt = 0.0;
        double LastKillAt = -1.0;
        double LastDeathAt = -1.0;
        double LastDisconnectAt = -1.0;
        std::vector<ClientSessionEvent> RecentEvents;
    };

    std::mutex s_trace_state_mutex;
    std::vector<ClientSessionTrace> s_active_client_sessions;
    size_t s_next_client_session_id = 1;
    Config s_config;

    void AddClientSessionEvent(ClientSessionTrace& Trace, double Now, size_t SequenceId, uint32_t ThreadId, const std::string& ClassName, const std::string& StackSignature)
    {
        constexpr size_t kMaxClientSessionEvents = 16;

        while (Trace.RecentEvents.size() >= kMaxClientSessionEvents)
        {
            Trace.RecentEvents.erase(Trace.RecentEvents.begin());
        }

        ClientSessionEvent Event;
        Event.Time = Now;
        Event.SequenceId = SequenceId;
        Event.ThreadId = ThreadId;
        Event.ClassName = ClassName;
        Event.StackSignature = StackSignature;
        Trace.RecentEvents.push_back(Event);
    }

    std::string FormatClientSessionEvents(const std::vector<ClientSessionEvent>& Events, double Now)
    {
        constexpr size_t kMaxEventsToFormat = 8;

        std::string Result;
        size_t Start = Events.size() > kMaxEventsToFormat ? Events.size() - kMaxEventsToFormat : 0;
        for (size_t i = Start; i < Events.size(); i++)
        {
            if (!Result.empty())
            {
                Result += "|";
            }

            const ClientSessionEvent& Event = Events[i];
            Result += StringFormat(
                "%zu:%s:%.3f:t%u:%s",
                Event.SequenceId,
                Event.ClassName.c_str(),
                Now - Event.Time,
                Event.ThreadId,
                Event.StackSignature.c_str());
        }

        return Result.empty() ? "none" : Result;
    }

    std::string BuildClientEvidenceFlags(bool TraceFound, bool RecentKill, bool RecentDeath, bool RecentDisconnect, bool TimerCandidate)
    {
        std::string Result;
        auto AddFlag = [&Result](const char* Flag)
        {
            if (!Result.empty())
            {
                Result += "|";
            }
            Result += Flag;
        };

        if (TraceFound)
        {
            AddFlag("trace_found");
        }
        if (RecentKill)
        {
            AddFlag("recent_kill");
        }
        if (RecentDeath)
        {
            AddFlag("recent_death");
        }
        if (RecentDisconnect)
        {
            AddFlag("recent_disconnect");
        }
        if (TimerCandidate)
        {
            AddFlag("duration_in_timer_window");
        }

        return Result.empty() ? "none" : Result;
    }

    const char* ClassifyClientLeave(bool RecentKill, bool RecentDeath, bool RecentDisconnect, bool TimerCandidate)
    {
        if (RecentDisconnect)
        {
            return "disconnect_session";
        }
        if (RecentKill || RecentDeath)
        {
            return "killed_or_death_related";
        }
        if (TimerCandidate)
        {
            return "timer_candidate";
        }
        return "manual_or_unknown";
    }

    ClientTraceContext BuildClientContextFromSession(const ClientSessionTrace& Trace, double Now, bool EvaluateLeaveCandidate)
    {
        ClientTraceContext Context;

        double Duration = Now - Trace.StartedAt;
        double RecentKillDelta = Trace.LastKillAt >= 0.0 ? Now - Trace.LastKillAt : -1.0;
        double RecentDeathDelta = Trace.LastDeathAt >= 0.0 ? Now - Trace.LastDeathAt : -1.0;
        double RecentDisconnectDelta = Trace.LastDisconnectAt >= 0.0 ? Now - Trace.LastDisconnectAt : -1.0;
        bool RecentKill = RecentKillDelta >= 0.0 && RecentKillDelta <= kKillDeathEvidenceWindowSeconds;
        bool RecentDeath = RecentDeathDelta >= 0.0 && RecentDeathDelta <= kKillDeathEvidenceWindowSeconds;
        bool RecentDisconnect = RecentDisconnectDelta >= 0.0 && RecentDisconnectDelta <= kDisconnectEvidenceWindowSeconds;
        bool TimerCandidate = EvaluateLeaveCandidate &&
                              Duration >= s_config.PvpTimerMinSeconds &&
                              Duration <= s_config.PvpTimerMaxSeconds &&
                              !RecentKill &&
                              !RecentDeath &&
                              !RecentDisconnect;

        Context.ClientTraceFound = true;
        Context.ClientTimerCandidate = TimerCandidate;
        Context.ClientPreventConfigured = s_config.PreventPvpTimerLeave;
        Context.ClientPreventWouldApply = s_config.PreventPvpTimerLeave && TimerCandidate;
        Context.ClientSessionId = Trace.SessionId;
        Context.ClientRemoteProfileId = Trace.RemoteProfileId;
        Context.ClientSessionRole = Trace.Role;
        Context.ClientSessionDuration = Duration;
        Context.ClientRecentKillDelta = RecentKillDelta;
        Context.ClientRecentDeathDelta = RecentDeathDelta;
        Context.ClientRecentDisconnectDelta = RecentDisconnectDelta;
        Context.ClientEvidenceFlags = BuildClientEvidenceFlags(true, RecentKill, RecentDeath, RecentDisconnect, TimerCandidate);
        Context.ClientLeaveReasonHint = EvaluateLeaveCandidate ? ClassifyClientLeave(RecentKill, RecentDeath, RecentDisconnect, TimerCandidate) : "not_applicable";
        Context.ClientRecentEvents = FormatClientSessionEvents(Trace.RecentEvents, Now);

        return Context;
    }

    size_t FindClientSessionIndex(const std::string& Role, long long RemoteProfileId)
    {
        size_t FallbackIndex = (size_t)-1;
        size_t FallbackCount = 0;
        for (size_t i = 0; i < s_active_client_sessions.size(); i++)
        {
            const ClientSessionTrace& Trace = s_active_client_sessions[i];
            if (Trace.Role != Role)
            {
                continue;
            }

            if (Trace.RemoteProfileId == RemoteProfileId)
            {
                return i;
            }

            FallbackIndex = i;
            FallbackCount++;
        }

        return FallbackCount == 1 ? FallbackIndex : (size_t)-1;
    }

    size_t FindCurrentSessionIndex()
    {
        if (s_active_client_sessions.empty())
        {
            return (size_t)-1;
        }

        auto GetSessionSortTime = [](const ClientSessionTrace& Trace)
        {
            return Trace.RecentEvents.empty() ? Trace.StartedAt : Trace.RecentEvents.back().Time;
        };

        size_t BestIndex = 0;
        double BestTime = GetSessionSortTime(s_active_client_sessions[0]);
        for (size_t i = 1; i < s_active_client_sessions.size(); i++)
        {
            const ClientSessionTrace& Trace = s_active_client_sessions[i];
            double Time = GetSessionSortTime(Trace);
            if (Time >= BestTime)
            {
                BestTime = Time;
                BestIndex = i;
            }
        }

        return BestIndex;
    }

    ClientTraceContext StartClientSession(double Now, size_t SequenceId, uint32_t ThreadId, const std::string& ClassName, const std::string& StackSignature, const TraceFields& Fields, const char* Role)
    {
        long long RemoteProfileId = Fields.Get(1);

        s_active_client_sessions.erase(
            std::remove_if(
                s_active_client_sessions.begin(),
                s_active_client_sessions.end(),
                [Role, RemoteProfileId](const ClientSessionTrace& Trace)
                {
                    return Trace.Role == Role && Trace.RemoteProfileId == RemoteProfileId;
                }),
            s_active_client_sessions.end());

        ClientSessionTrace Trace;
        Trace.SessionId = s_next_client_session_id++;
        Trace.Role = Role;
        Trace.RemoteProfileId = RemoteProfileId;
        Trace.Field1 = Fields.Get(1);
        Trace.Field2 = Fields.Get(2);
        Trace.Field3 = Fields.Get(3);
        Trace.Field4 = Fields.Get(4);
        Trace.StartedAt = Now;
        AddClientSessionEvent(Trace, Now, SequenceId, ThreadId, ClassName, StackSignature);

        ClientTraceContext Context;
        Context.ClientTraceFound = true;
        Context.ClientPreventConfigured = s_config.PreventPvpTimerLeave;
        Context.ClientSessionId = Trace.SessionId;
        Context.ClientRemoteProfileId = RemoteProfileId;
        Context.ClientSessionRole = Role;
        Context.ClientSessionDuration = 0.0;
        Context.ClientEvidenceFlags = "trace_found";
        Context.ClientRecentEvents = FormatClientSessionEvents(Trace.RecentEvents, Now);

        s_active_client_sessions.push_back(Trace);

        return Context;
    }

    ClientTraceContext UpdateClientSessionsForEvent(double Now, size_t SequenceId, uint32_t ThreadId, const std::string& ClassName, const std::string& StackSignature, const TraceFields& Fields)
    {
        if (ClassName == "RequestNotifyDeath")
        {
            for (ClientSessionTrace& Trace : s_active_client_sessions)
            {
                Trace.LastDeathAt = Now;
                AddClientSessionEvent(Trace, Now, SequenceId, ThreadId, ClassName, StackSignature);
            }
            if (s_active_client_sessions.size() == 1)
            {
                return BuildClientContextFromSession(s_active_client_sessions.front(), Now, false);
            }
            return ClientTraceContext();
        }

        if (ClassName == "RequestNotifyKillPlayer")
        {
            long long RemoteProfileId = Fields.Get(2);
            bool Matched = false;
            size_t MatchedIndex = (size_t)-1;
            for (ClientSessionTrace& Trace : s_active_client_sessions)
            {
                if (Trace.RemoteProfileId == RemoteProfileId)
                {
                    Trace.LastKillAt = Now;
                    AddClientSessionEvent(Trace, Now, SequenceId, ThreadId, ClassName, StackSignature);
                    Matched = true;
                    MatchedIndex = (size_t)(&Trace - s_active_client_sessions.data());
                }
            }

            if (!Matched && s_active_client_sessions.size() == 1)
            {
                ClientSessionTrace& Trace = s_active_client_sessions.front();
                Trace.LastKillAt = Now;
                AddClientSessionEvent(Trace, Now, SequenceId, ThreadId, ClassName, StackSignature);
                MatchedIndex = 0;
            }

            if (MatchedIndex != (size_t)-1)
            {
                return BuildClientContextFromSession(s_active_client_sessions[MatchedIndex], Now, false);
            }
            return ClientTraceContext();
        }

        if (ClassName == "RequestNotifyDisconnectSession")
        {
            long long RemoteProfileId = Fields.Get(1);
            size_t MatchedIndex = (size_t)-1;
            for (ClientSessionTrace& Trace : s_active_client_sessions)
            {
                if (Trace.RemoteProfileId == RemoteProfileId)
                {
                    Trace.LastDisconnectAt = Now;
                    AddClientSessionEvent(Trace, Now, SequenceId, ThreadId, ClassName, StackSignature);
                    MatchedIndex = (size_t)(&Trace - s_active_client_sessions.data());
                }
            }
            if (MatchedIndex != (size_t)-1)
            {
                return BuildClientContextFromSession(s_active_client_sessions[MatchedIndex], Now, false);
            }
            return ClientTraceContext();
        }

        return ClientTraceContext();
    }

    ClientTraceContext FinishClientSession(double Now, const std::string& Role, long long RemoteProfileId)
    {
        size_t SessionIndex = FindClientSessionIndex(Role, RemoteProfileId);
        if (SessionIndex == (size_t)-1)
        {
            ClientTraceContext Context;
            Context.ClientPreventConfigured = s_config.PreventPvpTimerLeave;
            Context.ClientRemoteProfileId = RemoteProfileId;
            Context.ClientSessionRole = Role;
            return Context;
        }

        ClientTraceContext Context = BuildClientContextFromSession(s_active_client_sessions[SessionIndex], Now, true);
        s_active_client_sessions.erase(s_active_client_sessions.begin() + SessionIndex);
        return Context;
    }
}

    void Configure(const Config& NewConfig)
    {
        std::scoped_lock lock(s_trace_state_mutex);

        s_config = NewConfig;
        if (s_config.PvpTimerMinSeconds <= 0.0)
        {
            s_config.PvpTimerMinSeconds = 700.0;
        }
        if (s_config.PvpTimerMaxSeconds <= 0.0)
        {
            s_config.PvpTimerMaxSeconds = 820.0;
        }
        if (s_config.PvpTimerMaxSeconds < s_config.PvpTimerMinSeconds)
        {
            s_config.PvpTimerMaxSeconds = s_config.PvpTimerMinSeconds;
        }
    }

    ClientTraceContext RememberTraceEventAndBuildContext(
        double Now,
        size_t SequenceId,
        uint32_t ThreadId,
        const std::string& ClassName,
        const std::string& StackSignature,
        const TraceFields& Fields)
    {
        std::scoped_lock lock(s_trace_state_mutex);

        if (ClassName == "RequestNotifyJoinSession")
        {
            return StartClientSession(Now, SequenceId, ThreadId, ClassName, StackSignature, Fields, "session");
        }
        else if (ClassName == "RequestNotifyJoinGuestPlayer")
        {
            return StartClientSession(Now, SequenceId, ThreadId, ClassName, StackSignature, Fields, "guest_player");
        }
        else if (ClassName == "RequestNotifyLeaveSession")
        {
            return FinishClientSession(Now, "session", Fields.Get(1));
        }
        else if (ClassName == "RequestNotifyLeaveGuestPlayer")
        {
            return FinishClientSession(Now, "guest_player", Fields.Get(1));
        }

        return UpdateClientSessionsForEvent(Now, SequenceId, ThreadId, ClassName, StackSignature, Fields);
    }

    ClientTraceContext GetCurrentSnapshot(double Now)
    {
        std::scoped_lock lock(s_trace_state_mutex);

        size_t SessionIndex = FindCurrentSessionIndex();
        if (SessionIndex == (size_t)-1)
        {
            ClientTraceContext Context;
            Context.ClientPreventConfigured = s_config.PreventPvpTimerLeave;
            return Context;
        }

        return BuildClientContextFromSession(s_active_client_sessions[SessionIndex], Now, true);
    }
}
