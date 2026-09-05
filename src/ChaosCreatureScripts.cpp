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
#include "Creature.h"
#include "Player.h"
#include "ScriptMgr.h"

class ChaosCreatureWorldScript : public WorldScript
{
public:
    ChaosCreatureWorldScript() : WorldScript("ChaosCreatureWorldScript") { }

    void OnAfterConfigLoad(bool reload) override
    {
        sChaosCreatureMgr->LoadConfig();

        // Creature templates are not loaded yet on the initial config pass;
        // OnStartup builds the pool then. On reload rebuild it right away.
        if (reload)
            sChaosCreatureMgr->BuildCreaturePool();
    }

    void OnStartup() override
    {
        sChaosCreatureMgr->BuildCreaturePool();
    }
};

class ChaosCreaturePlayerScript : public PlayerScript
{
public:
    ChaosCreaturePlayerScript() : PlayerScript("ChaosCreaturePlayerScript") { }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        sChaosCreatureMgr->OnPlayerLevelChanged(player, oldLevel);
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        sChaosCreatureMgr->OnPlayerUpdate(player, diff);
    }

    void OnPlayerLogout(Player* player) override
    {
        sChaosCreatureMgr->OnPlayerLogout(player);
    }
};

class ChaosCreatureUnitScript : public UnitScript
{
public:
    ChaosCreatureUnitScript() : UnitScript("ChaosCreatureUnitScript") { }

    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        if (Creature* creature = unit ? unit->ToCreature() : nullptr)
            sChaosCreatureMgr->OnCreatureDeath(creature);
    }
};

class ChaosCreatureAllCreatureScript : public AllCreatureScript
{
public:
    ChaosCreatureAllCreatureScript() : AllCreatureScript("ChaosCreatureAllCreatureScript") { }

    void OnBeforeCreatureSelectLevel(CreatureTemplate const* cinfo, Creature* creature, uint8& level) override
    {
        sChaosCreatureMgr->OnBeforeCreatureSelectLevel(cinfo, creature, level);
    }

    CreatureAI* GetCreatureAI(Creature* creature) const override
    {
        return sChaosCreatureMgr->GetCreatureAI(creature);
    }
};

void AddChaosCreatureScripts()
{
    new ChaosCreatureWorldScript();
    new ChaosCreaturePlayerScript();
    new ChaosCreatureUnitScript();
    new ChaosCreatureAllCreatureScript();
}
