/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

/// Opens every area to multiplayer by rewriting NETWORK_AREA_PARAM.
///
/// Dark Souls II keeps one row per online area in that param, keyed by the
/// area id the server also uses, and each row ends in a bitmask of what
/// multiplayer the area permits. Majula carries 4 and Heide carries 7, while
/// most of the game carries 63; Things Betwixt, which allows nothing, carries
/// 0. Raising every row to the full mask is what lets a summon sign be placed
/// in a hub area.
///
/// The param is patched once, as early as it can be found, because the value
/// appears to be consulted when an area loads rather than when the item is
/// used.
class DS2_UnlockAreaMultiPlayHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
