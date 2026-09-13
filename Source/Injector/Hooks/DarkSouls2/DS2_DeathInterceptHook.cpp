/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_DeathInterceptHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"

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

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#include "ThirdParty/detours/src/detours.h"
#endif

namespace
{
#ifdef _WIN32

    // Version 1.03 Calibrations 2.02, verified before anything is written.
    //
    // Slot +0x20 of ChrDeadActionCtrl, `void(ctrl, float delta)`, once a frame
    // for every character. The only function here that is ever held.
    constexpr size_t kUpdateOffset = 0x13c720;
    constexpr uint8_t kUpdateBytes[] = { 0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0x51, 0x08 };

    // Slot +0x10, `void(ctrl)`. Reads the same byte and fires the same
    // notifications, but never moves the state. Watched only.
    constexpr size_t kReplicaOffset = 0x13c3b0;
    constexpr uint8_t kReplicaBytes[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30 };

    // `void(ctrl, char kind)`, reached by a tail jump from `FUN_14030eb20`, a
    // virtual in five vftables. Kind 1 or 2 fires every consequence of a death
    // in one call and parks the controller in state 3. Watched only.
    constexpr size_t kInstantOffset = 0x13c500;
    constexpr uint8_t kInstantBytes[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x8d, 0x42, 0xff };

    constexpr size_t kContextOffset = 0x16148f0;
    constexpr size_t kLocalCharacter = 0xd0;   // ctx+0xd0, rebuilt on every load

    // ChrDeadActionCtrl
    constexpr size_t kCtrlCharacter = 0x08;
    constexpr size_t kCtrlState = 0x10;        // 0 alive, 1 waiting, 2 dying, 3 done
    constexpr size_t kCtrlNotified = 0x34;     // FUN_14013d430 already ran
    constexpr size_t kCtrlRewarded = 0x38;     // FUN_14013d560 already ran

    // The character
    constexpr size_t kCharacterData = 0xb8;
    constexpr size_t kHp = 0x168;
    constexpr size_t kHpMax = 0x174;           // after hollowing; +0x170 is the base

    // *(chr+0xb8)
    constexpr size_t kStateBits = 0x4c8;
    constexpr size_t kDeferred = 0x5fc;        // nonzero: the controller does not look
    constexpr size_t kPending = 0x759;
    constexpr size_t kParams = 0x75c;          // through +0x76d
    constexpr size_t kParamsLength = 0x12;

    // What the controller's own tail keeps clear while it sits in state 0.
    constexpr uint64_t kDyingBits = 0x4000 | 0x8000;

    enum Mode : int
    {
        Observe = 0,
        Cancel = 1,
    };

    using Update_p = void(*)(void* Ctrl, float Delta);
    using Replica_p = void(*)(void* Ctrl);
    using Instant_p = void(*)(void* Ctrl, char Kind);

    Update_p s_original_update = nullptr;
    Replica_p s_original_replica = nullptr;
    Instant_p s_original_instant = nullptr;

    uintptr_t s_base = 0;

    std::atomic<int> s_mode{ Observe };
    std::atomic<uint64_t> s_seen{ 0 };
    std::atomic<uint64_t> s_cancelled{ 0 };
    std::atomic<uint64_t> s_unexplained{ 0 };
    std::atomic<uint64_t> s_instant{ 0 };
    std::atomic<uint64_t> s_replica_calls{ 0 };

    // Touched only from the game's thread, inside the detours.
    void* s_local_ctrl = nullptr;
    uint8_t s_local_state = 0xff;

    struct Watched
    {
        void* Ctrl;
        uint32_t Signature;
    };
    Watched s_replicas[16] = {};

    std::atomic<bool> s_running{ false };
    std::thread s_thread;

    std::mutex s_log_mutex;
    std::filesystem::path s_log_path;
    std::filesystem::path s_request_path;

    void Append(const std::string& Text)
    {
        std::scoped_lock Lock(s_log_mutex);
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream)
        {
            Stream << Text;
        }
    }

    // Wall clock, so a line can be set beside the server's log.
    std::string Clock()
    {
        SYSTEMTIME Now;
        GetLocalTime(&Now);
        return StringFormat("%02u:%02u:%02u.%03u", Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds);
    }

    std::string Hex(const uint8_t* Bytes, size_t Length)
    {
        std::string Out;
        for (size_t i = 0; i < Length; ++i)
        {
            Out += StringFormat("%02x", Bytes[i]);
        }
        return Out;
    }

    void* LocalCharacter()
    {
        const uintptr_t Context = *(const uintptr_t*)(s_base + kContextOffset);
        return Context == 0 ? nullptr : *(void**)(Context + kLocalCharacter);
    }

    // The parameters the controller would have copied: who killed (a handle
    // FUN_14017b4f0 resolves), the flags that suppress single consequences,
    // and the cause that picks the timing row (10 for HP).
    std::string DescribeParams(const uint8_t* Data)
    {
        const uint8_t* P = Data + kParams;
        return StringFormat("matador=%08x flags=%08x causa=%u bruto=%s",
            *(const uint32_t*)P,
            *(const uint32_t*)(P + 0x04),
            *(const uint32_t*)(P + 0x0c),
            Hex(P, kParamsLength).c_str());
    }

    void UpdateHook(void* Ctrl, float Delta)
    {
        uint8_t* Bytes = (uint8_t*)Ctrl;
        void* Character = *(void**)(Bytes + kCtrlCharacter);
        if (Character == nullptr || Character != LocalCharacter())
        {
            s_original_update(Ctrl, Delta);
            return;
        }

        uint8_t* Chr = (uint8_t*)Character;
        uint8_t* Data = *(uint8_t**)(Chr + kCharacterData);
        const uint8_t Before = Bytes[kCtrlState];

        if (Ctrl != s_local_ctrl)
        {
            s_local_ctrl = Ctrl;
            s_local_state = Before;
            Append(StringFormat("%s  controlador do jogador local %p, personagem %p, estado %u\n",
                Clock().c_str(), Ctrl, Character, Before));
        }

        // The same three tests the controller makes, in its order. The HP
        // source runs earlier in the frame, so the byte is already there.
        const bool Pending = Before == 0 && Data != nullptr
            && *(const int32_t*)(Data + kDeferred) == 0
            && Data[kPending] != 0;

        if (Pending)
        {
            const int32_t Hp = *(const int32_t*)(Chr + kHp);
            const int32_t Max = *(const int32_t*)(Chr + kHpMax);
            const std::string Params = DescribeParams(Data);

            if (s_mode.load() == Cancel)
            {
                // Never leave the byte set: with it cleared and the HP back,
                // the source has nothing to say next frame.
                Data[kPending] = 0;
                if (Max > 0)
                {
                    *(int32_t*)(Chr + kHp) = Max;
                }
                *(uint64_t*)(Data + kStateBits) &= ~kDyingBits;

                // A cancel on every frame means something keeps killing and
                // the HP did not hold; say so without filling the disk.
                const uint64_t Count = ++s_cancelled;
                if (Count <= 20 || Count % 300 == 0)
                {
                    Append(StringFormat("%s  morte CANCELADA #%llu hp=%d -> %d %s\n",
                        Clock().c_str(), (unsigned long long)Count, Hp, Max, Params.c_str()));
                }
                return;
            }

            ++s_seen;
            Append(StringFormat("%s  morte vista hp=%d max=%d %s\n",
                Clock().c_str(), Hp, Max, Params.c_str()));
        }

        s_original_update(Ctrl, Delta);

        const uint8_t After = Bytes[kCtrlState];
        if (After != s_local_state)
        {
            // Leaving state 0 with no byte beforehand means FUN_14013cc30 set
            // it inside the call - character flags or animation event 0x19 -
            // and that death went past the check above.
            const bool Unexplained = Before == 0 && After != 0 && !Pending;
            if (Unexplained)
            {
                ++s_unexplained;
            }
            Append(StringFormat("%s  estado do controlador %u -> %u%s\n",
                Clock().c_str(), s_local_state, After,
                Unexplained ? "  SEM +0x759 ANTES: veio de FUN_14013cc30 e nao foi interceptada" : ""));
            s_local_state = After;
        }
    }

    void ReplicaHook(void* Ctrl)
    {
        ++s_replica_calls;
        if (Ctrl != nullptr)
        {
            const uint8_t* Bytes = (const uint8_t*)Ctrl;
            const void* Character = *(void**)(Bytes + kCtrlCharacter);

            // Dereference only where the original does.
            uint8_t Pending = 0;
            if (Character != nullptr && Bytes[kCtrlState] == 0)
            {
                const uint8_t* Data = *(const uint8_t**)((const uint8_t*)Character + kCharacterData);
                Pending = Data == nullptr ? 0 : Data[kPending];
            }

            const uint32_t Signature = (uint32_t)Bytes[kCtrlState]
                | ((uint32_t)Pending << 8)
                | ((uint32_t)Bytes[kCtrlNotified] << 16)
                | ((uint32_t)Bytes[kCtrlRewarded] << 24);

            Watched* Slot = nullptr;
            bool Known = false;
            for (Watched& Entry : s_replicas)
            {
                if (Entry.Ctrl == Ctrl)
                {
                    Slot = &Entry;
                    Known = true;
                    break;
                }
                if (Slot == nullptr && Entry.Ctrl == nullptr)
                {
                    Slot = &Entry;
                }
            }

            if (Slot != nullptr && (!Known || Slot->Signature != Signature))
            {
                Slot->Ctrl = Ctrl;
                Slot->Signature = Signature;
                Append(StringFormat("%s  slot +0x10 controlador=%p personagem=%p local=%d estado=%u +0x759=%u notificado=%u recompensado=%u de=+0x%zx\n",
                    Clock().c_str(), Ctrl, Character, Character != nullptr && Character == LocalCharacter() ? 1 : 0,
                    Bytes[kCtrlState], Pending, Bytes[kCtrlNotified], Bytes[kCtrlRewarded],
                    (size_t)((uintptr_t)_ReturnAddress() - s_base)));
            }
        }

        s_original_replica(Ctrl);
    }

    void InstantHook(void* Ctrl, char Kind)
    {
        if ((uint8_t)(Kind - 1) < 2 && Ctrl != nullptr)
        {
            const void* Character = *(void**)((uint8_t*)Ctrl + kCtrlCharacter);
            const bool Local = Character != nullptr && Character == LocalCharacter();
            const uint64_t Count = ++s_instant;
            if (Local || Count <= 30)
            {
                Append(StringFormat("%s  morte instantanea tipo=%d controlador=%p personagem=%p local=%d de=+0x%zx\n",
                    Clock().c_str(), (int)Kind, Ctrl, Character, Local ? 1 : 0,
                    (size_t)((uintptr_t)_ReturnAddress() - s_base)));
            }
        }

        s_original_instant(Ctrl, Kind);
    }

    bool BytesMatch(uintptr_t Address, const uint8_t* Expected, size_t Length)
    {
        return memcmp((const void*)Address, Expected, Length) == 0;
    }

    void Apply(const std::string& Line)
    {
        std::istringstream Parts(Line);
        std::string Verb;
        Parts >> Verb;

        if (Verb == "observe")
        {
            s_mode.store(Observe);
            Append(StringFormat("%s  === modo: observar ===\n", Clock().c_str()));
        }
        else if (Verb == "cancel")
        {
            s_mode.store(Cancel);
            Append(StringFormat("%s  === modo: cancelar a morte do jogador local ===\n", Clock().c_str()));
        }
        else if (Verb == "status")
        {
            Append(StringFormat("%s  === modo %s: vistas=%llu canceladas=%llu sem_+0x759=%llu instantaneas=%llu chamadas_slot_+0x10=%llu ===\n",
                Clock().c_str(), s_mode.load() == Cancel ? "cancelar" : "observar",
                (unsigned long long)s_seen.load(), (unsigned long long)s_cancelled.load(),
                (unsigned long long)s_unexplained.load(), (unsigned long long)s_instant.load(),
                (unsigned long long)s_replica_calls.load()));
        }
        else if (!Verb.empty())
        {
            Append(StringFormat("%s  === nao entendi: %s ===\n", Clock().c_str(), Line.c_str()));
        }
    }

    void Run()
    {
        while (s_running.load())
        {
            std::error_code Error;
            if (std::filesystem::exists(s_request_path, Error))
            {
                std::ifstream Stream(s_request_path);
                std::string Line;
                while (std::getline(Stream, Line))
                {
                    if (!Line.empty())
                    {
                        Apply(Line);
                    }
                }
                Stream.close();
                std::filesystem::remove(s_request_path, Error);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

#endif
}

bool DS2_DeathInterceptHook::Install(Injector& injector)
{
#ifdef _WIN32
    s_base = (uintptr_t)injector.GetBaseAddress();

    struct { size_t Offset; const uint8_t* Bytes; size_t Length; const char* Name; } Checks[] = {
        { kUpdateOffset, kUpdateBytes, sizeof(kUpdateBytes), "controlador da morte" },
        { kReplicaOffset, kReplicaBytes, sizeof(kReplicaBytes), "slot +0x10" },
        { kInstantOffset, kInstantBytes, sizeof(kInstantBytes), "morte instantanea" },
    };
    for (const auto& Check : Checks)
    {
        if (!BytesMatch(s_base + Check.Offset, Check.Bytes, Check.Length))
        {
            Error("[DS2_DeathInterceptHook] %s em +0x%zx nao e o esperado; recusando",
                Check.Name, Check.Offset);
            return false;
        }
    }

    s_log_path = injector.GetDllPath() / "DS2_Death.log";
    s_request_path = injector.GetDllPath() / "DS2_Death.req";

    s_original_update = (Update_p)(s_base + kUpdateOffset);
    s_original_replica = (Replica_p)(s_base + kReplicaOffset);
    s_original_instant = (Instant_p)(s_base + kInstantOffset);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_update, UpdateHook);
    DetourAttach(&(PVOID&)s_original_replica, ReplicaHook);
    DetourAttach(&(PVOID&)s_original_instant, InstantHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        Error("[DS2_DeathInterceptHook] nao consegui instalar os detours");
        return false;
    }

    s_running.store(true);
    s_thread = std::thread(Run);

    Append(StringFormat("%s  === ds2os morte: modo observar ===\n", Clock().c_str()));
    Log("[DS2_DeathInterceptHook] pronto; escreva cancel em DS2_Death.req para segurar a morte");
#endif
    return true;
}

void DS2_DeathInterceptHook::Uninstall()
{
#ifdef _WIN32
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }

    if (s_original_update != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_update, UpdateHook);
        DetourDetach(&(PVOID&)s_original_replica, ReplicaHook);
        DetourDetach(&(PVOID&)s_original_instant, InstantHook);
        DetourTransactionCommit();
        s_original_update = nullptr;
    }
#endif
}

const char* DS2_DeathInterceptHook::GetName()
{
    return "DS2 Death Intercept";
}
