/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

/// Clears the two counters that block a multiplayer session in Majula.
///
/// The game keeps a pair of saturating counters describing "multiplay is
/// blocked here". A per-frame updater raises them when a condition goes false
/// and lowers them when it goes true, and three separate places refuse to take
/// part when they read non-zero: the summon push handler reads the sign
/// owner's, the guest's join controller reads the guest's, and the host's
/// accept controller reads the host's. Each produces a different message, which
/// is why clearing one by hand only moved the failure along.
///
/// This patches the condition itself rather than the counters, so the game's
/// own edge logic lowers them. The object holding the edge flag can be rebuilt
/// when a client loads into another world, which would re-raise a counter
/// written to directly, mid-session.
class DS2_UnblockMultiPlayHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
