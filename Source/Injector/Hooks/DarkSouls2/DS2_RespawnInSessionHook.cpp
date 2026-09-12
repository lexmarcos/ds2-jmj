/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_RespawnInSessionHook.h"
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
#include "ThirdParty/detours/src/detours.h"
#endif

namespace
{
#ifdef _WIN32

    // Version 1.03 Calibrations 2.02. Every one is verified before a byte is
    // written or a call is made.
    constexpr size_t kPhantomDeathOffset = 0x190950;
    constexpr uint8_t kPhantomDeathBytes[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57 };

    // The state-7 handler. Detoured only to remember the session it is handed,
    // because the death terminal has no way to reach it: the path the game
    // takes there locks a weak pointer, and a hook cannot.
    constexpr size_t kPlayingOffset = 0x2c3830;
    constexpr uint8_t kPlayingBytes[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57 };

    // The flavour byte a join puts at request+0x14, computed from the role.
    constexpr size_t kFlavourOffset = 0x2d4830;
    constexpr uint8_t kFlavourBytes[] = { 0x48, 0x83, 0xec, 0x28, 0x8b, 0x09 };

    // Called with *(ctx+0x22e0) immediately before the warp by both the join
    // and the teardown. Copied because they both do it, not because its job is
    // understood.
    constexpr size_t kPrepareOffset = 0x500fd0;
    constexpr uint8_t kPrepareBytes[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20 };

    constexpr size_t kContextOffset = 0x16148f0;
    constexpr size_t kWarpSlot = 0x40;
    constexpr size_t kPrepareArgOffset = 0x22e0;
    // What the warp entry tests before it looks at the request at all.
    constexpr size_t kEntryState = 0x24ac;   // must be 0x1e
    constexpr size_t kEntryFlags = 0x24b1;   // bit 2 must be clear

    // The permission gate the join's flag has to pass. FUN_140248940 inverts a
    // virtual that is four instructions long - it asks whether this counter is
    // positive - so the counter is the whole gate, and it can be lifted for the
    // length of one call and put back.
    constexpr size_t kGateOwner = 0xd0;
    constexpr size_t kGateCounter = 0x168;

    // Session fields, all read live and all confirmed on a running co-op.
    constexpr size_t kSessionRole = 0xd8;
    constexpr size_t kSessionState = 0xf8;
    constexpr size_t kSessionHostMap = 0x19c;
    constexpr size_t kSessionPosition = 0x1a4;
    constexpr size_t kStatePlaying = 7;

    // Why the session layer is being asked to act. Measured: 2 is "the guest
    // died". The host dying reaches this same terminal on the guest's client
    // with a different one, and there is nothing to save in that case - the
    // session ends either way, and swallowing the goodbye only leaves the host
    // hanging on a farewell that never comes. It did, for good.
    constexpr uint32_t kReasonGuestDied = 2;

    struct WarpRequest
    {
        uint32_t Kind;
        uint32_t Reason;
        uint32_t Map;
        uint32_t Unknown0c;
        uint32_t Unknown10;
        uint8_t Flavour;
        uint8_t Pad15[3];
        float X, Y, Z, W;
        float Rotation[4];
    };
    static_assert(sizeof(WarpRequest) == 0x38, "the warp request is 0x38 bytes");

    using PhantomDeath_p = void(*)(void* Record, uint32_t Reason);
    PhantomDeath_p s_original_death = nullptr;

    using Playing_p = void(*)(void* Session, float Delta);
    Playing_p s_original_playing = nullptr;

    using Flavour_p = uint8_t(*)(void* RoleByte);
    Flavour_p s_flavour = nullptr;

    using Prepare_p = void(*)(void* Argument);
    Prepare_p s_prepare = nullptr;

    using Warp_p = uint8_t(*)(void* Context, WarpRequest* Request, uint8_t Flag);

    uintptr_t s_base = 0;
    std::atomic<void*> s_session{ nullptr };
    std::atomic<bool> s_enabled{ false };
    // Which flag to hand the warp. One is the form a join uses and goes
    // through the permission gate; zero is the form the teardown uses and
    // skips it. The first attempt was refused with one, and the teardown's own
    // warp was accepted in the same instant with zero, so both are worth
    // trying - the gate is the only thing that reads this.
    std::atomic<uint8_t> s_flag{ 1 };
    // Lift the gate around the warp when it would refuse. Measured: with the
    // flag clear the request is accepted but sends the player *home* - the map
    // and position are ignored, because that flag is what selects "go home"
    // rather than "go to this place". Only the join's flag carries a
    // destination, and only the gate stands in its way.
    std::atomic<bool> s_lift{ false };
    std::atomic<bool> s_running{ false };
    std::thread s_thread;

    std::filesystem::path s_log_path;
    std::filesystem::path s_request_path;

    void Append(const std::string& Text)
    {
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream)
        {
            Stream << Text;
        }
    }

    void PlayingHook(void* Session, float Delta)
    {
        // Nothing but a note of who is playing. The state-7 handler runs every
        // frame, so this is always fresh, and it is the only place the session
        // is handed over without a weak pointer in the way.
        s_session.store(Session);
        s_original_playing(Session, Delta);
    }

    void PhantomDeathHook(void* Record, uint32_t Reason)
    {
        void* Session = s_session.load();

        bool Redirect = s_enabled.load() && Record != nullptr && Session != nullptr &&
                        Reason == kReasonGuestDied;
        uint32_t State = 0;
        unsigned Role = 0xff;

        if (Redirect)
        {
            const uint8_t* Bytes = (const uint8_t*)Session;
            State = *(const uint32_t*)(Bytes + kSessionState);
            Role = *(const uint8_t*)(Bytes + kSessionRole);
            Redirect = State == kStatePlaying;
        }

        if (!Redirect)
        {
            Append(StringFormat(
                "  morte de fantasma: motivo=%u estado=%u papel=%u -> original%s\n",
                Reason, State, Role,
                (s_enabled.load() && Reason != kReasonGuestDied) ? " (nao foi o convidado que morreu)" : ""));
            s_original_death(Record, Reason);
            return;
        }

        const uintptr_t Context = *(uintptr_t*)(s_base + kContextOffset);
        if (Context == 0)
        {
            s_original_death(Record, Reason);
            return;
        }

        const uint8_t* SessionBytes = (const uint8_t*)Session;
        const float* Position = (const float*)(SessionBytes + kSessionPosition);

        WarpRequest Request = {};
        Request.Kind = 0;
        Request.Reason = 4;
        Request.Map = *(const uint32_t*)(SessionBytes + kSessionHostMap);
        Request.Unknown0c = 0xffffffff;
        Request.Unknown10 = 0;
        Request.Flavour = s_flavour((void*)(SessionBytes + kSessionRole));
        Request.X = Position[0];
        Request.Y = Position[1];
        Request.Z = Position[2];
        Request.W = 1.0f;
        Request.Rotation[0] = 0.0f;
        Request.Rotation[1] = 0.0f;
        Request.Rotation[2] = 0.0f;
        Request.Rotation[3] = 1.0f;

        void* PrepareArgument = *(void**)(Context + kPrepareArgOffset);
        if (PrepareArgument != nullptr)
        {
            s_prepare(PrepareArgument);
        }

        // Read before the attempt, because a refusal is worth nothing without
        // knowing which of the three tests said no.
        const uint32_t EntryState = *(const uint32_t*)(Context + kEntryState);
        const uint8_t EntryFlags = *(const uint8_t*)(Context + kEntryFlags);

        // The gate, read and optionally held open. Restored immediately: it
        // is a counter the game keeps for its own reasons, and leaving it
        // raised would be a lie told to everything else that reads it.
        int32_t* Counter = nullptr;
        int32_t Saved = 0;
        const uintptr_t GateOwner = *(uintptr_t*)(Context + kGateOwner);
        if (GateOwner != 0)
        {
            Counter = (int32_t*)(GateOwner + kGateCounter);
            Saved = *Counter;
        }

        const uint8_t Flag = s_flag.load();
        const bool Lift = s_lift.load() && Counter != nullptr && Saved <= 0;
        if (Lift)
        {
            *Counter = 1;
        }

        Warp_p Warp = *(Warp_p*)(*(uintptr_t*)Context + kWarpSlot);
        const uint8_t Accepted = Warp((void*)Context, &Request, Flag);

        if (Lift)
        {
            *Counter = Saved;
        }

        Append(StringFormat(
            "  morte de fantasma: motivo=%u papel=%u mapa=%08x sabor=%u destino=%.2f,%.2f,%.2f "
            "flag=%u [+0x24ac]=%08x [+0x24b1]=%02x portao=%d%s aceito=%u\n",
            Reason, Role, Request.Map, (unsigned)Request.Flavour,
            Request.X, Request.Y, Request.Z, (unsigned)Flag,
            EntryState, (unsigned)EntryFlags, Saved, Lift ? " (erguido)" : "",
            (unsigned)Accepted));

        if (!Accepted)
        {
            // Refused, so the player is still dead and nothing has been
            // marked. Hand it back rather than leave them stuck on a death
            // screen with no way out.
            Append("  recusado; deixando a morte normal seguir\n");
            s_original_death(Record, Reason);
            return;
        }

        // What the original does once it has acted: the record is done.
        *((uint8_t*)Record + 0xce) = 1;
    }

    bool BytesMatch(uintptr_t Address, const uint8_t* Expected, size_t Length)
    {
        return memcmp((const void*)Address, Expected, Length) == 0;
    }

    void Run()
    {
        while (s_running.load())
        {
            std::error_code Error;
            if (std::filesystem::exists(s_request_path, Error))
            {
                std::string Contents;
                {
                    std::ifstream Stream(s_request_path);
                    std::getline(Stream, Contents);
                }
                std::filesystem::remove(s_request_path, Error);

                // "0" off, "1" on with the join's flag, "2" on with the
                // teardown's flag.
                // "0" off, "1" the join's flag, "2" the teardown's flag,
                // "3" the join's flag with the gate held open for the call.
                const char Choice = Contents.empty() ? '0' : Contents[0];
                s_enabled.store(Choice != '0');
                s_flag.store(Choice == '2' ? 0 : 1);
                s_lift.store(Choice == '3');
                Append(StringFormat("=== renascer na sessao %s, flag %u, portao %s ===\n",
                    Choice == '0' ? "desligado" : "ligado", (unsigned)s_flag.load(),
                    s_lift.load() ? "erguido" : "intacto"));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

#endif
}

bool DS2_RespawnInSessionHook::Install(Injector& injector)
{
#ifdef _WIN32
    s_base = (uintptr_t)injector.GetBaseAddress();

    struct { size_t Offset; const uint8_t* Bytes; size_t Length; const char* Name; } Checks[] = {
        { kPhantomDeathOffset, kPhantomDeathBytes, sizeof(kPhantomDeathBytes), "morte de fantasma" },
        { kPlayingOffset, kPlayingBytes, sizeof(kPlayingBytes), "estado 7" },
        { kFlavourOffset, kFlavourBytes, sizeof(kFlavourBytes), "sabor" },
        { kPrepareOffset, kPrepareBytes, sizeof(kPrepareBytes), "preparo" },
    };
    for (const auto& Check : Checks)
    {
        if (!BytesMatch(s_base + Check.Offset, Check.Bytes, Check.Length))
        {
            Error("[DS2_RespawnInSessionHook] %s em +0x%zx nao e o esperado; recusando",
                Check.Name, Check.Offset);
            return false;
        }
    }

    s_log_path = injector.GetDllPath() / "DS2_Respawn.log";
    s_request_path = injector.GetDllPath() / "DS2_Respawn.req";

    s_original_death = (PhantomDeath_p)(s_base + kPhantomDeathOffset);
    s_original_playing = (Playing_p)(s_base + kPlayingOffset);
    s_flavour = (Flavour_p)(s_base + kFlavourOffset);
    s_prepare = (Prepare_p)(s_base + kPrepareOffset);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_death, PhantomDeathHook);
    DetourAttach(&(PVOID&)s_original_playing, PlayingHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        Error("[DS2_RespawnInSessionHook] nao consegui instalar os detours");
        return false;
    }

    s_running.store(true);
    s_thread = std::thread(Run);

    Append("=== ds2os renascer na sessao ===\n");
    Log("[DS2_RespawnInSessionHook] pronto; escreva 1 em DS2_Respawn.req para ligar");
#endif
    return true;
}

void DS2_RespawnInSessionHook::Uninstall()
{
#ifdef _WIN32
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }

    if (s_original_death != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_death, PhantomDeathHook);
        DetourDetach(&(PVOID&)s_original_playing, PlayingHook);
        DetourTransactionCommit();
        s_original_death = nullptr;
    }
#endif
}

const char* DS2_RespawnInSessionHook::GetName()
{
    return "DS2 Respawn In Session";
}
