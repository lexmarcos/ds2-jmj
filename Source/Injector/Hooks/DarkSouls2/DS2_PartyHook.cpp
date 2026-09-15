/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_PartyHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include "ThirdParty/detours/src/detours.h"
#endif

namespace
{
#ifdef _WIN32

    // Version 1.03 Calibrations 2.02, every entry point verified before
    // anything is written.
    //
    // The NetSvrSummonSignManager is reached the way the game's own getter
    // does (FUN_1405132a0: *(*0x141616cf8 + 0x30) is the NetSvrManager), then
    // its field +0x78, and it is believed only with its vftable. Measured the
    // same on both instances, 14/09.
    //
    //   +0x2a1410  "place my sign": maps the type through the player's state
    //              (FUN_14029c9b0), then FUN_1402a2780, which checks again,
    //              builds the cell and matching parameters, calls
    //              NetSvrSummonSignInterface::CreateSummonSign and keeps the
    //              sign at manager+0x18 (placed) / +0x24 (handle) / +0x40
    //              (type). A second call replaces the sign, as a second use of
    //              the soapstone does.
    //   +0x2a14c0  "summon this sign": (manager, &handle), what touching the
    //              sign and confirming does.
    //
    // Traced 14/09: a real White Sign Soapstone use reached CreateSummonSign
    // from +0x2a2b98 (inside FUN_1402a2780).
    constexpr size_t kPlaceOffset = 0x2a1410;
    constexpr uint8_t kPlacePrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x20, 0x56, 0x57 };
    constexpr size_t kSummonOffset = 0x2a14c0;
    constexpr uint8_t kSummonPrologue[] = { 0x48, 0x83, 0xec, 0x28, 0x8b, 0x02 };

    // SummonSignSetCtrl's update, called every frame on host and guest alike
    // (a re-armed breakpoint hit it in every half-second window on both). The
    // orders run here, so they run on the game's thread.
    constexpr size_t kTickOffset = 0x2139d0;
    constexpr uint8_t kTickPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };

    // SummonSignSetCtrl's AddSign (FUN_140213160), where a sign from the
    // server enters the cache; the same function DS2_RematchHook watches. It
    // files the entry into the collection at *(this - 8), and
    // FUN_14020e6f0(collection, &handle) finds it again. The entry (0x88
    // bytes) carries the sign id at +0x20, the owner's player id at +0x24, the
    // type at +0x28 and the owner's Steam ID as 16 hex characters at +0x38
    // (read on a red sign, 14/09).
    constexpr size_t kAddSignOffset = 0x213160;
    constexpr uint8_t kAddSignPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c };
    constexpr size_t kFindSignOffset = 0x20e6f0;
    constexpr uint8_t kFindSignPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10 };
    constexpr size_t kEntrySteamId = 0x38;

    // NetSvrSummonSignInterface: the two requests that carry this player's
    // MatchingParameter to the server, the sign's (CreateSummonSign, 4th
    // argument) and the sign poll's (GetSummonSignList, 5th). The client's
    // struct is 16 uint32 read here; which one the protocol's
    // name_engraved_ring is has to be measured (`sonda`).
    constexpr size_t kCreateSignOffset = 0x29dfa0;
    constexpr uint8_t kCreateSignPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18 };
    constexpr size_t kSignListOffset = 0x29e230;
    constexpr uint8_t kSignListPrologue[] = { 0x44, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 };
    constexpr size_t kMatchingWords = 16;

    constexpr size_t kNetSvrGlobal = 0x1616cf8;
    constexpr size_t kNetSvrField = 0x30;
    constexpr size_t kSummonSignField = 0x78;
    constexpr size_t kSummonSignVftable = 0x10d61f8;

    // The local character: *(*0x1416148f0 + 0xd0), its role block at +0xb0,
    // the role byte at +0x3c (0 owner of the world, 1 white phantom).
    constexpr size_t kGameGlobal = 0x16148f0;
    constexpr size_t kLocalCharacter = 0xd0;
    constexpr size_t kRoles = 0xb0;
    constexpr size_t kRole = 0x3c;

    constexpr size_t kPlacedOffset = 0x18;   // byte
    constexpr size_t kHandleOffset = 0x24;   // uint32
    constexpr size_t kTypeOffset = 0x40;     // byte

    constexpr uint8_t kWhiteSign = 1;        // the type the white soapstone places, and arrives as

    using SignMethod_p = void(*)(void* Manager, uint64_t Type);
    using Summon_p = void(*)(void* Manager, uint32_t* Handle);
    using Tick_p = void(*)(void* Self);
    using AddSign_p = uint32_t*(*)(void* Self, uint32_t* OutHandle, uint8_t Type, void* P4,
        uint32_t P5, uint32_t P6, void* P7, void* P8, uint8_t P9, uint32_t P10, void* P11);
    using FindSign_p = int32_t*(*)(void* Collection, int32_t* Handle);
    using CreateSign_p = void*(*)(void* This, uint32_t Area, void* Cell, uint32_t* Matching, int32_t Type, void* AppData, uint32_t* Out);
    using SignList_p = void*(*)(void* This, uint32_t Area, void* Cells, uint32_t Count, uint32_t* Matching, uint8_t A, uint8_t B, void* Out1, void* Out2);
    CreateSign_p s_original_create_sign = nullptr;
    SignList_p s_original_sign_list = nullptr;
    std::atomic<bool> s_probe{ false };
    std::atomic<int> s_probe_index{ 0 };
    std::atomic<uint32_t> s_probe_value{ 0 };
    std::atomic<bool> s_list_logged{ false };

    Tick_p s_original_tick = nullptr;
    AddSign_p s_original_add_sign = nullptr;
    SignMethod_p s_place = nullptr;
    Summon_p s_summon = nullptr;
    FindSign_p s_find_sign = nullptr;
    uintptr_t s_base = 0;

    constexpr int kNothing = -1;
    std::atomic<int> s_pending_place{ kNothing };
    std::atomic<bool> s_pending_status{ false };
    // "pausa" in the request file: no sign placed, none accepted, until
    // "retoma". Without it a session ended on purpose is rejoined within a
    // minute, which is the point for players and the opposite of what a test
    // tearing down wants.
    std::atomic<bool> s_paused{ false };
    std::atomic<bool> s_running{ false };
    std::thread s_thread;

    bool s_guest = false;
    std::vector<uint64_t> s_accept;

    // Guest bookkeeping, game thread only.
    ULONGLONG s_next_check = 0;
    int s_last_role = -1;
    bool s_back_home = false;
    // When the sign last went away while the guest stayed in its own world.
    // Being summoned removes the sign seconds before the guest's role changes,
    // and placing again inside that window would race the join (seen 15/09,
    // harmless that time).
    ULONGLONG s_gone_since = 0;
    constexpr ULONGLONG kJoinGraceMs = 30000;
    std::string s_last_guest_line;

    std::filesystem::path s_log_path;
    std::filesystem::path s_request_path;
    std::mutex s_log_mutex;

    void Append(const std::string& Text)
    {
        std::scoped_lock Lock(s_log_mutex);
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream)
        {
            Stream << Text;
        }
    }

    std::string Clock()
    {
        SYSTEMTIME Now;
        GetLocalTime(&Now);
        return StringFormat("%02u:%02u:%02u.%03u", Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds);
    }

    bool ReadPointer(uintptr_t At, uintptr_t& Out)
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

    bool ReadBytes(uintptr_t At, uint8_t* Out, size_t Length)
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

    void* ResolveManager()
    {
        uintptr_t Global = 0, NetSvr = 0, Manager = 0, Vftable = 0;
        if (!ReadPointer(s_base + kNetSvrGlobal, Global) || Global == 0) { return nullptr; }
        if (!ReadPointer(Global + kNetSvrField, NetSvr) || NetSvr == 0) { return nullptr; }
        if (!ReadPointer(NetSvr + kSummonSignField, Manager) || Manager == 0) { return nullptr; }
        if (!ReadPointer(Manager, Vftable) || Vftable != s_base + kSummonSignVftable) { return nullptr; }
        return (void*)Manager;
    }

    // -1 when there is no local character (title, loading).
    int LocalRole()
    {
        uintptr_t Context = 0, Character = 0, Roles = 0;
        uint8_t Role = 0;
        if (!ReadPointer(s_base + kGameGlobal, Context) || Context == 0) { return -1; }
        if (!ReadPointer(Context + kLocalCharacter, Character) || Character == 0) { return -1; }
        if (!ReadPointer(Character + kRoles, Roles) || Roles == 0) { return -1; }
        if (!ReadBytes(Roles + kRole, &Role, 1)) { return -1; }
        return Role;
    }

    struct SignState
    {
        bool Placed = false;
        uint32_t Handle = 0;
        uint8_t Type = 0;
    };

    SignState ReadSign(void* Manager)
    {
        SignState State;
        uint8_t Bytes[0x48] = {};
        if (ReadBytes((uintptr_t)Manager, Bytes, sizeof(Bytes)))
        {
            State.Placed = Bytes[kPlacedOffset] != 0;
            memcpy(&State.Handle, Bytes + kHandleOffset, sizeof(State.Handle));
            State.Type = Bytes[kTypeOffset];
        }
        return State;
    }

    std::string Describe(const SignState& State)
    {
        return StringFormat("placa %s, alca %08x, tipo %u", State.Placed ? "posta" : "nenhuma", State.Handle, (unsigned)State.Type);
    }

    void PlaceSign(void* Manager, uint8_t Type, const char* Why)
    {
        const SignState Before = ReadSign(Manager);
        s_place(Manager, Type);
        const SignState After = ReadSign(Manager);
        Append(StringFormat("%s  %s: placa tipo %u; antes: %s; depois: %s\n",
            Clock().c_str(), Why, (unsigned)Type, Describe(Before).c_str(), Describe(After).c_str()));
    }

    // The guest keeps its sign down while it is in its own world. Once a
    // second is plenty: the host only polls signs once a minute.
    void GuestTick(void* Manager)
    {
        const ULONGLONG Now = GetTickCount64();
        if (Now < s_next_check)
        {
            return;
        }
        s_next_check = Now + 1000;

        const int Role = LocalRole();
        if (s_last_role == 1 && Role == 0)
        {
            // Back from the host's world: whatever the manager still says, the
            // sign that was summoned is gone.
            s_back_home = true;
        }
        if (Role != -1)
        {
            s_last_role = Role;
        }

        std::string Line;
        const SignState State = ReadSign(Manager);
        const bool Missing = !State.Placed || State.Type != kWhiteSign;
        if (!Missing)
        {
            s_gone_since = 0;
        }
        if (s_paused.load())
        {
            Line = "pausado";
        }
        else if (Role != 0)
        {
            Line = StringFormat("fora do proprio mundo (papel %d): nenhuma placa", Role);
        }
        else if (Missing && !s_back_home && s_last_guest_line == "placa no chao" && s_gone_since == 0)
        {
            s_gone_since = Now;
            Line = "a placa sumiu; espero 30 s antes de repor (pode ser a invocacao)";
        }
        else if (Missing && !s_back_home && s_gone_since != 0 && Now - s_gone_since < kJoinGraceMs)
        {
            Line = "a placa sumiu; espero 30 s antes de repor (pode ser a invocacao)";
        }
        else if (Missing || s_back_home)
        {
            s_gone_since = 0;
            s_back_home = false;
            PlaceSign(Manager, kWhiteSign, State.Placed ? "convidado de volta: repondo" : "convidado");
            const SignState After = ReadSign(Manager);
            Line = After.Placed ? "placa no chao" : "a placa nao ficou; tento de novo em 10 s";
            if (!After.Placed)
            {
                s_next_check = Now + 10000;
            }
        }
        else
        {
            Line = "placa no chao";
        }
        if (Line != s_last_guest_line)
        {
            s_last_guest_line = Line;
            Append(StringFormat("%s  convidado: %s\n", Clock().c_str(), Line.c_str()));
        }
    }

    // Runs on the game's thread, every frame.
    void TickHook(void* Self)
    {
        s_original_tick(Self);

        const bool Orders = s_pending_place.load() != kNothing || s_pending_status.load();
        if (!Orders && !s_guest)
        {
            return;
        }
        void* Manager = ResolveManager();
        if (Manager == nullptr)
        {
            // Orders stay pending: the manager exists once the player is online.
            return;
        }

        const int Wanted = s_pending_place.exchange(kNothing);
        if (Wanted != kNothing)
        {
            PlaceSign(Manager, (uint8_t)Wanted, "pedido");
        }
        if (s_pending_status.exchange(false))
        {
            Append(StringFormat("%s  === status: manager %p, %s, papel %d, convidado %u, aceita %zu steam id(s), pausado %u ===\n",
                Clock().c_str(), Manager, Describe(ReadSign(Manager)).c_str(), LocalRole(), (unsigned)s_guest, s_accept.size(),
                (unsigned)s_paused.load()));
        }
        if (s_guest)
        {
            GuestTick(Manager);
        }
    }

    // The owner's Steam ID of a sign already in the cache, or 0.
    uint64_t SignOwnerSteamId(void* Self, uint32_t Handle)
    {
        uintptr_t Collection = 0;
        if (!ReadPointer((uintptr_t)Self - 8, Collection) || Collection == 0)
        {
            return 0;
        }
        int32_t Key = (int32_t)Handle;
        int32_t* Entry = s_find_sign((void*)Collection, &Key);
        if (Entry == nullptr)
        {
            return 0;
        }
        char Hex[17] = {};
        if (!ReadBytes((uintptr_t)Entry + kEntrySteamId, (uint8_t*)Hex, 16))
        {
            return 0;
        }
        return strtoull(Hex, nullptr, 16);
    }

    uint32_t* AddSignHook(void* Self, uint32_t* OutHandle, uint8_t Type, void* P4,
        uint32_t P5, uint32_t P6, void* P7, void* P8, uint8_t P9, uint32_t P10, void* P11)
    {
        uint32_t* Result = s_original_add_sign(Self, OutHandle, Type, P4, P5, P6, P7, P8, P9, P10, P11);
        if (s_accept.empty() || OutHandle == nullptr || *OutHandle == 0 || Type != kWhiteSign)
        {
            return Result;
        }
        if (s_paused.load())
        {
            Append(StringFormat("%s  host: placa %08x chegou com o party pausado; ignorada\n", Clock().c_str(), *OutHandle));
            return Result;
        }

        const uint64_t Owner = SignOwnerSteamId(Self, *OutHandle);
        bool Accepted = false;
        for (uint64_t Id : s_accept)
        {
            Accepted = Accepted || (Owner != 0 && Id == Owner);
        }
        if (!Accepted)
        {
            Append(StringFormat("%s  host: placa %08x do jogador %u (steam %llu) nao esta na lista; ignorada\n",
                Clock().c_str(), *OutHandle, P6, (unsigned long long)Owner));
            return Result;
        }
        const int Role = LocalRole();
        void* Manager = ResolveManager();
        if (Role != 0 || Manager == nullptr)
        {
            Append(StringFormat("%s  host: placa %08x do parceiro %llu, mas o host nao esta no proprio mundo (papel %d) ou sem manager; ignorada\n",
                Clock().c_str(), *OutHandle, (unsigned long long)Owner, Role));
            return Result;
        }
        uint32_t Handle = *OutHandle;
        Append(StringFormat("%s  host: invocando a placa %08x do parceiro %llu (jogador %u)\n",
            Clock().c_str(), Handle, (unsigned long long)Owner, P6));
        s_summon(Manager, &Handle);
        return Result;
    }

    std::string Words(const uint32_t* Matching)
    {
        std::string Text;
        uint32_t Copy[kMatchingWords] = {};
        if (!ReadBytes((uintptr_t)Matching, (uint8_t*)Copy, sizeof(Copy)))
        {
            return "ilegivel";
        }
        for (size_t i = 0; i < kMatchingWords; i++)
        {
            Text += StringFormat(" [%zu]=%u", i, Copy[i]);
        }
        return Text;
    }

    void* CreateSignHook(void* This, uint32_t Area, void* Cell, uint32_t* Matching, int32_t Type, void* AppData, uint32_t* Out)
    {
        if (Matching != nullptr && s_probe.exchange(false))
        {
            // One word at a time, with a small value: four large ones at once
            // kept the sign from ever leaving the client (15/09).
            const int Index = s_probe_index.load();
            Matching[Index] = s_probe_value.load();
            Append(StringFormat("%s  sonda: [%d]=%u nesta placa\n", Clock().c_str(), Index, s_probe_value.load()));
        }
        if (Matching != nullptr)
        {
            Append(StringFormat("%s  CreateSummonSign area %08x tipo %d matching:%s\n", Clock().c_str(), Area, Type, Words(Matching).c_str()));
        }
        return s_original_create_sign(This, Area, Cell, Matching, Type, AppData, Out);
    }

    void* SignListHook(void* This, uint32_t Area, void* Cells, uint32_t Count, uint32_t* Matching, uint8_t A, uint8_t B, void* Out1, void* Out2)
    {
        if (Matching != nullptr && !s_list_logged.exchange(true))
        {
            Append(StringFormat("%s  GetSummonSignList area %08x matching:%s\n", Clock().c_str(), Area, Words(Matching).c_str()));
        }
        return s_original_sign_list(This, Area, Cells, Count, Matching, A, B, Out1, Out2);
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
                std::ifstream Stream(s_request_path);
                std::string Line;
                while (std::getline(Stream, Line))
                {
                    int Type = 0;
                    int ProbeIndex = 0;
                    unsigned ProbeValue = 0;
                    if (sscanf(Line.c_str(), "placa %d", &Type) == 1 && Type >= 0 && Type < 0x14)
                    {
                        s_pending_place.store(Type);
                        Append(StringFormat("%s  === pedido de placa tipo %d, no proximo quadro ===\n", Clock().c_str(), Type));
                    }
                    else if (sscanf(Line.c_str(), "sonda %d %u", &ProbeIndex, &ProbeValue) == 2 && ProbeIndex >= 0 && ProbeIndex < (int)kMatchingWords)
                    {
                        s_probe_index.store(ProbeIndex);
                        s_probe_value.store(ProbeValue);
                        s_probe.store(true);
                        s_list_logged.store(false);
                        Append(StringFormat("%s  === sonda armada para a proxima placa ===\n", Clock().c_str()));
                    }
                    else if (Line.rfind("pausa", 0) == 0 || Line.rfind("retoma", 0) == 0)
                    {
                        const bool Pause = Line.rfind("pausa", 0) == 0;
                        s_paused.store(Pause);
                        Append(StringFormat("%s  === party %s ===\n", Clock().c_str(), Pause ? "pausado" : "retomado"));
                    }
                    else if (Line.rfind("status", 0) == 0)
                    {
                        s_pending_status.store(true);
                        if (ResolveManager() == nullptr)
                        {
                            Append(StringFormat("%s  === status: manager nao resolvido ainda (fica pendente) ===\n", Clock().c_str()));
                        }
                    }
                    else if (!Line.empty())
                    {
                        Append(StringFormat("%s  === pedido desconhecido: %s ===\n", Clock().c_str(), Line.c_str()));
                    }
                }
                Stream.close();
                std::filesystem::remove(s_request_path, Error);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    std::vector<uint64_t> ParseSteamIds(const std::string& Text)
    {
        std::vector<uint64_t> Ids;
        std::string Token;
        for (char C : Text + ",")
        {
            if (C >= '0' && C <= '9')
            {
                Token.push_back(C);
            }
            else if (!Token.empty())
            {
                Ids.push_back(strtoull(Token.c_str(), nullptr, 10));
                Token.clear();
            }
        }
        return Ids;
    }

#endif
}

void* DS2_PartyHook_SignManager()
{
#ifdef _WIN32
    return s_base == 0 ? nullptr : ResolveManager();
#else
    return nullptr;
#endif
}

bool DS2_PartyHook::Install(Injector& injector)
{
#ifdef _WIN32
    const uintptr_t Base = (uintptr_t)injector.GetBaseAddress();
    const uintptr_t Tick = Base + kTickOffset;
    const uintptr_t Place = Base + kPlaceOffset;
    const uintptr_t Summon = Base + kSummonOffset;
    const uintptr_t AddSign = Base + kAddSignOffset;
    const uintptr_t FindSign = Base + kFindSignOffset;
    const uintptr_t CreateSign = Base + kCreateSignOffset;
    const uintptr_t SignList = Base + kSignListOffset;

    if (!BytesMatch(Tick, kTickPrologue, sizeof(kTickPrologue)) ||
        !BytesMatch(Place, kPlacePrologue, sizeof(kPlacePrologue)) ||
        !BytesMatch(Summon, kSummonPrologue, sizeof(kSummonPrologue)) ||
        !BytesMatch(AddSign, kAddSignPrologue, sizeof(kAddSignPrologue)) ||
        !BytesMatch(FindSign, kFindSignPrologue, sizeof(kFindSignPrologue)) ||
        !BytesMatch(CreateSign, kCreateSignPrologue, sizeof(kCreateSignPrologue)) ||
        !BytesMatch(SignList, kSignListPrologue, sizeof(kSignListPrologue)))
    {
        Error("[DS2_PartyHook] o codigo de uma das sete entradas nao e o esperado; recusando");
        return false;
    }
    s_base = Base;

    const RuntimeConfig& Config = injector.GetConfig();
    s_guest = Config.DS2PartyGuest;
    s_accept = ParseSteamIds(Config.DS2PartyAccept);

    s_log_path = injector.GetDllPath() / "DS2_Party.log";
    s_request_path = injector.GetDllPath() / "DS2_Party.req";

    s_original_tick = (Tick_p)Tick;
    s_original_add_sign = (AddSign_p)AddSign;
    s_place = (SignMethod_p)Place;
    s_summon = (Summon_p)Summon;
    s_find_sign = (FindSign_p)FindSign;
    s_original_create_sign = (CreateSign_p)CreateSign;
    s_original_sign_list = (SignList_p)SignList;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_tick, TickHook);
    DetourAttach(&(PVOID&)s_original_add_sign, AddSignHook);
    DetourAttach(&(PVOID&)s_original_create_sign, CreateSignHook);
    DetourAttach(&(PVOID&)s_original_sign_list, SignListHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        Error("[DS2_PartyHook] nao consegui instalar os detours");
        return false;
    }

    s_running.store(true);
    s_thread = std::thread(Run);

    std::string Accepted;
    for (uint64_t Id : s_accept)
    {
        Accepted += StringFormat(" %llu", (unsigned long long)Id);
    }
    Append(StringFormat("%s  === ds2os party: convidado %s, aceita:%s ===\n", Clock().c_str(),
        s_guest ? "sim" : "nao", Accepted.empty() ? " ninguem" : Accepted.c_str()));
    Log("[DS2_PartyHook] pronto; convidado %s, %zu steam id(s) aceitos", s_guest ? "sim" : "nao", s_accept.size());
#endif
    return true;
}

void DS2_PartyHook::Uninstall()
{
#ifdef _WIN32
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }
    if (s_original_tick != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_tick, TickHook);
        DetourDetach(&(PVOID&)s_original_add_sign, AddSignHook);
        DetourDetach(&(PVOID&)s_original_create_sign, CreateSignHook);
        DetourDetach(&(PVOID&)s_original_sign_list, SignListHook);
        DetourTransactionCommit();
        s_original_tick = nullptr;
        s_original_add_sign = nullptr;
    }
#endif
}

const char* DS2_PartyHook::GetName()
{
    return "DS2 Party";
}
