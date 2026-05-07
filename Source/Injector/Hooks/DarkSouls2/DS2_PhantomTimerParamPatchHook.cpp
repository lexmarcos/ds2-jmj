/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_PhantomTimerParamPatchHook.h"
#include "Injector/Hooks/DarkSouls2/DS2_SessionTraceState.h"
#include "Injector/Config/RuntimeConfig.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"
#include "Shared/Platform/Platform.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
#if defined(_WIN32) && defined(_M_X64)
    constexpr uint8_t kBreakpointOpcode = 0xCC;
    constexpr DWORD64 kTrapFlag = 0x100;
    constexpr size_t kActiveTimerProbeOffset = 0x2c9844;
    constexpr size_t kActiveTimerDirectOffset = 0x0cfc;
    constexpr double kConsolePatchLogIntervalSeconds = 60.0;

    constexpr uint8_t kActiveTimerProbeExpectedBytes[] = {
        0x49, 0x8b, 0x4e, 0x40, 0x48, 0x8b, 0x01, 0xff,
        0x90, 0xd0, 0x00, 0x00, 0x00, 0x84, 0xc0, 0x0f
    };

    std::mutex s_patch_log_mutex;

    bool IsTruthyEnvVar(const char* Name)
    {
        const char* Value = std::getenv(Name);
        if (Value == nullptr)
        {
            return false;
        }

        std::string Text = Value;
        std::transform(
            Text.begin(),
            Text.end(),
            Text.begin(),
            [](unsigned char Ch)
            {
                return (char)std::tolower(Ch);
            });

        return Text == "1" || Text == "true" || Text == "yes" || Text == "on";
    }

    double ResolveTargetSeconds(const RuntimeConfig& Config)
    {
        double TargetSeconds = Config.DS2PhantomTimerSeconds > 0.0 ? Config.DS2PhantomTimerSeconds : 4000.0;

        const char* EnvValue = std::getenv("DS2_PHANTOM_TIMER_SECONDS");
        if (EnvValue != nullptr && EnvValue[0] != '\0')
        {
            char* End = nullptr;
            double Parsed = std::strtod(EnvValue, &End);
            if (End != EnvValue && std::isfinite(Parsed) && Parsed > 0.0)
            {
                TargetSeconds = Parsed;
            }
        }

        return TargetSeconds;
    }

    bool IsReadableProtection(uint32_t Protect)
    {
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
    }

    bool IsReadableRange(size_t Address, size_t Length)
    {
        if (Address == 0 || Length == 0 || Address + Length < Address)
        {
            return false;
        }

        size_t Cursor = Address;
        size_t End = Address + Length;
        while (Cursor < End)
        {
            MEMORY_BASIC_INFORMATION Info = {};
            if (VirtualQuery((void*)Cursor, &Info, sizeof(Info)) == 0)
            {
                return false;
            }

            if (Info.State != MEM_COMMIT || !IsReadableProtection(Info.Protect))
            {
                return false;
            }

            size_t RegionEnd = (size_t)Info.BaseAddress + Info.RegionSize;
            if (RegionEnd <= Cursor)
            {
                return false;
            }

            Cursor = std::min(RegionEnd, End);
        }

        return true;
    }

    bool TryReadBytes(size_t Address, size_t Length, std::vector<uint8_t>& OutBytes)
    {
        if (!IsReadableRange(Address, Length))
        {
            return false;
        }

        OutBytes.resize(Length);
        std::memcpy(OutBytes.data(), (const void*)Address, Length);
        return true;
    }

    bool TryReadFloat(size_t Address, float& OutValue)
    {
        if (!IsReadableRange(Address, sizeof(float)))
        {
            return false;
        }

        std::memcpy(&OutValue, (const void*)Address, sizeof(float));
        return true;
    }

    bool TryWriteFloat(size_t Address, float Value)
    {
        DWORD OldProtect = 0;
        if (!VirtualProtect((void*)Address, sizeof(float), PAGE_EXECUTE_READWRITE, &OldProtect))
        {
            return false;
        }

        std::memcpy((void*)Address, &Value, sizeof(float));

        DWORD RestoreProtect = 0;
        VirtualProtect((void*)Address, sizeof(float), OldProtect, &RestoreProtect);
        return true;
    }

    bool PatchCodeByte(size_t Address, uint8_t Value)
    {
        DWORD OldProtect = 0;
        if (!VirtualProtect((void*)Address, 1, PAGE_EXECUTE_READWRITE, &OldProtect))
        {
            return false;
        }

        std::memcpy((void*)Address, &Value, 1);

        DWORD RestoreProtect = 0;
        VirtualProtect((void*)Address, 1, OldProtect, &RestoreProtect);
        FlushInstructionCache(GetCurrentProcess(), (void*)Address, 1);
        return true;
    }

    void AppendPatchLog(const std::string& Text)
    {
        std::scoped_lock lock(s_patch_log_mutex);

        std::filesystem::path Path = Injector::Instance().GetDllPath() / "DS2_TimerParamPatch.log";
        std::ofstream File(Path, std::ios::out | std::ios::app);
        if (!File.is_open())
        {
            Error("[DS2TimerParamPatch] failed to open trace log: %s", Path.string().c_str());
            return;
        }

        File << Text;
    }

    bool IsValidTimerValue(float Value)
    {
        return std::isfinite(Value) && Value > 0.0f && Value <= 100000.0f;
    }

    class ActiveTimerBreakpointManager;
    ActiveTimerBreakpointManager* s_active_timer_breakpoint_manager = nullptr;
    LONG CALLBACK ActiveTimerVectoredHandler(EXCEPTION_POINTERS* Exception);

    class ActiveTimerBreakpointManager
    {
    public:
        bool Install(size_t BaseAddress, double TargetSeconds)
        {
            std::scoped_lock lock(m_mutex);
            if (m_handler != nullptr)
            {
                return true;
            }

            if (!std::isfinite(TargetSeconds) || TargetSeconds <= 0.0)
            {
                Warning("[DS2TimerParamPatch] active timer patch disabled: invalid target=%.3f", TargetSeconds);
                return true;
            }

            m_breakpoint_address = BaseAddress + kActiveTimerProbeOffset;
            m_target_seconds = (float)TargetSeconds;

            std::vector<uint8_t> ActualBytes;
            if (!TryReadBytes(m_breakpoint_address, sizeof(kActiveTimerProbeExpectedBytes), ActualBytes) ||
                std::memcmp(ActualBytes.data(), kActiveTimerProbeExpectedBytes, sizeof(kActiveTimerProbeExpectedBytes)) != 0)
            {
                Warning("[DS2TimerParamPatch] install_failed reason=signature_mismatch offset=0x%zx", kActiveTimerProbeOffset);
                AppendInstallLog("install_failed", "signature_mismatch");
                return true;
            }

            m_original_byte = kActiveTimerProbeExpectedBytes[0];
            if (!PatchCodeByte(m_breakpoint_address, kBreakpointOpcode))
            {
                Warning("[DS2TimerParamPatch] install_failed reason=patch_failed offset=0x%zx", kActiveTimerProbeOffset);
                AppendInstallLog("install_failed", "patch_failed");
                return true;
            }

            m_installed = true;
            s_active_timer_breakpoint_manager = this;
            m_handler = AddVectoredExceptionHandler(1, ActiveTimerVectoredHandler);
            if (m_handler == nullptr)
            {
                PatchCodeByte(m_breakpoint_address, m_original_byte);
                m_installed = false;
                s_active_timer_breakpoint_manager = nullptr;
                Warning("[DS2TimerParamPatch] install_failed reason=handler_failed offset=0x%zx", kActiveTimerProbeOffset);
                AppendInstallLog("install_failed", "handler_failed");
                return true;
            }

            Log("[DS2TimerParamPatch] active timer patch installed offset=0x%zx target=%.3f source=r14_plus_0xcfc",
                kActiveTimerProbeOffset,
                (double)m_target_seconds);
            AppendInstallLog("installed", "none");
            return true;
        }

        void Uninstall()
        {
            std::scoped_lock lock(m_mutex);
            if (m_handler != nullptr)
            {
                RemoveVectoredExceptionHandler(m_handler);
                m_handler = nullptr;
            }

            if (m_installed)
            {
                PatchCodeByte(m_breakpoint_address, m_original_byte);
                m_installed = false;
            }

            m_pending_single_steps.clear();
            if (s_active_timer_breakpoint_manager == this)
            {
                s_active_timer_breakpoint_manager = nullptr;
            }
        }

        LONG HandleException(EXCEPTION_POINTERS* Exception)
        {
            if (Exception == nullptr ||
                Exception->ExceptionRecord == nullptr ||
                Exception->ContextRecord == nullptr)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            if (Exception->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT)
            {
                return HandleBreakpoint(Exception);
            }

            if (Exception->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP)
            {
                return HandleSingleStep(Exception);
            }

            return EXCEPTION_CONTINUE_SEARCH;
        }

    private:
        LONG HandleBreakpoint(EXCEPTION_POINTERS* Exception)
        {
            CONTEXT* Context = Exception->ContextRecord;
            size_t RipAfterBreakpoint = (size_t)Context->Rip;
            size_t CandidateAddress = RipAfterBreakpoint > 0 ? RipAfterBreakpoint - 1 : 0;
            size_t ExceptionAddress = (size_t)Exception->ExceptionRecord->ExceptionAddress;
            if (CandidateAddress != m_breakpoint_address && ExceptionAddress != m_breakpoint_address)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            DWORD ThreadId = GetCurrentThreadId();
            {
                std::scoped_lock lock(m_mutex);
                if (!m_installed)
                {
                    return EXCEPTION_CONTINUE_SEARCH;
                }

                PatchCodeByte(m_breakpoint_address, m_original_byte);
                m_installed = false;
                m_pending_single_steps[ThreadId] = true;
                m_hit_count++;
            }

            TryPatchActiveTimer(*Context, ThreadId);

            Context->Rip = m_breakpoint_address;
            Context->EFlags |= kTrapFlag;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        LONG HandleSingleStep(EXCEPTION_POINTERS* Exception)
        {
            DWORD ThreadId = GetCurrentThreadId();

            {
                std::scoped_lock lock(m_mutex);
                auto Pending = m_pending_single_steps.find(ThreadId);
                if (Pending == m_pending_single_steps.end())
                {
                    return EXCEPTION_CONTINUE_SEARCH;
                }

                m_pending_single_steps.erase(Pending);
                if (PatchCodeByte(m_breakpoint_address, kBreakpointOpcode))
                {
                    m_installed = true;
                }
                else
                {
                    Warning("[DS2TimerParamPatch] reapply_failed offset=0x%zx", kActiveTimerProbeOffset);
                    AppendInstallLog("reapply_failed", "patch_failed");
                }
            }

            Exception->ContextRecord->EFlags &= ~kTrapFlag;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        void TryPatchActiveTimer(const CONTEXT& Context, DWORD ThreadId)
        {
            double Now = GetSeconds();
            DS2_SessionTraceState::ClientTraceContext Snapshot = DS2_SessionTraceState::GetCurrentSnapshot(Now);
            size_t TimerAddress = (size_t)Context.R14 + kActiveTimerDirectOffset;
            float CurrentSeconds = 0.0f;
            if (!TryReadFloat(TimerAddress, CurrentSeconds) || !IsValidTimerValue(CurrentSeconds))
            {
                return;
            }

            if (CurrentSeconds >= m_target_seconds - 1.0f)
            {
                MaybeLogTimerEvent("already_high_enough", Now, ThreadId, Snapshot, (size_t)Context.R14, TimerAddress, CurrentSeconds, CurrentSeconds);
                return;
            }

            if (!TryWriteFloat(TimerAddress, m_target_seconds))
            {
                MaybeLogTimerEvent("write_failed", Now, ThreadId, Snapshot, (size_t)Context.R14, TimerAddress, CurrentSeconds, CurrentSeconds);
                return;
            }

            m_patch_count++;
            MaybeLogTimerEvent("patched", Now, ThreadId, Snapshot, (size_t)Context.R14, TimerAddress, CurrentSeconds, m_target_seconds);
        }

        void AppendInstallLog(const char* Result, const char* Reason)
        {
            AppendPatchLog(StringFormat(
                "============================================================\n"
                "time=%.3f event=DS2ActiveTimerPatch result=%s reason=%s probe_offset=0x%zx address=0x%016llx target_seconds=%.3f source=r14_plus_0xcfc\n\n",
                GetSeconds(),
                Result,
                Reason,
                kActiveTimerProbeOffset,
                (unsigned long long)m_breakpoint_address,
                (double)m_target_seconds));
        }

        void MaybeLogTimerEvent(
            const char* Result,
            double Now,
            DWORD ThreadId,
            const DS2_SessionTraceState::ClientTraceContext& Snapshot,
            size_t R14,
            size_t TimerAddress,
            float OldSeconds,
            float NewSeconds)
        {
            bool ShouldLogConsole = false;
            {
                std::scoped_lock lock(m_event_mutex);
                const bool IsImportant = std::strcmp(Result, "write_failed") == 0;
                ShouldLogConsole = !m_logged_first_event ||
                                   IsImportant ||
                                   m_last_console_log_at < 0.0 ||
                                   Now - m_last_console_log_at >= kConsolePatchLogIntervalSeconds;
                if (ShouldLogConsole)
                {
                    m_logged_first_event = true;
                    m_last_console_log_at = Now;
                }
            }

            AppendPatchLog(StringFormat(
                "============================================================\n"
                "time=%.3f event=DS2ActiveTimerPatch result=%s source=r14_plus_0xcfc probe_offset=0x%zx thread_id=%u hit_count=%zu patch_count=%zu r14=0x%016llx timer_address=0x%016llx old_seconds=%.3f new_seconds=%.3f target_seconds=%.3f client_session_id=%zu client_session_role=%s client_duration=%.3f client_reason=%s\n\n",
                Now,
                Result,
                kActiveTimerProbeOffset,
                ThreadId,
                m_hit_count.load(),
                m_patch_count.load(),
                (unsigned long long)R14,
                (unsigned long long)TimerAddress,
                OldSeconds,
                NewSeconds,
                (double)m_target_seconds,
                Snapshot.ClientSessionId,
                Snapshot.ClientSessionRole.c_str(),
                Snapshot.ClientSessionDuration,
                Snapshot.ClientLeaveReasonHint.c_str()));

            if (!ShouldLogConsole)
            {
                return;
            }

            if (std::strcmp(Result, "patched") == 0)
            {
                Success("[DS2TimerParamPatch] patched active session timer old=%.3f target=%.3f trace_file=DS2_TimerParamPatch.log",
                    OldSeconds,
                    (double)m_target_seconds);
            }
            else if (std::strcmp(Result, "already_high_enough") == 0)
            {
                Log("[DS2TimerParamPatch] active session timer already high old=%.3f target=%.3f trace_file=DS2_TimerParamPatch.log",
                    OldSeconds,
                    (double)m_target_seconds);
            }
            else if (std::strcmp(Result, "write_failed") == 0)
            {
                Warning("[DS2TimerParamPatch] active session timer write failed old=%.3f target=%.3f trace_file=DS2_TimerParamPatch.log",
                    OldSeconds,
                    (double)m_target_seconds);
            }
        }

        std::mutex m_mutex;
        std::mutex m_event_mutex;
        PVOID m_handler = nullptr;
        bool m_installed = false;
        bool m_logged_first_event = false;
        uint8_t m_original_byte = 0;
        size_t m_breakpoint_address = 0;
        float m_target_seconds = 0.0f;
        double m_last_console_log_at = -1.0;
        std::atomic_size_t m_hit_count{0};
        std::atomic_size_t m_patch_count{0};
        std::unordered_map<DWORD, bool> m_pending_single_steps;
    };

    ActiveTimerBreakpointManager s_active_timer_manager;

    LONG CALLBACK ActiveTimerVectoredHandler(EXCEPTION_POINTERS* Exception)
    {
        if (s_active_timer_breakpoint_manager == nullptr)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        return s_active_timer_breakpoint_manager->HandleException(Exception);
    }
#endif
}

bool DS2_PhantomTimerParamPatchHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    const RuntimeConfig& Config = injector.GetConfig();
    const bool Enabled = Config.DS2PatchPhantomTimers || IsTruthyEnvVar("DS2_PATCH_PHANTOM_TIMERS");
    if (!Enabled)
    {
        return true;
    }

    double TargetSeconds = ResolveTargetSeconds(Config);
    Log("[DS2TimerParamPatch] enabled=1 target_seconds=%.3f config=%u env=%u",
        TargetSeconds,
        Config.DS2PatchPhantomTimers ? 1 : 0,
        IsTruthyEnvVar("DS2_PATCH_PHANTOM_TIMERS") ? 1 : 0);

    return s_active_timer_manager.Install((size_t)injector.GetBaseAddress(), TargetSeconds);
#else
    Warning("[DS2TimerParamPatch] phantom timer patch requires Windows x64; not installing.");
    return true;
#endif
}

void DS2_PhantomTimerParamPatchHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    s_active_timer_manager.Uninstall();
#endif
}

const char* DS2_PhantomTimerParamPatchHook::GetName()
{
    return "DS2 Phantom Timer Patch";
}
