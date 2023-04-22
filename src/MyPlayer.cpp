/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "list"
#include "string"
#include "random"

using namespace std;

#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "Configuration/Config.h"
#include "Creature.h"
#include "Object.h"
#include "Position.h"

using namespace Acore::ChatCommands;

// Add player scripts
class ChaosCreature : public PlayerScript
{
public:
    ChaosCreature() : PlayerScript("ChaosCreature") {}

    void OnLogin(Player* player) override
    {

    }
    void OnLevelChanged(Player* player, uint8 oldLevel) override
    {

    }
};

class ChaosCreatureCommand : public CommandScript
{
public:
    ChaosCreatureCommand(): CommandScript("ChaosCreatureCommand") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable ChaosCreatureCommandTable =
                {
                        {"mob", HandleChaosCreatureSelCommand, SEC_ADMINISTRATOR, Console::Yes},
                };
        static ChatCommandTable ChaosCommandBaseTable =
                {
                        {"chaos", ChaosCreatureCommandTable},
                };
        return ChaosCommandBaseTable;
    }

    static bool HandleChaosCreatureSelCommand(ChatHandler *handler)
    {
        Player *player = handler->GetSession()->GetPlayer();
        Map *map = player->GetMap();


        if(!player)
            return false;

        if (player->IsInCombat())
        {
            handler->SendSysMessage("You can't use this command while in combat!");
            handler->SetSentErrorMessage(true);
            return false;
        }
        QueryResult result = WorldDatabase.Query("SELECT entry FROM creature_template");

        if (!result)
        {
            handler->SendSysMessage("Query Failed");
            handler->SetSentErrorMessage(true);
            return false;
        }
        list<int> itemlist;
        if (result)
        {
            do
            {
                int entry = result->Fetch()[0].Get<uint32>();
                itemlist.emplace_back(entry);
            } while (result->NextRow());
        }

        int size = itemlist.size();
        random_device rd;
        mt19937 gen(rd());
        uniform_int_distribution<> distr(0, size);
        auto itemlistFront = itemlist.begin();
        advance(itemlistFront, distr(gen));
        int item = *itemlistFront;

        uint32 x = player->GetPositionX();
        uint32 y = player->GetPositionY();
        uint32 z = player->GetPostionZ();
        handler->SendSysMessage(std::to_string(x, y, z));




        return true;
    }
};


// Add all scripts in one
void AddChaosCreatureScripts()
{
    new ChaosCreature();
    new ChaosCreatureCommand();
}
