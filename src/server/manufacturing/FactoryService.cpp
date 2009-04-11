/*
	------------------------------------------------------------------------------------
	LICENSE:
	------------------------------------------------------------------------------------
	This file is part of EVEmu: EVE Online Server Emulator
	Copyright 2006 - 2008 The EVEmu Team
	For the latest information visit http://evemu.mmoforge.org
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
	Author:		Zhur
*/
#include "EvemuPCH.h"


PyCallable_Make_InnerDispatcher(FactoryService)

FactoryService::FactoryService(PyServiceMgr *mgr, DBcore *db)
: PyService(mgr, "factory"),
  m_db(db),
  m_dispatch(new Dispatcher(this))
{
	_SetCallDispatcher(m_dispatch);

	PyCallable_REG_CALL(FactoryService, GetBlueprintAttributes);
	PyCallable_REG_CALL(FactoryService, GetMaterialsForTypeWithActivity);
	PyCallable_REG_CALL(FactoryService, GetMaterialCompositionOfItemType);
}

FactoryService::~FactoryService() {
	delete m_dispatch;
}

PyResult FactoryService::Handle_GetBlueprintAttributes(PyCallArgs &call) {
	PyRep *result = NULL;

	Call_SingleIntegerArg arg;
	if(!arg.Decode(&call.tuple)) {
		_log(SERVICE__ERROR, "Failed to decode args.");
		return NULL;
	}

	BlueprintItem *b = m_manager->item_factory.GetBlueprint(arg.arg, false);
	if(b == NULL)
		return NULL;

	PyRep *res = b->GetBlueprintAttributes();

	b->Release();

	return(res);
}

PyResult FactoryService::Handle_GetMaterialsForTypeWithActivity(PyCallArgs &call) {
	PyRep *result = NULL;

	Call_TwoIntegerArgs call_args;
	if(!call_args.Decode(&call.tuple)) {
		_log(SERVICE__MESSAGE, "Failed to decode args.");
		return NULL;
	}

	result = m_db.GetMaterialsForTypeWithActivity(call_args.arg1);

	if(result == NULL)
		return NULL;

	return(result);
}

PyResult FactoryService::Handle_GetMaterialCompositionOfItemType(PyCallArgs &call) {
	PyRep *result = NULL;

	Call_SingleIntegerArg call_args;
	if(!call_args.Decode(&call.tuple)) {
		_log(SERVICE__MESSAGE, "Failed to decode args.");
		return NULL;
	}

	result = m_db.GetMaterialCompositionOfItemType(call_args.arg);

	if(result == NULL)
		return NULL;

	return(result);
}

