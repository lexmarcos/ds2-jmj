/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_TraceHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"
#include "Shared/Platform/Platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <map>
#include "ThirdParty/detours/src/detours.h"
#endif

namespace
{
#ifdef _WIN32

    struct Breakpoint
    {
        uintptr_t Address = 0;
        size_t Offset = 0;
        uint8_t Original = 0;
        bool Armed = false;

        // Which register to follow, and how many bytes to read there. A
        // register holding a pointer is the common case in this binary: the
        // interesting value is almost never *in* rcx or rdx, it is one
        // dereference away, in a local the caller built and will reuse. By the
        // time a request file could read that address the memory is gone.
        std::string DerefRegister;
        size_t DerefOffset = 0;
        size_t DerefLength = 0;
    };

    std::atomic<bool> s_running{ false };
    std::thread s_thread;
    PVOID s_handler = nullptr;
    uintptr_t s_base = 0;

    // `esd <ms> [rotulo]`: every EzState environment query the game evaluates
    // in a window, by id, with the values it answered. Event scripts decide
    // things like "Cannot use bonfire" through these, and a query id is what
    // a patch would have to change. FUN_140456a90 is the dispatcher (a
    // virtual, slot of the vftable at 0x1410ef418 region): (this, out value,
    // arguments, ?); the id is the arguments' slot +8, the answer a value and
    // a type tag at out[0] and out[2].
    constexpr size_t kEsdQueryOffset = 0x456a90;
    constexpr uint8_t kEsdQueryPrologue[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xc0 };
    // FUN_14045c6a0 is the same shape, and the map event scripts
    // (FUN_140471ae0, FUN_140473ac0) call it directly, not through the
    // dispatcher; ids it answers are marked "i".
    constexpr size_t kEsdInnerOffset = 0x45c6a0;
    constexpr uint8_t kEsdInnerPrologue[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24 };
    using EsdQuery_p = uint64_t(*)(void* This, uint32_t* Out, void** Arguments, void* P4);
    EsdQuery_p s_original_esd = nullptr;
    EsdQuery_p s_original_esd_inner = nullptr;
    std::atomic<bool> s_esd_on{ false };
    std::chrono::steady_clock::time_point s_esd_deadline;
    std::string s_esd_label;
    struct EsdSeen
    {
        uint64_t Count = 0;
        std::vector<std::pair<uint32_t, uint32_t>> Values;   // (value, tag), first few distinct
    };
    std::mutex s_esd_mutex;
    std::map<int32_t, EsdSeen> s_esd_seen;

    uint64_t EsdRecord(EsdQuery_p Original, int32_t Mark, void* This, uint32_t* Out, void** Arguments, void* P4)
    {
        if (!s_esd_on.load(std::memory_order_relaxed))
        {
            return Original(This, Out, Arguments, P4);
        }
        using Id_p = int32_t(*)(void*);
        const int32_t Id = ((Id_p)((*(void***)Arguments)[1]))(Arguments) ^ Mark;
        const uint64_t Result = Original(This, Out, Arguments, P4);
        const uint32_t Value = Out[0];
        const uint32_t Tag = Out[2];
        std::scoped_lock Lock(s_esd_mutex);
        EsdSeen& Seen = s_esd_seen[Id];
        ++Seen.Count;
        const std::pair<uint32_t, uint32_t> Pair{ Value, Tag };
        if (Seen.Values.size() < 4 && std::find(Seen.Values.begin(), Seen.Values.end(), Pair) == Seen.Values.end())
        {
            Seen.Values.push_back(Pair);
        }
        return Result;
    }

    // The outer dispatcher's ids as they are; the inner one's with the top
    // bit set, so the two stay apart in the report.
    uint64_t EsdQueryHook(void* This, uint32_t* Out, void** Arguments, void* P4)
    {
        return EsdRecord(s_original_esd, 0, This, Out, Arguments, P4);
    }

    uint64_t EsdInnerHook(void* This, uint32_t* Out, void** Arguments, void* P4)
    {
        return EsdRecord(s_original_esd_inner, (int32_t)0x80000000, This, Out, Arguments, P4);
    }

    // The image is about 28 MB. This only has to be an upper bound, for
    // deciding whether a stack slot looks like a code address in this module.
    constexpr uintptr_t kModuleSpan = 0x2000000;

    std::mutex s_mutex;
    std::unordered_map<uintptr_t, Breakpoint> s_breakpoints;

    std::filesystem::path s_log_path;
    std::filesystem::path s_request_path;

    void Append(const std::string& Text)
    {
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream.is_open())
        {
            Stream << Text;
        }
    }

    bool WriteByte(uintptr_t Address, uint8_t Value)
    {
        DWORD Previous = 0;
        if (!VirtualProtect((LPVOID)Address, 1, PAGE_EXECUTE_READWRITE, &Previous))
        {
            return false;
        }
        *(volatile uint8_t*)Address = Value;
        DWORD Ignored = 0;
        VirtualProtect((LPVOID)Address, 1, Previous, &Ignored);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)Address, 1);
        return true;
    }

    // A read that is allowed to fail. MSVC refuses __try in a function that
    // has to unwind C++ objects, and the handler is full of std::string, so
    // the guard lives here on its own.
    bool ReadGuarded(uintptr_t At, uint8_t* Out, size_t Length)
    {
        __try
        {
            memcpy(Out, (const void*)At, Length);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // A write watchpoint, by page protection rather than debug registers.
    //
    // Under Wine the debug registers of another thread are set by the
    // wineserver through ptrace, and with yama's ptrace_scope at 1 - the
    // default here - that attach is refused and the register write is lost
    // without an error. Page protection is something Wine always delivers: the
    // page holding the target is made read-only, every write to it faults with
    // the address of the instruction doing it, and the page is released for
    // exactly one instruction with the trap flag before being protected again.
    //
    // Every write to that 4 KB page faults, not just writes to the target, and
    // a heap page next to a player object is written many times a frame. So a
    // watch always carries a deadline, and the request thread lifts it.
    //
    // `wpr` watches reads too: the page is made inaccessible instead of
    // read-only, and a read of the target is reported like a write. That is
    // how a flag's readers are found when every one of them goes through a
    // getter the listing does not show.
    struct WatchHit
    {
        uintptr_t Rip = 0;
        uint64_t Count = 0;
        // 0 read, 1 write, as the fault reports it.
        ULONG_PTR Kind = 1;
        // Fixed storage on purpose. The handler must not allocate: the page it
        // protects sits in a heap, and an allocation that touched it would fault
        // again on the same thread while this handler holds the lock.
        char First[768] = {};
    };

    std::mutex s_watch_mutex;
    uintptr_t s_watch_address = 0;
    size_t s_watch_length = 0;
    uintptr_t s_watch_page = 0;
    // Readable without the lock: a read fault on any other page must be passed
    // on before taking it (see OnWatchException).
    std::atomic<uintptr_t> s_watch_page_seen{ 0 };
    std::atomic<bool> s_watch_reads{ false };
    // PAGE_READONLY for a write watch, PAGE_NOACCESS for a read watch.
    DWORD s_watch_trap = PAGE_READONLY;
    // The page of the last watch, kept after it is lifted. A thread that
    // faulted while the page was still read-only can reach the handler only
    // after the request thread has disarmed; that fault is ours to retry, and
    // passing it on crashed the game (0xC0000005, 13/09, 24k faults a second).
    std::atomic<uintptr_t> s_lifted_page{ 0 };
    DWORD s_watch_protection = 0;
    std::atomic<bool> s_watch_armed{ false };
    std::chrono::steady_clock::time_point s_watch_deadline;
    uint64_t s_watch_faults = 0;
    constexpr size_t kMaxWatchHits = 16;
    WatchHit s_watch_hits[kMaxWatchHits];
    size_t s_watch_hit_count = 0;
    constexpr uintptr_t kPageMask = ~(uintptr_t)0xFFF;
    constexpr DWORD kTrapFlagBit = 0x100;

    thread_local bool t_watch_step = false;

    void DescribeFrames(const CONTEXT* Registers, char* Out, size_t Size)
    {
        size_t Used = 0;
        Out[0] = 0;
        const uintptr_t* Stack = (const uintptr_t*)Registers->Rsp;
        int Shown = 0;
        for (int i = 0; i < 64 && Shown < 8 && Used + 24 < Size; i++)
        {
            uintptr_t Value = 0;
            if (!ReadGuarded((uintptr_t)&Stack[i], (uint8_t*)&Value, sizeof(Value)))
            {
                break;
            }
            if (Value > s_base && Value < s_base + kModuleSpan)
            {
                Used += (size_t)snprintf(Out + Used, Size - Used, " +0x%llx", (unsigned long long)(Value - s_base));
                Shown++;
            }
        }
        if (Shown == 0)
        {
            snprintf(Out, Size, " (nenhum)");
        }
    }

    // Returns true when the exception belonged to the watch.
    bool OnWatchException(PEXCEPTION_POINTERS Exception, LONG* Result)
    {
        const DWORD Code = Exception->ExceptionRecord->ExceptionCode;
        CONTEXT* Context = Exception->ContextRecord;

        if (Code == EXCEPTION_SINGLE_STEP)
        {
            if (!t_watch_step)
            {
                return false;
            }
            t_watch_step = false;
            Context->EFlags &= ~kTrapFlagBit;
            std::scoped_lock Lock(s_watch_mutex);
            if (s_watch_armed.load())
            {
                DWORD Ignored = 0;
                VirtualProtect((LPVOID)s_watch_page, 0x1000, s_watch_trap, &Ignored);
            }
            *Result = EXCEPTION_CONTINUE_EXECUTION;
            return true;
        }

        if (Code != EXCEPTION_ACCESS_VIOLATION || Exception->ExceptionRecord->NumberParameters < 2)
        {
            return false;
        }
        const ULONG_PTR Kind = Exception->ExceptionRecord->ExceptionInformation[0];
        const uintptr_t Target = (uintptr_t)Exception->ExceptionRecord->ExceptionInformation[1];

        // Decided before the lock. The frame walk below reads the stack under
        // __try, and a read that faults comes back through this handler first;
        // taking the lock for it would deadlock the thread on itself. Writes
        // can belong to any watch; a read only to a read watch, and only on its
        // own page (or the page just lifted), which the frame walk never reads.
        const uintptr_t FaultPage = Target & kPageMask;
        if (Kind == 0)
        {
            if (!s_watch_reads.load() ||
                (FaultPage != s_watch_page_seen.load() && FaultPage != s_lifted_page.load()))
            {
                return false;
            }
        }
        else if (Kind != 1)
        {
            return false;
        }

        // A write that faulted before the watch was lifted: the protection is
        // back, so running the instruction again is all it needs. Only when the
        // page really is writable now, or this would spin forever.
        auto RetryLifted = [&]() -> bool
        {
            if ((Target & kPageMask) != s_lifted_page.load())
            {
                return false;
            }
            MEMORY_BASIC_INFORMATION Info = {};
            if (VirtualQuery((LPCVOID)Target, &Info, sizeof(Info)) == 0)
            {
                return false;
            }
            const DWORD Protection = Info.Protect & 0xFF;
            const bool Writable = Protection == PAGE_READWRITE || Protection == PAGE_WRITECOPY ||
                Protection == PAGE_EXECUTE_READWRITE || Protection == PAGE_EXECUTE_WRITECOPY;
            const bool Readable = Writable || Protection == PAGE_READONLY || Protection == PAGE_EXECUTE_READ;
            if (Kind == 1 ? !Writable : !Readable)
            {
                return false;
            }
            *Result = EXCEPTION_CONTINUE_EXECUTION;
            return true;
        };

        if (!s_watch_armed.load() && (Target & kPageMask) != s_lifted_page.load())
        {
            return false;
        }

        // Under the lock a disarm is either not started or finished, protection
        // included, so the query inside RetryLifted sees the restored page.
        std::scoped_lock Lock(s_watch_mutex);
        if (!s_watch_armed.load() || (Target & kPageMask) != s_watch_page)
        {
            // Not armed, or armed again on another page after this fault.
            return RetryLifted();
        }

        s_watch_faults++;
        if (Target >= s_watch_address && Target < s_watch_address + s_watch_length)
        {
            const uintptr_t Rip = (uintptr_t)Context->Rip;
            WatchHit* Hit = nullptr;
            for (size_t i = 0; i < s_watch_hit_count; i++)
            {
                if (s_watch_hits[i].Rip == Rip)
                {
                    Hit = &s_watch_hits[i];
                    break;
                }
            }
            if (Hit == nullptr && s_watch_hit_count < kMaxWatchHits)
            {
                Hit = &s_watch_hits[s_watch_hit_count++];
                Hit->Rip = Rip;
                Hit->Count = 0;
                Hit->Kind = Kind;
                char Frames[256];
                DescribeFrames(Context, Frames, sizeof(Frames));
                snprintf(Hit->First, sizeof(Hit->First),
                    "rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx rsi=%016llx rdi=%016llx "
                    "r8=%016llx r9=%016llx rsp=%016llx alvo=%016llx pilha:%s",
                    (unsigned long long)Context->Rax, (unsigned long long)Context->Rbx,
                    (unsigned long long)Context->Rcx, (unsigned long long)Context->Rdx,
                    (unsigned long long)Context->Rsi, (unsigned long long)Context->Rdi,
                    (unsigned long long)Context->R8, (unsigned long long)Context->R9,
                    (unsigned long long)Context->Rsp, (unsigned long long)Target, Frames);
            }
            if (Hit != nullptr)
            {
                Hit->Count++;
            }
        }

        // Let this one instruction through, then protect the page again.
        DWORD Ignored = 0;
        VirtualProtect((LPVOID)s_watch_page, 0x1000, s_watch_protection, &Ignored);
        Context->EFlags |= kTrapFlagBit;
        t_watch_step = true;
        *Result = EXCEPTION_CONTINUE_EXECUTION;
        return true;
    }

    // Called by the request thread: lifts the protection and writes what was
    // seen. File I/O stays out of the handler.
    void DisarmWatch(const char* Why)
    {
        WatchHit Copied[kMaxWatchHits];
        size_t Count = 0;
        uint64_t Faults = 0;
        uintptr_t Address = 0;
        bool Reads = false;
        {
            std::scoped_lock Lock(s_watch_mutex);
            if (!s_watch_armed.load())
            {
                return;
            }
            // Protection first, so nothing below can fault on the page.
            s_lifted_page.store(s_watch_page);
            s_watch_armed.store(false);
            Reads = s_watch_reads.load();
            DWORD Ignored = 0;
            VirtualProtect((LPVOID)s_watch_page, 0x1000, s_watch_protection, &Ignored);
            Count = s_watch_hit_count;
            for (size_t i = 0; i < Count; i++)
            {
                Copied[i] = s_watch_hits[i];
            }
            s_watch_hit_count = 0;
            Faults = s_watch_faults;
            Address = s_watch_address;
        }
        std::vector<WatchHit> Hits(Copied, Copied + Count);

        std::string Text = StringFormat("\n=== vigia de %s em %016llx encerrada (%s): %llu faltas na pagina, %zu instrucoes ===\n",
            Reads ? "leitura" : "escrita", (unsigned long long)Address, Why, (unsigned long long)Faults, Hits.size());
        for (const WatchHit& Hit : Hits)
        {
            const bool InModule = Hit.Rip > s_base && Hit.Rip < s_base + kModuleSpan;
            const char* Verb = Hit.Kind == 0 ? "leu" : "escreveu";
            Text += InModule
                ? StringFormat("  %s em +0x%llx (%llux) %s\n", Verb, (unsigned long long)(Hit.Rip - s_base),
                    (unsigned long long)Hit.Count, Hit.First)
                : StringFormat("  %s em %016llx (%llux) %s\n", Verb, (unsigned long long)Hit.Rip,
                    (unsigned long long)Hit.Count, Hit.First);
        }
        Append(Text);
    }

    void ArmWatch(uintptr_t Address, size_t Length, int Seconds, bool Reads)
    {
        DisarmWatch("substituida");

        std::string Outcome;
        {
            std::scoped_lock Lock(s_watch_mutex);
            MEMORY_BASIC_INFORMATION Info = {};
            const DWORD Protection = VirtualQuery((LPCVOID)Address, &Info, sizeof(Info)) == 0
                ? 0 : (Info.State == MEM_COMMIT ? (Info.Protect & 0xFF) : 0);

            // Executable, guard or unmapped pages are someone else's business.
            if (Protection != PAGE_READWRITE && Protection != PAGE_WRITECOPY)
            {
                Outcome = StringFormat("\n=== vigia recusada: %016llx nao e uma pagina de dados gravavel (protecao %08lx) ===\n",
                    (unsigned long long)Address, (unsigned long)Info.Protect);
            }
            else
            {
                s_watch_address = Address;
                s_watch_length = Length == 0 ? 1 : Length;
                s_watch_page = Address & kPageMask;
                s_watch_page_seen.store(s_watch_page);
                s_watch_reads.store(Reads);
                s_watch_trap = Reads ? PAGE_NOACCESS : PAGE_READONLY;
                s_watch_protection = Info.Protect;
                s_watch_faults = 0;
                s_watch_hit_count = 0;
                s_watch_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(Seconds);

                // Format before protecting: nothing after VirtualProtect may allocate.
                Outcome = StringFormat("\n=== vigiando %s em %016llx (%zu bytes) por %d s ===\n",
                    Reads ? "leituras e escritas" : "escritas", (unsigned long long)Address, s_watch_length, Seconds);
                DWORD Previous = 0;
                if (VirtualProtect((LPVOID)s_watch_page, 0x1000, s_watch_trap, &Previous))
                {
                    s_watch_armed.store(true);
                }
                else
                {
                    Outcome = "\n=== vigia recusada: VirtualProtect falhou ===\n";
                }
            }
        }
        Append(Outcome);
    }

    LONG CALLBACK OnException(PEXCEPTION_POINTERS Exception)
    {
        LONG WatchResult = EXCEPTION_CONTINUE_SEARCH;
        if (OnWatchException(Exception, &WatchResult))
        {
            return WatchResult;
        }

        if (Exception->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Wine has been seen reporting the address both at the instruction and
        // one past it, so accept either, the way the timer hook already does.
        const uintptr_t Reported = (uintptr_t)Exception->ExceptionRecord->ExceptionAddress;
        const uintptr_t Candidate = (uintptr_t)Exception->ContextRecord->Rip - 1;

        std::string Line;
        {
            std::scoped_lock Lock(s_mutex);

            auto Found = s_breakpoints.find(Reported);
            if (Found == s_breakpoints.end())
            {
                Found = s_breakpoints.find(Candidate);
            }
            if (Found == s_breakpoints.end())
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            Breakpoint& Point = Found->second;

            // A second thread can reach the same address between the first
            // thread restoring the byte and this handler running - it already
            // fetched the 0xCC. Owning the address is what matters, not whether
            // it is still armed: leaving it to the next handler is what killed
            // the game when three thousand of these were armed at once.
            if (Point.Armed)
            {
                WriteByte(Point.Address, Point.Original);
                Point.Armed = false;
                // The arguments matter as much as the fact of the call: the
                // function that decides this looks something up by an id in
                // rdx, and knowing which id is what the comparison needs.
                // The return address names the caller outright. Climbing by
                // grep does not work here: everything reaches these functions
                // through adjustor thunks and vtables, so the static listing
                // shows a thunk and stops.
                uintptr_t Caller = 0;
                if (Point.Offset != 0)
                {
                    const uintptr_t* Stack = (const uintptr_t*)Exception->ContextRecord->Rsp;
                    if (Stack != nullptr)
                    {
                        Caller = *Stack;
                    }
                }

                // [rsp] is only a return address at a function's entry. Break
                // in the middle of one, which is where an interesting branch
                // usually is, and it is whatever local happened to be pushed.
                // So scan a window of the stack and report every slot that
                // looks like it points into this module's code: the real call
                // chain is in there, and reading several frames beats guessing
                // at one. Offsets, so they can be pasted into a disassembly.
                std::string Frames;
                {
                    const uintptr_t* Stack = (const uintptr_t*)Exception->ContextRecord->Rsp;
                    int Shown = 0;
                    for (int i = 0; i < 64 && Shown < 8; i++)
                    {
                        uintptr_t Value = Stack[i];
                        if (Value > s_base && Value < s_base + kModuleSpan)
                        {
                            Frames += StringFormat(" +0x%llx", (unsigned long long)(Value - s_base));
                            Shown++;
                        }
                    }
                    if (Frames.empty())
                    {
                        Frames = " (nenhum)";
                    }
                }

                // The dereference, when the request asked for one. A read
                // here can fault — the register may hold anything — and a
                // fault inside a vectored handler takes the game with it, so
                // it is guarded and a bad address is reported as such rather
                // than being allowed to happen.
                std::string Followed;
                if (Point.DerefLength > 0)
                {
                    uintptr_t At = 0;
                    const std::string& Which = Point.DerefRegister;
                    const CONTEXT* Registers = Exception->ContextRecord;
                    if (Which == "rcx") { At = (uintptr_t)Registers->Rcx; }
                    else if (Which == "rdx") { At = (uintptr_t)Registers->Rdx; }
                    else if (Which == "r8") { At = (uintptr_t)Registers->R8; }
                    else if (Which == "r9") { At = (uintptr_t)Registers->R9; }
                    else if (Which == "rax") { At = (uintptr_t)Registers->Rax; }
                    else if (Which == "rbx") { At = (uintptr_t)Registers->Rbx; }
                    else if (Which == "rsi") { At = (uintptr_t)Registers->Rsi; }
                    else if (Which == "rdi") { At = (uintptr_t)Registers->Rdi; }

                    if (At == 0)
                    {
                        Followed = StringFormat(" [%s: registrador desconhecido ou nulo]", Which.c_str());
                    }
                    else
                    {
                        At += Point.DerefOffset;
                        uint8_t Bytes[64] = {};
                        if (ReadGuarded(At, Bytes, Point.DerefLength))
                        {
                            Followed = Point.DerefOffset == 0
                                ? StringFormat(" [%s]=", Which.c_str())
                                : StringFormat(" [%s+%zx]=", Which.c_str(), Point.DerefOffset);
                            for (size_t i = 0; i < Point.DerefLength; i++)
                            {
                                Followed += StringFormat("%02x", Bytes[i]);
                            }
                        }
                        else
                        {
                            Followed = StringFormat(" [%s=%016llx ilegivel]",
                                Which.c_str(), (unsigned long long)At);
                        }
                    }
                }

                Line = StringFormat(
                    "  alcancado +0x%zx de=+0x%llx rcx=%016llx rdx=%016llx r8=%016llx%s pilha:%s\n",
                    Point.Offset,
                    (unsigned long long)(Caller >= s_base ? Caller - s_base : Caller),
                    (unsigned long long)Exception->ContextRecord->Rcx,
                    (unsigned long long)Exception->ContextRecord->Rdx,
                    (unsigned long long)Exception->ContextRecord->R8,
                    Followed.c_str(),
                    Frames.c_str());
            }

            Exception->ContextRecord->Rip = (DWORD64)Point.Address;
        }

        if (!Line.empty())
        {
            Append(Line);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void DisarmAll()
    {
        std::scoped_lock Lock(s_mutex);
        for (auto& Pair : s_breakpoints)
        {
            if (Pair.second.Armed)
            {
                WriteByte(Pair.second.Address, Pair.second.Original);
                Pair.second.Armed = false;
            }
        }
        s_breakpoints.clear();
    }

    void Arm(size_t Offset, const std::string& DerefRegister = std::string(), size_t DerefOffset = 0, size_t DerefLength = 0)
    {
        const uintptr_t Address = s_base + Offset;

        std::scoped_lock Lock(s_mutex);
        if (s_breakpoints.count(Address) != 0)
        {
            return;
        }

        Breakpoint Point;
        Point.Address = Address;
        Point.Offset = Offset;
        Point.DerefRegister = DerefRegister;
        Point.DerefOffset = DerefOffset;
        Point.DerefLength = DerefLength > 64 ? 64 : DerefLength;
        Point.Original = *(volatile uint8_t*)Address;

        if (Point.Original == 0xCC)
        {
            return;
        }
        if (!WriteByte(Address, 0xCC))
        {
            return;
        }

        Point.Armed = true;
        s_breakpoints[Address] = Point;
    }

    // Requests, one per line:
    //   bp <hex offset from the module base> [deref <registrador>[+<hex>] <bytes>]
    //   clear
    //   report
    //   wp <hex absolute address> <decimal length> [seconds, default 3, max 20]
    //   wpr <same>   (reads too)
    //   wpclear
    //   esd <ms> [rotulo]   EzState queries evaluated in the window, by id
    //
    // The deref is what makes an argument readable. Half the interesting
    // values in this binary are behind a pointer in rcx or rdx — a handle, a
    // small struct the caller built on its stack — and by the time a request
    // file could be answered that memory has been reused. Registers: rcx rdx
    // r8 r9 rax rbx rsi rdi; at most 64 bytes.
    void ServeRequests()
    {
        std::error_code Error;
        if (!std::filesystem::exists(s_request_path, Error))
        {
            return;
        }

        std::vector<std::string> Lines;
        {
            std::ifstream Stream(s_request_path);
            std::string Line;
            while (std::getline(Stream, Line))
            {
                Lines.push_back(Line);
            }
        }
        std::filesystem::remove(s_request_path, Error);

        size_t Added = 0;
        for (const std::string& Line : Lines)
        {
            std::istringstream Parts(Line);
            std::string Kind;
            Parts >> Kind;

            if (Kind == "clear")
            {
                DisarmAll();
                Append("\n=== limpo ===\n");
            }
            else if (Kind == "bp")
            {
                // bp <hex offset> [deref <register> <decimal length>]
                std::string Where;
                Parts >> Where;
                std::string Follow;
                std::string Register;
                size_t Length = 0;
                size_t FieldOffset = 0;
                Parts >> Follow;
                if (Follow == "deref")
                {
                    Parts >> Register >> Length;
                    // "rcx+e0" reads a field instead of the head of the
                    // object, which is what most questions here actually are.
                    const size_t Plus = Register.find('+');
                    if (Plus != std::string::npos)
                    {
                        FieldOffset = (size_t)strtoull(Register.c_str() + Plus + 1, nullptr, 16);
                        Register = Register.substr(0, Plus);
                    }
                }
                if (!Where.empty())
                {
                    Arm((size_t)strtoull(Where.c_str(), nullptr, 16), Register, FieldOffset, Length);
                    Added++;
                }
            }
            else if (Kind == "wp" || Kind == "wpr")
            {
                // wp <hex absolute address> <decimal length> [seconds]
                std::string Where;
                size_t Length = 4;
                int Seconds = 3;
                Parts >> Where >> Length >> Seconds;
                if (Seconds < 1) { Seconds = 1; }
                if (Seconds > 20) { Seconds = 20; }
                if (!Where.empty())
                {
                    ArmWatch((uintptr_t)strtoull(Where.c_str(), nullptr, 16), Length, Seconds, Kind == "wpr");
                }
            }
            else if (Kind == "esd")
            {
                // esd <decimal ms, max 20000> [rotulo]
                int Ms = 1500;
                std::string Label;
                Parts >> Ms >> Label;
                if (Ms < 100) { Ms = 100; }
                if (Ms > 20000) { Ms = 20000; }
                if (s_original_esd == nullptr)
                {
                    Append("=== esd: o despachante nao foi instalado ===\n");
                }
                else
                {
                    {
                        std::scoped_lock Lock(s_esd_mutex);
                        s_esd_seen.clear();
                        s_esd_label = Label;
                    }
                    s_esd_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(Ms);
                    s_esd_on.store(true);
                    Append(StringFormat("=== esd: consultas por %d ms (%s) ===\n", Ms, Label.c_str()));
                }
            }
            else if (Kind == "wpclear")
            {
                DisarmWatch("pedido");
            }
            else if (Kind == "report")
            {
                std::scoped_lock Lock(s_mutex);
                size_t Still = 0;
                for (const auto& Pair : s_breakpoints)
                {
                    if (Pair.second.Armed)
                    {
                        Still++;
                    }
                }
                Append(StringFormat("=== %zu armados, %zu ja alcancados ===\n",
                    Still, s_breakpoints.size() - Still));
            }
        }

        if (Added > 0)
        {
            Append(StringFormat("\n=== armados %zu enderecos ===\n", Added));
        }
    }

    void FlushEsd()
    {
        s_esd_on.store(false);
        std::map<int32_t, EsdSeen> Seen;
        std::string Label;
        {
            std::scoped_lock Lock(s_esd_mutex);
            Seen.swap(s_esd_seen);
            Label = s_esd_label;
        }
        std::string Text = StringFormat("=== esd (%s): %zu consultas distintas ===\n", Label.c_str(), Seen.size());
        for (const auto& [Id, Entry] : Seen)
        {
            std::string Values;
            for (const auto& [Value, Tag] : Entry.Values)
            {
                Values += StringFormat(" %08x/%u", Value, Tag);
            }
            const bool Inner = (Id & (int32_t)0x80000000) != 0;
            const int32_t Plain = Id & 0x7fffffff;
            Text += StringFormat("  esd %s %s %08x (%d) x%llu:%s\n", Label.c_str(), Inner ? "i" : "e", (uint32_t)Plain, Plain,
                (unsigned long long)Entry.Count, Values.c_str());
        }
        Append(Text);
    }

    void Run()
    {
        while (s_running.load())
        {
            ServeRequests();
            if (s_esd_on.load() && std::chrono::steady_clock::now() >= s_esd_deadline)
            {
                FlushEsd();
            }
            if (s_watch_armed.load() && std::chrono::steady_clock::now() >= s_watch_deadline)
            {
                DisarmWatch("prazo");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

#endif
}

bool DS2_TraceHook::Install(Injector& injector)
{
#ifdef _WIN32
    s_base = (uintptr_t)injector.GetBaseAddress();
    s_log_path = injector.GetDllPath() / "DS2_Trace.log";
    s_request_path = injector.GetDllPath() / "DS2_Trace.req";

    s_handler = AddVectoredExceptionHandler(1, OnException);
    if (s_handler == nullptr)
    {
        Error("[DS2Trace] nao consegui instalar o handler");
        return true;
    }

    {
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream.is_open())
        {
            Stream << StringFormat("\n============ tracador pronto base=0x%016llx ============\n",
                (unsigned long long)s_base);
        }
    }

    if (memcmp((const void*)(s_base + kEsdQueryOffset), kEsdQueryPrologue, sizeof(kEsdQueryPrologue)) == 0)
    {
        s_original_esd = (EsdQuery_p)(s_base + kEsdQueryOffset);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)s_original_esd, EsdQueryHook);
        const bool Inner = memcmp((const void*)(s_base + kEsdInnerOffset), kEsdInnerPrologue, sizeof(kEsdInnerPrologue)) == 0;
        if (Inner)
        {
            s_original_esd_inner = (EsdQuery_p)(s_base + kEsdInnerOffset);
            DetourAttach(&(PVOID&)s_original_esd_inner, EsdInnerHook);
        }
        if (DetourTransactionCommit() != NO_ERROR)
        {
            s_original_esd = nullptr;
            s_original_esd_inner = nullptr;
            Error("[DS2Trace] nao consegui instalar o espiao de EzState");
        }
    }

    s_running.store(true);
    s_thread = std::thread(Run);

    Log("[DS2Trace] pronto, pedidos em %s", s_request_path.string().c_str());
#endif
    return true;
}

void DS2_TraceHook::Uninstall()
{
#ifdef _WIN32
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }
    DisarmAll();
    DisarmWatch("desinstalando");
    if (s_original_esd != nullptr)
    {
        s_esd_on.store(false);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_esd, EsdQueryHook);
        if (s_original_esd_inner != nullptr)
        {
            DetourDetach(&(PVOID&)s_original_esd_inner, EsdInnerHook);
        }
        DetourTransactionCommit();
        s_original_esd = nullptr;
        s_original_esd_inner = nullptr;
    }
    if (s_handler != nullptr)
    {
        RemoveVectoredExceptionHandler(s_handler);
        s_handler = nullptr;
    }
#endif
}

const char* DS2_TraceHook::GetName()
{
    return "DS2 Trace";
}
