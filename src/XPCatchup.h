/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>
 */

#ifndef AZEROTHCORE_XPCATCHUP_H
#define AZEROTHCORE_XPCATCHUP_H

#include "ScriptMgr.h"
#include <unordered_map>
#include <vector>

namespace XPCatchup
{

// Pending XP pool per kill event
struct PendingXP
{
    uint32 total = 0;          // Total XP to redistribute
    uint8  count = 0;          // How many group members contributed
    time_t timestamp = 0;      // When the first XP was captured (for cleanup)
};

// Target share for dynamic XP distribution
struct TargetShare
{
    Player* player;
    uint32 weight;             // proportional weight for XP distribution
};

// Module config
extern bool   _enabled;
extern uint32 _levelWindow;
extern uint32 _distribution;
extern uint32 _threshold;
extern bool   _requireRealMaster;
extern bool   _chatdebug;
extern bool   _logging;

// Global state
extern std::unordered_map<ObjectGuid, PendingXP> _pendingXP;
extern std::unordered_map<ObjectGuid, time_t>    _cooldown;

} // namespace XPCatchup

class XPCatchupPlayerScript : public PlayerScript
{
public:
    XPCatchupPlayerScript() : PlayerScript("XPCatchupPlayerScript") { }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource) override;
};

class XPCatchupWorldScript : public WorldScript
{
public:
    XPCatchupWorldScript() : WorldScript("XPCatchupWorldScript") { }

    void OnAfterConfigLoad(bool reload) override;
    void OnBeforeWorldInitialized() override;
    void OnUpdate(uint32 diff) override;
};

void AddXPCatchupScripts();

#endif
