/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MOD_CHAOS_CREATURE_MGR_H
#define MOD_CHAOS_CREATURE_MGR_H

#include "ObjectGuid.h"
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Creature;
class CreatureAI;
class Player;
struct CreatureTemplate;

struct ChaosChallengeCreature
{
    ObjectGuid Guid;
    uint32 Entry = 0;
    std::string Name;
    bool Dead = false;
};

struct ChaosChallenge
{
    uint32 MapId = 0;
    uint32 InstanceId = 0;
    uint8 Level = 0;
    uint32 RemainingMs = 0;
    uint32 HostilityCheckMs = 0;
    bool Completed = false;
    std::vector<ChaosChallengeCreature> Creatures;
    std::vector<std::string> PendingMessages;

    [[nodiscard]] uint32 AliveCount() const;
};

/// A level change that is waiting to settle before it may start a challenge.
struct ChaosPendingLevelUp
{
    uint8 LevelBefore = 0;   // level the character had before the first change
    uint32 RemainingMs = 0;  // restarted by every further level change
};

/**
 * Chaos Creature challenge manager.
 *
 * Every level-up from the configured minimum level spawns a handful of random
 * creatures (scaled to the player's level) around the player. Killing all of
 * them before the timer runs out grants a permanent bonus talent point.
 *
 * Threading: player and creature hooks run on map-update threads, so all
 * shared state is guarded by a mutex. Player-facing effects (messages,
 * rewards, despawns) are only ever applied from the owning player's own
 * update tick so they run on that player's map thread.
 */
class ChaosCreatureMgr
{
public:
    static ChaosCreatureMgr* instance();

    void LoadConfig();
    void BuildCreaturePool();

    [[nodiscard]] bool IsEnabled() const { return _enabled; }

    // Gameplay hooks
    void OnPlayerLevelChanged(Player* player, uint8 oldLevel);
    void OnPlayerUpdate(Player* player, uint32 diff);
    void OnPlayerLogout(Player* player);
    void OnCreatureDeath(Creature* creature);

    // Spawn-time hooks (only act while one of our summons is being created)
    void OnBeforeCreatureSelectLevel(CreatureTemplate const* cinfo, Creature* creature, uint8& level);
    CreatureAI* GetCreatureAI(Creature* creature);

private:
    ChaosCreatureMgr() = default;

    void UpdatePendingLevelUp(Player* player, uint32 diff);
    void TryStartChallenge(Player* player, uint8 levelBefore);
    void StartChallenge(Player* player);
    void FinishChallenge(Player* player, ChaosChallenge const& challenge, bool success);
    void DespawnCreatures(Player* player, ChaosChallenge const& challenge);
    Creature* SpawnScaledCreature(Player* player, uint32 entry, uint8 level);
    [[nodiscard]] bool IsEligibleTemplate(CreatureTemplate const& cinfo) const;

    static void MakeHostile(Creature* creature);
    static void MarkCreatureDone(ChaosChallenge& challenge, ObjectGuid const& guid, char const* outcome);
    static void SendMessage(Player* player, std::string const& text, bool onScreen);

    // Config
    bool _enabled = false;
    uint8 _minLevel = 10;
    uint32 _creatureCount = 3;
    uint32 _timeLimitMs = 15 * 60 * 1000;
    uint32 _levelUpDelayMs = 5 * 1000;
    float _spawnRadius = 30.0f;
    float _spawnMinDistance = 8.0f;
    uint32 _talentPoints = 1;
    bool _replaceScriptedAI = true;
    bool _includeCritters = true;
    bool _includeBosses = true;

    std::vector<uint32> _pool;

    // Lock-free fast paths for the per-player update hook
    std::atomic<uint32> _activeChallenges{0};
    std::atomic<uint32> _pendingLevelUpCount{0};
    std::mutex _lock;
    std::unordered_map<ObjectGuid, ChaosChallenge> _challenges;   // player guid -> challenge
    std::unordered_map<ObjectGuid, ObjectGuid> _creatureOwners;   // creature guid -> player guid
    std::unordered_map<ObjectGuid, ChaosPendingLevelUp> _pendingLevelUps;   // player guid -> settling level-up
};

#define sChaosCreatureMgr ChaosCreatureMgr::instance()

#endif // MOD_CHAOS_CREATURE_MGR_H
