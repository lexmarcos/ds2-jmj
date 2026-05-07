/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace DS2_SessionTraceState
{
    constexpr size_t kMaxTraceField = 16;

    struct Config
    {
        bool PreventPvpTimerLeave = false;
        double PvpTimerMinSeconds = 700.0;
        double PvpTimerMaxSeconds = 820.0;
    };

    struct TraceFields
    {
        long long Values[kMaxTraceField + 1] = {};
        bool HasValue[kMaxTraceField + 1] = {};

        long long Get(size_t Field, long long DefaultValue = 0) const
        {
            if (Field <= kMaxTraceField && HasValue[Field])
            {
                return Values[Field];
            }

            return DefaultValue;
        }
    };

    struct ClientTraceContext
    {
        bool ClientTraceFound = false;
        bool ClientTimerCandidate = false;
        bool ClientPreventConfigured = false;
        bool ClientPreventWouldApply = false;
        size_t ClientSessionId = 0;
        long long ClientRemoteProfileId = 0;
        std::string ClientSessionRole = "none";
        std::string ClientLeaveReasonHint = "not_applicable";
        std::string ClientEvidenceFlags = "none";
        std::string ClientRecentEvents = "none";
        double ClientSessionDuration = -1.0;
        double ClientRecentKillDelta = -1.0;
        double ClientRecentDeathDelta = -1.0;
        double ClientRecentDisconnectDelta = -1.0;
    };

    void Configure(const Config& NewConfig);

    ClientTraceContext RememberTraceEventAndBuildContext(
        double Now,
        size_t SequenceId,
        uint32_t ThreadId,
        const std::string& ClassName,
        const std::string& StackSignature,
        const TraceFields& Fields);

    ClientTraceContext GetCurrentSnapshot(double Now);
}
