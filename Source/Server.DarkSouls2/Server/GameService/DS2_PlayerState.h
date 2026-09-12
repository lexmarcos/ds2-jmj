/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include <string>

#include "Protobuf/DS2_Protobufs.h"
#include "Server/GameService/Utils/DS2_GameIds.h"

#include "Server/GameService/PlayerState.h"
#include "Shared/Core/Utils/Strings.h"

#define DEFINE_FIELD(type, name, default_value)                             \
    private: type name = default_value;                                     \
    public:                                                                 \
        const type& Get##name() const { return name; }                      \
        type& Get##name##_Mutable() { return name; }                        \
        void Set##name(const type& input) { name = input; Mutated(); }   

struct DS2_PlayerState : public PlayerState
{
public:

    // Current online matching area the player is in.
    DEFINE_FIELD(DS2_OnlineAreaId, CurrentArea, DS2_OnlineAreaId::None)
    
    // Similar to currentarea but more finely set, will be set to 0 if no online
    // activity can happen in the area.
    DEFINE_FIELD(int, CurrentOnlineActivityArea, 0)    

    // The cell within the area, as the client reports it. Needed to tell a
    // player something is happening where they actually stand.
    DEFINE_FIELD(uint32_t, CurrentCellId, 0)
    
    // What type of visitor the player can currently be summoned as.
    DEFINE_FIELD(DS2_Frpg2RequestMessage::VisitorType, VisitorPool, DS2_Frpg2RequestMessage::VisitorType::VisitorType_None)
    
    // Information the player sends and periodically patches with 
    // RequestUpdatePlayerStatus requests.
    DEFINE_FIELD(DS2_Frpg2PlayerData::AllStatus, PlayerStatus, DS2_Frpg2PlayerData::AllStatus())

    virtual uint32_t GetCurrentAreaId() override
    {
        return (uint32_t)CurrentArea;
    }

    virtual bool IsInGame() override
    {
        return GetPlayerStatus().has_player_status() &&
               GetPlayerId() != 0;
    }

    virtual size_t GetSoulCount() override
    {
        return 0;
    }

    virtual size_t GetSoulMemory() override
    {
        auto Status = GetPlayerStatus().player_status();
        return Status.soul_memory();
    }

    virtual size_t GetDeathCount() override
    {
        return 0;
    }

    virtual size_t GetMultiplayerSessionCount() override
    {
        return 0;
    }

    virtual double GetPlayTime() override
    {
        auto Status = GetPlayerStatus().player_status();
        return Status.play_time_seconds();
    }

    virtual std::string GetConvenantStatusDescription() override
    {
        return "";
    }

    // What the client tells us about itself that has no field of its own.
    // Written out rather than left blank because the harness reads it, and the
    // one that matters is the effigy count: a hollow character cannot be
    // summoned as a white phantom, and a whole afternoon went into a staging
    // that could never have worked because nobody could see that.
    virtual std::string GetStatusDescription() override
    {
        if (!GetPlayerStatus().has_player_status())
        {
            return "";
        }

        auto Status = GetPlayerStatus().player_status();
        return StringFormat("efigies %u, arquetipo %u, fogueira %u",
            Status.human_effigy_burnt(), Status.archetype(), Status.sitting_at_bonfire());
    }

};

#undef DEFINE_FIELD