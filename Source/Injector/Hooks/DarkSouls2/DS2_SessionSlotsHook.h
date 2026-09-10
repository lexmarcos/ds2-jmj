/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

/// Raises the number of players a session can hold from six to twelve.
///
/// The six is not a constant. Two objects each keep their per-player records
/// in a fixed array inside themselves, sized for the host plus five, and both
/// arrays have live fields behind them. So both allocations grow and both
/// arrays move past the old end of their object, which leaves every field
/// already in place at the offset it has. Nothing shifts; only the addresses
/// of the arrays themselves, the counts, and the bounds.
///
/// The addresses are in DS2_SessionSlotPatchTable.h, generated rather than
/// typed. Every one rewrites a displacement or an immediate in place, so no
/// instruction changes length.
///
/// Install verifies all of them before writing any. One wrong byte means the
/// build has moved and the whole table is stale, in which case nothing is
/// written at all: a half-applied layout change would corrupt a live session
/// rather than merely refuse one.
class DS2_SessionSlotsHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
