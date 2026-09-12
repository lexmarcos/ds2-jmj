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

    // "Send the player to where they last rested." Builds its own warp request
    // out of the respawn record and calls the warp itself. The seventh attempt
    // proved this call is the piece nobody else will make: a phantom's death
    // does not stand him up, the session teardown does, and the teardown is
    // exactly what M1 refuses. So the standing up has to be ours.
    constexpr size_t kRespawnOffset = 0x44fde0;
    constexpr uint8_t kRespawnBytes[] = { 0x48, 0x83, 0xec, 0x68, 0x48, 0x8d, 0x54, 0x24, 0x20 };
    constexpr size_t kRespawnRecordOffset = 0x70;

    // Frames to let the death finish before standing him up. The death
    // terminal has bookkeeping of its own to do - the bloodstain, the souls,
    // the hollowing - and reviving him in the middle of it would race.
    constexpr int kReviveDelayFrames = 90;

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
    constexpr size_t kSessionEnd = 0x1cc;

    // The arrival handler's very first test: `*(char*)(*(session+0x108)+8)`.
    // Nonzero and it does not arrive at all - it goes straight to
    // EndSession(0x13), which costs a staging and says nothing.
    //
    // Measured 12/09: **0 on a real join, 1 at the instant the guest dies**.
    // So the replay cannot simply be issued; it has to wait for whatever this
    // is to pass. Whether it ever passes while the player is dead is the one
    // thing left to find out, and waiting is how to ask.
    constexpr size_t kArriveGuardOwner = 0x108;
    constexpr size_t kArriveGuardByte = 8;

    // The arrival handler re-snapshots the "where he came from" block out of
    // the live player and the live counters: +0x1a0 through +0x1c8, and one of
    // those is `ctx+0xd0 +0x168`, the gate, which is 0 at a guest's death and
    // was certainly positive when he was first summoned. A replay would
    // overwrite the real join's record with what is true now, and whatever
    // eventually sends him home reads that record. It is saved and put back;
    // the replay's own warp is built from the payload, not from this block, so
    // restoring it immediately changes nothing the arrival needed.
    constexpr size_t kReturnBlock = 0x1a0;
    constexpr size_t kReturnBlockSize = 0x28;
    constexpr size_t kStatePlaying = 7;

    // Why the session layer is being asked to act. Measured: 2 is "the guest
    // died". The host dying reaches this same terminal on the guest's client
    // with a different one, and there is nothing to save in that case - the
    // session ends either way, and swallowing the goodbye only leaves the host
    // hanging on a farewell that never comes. It did, for good.
    constexpr uint32_t kReasonGuestDied = 2;

    // The state the join starts from. `FUN_1402c4450` is state 1's handler and
    // it is the only place state 2 is ever written: it waits for the peer link
    // (`session+0x100`, field +0xa4, reaching 4), builds a block out of
    // session+0xd8..+0xf0, and only then moves on. So joining a host's world is
    // a peer handshake, not a warp - which is why re-issuing the warp put the
    // guest in the right place of the wrong world.
    constexpr uint32_t kStateJoining = 1;
    // Where the join waits for its destination. Measured: writing state 1 on a
    // guest's death makes the machine advance to 2 by itself - the peer link
    // is still up, so state 1's handler does its work and moves on - and then
    // it sits there, because state 2 only runs when a message arrives with
    // somewhere to put the player. Its guard is `if (state == 2)`, so once the
    // machine is parked there the handler can be called directly.
    constexpr uint32_t kStateArriving = 2;

    // State 2's handler, `FUN_1402c2a80`. Decompiled, and it settles what the
    // last four attempts were guessing at:
    //
    //   * it builds the very warp this hook was building by hand - kind 0,
    //     motive 4, the map, the flavour byte from FUN_1402d4830, a position,
    //     1.0f, a quaternion - and calls it through ctx slot +0x40 with the
    //     flag set to one. So the warp was never the missing half.
    //   * around that warp it runs FUN_1402bbf20, then FUN_140500fd0, then
    //     sends the peer a message and moves to state 3. *That* is the half
    //     that decides whose world the player lands in.
    //   * `session+0x1a4`, which attempt four fed to the warp as a
    //     destination, is written *by this handler* from the player object -
    //     it is a record of where the guest stood before he was summoned.
    //     Warping there put him back in his own world, exactly as measured.
    //
    // Everything it needs that the session does not already hold arrives in
    // param_2, from the host, over the network. So it is cached on the way
    // past and replayed.
    constexpr size_t kArriveOffset = 0x2c2a80;
    constexpr uint8_t kArriveBytes[] = { 0x40, 0x55, 0x41, 0x54, 0x41, 0x56, 0x48, 0x8d, 0x6c, 0x24, 0xe0 };

    // The per-frame dispatcher. State 2 is not in its switch - it only runs
    // when the message arrives - so the replay is issued from here, on the
    // game's own thread, the frame after the machine parks.
    constexpr size_t kDispatchOffset = 0x2c3630;
    constexpr uint8_t kDispatchBytes[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30 };

    // The payload, as read by the handler. Nine dwords are touched; the whole
    // window is kept because a field nobody reads today is still a field.
    //
    //   [0] map          -> also copied to session+0x19c
    //   [1] [2] [3]      -> the destination handed to the warp
    //   [4]              -> never read
    //   [5] yaw          -> cos/sin, becomes the facing quaternion
    //   [6] (short)      -> FUN_14051c6a0
    //   [7]              -> also copied to session+0x198
    //   [8] (byte)       -> also copied to session+0x1c9
    constexpr size_t kPayloadWords = 16;

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

    using Respawn_p = void(*)(void* Record);
    Respawn_p s_respawn = nullptr;

    using Dispatch_p = void(*)(void* Session, float Delta);
    Dispatch_p s_original_dispatch = nullptr;

    using Arrive_p = void(*)(void* Session, uint32_t* Payload);
    Arrive_p s_original_arrive = nullptr;

    // The last payload the host actually sent, kept from the join. Replaying
    // it lands the guest back on the spot he was summoned to - somewhere the
    // host walked to on purpose, which is a far better guarantee than any
    // position this hook could synthesise. Where he died is not: he may well
    // have died in the water.
    uint32_t s_payload[kPayloadWords] = {};
    std::atomic<bool> s_payload_valid{ false };
    std::atomic<bool> s_pending_arrive{ false };
    // After the replay the machine goes to state 3, and 3 is not in the
    // dispatcher's switch either - the arrival sends the host a message and
    // waits for the answer. Whether a host answers a guest it already counts
    // as joined is the one thing left to measure, so the state is followed for
    // a few seconds afterwards instead of guessing.
    std::atomic<int> s_trace{ 0 };
    uint32_t s_trace_last = 0xffffffff;
    // How long to hold the replay waiting for the guard to clear, in frames,
    // and what the guard last read - so the wait reports the value changing
    // rather than one line per frame.
    constexpr int kGuardWaitFrames = 1800;
    int s_guard_waited = 0;
    uint8_t s_guard_last = 0xff;

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
    // Instead of sending a warp, hand the session back to the state the join
    // starts from and let the game redo it. Everything the handshake needs is
    // still in the session object; nothing here has to be synthesised.
    std::atomic<bool> s_rejoin{ false };
    // The other shape of the same idea. Option 4 swallows the death to keep
    // the guest where he fell; this one lets the death run its normal course -
    // the guest loads back at his own bonfire, alive - and only then pulls him
    // back to the host. The session survives the trip because the teardown
    // request is refused separately (`DS2_Session.req` = block), so nothing
    // has to be rebuilt: state 7 is still there when he stands up.
    //
    // It exists because the arrival's guard reads 1 at the instant of death.
    // If that is "this player is in no state to go anywhere", then waiting for
    // him to be somewhere is the answer, not forcing the arrival.
    std::atomic<bool> s_rearm{ false };
    std::atomic<bool> s_rearm_pending{ false };
    int s_rearm_waited = 0;

    // The eighth attempt, and the one the seventh's failure designs. Same as
    // option 5 - the death runs in full, the teardown is refused, the session
    // stays at 7 - except that this one does not wait for the game to stand
    // the guest up, because the seventh measured that it never will. It calls
    // the respawn itself, and only then waits for the arrival guard to open.
    std::atomic<bool> s_revive{ false };
    std::atomic<bool> s_revive_pending{ false };
    int s_revive_waited = 0;
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

    uint8_t ReadArriveGuard(void* Session)
    {
        const uintptr_t Owner = *(const uintptr_t*)((const uint8_t*)Session + kArriveGuardOwner);
        return Owner == 0 ? 0xff : *(const uint8_t*)(Owner + kArriveGuardByte);
    }

    void ArriveHook(void* Session, uint32_t* Payload)
    {
        if (Payload != nullptr)
        {
            memcpy(s_payload, Payload, sizeof(s_payload));
            s_payload_valid.store(true);
            Append(StringFormat(
                "  chegada: mapa=%08x destino=%.2f,%.2f,%.2f giro=%.3f [4]=%08x [6]=%04x [7]=%08x [8]=%02x\n",
                Payload[0], *(float*)&Payload[1], *(float*)&Payload[2], *(float*)&Payload[3],
                *(float*)&Payload[5], Payload[4], (unsigned)(uint16_t)Payload[6],
                Payload[7], (unsigned)(uint8_t)Payload[8]));
        }

        const uint8_t Before = Session == nullptr ? 0xff : ReadArriveGuard(Session);
        s_original_arrive(Session, Payload);
        const uint8_t After = Session == nullptr ? 0xff : ReadArriveGuard(Session);
        Append(StringFormat("  guarda da chegada: antes=%02x depois=%02x\n",
            (unsigned)Before, (unsigned)After));
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

        if (s_rearm.load() || s_revive.load())
        {
            // Nothing is taken away from the death: it runs exactly as the
            // game wrote it. What follows differs - option 5 waits for the
            // game to stand him up, option 6 knows it will not and does it.
            const bool Revive = s_revive.load();
            Append(StringFormat(
                "  morte de fantasma: motivo=%u papel=%u -> morte normal, e depois %s\n",
                Reason, Role, Revive ? "levantar e puxar de volta" : "puxar de volta"));

            if (Revive)
            {
                s_revive_pending.store(true);
                s_revive_waited = 0;
            }
            else
            {
                s_rearm_pending.store(true);
                s_rearm_waited = 0;
            }

            s_original_death(Record, Reason);
            return;
        }

        if (s_rejoin.load())
        {
            s_guard_waited = 0;
            s_guard_last = 0xff;

            if (!s_payload_valid.load())
            {
                Append("  reentrada pedida mas nenhum convite foi visto; deixando a morte normal seguir\n");
                s_original_death(Record, Reason);
                return;
            }

            // Nothing is emitted: the state machine is put back to where a
            // join begins and asked to do it again. The record is marked done
            // either way, or the death screen never clears.
            *(uint32_t*)((uint8_t*)Session + kSessionState) = kStateJoining;
            *((uint8_t*)Record + 0xce) = 1;
            s_pending_arrive.store(true);
            Append(StringFormat(
                "  morte de fantasma: motivo=%u papel=%u -> reentrando pelo estado %u\n",
                Reason, Role, kStateJoining));
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

    void DispatchHook(void* Session, float Delta)
    {
        // The rejoin parks itself in state 2 and waits for a message that will
        // never come: nobody is going to invite a player who is already here.
        // So the host's own invitation is played again.
        // Standing him up ourselves, a frame or so after the death has had its
        // say. Nothing else is going to: measured 12/09, a guest whose
        // teardown is refused stays dead where he fell for as long as anyone
        // cares to watch.
        if (s_revive_pending.load())
        {
            if (++s_revive_waited >= kReviveDelayFrames)
            {
                s_revive_pending.store(false);

                const uintptr_t Context = *(uintptr_t*)(s_base + kContextOffset);
                void* Record = Context == 0
                    ? nullptr
                    : *(void**)(Context + kRespawnRecordOffset);

                if (Record != nullptr)
                {
                    Append(StringFormat("  levantando na ultima fogueira apos %d quadros\n",
                        s_revive_waited));
                    s_respawn(Record);
                    s_rearm_pending.store(true);
                    s_rearm_waited = 0;
                }
                else
                {
                    Append("  sem registro de renascimento; nao da para levantar\n");
                }
            }
        }

        // Waiting for the guest to be back on his feet in his own world, with
        // the session still standing because the teardown was refused. When
        // the guard opens, the join is started over from the top.
        if (s_rearm_pending.load() && Session != nullptr)
        {
            uint8_t* Bytes = (uint8_t*)Session;
            const uint32_t State = *(uint32_t*)(Bytes + kSessionState);
            const uint8_t Guard = ReadArriveGuard(Session);
            ++s_rearm_waited;

            if (State == kStatePlaying && Guard == 0 && s_payload_valid.load())
            {
                s_rearm_pending.store(false);
                Append(StringFormat(
                    "  de pe de novo apos %d quadros, guarda=%02x; recomecando o join\n",
                    s_rearm_waited, (unsigned)Guard));
                *(uint32_t*)(Bytes + kSessionState) = kStateJoining;
                s_guard_waited = 0;
                s_guard_last = 0xff;
                s_pending_arrive.store(true);
            }
            else if (s_rearm_waited % 600 == 0)
            {
                Append(StringFormat("  esperando: estado=%u guarda=%02x apos %d quadros\n",
                    State, (unsigned)Guard, s_rearm_waited));
            }

            if (s_rearm_waited >= 7200)
            {
                s_rearm_pending.store(false);
                Append(StringFormat(
                    "  desisti de puxar de volta: estado=%u guarda=%02x\n",
                    State, (unsigned)Guard));
            }
        }

        if (s_pending_arrive.load() && Session != nullptr)
        {
            uint8_t* Bytes = (uint8_t*)Session;
            const uint32_t State = *(uint32_t*)(Bytes + kSessionState);

            if (State == kStateArriving && s_payload_valid.load())
            {
                const uint8_t Guard = ReadArriveGuard(Session);
                if (Guard != s_guard_last)
                {
                    s_guard_last = Guard;
                    Append(StringFormat("  guarda da chegada = %02x apos %d quadros\n",
                        (unsigned)Guard, s_guard_waited));
                }

                if (Guard != 0)
                {
                    // Not yet. The arrival would go straight to
                    // EndSession(0x13) and take the staging with it, so the
                    // replay waits - and gives up out loud rather than
                    // silently sitting armed.
                    if (++s_guard_waited >= kGuardWaitFrames)
                    {
                        s_pending_arrive.store(false);
                        Append(StringFormat(
                            "  a guarda ficou em %02x por %d quadros; desistindo da repeticao\n",
                            (unsigned)Guard, s_guard_waited));
                    }
                }
                else
                {
                    s_pending_arrive.store(false);
                    uint32_t Payload[kPayloadWords];
                    memcpy(Payload, s_payload, sizeof(Payload));

                    uint8_t Saved[kReturnBlockSize];
                    memcpy(Saved, Bytes + kReturnBlock, sizeof(Saved));

                    Append(StringFormat(
                        "  estado 2; repetindo o convite do host: mapa=%08x destino=%.2f,%.2f,%.2f\n",
                        Payload[0], *(float*)&Payload[1], *(float*)&Payload[2], *(float*)&Payload[3]));

                    s_original_arrive(Session, Payload);

                    // Put the original join's record of "where he came from" back.
                    memcpy(Bytes + kReturnBlock, Saved, sizeof(Saved));

                    Append(StringFormat("  depois da chegada: estado=%u\n",
                        *(uint32_t*)(Bytes + kSessionState)));
                    s_trace.store(900);
                    s_trace_last = 0xffffffff;
                }
            }
            else if (State != kStateJoining && State != kStateArriving)
            {
                // It went somewhere else - 0xb is the handshake giving up.
                // Say so rather than sit armed forever.
                s_pending_arrive.store(false);
                Append(StringFormat("  a reentrada saiu para o estado %u; desarmando\n", State));
            }
        }

        if (s_trace.load() > 0 && Session != nullptr)
        {
            uint8_t* Bytes = (uint8_t*)Session;
            const uint32_t State = *(uint32_t*)(Bytes + kSessionState);
            if (State != s_trace_last)
            {
                s_trace_last = State;
                Append(StringFormat("  estado -> %u (fim=%u)\n", State,
                    *(uint32_t*)(Bytes + kSessionEnd)));
            }
            s_trace.fetch_sub(1);
            if (s_trace.load() == 0)
            {
                Append(StringFormat("  fim do rastro, estado=%u\n", State));
            }
        }

        s_original_dispatch(Session, Delta);
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

                // "0" off, "1" the join's flag, "2" the teardown's flag,
                // "3" the join's flag with the gate held open for the call,
                // "4" no warp at all: the session is put back to the state a
                // join starts from and the host's own invitation is replayed,
                // "5" the death runs normally and the guest is pulled back
                // once he is standing again, "6" the same but standing him up
                // is done here, because the game will not.
                const char Choice = Contents.empty() ? '0' : Contents[0];
                s_enabled.store(Choice != '0');
                s_flag.store(Choice == '2' ? 0 : 1);
                s_lift.store(Choice == '3');
                s_rejoin.store(Choice == '4');
                s_rearm.store(Choice == '5');
                s_revive.store(Choice == '6');
                Append(StringFormat("=== renascer na sessao %s, %s ===\n",
                    Choice == '0' ? "desligado" : "ligado",
                    s_revive.load()
                        ? "morte normal, levantar na fogueira e puxar de volta"
                        : s_rearm.load()
                        ? "morte normal, e puxar de volta quando ele levantar"
                        : s_rejoin.load()
                        ? "reentrando pelo estado 1 e repetindo o convite"
                        : (s_flag.load() ? (s_lift.load() ? "flag 1, portao erguido" : "flag 1")
                                         : "flag 0")));
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
        { kDispatchOffset, kDispatchBytes, sizeof(kDispatchBytes), "despachante" },
        { kArriveOffset, kArriveBytes, sizeof(kArriveBytes), "chegada" },
        { kRespawnOffset, kRespawnBytes, sizeof(kRespawnBytes), "renascimento" },
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
    s_original_dispatch = (Dispatch_p)(s_base + kDispatchOffset);
    s_original_arrive = (Arrive_p)(s_base + kArriveOffset);
    s_respawn = (Respawn_p)(s_base + kRespawnOffset);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_death, PhantomDeathHook);
    DetourAttach(&(PVOID&)s_original_playing, PlayingHook);
    DetourAttach(&(PVOID&)s_original_dispatch, DispatchHook);
    DetourAttach(&(PVOID&)s_original_arrive, ArriveHook);
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
        DetourDetach(&(PVOID&)s_original_dispatch, DispatchHook);
        DetourDetach(&(PVOID&)s_original_arrive, ArriveHook);
        DetourTransactionCommit();
        s_original_death = nullptr;
    }
#endif
}

const char* DS2_RespawnInSessionHook::GetName()
{
    return "DS2 Respawn In Session";
}
