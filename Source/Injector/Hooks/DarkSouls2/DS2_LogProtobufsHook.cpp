/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_LogProtobufsHook.h"
#include "Injector/Hooks/DarkSouls2/DS2_SessionTraceState.h"
#include "Injector/Config/BuildConfig.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"
#include "Shared/Core/Utils/Rtti.h"
#include "Shared/Core/Utils/Protobuf.h"
#include "Shared/Core/Utils/File.h"
#include "Shared/Platform/Platform.h"
#include "ThirdParty/detours/src/detours.h"

#include <vector>
#include <iterator>
#include <string>
#include <fstream>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
    DecodedProtobufRegistry s_decoded_registry;
    std::atomic_size_t s_packet_count{0};
    std::atomic_size_t s_trace_sequence{0};
    bool s_log_all_protobufs = false;
    bool s_trace_leave_session = false;
    bool s_prevent_pvp_timer_leave = false;
    double s_pvp_timer_min_seconds = 700.0;
    double s_pvp_timer_max_seconds = 820.0;
    std::mutex s_trace_file_mutex;
    std::mutex s_trace_state_mutex;

    constexpr size_t kMaxDecodedTraceField = 16;

    struct DecodedTraceFields
    {
        long long Values[kMaxDecodedTraceField + 1] = {};
        bool HasValue[kMaxDecodedTraceField + 1] = {};

        long long Get(size_t Field, long long DefaultValue = 0) const
        {
            if (Field <= kMaxDecodedTraceField && HasValue[Field])
            {
                return Values[Field];
            }

            return DefaultValue;
        }
    };

    struct RecentTraceEvent
    {
        double Time = 0.0;
        size_t SequenceId = 0;
        uint32_t ThreadId = 0;
        std::string ClassName;
        std::string StackSignature;
    };

    struct TraceContext
    {
        std::string RecentEventsBefore;
        double RecentJoinSessionDelta = -1.0;
        double RecentJoinGuestPlayerDelta = -1.0;
        double RecentKillDelta = -1.0;
        double RecentDeathDelta = -1.0;
        double RecentDisconnectDelta = -1.0;
        double RecentLeaveSessionDelta = -1.0;
        double RecentLeaveGuestPlayerDelta = -1.0;
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

    void ApplyClientTraceContext(TraceContext& Context, const DS2_SessionTraceState::ClientTraceContext& ClientContext)
    {
        Context.ClientTraceFound = ClientContext.ClientTraceFound;
        Context.ClientTimerCandidate = ClientContext.ClientTimerCandidate;
        Context.ClientPreventConfigured = ClientContext.ClientPreventConfigured;
        Context.ClientPreventWouldApply = ClientContext.ClientPreventWouldApply;
        Context.ClientSessionId = ClientContext.ClientSessionId;
        Context.ClientRemoteProfileId = ClientContext.ClientRemoteProfileId;
        Context.ClientSessionRole = ClientContext.ClientSessionRole;
        Context.ClientLeaveReasonHint = ClientContext.ClientLeaveReasonHint;
        Context.ClientEvidenceFlags = ClientContext.ClientEvidenceFlags;
        Context.ClientRecentEvents = ClientContext.ClientRecentEvents;
        Context.ClientSessionDuration = ClientContext.ClientSessionDuration;
        Context.ClientRecentKillDelta = ClientContext.ClientRecentKillDelta;
        Context.ClientRecentDeathDelta = ClientContext.ClientRecentDeathDelta;
        Context.ClientRecentDisconnectDelta = ClientContext.ClientRecentDisconnectDelta;
    }

    std::deque<RecentTraceEvent> s_recent_trace_events;

    using SerializeWithCachedSizesToArray_p = uint8_t*(*)(void* this_ptr, uint8_t* target);
    SerializeWithCachedSizesToArray_p s_original_SerializeWithCachedSizesToArray;

    bool IsLeaveSessionTraceMessage(const std::string& ClassName)
    {
        return ClassName == "RequestNotifyLeaveSession" ||
               ClassName == "RequestNotifyLeaveGuestPlayer" ||
               ClassName == "RequestNotifyDisconnectSession";
    }

    bool IsSessionTraceMessage(const std::string& ClassName)
    {
        return IsLeaveSessionTraceMessage(ClassName) ||
               ClassName == "RequestNotifyJoinSession" ||
               ClassName == "RequestNotifyJoinGuestPlayer" ||
               ClassName == "RequestNotifyKillPlayer" ||
               ClassName == "RequestNotifyDeath";
    }

    std::string ToLowerAscii(const std::string& Input)
    {
        std::string Result = Input;
        for (char& Ch : Result)
        {
            Ch = (char)std::tolower((unsigned char)Ch);
        }
        return Result;
    }

    bool IsDarkSoulsIIFrame(const Callstack::Frame& Frame)
    {
        std::string Module = ToLowerAscii(Frame.Module);
        return Module == "darksoulsii" || Module == "darksoulsii.exe";
    }

    uint32_t GetCurrentTraceThreadId()
    {
#ifdef _WIN32
        return (uint32_t)GetCurrentThreadId();
#else
        return 0;
#endif
    }

    std::string BytesToHex(const uint8_t* Data, size_t Length)
    {
        std::string Result;
        Result.reserve(Length * 3);

        constexpr char HexChars[] = "0123456789abcdef";
        for (size_t i = 0; i < Length; i++)
        {
            if (i > 0)
            {
                Result += ' ';
            }

            uint8_t Value = Data[i];
            Result += HexChars[(Value >> 4) & 0xF];
            Result += HexChars[Value & 0xF];
        }

        return Result;
    }

    bool ReadTraceVarint(const uint8_t*& Cursor, const uint8_t* End, uint64_t& Value)
    {
        Value = 0;

        for (int Shift = 0; Shift <= 63 && Cursor < End; Shift += 7)
        {
            uint8_t Byte = *Cursor++;
            Value |= (uint64_t)(Byte & 0x7F) << Shift;
            if ((Byte & 0x80) == 0)
            {
                return true;
            }
        }

        return false;
    }

    DecodedTraceFields DecodeTraceFields(const uint8_t* Data, size_t Length)
    {
        DecodedTraceFields Result;

        const uint8_t* Cursor = Data;
        const uint8_t* End = Data + Length;
        while (Cursor < End)
        {
            uint64_t Tag = 0;
            if (!ReadTraceVarint(Cursor, End, Tag))
            {
                break;
            }

            size_t Field = (size_t)(Tag >> 3);
            uint64_t WireType = Tag & 0x7;

            if (WireType == 0)
            {
                uint64_t Value = 0;
                if (!ReadTraceVarint(Cursor, End, Value))
                {
                    break;
                }

                if (Field <= kMaxDecodedTraceField)
                {
                    Result.Values[Field] = (long long)Value;
                    Result.HasValue[Field] = true;
                }
            }
            else if (WireType == 1)
            {
                if ((size_t)(End - Cursor) < 8)
                {
                    break;
                }
                Cursor += 8;
            }
            else if (WireType == 2)
            {
                uint64_t FieldLength = 0;
                if (!ReadTraceVarint(Cursor, End, FieldLength) || (uint64_t)(End - Cursor) < FieldLength)
                {
                    break;
                }
                Cursor += FieldLength;
            }
            else if (WireType == 5)
            {
                if ((size_t)(End - Cursor) < 4)
                {
                    break;
                }
                Cursor += 4;
            }
            else
            {
                break;
            }
        }

        return Result;
    }

    DS2_SessionTraceState::TraceFields ToSessionTraceFields(const DecodedTraceFields& Fields)
    {
        DS2_SessionTraceState::TraceFields Result;
        for (size_t Field = 0; Field <= std::min(kMaxDecodedTraceField, DS2_SessionTraceState::kMaxTraceField); Field++)
        {
            Result.Values[Field] = Fields.Values[Field];
            Result.HasValue[Field] = Fields.HasValue[Field];
        }

        return Result;
    }

    std::filesystem::path GetProtobufDumpPath(const std::string& Filename)
    {
        std::error_code Error;
        std::filesystem::path Directory = std::filesystem::temp_directory_path(Error);
        if (Error)
        {
            Directory = Injector::Instance().GetDllPath();
            Error.clear();
        }

        Directory /= "DS3OS";
        Directory /= "ProtobufDump";

        std::filesystem::create_directories(Directory, Error);
        if (Error)
        {
            return Injector::Instance().GetDllPath() / Filename;
        }

        return Directory / Filename;
    }

    void HashBytes(uint64_t& Hash, const void* Data, size_t Length)
    {
        const uint8_t* Bytes = reinterpret_cast<const uint8_t*>(Data);
        for (size_t i = 0; i < Length; i++)
        {
            Hash ^= Bytes[i];
            Hash *= 1099511628211ull;
        }
    }

    void HashString(uint64_t& Hash, const std::string& Text)
    {
        HashBytes(Hash, Text.data(), Text.size());
    }

    std::string BuildStackSignature(const Callstack& Stack, intptr_t BaseAddress)
    {
        uint64_t Hash = 1469598103934665603ull;
        size_t FrameCount = std::min<size_t>(Stack.Frames.size(), 16);

        for (size_t i = 0; i < FrameCount; i++)
        {
            const Callstack::Frame& Frame = Stack.Frames[i];

            HashString(Hash, ToLowerAscii(Frame.Module));
            HashString(Hash, Frame.Function);

            size_t AddressComponent = Frame.Address;
            if (IsDarkSoulsIIFrame(Frame) && BaseAddress != 0 && Frame.Address >= (size_t)BaseAddress)
            {
                AddressComponent = Frame.Address - (size_t)BaseAddress;
            }

            HashBytes(Hash, &AddressComponent, sizeof(AddressComponent));
        }

        return StringFormat("0x%016llx", (unsigned long long)Hash);
    }

    void UpdateDeltaForClass(TraceContext& Context, const RecentTraceEvent& Event, double Now)
    {
        double Delta = Now - Event.Time;
        if (Event.ClassName == "RequestNotifyJoinSession" && Context.RecentJoinSessionDelta < 0.0)
        {
            Context.RecentJoinSessionDelta = Delta;
        }
        else if (Event.ClassName == "RequestNotifyJoinGuestPlayer" && Context.RecentJoinGuestPlayerDelta < 0.0)
        {
            Context.RecentJoinGuestPlayerDelta = Delta;
        }
        else if (Event.ClassName == "RequestNotifyKillPlayer" && Context.RecentKillDelta < 0.0)
        {
            Context.RecentKillDelta = Delta;
        }
        else if (Event.ClassName == "RequestNotifyDeath" && Context.RecentDeathDelta < 0.0)
        {
            Context.RecentDeathDelta = Delta;
        }
        else if (Event.ClassName == "RequestNotifyDisconnectSession" && Context.RecentDisconnectDelta < 0.0)
        {
            Context.RecentDisconnectDelta = Delta;
        }
        else if (Event.ClassName == "RequestNotifyLeaveSession" && Context.RecentLeaveSessionDelta < 0.0)
        {
            Context.RecentLeaveSessionDelta = Delta;
        }
        else if (Event.ClassName == "RequestNotifyLeaveGuestPlayer" && Context.RecentLeaveGuestPlayerDelta < 0.0)
        {
            Context.RecentLeaveGuestPlayerDelta = Delta;
        }
    }

    TraceContext RememberTraceEventAndBuildContext(double Now, size_t SequenceId, uint32_t ThreadId, const std::string& ClassName, const std::string& StackSignature, const DecodedTraceFields& Fields)
    {
        constexpr double kMaxRecentEventAgeSeconds = 120.0;
        constexpr size_t kMaxRecentEvents = 16;
        constexpr size_t kRecentEventsToFormat = 8;

        TraceContext Context;
        {
            std::scoped_lock lock(s_trace_state_mutex);

            size_t FormattedCount = 0;
            for (auto Iter = s_recent_trace_events.rbegin(); Iter != s_recent_trace_events.rend(); ++Iter)
            {
                const RecentTraceEvent& Event = *Iter;
                double Delta = Now - Event.Time;
                if (Delta > kMaxRecentEventAgeSeconds)
                {
                    break;
                }

                UpdateDeltaForClass(Context, Event, Now);

                if (FormattedCount < kRecentEventsToFormat)
                {
                    if (!Context.RecentEventsBefore.empty())
                    {
                        Context.RecentEventsBefore += "|";
                    }

                    Context.RecentEventsBefore += StringFormat(
                        "%zu:%s:%.3f:t%u:%s",
                        Event.SequenceId,
                        Event.ClassName.c_str(),
                        Delta,
                        Event.ThreadId,
                        Event.StackSignature.c_str());
                    FormattedCount++;
                }
            }

            if (Context.RecentEventsBefore.empty())
            {
                Context.RecentEventsBefore = "none";
            }

            double OldestAllowedTime = Now - kMaxRecentEventAgeSeconds;
            s_recent_trace_events.erase(
                std::remove_if(
                    s_recent_trace_events.begin(),
                    s_recent_trace_events.end(),
                    [OldestAllowedTime](const RecentTraceEvent& Event)
                    {
                        return Event.Time < OldestAllowedTime;
                    }),
                s_recent_trace_events.end());

            while (s_recent_trace_events.size() >= kMaxRecentEvents)
            {
                s_recent_trace_events.pop_front();
            }

            RecentTraceEvent Event;
            Event.Time = Now;
            Event.SequenceId = SequenceId;
            Event.ThreadId = ThreadId;
            Event.ClassName = ClassName;
            Event.StackSignature = StackSignature;
            s_recent_trace_events.push_back(Event);
        }

        ApplyClientTraceContext(
            Context,
            DS2_SessionTraceState::RememberTraceEventAndBuildContext(
                Now,
                SequenceId,
                ThreadId,
                ClassName,
                StackSignature,
                ToSessionTraceFields(Fields)));

        return Context;
    }

    void AppendTraceLog(const std::string& Text)
    {
        std::scoped_lock lock(s_trace_file_mutex);

        std::filesystem::path Path = Injector::Instance().GetDllPath() / "DS2_LeaveSessionTrace.log";
        std::ofstream File(Path, std::ios::out | std::ios::app | std::ios::binary);
        if (!File.is_open())
        {
            Error("Failed to open DS2 leave-session trace log: %s", Path.string().c_str());
            return;
        }

        File << Text;
    }

    bool IsReadableProtection(uint32_t Protect)
    {
#ifdef _WIN32
        if ((Protect & PAGE_GUARD) != 0 || (Protect & PAGE_NOACCESS) != 0)
        {
            return false;
        }

        uint32_t BaseProtect = Protect & 0xFF;
        return BaseProtect == PAGE_READONLY ||
               BaseProtect == PAGE_READWRITE ||
               BaseProtect == PAGE_WRITECOPY ||
               BaseProtect == PAGE_EXECUTE_READ ||
               BaseProtect == PAGE_EXECUTE_READWRITE ||
               BaseProtect == PAGE_EXECUTE_WRITECOPY;
#else
        return false;
#endif
    }

    bool TryReadProcessBytes(size_t Address, size_t Length, std::string& OutHex)
    {
#ifdef _WIN32
        size_t Offset = 0;
        while (Offset < Length)
        {
            MEMORY_BASIC_INFORMATION Info = {};
            if (VirtualQuery((void*)(Address + Offset), &Info, sizeof(Info)) == 0)
            {
                return false;
            }

            if (Info.State != MEM_COMMIT || !IsReadableProtection(Info.Protect))
            {
                return false;
            }

            size_t RegionEnd = (size_t)Info.BaseAddress + Info.RegionSize;
            if (RegionEnd <= Address + Offset)
            {
                return false;
            }

            Offset = std::min(Length, RegionEnd - Address);
        }

        OutHex = BytesToHex((const uint8_t*)Address, Length);
        return true;
#else
        return false;
#endif
    }

    std::string BuildTimerCandidateProbe(const Callstack& Stack, const TraceContext& Context, intptr_t BaseAddress)
    {
        if (!Context.ClientTimerCandidate)
        {
            return "";
        }

        std::string Result;
        Result += StringFormat(
            "timer_candidate_probe prevent_configured=%u prevent_would_apply=%u prevent_action=%s timer_min=%.3f timer_max=%.3f\n",
            Context.ClientPreventConfigured ? 1 : 0,
            Context.ClientPreventWouldApply ? 1 : 0,
            Context.ClientPreventWouldApply ? "not_applied_no_timer_patch_hook" : "none",
            s_pvp_timer_min_seconds,
            s_pvp_timer_max_seconds);

        constexpr size_t kCodeRadius = 32;
        constexpr size_t kMaxProbeFrames = 16;
        size_t ProbeFrameCount = 0;
        for (size_t FrameIndex = 0; FrameIndex < Stack.Frames.size() && ProbeFrameCount < kMaxProbeFrames; FrameIndex++)
        {
            const Callstack::Frame& Frame = Stack.Frames[FrameIndex];
            if (!IsDarkSoulsIIFrame(Frame))
            {
                continue;
            }

            std::string CodeHex = "<unreadable>";
            if (Frame.Address > kCodeRadius)
            {
                TryReadProcessBytes(Frame.Address - kCodeRadius, (kCodeRadius * 2) + 1, CodeHex);
            }

            std::string RelativeAddress = "unknown";
            if (BaseAddress != 0 && Frame.Address >= (size_t)BaseAddress)
            {
                RelativeAddress = StringFormat("0x%zx", Frame.Address - (size_t)BaseAddress);
            }

            Result += StringFormat(
                "timer_candidate_frame index=%zu address=0x%016zx game_rel=%s code_radius=%u code_hex=%s\n",
                FrameIndex,
                Frame.Address,
                RelativeAddress.c_str(),
                (uint32_t)kCodeRadius,
                CodeHex.c_str());

            ProbeFrameCount++;
        }

        return Result;
    }

    void TraceSessionMessage(const std::string& RttiName, const std::string& ClassName, const uint8_t* Data, size_t Length)
    {
        intptr_t BaseAddress = Injector::Instance().GetBaseAddress();
        double Now = GetSeconds();
        size_t SequenceId = s_trace_sequence.fetch_add(1) + 1;
        uint32_t ThreadId = GetCurrentTraceThreadId();
        DecodedTraceFields Fields = DecodeTraceFields(Data, Length);

        DecodedProtobufRegistry Registry;
        const DecodedProtobufMessage* Message = Registry.Decode(ClassName, Data, Length);
        std::string Decoded = Message != nullptr ? Registry.ToString() : "<decode failed>\n";

        std::unique_ptr<Callstack> Stack = CaptureCallstack(0, 48);
        std::string StackSignature = BuildStackSignature(*Stack, BaseAddress);
        TraceContext Context = RememberTraceEventAndBuildContext(Now, SequenceId, ThreadId, ClassName, StackSignature, Fields);

        std::string Trace;
        Trace += "============================================================\n";
        Trace += StringFormat("time=%.3f event=DS2ClientSessionTrace trace_scope=%s class=%s rtti=%s size=%u base=0x%p thread_id=%u sequence_id=%zu stack_signature=%s\n",
            Now,
            IsLeaveSessionTraceMessage(ClassName) ? "leave" : "session_event",
            ClassName.c_str(),
            RttiName.c_str(),
            (uint32_t)Length,
            (void*)BaseAddress,
            ThreadId,
            SequenceId,
            StackSignature.c_str());
        Trace += StringFormat("recent_events_before=%s recent_join_session_delta=%.3f recent_join_guest_player_delta=%.3f recent_kill_delta=%.3f recent_death_delta=%.3f recent_disconnect_delta=%.3f recent_leave_session_delta=%.3f recent_leave_guest_player_delta=%.3f\n",
            Context.RecentEventsBefore.c_str(),
            Context.RecentJoinSessionDelta,
            Context.RecentJoinGuestPlayerDelta,
            Context.RecentKillDelta,
            Context.RecentDeathDelta,
            Context.RecentDisconnectDelta,
            Context.RecentLeaveSessionDelta,
            Context.RecentLeaveGuestPlayerDelta);
        Trace += StringFormat("client_trace_state=%s client_session_id=%zu client_session_role=%s client_remote_profile_id=%lld client_leave_reason_hint=%s client_evidence_flags=%s client_session_duration=%.3f client_recent_kill_delta=%.3f client_recent_death_delta=%.3f client_recent_disconnect_delta=%.3f client_prevent_timer_configured=%u client_prevent_timer_would_apply=%u client_recent_events=%s\n",
            Context.ClientTraceFound ? "known" : "unknown",
            Context.ClientSessionId,
            Context.ClientSessionRole.c_str(),
            Context.ClientRemoteProfileId,
            Context.ClientLeaveReasonHint.c_str(),
            Context.ClientEvidenceFlags.c_str(),
            Context.ClientSessionDuration,
            Context.ClientRecentKillDelta,
            Context.ClientRecentDeathDelta,
            Context.ClientRecentDisconnectDelta,
            Context.ClientPreventConfigured ? 1 : 0,
            Context.ClientPreventWouldApply ? 1 : 0,
            Context.ClientRecentEvents.c_str());
        Trace += "raw_hex=" + BytesToHex(Data, Length) + "\n";
        Trace += "decoded_protobuf:\n";
        Trace += Decoded;
        Trace += "callstack:\n";

        size_t FrameIndex = 0;
        for (const Callstack::Frame& Frame : Stack->Frames)
        {
            std::string RelativeAddress = "";
            if (IsDarkSoulsIIFrame(Frame) && BaseAddress != 0 && Frame.Address >= (size_t)BaseAddress)
            {
                RelativeAddress = StringFormat(" game_rel=0x%zx", Frame.Address - (size_t)BaseAddress);
            }

            Trace += StringFormat(
                "  #%02zu address=0x%016zx%s module=%s function=%s file=%s:%zu\n",
                FrameIndex++,
                Frame.Address,
                RelativeAddress.c_str(),
                Frame.Module.empty() ? "<unknown>" : Frame.Module.c_str(),
                Frame.Function.empty() ? "<unknown>" : Frame.Function.c_str(),
                Frame.Filename.empty() ? "<unknown>" : Frame.Filename.c_str(),
                Frame.Line);
        }

        Trace += BuildTimerCandidateProbe(*Stack, Context, BaseAddress);
        Trace += "\n";

        Log("[DS2LeaveTrace] class=%s size=%u sequence_id=%zu thread_id=%u stack_signature=%s client_reason=%s client_duration=%.3f prevent_timer=%u prevent_would_apply=%u callstack_frames=%u trace_file=DS2_LeaveSessionTrace.log",
            ClassName.c_str(),
            (uint32_t)Length,
            SequenceId,
            ThreadId,
            StackSignature.c_str(),
            Context.ClientLeaveReasonHint.c_str(),
            Context.ClientSessionDuration,
            Context.ClientPreventConfigured ? 1 : 0,
            Context.ClientPreventWouldApply ? 1 : 0,
            (uint32_t)Stack->Frames.size());

        AppendTraceLog(Trace);
    }

    uint8_t* SerializeWithCachedSizesToArrayHook(void* this_ptr, uint8_t* target)
    {
        std::string RttiName = GetRttiNameFromObject(this_ptr);
        std::string ClassName = RttiName;
        if (size_t pos = ClassName.find_last_of(':'); pos != std::string::npos)
        {
            ClassName = ClassName.substr(pos + 1);
        }

        // Log that this protobuf was sent.
        if (s_log_all_protobufs)
        {
            Log(">> %s", RttiName.c_str());
        }

        // Pass over to original serialization function to encode.
        uint8_t* end = s_original_SerializeWithCachedSizesToArray(this_ptr, target);

        // Store protobuf on disk if needed.
        if constexpr (BuildConfig::WRITE_OUT_PROTOBUFS)
        {
            if (s_log_all_protobufs)
            {
                std::vector<uint8_t> bytes(target, end);
                WriteBytesToFile(GetProtobufDumpPath(StringFormat("%llu_%s.bin", s_packet_count.fetch_add(1), ClassName.c_str())), bytes);
            }
        }

        // Store decoded protobuf.
        if (s_log_all_protobufs)
        {
            s_decoded_registry.Decode(ClassName, target, end - target);
        }

        // Store decoded protobuf on disk if needed.
        if constexpr (BuildConfig::WRITE_OUT_DECODED_PROTOBUFS)
        {
            if (s_log_all_protobufs)
            {
                WriteTextToFile(GetProtobufDumpPath("decoded.proto"), s_decoded_registry.ToString());
            }
        }        

        if (s_trace_leave_session && IsSessionTraceMessage(ClassName))
        {
            TraceSessionMessage(RttiName, ClassName, target, end - target);
        }

        return end;
    }

    using ParseFromArray_p = bool (*)(void* this_ptr, void* data, int size);
    ParseFromArray_p s_original_ParseFromArray;

    bool ParseFromArrayHook(void* this_ptr, void* data, int size)
    {
        std::string RttiName = GetRttiNameFromObject(this_ptr);
        std::string ClassName = RttiName;
        if (size_t pos = ClassName.find_last_of(':'); pos != std::string::npos)
        {
            ClassName = ClassName.substr(pos + 1);
        }

        // Log that this protobuf was recieved.
        Log("<< %s", RttiName.c_str());

        // Store protobuf on disk if needed.
        if constexpr (BuildConfig::WRITE_OUT_PROTOBUFS)
        {
            std::vector<uint8_t> bytes((uint8_t*)data, (uint8_t*)data + size);
            WriteBytesToFile(GetProtobufDumpPath(StringFormat("%llu_%s.bin", s_packet_count.fetch_add(1), ClassName.c_str())), bytes);
        }

        // Store decoded version of the protobuf.
        s_decoded_registry.Decode(ClassName, (uint8_t*)data, size);

        // Store decoded protobuf on disk if needed.
        if constexpr (BuildConfig::WRITE_OUT_DECODED_PROTOBUFS)
        {
            WriteTextToFile(GetProtobufDumpPath("decoded.proto"), s_decoded_registry.ToString());
        }

        // Pass over the original parsing function.
        bool result = s_original_ParseFromArray(this_ptr, data, size);
        if (!result)
        {
            Error("!! Failed to parse incoming protobuf, check format from ds3os.");
        }

        return result;
    }
};

bool DS2_LogProtobufsHook::Install_SerializeWithCachedSizesToArray(Injector& injector)
{
    // This is the prolog of SerializeWithCachedSizesToArray
    std::vector<intptr_t> matches = injector.SearchAOB({
        0x40, 0x55,
        0x56,
        0x57,
        0x48, 0x81, 0xec, 0xd0, 0x00, 0x00, 0x00,
        0x48, 0xc7, 0x44, 0x24, 0x28, 0xfe, 0xff, 0xff, 0xff,
        0x48, 0x89, 0x9c, 0x24, 0x00, 0x01, 0x00, 0x00,
        {}, {}, {}, {}, {}, {}, {}, // skip abs address.
        0x48, 0x33, 0xc4,
        0x48, 0x89, 0x84, 0x24, 0xc0, 0x00, 0x00, 0x00,
        0x48, 0x8b, 0xf2,
        0x48, 0x8b, 0xd9,
        0x33, 0xff,
        0x89, 0x7c, 0x24, 0x24,
        0x48, 0x8b, 0x01,
        0xff, 0x50, 0x58,
        0x48, 0x63, 0xe8,
        0x41, 0x83, 0xc9, 0xff,
        0x44, 0x8b, 0xc5,
        0x48, 0x8b, 0xd6,
        0x48, 0x8d, 0x4c, 0x24, 0x50
        });

    if (matches.size() == 0)
    {
        Error("Failed to find injection point for logging protobufs.");
        return false;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    s_original_SerializeWithCachedSizesToArray = reinterpret_cast<SerializeWithCachedSizesToArray_p>(matches[0]);
    DetourAttach(&(PVOID&)s_original_SerializeWithCachedSizesToArray, SerializeWithCachedSizesToArrayHook);

    DetourTransactionCommit();

    return true;
}

bool DS2_LogProtobufsHook::Install_ParseFromArray(Injector& injector)
{
    // This is the prolog of ParseFromArray
    std::vector<intptr_t> matches = injector.SearchAOB({
        0x49, 0x8b, 0xc0,
        0x4c, 0x8b, 0xca,
        0x4c, 0x8b, 0xc1,
        0x48, 0x8b, 0xd0,
        0x49, 0x8b, 0xc9,
        0xe9, 0xac, 0xfe,
        0xff, 0xff,
    });

    if (matches.size() == 0)
    {
        Error("Failed to find injection point for logging protobufs.");
        return false;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    s_original_ParseFromArray = reinterpret_cast<ParseFromArray_p>(matches[0]);
    DetourAttach(&(PVOID&)s_original_ParseFromArray, ParseFromArrayHook);

    DetourTransactionCommit();

    return true;
}

bool DS2_LogProtobufsHook::Install(Injector& injector)
{
#ifdef _DEBUG
    s_log_all_protobufs = true;
#else
    s_log_all_protobufs = false;
#endif
    const RuntimeConfig& Config = injector.GetConfig();
    s_prevent_pvp_timer_leave = Config.DS2PreventPvpTimerLeave;
    s_pvp_timer_min_seconds = Config.DS2PvpTimerMinSeconds > 0.0 ? Config.DS2PvpTimerMinSeconds : 700.0;
    s_pvp_timer_max_seconds = Config.DS2PvpTimerMaxSeconds > 0.0 ? Config.DS2PvpTimerMaxSeconds : 820.0;
    if (s_pvp_timer_max_seconds < s_pvp_timer_min_seconds)
    {
        s_pvp_timer_max_seconds = s_pvp_timer_min_seconds;
    }

    const char* PreventTimerEnv = std::getenv("DS2_PREVENT_PVP_TIMER_LEAVE");
    if (PreventTimerEnv != nullptr)
    {
        std::string PreventTimerEnvText = ToLowerAscii(PreventTimerEnv);
        if (PreventTimerEnvText == "1" || PreventTimerEnvText == "true")
        {
            s_prevent_pvp_timer_leave = true;
        }
    }

    s_trace_leave_session = Config.DS2TraceLeaveSession || s_prevent_pvp_timer_leave;

    DS2_SessionTraceState::Config SessionTraceConfig;
    SessionTraceConfig.PreventPvpTimerLeave = s_prevent_pvp_timer_leave;
    SessionTraceConfig.PvpTimerMinSeconds = s_pvp_timer_min_seconds;
    SessionTraceConfig.PvpTimerMaxSeconds = s_pvp_timer_max_seconds;
    DS2_SessionTraceState::Configure(SessionTraceConfig);

    Log("[DS2LeaveTrace] trace=%u prevent_timer=%u timer_min=%.3f timer_max=%.3f",
        s_trace_leave_session ? 1 : 0,
        s_prevent_pvp_timer_leave ? 1 : 0,
        s_pvp_timer_min_seconds,
        s_pvp_timer_max_seconds);

    bool Installed = Install_SerializeWithCachedSizesToArray(injector);

#ifdef _DEBUG
    Installed = Installed && Install_ParseFromArray(injector);
#endif

    return Installed;
}

void DS2_LogProtobufsHook::Uninstall()
{

}

const char* DS2_LogProtobufsHook::GetName()
{
    return s_log_all_protobufs ? "DS2 Log Protobufs" : "DS2 Trace Leave Session";
}

