/*
 * Dark Souls 3 - Open Server
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#pragma once

#include "Injector/Hooks/Hook.h"

/// Lets a hollow host see and touch summon signs.
///
/// A hollow character can place a white sign — measured 14/09, the server logs
/// `Sign created` for it — but a hollow host never gets the "Touch Summon Sign"
/// prompt, even with the sign already delivered to its client. The seamless
/// co-op brief says the effigy stops gating multiplayer, so the sign filter
/// is told the local player is human; hollowing itself (the level, the
/// maximum HP, the look) is untouched.
class DS2_HollowSummonHook : public Hook
{
public:
    virtual bool Install(Injector& injector) override;
    virtual void Uninstall() override;
    virtual const char* GetName() override;
};
