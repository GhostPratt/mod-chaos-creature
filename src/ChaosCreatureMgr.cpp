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

#include "ChaosCreatureMgr.h"
#include "Chat.h"
#include "CombatAI.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureData.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr char const* CHAOS_PREFIX = "|cffff4500[Chaos]|r ";
    constexpr uint32 MAX_SPAWN_ATTEMPTS = 25;
    constexpr uint32 HOSTILITY_CHECK_INTERVAL_MS = 1000;

    // The summons outlive the challenge timer by this much. The challenge
    // despawns them itself when it expires; their own timer is only the
    // fallback for an owner who is on another map by then. Without the margin
    // a summon could expire a tick before the challenge and be mistaken for a
    // creature that vanished on its own, which counts as slain.
    constexpr uint32 SUMMON_GRACE_MS = 30 * IN_MILLISECONDS;

    // Every unit flag that stops a player from attacking or selecting a creature.
    constexpr uint32 BLOCKING_UNIT_FLAGS = UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_ATTACKABLE_1 |
        UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_NON_ATTACKABLE_2 | UNIT_FLAG_NOT_SELECTABLE |
        UNIT_FLAG_PACIFIED;

    /**
     * Plain aggressive AI for chaos creatures.
     *
     * The stock evade handling despawns creatures flagged
     * CREATURE_FLAG_EXTRA_HARD_RESET (most TBC/WotLK bosses) and schedules a
     * fresh Map::SummonCreature of the same entry. That replacement would be
     * a different creature: unscaled, with its template faction, without a
     * despawn timer and unknown to the challenge. Evade like any other
     * creature instead and stay the same summon.
     */
    class ChaosAggressorAI : public AggressorAI
    {
    public:
        explicit ChaosAggressorAI(Creature* creature) : AggressorAI(creature) { }

        void EnterEvadeMode(EvadeReason why) override
        {
            if (!_EnterEvadeMode(why))
                return;

            me->GetMotionMaster()->MoveTargetedHome();
            Reset();
            sScriptMgr->OnUnitEnterEvadeMode(me, why);
        }
    };

    /**
     * Spawn-time context for the creature currently being summoned by this module.
     *
     * Map::SummonCreature is fully synchronous on the calling map thread, so a
     * thread-local marker set right before the call is visible to the
     * SelectLevel / GetCreatureAI hooks that fire during creature creation and
     * cannot leak into unrelated spawns on other threads.
     */
    struct PendingSpawn
    {
        bool Active = false;
        uint32 Entry = 0;
        uint8 Level = 0;
    };

    thread_local PendingSpawn s_pendingSpawn;

    std::string JoinNames(std::vector<ChaosChallengeCreature> const& creatures)
    {
        std::string result;
        for (std::size_t i = 0; i < creatures.size(); ++i)
        {
            if (i > 0)
                result += (i + 1 == creatures.size()) ? " and " : ", ";

            result += "|cffffff00" + creatures[i].Name + "|r";
        }
        return result;
    }
}

uint32 ChaosChallenge::AliveCount() const
{
    return static_cast<uint32>(std::count_if(Creatures.begin(), Creatures.end(),
        [](ChaosChallengeCreature const& c) { return !c.Dead; }));
}

ChaosCreatureMgr* ChaosCreatureMgr::instance()
{
    static ChaosCreatureMgr instance;
    return &instance;
}

void ChaosCreatureMgr::LoadConfig()
{
    _enabled = sConfigMgr->GetOption<bool>("ChaosCreature.Enable", true);
    _minLevel = sConfigMgr->GetOption<uint8>("ChaosCreature.MinLevel", 10);
    _creatureCount = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("ChaosCreature.CreatureCount", 3));
    uint32 const timeLimitMinutes = sConfigMgr->GetOption<uint32>("ChaosCreature.TimeLimitMinutes", 15);
    _timeLimitMs = std::max<uint32>(1, timeLimitMinutes) * MINUTE * IN_MILLISECONDS;
    _levelUpDelayMs = sConfigMgr->GetOption<uint32>("ChaosCreature.LevelUpDelaySeconds", 5) * IN_MILLISECONDS;
    _spawnRadius = std::max(5.0f, sConfigMgr->GetOption<float>("ChaosCreature.SpawnRadius", 30.0f));
    _spawnMinDistance = sConfigMgr->GetOption<float>("ChaosCreature.SpawnMinDistance", 8.0f);
    _spawnMinDistance = std::clamp(_spawnMinDistance, 0.0f, _spawnRadius);
    _talentPoints = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("ChaosCreature.TalentPoints", 1));
    _replaceScriptedAI = sConfigMgr->GetOption<bool>("ChaosCreature.ReplaceScriptedAI", true);
    _includeCritters = sConfigMgr->GetOption<bool>("ChaosCreature.IncludeCritters", true);
    _includeBosses = sConfigMgr->GetOption<bool>("ChaosCreature.IncludeBosses", true);
}

bool ChaosCreatureMgr::IsEligibleTemplate(CreatureTemplate const& cinfo) const
{
    // Invisible helpers / event controllers can never be found or killed.
    if (cinfo.HasFlagsExtra(CREATURE_FLAG_EXTRA_TRIGGER))
        return false;

    if (cinfo.unit_flags & (UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE))
        return false;

    if (cinfo.VehicleId)
        return false;

    if (!_includeCritters && cinfo.type == CREATURE_TYPE_CRITTER)
        return false;

    if (!_includeBosses && (cinfo.rank == CREATURE_ELITE_WORLDBOSS || (cinfo.type_flags & CREATURE_TYPE_FLAG_BOSS_MOB)))
        return false;

    // Needs at least one display model that is not a trigger model.
    bool visible = false;
    for (CreatureModel const& model : cinfo.Models)
    {
        CreatureModelInfo const* modelInfo = sObjectMgr->GetCreatureModelInfo(model.CreatureDisplayID);
        if (modelInfo && !modelInfo->is_trigger)
        {
            visible = true;
            break;
        }
    }

    return visible;
}

void ChaosCreatureMgr::BuildCreaturePool()
{
    _pool.clear();

    if (!_enabled)
        return;

    for (auto const& [entry, cinfo] : *sObjectMgr->GetCreatureTemplates())
    {
        if (IsEligibleTemplate(cinfo))
            _pool.push_back(entry);
    }

    LOG_INFO("module.chaoscreature", "Chaos Creature: {} creature templates eligible for challenges.", _pool.size());

    if (_pool.empty())
    {
        LOG_ERROR("module.chaoscreature", "Chaos Creature: no eligible creature templates found, module disabled.");
        _enabled = false;
    }
}

void ChaosCreatureMgr::SendMessage(Player* player, std::string const& text, bool onScreen)
{
    if (!player || !player->GetSession())
        return;

    ChatHandler(player->GetSession()).SendSysMessage(CHAOS_PREFIX + text);

    if (onScreen)
        player->GetSession()->SendAreaTriggerMessage(text);
}

void ChaosCreatureMgr::OnBeforeCreatureSelectLevel(CreatureTemplate const* cinfo, Creature* /*creature*/, uint8& level)
{
    if (s_pendingSpawn.Active && cinfo && cinfo->Entry == s_pendingSpawn.Entry)
        level = s_pendingSpawn.Level;
}

CreatureAI* ChaosCreatureMgr::GetCreatureAI(Creature* creature)
{
    if (!s_pendingSpawn.Active || !creature || creature->GetEntry() != s_pendingSpawn.Entry)
        return nullptr;

    // Boss and dungeon scripts assume they live inside their instance and
    // dereference the instance script unconditionally; give our copies a
    // plain aggressive AI instead so they behave anywhere in the world.
    if (_replaceScriptedAI && creature->GetScriptId())
        return new ChaosAggressorAI(creature);

    // Hard-reset creatures would replace themselves with an untracked,
    // permanent copy the first time they evade (see ChaosAggressorAI).
    if (creature->GetCreatureTemplate()->HasFlagsExtra(CREATURE_FLAG_EXTRA_HARD_RESET))
        return new ChaosAggressorAI(creature);

    return nullptr;
}

void ChaosCreatureMgr::MakeHostile(Creature* creature)
{
    // FACTION_MONSTER is the generic hostile world creature faction: every
    // player can attack it whatever their race or reputation.
    if (creature->GetFaction() != FACTION_MONSTER)
        creature->SetFaction(FACTION_MONSTER);

    if (creature->GetNpcFlags() != UNIT_NPC_FLAG_NONE)
        creature->ReplaceAllNpcFlags(UNIT_NPC_FLAG_NONE);

    if (creature->HasUnitFlag(UnitFlags(BLOCKING_UNIT_FLAGS)))
        creature->RemoveUnitFlag(UnitFlags(BLOCKING_UNIT_FLAGS));
}

Creature* ChaosCreatureMgr::SpawnScaledCreature(Player* player, uint32 entry, uint8 level)
{
    float const angle = frand(0.0f, 2.0f * static_cast<float>(M_PI));
    float const distance = frand(_spawnMinDistance, _spawnRadius);

    Position pos = player->GetPosition();
    player->MovePositionToFirstCollision(pos, distance, angle);
    pos.SetOrientation(pos.GetAbsoluteAngle(player));

    s_pendingSpawn = { true, entry, level };
    TempSummon* summon = player->GetMap()->SummonCreature(entry, pos, nullptr, _timeLimitMs + SUMMON_GRACE_MS);
    s_pendingSpawn = {};

    if (!summon)
        return nullptr;

    summon->SetTempSummonType(TEMPSUMMON_TIMED_DESPAWN);

    // Make sure the copy is hostile, targetable and comes for the player.
    MakeHostile(summon);
    summon->SetReactState(REACT_AGGRESSIVE);
    summon->SetHomePosition(pos);

    if (summon->AI())
        summon->AI()->AttackStart(player);

    return summon;
}

void ChaosCreatureMgr::StartChallenge(Player* player)
{
    ObjectGuid const playerGuid = player->GetGUID();
    uint8 const level = player->GetLevel();

    ChaosChallenge challenge;
    challenge.MapId = player->GetMapId();
    challenge.InstanceId = player->GetInstanceId();
    challenge.Level = level;
    challenge.RemainingMs = _timeLimitMs;
    challenge.HostilityCheckMs = HOSTILITY_CHECK_INTERVAL_MS;

    uint32 attempts = 0;
    while (challenge.Creatures.size() < _creatureCount && attempts++ < MAX_SPAWN_ATTEMPTS)
    {
        uint32 const entry = _pool[urand(0, _pool.size() - 1)];
        Creature* creature = SpawnScaledCreature(player, entry, level);
        if (!creature)
        {
            LOG_DEBUG("module.chaoscreature", "Chaos Creature: failed to summon entry {} for player {}", entry,
                player->GetName());
            continue;
        }

        ChaosChallengeCreature info;
        info.Guid = creature->GetGUID();
        info.Entry = entry;
        info.Name = creature->GetName();
        challenge.Creatures.push_back(std::move(info));
    }

    if (challenge.Creatures.empty())
    {
        LOG_ERROR("module.chaoscreature", "Chaos Creature: could not summon any creature for player {}",
            player->GetName());
        return;
    }

    std::string const names = JoinNames(challenge.Creatures);
    uint32 const count = challenge.Creatures.size();
    uint32 const minutes = _timeLimitMs / (MINUTE * IN_MILLISECONDS);

    {
        std::lock_guard<std::mutex> guard(_lock);
        for (ChaosChallengeCreature const& c : challenge.Creatures)
            _creatureOwners[c.Guid] = playerGuid;

        _challenges[playerGuid] = std::move(challenge);
        ++_activeChallenges;
    }

    SendMessage(player, Acore::StringFormat("Level {} reached! {} level-{} creature{} appeared nearby: {}.",
        level, count, level, count == 1 ? " has" : "s have", names), false);
    SendMessage(player, Acore::StringFormat("Slay {} within {} minute{} to earn {} permanent talent point{}!",
        count == 1 ? "it" : "them all", minutes, minutes == 1 ? "" : "s", _talentPoints,
        _talentPoints == 1 ? "" : "s"), true);

    LOG_DEBUG("module.chaoscreature", "Chaos Creature: started challenge for player {} at level {} with {} creatures",
        player->GetName(), level, count);
}

void ChaosCreatureMgr::OnPlayerLevelChanged(Player* player, uint8 oldLevel)
{
    if (!_enabled || !player)
        return;

    // Do not start anything yet. Bots and GM commands move a character many
    // levels in one go, either as one big jump or as a burst of single
    // level-ups; only a level that settles exactly one above where it started
    // is a real level-up. OnPlayerUpdate decides once the level stops moving.
    std::lock_guard<std::mutex> guard(_lock);

    auto [itr, inserted] = _pendingLevelUps.try_emplace(player->GetGUID());
    if (inserted)
    {
        itr->second.LevelBefore = oldLevel;
        ++_pendingLevelUpCount;
    }

    itr->second.RemainingMs = _levelUpDelayMs;
}

void ChaosCreatureMgr::UpdatePendingLevelUp(Player* player, uint32 diff)
{
    uint8 levelBefore = 0;

    {
        std::lock_guard<std::mutex> guard(_lock);

        auto itr = _pendingLevelUps.find(player->GetGUID());
        if (itr == _pendingLevelUps.end())
            return;

        if (itr->second.RemainingMs > diff)
        {
            itr->second.RemainingMs -= diff;
            return;
        }

        levelBefore = itr->second.LevelBefore;
        _pendingLevelUps.erase(itr);
        --_pendingLevelUpCount;
    }

    TryStartChallenge(player, levelBefore);
}

void ChaosCreatureMgr::TryStartChallenge(Player* player, uint8 levelBefore)
{
    uint8 const level = player->GetLevel();
    if (level != levelBefore + 1)
    {
        LOG_DEBUG("module.chaoscreature", "Chaos Creature: ignoring level change {} -> {} of player {}, "
            "not a single level-up", levelBefore, level, player->GetName());
        return;
    }

    if (level < _minLevel)
        return;

    if (player->InBattleground() || player->InArena())
        return;

    bool alreadyActive = false;
    {
        std::lock_guard<std::mutex> guard(_lock);
        alreadyActive = _challenges.count(player->GetGUID()) > 0;
    }

    if (alreadyActive)
    {
        SendMessage(player, "You already have an active chaos challenge. Finish it before more chaos finds you!",
            false);
        return;
    }

    StartChallenge(player);
}

void ChaosCreatureMgr::OnCreatureDeath(Creature* creature)
{
    if (!_enabled || !creature)
        return;

    std::lock_guard<std::mutex> guard(_lock);

    auto ownerItr = _creatureOwners.find(creature->GetGUID());
    if (ownerItr == _creatureOwners.end())
        return;

    ObjectGuid const playerGuid = ownerItr->second;
    _creatureOwners.erase(ownerItr);

    auto challengeItr = _challenges.find(playerGuid);
    if (challengeItr == _challenges.end())
        return;

    MarkCreatureDone(challengeItr->second, creature->GetGUID(), "slain");
}

void ChaosCreatureMgr::MarkCreatureDone(ChaosChallenge& challenge, ObjectGuid const& guid, char const* outcome)
{
    auto itr = std::find_if(challenge.Creatures.begin(), challenge.Creatures.end(),
        [&guid](ChaosChallengeCreature const& c) { return c.Guid == guid; });
    if (itr == challenge.Creatures.end() || itr->Dead)
        return;

    itr->Dead = true;

    uint32 const alive = challenge.AliveCount();
    if (alive == 0)
        challenge.Completed = true;
    else
        challenge.PendingMessages.push_back(Acore::StringFormat("|cffffff00{}|r {}! {} creature{} left.",
            itr->Name, outcome, alive, alive == 1 ? "" : "s"));
}

void ChaosCreatureMgr::DespawnCreatures(Player* player, ChaosChallenge const& challenge)
{
    // Creatures on another map are left to their own summon timer; touching
    // them from this thread would be unsafe. This is a best-effort cleanup.
    Map* map = player->GetMap();
    if (!map || map->GetId() != challenge.MapId || map->GetInstanceId() != challenge.InstanceId)
        return;

    for (ChaosChallengeCreature const& c : challenge.Creatures)
    {
        if (c.Dead)
            continue;

        if (Creature* creature = map->GetCreature(c.Guid))
            creature->DespawnOrUnsummon();
    }
}

void ChaosCreatureMgr::FinishChallenge(Player* player, ChaosChallenge const& challenge, bool success)
{
    if (success)
    {
        player->RewardExtraBonusTalentPoints(_talentPoints);
        player->InitTalentForLevel();
        player->SaveToDB(false, false);

        SendMessage(player, Acore::StringFormat("Chaos conquered! You have earned {} permanent talent point{}.",
            _talentPoints, _talentPoints == 1 ? "" : "s"), true);

        LOG_DEBUG("module.chaoscreature", "Chaos Creature: player {} completed the level {} challenge",
            player->GetName(), challenge.Level);
        return;
    }

    DespawnCreatures(player, challenge);
    SendMessage(player, "Time is up! The chaos creatures have vanished.", true);

    LOG_DEBUG("module.chaoscreature", "Chaos Creature: player {} failed the level {} challenge",
        player->GetName(), challenge.Level);
}

void ChaosCreatureMgr::OnPlayerUpdate(Player* player, uint32 diff)
{
    if (!_enabled || !player || !player->IsInWorld())
        return;

    if (_pendingLevelUpCount.load(std::memory_order_relaxed) != 0)
        UpdatePendingLevelUp(player, diff);

    if (_activeChallenges.load(std::memory_order_relaxed) == 0)
        return;

    std::vector<std::string> messages;
    std::vector<ObjectGuid> hostilityCheck;
    uint32 challengeMapId = 0;
    uint32 challengeInstanceId = 0;
    ChaosChallenge finished;
    bool hasFinished = false;
    bool success = false;

    {
        std::lock_guard<std::mutex> guard(_lock);

        auto itr = _challenges.find(player->GetGUID());
        if (itr == _challenges.end())
            return;

        ChaosChallenge& challenge = itr->second;
        messages.swap(challenge.PendingMessages);

        if (challenge.Completed)
        {
            hasFinished = true;
            success = true;
        }
        else if (challenge.RemainingMs <= diff)
        {
            hasFinished = true;
        }
        else
        {
            challenge.RemainingMs -= diff;
        }

        if (hasFinished)
        {
            for (ChaosChallengeCreature const& c : challenge.Creatures)
                _creatureOwners.erase(c.Guid);

            finished = std::move(challenge);
            _challenges.erase(itr);
            --_activeChallenges;
        }
        else if (challenge.HostilityCheckMs > diff)
        {
            challenge.HostilityCheckMs -= diff;
        }
        else
        {
            challenge.HostilityCheckMs = HOSTILITY_CHECK_INTERVAL_MS;
            challengeMapId = challenge.MapId;
            challengeInstanceId = challenge.InstanceId;
            for (ChaosChallengeCreature const& c : challenge.Creatures)
                if (!c.Dead)
                    hostilityCheck.push_back(c.Guid);
        }
    }

    for (std::string const& message : messages)
        SendMessage(player, message, false);

    // Creature scripts (SmartAI resets, entry updates, charm removal) put the
    // template faction and flags back, which can leave a creature the player
    // is not allowed to attack. Re-apply ours. Only on the player's own map:
    // creatures elsewhere belong to another map thread.
    if (!hostilityCheck.empty())
    {
        Map* map = player->GetMap();
        if (map && map->GetId() == challengeMapId && map->GetInstanceId() == challengeInstanceId)
        {
            std::vector<ObjectGuid> vanished;
            for (ObjectGuid const& guid : hostilityCheck)
            {
                Creature* creature = map->GetCreature(guid);
                if (!creature)
                    vanished.push_back(guid);
                else if (creature->IsAlive())
                    MakeHostile(creature);
            }

            // A creature that left the world without dying (its own script
            // despawned it) can no longer be killed; count it as slain so the
            // challenge stays winnable. Completion is picked up next tick.
            if (!vanished.empty())
            {
                std::lock_guard<std::mutex> guard(_lock);

                auto itr = _challenges.find(player->GetGUID());
                if (itr != _challenges.end())
                {
                    for (ObjectGuid const& guid : vanished)
                    {
                        _creatureOwners.erase(guid);
                        MarkCreatureDone(itr->second, guid, "vanished and counts as slain");
                    }
                }
            }
        }
    }

    if (hasFinished)
        FinishChallenge(player, finished, success);
}

void ChaosCreatureMgr::OnPlayerLogout(Player* player)
{
    if (!player)
        return;

    ChaosChallenge challenge;

    {
        std::lock_guard<std::mutex> guard(_lock);

        if (_pendingLevelUps.erase(player->GetGUID()))
            --_pendingLevelUpCount;

        auto itr = _challenges.find(player->GetGUID());
        if (itr == _challenges.end())
            return;

        for (ChaosChallengeCreature const& c : itr->second.Creatures)
            _creatureOwners.erase(c.Guid);

        challenge = std::move(itr->second);
        _challenges.erase(itr);
        --_activeChallenges;
    }

    DespawnCreatures(player, challenge);
}
