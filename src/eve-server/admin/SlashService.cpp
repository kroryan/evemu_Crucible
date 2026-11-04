/*
    ------------------------------------------------------------------------------------
    LICENSE:
    ------------------------------------------------------------------------------------
    This file is part of EVEmu: EVE Online Server Emulator
    Copyright 2006 - 2021 The EVEmu Team
    For the latest information visit https://evemu.dev
    ------------------------------------------------------------------------------------
    This program is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License as published by the Free Software
    Foundation; either version 2 of the License, or (at your option) any later
    version.

    This program is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License along with
    this program; if not, write to the Free Software Foundation, Inc., 59 Temple
    Place - Suite 330, Boston, MA 02111-1307, USA, or go to
    http://www.gnu.org/copyleft/lesser.txt.
    ------------------------------------------------------------------------------------
    Author:        Zhur
*/


#include "eve-server.h"

#include "Client.h"
#include "admin/CommandDispatcher.h"
#include "admin/SlashService.h"

namespace {
bool IsBotLocaliCommand(const std::string& command)
{
    std::string::size_type start = command.find_first_not_of(" \t");
    if (start == std::string::npos)
        return false;
    if (command[start] == '/')
        ++start;

    const std::string::size_type end = command.find_first_of(" \t", start);
    return command.compare(start, end == std::string::npos ? std::string::npos : end - start,
                           "botlocali") == 0;
}
}

SlashService::SlashService(CommandDispatcher *cd) :
    Service("slash"),
    m_commandDispatch(cd)
{
    this->Add("SlashCmd", static_cast <PyResult (SlashService::*) (PyCallArgs &, PyWString*)> (&SlashService::SlashCmd));
    this->Add("SlashCmd", static_cast <PyResult (SlashService::*) (PyCallArgs &, PyString*)> (&SlashService::SlashCmd));
}

PyResult SlashService::SlashCmd (PyCallArgs& call, PyWString* command)
{
    if (is_log_enabled(COMMAND__DUMP)) {
        sLog.White("SlashService::Handle_SlashCmd()", "size=%lu", call.tuple->size());
        call.Dump(COMMAND__DUMP);
    }

    PyResult result = SlashCommand(call.client, command->content());
    if (IsBotLocaliCommand(command->content()) && result.ssResult != nullptr &&
        result.ssResult->IsString()) {
        call.client->SendInfoModalMsg("%s", result.ssResult->AsString()->content().c_str());
        return PyStatic.NewNone();
    }
    return result;
}

PyResult SlashService::SlashCmd (PyCallArgs& call, PyString* command)
{
    if (is_log_enabled(COMMAND__DUMP)) {
        sLog.White("SlashService::Handle_SlashCmd()", "size=%lu", call.tuple->size());
        call.Dump(COMMAND__DUMP);
    }

    PyResult result = SlashCommand(call.client, command->content());
    if (IsBotLocaliCommand(command->content()) && result.ssResult != nullptr &&
        result.ssResult->IsString()) {
        call.client->SendInfoModalMsg("%s", result.ssResult->AsString()->content().c_str());
        return PyStatic.NewNone();
    }
    return result;
}

PyResult SlashService::SlashCommand(Client * client, std::string command)
{
    const int64 role = client->GetAccountRole();
    const bool hasSlashRole = (role & Acct::Role::SLASH) == Acct::Role::SLASH;
    const bool hasAdminRole = (role & Acct::Role::ADMIN) == Acct::Role::ADMIN;
    const bool hasProgrammerRole = (role & Acct::Role::PROGRAMMER) == Acct::Role::PROGRAMMER;
    if (!hasSlashRole && !hasAdminRole && !hasProgrammerRole) {
        _log(COMMAND__ERROR, "%s: Client '%s' does not have slash, admin or programmer access.",
             GetName(), client->GetName());
        throw CustomError("You need a slash, admin or programmer role to execute commands.");
    }

    return m_commandDispatch->Execute( client, command.c_str() );
}

/*(258713, `Role GMH needed`)
 * (258714, `Role GML needed`)
 */
