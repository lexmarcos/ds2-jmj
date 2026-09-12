/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_NavHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
#ifdef _WIN32

    // Version 1.03 Calibrations 2.02.
    constexpr size_t kContextOffset = 0x16148f0;
    constexpr size_t kOwnerOffset = 0xa8;
    constexpr size_t kPlayerOffset = 0xc0;
    constexpr size_t kPositionOffset = 0xa8;   // x, y, z - the triple that moves
    constexpr size_t kFacingXOffset = 0xbc;
    constexpr size_t kFacingZOffset = 0xc4;

    uintptr_t s_base = 0;

    std::atomic<bool> s_running{ false };
    std::thread s_thread;

    std::filesystem::path s_state_path;
    std::filesystem::path s_temp_path;

    // Isolated because MSVC refuses __try in a function that unwinds C++
    // objects, and every caller below holds one.
    bool ReadGuarded(uintptr_t At, void* Out, size_t Length)
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

    bool ReadPointer(uintptr_t At, uintptr_t& Out)
    {
        uintptr_t Value = 0;
        if (!ReadGuarded(At, &Value, sizeof(Value)))
        {
            return false;
        }
        Out = Value;
        return Value != 0;
    }

    bool Resolve(uintptr_t& Player)
    {
        uintptr_t Context = 0;
        uintptr_t Owner = 0;
        return ReadPointer(s_base + kContextOffset, Context) &&
               ReadPointer(Context + kOwnerOffset, Owner) &&
               ReadPointer(Owner + kPlayerOffset, Player);
    }

    void Publish(const std::string& Line)
    {
        // Written through a rename so a polling reader never catches half a
        // line. The rename can fail - it did, under Wine, while the reader had
        // the file open - and swallowing that leaves the old sample in place,
        // which reads as a current position and is the worst kind of wrong. So
        // a failed rename falls back to writing in place: a torn read fails to
        // parse and is discarded, and the tick below catches what is left.
        {
            std::ofstream Stream(s_temp_path, std::ios::trunc);
            if (!Stream)
            {
                return;
            }
            Stream << Line;
        }

        std::error_code Error;
        std::filesystem::rename(s_temp_path, s_state_path, Error);
        if (Error)
        {
            std::ofstream Stream(s_state_path, std::ios::trunc);
            if (Stream)
            {
                Stream << Line;
            }
        }
    }

    void Run()
    {
        // Counts samples, not milliseconds. A reader cannot tell a position
        // that has not changed from a file that has not been written, and the
        // difference decides whether a character is standing still or the
        // publisher stopped - which is a walk that arrived against a walk that
        // is stuck.
        uint64_t Tick = 0;

        while (s_running.load())
        {
            ++Tick;
            uintptr_t Player = 0;
            float Position[3] = {};
            float FacingX = 0.0f;
            float FacingZ = 0.0f;

            if (Resolve(Player) &&
                ReadGuarded(Player + kPositionOffset, Position, sizeof(Position)) &&
                ReadGuarded(Player + kFacingXOffset, &FacingX, sizeof(FacingX)) &&
                ReadGuarded(Player + kFacingZOffset, &FacingZ, sizeof(FacingZ)))
            {
                Publish(StringFormat("%.4f %.4f %.4f %.4f %.4f %p %llu\n",
                    Position[0], Position[1], Position[2], FacingX, FacingZ,
                    (void*)Player, (unsigned long long)Tick));
            }
            else
            {
                // No world loaded, or the player was torn down mid-read. Saying
                // so beats leaving a stale position that reads as current.
                Publish(StringFormat("sem jogador %llu\n", (unsigned long long)Tick));
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

#endif
}

bool DS2_NavHook::Install(Injector& injector)
{
#ifdef _WIN32
    s_base = (uintptr_t)injector.GetBaseAddress();
    s_state_path = injector.GetDllPath() / "DS2_Nav.txt";
    s_temp_path = injector.GetDllPath() / "DS2_Nav.tmp";

    s_running.store(true);
    s_thread = std::thread(Run);

    Log("[DS2_NavHook] pronto; a posicao do jogador sai em DS2_Nav.txt");
#endif
    return true;
}

void DS2_NavHook::Uninstall()
{
#ifdef _WIN32
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }
#endif
}

const char* DS2_NavHook::GetName()
{
    return "DS2 Nav";
}
