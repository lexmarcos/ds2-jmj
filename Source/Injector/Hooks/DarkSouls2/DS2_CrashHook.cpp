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

    // A MapEntity in hand is described whole: the map its owner belongs to,
    // then every node of its component list at +0x18 (next at +0x10) and every
    // slot of its component array at +0x98 (count, a short, at +0xa0), each
    // with the first eight bytes of what it points at. On 18/09 every guest
    // death had a live MapEntity in hand and one of its components already
    // freed; this is what names which component, and whose map the entity is.
    constexpr uintptr_t kMapEntityVftable = 0x10e7b68;
    constexpr uintptr_t kMapOwnerVftable = 0x10e87f0;

    bool SafeQword(uintptr_t At, uintptr_t& Out)
    {
        __try
        {
            Out = *(const uintptr_t*)At;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SafeShort(uintptr_t At, int16_t& Out)
    {
        __try
        {
            Out = *(const int16_t*)At;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int DescribeFirst(char* Text, int Used, int Size, uintptr_t Object)
    {
        uintptr_t First = 0;
        if (!SafeQword(Object, First))
        {
            return snprintf(Text + Used, Size - Used, " %p(unreadable)", (void*)Object);
        }
        if (InGame(First))
        {
            return snprintf(Text + Used, Size - Used, " %p(+0x%llx)", (void*)Object,
                (unsigned long long)(First - s_base));
        }
        return snprintf(Text + Used, Size - Used, " %p(ROTTEN %016llx)", (void*)Object, (unsigned long long)First);
    }

    int DescribeEntity(char* Text, int Used, int Size, const char* Name, uintptr_t Entity)
    {
        int Start = Used;
        uintptr_t Owner = 0, OwnerVftable = 0, Map = 0;
        uint32_t MapId = 0xffffffff;
        if (SafeQword(Entity + 0x28, Owner) && Owner != 0 && SafeQword(Owner, OwnerVftable) &&
            OwnerVftable == s_base + kMapOwnerVftable && SafeQword(Owner + 0x08, Map))
        {
            MapId = (uint32_t)Map;
        }
        Used += snprintf(Text + Used, Size - Used, "    MapEntity %s=%p, owner %p, map %08x\n      list +0x18:", Name,
            (void*)Entity, (void*)Owner, MapId);
        uintptr_t Node = 0;
        SafeQword(Entity + 0x18, Node);
        for (int k = 0; k < 16 && Node != 0 && Used < Size - 80; ++k)
        {
            Used += DescribeFirst(Text, Used, Size, Node);
            if (!SafeQword(Node + 0x10, Node))
            {
                break;
            }
        }
        int16_t Count = 0;
        uintptr_t Array = 0;
        SafeShort(Entity + 0xa0, Count);
        SafeQword(Entity + 0x98, Array);
        Used += snprintf(Text + Used, Size - Used, "\n      array +0x98 (%d):", (int)Count);
        for (int k = 0; k < Count && k < 16 && Array != 0 && Used < Size - 80; ++k)
        {
            uintptr_t Component = 0;
            if (SafeQword(Array + (uintptr_t)k * 8, Component) && Component != 0)
            {
                Used += DescribeFirst(Text, Used, Size, Component);
            }
        }
        Used += snprintf(Text + Used, Size - Used, "\n");
        return Used - Start;
    }

    // The one exception that kills the process, wherever it happened. The
    // vectored handler above only writes faults inside the game's image, on
    // purpose: every guarded read in the injector faults first-chance outside
    // it and is then handled, and those would drown the log. Measured 18/09:
    // the host died on the real Brume Tower's teardown with exit code
    // 0xC0000005 and not one line here, so the fault was outside the image.
    // This filter only runs for an exception nobody handled.
    LPTOP_LEVEL_EXCEPTION_FILTER s_previous_filter = nullptr;
    uintptr_t s_self_base = 0;
    uintptr_t s_self_end = 0;

    int DescribeAddress(char* Text, int Used, int Size, uintptr_t Address)
    {
        if (InGame(Address))
        {
            return snprintf(Text + Used, Size - Used, "DarkSoulsII.exe+0x%llx", (unsigned long long)(Address - s_base));
        }
        if (Address >= s_self_base && Address < s_self_end)
        {
            return snprintf(Text + Used, Size - Used, "Injector.dll+0x%llx", (unsigned long long)(Address - s_self_base));
        }
        HMODULE Module = nullptr;
        char Name[MAX_PATH] = {};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)Address, &Module) && Module != nullptr && GetModuleFileNameA(Module, Name, sizeof(Name)) != 0)
        {
            const char* Base = strrchr(Name, '\\');
            return snprintf(Text + Used, Size - Used, "%s+0x%llx", Base != nullptr ? Base + 1 : Name,
                (unsigned long long)(Address - (uintptr_t)Module));
        }
        return snprintf(Text + Used, Size - Used, "%p (no module)", (void*)Address);
    }

    LONG WINAPI FatalFilter(EXCEPTION_POINTERS* Info)
    {
        const EXCEPTION_RECORD* Record = Info->ExceptionRecord;
        const CONTEXT* Context = Info->ContextRecord;
        SYSTEMTIME Now;
        GetLocalTime(&Now);
        char Text[4096];
        int Used = snprintf(Text, sizeof(Text), "%02u:%02u:%02u.%03u  FATAL exception %08lx at ", Now.wHour, Now.wMinute,
            Now.wSecond, Now.wMilliseconds, (unsigned long)Record->ExceptionCode);
        Used += DescribeAddress(Text, Used, (int)sizeof(Text), (uintptr_t)Context->Rip);
        Used += snprintf(Text + Used, sizeof(Text) - Used,
            " (thread %lu), %s 0x%llx\n    rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx rsi=%016llx rdi=%016llx\n"
            "    r8 =%016llx r9 =%016llx r12=%016llx r13=%016llx r14=%016llx r15=%016llx rsp=%016llx\n    stack:",
            GetCurrentThreadId(),
            Record->NumberParameters >= 2 ? (Record->ExceptionInformation[0] == 1 ? "writing" :
                (Record->ExceptionInformation[0] == 8 ? "executing" : "reading")) : "at",
            Record->NumberParameters >= 2 ? (unsigned long long)Record->ExceptionInformation[1] : 0ull,
            (unsigned long long)Context->Rax, (unsigned long long)Context->Rbx, (unsigned long long)Context->Rcx,
            (unsigned long long)Context->Rdx, (unsigned long long)Context->Rsi, (unsigned long long)Context->Rdi,
            (unsigned long long)Context->R8, (unsigned long long)Context->R9, (unsigned long long)Context->R12,
            (unsigned long long)Context->R13, (unsigned long long)Context->R14, (unsigned long long)Context->R15,
            (unsigned long long)Context->Rsp);
        if (Record->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
        {
            const uintptr_t* Stack = (const uintptr_t*)Context->Rsp;
            int Found = 0;
            for (int i = 0; i < kStackWords && Found < kMaxReturns && Used < (int)sizeof(Text) - 64; ++i)
            {
                MEMORY_BASIC_INFORMATION Region;
                if (i % 32 == 0 && (VirtualQuery(Stack + i, &Region, sizeof(Region)) == 0 || Region.State != MEM_COMMIT))
                {
                    break;
                }
                const uintptr_t Word = Stack[i];
                if (InGame(Word) || (Word >= s_self_base && Word < s_self_end))
                {
                    Used += snprintf(Text + Used, sizeof(Text) - Used, " ");
                    Used += DescribeAddress(Text, Used, (int)sizeof(Text), Word);
                    ++Found;
                }
            }
        }
        Used += snprintf(Text + Used, sizeof(Text) - Used, "\n");
        Write(Text, (size_t)Used < sizeof(Text) ? (size_t)Used : sizeof(Text) - 1);
        return s_previous_filter != nullptr ? s_previous_filter(Info) : EXCEPTION_CONTINUE_SEARCH;
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
        char Text[8192];
        int Used = snprintf(Text, sizeof(Text),
            "%02u:%02u:%02u.%03u  excecao %08lx em +0x%llx (thread %lu), %s 0x%llx\n"
            "    rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n"
            "    rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n"
            "    r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx\n"
            "    r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n"
            "    retornos no jogo:",
            Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds, (unsigned long)Code,
            (unsigned long long)(Context->Rip - s_base), GetCurrentThreadId(),
            Record->NumberParameters >= 2 ? (Record->ExceptionInformation[0] == 1 ? "writing" :
                (Record->ExceptionInformation[0] == 8 ? "executing" : "reading")) : "at",
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

        // Whose objects were in hand. A register that points at committed
        // memory whose first eight bytes are an address inside the game is an
        // object with a vftable, and the vftable's offset names the class in
        // Ghidra without any guessing. This is what the crashes of 17/09 were
        // missing: the faulting pointer said a block had been reused, and
        // nothing said who was still holding it.
        {
            static const char* const Names[16] = { "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
            const uintptr_t Values[16] = { (uintptr_t)Context->Rax, (uintptr_t)Context->Rbx,
                (uintptr_t)Context->Rcx, (uintptr_t)Context->Rdx, (uintptr_t)Context->Rsi,
                (uintptr_t)Context->Rdi, (uintptr_t)Context->Rbp, (uintptr_t)Context->Rsp,
                (uintptr_t)Context->R8, (uintptr_t)Context->R9, (uintptr_t)Context->R10,
                (uintptr_t)Context->R11, (uintptr_t)Context->R12, (uintptr_t)Context->R13,
                (uintptr_t)Context->R14, (uintptr_t)Context->R15 };
            int Objects = 0;
            int Header = 0;
            for (int i = 0; i < 16 && Used < (int)sizeof(Text) - 400; ++i)
            {
                const uintptr_t At = Values[i];
                if (At == 0 || (At & 7) != 0 || At == (uintptr_t)Context->Rsp || At == (uintptr_t)Context->Rbp)
                {
                    continue;
                }
                MEMORY_BASIC_INFORMATION Region;
                if (VirtualQuery((LPCVOID)At, &Region, sizeof(Region)) == 0 || Region.State != MEM_COMMIT ||
                    (Region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
                {
                    continue;
                }
                uintptr_t Vftable = 0;
                __try
                {
                    Vftable = *(const uintptr_t*)At;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    continue;
                }
                if (!InGame(Vftable))
                {
                    continue;
                }
                if (Header == 0)
                {
                    Used += snprintf(Text + Used, sizeof(Text) - Used, "    objetos na mao (tabela virtual):");
                    Header = 1;
                }
                Used += snprintf(Text + Used, sizeof(Text) - Used, " %s=+0x%llx", Names[i],
                    (unsigned long long)(Vftable - s_base));
                if (Vftable == s_base + kMapEntityVftable && Used < (int)sizeof(Text) - 1200)
                {
                    Used += snprintf(Text + Used, sizeof(Text) - Used, "\n");
                    Used += DescribeEntity(Text, Used, (int)sizeof(Text), Names[i], At);
                    Used += snprintf(Text + Used, sizeof(Text) - Used, "   ");
                }
                // The first two get their first sixty-four bytes written down,
                // which is where a stale pointer's neighbours are.
                if (Objects < 2)
                {
                    unsigned char Bytes[64] = {};
                    int Have = 0;
                    __try
                    {
                        memcpy(Bytes, (const void*)At, sizeof(Bytes));
                        Have = 1;
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        Have = 0;
                    }
                    if (Have != 0)
                    {
                        Used += snprintf(Text + Used, sizeof(Text) - Used, "\n      %s %p:", Names[i], (void*)At);
                        for (int b = 0; b < 64 && Used < (int)sizeof(Text) - 8; ++b)
                        {
                            Used += snprintf(Text + Used, sizeof(Text) - Used, "%s%02x",
                                (b % 8) == 0 ? " " : "", Bytes[b]);
                        }
                        Used += snprintf(Text + Used, sizeof(Text) - Used, "\n   ");
                    }
                }
                ++Objects;
            }
            if (Header != 0)
            {
                Used += snprintf(Text + Used, sizeof(Text) - Used, "\n");
            }
        }

        // The world's per-map network object tables at the moment of the fault
        // (FUN_140419a70: *(*(0x1416148f0)+0x40) + 0x20 + index*8, each table's
        // 0xa0-byte blocks at +0x18 and count at +0x20). On 18/09 every value
        // found written over a live object was the content of one of these
        // blocks, and the old table was proven to be written by nobody after
        // its map went; this says whether the live table sits on top of the
        // object that broke.
        {
            uintptr_t Global = 0, World = 0;
            if (SafeQword(s_base + 0x16148f0, Global) && Global != 0 && SafeQword(Global + 0x40, World) && World != 0)
            {
                int Header = 0;
                for (int k = 0; k < 0x2a && Used < (int)sizeof(Text) - 120; ++k)
                {
                    uintptr_t Table = 0, Blocks = 0, Count = 0;
                    if (!SafeQword(World + 0x20 + (uintptr_t)k * 8, Table) || Table == 0)
                    {
                        continue;
                    }
                    SafeQword(Table + 0x18, Blocks);
                    SafeQword(Table + 0x20, Count);
                    if (Header == 0)
                    {
                        Used += snprintf(Text + Used, sizeof(Text) - Used, "    object tables:");
                        Header = 1;
                    }
                    Used += snprintf(Text + Used, sizeof(Text) - Used, " [%d] %p blocks %p-%p", k, (void*)Table,
                        (void*)Blocks, (void*)(Blocks + (uintptr_t)(uint32_t)Count * 0xa0));
                }
                if (Header != 0)
                {
                    Used += snprintf(Text + Used, sizeof(Text) - Used, "\n");
                }
            }
        }

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

    {
        HMODULE Self = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)&FatalFilter, &Self) && Self != nullptr)
        {
            const IMAGE_DOS_HEADER* SelfDos = (const IMAGE_DOS_HEADER*)Self;
            const IMAGE_NT_HEADERS64* SelfNt = (const IMAGE_NT_HEADERS64*)((uintptr_t)Self + SelfDos->e_lfanew);
            s_self_base = (uintptr_t)Self;
            s_self_end = s_self_base + SelfNt->OptionalHeader.SizeOfImage;
        }
    }
    s_previous_filter = SetUnhandledExceptionFilter(FatalFilter);

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
