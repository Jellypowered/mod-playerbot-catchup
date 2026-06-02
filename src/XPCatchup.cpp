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

#include "XPCatchup.h"
#include "Chat.h"
#include "Group.h"
#include "WorldPacket.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ScriptMgr.h"
#include "World.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

// Static member definitions
namespace XPCatchup
{
    bool _enabled = true;
    uint32 _levelWindow = 5;
    uint32 _distribution = 0;
    uint32 _threshold = 10;
    bool _requireRealMaster = true;
    bool _chatdebug = false;
    bool _logging = false;

    std::unordered_map<ObjectGuid, PendingXP> _pendingXP;
}

using namespace XPCatchup;

// Helper: Find the master (party leader, must be a real player)
static Player* FindMaster(Group* group)
{
    ObjectGuid leaderGUID = group->GetLeaderGUID();
    if (leaderGUID.IsEmpty())
        return nullptr;

    Player* leader = ObjectAccessor::FindPlayer(leaderGUID);
    if (!leader)
        return nullptr;

    if (!leader->GetSession())
        return nullptr;

    // Only require the master to be a real player if config says so
    // Requires mod-playerbots (IsBot() only exists when MOD_PLAYERBOTS is defined)
#ifdef MOD_PLAYERBOTS
    if (_requireRealMaster && leader->GetSession()->IsBot())
        return nullptr;
#endif

    return leader;
}

// Helper: Check if a player is within the level window of the master
static bool IsWithinLevelWindow(Player* member, Player* master)
{
    int8 levelDiff = std::abs(master->GetLevel() - member->GetLevel());
    return levelDiff <= static_cast<int8>(_levelWindow);
}

// Helper: Find the single lowest-XP eligible member
static Player* FindLowestXPMember(Group* group, ObjectGuid excludeGUID)
{
    Player* master = FindMaster(group);
    if (!master)
        return nullptr;

    Player* lowest = nullptr;
    float lowestRatio = 999999.0f;

    group->DoForAllMembers([&](Player* member)
    {
        if (!member)
            return;
        if (member->GetGUID() == excludeGUID)
            return;
        if (member == master)
            return;
        if (!IsWithinLevelWindow(member, master))
            return;
        if (member->GetLevel() > master->GetLevel())
            return;

        uint32 currentXP = member->GetUInt32Value(PLAYER_XP);
        uint32 nextLevelXP = member->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
        float ratio = (nextLevelXP > 0) ? (float)currentXP / nextLevelXP : 0.0f;

        if (ratio < lowestRatio)
        {
            lowestRatio = ratio;
            lowest = member;
        }
    });

    return lowest;
}

// Helper: Find the master's XP deficit weight (negative when master is ahead)
static int32 GetDeficit(Player* member, Player* master)
{
    uint32 masterXP = master->GetUInt32Value(PLAYER_XP);
    uint32 masterNextLevelXP = master->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
    float masterRatio = (masterNextLevelXP > 0) ? (float)masterXP / masterNextLevelXP : 0.0f;
    uint32 masterLevel = master->GetLevel();

    uint32 memberXP = member->GetUInt32Value(PLAYER_XP);
    uint32 memberNextLevelXP = member->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
    float memberRatio = (memberNextLevelXP > 0) ? (float)memberXP / memberNextLevelXP : 0.0f;
    uint32 levelDeficit = masterLevel - member->GetLevel();
    return static_cast<int32>(levelDeficit * 10000.0f)
         + static_cast<int32>((masterRatio - memberRatio) * 10000.0f);
}

// Helper: Find dynamic targets with weighted XP distribution
static std::vector<TargetShare> FindDynamicTargets(Group* group, Player* master)
{
    std::vector<TargetShare> targets;
    uint32 totalWeight = 0;

    group->DoForAllMembers([&](Player* member)
    {
        if (!member)
            return;
        if (member == master)
            return;
        if (!IsWithinLevelWindow(member, master))
            return;
        if (member->GetLevel() > master->GetLevel())
            return;

        int32 deficit = GetDeficit(member, master);
        uint32 weight = deficit > 0 ? static_cast<uint32>(deficit) : 1;
        totalWeight += weight;
        targets.push_back({member, weight});
    });

    // Edge case: If no eligible targets found, force equal split among all non-master members
    if (targets.empty())
    {
        group->DoForAllMembers([&](Player* member)
        {
            if (!member || member == master)
                return;
            targets.push_back({member, 1});
        });
    }

    // Include master in distribution when they have the lowest XP progress ratio
    // (i.e., bots have caught up to or passed the master)
    float masterRatio = (master->GetUInt32Value(PLAYER_NEXT_LEVEL_XP) > 0)
        ? (float)master->GetUInt32Value(PLAYER_XP) / master->GetUInt32Value(PLAYER_NEXT_LEVEL_XP)
        : 0.0f;

    bool masterIsLowest = true;
    group->DoForAllMembers([&](Player* member)
    {
        if (!member || member == master)
            return;

        float memberRatio = (member->GetUInt32Value(PLAYER_NEXT_LEVEL_XP) > 0)
            ? (float)member->GetUInt32Value(PLAYER_XP) / member->GetUInt32Value(PLAYER_NEXT_LEVEL_XP)
            : 0.0f;

        if (memberRatio < masterRatio)
        {
            masterIsLowest = false;
        }
    });

    if (masterIsLowest && !targets.empty())
    {
        uint32 masterWeight = 1;
        totalWeight += masterWeight;
        targets.push_back({master, masterWeight});
    }

    return targets;
}

// Check if any group member has >= threshold XP deficit relative to the master.
// Returns false when the group is close enough that catchup is unnecessary.
static bool HasCatchupNeed(Group* group, Player* master)
{
    float masterRatio = (master->GetUInt32Value(PLAYER_NEXT_LEVEL_XP) > 0)
        ? (float)master->GetUInt32Value(PLAYER_XP) / master->GetUInt32Value(PLAYER_NEXT_LEVEL_XP)
        : 0.0f;

    bool found = false;
    group->DoForAllMembers([&](Player* member)
    {
        if (!member || member == master)
            return;

        float memberRatio = (member->GetUInt32Value(PLAYER_NEXT_LEVEL_XP) > 0)
            ? (float)member->GetUInt32Value(PLAYER_XP) / member->GetUInt32Value(PLAYER_NEXT_LEVEL_XP)
            : 0.0f;

        // Threshold deficit: member ratio is at least threshold percentage points behind master
        if ((masterRatio - memberRatio) >= _threshold * 0.01f)
        {
            found = true;
        }
    });

    return found;
}

// Helper: Distribute XP pool among targets proportionally by weight
static void DistributeXP(uint32 pool, std::vector<TargetShare>& targets, Player* victim, Player* master, Group* group)
{
    if (targets.empty())
        return;

    uint32 totalWeight = 0;
    for (auto& t : targets)
        totalWeight += t.weight;

    uint32 distributed = 0;
    std::vector<uint32> shares;
    std::vector<int32> deficits;

    // Save XP deficits before giving XP (state before this kill)
    uint32 masterXP = master->GetUInt32Value(PLAYER_XP);
    uint32 masterNextLevelXP = master->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
    float masterRatio = (masterNextLevelXP > 0) ? (float)masterXP / masterNextLevelXP : 0.0f;
    uint32 masterLevel = master->GetLevel();

    for (auto& t : targets)
    {
        uint32 memberXP = t.player->GetUInt32Value(PLAYER_XP);
        uint32 memberNextLevelXP = t.player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
        float memberRatio = (memberNextLevelXP > 0) ? (float)memberXP / memberNextLevelXP : 0.0f;
        uint32 levelDeficit = masterLevel - t.player->GetLevel();
        int32 totalDeficit = static_cast<int32>(levelDeficit * 10000.0f)
                           + static_cast<int32>((masterRatio - memberRatio) * 10000.0f);
        deficits.push_back(totalDeficit);
    }

    // Give each target their proportional share
    for (auto& t : targets)
    {
        uint32 share = (pool * t.weight) / totalWeight;
        t.player->GiveXP(share, victim, 1.0f);
        distributed += share;
        shares.push_back(share);
    }

    // Handle rounding: give 1 extra XP to the first target to close the gap
    if (distributed < pool)
    {
        shares[0] += pool - distributed;
        targets[0].player->GiveXP(pool - distributed, victim, 1.0f);
    }

    // Debug: log what each character received to party chat
    if (_chatdebug)
    {
        size_t idx = 0;
        for (auto& t : targets)
        {
            uint32 share = shares[idx];
            int32 deficit = deficits[idx];
            int32 displayDeficit = std::max(0, deficit);
            float deficitPct = displayDeficit / 100.0f;

            if (_logging)
            {
                LOG_INFO("XPCatchup", "[XP Catch-Up] {} received {} XP (weight {}; deficit: {:.2f}%)",
                    t.player->GetName(), share, t.weight, deficitPct);
            }

            std::ostringstream msg;
            msg << "[XP Catch-Up] received " << share << " XP (weight " << t.weight
                << "; deficit: " << std::fixed << std::setprecision(2) << deficitPct << "%)";

            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, LANG_UNIVERSAL, t.player, nullptr, msg.str());
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member->GetSession())
                    member->GetSession()->SendPacket(&data);
            }
            idx++;
        }
    }
    else
    {
        // log distribution summary to console (if _logging)
        if (_logging)
        {
        std::ostringstream oss;
        oss << "[XP Catch-Up] Distribution: total=" << pool
            << " targets=" << targets.size();
        for (auto& t : targets)
        {
            oss << " " << t.player->GetName() << "=" << (pool * t.weight) / totalWeight;
        }
            LOG_INFO("XPCatchup", "%s", oss.str().c_str());
        }
    }
}

// Main hook: OnPlayerGiveXP
void XPCatchupPlayerScript::OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource)
{
    // Only intercept kill XP
    if (xpSource != XPSOURCE_KILL)
        return;

    // Only if player is in a group
    Group* group = player->GetGroup();
    if (!group)
        return;

    // Check if module is enabled
    if (!_enabled)
        return;

    // Skip catchup when no member has >= 10% deficit (group is close enough)
    // This lets natural XP flow when everyone is within 10% of each other
    Player* master = FindMaster(group);
    if (master && !HasCatchupNeed(group, master))
    {
        if (_logging)
        {
            LOG_INFO("XPCatchup", "[XP Catch-Up] No catchup need: group within 10% deficit. Skipping victim {}", victim->GetEntry());
        }
        return;
    }

    // Check if this victim is already pending
    auto it = _pendingXP.find(victim->GetGUID());
    bool isFirst = (it == _pendingXP.end());

    if (isFirst)
    {
        // First player: their XP goes into the pool
        _pendingXP[victim->GetGUID()] = { amount, 1, time(NULL) };

        if (_logging)
        {
            LOG_INFO("XPCatchup", "[XP Catch-Up] First player {} for victim {} (GUID: {}): {} XP (added to pool)",
                player->GetName(), victim->GetEntry(), victim->GetGUID().ToString(), amount);
        }

        // Zero this player's amount so only the pool has this XP
        amount = 0;
        return;
    }

    // Not first player — add their XP to the redistribution pool and zero their share
    it->second.total += amount;
    it->second.count++;
    amount = 0;

    if (_logging)
    {
        LOG_INFO("XPCatchup", "[XP Catch-Up] Contribution from {} to victim {} (GUID: {}): pool now {} XP (count {})",
            player->GetName(), victim->GetEntry(), victim->GetGUID().ToString(), it->second.total, it->second.count);
    }

    // Redistribute pool when all members have contributed
    if (it->second.count >= group->GetMembersCount())
    {
        Player* master = FindMaster(group);
        if (!master)
        {
            if (_logging)
            {
                LOG_INFO("XPCatchup", "[XP Catch-Up] No valid master found for victim {}. Pool discarded ({})",
                    victim->GetEntry(), it->second.total);
            }
            _pendingXP.erase(it);
            return;
        }

        if (_logging)
        {
            LOG_INFO("XPCatchup", "[XP Catch-Up] Master {} (level {}) found. Redistributing {} XP from {} contributors",
                master->GetName(), master->GetLevel(), it->second.total, it->second.count);
        }

        if (_distribution == 1)
        {
            // Dynamic mode: split pool proportionally by XP deficit
            auto targets = FindDynamicTargets(group, master);
            if (_logging)
            {
                LOG_INFO("XPCatchup", "[XP Catch-Up] Dynamic mode: {} eligible targets found", targets.size());
            }
            DistributeXP(it->second.total, targets, player, master, group);
        }
        else
        {
            // Lowest mode (default): give all to single lowest-XP member
            Player* target = FindLowestXPMember(group, ObjectGuid::Empty);
            // If no eligible bot found (all bots ahead of master in level),
            // give XP to the master so they don't lose progress
            if (!target)
            {
                target = master;
            }

            if (target)
            {
                if (_logging)
                {
                    LOG_INFO("XPCatchup", "[XP Catch-Up] Lowest mode: giving {} XP to {} (ratio {:.4f}) -> master ratio {:.4f}",
                        it->second.total, target->GetName(),
                        (float)target->GetUInt32Value(PLAYER_XP) / std::max(1u, target->GetUInt32Value(PLAYER_NEXT_LEVEL_XP)),
                        (float)master->GetUInt32Value(PLAYER_XP) / std::max(1u, master->GetUInt32Value(PLAYER_NEXT_LEVEL_XP)));
                }

                // Save XP progress ratios before giving XP (state before this kill)
                uint32 targetXPBefore = target->GetUInt32Value(PLAYER_XP);
                uint32 targetNextLevelXP = target->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
                float targetRatio = (targetNextLevelXP > 0) ? (float)targetXPBefore / targetNextLevelXP : 0.0f;
                uint32 masterXPBefore = master->GetUInt32Value(PLAYER_XP);
                uint32 masterNextLevelXP = master->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
                float masterRatio = (masterNextLevelXP > 0) ? (float)masterXPBefore / masterNextLevelXP : 0.0f;
                uint32 masterLevel = master->GetLevel();

                target->GiveXP(it->second.total, player, 1.0f);

                // Debug: log distribution to party chat
                if (_chatdebug)
                {
                    uint32 levelDeficit = masterLevel - target->GetLevel();
                    int32 totalDeficit = static_cast<int32>(levelDeficit * 10000.0f)
                                       + static_cast<int32>((masterRatio - targetRatio) * 10000.0f);
                    int32 deficit = std::max(0, totalDeficit);
                    float deficitPct = deficit / 100.0f;
                    uint32 weight = deficit > 0 ? static_cast<uint32>(deficit) : 1;

                    std::ostringstream msg;
                    msg << "[XP Catch-Up] received " << it->second.total << " XP (weight " << weight
                        << "; deficit: " << std::fixed << std::setprecision(2) << deficitPct << "%)";

                    WorldPacket data;
                    ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, LANG_UNIVERSAL, target, nullptr, msg.str());
                    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    {
                        Player* m = ref->GetSource();
                        if (m && m->GetSession())
                            m->GetSession()->SendPacket(&data);
                    }
                }
            }
            else
            {
                if (_logging)
                {
                    LOG_INFO("XPCatchup", "[XP Catch-Up] Lowest mode: no eligible target found for victim {}. Pool discarded ({})",
                        victim->GetEntry(), it->second.total);
                }
            }
        }

        // Clean up pending XP
        _pendingXP.erase(it);
    }
}

// WorldScript: Initialize config
void XPCatchupWorldScript::OnAfterConfigLoad(bool reload)
{
    _enabled           = sConfigMgr->GetOption<bool>("XPCatchup.Enable", true);
    _levelWindow       = sConfigMgr->GetOption<uint32>("XPCatchup.LevelWindow", 5);
    _distribution      = sConfigMgr->GetOption<uint32>("XPCatchup.Distribution", 0);
    _threshold         = sConfigMgr->GetOption<uint32>("XPCatchup.Threshold", 10);
#ifdef MOD_PLAYERBOTS
    _requireRealMaster = sConfigMgr->GetOption<bool>("XPCatchup.RequireRealMaster", true);
#else
    _requireRealMaster = false; // no-op without mod-playerbots
#endif
    _chatdebug         = sConfigMgr->GetOption<bool>("XPCatchup.ChatDebug", false);
    _logging           = sConfigMgr->GetOption<bool>("XPCatchup.Logging", false);

    if (reload)
    {
        LOG_INFO("XPCatchup", "Configuration reloaded. Enable={}, LevelWindow={}, Distribution={}, RequireRealMaster={}, ChatDebug={}, Logging={}",
            _enabled, _levelWindow, _distribution, _requireRealMaster, _chatdebug, _logging);
    }
    else
    {
        //LOG_INFO("XPCatchup", "XP Catch-Up loaded. Enable={}, LevelWindow={}, Distribution={}, RequireRealMaster={}, ChatDebug={}, Logging={}",
        //    _enabled, _levelWindow, _distribution, _requireRealMaster, _chatdebug, _logging);
    }
}

// WorldScript: Final initialization right before the world becomes operational
void XPCatchupWorldScript::OnBeforeWorldInitialized()
{
    LOG_INFO("server.loading", " ");
    LOG_INFO("server.loading", "╔══════════════════════════════════════════════════════════╗");
    LOG_INFO("server.loading", "║                                                          ║");
    LOG_INFO("server.loading", "║               XP Catch-Up Module                         ║");
    LOG_INFO("server.loading", "║                                                          ║");
    LOG_INFO("server.loading", "╟──────────────────────────────────────────────────────────╢");
    LOG_INFO("server.loading", "║     Dynamically redistributes kill XP so party members   ║");
    LOG_INFO("server.loading", "║     stay on the same progression track — no more         ║");
    LOG_INFO("server.loading", "║     lagging behind!                                      ║");
    LOG_INFO("server.loading", "╟──────────────────────────────────────────────────────────╢");
    LOG_INFO("server.loading", "║                  Author: Jellypowered                    ║");
    LOG_INFO("server.loading", "║               Licensed under GNU GPL v2                  ║");
    LOG_INFO("server.loading", "╚══════════════════════════════════════════════════════════╝");

    LOG_INFO("XPCatchup", "XP Catch-Up Config loaded with options:");
    LOG_INFO("XPCatchup", "Enable={}, LevelWindow={}, Distribution={}, RequireRealMaster={}, ChatDebug={}, Logging={}",
        _enabled, _levelWindow, _distribution, _requireRealMaster, _chatdebug, _logging);
}

// WorldScript: Cleanup expired entries periodically
void XPCatchupWorldScript::OnUpdate(uint32 diff)
{
    static uint32 cleanupTimer = 0;
    cleanupTimer += diff;
    if (cleanupTimer < 5000)
        return;
    cleanupTimer = 0;

    time_t now = time(NULL);

    // Clean up expired pending XP entries (older than 2 seconds)
    for (auto it = _pendingXP.begin(); it != _pendingXP.end(); )
    {
        if (difftime(now, it->second.timestamp) > 2)
            it = _pendingXP.erase(it);
        else
            ++it;
    }
}

// Script loader function
void AddXPCatchupScripts()
{
    new XPCatchupPlayerScript();
    new XPCatchupWorldScript();
}
