/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_ProgressCarryHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
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

    // Version 1.03 Calibrations 2.02 (docs/DS2_WORLD_STATE.md).
    //
    //   +0x25ce10  NetP2pPacketEventFlag's receive (slot +0x20): 8 bytes, the
    //              flag as uint32 and the value as a byte; refuses a global
    //              flag unless the sender is the host, then FUN_1404750b0.
    //   +0x474a60  the game's flag setter (mgr, flag, value): in a session a
    //              guest may only set map flags, and a change is sent to the
    //              session by FUN_14051e6b0.
    constexpr size_t kReceiveOffset = 0x25ce10;
    constexpr uint8_t kReceivePrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x70, 0x33, 0xc0, 0x41, 0x8b, 0xc8 };
    constexpr size_t kSetterOffset = 0x474a60;
    constexpr uint8_t kSetterPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20 };

    // *(*(*0x1416148f0 + 0x70) + 0x20) is the EventFlagManager, believed only
    // with its vftable. At +0x20 a 31-bucket table keyed by flag / 10000; a
    // node holds the bytes at +0x00, their length at +0x08, the category at
    // +0x0c and the next node at +0x10. +0x118 is nonzero while the manager
    // holds another player's world (FUN_1404744b0).
    constexpr size_t kGameGlobal = 0x16148f0;
    constexpr size_t kEventManager = 0x70;
    constexpr size_t kFlagManager = 0x20;
    constexpr size_t kFlagManagerVftable = 0x10eff58;
    constexpr size_t kBuckets = 0x20;
    constexpr size_t kOtherWorld = 0x118;

    constexpr size_t kLocalCharacter = 0xd0;
    constexpr size_t kRoles = 0xb0;
    constexpr size_t kRole = 0x3c;

    // A guest back in its own world applies once this long has passed, so a
    // load still settling is not written into.
    constexpr ULONGLONG kHomeSettleMs = 5000;

    using Receive_p = void(*)(void* This, uint64_t* Payload, uint64_t Length, void* Sender);
    using Setter_p = void(*)(void* Manager, uint32_t Flag, uint8_t Value);
    Receive_p s_original_receive = nullptr;
    Setter_p s_setter = nullptr;
    uintptr_t s_base = 0;
    std::atomic<bool> s_installed{ false };

    std::filesystem::path s_log_path;
    std::filesystem::path s_request_path;
    std::filesystem::path s_pending_path;
    std::mutex s_log_mutex;

    // Game thread only, except the orders queue.
    std::map<uint32_t, uint8_t> s_kept;
    ULONGLONG s_home_since = 0;
    ULONGLONG s_next_check = 0;
    uint64_t s_received = 0;

    struct Order
    {
        enum Kind { Set, Read, Status, Forget } What;
        uint32_t Flag = 0;
        uint8_t Value = 0;
    };
    std::mutex s_orders_mutex;
    std::vector<Order> s_orders;
    std::atomic<bool> s_running{ false };
    std::thread s_thread;

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

    bool BytesMatch(uintptr_t Address, const uint8_t* Expected, size_t Length)
    {
        return memcmp((const void*)Address, Expected, Length) == 0;
    }

    // FUN_14025cdb0 answers 0 for these: the flags a guest cannot set and a
    // client only takes from the host.
    bool IsGlobal(uint32_t Flag)
    {
        const uint32_t Millions = Flag / 1000000;
        return Millions % 1000 == 0 && (Flag / 10000) % 100 > 2;
    }

    uintptr_t FlagManager()
    {
        uintptr_t Context = 0, Events = 0, Manager = 0, Vftable = 0;
        if (!ReadPointer(s_base + kGameGlobal, Context) || Context == 0) { return 0; }
        if (!ReadPointer(Context + kEventManager, Events) || Events == 0) { return 0; }
        if (!ReadPointer(Events + kFlagManager, Manager) || Manager == 0) { return 0; }
        if (!ReadPointer(Manager, Vftable) || Vftable != s_base + kFlagManagerVftable) { return 0; }
        return Manager;
    }

    bool HoldsOtherWorld(uintptr_t Manager)
    {
        uint8_t Bytes[4] = {};
        return ReadBytes(Manager + kOtherWorld, Bytes, sizeof(Bytes)) && (Bytes[0] | Bytes[1] | Bytes[2] | Bytes[3]) != 0;
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

    // 1 or 0, or -1 when the flag's category is not loaded.
    int ReadFlag(uintptr_t Manager, uint32_t Flag)
    {
        const uint32_t Category = Flag / 10000;
        const uint32_t Bit = Flag % 10000;
        uintptr_t Node = 0;
        if (!ReadPointer(Manager + kBuckets + (size_t)((Category * 0x89u) % 0x1f) * 8, Node))
        {
            return -1;
        }
        for (int Guard = 0; Node != 0 && Guard < 64; ++Guard)
        {
            uint8_t Header[0x18] = {};
            if (!ReadBytes(Node, Header, sizeof(Header)))
            {
                return -1;
            }
            uintptr_t Data = 0, Next = 0;
            uint32_t Length = 0, NodeCategory = 0;
            memcpy(&Data, Header, 8);
            memcpy(&Length, Header + 8, 4);
            memcpy(&NodeCategory, Header + 0xc, 4);
            memcpy(&Next, Header + 0x10, 8);
            if (NodeCategory == Category)
            {
                uint8_t Byte = 0;
                if ((Bit >> 3) >= Length || !ReadBytes(Data + (Bit >> 3), &Byte, 1))
                {
                    return -1;
                }
                return (Byte & (0x80 >> (Bit & 7))) != 0 ? 1 : 0;
            }
            Node = Next;
        }
        return -1;
    }

    void SaveKept()
    {
        std::ofstream Stream(s_pending_path, std::ios::trunc);
        for (const auto& [Flag, Value] : s_kept)
        {
            Stream << Flag << ' ' << (unsigned)Value << '\n';
        }
    }

    void LoadKept()
    {
        std::ifstream Stream(s_pending_path);
        uint32_t Flag = 0;
        unsigned Value = 0;
        while (Stream >> Flag >> Value)
        {
            s_kept[Flag] = (uint8_t)(Value != 0);
        }
    }

    void ReceiveHook(void* This, uint64_t* Payload, uint64_t Length, void* Sender)
    {
        uint8_t Packet[8] = {};
        const bool Have = (uint32_t)Length == 8 && ReadBytes((uintptr_t)Payload, Packet, sizeof(Packet));
        s_original_receive(This, Payload, Length, Sender);
        if (!Have)
        {
            return;
        }
        uint32_t Flag = 0;
        memcpy(&Flag, Packet, 4);
        const uint8_t Value = Packet[4] != 0 ? 1 : 0;
        ++s_received;

        const uintptr_t Manager = FlagManager();
        const int Role = LocalRole();
        const bool Guest = Role > 0 && Manager != 0 && HoldsOtherWorld(Manager);
        const int Now = Manager != 0 ? ReadFlag(Manager, Flag) : -1;
        std::string Fate;
        if (!Guest)
        {
            Fate = "nao sou convidado; nada a guardar";
        }
        else if (!IsGlobal(Flag))
        {
            Fate = "flag de mapa: fica no mundo do host";
        }
        else if (Now != Value)
        {
            Fate = StringFormat("o jogo nao aplicou (le %d); nao guardo", Now);
        }
        else
        {
            s_kept[Flag] = Value;
            SaveKept();
            Fate = StringFormat("guardada para o meu mundo (%zu guardadas)", s_kept.size());
        }
        Append(StringFormat("%s  recebida flag %u = %u (papel %d): %s\n", Clock().c_str(), Flag, (unsigned)Value, Role, Fate.c_str()));
    }

    void RunOrders(uintptr_t Manager)
    {
        std::vector<Order> Orders;
        {
            std::scoped_lock Lock(s_orders_mutex);
            Orders.swap(s_orders);
        }
        for (const Order& Next : Orders)
        {
            switch (Next.What)
            {
            case Order::Set:
            {
                const int Before = ReadFlag(Manager, Next.Flag);
                s_setter((void*)Manager, Next.Flag, Next.Value);
                const int After = ReadFlag(Manager, Next.Flag);
                Append(StringFormat("%s  flag %u <- %u pelo setter do jogo: antes %d, depois %d (papel %d)\n",
                    Clock().c_str(), Next.Flag, (unsigned)Next.Value, Before, After, LocalRole()));
                break;
            }
            case Order::Read:
                Append(StringFormat("%s  flag %u = %d (papel %d, mundo de outro %u)\n", Clock().c_str(), Next.Flag,
                    ReadFlag(Manager, Next.Flag), LocalRole(), (unsigned)HoldsOtherWorld(Manager)));
                break;
            case Order::Status:
            {
                std::string Kept;
                for (const auto& [Flag, Value] : s_kept)
                {
                    Kept += StringFormat(" %u=%u", Flag, (unsigned)Value);
                }
                Append(StringFormat("%s  === status: papel %d, mundo de outro %u, recebidas %llu, guardadas:%s ===\n",
                    Clock().c_str(), LocalRole(), (unsigned)HoldsOtherWorld(Manager), (unsigned long long)s_received,
                    Kept.empty() ? " nenhuma" : Kept.c_str()));
                break;
            }
            case Order::Forget:
                s_kept.clear();
                SaveKept();
                Append(StringFormat("%s  === guardadas esquecidas ===\n", Clock().c_str()));
                break;
            }
        }
    }

    // Once in its own world and settled, the guest sets what it kept.
    void ApplyKept(uintptr_t Manager, ULONGLONG Now)
    {
        if (s_kept.empty())
        {
            s_home_since = 0;
            return;
        }
        if (LocalRole() != 0 || HoldsOtherWorld(Manager))
        {
            s_home_since = 0;
            return;
        }
        if (s_home_since == 0)
        {
            s_home_since = Now;
        }
        if (Now - s_home_since < kHomeSettleMs)
        {
            return;
        }
        bool Changed = false;
        for (auto It = s_kept.begin(); It != s_kept.end();)
        {
            const uint32_t Flag = It->first;
            const uint8_t Value = It->second;
            const int Before = ReadFlag(Manager, Flag);
            if (Before == -1)
            {
                ++It;   // its category is not loaded here
                continue;
            }
            if (Before != Value)
            {
                s_setter((void*)Manager, Flag, Value);
            }
            const int After = ReadFlag(Manager, Flag);
            Append(StringFormat("%s  no meu mundo: flag %u <- %u (antes %d, depois %d)%s\n", Clock().c_str(), Flag,
                (unsigned)Value, Before, After, After == Value ? "" : " NAO APLICOU; guardo de novo"));
            if (After == Value)
            {
                It = s_kept.erase(It);
                Changed = true;
            }
            else
            {
                ++It;
            }
        }
        if (Changed)
        {
            SaveKept();
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
                    unsigned Flag = 0, Value = 0;
                    Order Next{ Order::Status };
                    bool Known = true;
                    if (sscanf(Line.c_str(), "flag %u %u", &Flag, &Value) == 2 && Value <= 1)
                    {
                        Next = { Order::Set, Flag, (uint8_t)Value };
                    }
                    else if (sscanf(Line.c_str(), "le %u", &Flag) == 1)
                    {
                        Next = { Order::Read, Flag, 0 };
                    }
                    else if (Line.rfind("status", 0) == 0)
                    {
                        Next = { Order::Status };
                    }
                    else if (Line.rfind("limpa", 0) == 0)
                    {
                        Next = { Order::Forget };
                    }
                    else
                    {
                        Known = false;
                        if (!Line.empty())
                        {
                            Append(StringFormat("%s  === pedido desconhecido: %s ===\n", Clock().c_str(), Line.c_str()));
                        }
                    }
                    if (Known)
                    {
                        std::scoped_lock Lock(s_orders_mutex);
                        s_orders.push_back(Next);
                    }
                }
                Stream.close();
                std::filesystem::remove(s_request_path, Error);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

#endif
}

void DS2_ProgressCarry_Tick()
{
#ifdef _WIN32
    if (!s_installed.load())
    {
        return;
    }
    const ULONGLONG Now = GetTickCount64();
    if (Now < s_next_check)
    {
        return;
    }
    s_next_check = Now + 250;
    const uintptr_t Manager = FlagManager();
    if (Manager == 0)
    {
        return;   // orders wait for the world
    }
    RunOrders(Manager);
    ApplyKept(Manager, Now);
#endif
}

bool DS2_ProgressCarryHook::Install(Injector& injector)
{
#ifdef _WIN32
    const uintptr_t Base = (uintptr_t)injector.GetBaseAddress();
    const uintptr_t Receive = Base + kReceiveOffset;
    const uintptr_t Setter = Base + kSetterOffset;
    if (!BytesMatch(Receive, kReceivePrologue, sizeof(kReceivePrologue)) ||
        !BytesMatch(Setter, kSetterPrologue, sizeof(kSetterPrologue)))
    {
        Error("[DS2_ProgressCarryHook] o codigo do recebimento de flag ou do setter nao e o esperado; recusando");
        return false;
    }
    s_base = Base;
    s_setter = (Setter_p)Setter;
    s_original_receive = (Receive_p)Receive;

    s_log_path = injector.GetDllPath() / "DS2_Carry.log";
    s_request_path = injector.GetDllPath() / "DS2_Carry.req";
    s_pending_path = injector.GetDllPath() / "DS2_Carry.pending";
    LoadKept();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_receive, ReceiveHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        Error("[DS2_ProgressCarryHook] nao consegui instalar o detour");
        return false;
    }

    s_running.store(true);
    s_thread = std::thread(Run);
    s_installed.store(true);
    Append(StringFormat("%s  === ds2os progresso: %zu flag(s) guardada(s) de antes ===\n", Clock().c_str(), s_kept.size()));
    Log("[DS2_ProgressCarryHook] pronto; %zu flag(s) guardada(s)", s_kept.size());
#endif
    return true;
}

void DS2_ProgressCarryHook::Uninstall()
{
#ifdef _WIN32
    s_installed.store(false);
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }
    if (s_original_receive != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_receive, ReceiveHook);
        DetourTransactionCommit();
        s_original_receive = nullptr;
    }
#endif
}

const char* DS2_ProgressCarryHook::GetName()
{
    return "DS2 Progress Carry";
}
