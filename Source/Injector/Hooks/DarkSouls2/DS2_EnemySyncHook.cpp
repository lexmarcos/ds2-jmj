/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_EnemySyncHook.h"
#include "Injector/Injector/Injector.h"
#include "Shared/Core/Utils/Logging.h"
#include "Shared/Core/Utils/Strings.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include "ThirdParty/detours/src/detours.h"
#endif

namespace
{
#if defined(_WIN32) && defined(_M_X64)

    // Version 1.03 Calibrations 2.02. Every prologue below was read out of the
    // shipped executable on 20/09 and is verified before anything is hooked.
    //
    //   +0x517080  unbind   48 89 5c 24 08 57 48 83 ec 20
    //   +0x517040  arm      the same opening, then movb $1,0x74(%rbx)
    //                       and movb $0,0x198(%rbx)
    //   +0x516370  gate     c6 81 98 01 00 00 01 c3   -- +0x198 = 1; ret
    //   +0x416ac0  release  48 89 54 24 10 57 41 55 48 83 ec 48, then
    //                       call FUN_1403bb3d0 and `cmp $0x29,%eax; ja`
    constexpr size_t kUnbindOffset = 0x517080;
    constexpr uint8_t kUnbindPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20 };
    constexpr size_t kArmOffset = 0x517040;
    constexpr uint8_t kArmPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20 };
    constexpr size_t kGateOffset = 0x516370;
    constexpr uint8_t kGatePrologue[] = { 0xc6, 0x81, 0x98, 0x01, 0x00, 0x00, 0x01, 0xc3 };
    constexpr size_t kMapReleaseOffset = 0x416ac0;
    constexpr uint8_t kMapReleasePrologue[] = { 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x41, 0x55, 0x48, 0x83, 0xec, 0x48 };

    constexpr size_t kNetSvrGlobal = 0x1616cf8;
    constexpr size_t kNetEnemyField = 0x28;
    constexpr size_t kNetEnemyVftable = 0x10fb580;
    constexpr size_t kSyncState = 0x08;
    constexpr size_t kSyncCount = 0x0c;
    constexpr size_t kSyncBoundMap = 0x18;

    // What the release's second argument actually is, settled by the log on
    // 20/09: not a map id but a pointer. FUN_1403bb3d0, the slot lookup the
    // release opens with, is three instructions -
    //
    //     mov 0x8(%rcx),%rax    ; -> the backread owner
    //     mov 0xc(%rax),%eax    ; -> its area slot
    //     ret
    //
    // - and `+0x08` / `+0x0c` of that owner are exactly DS2_BackreadHook's own
    // kOwnerMap and kOwnerIndexField. So the map id is two reads away, and the
    // `cmp $0x29; ja` the release then does is mirrored on the same index.
    constexpr size_t kAreaOwnerField = 0x08;
    constexpr size_t kOwnerMap = 0x08;
    constexpr size_t kOwnerIndex = 0x0c;
    constexpr uint32_t kMaxSlot = 0x29;

    using Simple_p = void(*)(void* Manager);
    using MapRelease_p = void*(*)(void* ChrManager, void* Area);

    Simple_p s_unbind = nullptr;
    Simple_p s_arm = nullptr;
    Simple_p s_gate = nullptr;
    MapRelease_p s_original_release = nullptr;
    uintptr_t s_base = 0;
    bool s_installed = false;

    std::filesystem::path s_log_path;
    std::mutex s_log_mutex;
    std::atomic<uint32_t> s_seen{ 0 };
    std::atomic<uint32_t> s_unbound{ 0 };

    std::string Clock()
    {
        SYSTEMTIME Now;
        GetLocalTime(&Now);
        return StringFormat("%02u:%02u:%02u.%03u", Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds);
    }

    void Append(const std::string& Text)
    {
        std::scoped_lock Lock(s_log_mutex);
        std::ofstream Stream(s_log_path, std::ios::app);
        if (Stream)
        {
            Stream << Text;
        }
    }

    bool Read(uintptr_t At, void* Out, size_t Length)
    {
        __try
        {
            memcpy(Out, (const void*)At, Length);
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }

    // The manager, or null. The vftable is the whole reason to believe it:
    // the field is reached through two dereferences and the object it names
    // does not exist before a session.
    void* Manager()
    {
        uintptr_t Global = 0, Sync = 0, Vftable = 0;
        if (s_base == 0 ||
            !Read(s_base + kNetSvrGlobal, &Global, sizeof(Global)) || Global == 0 ||
            !Read(Global + kNetEnemyField, &Sync, sizeof(Sync)) || Sync == 0 ||
            !Read(Sync, &Vftable, sizeof(Vftable)) || Vftable != s_base + kNetEnemyVftable)
        {
            return nullptr;
        }
        return (void*)Sync;
    }

    uint32_t Field(size_t At)
    {
        void* Sync = Manager();
        uint32_t Value = 0;
        return Sync != nullptr && Read((uintptr_t)Sync + At, &Value, sizeof(Value)) ? Value : 0;
    }

    // Runs on the game's thread, at the entry of the per-map character
    // release. This is the last moment the records are still whole.
    void* MapReleaseHook(void* ChrManager, void* Area)
    {
        const uint32_t Bound = DS2_EnemySync::BoundMap();
        uintptr_t Owner = 0;
        uint32_t Asked = 0, Slot = 0xffffffff;
        if (Area != nullptr && Read((uintptr_t)Area + kAreaOwnerField, &Owner, sizeof(Owner)) && Owner != 0)
        {
            Read(Owner + kOwnerMap, &Asked, sizeof(Asked));
            Read(Owner + kOwnerIndex, &Slot, sizeof(Slot));
        }

        if (s_seen.fetch_add(1) < 20)
        {
            Append(StringFormat("%s  soltando personagens do mapa %08x (slot %u), ligado %08x, estado %u, %u registros\n",
                Clock().c_str(), Asked, Slot, Bound,
                DS2_EnemySync::State(), DS2_EnemySync::Count()));
        }

        // The release's own early-out, mirrored: a slot past 0x29 is not an
        // area and the game does nothing with it, so neither do we.
        if (Bound != 0 && Asked == Bound && Slot <= kMaxSlot)
        {
            if (DS2_EnemySync::Unbind("o mapa dos registros esta sendo derrubado"))
            {
                s_unbound.fetch_add(1);
            }
        }
        return s_original_release(ChrManager, Area);
    }

    bool Matches(uintptr_t At, const uint8_t* Expected, size_t Length)
    {
        uint8_t Found[24] = {};
        if (Length > sizeof(Found) || !Read(At, Found, Length))
        {
            return false;
        }
        return memcmp(Found, Expected, Length) == 0;
    }

#endif
}

namespace DS2_EnemySync
{
    uint32_t BoundMap()
    {
#if defined(_WIN32) && defined(_M_X64)
        return Field(kSyncState) == 0 ? 0 : Field(kSyncBoundMap);
#else
        return 0;
#endif
    }

    uint32_t State()
    {
#if defined(_WIN32) && defined(_M_X64)
        return Field(kSyncState);
#else
        return 0;
#endif
    }

    uint32_t Count()
    {
#if defined(_WIN32) && defined(_M_X64)
        return Field(kSyncCount);
#else
        return 0;
#endif
    }

    bool Unbind(const char* Why)
    {
#if defined(_WIN32) && defined(_M_X64)
        void* Sync = Manager();
        const uint32_t State = Field(kSyncState);
        // The guard is the state, not the count: a bound table with no
        // records still has to be told, and the count is what the plan warns
        // against testing.
        if (Sync == nullptr || s_unbind == nullptr || State == 0)
        {
            return false;
        }
        const uint32_t Map = Field(kSyncBoundMap);
        const uint32_t Records = Field(kSyncCount);
        s_unbind(Sync);
        Append(StringFormat("%s  sync desligado do mapa %08x (estado %u, %u registros): %s; agora estado %u\n",
            Clock().c_str(), Map, State, Records, Why == nullptr ? "" : Why, Field(kSyncState)));
        return true;
#else
        (void)Why;
        return false;
#endif
    }

    bool Arm(const char* Why)
    {
#if defined(_WIN32) && defined(_M_X64)
        void* Sync = Manager();
        if (Sync == nullptr || s_arm == nullptr)
        {
            return false;
        }
        s_arm(Sync);
        Append(StringFormat("%s  sync armado (%s); estado %u, mapa %08x, %u registros\n",
            Clock().c_str(), Why == nullptr ? "" : Why, Field(kSyncState),
            Field(kSyncBoundMap), Field(kSyncCount)));
        return true;
#else
        (void)Why;
        return false;
#endif
    }

    bool OpenGuestGate()
    {
#if defined(_WIN32) && defined(_M_X64)
        void* Sync = Manager();
        if (Sync == nullptr || s_gate == nullptr)
        {
            return false;
        }
        s_gate(Sync);
        return true;
#else
        return false;
#endif
    }
}

bool DS2_EnemySyncHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    const uintptr_t Base = (uintptr_t)GetModuleHandleA(nullptr);
    if (Base == 0)
    {
        Error("[DS2EnemySync] no module base.");
        return false;
    }
    s_base = Base;

    if (!Matches(Base + kUnbindOffset, kUnbindPrologue, sizeof(kUnbindPrologue)) ||
        !Matches(Base + kArmOffset, kArmPrologue, sizeof(kArmPrologue)) ||
        !Matches(Base + kGateOffset, kGatePrologue, sizeof(kGatePrologue)) ||
        !Matches(Base + kMapReleaseOffset, kMapReleasePrologue, sizeof(kMapReleasePrologue)))
    {
        Error("[DS2EnemySync] the executable is not the version these offsets were measured against; nothing hooked.");
        return false;
    }

    s_log_path = injector.GetDllPath() / "DS2_EnemySync.log";
    s_unbind = (Simple_p)(Base + kUnbindOffset);
    s_arm = (Simple_p)(Base + kArmOffset);
    s_gate = (Simple_p)(Base + kGateOffset);
    s_original_release = (MapRelease_p)(Base + kMapReleaseOffset);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_release, MapReleaseHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        Error("[DS2EnemySync] could not hook the per-map character release.");
        return false;
    }

    s_installed = true;
    Append(StringFormat("%s  === ds2os sync de inimigos: desligo antes do mapa dos registros cair ===\n", Clock().c_str()));
    Log("[DS2EnemySync] the enemy table is unbound before its own map is torn down.");
#endif
    return true;
}

void DS2_EnemySyncHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    if (s_installed)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_release, MapReleaseHook);
        DetourTransactionCommit();
        s_installed = false;
    }
#endif
}

const char* DS2_EnemySyncHook::GetName()
{
    return "DS2 Enemy Sync";
}
