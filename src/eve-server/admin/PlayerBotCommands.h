// no include guard: consumed by AllCommands.h's COMMAND macro
COMMAND(bot, Acct::Role::ADMIN,
        "spawn <profession> <n> | status | dismiss <characterID> | follow | come | stay | dock | mine | attack <entityID> | fleet [invite|join <fleetID> [botID]|leave [botID]|order <command> [targetID]] | corp invite [corpID] [botID] | give <typeID> <quantity> [botID] | contract <botID> [assigneeID] [reward] [destinationStationID]")
COMMAND(botlocali, Acct::Role::ADMIN, "show online PlayerBot counts by solar system")
