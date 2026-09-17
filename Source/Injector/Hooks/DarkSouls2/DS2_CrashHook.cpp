/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_CrashHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
#ifdef _WIN32

    // 256, not 32: on 17/09 a burst of eight handled faults during one warp
    // (+0x1caa60/+0x1caaf0, a rotten list node) spent the cap, and the fatal
    // fault three legs later left no record at all.
    constexpr uint32_t kMaxReports = 256;
    constexpr int kStackWords = 256;
    constexpr int kMaxReturns = 24;

    uintptr_t s_base = 0;
    uintptr_t s_end = 0;
    PVOID s_handler = nullptr;
    std::atomic<uint32_t> s_reports{ 0 };
    wchar_t s_log_path[MAX_PATH] = {};

    bool InGame(uintptr_t Address)
    {
        return Address >= s_base && Address < s_end;
    }

    // Everything on the stack, with no allocation: the heap may be what broke.
    void Write(const char* Text, size_t Length)
    {
        HANDLE File = CreateFileW(s_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (File == INVALID_HANDLE_VALUE)
        {
            return;
        }
        DWORD Written = 0;
        WriteFile(File, Text, (DWORD)Length, &Written, nullptr);
        CloseHandle(File);
    }

    LONG CALLBACK Handler(EXCEPTION_POINTERS* Info)
    {
        const EXCEPTION_RECORD* Record = Info->ExceptionRecord;
        const CONTEXT* Context = Info->ContextRecord;
        const DWORD Code = Record->ExceptionCode;
        if (Code != EXCEPTION_ACCESS_VIOLATION && Code != EXCEPTION_ILLEGAL_INSTRUCTION &&
            Code != EXCEPTION_STACK_OVERFLOW && Code != EXCEPTION_PRIV_INSTRUCTION)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        if (!InGame((uintptr_t)Context->Rip) || s_reports.fetch_add(1) >= kMaxReports)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        SYSTEMTIME Now;
        GetLocalTime(&Now);
        char Text[4096];
        int Used = snprintf(Text, sizeof(Text),
            "%02u:%02u:%02u.%03u  excecao %08lx em +0x%llx (thread %lu), %s 0x%llx\n"
            "    rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n"
            "    rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n"
            "    r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx\n"
            "    r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n"
            "    retornos no jogo:",
            Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds, (unsigned long)Code,
            (unsigned long long)(Context->Rip - s_base), GetCurrentThreadId(),
            Record->NumberParameters >= 2 ? (Record->ExceptionInformation[0] == 1 ? "escrevendo" :
                (Record->ExceptionInformation[0] == 8 ? "executando" : "lendo")) : "em",
            Record->NumberParameters >= 2 ? (unsigned long long)Record->ExceptionInformation[1] : 0ull,
            (unsigned long long)Context->Rax, (unsigned long long)Context->Rbx, (unsigned long long)Context->Rcx,
            (unsigned long long)Context->Rdx, (unsigned long long)Context->Rsi, (unsigned long long)Context->Rdi,
            (unsigned long long)Context->Rbp, (unsigned long long)Context->Rsp, (unsigned long long)Context->R8,
            (unsigned long long)Context->R9, (unsigned long long)Context->R10, (unsigned long long)Context->R11,
            (unsigned long long)Context->R12, (unsigned long long)Context->R13, (unsigned long long)Context->R14,
            (unsigned long long)Context->R15);
        if (Used < 0)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // A stack overflow has no room left to read beyond; the rest does.
        int Found = 0;
        if (Code != EXCEPTION_STACK_OVERFLOW)
        {
            const uintptr_t* Stack = (const uintptr_t*)Context->Rsp;
            for (int i = 0; i < kStackWords && Found < kMaxReturns && Used < (int)sizeof(Text) - 32; ++i)
            {
                MEMORY_BASIC_INFORMATION Region;
                if (i % 32 == 0 && (VirtualQuery(Stack + i, &Region, sizeof(Region)) == 0 || Region.State != MEM_COMMIT))
                {
                    break;
                }
                if (InGame(Stack[i]))
                {
                    Used += snprintf(Text + Used, sizeof(Text) - Used, " +0x%llx", (unsigned long long)(Stack[i] - s_base));
                    ++Found;
                }
            }
        }
        Used += snprintf(Text + Used, sizeof(Text) - Used, "\n");
        Write(Text, (size_t)Used < sizeof(Text) ? (size_t)Used : sizeof(Text) - 1);
        return EXCEPTION_CONTINUE_SEARCH;
    }

#endif
}

bool DS2_CrashHook::Install(Injector& injector)
{
#ifdef _WIN32
    s_base = (uintptr_t)injector.GetBaseAddress();
    const IMAGE_DOS_HEADER* Dos = (const IMAGE_DOS_HEADER*)s_base;
    const IMAGE_NT_HEADERS64* Nt = (const IMAGE_NT_HEADERS64*)(s_base + Dos->e_lfanew);
    s_end = s_base + Nt->OptionalHeader.SizeOfImage;

    const std::wstring Path = (injector.GetDllPath() / "DS2_Crash.log").wstring();
    wcsncpy_s(s_log_path, Path.c_str(), _TRUNCATE);

    s_handler = AddVectoredExceptionHandler(1, Handler);
    if (s_handler == nullptr)
    {
        Error("[DS2_CrashHook] nao consegui instalar o handler");
        return false;
    }
    const char Line[] = "=== ds2os: vigia de excecoes no jogo ===\n";
    Write(Line, sizeof(Line) - 1);
#endif
    return true;
}

void DS2_CrashHook::Uninstall()
{
#ifdef _WIN32
    if (s_handler != nullptr)
    {
        RemoveVectoredExceptionHandler(s_handler);
        s_handler = nullptr;
    }
#endif
}

const char* DS2_CrashHook::GetName()
{
    return "DS2 Crash";
}
