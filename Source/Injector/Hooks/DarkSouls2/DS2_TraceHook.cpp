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

#include <atomic>
#include <chrono>
#include <cstdint>
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
        size_t DerefLength = 0;
    };

    std::atomic<bool> s_running{ false };
    std::thread s_thread;
    PVOID s_handler = nullptr;
    uintptr_t s_base = 0;

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

    LONG CALLBACK OnException(PEXCEPTION_POINTERS Exception)
    {
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
                        uint8_t Bytes[64] = {};
                        if (ReadGuarded(At, Bytes, Point.DerefLength))
                        {
                            Followed = StringFormat(" [%s]=", Which.c_str());
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

    void Arm(size_t Offset, const std::string& DerefRegister = std::string(), size_t DerefLength = 0)
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
    //   bp <hex offset from the module base> [deref <registrador> <bytes>]
    //   clear
    //   report
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
                Parts >> Follow;
                if (Follow == "deref")
                {
                    Parts >> Register >> Length;
                }
                if (!Where.empty())
                {
                    Arm((size_t)strtoull(Where.c_str(), nullptr, 16), Register, Length);
                    Added++;
                }
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

    void Run()
    {
        while (s_running.load())
        {
            ServeRequests();
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
