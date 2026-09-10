/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_UnlockAreaMultiPlayHook.h"
#include "Injector/Config/RuntimeConfig.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"
#include "Shared/Platform/Platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
#ifdef _WIN32

    // Layout of the loaded param, read out of a live one. Offsets are from the
    // start of the header, which sits 0x0C before the type name.
    constexpr size_t kNameOffset      = 0x0c;   // "NETWORK_AREA_PARAM"
    constexpr size_t kRowCountOffset  = 0x0a;   // uint16
    constexpr size_t kIndexOffset     = 0x48;   // first index entry
    constexpr size_t kIndexStride     = 0x18;   // { int64 data, int64 name, int64 id }
    constexpr size_t kRowSize         = 28;
    constexpr size_t kRowMaskOffset   = 0x18;   // the permission bitmask ends the row

    constexpr const char* kParamName = "NETWORK_AREA_PARAM";

    // Every area in the game carries one of these. The full mask is what most
    // of the world already has; Majula is the odd one out at 4.
    constexpr int32_t kFullMask = 63;

    std::atomic<bool> s_running{ false };
    std::thread s_thread;
    std::filesystem::path s_log_path;

    void Append(const std::string& Text)
    {
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream.is_open())
        {
            Stream << Text;
        }
    }

    // Kept apart from anything that needs unwinding: MSVC will not accept
    // __try beside it, and every read here is into memory the game owns.
    bool TryRead(const void* From, void* Into, size_t Length)
    {
        __try
        {
            memcpy(Into, From, Length);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool TryWrite(void* Into, const void* From, size_t Length)
    {
        __try
        {
            memcpy(Into, From, Length);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Finds the loaded param by its own type name. There is no fixed address
    // for it; it is heap memory that moves between runs.
    std::vector<uintptr_t> FindParamHeaders()
    {
        const size_t NameLength = strlen(kParamName);

        SYSTEM_INFO Info = {};
        GetSystemInfo(&Info);

        uintptr_t At = (uintptr_t)Info.lpMinimumApplicationAddress;
        const uintptr_t End = (uintptr_t)Info.lpMaximumApplicationAddress;

        std::vector<uint8_t> Buffer(1024 * 1024);
        std::vector<uintptr_t> Found;

        while (At < End && Found.size() < 8)
        {
            MEMORY_BASIC_INFORMATION Region = {};
            if (VirtualQuery((LPCVOID)At, &Region, sizeof(Region)) == 0)
            {
                break;
            }

            const uintptr_t Next = (uintptr_t)Region.BaseAddress + Region.RegionSize;

            const bool Readable =
                Region.State == MEM_COMMIT &&
                (Region.Protect & PAGE_GUARD) == 0 &&
                (Region.Protect & PAGE_NOACCESS) == 0;

            if (Readable && Region.RegionSize <= 512u * 1024u * 1024u)
            {
                size_t Done = 0;
                while (Done < Region.RegionSize && Found.size() < 8)
                {
                    const size_t Take = (std::min)(Buffer.size(), Region.RegionSize - Done);
                    const uintptr_t From = (uintptr_t)Region.BaseAddress + Done;
                    if (TryRead((const void*)From, Buffer.data(), Take))
                    {
                        for (size_t i = 0; i + NameLength <= Take; ++i)
                        {
                            if (memcmp(Buffer.data() + i, kParamName, NameLength) == 0)
                            {
                                const uintptr_t Name = From + i;
                                if (Name >= kNameOffset)
                                {
                                    Found.push_back(Name - kNameOffset);
                                }
                            }
                        }
                    }
                    Done += Take;
                }
            }

            if (Next <= At)
            {
                break;
            }
            At = Next;
        }

        return Found;
    }

    // Returns how many rows were raised, or -1 if the header did not check out.
    int PatchOne(uintptr_t Header)
    {
        uint16_t RowCount = 0;
        if (!TryRead((const void*)(Header + kRowCountOffset), &RowCount, sizeof(RowCount)))
        {
            return -1;
        }
        if (RowCount == 0 || RowCount > 512)
        {
            return -1;
        }

        int Raised = 0;
        for (uint16_t i = 0; i < RowCount; ++i)
        {
            const uintptr_t Entry = Header + kIndexOffset + (size_t)i * kIndexStride;

            int64_t DataOffset = 0;
            int64_t RowId = 0;
            if (!TryRead((const void*)Entry, &DataOffset, sizeof(DataOffset)) ||
                !TryRead((const void*)(Entry + 0x10), &RowId, sizeof(RowId)))
            {
                continue;
            }
            if (DataOffset <= 0 || DataOffset > 0x100000 || RowId <= 0)
            {
                continue;
            }

            const uintptr_t Mask = Header + (uintptr_t)DataOffset + kRowMaskOffset;

            int32_t Current = 0;
            if (!TryRead((const void*)Mask, &Current, sizeof(Current)))
            {
                continue;
            }
            if (Current < 0 || Current > kFullMask || Current == kFullMask)
            {
                continue;
            }

            if (TryWrite((void*)Mask, &kFullMask, sizeof(kFullMask)))
            {
                Append(StringFormat("    area=0x%08llx antes=%d agora=%d\n",
                    (unsigned long long)RowId, (int)Current, (int)kFullMask));
                Raised++;
            }
        }

        return Raised;
    }

    void Run()
    {
        // The param is not loaded when the injector runs, so keep looking. It
        // is patched again on later passes because a row raised once should
        // stay raised even if the game reloads the table.
        int Attempts = 0;
        while (s_running.load() && Attempts < 600)
        {
            Attempts++;

            const std::vector<uintptr_t> Headers = FindParamHeaders();
            if (!Headers.empty())
            {
                int Total = 0;
                for (uintptr_t Header : Headers)
                {
                    const int Raised = PatchOne(Header);
                    if (Raised > 0)
                    {
                        Total += Raised;
                    }
                }

                if (Total > 0)
                {
                    Append(StringFormat(
                        "event=DS2UnlockAreas result=patched headers=%zu linhas=%d mascara=%d\n",
                        Headers.size(), Total, (int)kFullMask));
                    Log("[DS2UnlockAreas] %d areas abertas para multiplayer", Total);
                }
            }

            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

#endif
}

bool DS2_UnlockAreaMultiPlayHook::Install(Injector& injector)
{
#ifdef _WIN32
    s_log_path = injector.GetDllPath() / "DS2_UnlockAreas.log";

    {
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream.is_open())
        {
            Stream << "\n============ abrindo areas para multiplayer ============\n";
        }
    }

    s_running.store(true);
    s_thread = std::thread(Run);

    Log("[DS2UnlockAreas] procurando NETWORK_AREA_PARAM");
#endif
    return true;
}

void DS2_UnlockAreaMultiPlayHook::Uninstall()
{
#ifdef _WIN32
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }
#endif
}

const char* DS2_UnlockAreaMultiPlayHook::GetName()
{
    return "DS2 Unlock Area MultiPlay";
}
