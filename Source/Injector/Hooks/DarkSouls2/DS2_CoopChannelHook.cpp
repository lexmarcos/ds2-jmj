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
    // Set when the member is added (FUN_140a72740): its id equals
    // GetLobbyOwner of the session's lobby. The game logs it as "Host".
    constexpr size_t kMemberIsHost = 0xad;
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
    // Kinds 2 and up are the host's events (DS2_CoopChannel::HostEvent),
    // sent once, reliably, to every other member.
    constexpr uint8_t kKindFirstEvent = 2;
    constexpr uint8_t kKindLastEvent = 2 + DS2_CoopChannel::kHostEventCount - 1;
    // A guest's answer to a vote: Id the vote, Type 1 yes and 0 no.
    constexpr uint8_t kKindAnswer = 0x20;
    // The host's lit bonfires, a bitmap over the bonfire table's order: the
    // count in Reserved, the bits in Map, Type and Id.
    constexpr uint8_t kKindLit = 0x30;
    // One map's three flag categories, 75 bytes, in a packet of its own
    // because they do not fit in an announcement. See PublishMapFlags.
    constexpr uint8_t kKindMapFlags = 0x40;
    constexpr size_t kMaxFlagMaps = 8;
    // One chunk of a map's object state; see PublishMapObjects.
    constexpr uint8_t kKindMapObjState = 0x50;
    constexpr size_t kMapObjPerPacket = 128;
    constexpr size_t kMapObjChunks = (DS2_CoopChannel::kMapObjMax + kMapObjPerPacket - 1) / kMapObjPerPacket;
    // A guest's event: 0x28 + DS2_CoopChannel::GuestEvent.
    constexpr uint8_t kKindFirstGuestEvent = 0x28;
    constexpr uint8_t kKindLastGuestEvent = 0x28 + DS2_CoopChannel::kGuestEventCount - 1;
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
#pragma pack(push, 1)
    struct MapFlagsPacket
    {
        char Magic[4];
        uint8_t Version;
        uint8_t Kind;
        uint8_t Role;
        uint8_t Reserved;
        uint32_t Map;
        uint8_t Bytes[DS2_CoopChannel::kMapFlagBytes];
    };
#pragma pack(pop)
    static_assert(sizeof(MapFlagsPacket) == 87, "the map flags packet is 87 bytes on the wire");
#pragma pack(push, 1)
    struct MapObjPacket
    {
        char Magic[4];
        uint8_t Version;
        uint8_t Kind;
        uint8_t Role;
        uint8_t Reserved;
        uint32_t Map;
        uint16_t Total;                          // entries the host has for this map
        uint16_t First;                          // where this chunk starts
        uint16_t Count;                          // entries in this chunk
        uint16_t Index[kMapObjPerPacket];
        uint8_t State[kMapObjPerPacket];
    };
#pragma pack(pop)
    static_assert(sizeof(MapObjPacket) == 402, "the map object packet is 402 bytes on the wire");
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
        bool Host[kMaxMembers] = {};
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
    // The host's lit bonfires, last heard (guest) and to send (host).
    struct Lit
    {
        bool Valid = false;
        uint8_t Count = 0;
        uint32_t Bits[3] = {};
        ULONGLONG Tick = 0;
    };
    std::mutex s_net_mutex;
    Lit s_lit;
    std::atomic<uint8_t> s_lit_count{ 0 };
    std::atomic<uint32_t> s_lit_bits[3];
    std::atomic<ULONGLONG> s_lit_tick{ 0 };
    ULONGLONG s_lit_sent_tick = 0;
    uint8_t s_lit_sent_count = 0;
    uint32_t s_lit_sent_bits[3] = {};

    // One map's three flag categories: what the game's thread published (the
    // host) and what came in from the host of this session (a guest). Both
    // under s_flags_mutex, because 75 bytes are not an atomic.
    struct MapFlags
    {
        uint32_t Map = 0;
        uint8_t Bytes[DS2_CoopChannel::kMapFlagBytes] = {};
        ULONGLONG Tick = 0;
    };
    std::mutex s_flags_mutex;
    MapFlags s_flags_mine[kMaxFlagMaps];     // the host's, to send
    MapFlags s_flags_sent[kMaxFlagMaps];     // what the poll last sent
    MapFlags s_flags_heard[kMaxFlagMaps];    // a guest's, received

    // One map's object state: what the game's thread published (the host) and
    // what the host of this session sent (a guest). Under s_obj_mutex.
    struct MapObjects
    {
        uint32_t Map = 0;
        uint16_t Count = 0;
        uint16_t Index[DS2_CoopChannel::kMapObjMax] = {};
        uint8_t State[DS2_CoopChannel::kMapObjMax] = {};
        uint32_t Chunks = 0;                     // bit per chunk, for the guest
        ULONGLONG Tick = 0;
    };
    std::mutex s_obj_mutex;
    MapObjects s_obj_mine[kMaxFlagMaps];
    MapObjects s_obj_sent[kMaxFlagMaps];
    MapObjects s_obj_heard[kMaxFlagMaps];
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

    // Host events: bits queued by the game's thread, sent by the next poll.
    // Received ones count up per kind; the game's thread takes each once.
    // A queued event nobody could be told within this long is dropped: a
    // guest summoned later must not get a rest that happened before it came.
    std::atomic<uint32_t> s_events_pending{ 0 };
    std::atomic<ULONGLONG> s_events_tick{ 0 };
    std::atomic<uint32_t> s_event_map[DS2_CoopChannel::kHostEventCount] = {};
    std::atomic<uint32_t> s_event_id[DS2_CoopChannel::kHostEventCount] = {};
    std::atomic<int32_t> s_event_type[DS2_CoopChannel::kHostEventCount] = {};

    // Guest events waiting to be sent (guest side) and received ones (host).
    std::atomic<bool> s_guest_pending[DS2_CoopChannel::kGuestEventCount] = {};
    std::atomic<uint32_t> s_guest_map[DS2_CoopChannel::kGuestEventCount] = {};
    std::atomic<uint32_t> s_guest_id[DS2_CoopChannel::kGuestEventCount] = {};
    struct HeardGuestEvent
    {
        uint64_t Count = 0;
        uint32_t Map = 0;
        uint32_t Id = 0;
        uint64_t From = 0;
        ULONGLONG Tick = 0;
    };
    HeardGuestEvent s_heard_guest[DS2_CoopChannel::kGuestEventCount];   // under s_net_mutex
    uint64_t s_taken_guest[DS2_CoopChannel::kGuestEventCount] = {};

    // Votes: a guest's answer waiting to be sent, and the answers a host got.
    std::atomic<bool> s_answer_pending{ false };
    std::atomic<uint32_t> s_answer_vote{ 0 };
    std::atomic<bool> s_answer_yes{ false };
    struct Answer
    {
        uint32_t Vote = 0;
        uint64_t From = 0;
        bool Yes = false;
    };
    constexpr size_t kMaxAnswers = 16;
    Answer s_answers[kMaxAnswers];   // under s_net_mutex
    size_t s_answer_next = 0;
    constexpr ULONGLONG kEventFreshMs = 5000;
    struct HeardEvent
    {
        uint64_t Count = 0;
        Announcement Last = {};
        uint64_t From = 0;
        ULONGLONG Tick = 0;
    };
    HeardEvent s_heard_events[DS2_CoopChannel::kHostEventCount];
    uint64_t s_taken_events[DS2_CoopChannel::kHostEventCount] = {};

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

    size_t ReadMembers(uintptr_t Session, uint64_t Ids[kMaxMembers], bool Host[kMaxMembers])
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
            uint8_t IsHost = 0;
            if (ReadPointer(At, Member) && ReadPointer(Member, Vftable) && Vftable == s_base + kMemberVftable &&
                ReadBytes(Member + kMemberSteamId, &Id, sizeof(Id)) && Id != 0 &&
                ReadBytes(Member + kMemberIsHost, &IsHost, 1))
            {
                Host[Count] = IsHost != 0;
                Ids[Count++] = Id;
            }
        }
        return Count;
    }

    // Under s_net_mutex: the host of a session seen in the last few seconds.
    bool IsHostLocked(uint64_t Id, ULONGLONG Now)
    {
        for (const Members& Session : s_sessions)
        {
            if (Session.Session == 0 || Now - Session.Tick > kMembersFreshMs)
            {
                continue;
            }
            for (size_t i = 0; i < Session.Count; ++i)
            {
                if (Session.Ids[i] == Id && Session.Host[i])
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Under s_net_mutex: a member of a session this machine hosts.
    bool IsMyGuestLocked(uint64_t Id, ULONGLONG Now)
    {
        const uint64_t Self = s_self.load();
        for (const Members& Session : s_sessions)
        {
            if (Session.Session == 0 || Now - Session.Tick > kMembersFreshMs)
            {
                continue;
            }
            bool SelfIsHost = false, Has = false;
            for (size_t i = 0; i < Session.Count; ++i)
            {
                SelfIsHost = SelfIsHost || (Session.Ids[i] == Self && Session.Host[i]);
                Has = Has || (Session.Ids[i] == Id && Id != Self);
            }
            if (SelfIsHost && Has)
            {
                return true;
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
            memcmp(Slot->Ids, Now.Ids, sizeof(Now.Ids)) != 0 || memcmp(Slot->Host, Now.Host, sizeof(Now.Host)) != 0;
        *Slot = Now;
        return Changed;
    }

    std::string DescribeMembers(const Members& Session)
    {
        std::string Out;
        for (size_t i = 0; i < Session.Count; ++i)
        {
            Out += StringFormat(" %016llx%s", (unsigned long long)Session.Ids[i], Session.Host[i] ? " (host)" : "");
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

    // A map's three flag categories from the host of this session.
    void HandleMapFlags(const uint8_t* Data, uint32_t Size, uint64_t From)
    {
        MapFlagsPacket Said;
        memcpy(&Said, Data, sizeof(Said));
        if (memcmp(Said.Magic, kMagic, sizeof(kMagic)) != 0 || Said.Version != kVersion || Said.Map == 0 ||
            Said.Role != kWorldOwner)
        {
            Refuse("flags de mapa nao sao desta versao", From, Size);
            return;
        }
        const ULONGLONG Now = GetTickCount64();
        {
            std::scoped_lock Lock(s_net_mutex);
            if (!IsHostLocked(From, Now))
            {
                Refuse("flags de mapa de quem nao e o host da sessao", From, Size);
                return;
            }
        }
        bool Changed = false;
        {
            std::scoped_lock Lock(s_flags_mutex);
            MapFlags* Slot = nullptr;
            MapFlags* Oldest = &s_flags_heard[0];
            for (MapFlags& Entry : s_flags_heard)
            {
                if (Entry.Map == Said.Map)
                {
                    Slot = &Entry;
                    break;
                }
                if (Entry.Map == 0 || Entry.Tick < Oldest->Tick)
                {
                    Oldest = &Entry;
                }
            }
            if (Slot == nullptr)
            {
                Slot = Oldest;
                Slot->Map = Said.Map;
                memset(Slot->Bytes, 0, sizeof(Slot->Bytes));
            }
            Changed = memcmp(Slot->Bytes, Said.Bytes, sizeof(Slot->Bytes)) != 0;
            memcpy(Slot->Bytes, Said.Bytes, sizeof(Slot->Bytes));
            Slot->Tick = Now;
        }
        if (Changed)
        {
            Append(StringFormat("%s  flags do mapa %08x recebidos do host\n", Clock().c_str(), Said.Map));
        }
    }

    // One chunk of a map's object state from the host of this session.
    void HandleMapObjects(const uint8_t* Data, uint32_t Size, uint64_t From)
    {
        MapObjPacket Said;
        memcpy(&Said, Data, sizeof(Said));
        if (memcmp(Said.Magic, kMagic, sizeof(kMagic)) != 0 || Said.Version != kVersion || Said.Map == 0 ||
            Said.Role != kWorldOwner || Said.Count > kMapObjPerPacket ||
            Said.Total > DS2_CoopChannel::kMapObjMax ||
            (size_t)Said.First + Said.Count > DS2_CoopChannel::kMapObjMax)
        {
            Refuse("estado de objetos nao e desta versao", From, Size);
            return;
        }
        const ULONGLONG Now = GetTickCount64();
        {
            std::scoped_lock Lock(s_net_mutex);
            if (!IsHostLocked(From, Now))
            {
                Refuse("estado de objetos de quem nao e o host da sessao", From, Size);
                return;
            }
        }
        const uint32_t Chunk = 1u << (Said.First / kMapObjPerPacket);
        bool Complete = false;
        {
            std::scoped_lock Lock(s_obj_mutex);
            MapObjects* Slot = nullptr;
            MapObjects* Oldest = &s_obj_heard[0];
            for (MapObjects& Entry : s_obj_heard)
            {
                if (Entry.Map == Said.Map)
                {
                    Slot = &Entry;
                    break;
                }
                if (Entry.Map == 0 || Entry.Tick < Oldest->Tick)
                {
                    Oldest = &Entry;
                }
            }
            if (Slot == nullptr)
            {
                Slot = Oldest;
                Slot->Map = Said.Map;
                Slot->Chunks = 0;
            }
            // A new total means the host rebuilt the list; start again.
            if (Slot->Count != Said.Total)
            {
                Slot->Count = Said.Total;
                Slot->Chunks = 0;
            }
            memcpy(Slot->Index + Said.First, Said.Index, Said.Count * sizeof(uint16_t));
            memcpy(Slot->State + Said.First, Said.State, Said.Count);
            Slot->Chunks |= Chunk;
            Slot->Tick = Now;
            const uint32_t Wanted = Said.Total == 0 ? 0u
                : (1u << ((Said.Total + kMapObjPerPacket - 1) / kMapObjPerPacket)) - 1u;
            Complete = (Slot->Chunks & Wanted) == Wanted;
        }
        if (Complete)
        {
            Append(StringFormat("%s  estado de %u objetos do mapa %08x recebido do host\n",
                Clock().c_str(), (unsigned)Said.Total, Said.Map));
        }
    }

    void Handle(const uint8_t* Data, uint32_t Size, uint64_t From)
    {
        if (Size == sizeof(MapObjPacket) && Data[5] == kKindMapObjState)
        {
            HandleMapObjects(Data, Size, From);
            return;
        }
        if (Size == sizeof(MapFlagsPacket) && Data[5] == kKindMapFlags)
        {
            HandleMapFlags(Data, Size, From);
            return;
        }
        Announcement Said;
        if (Size != sizeof(Said))
        {
            Refuse("tamanho", From, Size);
            return;
        }
        memcpy(&Said, Data, sizeof(Said));
        const bool Event = Said.Kind >= kKindFirstEvent && Said.Kind <= kKindLastEvent;
        if (memcmp(Said.Magic, kMagic, sizeof(kMagic)) != 0 || Said.Version != kVersion ||
            (Said.Kind != kKindBonfire && Said.Kind != kKindAnswer && Said.Kind != kKindLit && !Event &&
             !(Said.Kind >= kKindFirstGuestEvent && Said.Kind <= kKindLastGuestEvent)))
        {
            Refuse("nao e um anuncio desta versao", From, Size);
            return;
        }
        if (Said.Kind >= kKindFirstGuestEvent && Said.Kind <= kKindLastGuestEvent)
        {
            bool Member = false;
            {
                std::scoped_lock Lock(s_net_mutex);
                Member = IsMyGuestLocked(From, GetTickCount64());
                if (Member)
                {
                    HeardGuestEvent& Entry = s_heard_guest[Said.Kind - kKindFirstGuestEvent];
                    ++Entry.Count;
                    Entry.Map = Said.Map;
                    Entry.Id = Said.Id;
                    Entry.From = From;
                    Entry.Tick = GetTickCount64();
                }
            }
            if (!Member)
            {
                Refuse("evento de quem nao e convidado desta sessao", From, Size);
                return;
            }
            Append(StringFormat("%s  evento %u do convidado %016llx: mapa %08x id %08x\n", Clock().c_str(), (unsigned)Said.Kind,
                (unsigned long long)From, Said.Map, Said.Id));
            return;
        }
        if (Said.Kind == kKindAnswer)
        {
            // From a member of a session this machine hosts.
            bool Member = false;
            {
                std::scoped_lock Lock(s_net_mutex);
                Member = IsMyGuestLocked(From, GetTickCount64());
                if (Member)
                {
                    bool Replaced = false;
                    for (Answer& Entry : s_answers)
                    {
                        if (Entry.Vote == Said.Id && Entry.From == From)
                        {
                            Entry.Yes = Said.Type != 0;
                            Replaced = true;
                        }
                    }
                    if (!Replaced)
                    {
                        s_answers[s_answer_next] = { Said.Id, From, Said.Type != 0 };
                        s_answer_next = (s_answer_next + 1) % kMaxAnswers;
                    }
                }
            }
            if (!Member)
            {
                Refuse("resposta de quem nao e convidado desta sessao", From, Size);
                return;
            }
            Append(StringFormat("%s  resposta de %016llx a votacao %u: %s\n", Clock().c_str(),
                (unsigned long long)From, Said.Id, Said.Type != 0 ? "sim" : "nao"));
            return;
        }
        if (Said.Role != kWorldOwner)
        {
            Refuse("so o dono do mundo anuncia a fogueira", From, Size);
            return;
        }
        if (Said.Kind == kKindLit)
        {
            bool FromHost = false, Changed = false;
            {
                std::scoped_lock Lock(s_net_mutex);
                const ULONGLONG Now = GetTickCount64();
                FromHost = IsHostLocked(From, Now);
                if (FromHost)
                {
                    Changed = !s_lit.Valid || s_lit.Count != Said.Reserved || s_lit.Bits[0] != Said.Map ||
                        s_lit.Bits[1] != (uint32_t)Said.Type || s_lit.Bits[2] != Said.Id;
                    s_lit.Valid = true;
                    s_lit.Count = Said.Reserved;
                    s_lit.Bits[0] = Said.Map;
                    s_lit.Bits[1] = (uint32_t)Said.Type;
                    s_lit.Bits[2] = Said.Id;
                    s_lit.Tick = Now;
                }
            }
            if (!FromHost)
            {
                Refuse("fogueiras acesas de quem nao e o host de uma sessao", From, Size);
            }
            else if (Changed)
            {
                Append(StringFormat("%s  fogueiras acesas do host %016llx: %u na tabela, %08x %08x %08x\n", Clock().c_str(),
                    (unsigned long long)From, (unsigned)Said.Reserved, Said.Map, (uint32_t)Said.Type, Said.Id));
            }
            return;
        }
        if (Event)
        {
            bool EventFromHost = false;
            {
                std::scoped_lock Lock(s_net_mutex);
                const ULONGLONG Now = GetTickCount64();
                EventFromHost = IsHostLocked(From, Now);
                if (EventFromHost)
                {
                    HeardEvent& Entry = s_heard_events[Said.Kind - kKindFirstEvent];
                    ++Entry.Count;
                    Entry.Last = Said;
                    Entry.From = From;
                    Entry.Tick = Now;
                }
            }
            if (!EventFromHost)
            {
                Refuse("evento de quem nao e o host de uma sessao", From, Size);
                return;
            }
            Append(StringFormat("%s  evento %u do host recebido de %016llx: mapa %08x tipo %d id %08x (seq %u)\n",
                Clock().c_str(), (unsigned)Said.Kind, (unsigned long long)From, Said.Map, Said.Type, Said.Id, Said.Sequence));
            return;
        }

        bool Host = false, Changed = false;
        {
            std::scoped_lock Lock(s_net_mutex);
            const ULONGLONG Now = GetTickCount64();
            Host = IsHostLocked(From, Now);
            if (Host)
            {
                Changed = !s_heard.Valid || s_heard.From != From || s_heard.Last.Map != Said.Map ||
                    s_heard.Last.Type != Said.Type || s_heard.Last.Id != Said.Id;
                s_heard.Valid = true;
                s_heard.Last = Said;
                s_heard.From = From;
                s_heard.Tick = Now;
            }
        }

        if (!Host)
        {
            Refuse("nao e o host de uma sessao", From, Size);
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
            uint8_t Buffer[1024];
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

        // Only the session's host speaks for the world. A guest on its way in
        // is still the owner of its own world for a few seconds (measured
        // 14/09: five announcements of its own bonfire before it arrived).
        uint64_t Others[kMaxMembers] = {};
        size_t Count = 0;
        bool SelfIsHost = false;
        uint64_t Hash = 1469598103934665603ull;
        for (size_t i = 0; i < Now.Count; ++i)
        {
            if (Now.Ids[i] == Self)
            {
                SelfIsHost = Now.Host[i];
            }
            else
            {
                Others[Count++] = Now.Ids[i];
                Hash = (Hash ^ Now.Ids[i]) * 1099511628211ull;
            }
        }
        if (!SelfIsHost || Count == 0)
        {
            return;
        }

        const auto SendTo = (Send_p)VirtualAt(Net, kSendSlot);
        const uint32_t Events = Tick - s_events_tick.load() > kEventFreshMs ? 0 : s_events_pending.exchange(0);
        for (uint8_t Index = 0; Index < DS2_CoopChannel::kHostEventCount; ++Index)
        {
            if ((Events & (1u << Index)) == 0)
            {
                continue;
            }
            Announcement Event = {};
            memcpy(Event.Magic, kMagic, sizeof(kMagic));
            Event.Version = kVersion;
            Event.Kind = (uint8_t)(kKindFirstEvent + Index);
            Event.Role = Mine.Role;
            const uint32_t MapOverride = s_event_map[Index].load();
            const uint32_t IdOverride = s_event_id[Index].load();
            Event.Map = MapOverride != 0 ? MapOverride : Mine.Map;
            Event.Type = s_event_type[Index].load();
            Event.Id = (MapOverride != 0 || IdOverride != 0) ? IdOverride : Mine.Id;
            {
                std::scoped_lock Lock(s_announce_mutex);
                Event.Sequence = ++s_sequence;
            }
            size_t Delivered = 0;
            for (size_t i = 0; i < Count; ++i)
            {
                if (SendTo(Net, Others[i], &Event, sizeof(Event), kReliable, kChannel))
                {
                    ++Delivered;
                    ++s_sent;
                }
                else
                {
                    ++s_send_failed;
                }
            }
            Append(StringFormat("%s  evento %u enviado na sessao %p (seq %u) para %zu de %zu membros\n",
                Clock().c_str(), (unsigned)Event.Kind, (void*)Now.Session, Event.Sequence, Delivered, Count));
        }

        // The lit bonfires, when they changed or every two seconds.
        {
            const uint8_t LitCount = s_lit_count.load();
            uint32_t Bits[3] = { s_lit_bits[0].load(), s_lit_bits[1].load(), s_lit_bits[2].load() };
            const ULONGLONG LitTick = s_lit_tick.load();
            const bool Fresh = LitTick != 0 && Tick - LitTick <= kLocalStaleMs;
            const bool LitChanged = LitCount != s_lit_sent_count || memcmp(Bits, s_lit_sent_bits, sizeof(Bits)) != 0;
            if (Fresh && LitCount != 0 && (LitChanged || Tick - s_lit_sent_tick >= kAnnounceEveryMs))
            {
                Announcement Said = {};
                memcpy(Said.Magic, kMagic, sizeof(kMagic));
                Said.Version = kVersion;
                Said.Kind = kKindLit;
                Said.Role = Mine.Role;
                Said.Reserved = LitCount;
                Said.Map = Bits[0];
                Said.Type = (int32_t)Bits[1];
                Said.Id = Bits[2];
                {
                    std::scoped_lock Lock(s_announce_mutex);
                    Said.Sequence = ++s_sequence;
                }
                for (size_t i = 0; i < Count; ++i)
                {
                    SendTo(Net, Others[i], &Said, sizeof(Said), kReliable, kChannel) ? ++s_sent : ++s_send_failed;
                }
                if (LitChanged)
                {
                    Append(StringFormat("%s  fogueiras acesas enviadas (%u na tabela, %08x %08x %08x) para %zu membros\n",
                        Clock().c_str(), (unsigned)LitCount, Bits[0], Bits[1], Bits[2], Count));
                }
                s_lit_sent_count = LitCount;
                memcpy(s_lit_sent_bits, Bits, sizeof(Bits));
                s_lit_sent_tick = Tick;
            }
        }

        // Each loaded map's three flag categories, when they changed or every
        // two seconds. Only the owner of the world speaks: a guest's copy of
        // the arena is the host's, and echoing it back would be noise.
        if (Mine.Role == kWorldOwner)
        {
            MapFlagsPacket Out[kMaxFlagMaps] = {};
            size_t Ready = 0;
            {
                std::scoped_lock Lock(s_flags_mutex);
                for (size_t i = 0; i < kMaxFlagMaps; ++i)
                {
                    const MapFlags& Mineable = s_flags_mine[i];
                    if (Mineable.Map == 0 || Tick - Mineable.Tick > kLocalStaleMs)
                    {
                        continue;
                    }
                    MapFlags& Last = s_flags_sent[i];
                    const bool Changed = Last.Map != Mineable.Map ||
                        memcmp(Last.Bytes, Mineable.Bytes, sizeof(Last.Bytes)) != 0;
                    if (!Changed && Tick - Last.Tick < kAnnounceEveryMs)
                    {
                        continue;
                    }
                    Last.Map = Mineable.Map;
                    memcpy(Last.Bytes, Mineable.Bytes, sizeof(Last.Bytes));
                    Last.Tick = Tick;
                    MapFlagsPacket& Packet = Out[Ready++];
                    memcpy(Packet.Magic, kMagic, sizeof(kMagic));
                    Packet.Version = kVersion;
                    Packet.Kind = kKindMapFlags;
                    Packet.Role = Mine.Role;
                    Packet.Map = Mineable.Map;
                    memcpy(Packet.Bytes, Mineable.Bytes, sizeof(Packet.Bytes));
                }
            }
            for (size_t j = 0; j < Ready; ++j)
            {
                for (size_t i = 0; i < Count; ++i)
                {
                    SendTo(Net, Others[i], &Out[j], sizeof(Out[j]), kReliable, kChannel) ? ++s_sent : ++s_send_failed;
                }
            }
        }

        // Each loaded map's object state, in chunks, when it changed or every
        // two seconds. The owner of the world alone speaks, as with the flags.
        if (Mine.Role == kWorldOwner)
        {
            MapObjects Send[kMaxFlagMaps];
            size_t Ready = 0;
            {
                std::scoped_lock Lock(s_obj_mutex);
                for (size_t i = 0; i < kMaxFlagMaps; ++i)
                {
                    const MapObjects& Have = s_obj_mine[i];
                    if (Have.Map == 0 || Tick - Have.Tick > kLocalStaleMs)
                    {
                        continue;
                    }
                    MapObjects& Last = s_obj_sent[i];
                    const bool Changed = Last.Map != Have.Map || Last.Count != Have.Count ||
                        memcmp(Last.Index, Have.Index, Have.Count * sizeof(uint16_t)) != 0 ||
                        memcmp(Last.State, Have.State, Have.Count) != 0;
                    if (!Changed && Tick - Last.Tick < kAnnounceEveryMs)
                    {
                        continue;
                    }
                    Last.Map = Have.Map;
                    Last.Count = Have.Count;
                    memcpy(Last.Index, Have.Index, sizeof(Last.Index));
                    memcpy(Last.State, Have.State, sizeof(Last.State));
                    Last.Tick = Tick;
                    Send[Ready++] = Have;
                }
            }
            for (size_t j = 0; j < Ready; ++j)
            {
                for (size_t At = 0; At < Send[j].Count || At == 0; At += kMapObjPerPacket)
                {
                    MapObjPacket Packet = {};
                    memcpy(Packet.Magic, kMagic, sizeof(kMagic));
                    Packet.Version = kVersion;
                    Packet.Kind = kKindMapObjState;
                    Packet.Role = Mine.Role;
                    Packet.Map = Send[j].Map;
                    Packet.Total = Send[j].Count;
                    Packet.First = (uint16_t)At;
                    const size_t Left = Send[j].Count > At ? Send[j].Count - At : 0;
                    Packet.Count = (uint16_t)(Left < kMapObjPerPacket ? Left : kMapObjPerPacket);
                    memcpy(Packet.Index, Send[j].Index + At, Packet.Count * sizeof(uint16_t));
                    memcpy(Packet.State, Send[j].State + At, Packet.Count);
                    for (size_t i = 0; i < Count; ++i)
                    {
                        SendTo(Net, Others[i], &Packet, sizeof(Packet), kReliable, kChannel) ? ++s_sent : ++s_send_failed;
                    }
                    if (Left <= kMapObjPerPacket)
                    {
                        break;
                    }
                }
            }
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
        Now.Count = ReadMembers(Session, Now.Ids, Now.Host);
        Now.Tick = GetTickCount64();
        const uint64_t Self = SelfId();
        if (Remember(Now))
        {
            Append(StringFormat("%s  sessao %p: %zu membros:%s (eu %016llx)\n",
                Clock().c_str(), (void*)Session, Now.Count, DescribeMembers(Now).c_str(),
                (unsigned long long)Self));
        }

        Receive(Net);
        Announce(Net, Now, Self);

        // A guest's events go to the host of this session.
        for (uint8_t Index = 0; Index < DS2_CoopChannel::kGuestEventCount; ++Index)
        {
            if (!s_guest_pending[Index].load())
            {
                continue;
            }
            uint64_t Host = 0;
            for (size_t i = 0; i < Now.Count; ++i)
            {
                if (Now.Host[i] && Now.Ids[i] != Self)
                {
                    Host = Now.Ids[i];
                }
            }
            if (Host == 0 || !s_guest_pending[Index].exchange(false))
            {
                continue;
            }
            Announcement Said = {};
            memcpy(Said.Magic, kMagic, sizeof(kMagic));
            Said.Version = kVersion;
            Said.Kind = (uint8_t)(kKindFirstGuestEvent + Index);
            Said.Role = kWorldOwner;   // passes the sender check; the kind says who it is from
            Said.Map = s_guest_map[Index].load();
            Said.Id = s_guest_id[Index].load();
            const auto SendTo = (Send_p)VirtualAt(Net, kSendSlot);
            const bool Sent = SendTo(Net, Host, &Said, sizeof(Said), kReliable, kChannel);
            Sent ? ++s_sent : ++s_send_failed;
            Append(StringFormat("%s  evento %u enviado ao host %016llx: mapa %08x id %08x%s\n", Clock().c_str(), (unsigned)Said.Kind,
                (unsigned long long)Host, Said.Map, Said.Id, Sent ? "" : " (falhou)"));
        }

        // A guest's answer goes to the host of this session.
        if (s_answer_pending.load())
        {
            uint64_t Host = 0;
            for (size_t i = 0; i < Now.Count; ++i)
            {
                if (Now.Host[i] && Now.Ids[i] != Self)
                {
                    Host = Now.Ids[i];
                }
            }
            if (Host != 0 && s_answer_pending.exchange(false))
            {
                Announcement Reply = {};
                memcpy(Reply.Magic, kMagic, sizeof(kMagic));
                Reply.Version = kVersion;
                Reply.Kind = kKindAnswer;
                Reply.Role = 1;
                Reply.Id = s_answer_vote.load();
                Reply.Type = s_answer_yes.load() ? 1 : 0;
                const auto SendTo = (Send_p)VirtualAt(Net, kSendSlot);
                const bool Sent = SendTo(Net, Host, &Reply, sizeof(Reply), kReliable, kChannel);
                Sent ? ++s_sent : ++s_send_failed;
                Append(StringFormat("%s  resposta a votacao %u enviada ao host %016llx: %s%s\n", Clock().c_str(), Reply.Id,
                    (unsigned long long)Host, Reply.Type != 0 ? "sim" : "nao", Sent ? "" : " (falhou)"));
            }
        }
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
                    (unsigned long long)(Now - Session.Tick), Session.Count, DescribeMembers(Session).c_str());
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
    if (!s_heard.Valid || Now - s_heard.Tick > kHostFreshMs || !IsHostLocked(s_heard.From, Now))
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

size_t DS2_CoopChannel::GuestCount()
{
#ifdef _WIN32
    const uint64_t Self = s_self.load();
    std::scoped_lock Lock(s_net_mutex);
    const ULONGLONG Now = GetTickCount64();
    size_t Guests = 0;
    for (const Members& Session : s_sessions)
    {
        if (Session.Session == 0 || Now - Session.Tick > kMembersFreshMs)
        {
            continue;
        }
        bool SelfIsHost = false;
        for (size_t i = 0; i < Session.Count; ++i)
        {
            SelfIsHost = SelfIsHost || (Session.Ids[i] == Self && Session.Host[i]);
        }
        if (SelfIsHost && Session.Count > 1)
        {
            Guests += Session.Count - 1;
        }
    }
    return Guests;
#else
    return 0;
#endif
}

void DS2_CoopChannel::SendGuestAnswer(uint32_t Vote, bool Yes)
{
#ifdef _WIN32
    s_answer_vote.store(Vote);
    s_answer_yes.store(Yes);
    s_answer_pending.store(true);
#endif
}

void DS2_CoopChannel::GuestAnswers(uint32_t Vote, size_t& Yes, size_t& No)
{
    Yes = 0;
    No = 0;
#ifdef _WIN32
    std::scoped_lock Lock(s_net_mutex);
    for (const Answer& Entry : s_answers)
    {
        if (Entry.From != 0 && Entry.Vote == Vote)
        {
            Entry.Yes ? ++Yes : ++No;
        }
    }
#endif
}

void DS2_CoopChannel::SendGuestEvent(GuestEvent Event, uint32_t Map, uint32_t Id)
{
#ifdef _WIN32
    s_guest_map[(size_t)Event].store(Map);
    s_guest_id[(size_t)Event].store(Id);
    s_guest_pending[(size_t)Event].store(true);
#endif
}

bool DS2_CoopChannel::TakeGuestEvent(GuestEvent Event, Bonfire& Out)
{
#ifdef _WIN32
    std::scoped_lock Lock(s_net_mutex);
    const size_t Index = (size_t)Event;
    const HeardGuestEvent& Entry = s_heard_guest[Index];
    if (Entry.Count == s_taken_guest[Index])
    {
        return false;
    }
    s_taken_guest[Index] = Entry.Count;
    const ULONGLONG Now = GetTickCount64();
    if (Now - Entry.Tick > kHostFreshMs)
    {
        return false;
    }
    Out.Map = Entry.Map;
    Out.Type = 0;
    Out.Id = Entry.Id;
    Out.From = Entry.From;
    Out.AgeMs = Now - Entry.Tick;
    return true;
#else
    return false;
#endif
}

void DS2_CoopChannel::PublishMapFlags(uint32_t Map, const uint8_t Bytes[DS2_CoopChannel::kMapFlagBytes])
{
#ifdef _WIN32
    if (Map == 0 || Bytes == nullptr)
    {
        return;
    }
    const ULONGLONG Now = GetTickCount64();
    std::scoped_lock Lock(s_flags_mutex);
    MapFlags* Slot = nullptr;
    MapFlags* Oldest = &s_flags_mine[0];
    for (MapFlags& Entry : s_flags_mine)
    {
        if (Entry.Map == Map)
        {
            Slot = &Entry;
            break;
        }
        if (Entry.Map == 0 || Entry.Tick < Oldest->Tick)
        {
            Oldest = &Entry;
        }
    }
    if (Slot == nullptr)
    {
        Slot = Oldest;
        Slot->Map = Map;
    }
    memcpy(Slot->Bytes, Bytes, DS2_CoopChannel::kMapFlagBytes);
    Slot->Tick = Now;
#else
    (void)Map;
    (void)Bytes;
#endif
}

bool DS2_CoopChannel::HostMapFlags(uint32_t Map, uint8_t Bytes[DS2_CoopChannel::kMapFlagBytes], uint64_t& AgeMs)
{
#ifdef _WIN32
    AgeMs = 0;
    if (Map == 0 || Bytes == nullptr)
    {
        return false;
    }
    const ULONGLONG Now = GetTickCount64();
    std::scoped_lock Lock(s_flags_mutex);
    for (const MapFlags& Entry : s_flags_heard)
    {
        if (Entry.Map == Map && Entry.Tick != 0)
        {
            memcpy(Bytes, Entry.Bytes, DS2_CoopChannel::kMapFlagBytes);
            AgeMs = Now - Entry.Tick;
            return true;
        }
    }
    return false;
#else
    (void)Map;
    (void)Bytes;
    AgeMs = 0;
    return false;
#endif
}

void DS2_CoopChannel::PublishMapObjects(uint32_t Map, const uint16_t* Index, const uint8_t* State, size_t Count)
{
#ifdef _WIN32
    if (Map == 0 || Index == nullptr || State == nullptr || Count > DS2_CoopChannel::kMapObjMax)
    {
        return;
    }
    const ULONGLONG Now = GetTickCount64();
    std::scoped_lock Lock(s_obj_mutex);
    MapObjects* Slot = nullptr;
    MapObjects* Oldest = &s_obj_mine[0];
    for (MapObjects& Entry : s_obj_mine)
    {
        if (Entry.Map == Map)
        {
            Slot = &Entry;
            break;
        }
        if (Entry.Map == 0 || Entry.Tick < Oldest->Tick)
        {
            Oldest = &Entry;
        }
    }
    if (Slot == nullptr)
    {
        Slot = Oldest;
        Slot->Map = Map;
    }
    Slot->Count = (uint16_t)Count;
    memcpy(Slot->Index, Index, Count * sizeof(uint16_t));
    memcpy(Slot->State, State, Count);
    Slot->Tick = Now;
#else
    (void)Map; (void)Index; (void)State; (void)Count;
#endif
}

size_t DS2_CoopChannel::HostMapObjects(uint32_t Map, uint16_t* Index, uint8_t* State, size_t Room, uint64_t& AgeMs)
{
#ifdef _WIN32
    AgeMs = 0;
    if (Map == 0 || Index == nullptr || State == nullptr)
    {
        return 0;
    }
    const ULONGLONG Now = GetTickCount64();
    std::scoped_lock Lock(s_obj_mutex);
    for (const MapObjects& Entry : s_obj_heard)
    {
        if (Entry.Map != Map || Entry.Tick == 0 || Entry.Count == 0)
        {
            continue;
        }
        // Only a list every chunk of which arrived.
        const uint32_t Wanted = (1u << ((Entry.Count + kMapObjPerPacket - 1) / kMapObjPerPacket)) - 1u;
        if ((Entry.Chunks & Wanted) != Wanted)
        {
            return 0;
        }
        const size_t Give = Entry.Count < Room ? Entry.Count : Room;
        memcpy(Index, Entry.Index, Give * sizeof(uint16_t));
        memcpy(State, Entry.State, Give);
        AgeMs = Now - Entry.Tick;
        return Give;
    }
    return 0;
#else
    (void)Map; (void)Index; (void)State; (void)Room;
    AgeMs = 0;
    return 0;
#endif
}

void DS2_CoopChannel::PublishLit(uint8_t Count, const uint32_t Bits[3])
{
#ifdef _WIN32
    s_lit_count.store(Count);
    for (size_t i = 0; i < 3; ++i)
    {
        s_lit_bits[i].store(Bits[i]);
    }
    s_lit_tick.store(GetTickCount64());
#else
    (void)Count;
    (void)Bits;
#endif
}

bool DS2_CoopChannel::HostLit(uint8_t& Count, uint32_t Bits[3], uint64_t& AgeMs)
{
#ifdef _WIN32
    std::scoped_lock Lock(s_net_mutex);
    if (!s_lit.Valid)
    {
        return false;
    }
    const ULONGLONG Now = GetTickCount64();
    if (Now - s_lit.Tick > kHostFreshMs)
    {
        return false;
    }
    Count = s_lit.Count;
    Bits[0] = s_lit.Bits[0];
    Bits[1] = s_lit.Bits[1];
    Bits[2] = s_lit.Bits[2];
    AgeMs = Now - s_lit.Tick;
    return true;
#else
    (void)Count;
    (void)Bits;
    (void)AgeMs;
    return false;
#endif
}

uint64_t DS2_CoopChannel::SelfSteamId()
{
#ifdef _WIN32
    return s_self.load();
#else
    return 0;
#endif
}

void DS2_CoopChannel::SendHostEvent(HostEvent Event, uint32_t Map, uint32_t Id, int32_t Type)
{
#ifdef _WIN32
    s_event_map[(size_t)Event].store(Map);
    s_event_id[(size_t)Event].store(Id);
    s_event_type[(size_t)Event].store(Type);
    const ULONGLONG Now = GetTickCount64();
    if (Now - s_events_tick.load() > kEventFreshMs)
    {
        s_events_pending.store(0);
    }
    s_events_tick.store(Now);
    s_events_pending.fetch_or(1u << (uint32_t)Event);
#endif
}

bool DS2_CoopChannel::TakeHostEvent(HostEvent Event, Bonfire& Out)
{
#ifdef _WIN32
    std::scoped_lock Lock(s_net_mutex);
    const size_t Index = (size_t)Event;
    const HeardEvent& Entry = s_heard_events[Index];
    if (Entry.Count == s_taken_events[Index])
    {
        return false;
    }
    s_taken_events[Index] = Entry.Count;
    const ULONGLONG Now = GetTickCount64();
    if (Now - Entry.Tick > kHostFreshMs || !IsHostLocked(Entry.From, Now))
    {
        return false;
    }
    Out.Map = Entry.Last.Map;
    Out.Type = Entry.Last.Type;
    Out.Id = Entry.Last.Id;
    Out.From = Entry.From;
    Out.AgeMs = Now - Entry.Tick;
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
