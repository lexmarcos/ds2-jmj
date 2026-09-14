/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_CoopChannelHook.h"
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
#include "ThirdParty/detours/src/detours.h"
#endif

namespace
{
#ifdef _WIN32

    // Version 1.03 Calibrations 2.02, verified before anything is written.
    //
    // Slot +0x108 of DLNRD::SteamSessionLight, `void(session)`: the session's
    // queued events, then channel 0 read until it is empty.
    constexpr size_t kPollOffset = 0xa75800;
    constexpr uint8_t kPollBytes[] = { 0x48, 0x8b, 0xc4, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xec, 0x50, 0x05, 0x00, 0x00 };

    constexpr size_t kSessionVftable = 0x11b1058;
    constexpr size_t kSessionPollSlot = 0x108;
    constexpr size_t kMembersBegin = 0x68;
    constexpr size_t kMembersEnd = 0x70;
    constexpr size_t kMemberVftable = 0x11b35e8;       // SteamSessionMemberLight
    constexpr size_t kMemberSteamId = 0xc8;
    constexpr size_t kMaxMembers = 16;
    constexpr size_t kMaxSessions = 4;

    // ISteamNetworking and ISteamUser by slot, called the way the game calls
    // them: a CSteamID goes in by value and comes out through a pointer.
    using SteamAccessor_p = void*(*)();
    using Send_p = bool(*)(void* Networking, uint64_t To, const void* Data, uint32_t Size, int Kind, int Channel);
    using Available_p = bool(*)(void* Networking, uint32_t* Size, int Channel);
    using Read_p = bool(*)(void* Networking, void* Data, uint32_t Capacity, uint32_t* Size, uint64_t* From, int Channel);
    using SteamId_p = uint64_t*(*)(void* User, uint64_t* Out);
    constexpr size_t kSendSlot = 0x00;
    constexpr size_t kAvailableSlot = 0x08;
    constexpr size_t kReadSlot = 0x10;
    constexpr size_t kSteamIdSlot = 0x10;
    constexpr int kReliable = 2;                       // k_EP2PSendReliable
    constexpr int kChannel = 7;                        // the game reads 0 and nothing else

    constexpr uint8_t kVersion = 1;
    constexpr uint8_t kKindBonfire = 1;
    constexpr uint8_t kWorldOwner = 0;                 // *(chr+0xb0)+0x3c

    constexpr ULONGLONG kAnnounceEveryMs = 2000;
    constexpr ULONGLONG kLocalStaleMs = 2000;          // no frame published: loading, or no player
    constexpr ULONGLONG kHostFreshMs = 30000;
    constexpr ULONGLONG kMembersFreshMs = 5000;

#pragma pack(push, 1)
    struct Announcement
    {
        char Magic[4];
        uint8_t Version;
        uint8_t Kind;
        uint8_t Role;                                  // the sender's
        uint8_t Reserved;
        uint32_t Map;
        int32_t Type;
        uint32_t Id;
        uint32_t Sequence;
    };
#pragma pack(pop)
    static_assert(sizeof(Announcement) == 24, "the announcement is 24 bytes on the wire");
    constexpr char kMagic[4] = { 'J', 'M', 'J', 'C' };

    using Poll_p = void(*)(void* Session);
    Poll_p s_original_poll = nullptr;

    uintptr_t s_base = 0;
    std::atomic<SteamAccessor_p> s_networking{ nullptr };
    std::atomic<SteamAccessor_p> s_user{ nullptr };
    std::atomic<uint64_t> s_self{ 0 };

    // Published by the game's thread.
    struct Local
    {
        uint8_t Role = 0xff;
        uint32_t Map = 0;
        int32_t Type = 0;
        uint32_t Id = 0;
        ULONGLONG Tick = 0;
    };
    std::mutex s_local_mutex;
    Local s_local;

    // Written by the polls, read by the game's thread and by status.
    struct Members
    {
        uintptr_t Session = 0;
        uint64_t Ids[kMaxMembers] = {};
        size_t Count = 0;
        ULONGLONG Tick = 0;
    };
    struct Heard
    {
        bool Valid = false;
        Announcement Last = {};
        uint64_t From = 0;
        ULONGLONG Tick = 0;
    };
    std::mutex s_net_mutex;
    Members s_sessions[kMaxSessions];
    Heard s_heard;

    // What was last announced, per session: two sessions polled in turn
    // must not look like one whose members keep changing.
    struct Sent
    {
        uintptr_t Session = 0;
        uint32_t Map = 0;
        int32_t Type = 0;
        uint32_t Id = 0;
        uint64_t Members = 0;
        bool Failed = false;
        ULONGLONG Tick = 0;
    };
    std::mutex s_announce_mutex;
    Sent s_sent_state[kMaxSessions];
    uint32_t s_sequence = 0;

    std::atomic<uint64_t> s_polls{ 0 };
    std::atomic<uint64_t> s_foreign{ 0 };
    std::atomic<uint64_t> s_sent{ 0 };
    std::atomic<uint64_t> s_send_failed{ 0 };
    std::atomic<uint64_t> s_received{ 0 };
    std::atomic<uint64_t> s_refused{ 0 };
    std::atomic<bool> s_steam_missing_logged{ false };

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

    std::string Clock()
    {
        SYSTEMTIME Now;
        GetLocalTime(&Now);
        return StringFormat("%02u:%02u:%02u.%03u", Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds);
    }

    bool ReadBytes(uintptr_t Address, void* Out, size_t Length)
    {
        __try
        {
            memcpy(Out, (const void*)Address, Length);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadPointer(uintptr_t Address, uintptr_t& Out)
    {
        Out = 0;
        return ReadBytes(Address, &Out, sizeof(Out)) && Out != 0;
    }

    bool BytesMatch(uintptr_t Address, const uint8_t* Expected, size_t Length)
    {
        return memcmp((const void*)Address, Expected, Length) == 0;
    }

    void* VirtualAt(void* Interface, size_t Slot)
    {
        return (*(void***)Interface)[Slot / sizeof(void*)];
    }

    // steam_api64.dll is one of the game's imports, but the injector can be in
    // before the loader has gone through them; asked for on the first poll.
    void* Networking()
    {
        SteamAccessor_p Accessor = s_networking.load();
        if (Accessor == nullptr)
        {
            HMODULE Steam = GetModuleHandleW(L"steam_api64.dll");
            if (Steam != nullptr)
            {
                s_user.store((SteamAccessor_p)GetProcAddress(Steam, "SteamUser"));
                Accessor = (SteamAccessor_p)GetProcAddress(Steam, "SteamNetworking");
                s_networking.store(Accessor);
            }
            if (Accessor == nullptr)
            {
                if (!s_steam_missing_logged.exchange(true))
                {
                    Append(StringFormat("%s  steam_api64.dll sem SteamNetworking; o canal fica mudo\n", Clock().c_str()));
                }
                return nullptr;
            }
        }
        return Accessor();
    }

    uint64_t SelfId()
    {
        uint64_t Self = s_self.load();
        const SteamAccessor_p Accessor = s_user.load();
        if (Self == 0 && Accessor != nullptr)
        {
            if (void* User = Accessor())
            {
                ((SteamId_p)VirtualAt(User, kSteamIdSlot))(User, &Self);
                s_self.store(Self);
            }
        }
        return Self;
    }

    size_t ReadMembers(uintptr_t Session, uint64_t Ids[kMaxMembers])
    {
        uintptr_t Begin = 0, End = 0;
        if (!ReadPointer(Session + kMembersBegin, Begin) ||
            !ReadBytes(Session + kMembersEnd, &End, sizeof(End)) ||
            End < Begin || (End - Begin) % sizeof(uintptr_t) != 0)
        {
            return 0;
        }

        size_t Count = 0;
        for (uintptr_t At = Begin; At < End && Count < kMaxMembers; At += sizeof(uintptr_t))
        {
            uintptr_t Member = 0, Vftable = 0;
            uint64_t Id = 0;
            if (ReadPointer(At, Member) && ReadPointer(Member, Vftable) && Vftable == s_base + kMemberVftable &&
                ReadBytes(Member + kMemberSteamId, &Id, sizeof(Id)) && Id != 0)
            {
                Ids[Count++] = Id;
            }
        }
        return Count;
    }

    // Under s_net_mutex.
    bool IsMemberLocked(uint64_t Id, ULONGLONG Now)
    {
        for (const Members& Session : s_sessions)
        {
            if (Session.Session == 0 || Now - Session.Tick > kMembersFreshMs)
            {
                continue;
            }
            for (size_t i = 0; i < Session.Count; ++i)
            {
                if (Session.Ids[i] == Id)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Returns true when this session's members are not what they were.
    bool Remember(const Members& Now)
    {
        std::scoped_lock Lock(s_net_mutex);
        Members* Slot = nullptr;
        for (Members& Session : s_sessions)
        {
            if (Session.Session == Now.Session)
            {
                Slot = &Session;
                break;
            }
        }
        if (Slot == nullptr)
        {
            Slot = &s_sessions[0];
            for (Members& Session : s_sessions)
            {
                if (Session.Tick < Slot->Tick)
                {
                    Slot = &Session;
                }
            }
        }
        const bool Changed = Slot->Session != Now.Session || Slot->Count != Now.Count ||
            memcmp(Slot->Ids, Now.Ids, sizeof(Now.Ids)) != 0;
        *Slot = Now;
        return Changed;
    }

    std::string DescribeIds(const uint64_t* Ids, size_t Count)
    {
        std::string Out;
        for (size_t i = 0; i < Count; ++i)
        {
            Out += StringFormat(" %016llx", (unsigned long long)Ids[i]);
        }
        return Out.empty() ? " nenhum" : Out;
    }

    void Refuse(const char* Why, uint64_t From, uint32_t Size)
    {
        const uint64_t Count = ++s_refused;
        if (Count <= 20 || Count % 100 == 0)
        {
            Append(StringFormat("%s  pacote RECUSADO #%llu de %016llx (%u bytes): %s\n",
                Clock().c_str(), (unsigned long long)Count, (unsigned long long)From, Size, Why));
        }
    }

    void Handle(const uint8_t* Data, uint32_t Size, uint64_t From)
    {
        Announcement Said;
        if (Size != sizeof(Said))
        {
            Refuse("tamanho", From, Size);
            return;
        }
        memcpy(&Said, Data, sizeof(Said));
        if (memcmp(Said.Magic, kMagic, sizeof(kMagic)) != 0 || Said.Version != kVersion || Said.Kind != kKindBonfire)
        {
            Refuse("nao e um anuncio desta versao", From, Size);
            return;
        }
        if (Said.Role != kWorldOwner)
        {
            Refuse("so o dono do mundo anuncia a fogueira", From, Size);
            return;
        }

        bool Member = false, Changed = false;
        {
            std::scoped_lock Lock(s_net_mutex);
            const ULONGLONG Now = GetTickCount64();
            Member = IsMemberLocked(From, Now);
            if (Member)
            {
                Changed = !s_heard.Valid || s_heard.From != From || s_heard.Last.Map != Said.Map ||
                    s_heard.Last.Type != Said.Type || s_heard.Last.Id != Said.Id;
                s_heard.Valid = true;
                s_heard.Last = Said;
                s_heard.From = From;
                s_heard.Tick = Now;
            }
        }

        if (!Member)
        {
            Refuse("de fora da sessao", From, Size);
        }
        else if (Changed)
        {
            Append(StringFormat("%s  fogueira do host recebida de %016llx: mapa %08x tipo %d id %08x (seq %u)\n",
                Clock().c_str(), (unsigned long long)From, Said.Map, Said.Type, Said.Id, Said.Sequence));
        }
    }

    void Receive(void* Net)
    {
        const auto Available = (Available_p)VirtualAt(Net, kAvailableSlot);
        const auto Read = (Read_p)VirtualAt(Net, kReadSlot);
        for (int i = 0; i < 32; ++i)
        {
            uint32_t Size = 0;
            if (!Available(Net, &Size, kChannel))
            {
                break;
            }

            // A packet too big for the buffer is still taken off the queue
            // (truncated) and refused by its size.
            uint8_t Buffer[256];
            uint32_t Got = 0;
            uint64_t From = 0;
            if (!Read(Net, Buffer, sizeof(Buffer), &Got, &From, kChannel))
            {
                break;
            }
            ++s_received;
            Handle(Buffer, Size > sizeof(Buffer) ? Size : Got, From);
        }
    }

    void Announce(void* Net, const Members& Now, uint64_t Self)
    {
        Local Mine;
        {
            std::scoped_lock Lock(s_local_mutex);
            Mine = s_local;
        }
        const ULONGLONG Tick = GetTickCount64();
        if (Mine.Tick == 0 || Tick - Mine.Tick > kLocalStaleMs || Mine.Role != kWorldOwner)
        {
            return;
        }

        uint64_t Others[kMaxMembers] = {};
        size_t Count = 0;
        uint64_t Hash = 1469598103934665603ull;
        for (size_t i = 0; i < Now.Count; ++i)
        {
            if (Now.Ids[i] != Self)
            {
                Others[Count++] = Now.Ids[i];
                Hash = (Hash ^ Now.Ids[i]) * 1099511628211ull;
            }
        }
        if (Count == 0)
        {
            return;
        }

        std::scoped_lock Lock(s_announce_mutex);
        Sent* Last = nullptr;
        for (Sent& Entry : s_sent_state)
        {
            if (Entry.Session == Now.Session)
            {
                Last = &Entry;
                break;
            }
        }
        if (Last == nullptr)
        {
            Last = &s_sent_state[0];
            for (Sent& Entry : s_sent_state)
            {
                if (Entry.Tick < Last->Tick)
                {
                    Last = &Entry;
                }
            }
            *Last = Sent();
        }
        const bool Changed = Last->Session != Now.Session || Mine.Map != Last->Map ||
            Mine.Type != Last->Type || Mine.Id != Last->Id || Hash != Last->Members;
        if (!Changed && Tick - Last->Tick < kAnnounceEveryMs)
        {
            return;
        }

        Announcement Said = {};
        memcpy(Said.Magic, kMagic, sizeof(kMagic));
        Said.Version = kVersion;
        Said.Kind = kKindBonfire;
        Said.Role = Mine.Role;
        Said.Map = Mine.Map;
        Said.Type = Mine.Type;
        Said.Id = Mine.Id;
        Said.Sequence = ++s_sequence;

        const auto Send = (Send_p)VirtualAt(Net, kSendSlot);
        size_t Delivered = 0;
        std::string Failed;
        for (size_t i = 0; i < Count; ++i)
        {
            if (Send(Net, Others[i], &Said, sizeof(Said), kReliable, kChannel))
            {
                ++Delivered;
                ++s_sent;
            }
            else
            {
                ++s_send_failed;
                Failed += StringFormat(" %016llx", (unsigned long long)Others[i]);
            }
        }

        const bool FailedNow = !Failed.empty();
        if (Changed || FailedNow != Last->Failed)
        {
            Append(StringFormat("%s  fogueira anunciada na sessao %p: mapa %08x tipo %d id %08x (seq %u) para %zu de %zu membros%s%s\n",
                Clock().c_str(), (void*)Now.Session, Mine.Map, Mine.Type, Mine.Id, Said.Sequence, Delivered, Count,
                FailedNow ? "; falhou para" : "", Failed.c_str()));
        }
        Last->Session = Now.Session;
        Last->Map = Mine.Map;
        Last->Type = Mine.Type;
        Last->Id = Mine.Id;
        Last->Members = Hash;
        Last->Failed = FailedNow;
        Last->Tick = Tick;
    }

    void Pump(uintptr_t Session)
    {
        uintptr_t Vftable = 0;
        if (!ReadPointer(Session, Vftable) || Vftable != s_base + kSessionVftable)
        {
            ++s_foreign;
            return;
        }
        ++s_polls;

        void* Net = Networking();
        if (Net == nullptr)
        {
            return;
        }

        Members Now;
        Now.Session = Session;
        Now.Count = ReadMembers(Session, Now.Ids);
        Now.Tick = GetTickCount64();
        const uint64_t Self = SelfId();
        if (Remember(Now))
        {
            Append(StringFormat("%s  sessao %p: %zu membros:%s (eu %016llx)\n",
                Clock().c_str(), (void*)Session, Now.Count, DescribeIds(Now.Ids, Now.Count).c_str(),
                (unsigned long long)Self));
        }

        Receive(Net);
        Announce(Net, Now, Self);
    }

    void PollHook(void* Session)
    {
        s_original_poll(Session);
        Pump((uintptr_t)Session);
    }

    void WriteStatus()
    {
        const ULONGLONG Now = GetTickCount64();
        Local Mine;
        {
            std::scoped_lock Lock(s_local_mutex);
            Mine = s_local;
        }
        Members Sessions[kMaxSessions];
        Heard Last;
        {
            std::scoped_lock Lock(s_net_mutex);
            for (size_t i = 0; i < kMaxSessions; ++i)
            {
                Sessions[i] = s_sessions[i];
            }
            Last = s_heard;
        }

        std::string Text = StringFormat("%s  === canal %d: polls=%llu estranhos=%llu enviados=%llu falhas=%llu recebidos=%llu recusados=%llu eu=%016llx ===\n",
            Clock().c_str(), kChannel, (unsigned long long)s_polls.load(), (unsigned long long)s_foreign.load(),
            (unsigned long long)s_sent.load(), (unsigned long long)s_send_failed.load(),
            (unsigned long long)s_received.load(), (unsigned long long)s_refused.load(),
            (unsigned long long)s_self.load());
        for (const Members& Session : Sessions)
        {
            if (Session.Session != 0)
            {
                Text += StringFormat("    sessao %p vista ha %llu ms: %zu membros:%s\n", (void*)Session.Session,
                    (unsigned long long)(Now - Session.Tick), Session.Count, DescribeIds(Session.Ids, Session.Count).c_str());
            }
        }
        Text += Mine.Tick == 0
            ? std::string("    local: nada publicado\n")
            : StringFormat("    local ha %llu ms: papel %u, registro mapa %08x tipo %d id %08x\n",
                (unsigned long long)(Now - Mine.Tick), Mine.Role, Mine.Map, Mine.Type, Mine.Id);
        Text += !Last.Valid
            ? std::string("    do host: nada recebido\n")
            : StringFormat("    do host ha %llu ms: %016llx, mapa %08x tipo %d id %08x (seq %u)\n",
                (unsigned long long)(Now - Last.Tick), (unsigned long long)Last.From, Last.Last.Map,
                Last.Last.Type, Last.Last.Id, Last.Last.Sequence);
        Append(Text);
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
                    std::istringstream Parts(Line);
                    std::string Verb;
                    Parts >> Verb;
                    if (Verb == "status")
                    {
                        WriteStatus();
                    }
                    else if (!Verb.empty())
                    {
                        Append(StringFormat("%s  pedido desconhecido: %s\n", Clock().c_str(), Line.c_str()));
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

void DS2_CoopChannel::PublishLocal(uint8_t Role, uint32_t Map, int32_t Type, uint32_t Id)
{
#ifdef _WIN32
    std::scoped_lock Lock(s_local_mutex);
    s_local.Role = Role;
    s_local.Map = Map;
    s_local.Type = Type;
    s_local.Id = Id;
    s_local.Tick = GetTickCount64();
#endif
}

bool DS2_CoopChannel::HostBonfire(Bonfire& Out)
{
#ifdef _WIN32
    std::scoped_lock Lock(s_net_mutex);
    const ULONGLONG Now = GetTickCount64();
    if (!s_heard.Valid || Now - s_heard.Tick > kHostFreshMs || !IsMemberLocked(s_heard.From, Now))
    {
        return false;
    }
    Out.Map = s_heard.Last.Map;
    Out.Type = s_heard.Last.Type;
    Out.Id = s_heard.Last.Id;
    Out.From = s_heard.From;
    Out.AgeMs = Now - s_heard.Tick;
    return true;
#else
    return false;
#endif
}

bool DS2_CoopChannelHook::Install(Injector& injector)
{
#ifdef _WIN32
    s_base = (uintptr_t)injector.GetBaseAddress();

    if (!BytesMatch(s_base + kPollOffset, kPollBytes, sizeof(kPollBytes)) ||
        *(const uintptr_t*)(s_base + kSessionVftable + kSessionPollSlot) != s_base + kPollOffset)
    {
        Error("[DS2_CoopChannelHook] o poll da sessao em +0x%zx nao e o esperado; recusando", kPollOffset);
        return false;
    }

    s_log_path = injector.GetDllPath() / "DS2_Channel.log";
    s_request_path = injector.GetDllPath() / "DS2_Channel.req";
    s_original_poll = (Poll_p)(s_base + kPollOffset);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_poll, PollHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        Error("[DS2_CoopChannelHook] nao consegui instalar o detour");
        return false;
    }

    s_running.store(true);
    s_thread = std::thread(Run);

    Append(StringFormat("%s  === ds2os canal P2P %d: a fogueira do host ===\n", Clock().c_str(), kChannel));
    Log("[DS2_CoopChannelHook] pronto; o host anuncia a fogueira no canal P2P %d", kChannel);
#endif
    return true;
}

void DS2_CoopChannelHook::Uninstall()
{
#ifdef _WIN32
    s_running.store(false);
    if (s_thread.joinable())
    {
        s_thread.join();
    }

    if (s_original_poll != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_poll, PollHook);
        DetourTransactionCommit();
        s_original_poll = nullptr;
    }
#endif
}

const char* DS2_CoopChannelHook::GetName()
{
    return "DS2 Coop Channel";
}
