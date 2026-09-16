/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Injector/Hooks/DarkSouls2/DS2_TravelWatchHook.h"
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

    // Version 1.03 Calibrations 2.02.
    constexpr size_t kEntityDtorOffset = 0x3b9ea0;
    constexpr uint8_t kEntityDtorPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x05 };
    constexpr size_t kComponentFreeOffset = 0x3f6300;
    constexpr uint8_t kComponentFreePrologue[] = { 0x40, 0x53, 0x57, 0x41, 0x55, 0x41, 0x57 };
    constexpr size_t kMapObjectsOffset = 0x1c5dd0;
    constexpr uint8_t kMapObjectsPrologue[] = { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56 };
    constexpr size_t kMapCharactersOffset = 0x247650;
    constexpr uint8_t kMapCharactersPrologue[] = { 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };

    // MapEntity: the owner of the map at +0x28 (its map id at +0x08), the kind
    // at +0xa2. A MapModelComponent carries its entity at +0x08 and the id of
    // the component it registered at +0xc0.
    constexpr size_t kEntityOwner = 0x28;
    constexpr size_t kOwnerMap = 0x08;
    constexpr size_t kEntityKind = 0xa2;
    constexpr size_t kComponentEntity = 0x08;
    constexpr size_t kComponentRegistered = 0xc8;
    constexpr size_t kComponentId = 0xc0;

    constexpr size_t kFrames = 6;
    constexpr uint64_t kMaxLines = 4000;   // one travel is a few hundred

    using EntityDtor_p = void*(*)(void* Entity, uint32_t Flags);
    using ComponentFree_p = void(*)(void* Component, char Flag);
    using ByMap_p = void(*)(void* Object, int32_t MapIndex);

    EntityDtor_p s_original_entity = nullptr;
    ComponentFree_p s_original_component = nullptr;
    ByMap_p s_original_objects = nullptr;
    ByMap_p s_original_characters = nullptr;

    uintptr_t s_base = 0;
    std::filesystem::path s_log_path;
    std::mutex s_log_mutex;
    std::atomic<ULONGLONG> s_open_until{ 0 };
    std::atomic<uint64_t> s_lines{ 0 };
    std::atomic<bool> s_ready{ false };

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

    bool WindowOpen()
    {
        const ULONGLONG Until = s_open_until.load();
        return Until != 0 && GetTickCount64() < Until && s_lines.load() < kMaxLines;
    }

    // The return addresses, as module offsets, so a line can be read against
    // the binary the way the crash log's are.
    std::string Stack()
    {
        void* Frames[kFrames] = {};
        const USHORT Count = RtlCaptureStackBackTrace(1, (ULONG)kFrames, Frames, nullptr);
        std::string Out;
        for (USHORT i = 0; i < Count; ++i)
        {
            const uintptr_t At = (uintptr_t)Frames[i];
            if (At >= s_base && At < s_base + 0x2000000)
            {
                Out += StringFormat(" +0x%zx", (size_t)(At - s_base));
            }
        }
        return Out;
    }

    // Reading a dying object can fault; a watcher must never be the thing that
    // closes the game.
    bool Peek(uintptr_t At, void* Out, size_t Length)
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

    // The map an entity belongs to, and its kind. 0xffffffff when unreadable.
    void DescribeEntity(uintptr_t Entity, uint32_t& Map, uint16_t& Kind)
    {
        Map = 0xffffffff;
        Kind = 0xffff;
        uintptr_t Owner = 0;
        if (Entity != 0 && Peek(Entity + kEntityOwner, &Owner, sizeof(Owner)) && Owner != 0)
        {
            uint32_t Id = 0;
            if (Peek(Owner + kOwnerMap, &Id, sizeof(Id)))
            {
                Map = Id;
            }
        }
        uint16_t Value = 0;
        if (Entity != 0 && Peek(Entity + kEntityKind, &Value, sizeof(Value)))
        {
            Kind = Value;
        }
    }

    void Note(const std::string& Text)
    {
        s_lines.fetch_add(1);
        Append(StringFormat("%s  t%lu  %s%s\n", Clock().c_str(), GetCurrentThreadId(), Text.c_str(), Stack().c_str()));
    }

    void* EntityDtorHook(void* Entity, uint32_t Flags)
    {
        if (WindowOpen())
        {
            uint32_t Map = 0;
            uint16_t Kind = 0;
            DescribeEntity((uintptr_t)Entity, Map, Kind);
            Note(StringFormat("MapEntity %p destruida (mapa %08x, tipo %u, flags %u)", Entity, Map, (unsigned)Kind, Flags));
        }
        return s_original_entity(Entity, Flags);
    }

    void ComponentFreeHook(void* Component, char Flag)
    {
        if (WindowOpen())
        {
            uintptr_t Entity = 0, Registered = 0;
            uint32_t Map = 0xffffffff, Id = 0;
            uint16_t Kind = 0;
            if (Peek((uintptr_t)Component + kComponentEntity, &Entity, sizeof(Entity)))
            {
                DescribeEntity(Entity, Map, Kind);
            }
            Peek((uintptr_t)Component + kComponentRegistered, &Registered, sizeof(Registered));
            Peek((uintptr_t)Component + kComponentId, &Id, sizeof(Id));
            Note(StringFormat("MapModelComponent %p solto (entidade %p, mapa %08x, tipo %u, +0xc8 %p, id %u, flag %d)",
                Component, (void*)Entity, Map, (unsigned)Kind, (void*)Registered, Id, (int)Flag));
        }
        s_original_component(Component, Flag);
    }

    void MapObjectsHook(void* Object, int32_t MapIndex)
    {
        if (WindowOpen())
        {
            Note(StringFormat("objetos do mapa de indice %d desmontados (%p)", MapIndex, Object));
        }
        s_original_objects(Object, MapIndex);
    }

    void MapCharactersHook(void* Object, int32_t MapIndex)
    {
        if (WindowOpen())
        {
            Note(StringFormat("personagens do mapa de indice %d desmontados (%p)", MapIndex, Object));
        }
        s_original_characters(Object, MapIndex);
    }

    bool Matches(uintptr_t Address, const uint8_t* Expected, size_t Length)
    {
        return memcmp((const void*)Address, Expected, Length) == 0;
    }

#endif
}

bool DS2_TravelWatchHook::Install(Injector& injector)
{
#if defined(_WIN32) && defined(_M_X64)
    const uintptr_t Base = (uintptr_t)injector.GetBaseAddress();
    if (!Matches(Base + kEntityDtorOffset, kEntityDtorPrologue, sizeof(kEntityDtorPrologue)) ||
        !Matches(Base + kComponentFreeOffset, kComponentFreePrologue, sizeof(kComponentFreePrologue)) ||
        !Matches(Base + kMapObjectsOffset, kMapObjectsPrologue, sizeof(kMapObjectsPrologue)) ||
        !Matches(Base + kMapCharactersOffset, kMapCharactersPrologue, sizeof(kMapCharactersPrologue)))
    {
        Error("[DS2TravelWatch] o codigo de uma das quatro portas nao e o esperado; nao instalado");
        return false;
    }

    s_base = Base;
    s_log_path = injector.GetDllPath() / "DS2_TravelWatch.log";
    s_original_entity = (EntityDtor_p)(Base + kEntityDtorOffset);
    s_original_component = (ComponentFree_p)(Base + kComponentFreeOffset);
    s_original_objects = (ByMap_p)(Base + kMapObjectsOffset);
    s_original_characters = (ByMap_p)(Base + kMapCharactersOffset);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original_entity, EntityDtorHook);
    DetourAttach(&(PVOID&)s_original_component, ComponentFreeHook);
    DetourAttach(&(PVOID&)s_original_objects, MapObjectsHook);
    DetourAttach(&(PVOID&)s_original_characters, MapCharactersHook);
    if (DetourTransactionCommit() != NO_ERROR)
    {
        s_original_entity = nullptr;
        Error("[DS2TravelWatch] nao consegui instalar os detours");
        return false;
    }

    s_ready.store(true);
    Append(StringFormat("%s  === ds2os vigia da viagem: quem destroi o que depois de chegar ===\n", Clock().c_str()));
    Log("[DS2TravelWatch] pronto; so escreve enquanto uma viagem estiver aberta");
#endif
    return true;
}

void DS2_TravelWatchHook::Uninstall()
{
#if defined(_WIN32) && defined(_M_X64)
    s_ready.store(false);
    s_open_until.store(0);
    if (s_original_entity != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)s_original_entity, EntityDtorHook);
        DetourDetach(&(PVOID&)s_original_component, ComponentFreeHook);
        DetourDetach(&(PVOID&)s_original_objects, MapObjectsHook);
        DetourDetach(&(PVOID&)s_original_characters, MapCharactersHook);
        DetourTransactionCommit();
        s_original_entity = nullptr;
    }
#endif
}

const char* DS2_TravelWatchHook::GetName()
{
    return "DS2 Travel Watch";
}

namespace DS2_TravelWatch
{
    void Open(uint32_t Milliseconds, const char* Why)
    {
#if defined(_WIN32) && defined(_M_X64)
        if (!s_ready.load())
        {
            return;
        }
        s_lines.store(0);
        s_open_until.store(GetTickCount64() + Milliseconds);
        Append(StringFormat("%s  --- janela aberta por %u ms: %s ---\n", Clock().c_str(), Milliseconds,
            Why != nullptr ? Why : ""));
#else
        (void)Milliseconds;
        (void)Why;
#endif
    }
}
