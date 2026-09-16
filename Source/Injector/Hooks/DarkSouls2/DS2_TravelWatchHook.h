/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

#include <cstdint>

/// Who destroys what in the seconds after a travel (M8).
///
/// The guest's game closes 1 to 3 seconds after arriving from a travel without
/// a warp, always on memory of the map heap that was freed while something
/// still pointed at it: the two per-frame jobs of a MapModelComponent
/// (FUN_1403f4f60, the pre-draw, and FUN_1403f4f10, which reads the model
/// instance at `comp+0x40`), the component list an entity keeps
/// (FUN_1401cbf20), and the registry by id (FUN_14040d2e0). Solo it never
/// happens; it needs a session, which is what makes it expensive to measure -
/// a crash there costs the guest ten illegal-disconnect points.
///
/// Until 16/09 this file said the guest's remaining crash, at +0x3f4f2b, was
/// inside FUN_1403f4f60. It is not: the PE exception directory puts that
/// address in FUN_1403f4f10 (0x3f4f10..0x3f4f53), a different function, which
/// is why the guard on the pre-draw never caught it.
///
/// So the window is written down instead of waited for: while it is open, the
/// four doors a map entity leaves by say who went and from where, into
/// `DS2_TravelWatch.log`. A travel that does not crash records just as much as
/// one that does, and costs nothing.
///
///   FUN_1403b9ea0  ~MapEntity(entity, flags): the entity itself
///   FUN_1403f6300  MapModelComponent release(comp, flag): the +0xc8 the
///                  pre-draw crash reads is let go here
///   FUN_1401c5dd0(obj, map index) and FUN_140247650(obj, map index): the two
///                  places that tear a whole map's objects down
class DS2_TravelWatchHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};

namespace DS2_TravelWatch
{
    /// Write down every destruction for this many milliseconds. Called from
    /// the game's thread when a travel starts and again when it lands; a new
    /// call extends the window. `Why` goes into the log as the reason.
    void Open(uint32_t Milliseconds, const char* Why);

    /// For this many milliseconds, every model component that comes past the
    /// per-frame hooks is detached from its entity when the two are in
    /// different heaps, through the game's own FUN_14040cea0 and without
    /// freeing anything.
    ///
    /// This is the fix for the corruption, not a net for it. A map part takes
    /// its whole heap away at once, with no destructor and no detach, so a
    /// component of the origin map still attached to a character that crossed
    /// without a load becomes a dead node in the list at entity+0x18 - and any
    /// of the 74 copies of GetComponent<T> that walks that list then reads a
    /// freed vftable. Called from the game's thread just before the travel
    /// lets the origin map go, which is the last moment both ends are alive.
    void DetachCrossHeap(uint32_t Milliseconds, const char* Why);
}
