/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_StateProbeHook.h"
#include "Injector/Hooks/DarkSouls2/DS2_SessionTraceState.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"
#include "Shared/Platform/Platform.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#if defined(_WIN32) && defined(_M_X64)

namespace
{
    constexpr uint8_t kBreakpointOpcode = 0xCC;
    constexpr uint32_t kTrapFlag = 0x100;
    constexpr size_t kPointerWindowBefore = 0x20;
    constexpr size_t kPointerWindowLength = 0x60;

    std::mutex s_probe_file_mutex;
    std::atomic_size_t s_hit_sequence{0};

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

    std::string BytesToHex(const std::vector<uint8_t>& Data)
    {
        return Data.empty() ? "" : BytesToHex(Data.data(), Data.size());
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

            Cursor = std::min(End, RegionEnd);
        }

        return true;
    }

    bool TryReadProcessBytes(size_t Address, size_t Length, std::vector<uint8_t>& OutBytes)
    {
        if (!IsReadableRange(Address, Length))
        {
            return false;
        }

        OutBytes.resize(Length);
        SIZE_T BytesRead = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)Address, OutBytes.data(), Length, &BytesRead) || BytesRead != Length)
        {
            OutBytes.clear();
            return false;
        }

        return true;
    }

    bool TryReadQword(size_t Address, uint64_t& OutValue, std::string& OutHex)
    {
        std::vector<uint8_t> Bytes;
        if (!TryReadProcessBytes(Address, sizeof(uint64_t), Bytes))
        {
            return false;
        }

        std::memcpy(&OutValue, Bytes.data(), sizeof(uint64_t));
        OutHex = BytesToHex(Bytes);
        return true;
    }

    bool PatchByte(size_t Address, uint8_t Value)
    {
        DWORD OldProtect = 0;
        if (!VirtualProtect((LPVOID)Address, 1, PAGE_EXECUTE_READWRITE, &OldProtect))
        {
            return false;
        }

        *reinterpret_cast<volatile uint8_t*>(Address) = Value;
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)Address, 1);

        DWORD IgnoredProtect = 0;
        VirtualProtect((LPVOID)Address, 1, OldProtect, &IgnoredProtect);
        return true;
    }

    void AppendProbeLog(const std::string& Text)
    {
        std::scoped_lock lock(s_probe_file_mutex);

        std::filesystem::path Path = Injector::Instance().GetDllPath() / "DS2_StateProbeTrace.log";
        std::ofstream File(Path, std::ios::out | std::ios::app | std::ios::binary);
        if (!File.is_open())
        {
            Error("Failed to open DS2 state-probe trace log: %s", Path.string().c_str());
            return;
        }

        File << Text;
    }

    void LogProbeInstallFailure(size_t Offset, size_t Address, const char* Reason, const std::vector<uint8_t>& Expected, const std::vector<uint8_t>& Actual)
    {
        std::string ActualText = Actual.empty() ? "<unreadable>" : BytesToHex(Actual);
        std::string Text = StringFormat(
            "state_probe_install_failed probe_offset=0x%zx absolute_address=0x%016llx reason=%s expected=%s actual=%s\n",
            Offset,
            (unsigned long long)Address,
            Reason,
            BytesToHex(Expected).c_str(),
            ActualText.c_str());

        Warning("[DS2StateProbe] %s", Text.c_str());
        AppendProbeLog(Text);
    }

    void AppendPointerDump(std::string& Trace, const char* RegisterName, size_t Pointer)
    {
        size_t WindowStart = Pointer > kPointerWindowBefore ? Pointer - kPointerWindowBefore : Pointer;
        std::vector<uint8_t> WindowBytes;
        bool WindowReadable = Pointer != 0 && TryReadProcessBytes(WindowStart, kPointerWindowLength, WindowBytes);

        Trace += StringFormat(
            "pointer_memory register=%s value=0x%016llx window_start=0x%016llx window_size=0x%zx readable=%u bytes=%s\n",
            RegisterName,
            (unsigned long long)Pointer,
            (unsigned long long)WindowStart,
            kPointerWindowLength,
            WindowReadable ? 1 : 0,
            WindowReadable ? BytesToHex(WindowBytes).c_str() : "<unreadable>");

        constexpr size_t CommonOffsets[] = { 0xF8, 0x198, 0xD8, 0x40 };
        for (size_t Offset : CommonOffsets)
        {
            size_t ReadAddress = Pointer + Offset;
            uint64_t QwordValue = 0;
            std::string QwordHex;
            bool Readable = Pointer != 0 && ReadAddress >= Pointer && TryReadQword(ReadAddress, QwordValue, QwordHex);

            Trace += StringFormat(
                "pointer_offset_read register=%s base=0x%016llx offset=0x%zx address=0x%016llx readable=%u qword=0x%016llx bytes=%s\n",
                RegisterName,
                (unsigned long long)Pointer,
                Offset,
                (unsigned long long)ReadAddress,
                Readable ? 1 : 0,
                (unsigned long long)(Readable ? QwordValue : 0),
                Readable ? QwordHex.c_str() : "<unreadable>");
        }
    }

    void AppendClientSnapshot(std::string& Trace)
    {
        DS2_SessionTraceState::ClientTraceContext Context = DS2_SessionTraceState::GetCurrentSnapshot(GetSeconds());

        Trace += StringFormat(
            "client_trace_state=%s client_session_id=%zu client_session_role=%s client_remote_profile_id=%lld client_leave_reason_hint=%s client_evidence_flags=%s client_session_duration=%.3f client_recent_kill_delta=%.3f client_recent_death_delta=%.3f client_recent_disconnect_delta=%.3f client_prevent_timer_configured=%u client_prevent_timer_would_apply=%u client_recent_events=%s\n",
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
    }

    struct ProbeBreakpoint
    {
        size_t Offset = 0;
        size_t Address = 0;
        std::vector<uint8_t> ExpectedBytes;
        uint8_t OriginalByte = 0;
        bool Installed = false;
    };

    class VectoredBreakpointManager
    {
    public:
        bool Install(size_t BaseAddress)
        {
            std::scoped_lock lock(m_mutex);

            if (m_handler != nullptr)
            {
                return true;
            }

            m_base_address = BaseAddress;
            m_breakpoints.clear();
            m_pending_single_steps.clear();

            g_manager = this;
            m_handler = AddVectoredExceptionHandler(1, VectoredExceptionHandler);
            if (m_handler == nullptr)
            {
                g_manager = nullptr;
                LogProbeInstallFailure(0, 0, "add_vectored_exception_handler_failed", {}, {});
                return true;
            }

            InstallProbe(0x2c3c19, { 0x4c, 0x8b, 0xb4, 0x24, 0xe0, 0x00, 0x00, 0x00, 0xc7, 0x87, 0xf8, 0x00, 0x00, 0x00, 0x09, 0x00 });
            InstallProbe(0x2c36d7, { 0xeb, 0x24, 0x0f, 0x28, 0xce, 0x48, 0x8b, 0xcb, 0xe8, 0x9c, 0x05 });
            InstallProbe(0x2c9844, { 0x49, 0x8b, 0x4e, 0x40, 0x48, 0x8b, 0x01, 0xff, 0x90, 0xd0, 0x00, 0x00, 0x00, 0x84, 0xc0, 0x0f });

            if (m_breakpoints.empty())
            {
                RemoveVectoredExceptionHandler(m_handler);
                m_handler = nullptr;
                g_manager = nullptr;
            }

            Log("[DS2StateProbe] installed_probes=%u trace_file=DS2_StateProbeTrace.log", (uint32_t)m_breakpoints.size());
            return true;
        }

        void Uninstall()
        {
            std::scoped_lock lock(m_mutex);

            for (ProbeBreakpoint& Breakpoint : m_breakpoints)
            {
                if (Breakpoint.Installed)
                {
                    PatchByte(Breakpoint.Address, Breakpoint.OriginalByte);
                    Breakpoint.Installed = false;
                }
            }

            m_breakpoints.clear();
            m_pending_single_steps.clear();

            if (m_handler != nullptr)
            {
                RemoveVectoredExceptionHandler(m_handler);
                m_handler = nullptr;
            }

            if (g_manager == this)
            {
                g_manager = nullptr;
            }
        }

        LONG HandleException(EXCEPTION_POINTERS* ExceptionInfo)
        {
            if (ExceptionInfo == nullptr || ExceptionInfo->ExceptionRecord == nullptr || ExceptionInfo->ContextRecord == nullptr)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            DWORD ExceptionCode = ExceptionInfo->ExceptionRecord->ExceptionCode;
            if (ExceptionCode == EXCEPTION_BREAKPOINT)
            {
                return HandleBreakpoint(ExceptionInfo);
            }
            if (ExceptionCode == EXCEPTION_SINGLE_STEP)
            {
                return HandleSingleStep(ExceptionInfo);
            }

            return EXCEPTION_CONTINUE_SEARCH;
        }

    private:
        void InstallProbe(size_t Offset, std::initializer_list<uint8_t> ExpectedBytes)
        {
            ProbeBreakpoint Breakpoint;
            Breakpoint.Offset = Offset;
            Breakpoint.Address = m_base_address + Offset;
            Breakpoint.ExpectedBytes.assign(ExpectedBytes.begin(), ExpectedBytes.end());
            Breakpoint.OriginalByte = Breakpoint.ExpectedBytes.empty() ? 0 : Breakpoint.ExpectedBytes[0];

            std::vector<uint8_t> ActualBytes;
            if (!TryReadProcessBytes(Breakpoint.Address, Breakpoint.ExpectedBytes.size(), ActualBytes))
            {
                LogProbeInstallFailure(Breakpoint.Offset, Breakpoint.Address, "signature_unreadable", Breakpoint.ExpectedBytes, ActualBytes);
                return;
            }

            if (ActualBytes != Breakpoint.ExpectedBytes)
            {
                LogProbeInstallFailure(Breakpoint.Offset, Breakpoint.Address, "signature_mismatch", Breakpoint.ExpectedBytes, ActualBytes);
                return;
            }

            if (!PatchByte(Breakpoint.Address, kBreakpointOpcode))
            {
                LogProbeInstallFailure(Breakpoint.Offset, Breakpoint.Address, "patch_failed", Breakpoint.ExpectedBytes, ActualBytes);
                return;
            }

            Breakpoint.Installed = true;
            m_breakpoints.push_back(Breakpoint);
        }

        bool FindInstalledBreakpoint(size_t Address, size_t& OutIndex)
        {
            for (size_t i = 0; i < m_breakpoints.size(); i++)
            {
                const ProbeBreakpoint& Breakpoint = m_breakpoints[i];
                if (Breakpoint.Installed && Breakpoint.Address == Address)
                {
                    OutIndex = i;
                    return true;
                }
            }

            return false;
        }

        LONG HandleBreakpoint(EXCEPTION_POINTERS* ExceptionInfo)
        {
            CONTEXT* Context = ExceptionInfo->ContextRecord;
            size_t RipAfterBreakpoint = (size_t)Context->Rip;
            size_t CandidateAddress = RipAfterBreakpoint > 0 ? RipAfterBreakpoint - 1 : 0;
            size_t ExceptionAddress = (size_t)ExceptionInfo->ExceptionRecord->ExceptionAddress;

            ProbeBreakpoint Breakpoint;
            CONTEXT LogContext = *Context;
            DWORD ThreadId = GetCurrentThreadId();
            bool Found = false;

            {
                std::scoped_lock lock(m_mutex);

                size_t BreakpointIndex = 0;
                Found = FindInstalledBreakpoint(CandidateAddress, BreakpointIndex) ||
                        FindInstalledBreakpoint(ExceptionAddress, BreakpointIndex);
                if (!Found)
                {
                    return EXCEPTION_CONTINUE_SEARCH;
                }

                ProbeBreakpoint& LiveBreakpoint = m_breakpoints[BreakpointIndex];
                if (!PatchByte(LiveBreakpoint.Address, LiveBreakpoint.OriginalByte))
                {
                    LogProbeInstallFailure(LiveBreakpoint.Offset, LiveBreakpoint.Address, "restore_original_failed", LiveBreakpoint.ExpectedBytes, {});
                    return EXCEPTION_CONTINUE_SEARCH;
                }

                LiveBreakpoint.Installed = false;
                m_pending_single_steps[ThreadId] = BreakpointIndex;
                Breakpoint = LiveBreakpoint;
            }

            Context->Rip = Breakpoint.Address;
            Context->EFlags |= kTrapFlag;

            LogContext.Rip = Breakpoint.Address;
            LogProbeHit(Breakpoint, LogContext, ThreadId);

            return EXCEPTION_CONTINUE_EXECUTION;
        }

        LONG HandleSingleStep(EXCEPTION_POINTERS* ExceptionInfo)
        {
            DWORD ThreadId = GetCurrentThreadId();

            {
                std::scoped_lock lock(m_mutex);

                auto PendingIter = m_pending_single_steps.find(ThreadId);
                if (PendingIter == m_pending_single_steps.end())
                {
                    return EXCEPTION_CONTINUE_SEARCH;
                }

                size_t BreakpointIndex = PendingIter->second;
                m_pending_single_steps.erase(PendingIter);

                if (BreakpointIndex < m_breakpoints.size())
                {
                    ProbeBreakpoint& Breakpoint = m_breakpoints[BreakpointIndex];
                    if (PatchByte(Breakpoint.Address, kBreakpointOpcode))
                    {
                        Breakpoint.Installed = true;
                    }
                    else
                    {
                        Breakpoint.Installed = false;
                        std::string Text = StringFormat(
                            "state_probe_reapply_failed probe_offset=0x%zx absolute_address=0x%016llx\n",
                            Breakpoint.Offset,
                            (unsigned long long)Breakpoint.Address);
                        Error("[DS2StateProbe] %s", Text.c_str());
                        AppendProbeLog(Text);
                    }
                }
            }

            ExceptionInfo->ContextRecord->EFlags &= ~kTrapFlag;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        void LogProbeHit(const ProbeBreakpoint& Breakpoint, const CONTEXT& Context, DWORD ThreadId)
        {
            size_t SequenceId = s_hit_sequence.fetch_add(1) + 1;
            double Now = GetSeconds();

            std::string Trace;
            Trace += "============================================================\n";
            Trace += StringFormat(
                "time=%.3f event=DS2StateProbe probe_offset=0x%zx absolute_address=0x%016llx thread_id=%u hit_sequence_id=%zu base=0x%016llx\n",
                Now,
                Breakpoint.Offset,
                (unsigned long long)Breakpoint.Address,
                ThreadId,
                SequenceId,
                (unsigned long long)m_base_address);

            Trace += StringFormat(
                "registers RAX=0x%016llx RBX=0x%016llx RCX=0x%016llx RDX=0x%016llx RSI=0x%016llx RDI=0x%016llx R8=0x%016llx R9=0x%016llx R10=0x%016llx R11=0x%016llx R12=0x%016llx R13=0x%016llx R14=0x%016llx R15=0x%016llx RSP=0x%016llx RBP=0x%016llx RIP=0x%016llx\n",
                (unsigned long long)Context.Rax,
                (unsigned long long)Context.Rbx,
                (unsigned long long)Context.Rcx,
                (unsigned long long)Context.Rdx,
                (unsigned long long)Context.Rsi,
                (unsigned long long)Context.Rdi,
                (unsigned long long)Context.R8,
                (unsigned long long)Context.R9,
                (unsigned long long)Context.R10,
                (unsigned long long)Context.R11,
                (unsigned long long)Context.R12,
                (unsigned long long)Context.R13,
                (unsigned long long)Context.R14,
                (unsigned long long)Context.R15,
                (unsigned long long)Context.Rsp,
                (unsigned long long)Context.Rbp,
                (unsigned long long)Context.Rip);

            struct RegisterPointer
            {
                const char* Name;
                size_t Value;
            };

            RegisterPointer Pointers[] =
            {
                { "RCX", (size_t)Context.Rcx },
                { "RDX", (size_t)Context.Rdx },
                { "R8",  (size_t)Context.R8 },
                { "R9",  (size_t)Context.R9 },
                { "RBX", (size_t)Context.Rbx },
                { "RDI", (size_t)Context.Rdi },
                { "RSI", (size_t)Context.Rsi },
                { "R14", (size_t)Context.R14 },
            };

            for (const RegisterPointer& Pointer : Pointers)
            {
                AppendPointerDump(Trace, Pointer.Name, Pointer.Value);
            }

            AppendClientSnapshot(Trace);
            Trace += "\n";

            AppendProbeLog(Trace);

            Log(
                "[DS2StateProbe] offset=0x%zx address=0x%016llx sequence_id=%zu thread_id=%u trace_file=DS2_StateProbeTrace.log",
                Breakpoint.Offset,
                (unsigned long long)Breakpoint.Address,
                SequenceId,
                ThreadId);
        }

        static LONG WINAPI VectoredExceptionHandler(EXCEPTION_POINTERS* ExceptionInfo)
        {
            if (g_manager == nullptr)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            return g_manager->HandleException(ExceptionInfo);
        }

        static VectoredBreakpointManager* g_manager;

        std::mutex m_mutex;
        size_t m_base_address = 0;
        PVOID m_handler = nullptr;
        std::vector<ProbeBreakpoint> m_breakpoints;
        std::unordered_map<DWORD, size_t> m_pending_single_steps;
    };

    VectoredBreakpointManager* VectoredBreakpointManager::g_manager = nullptr;
    VectoredBreakpointManager s_breakpoint_manager;
}

#endif

bool DS2_StateProbeHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    const RuntimeConfig& Config = injector.GetConfig();
    if (!Config.DS2TraceLeaveSession || !Config.DS2TraceStateProbe)
    {
        return true;
    }

    Log("[DS2StateProbe] trace_state_probe=1");
    return s_breakpoint_manager.Install((size_t)injector.GetBaseAddress());
#else
    Warning("[DS2StateProbe] state probe requires Windows x64; not installing.");
    return true;
#endif
}

void DS2_StateProbeHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    s_breakpoint_manager.Uninstall();
#endif
}

const char* DS2_StateProbeHook::GetName()
{
    return "DS2 State Probe";
}
