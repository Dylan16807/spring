/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */


#include <set>
#include <cctype>


#include "LuaRules.h"

#include "LuaInclude.h"

#include "LuaCallInCheck.h"
#include "LuaUtils.h"
#include "LuaMaterial.h"
#include "LuaSyncedCtrl.h"
#include "LuaSyncedRead.h"
#include "LuaUnitRendering.h"
#include "LuaUnitDefs.h"
#include "LuaWeaponDefs.h"
#include "LuaOpenGL.h"

#include "Game/Game.h"
#include "Sim/Units/CommandAI/Command.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Projectiles/Projectile.h"
#include "Sim/Units/BuildInfo.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/Scripts/CobInstance.h" // for UNPACK{X,Z}
#include "Sim/Weapons/Weapon.h"
#include "Sim/Weapons/WeaponDef.h"
#include "System/Log/ILog.h"
#include "System/FileSystem/VFSModes.h" // for SPRING_VFS_*

#include <assert.h>

CLuaRules* luaRules = NULL;

static const char* LuaRulesSyncedFilename   = "LuaRules/main.lua";
static const char* LuaRulesUnsyncedFilename = "LuaRules/draw.lua";

const int* CLuaRules::currentCobArgs = NULL;


/******************************************************************************/
/******************************************************************************/

void CLuaRules::LoadHandler()
{
	//FIXME GML: this needs a mutex!!!
	if (luaRules != NULL) {
		return;
	}

	luaRules = new CLuaRules();

	if (!luaRules->IsValid()) {
		FreeHandler();
	}
}


void CLuaRules::FreeHandler()
{
	//FIXME GML: this needs a mutex!!!
	delete luaRules; luaRules = NULL;
}


/******************************************************************************/
/******************************************************************************/

CLuaRules::CLuaRules()
: CLuaHandleSynced("LuaRules", LUA_HANDLE_ORDER_RULES)
{
	if (!IsValid()) {
		return;
	}

	SetFullCtrl(true, true);
	SetFullRead(true, true);
	SetCtrlTeam(AllAccessTeam, true);
	SetReadTeam(AllAccessTeam, true);
	SetReadAllyTeam(AllAccessTeam, true);
	SetSelectTeam(AllAccessTeam, true);

	Init(LuaRulesSyncedFilename, LuaRulesUnsyncedFilename, SPRING_VFS_MOD);

	if (!IsValid()) {
		return;
	}

	BEGIN_ITERATE_LUA_STATES();

	if (SingleState() || L == L_Sim) {
		haveCommandFallback            = HasCallIn(L, "CommandFallback");
		haveAllowCommand               = HasCallIn(L, "AllowCommand");
		haveAllowUnitCreation          = HasCallIn(L, "AllowUnitCreation");
		haveAllowUnitTransfer          = HasCallIn(L, "AllowUnitTransfer");
		haveAllowUnitBuildStep         = HasCallIn(L, "AllowUnitBuildStep");
		haveAllowFeatureCreation       = HasCallIn(L, "AllowFeatureCreation");
		haveAllowFeatureBuildStep      = HasCallIn(L, "AllowFeatureBuildStep");
		haveAllowResourceLevel         = HasCallIn(L, "AllowResourceLevel");
		haveAllowResourceTransfer      = HasCallIn(L, "AllowResourceTransfer");
		haveAllowDirectUnitControl     = HasCallIn(L, "AllowDirectUnitControl");
		haveAllowStartPosition         = HasCallIn(L, "AllowStartPosition");

		haveMoveCtrlNotify             = HasCallIn(L, "MoveCtrlNotify");
		haveTerraformComplete          = HasCallIn(L, "TerraformComplete");
		haveAllowWeaponTargetCheck     = HasCallIn(L, "AllowWeaponTargetCheck");
		haveAllowWeaponTarget          = HasCallIn(L, "AllowWeaponTarget");
		haveAllowWeaponInterceptTarget = HasCallIn(L, "AllowWeaponInterceptTarget");
		haveUnitPreDamaged             = HasCallIn(L, "UnitPreDamaged");
		haveFeaturePreDamaged          = HasCallIn(L, "FeaturePreDamaged");
		haveShieldPreDamaged           = HasCallIn(L, "ShieldPreDamaged");
	}
	if (SingleState() || L == L_Draw) {
		haveDrawUnit       = HasCallIn(L, "DrawUnit"      );
		haveDrawFeature    = HasCallIn(L, "DrawFeature"   );
		haveDrawShield     = HasCallIn(L, "DrawShield"    );
		haveDrawProjectile = HasCallIn(L, "DrawProjectile");

		SetupUnsyncedFunction(L, "DrawUnit");
		SetupUnsyncedFunction(L, "DrawFeature");
		SetupUnsyncedFunction(L, "DrawShield");
		SetupUnsyncedFunction(L, "DrawProjectile");
		SetupUnsyncedFunction(L, "RecvSkirmishAIMessage");
	}

	END_ITERATE_LUA_STATES();
}

CLuaRules::~CLuaRules()
{
	if (L_Sim != NULL || L_Draw != NULL) {
		Shutdown();
		KillLua();
	}

	assert(this == luaRules);
	assert(!IsValid());

	// make sure to really get rid of the LuaRules environment if we were
	// called from outside FreeHandler (see LuaHandle::KillActiveHandle())
	// note that ctor is only ever reached from LoadHandler
	if (killMe) {
		luaRules = NULL;
	}
}



bool CLuaRules::AddSyncedCode(lua_State *L)
{
	lua_getglobal(L, "Script");
	LuaPushNamedCFunc(L, "PermitHelperAIs", PermitHelperAIs);
	lua_pop(L, 1);

	return true;
}


bool CLuaRules::AddUnsyncedCode(lua_State *L)
{
	lua_pushliteral(L, "UNSYNCED");
	lua_gettable(L, LUA_REGISTRYINDEX);

	lua_pushliteral(L, "Spring");
	lua_rawget(L, -2);
	lua_pushliteral(L, "UnitRendering");
	lua_newtable(L);
	LuaUnitRendering::PushEntries(L);
	lua_rawset(L, -3);
	lua_pop(L, 1); // Spring

	lua_pop(L, 1); // UNSYNCED

	return true;
}


/******************************************************************************/
/******************************************************************************/
//
// LuaRules Call-Ins
//

bool CLuaRules::SyncedUpdateCallIn(lua_State *L, const string& name)
{
	#define UPDATE_HAVE_CALLIN(callinName) \
		have ## callinName = HasCallIn( L, #callinName );

	     if (name == "CommandFallback"       ) { UPDATE_HAVE_CALLIN(CommandFallback); }
	else if (name == "AllowCommand"          ) { UPDATE_HAVE_CALLIN(AllowCommand); }
	else if (name == "AllowUnitCreation"     ) { UPDATE_HAVE_CALLIN(AllowUnitCreation); }
	else if (name == "AllowUnitTransfer"     ) { UPDATE_HAVE_CALLIN(AllowUnitTransfer); }
	else if (name == "AllowUnitBuildStep"    ) { UPDATE_HAVE_CALLIN(AllowUnitBuildStep); }
	else if (name == "AllowFeatureCreation"  ) { UPDATE_HAVE_CALLIN(AllowFeatureCreation); }
	else if (name == "AllowFeatureBuildStep" ) { UPDATE_HAVE_CALLIN(AllowFeatureBuildStep); }
	else if (name == "AllowResourceLevel"    ) { UPDATE_HAVE_CALLIN(AllowResourceLevel); }
	else if (name == "AllowResourceTransfer" ) { UPDATE_HAVE_CALLIN(AllowResourceTransfer); }
	else if (name == "AllowDirectUnitControl") { UPDATE_HAVE_CALLIN(AllowDirectUnitControl); }
	else if (name == "AllowStartPosition"    ) { UPDATE_HAVE_CALLIN(AllowStartPosition); }
	else if (name == "MoveCtrlNotify"        ) { UPDATE_HAVE_CALLIN(MoveCtrlNotify); }
	else if (name == "TerraformComplete"     ) { UPDATE_HAVE_CALLIN(TerraformComplete); }
	else if (name == "UnitPreDamaged"        ) { UPDATE_HAVE_CALLIN(UnitPreDamaged); }
	else if (name == "FeaturePreDamaged"     ) { UPDATE_HAVE_CALLIN(FeaturePreDamaged); }
	else if (name == "ShieldPreDamaged"      ) { UPDATE_HAVE_CALLIN(ShieldPreDamaged); }
	else if (name == "AllowWeaponTargetCheck") { UPDATE_HAVE_CALLIN(AllowWeaponTargetCheck); }
	else if (name == "AllowWeaponTarget"     ) { UPDATE_HAVE_CALLIN(AllowWeaponTarget); }
	else {
		return CLuaHandleSynced::SyncedUpdateCallIn(L, name);
	}

	#undef UPDATE_HAVE_CALLIN
	return true;
}


bool CLuaRules::UnsyncedUpdateCallIn(lua_State *L, const string& name)
{
	     if (name == "DrawUnit"      ) { haveDrawUnit       = HasCallIn(L, "DrawUnit"      ); }
	else if (name == "DrawFeature"   ) { haveDrawFeature    = HasCallIn(L, "DrawFeature"   ); }
	else if (name == "DrawShield"    ) { haveDrawShield     = HasCallIn(L, "DrawShield"    ); }
	else if (name == "DrawProjectile") { haveDrawProjectile = HasCallIn(L, "DrawProjectile"); }

	return CLuaHandleSynced::UnsyncedUpdateCallIn(L, name);
}

/// pushes 7 items on the stack
static void PushUnitAndCommand(lua_State* L, const CUnit* unit, const Command& cmd)
{
	// push the unit info
	lua_pushnumber(L, unit->id);
	lua_pushnumber(L, unit->unitDef->id);
	lua_pushnumber(L, unit->team);

	// push the command id
	lua_pushnumber(L, cmd.GetID());

	// push the params list
	LuaUtils::PushCommandParamsTable(L, cmd, false);
	// push the options table
	LuaUtils::PushCommandOptionsTable(L, cmd, false);

	// push the command tag
	lua_pushnumber(L, cmd.tag);
}

bool CLuaRules::CommandFallback(const CUnit* unit, const Command& cmd)
{
	if (!haveCommandFallback)
		return true; // the call is not defined

	LUA_CALL_IN_CHECK(L, true);
	luaL_checkstack(L, 9, __FUNCTION__);
	static const LuaHashString cmdStr(__FUNCTION__);
	if (!cmdStr.GetGlobalFunc(L))
		return true; // the call is not defined

	PushUnitAndCommand(L, unit, cmd);

	// call the function
	if (!RunCallIn(cmdStr, 7, 1))
		return true;

	// get the results
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return value", cmdStr.GetString().c_str());
		lua_pop(L, 1);
		return true;
	}

	const bool retval = !!lua_toboolean(L, -1);
	lua_pop(L, 1);

	// return 'true' to remove the command
	return retval;
}


bool CLuaRules::AllowCommand(const CUnit* unit, const Command& cmd, bool fromSynced)
{
	if (!haveAllowCommand)
		return true; // the call is not defined

	LUA_CALL_IN_CHECK(L, true);
	luaL_checkstack(L, 10, __FUNCTION__);
	static const LuaHashString cmdStr(__FUNCTION__);
	if (!cmdStr.GetGlobalFunc(L))
		return true; // the call is not defined

	PushUnitAndCommand(L, unit, cmd);

	lua_pushboolean(L, fromSynced);

	// call the function
	if (!RunCallIn(cmdStr, 8, 1))
		return true;

	// get the results
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return value", cmdStr.GetString().c_str());
		lua_pop(L, 1);
		return true;
	}
	const bool retval = !!lua_toboolean(L, -1);
	lua_pop(L, 1);
	return retval;
}


bool CLuaRules::AllowUnitCreation(const UnitDef* unitDef,
                                  const CUnit* builder, const BuildInfo* buildInfo)
{
	if (!haveAllowUnitCreation)
		return true; // the call is not defined

	LUA_CALL_IN_CHECK(L, true);
	luaL_checkstack(L, 9, __FUNCTION__);
	static const LuaHashString cmdStr(__FUNCTION__);
	if (!cmdStr.GetGlobalFunc(L))
		return true; // the call is not defined

	lua_pushnumber(L, unitDef->id);
	lua_pushnumber(L, builder->id);
	lua_pushnumber(L, builder->team);

	if (buildInfo != NULL) {
		lua_pushnumber(L, buildInfo->pos.x);
		lua_pushnumber(L, buildInfo->pos.y);
		lua_pushnumber(L, buildInfo->pos.z);
		lua_pushnumber(L, buildInfo->buildFacing);
	}

	// call the function
	if (!RunCallIn(cmdStr, (buildInfo != NULL)? 7 : 3, 1))
		return true;

	// get the results
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return value", cmdStr.GetString().c_str());
		lua_pop(L, 1);
		return true;
	}
	const bool retval = !!lua_toboolean(L, -1);
	lua_pop(L, 1);
	return retval;
}



bool CLuaRules::AllowUnitTransfer(const CUnit* unit, int newTeam, bool capture)
{
	if (!haveAllowUnitTransfer)
		return true; // the call is not defined

	LUA_CALL_IN_CHECK(L, true);
	luaL_checkstack(L, 7, __FUNCTION__);
	static const LuaHashString cmdStr(__FUNCTION__);
	if (!cmdStr.GetGlobalFunc(L))
		return true; // the call is not defined

	lua_pushnumber(L, unit->id);
	lua_pushnumber(L, unit->unitDef->id);
	lua_pushnumber(L, unit->team);
	lua_pushnumber(L, newTeam);
	lua_pushboolean(L, capture);

	// call the function
	if (!RunCallIn(cmdStr, 5, 1))
		return true;

	// get the results
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return value", cmdStr.GetString().c_str());
		lua_pop(L, 1);
		return true;
	}

	const bool retval = !!lua_toboolean(L, -1);
	lua_pop(L, 1);
	return retval;
}


bool CLuaRules::AllowUnitBuildStep(const CUnit* builder,
                                   const CUnit* unit, float part)
{
	if (!haveAllowUnitBuildStep)
		return true; // the call is not defined

	LUA_CALL_IN_CHECK(L, true);
	luaL_checkstack(L, 7, __FUNCTION__);
	static const LuaHashString cmdStr(__FUNCTION__);
	if (!cmdStr.GetGlobalFunc(L))
		return true; // the call is not defined

	lua_pushnumber(L, builder->id);
	lua_pushnumber(L, builder->team);
	lua_pushnumber(L, unit->id);
	lua_pushnumber(L, unit->unitDef->id);
	lua_pushnumber(L, part);

	// call the function
	if (!RunCallIn(cmdStr, 5, 1))
		return true;

	// get the results
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return value", cmdStr.GetString().c_str());
		lua_pop(L, 1);
		return true;
	}

	const bool retval = !!lua_toboolean(L, -1);
	lua_pop(L, 1);
	return retval;
}


bool CLuaRules::AllowFeatureCreation(const FeatureDef* featureDef,
                                     int teamID, const float3& pos)
{
	if (!haveAllowFeatureCreation)
		return true; // the call is not defined

	LUA_CALL_IN_CHECK(L, true);
	luaL_checkstack(L, 7, __FUNCTION__);
	static const LuaHashString cmdStr(__FUNCTION__);
	if (!cmdStr.GetGlobalFunc(L))
		return true; // the call is not defined

	lua_pushnumber(L, featureDef->id);
	lua_pushnumber(L, teamID);
	lua_pushnumber(L, pos.x);
	lua_pushnumber(L, pos.y);
	lua_pushnumber(L, pos.z);

	// call the function
	if (!RunCallIn(cmdStr, 5, 1))
		return true;

	// get the results
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return value", cmdStr.GetString().c_str());
		lua_pop(L, 1);
		return true;
	}

	const bool retval = !!lua_toboolean(L, -1);
	lua_pop(L, 1);
	return retval;
}


bool CLuaRules::AllowFeatureBuildStep(const CUnit* builder,
                                      const CFeature* feature, float part)
{
	if (!haveAllowFeatureBuildStep)
		return true; // the call is not defined

	LUA_CALL_IN_CHECK(L, true);
	luaL_checkstack(L, 7, __FUNCTION__);
	static const LuaHashString cmdStr(__FUNCTION__);
	if (!cmdStr.GetGlobalFunc(L))
		return true; // the call is not defined

	lua_pushnumber(L, builder->id);
	lua_pushnumber(L, builder->team);
	lua_pushnumber(L, feature->id);
	lua_pushnumber(L, feature->def->id);
	lua_pushnumber(L, part);

	// call the function
	if (!RunCallIn(cmdStr, 5, 1))
		return true;

	// get the results
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return value", cmdStr.GetString().c_str());
		lua_pop(L, 1);
		return true;
	}

	const bool retval = !!lua_toboolean(L, -1);
	lua_pop(L, 1);
	return retval;
}


bool CLuaRules::AllowResourceLevel(int teamID, const string& type, float level)
{
	if (!haveAllowResourceLevel)
		return true; // the call is not defined

	LUA_CALL_IN_CHECK(L, true);
	luaL_checkstack(L, 5, __FUNCTION__);
	static const LuaHashString cmdStr(__FUNCTION__);
	if (!cmdStr.GetGlobalFunc(L))
		return true; // the call is not defined

	lua_pushnumber(L, teamID);
	lua_pushsstring(L, type);
	lua_pushnumber(L, level);

	// call the function
	if (!RunCallIn(cmdStr, 3, 1))
		return true;

	// get the results
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return value", cmdStr.GetString().c_str());
		lua_pop(L, 1);
		return true;
	}

	const bool retval = !!lua_toboolean(L, -1);
	lua_pop(L, 1);
	return retval;
}


bool CLuaRules::AllowResourceTransfer(int oldTeam, int newTeam,
                                      const string& type, float amount)
{
	if (!haveAllowResourceTransfer)
		return true; // the call is not defined

	LUA_CALL_IN_CHECK(L, true);
	luaL_checkstack(L, 6, __FUNCTION__);
	static const LuaHashString cmdStr(__FUNCTION__);
	if (!cmdStr.GetGlobalFunc(L))
		return true; // the call is not defined

	lua_pushnumber(L, oldTeam);
	lua_pushnumber(L, newTeam);
	lua_pushsstring(L, type);
	lua_pushnumber(L, amount);

	// call the function
	if (!RunCallIn(cmdStr, 4, 1))
		return true;

	// get the results
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return value", cmdStr.GetString().c_str());
		lua_pop(L, 1);
		return true;
	}

	const bool retval = !!lua_toboolean(L, -1);
	lua_pop(L, 1);
	return retval;
}


bool CLuaRules::AllowDirectUnitControl(int playerID, const CUnit* unit)
{
	if (!haveAllowDirectUnitControl)
		return true; // the call is not defined

	LUA_CALL_IN_CHECK(L, true);
	luaL_checkstack(L, 6, __FUNCTION__);
	static const LuaHashString cmdStr(__FUNCTION__);
	if (!cmdStr.GetGlobalFunc(L))
		return true; // the call is not defined

	lua_pushnumber(L, unit->id);
	lua_pushnumber(L, unit->unitDef->id);
	lua_pushnumber(L, unit->team);
	lua_pushnumber(L, playerID);

	// call the function
	if (!RunCallIn(cmdStr, 4, 1))
		return true;

	// get the results
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return value", cmdStr.GetString().c_str());
		lua_pop(L, 1);
		return true;
	}

	const bool retval = !!lua_toboolean(L, -1);
	lua_pop(L, 1);
	return retval;
}


bool CLuaRules::AllowStartPosition(int playerID, unsigned char readyState, const float3& clampedPos, const float3& rawPickPos)
{
	if (!haveAllowStartPosition)
		return true; // the call is not defined

	LUA_CALL_IN_CHECK(L, true);
	luaL_checkstack(L, 13, __FUNCTION__);

	static const LuaHashString cmdStr(__FUNCTION__);

	if (!cmdStr.GetGlobalFunc(L))
		return true; // the call is not defined

	// push the start position and playerID
	lua_pushnumber(L, clampedPos.x);
	lua_pushnumber(L, clampedPos.y);
	lua_pushnumber(L, clampedPos.z);
	lua_pushnumber(L, playerID);
	lua_pushnumber(L, readyState);
	lua_pushnumber(L, rawPickPos.x);
	lua_pushnumber(L, rawPickPos.y);
	lua_pushnumber(L, rawPickPos.z);

	// call the function
	if (!RunCallIn(cmdStr, 8, 1))
		return true;

	// get the results
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return value", cmdStr.GetString().c_str());
		lua_pop(L, 1);
		return true;
	}
	const bool retval = !!lua_toboolean(L, -1);
	lua_pop(L, 1);
	return retval;
}


bool CLuaRules::MoveCtrlNotify(const CUnit* unit, int data)
{
	if (!haveMoveCtrlNotify)
		return false; // the call is not defined

	LUA_CALL_IN_CHECK(L, false);
	luaL_checkstack(L, 6, __FUNCTION__);
	static const LuaHashString cmdStr(__FUNCTION__);
	if (!cmdStr.GetGlobalFunc(L))
		return false; // the call is not defined

	// push the unit info
	lua_pushnumber(L, unit->id);
	lua_pushnumber(L, unit->unitDef->id);
	lua_pushnumber(L, unit->team);
	lua_pushnumber(L, data);

	// call the function
	if (!RunCallIn(cmdStr, 4, 1))
		return false;

	// get the results
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return value", cmdStr.GetString().c_str());
		lua_pop(L, 1);
		return false;
	}

	const bool retval = !!lua_toboolean(L, -1);
	lua_pop(L, 1);

	return retval;
}


bool CLuaRules::TerraformComplete(const CUnit* unit, const CUnit* build)
{
	if (!haveTerraformComplete)
		return false; // the call is not defined

	LUA_CALL_IN_CHECK(L, false);
	luaL_checkstack(L, 8, __FUNCTION__);
	static const LuaHashString cmdStr(__FUNCTION__);
	if (!cmdStr.GetGlobalFunc(L))
		return false; // the call is not defined

	// push the unit info
	lua_pushnumber(L, unit->id);
	lua_pushnumber(L, unit->unitDef->id);
	lua_pushnumber(L, unit->team);

	// push the construction info
	lua_pushnumber(L, build->id);
	lua_pushnumber(L, build->unitDef->id);
	lua_pushnumber(L, build->team);

	// call the function
	if (!RunCallIn(cmdStr, 6, 1))
		return false;

	// get the results
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return value", cmdStr.GetString().c_str());
		lua_pop(L, 1);
		return false;
	}

	const bool retval = !!lua_toboolean(L, -1);
	lua_pop(L, 1);

	// return 'true' to remove the command
	return retval;
}


/**
 * called after every damage modification (even HitByWeaponId)
 * but before the damage is applied
 *
 * expects two numbers returned by lua code:
 * 1st is stored under *newDamage if newDamage != NULL
 * 2nd is stored under *impulseMult if impulseMult != NULL
 */
bool CLuaRules::UnitPreDamaged(
	const CUnit* unit,
	const CUnit* attacker,
	float damage,
	int weaponDefID,
	int projectileID,
	bool paralyzer,
	float* newDamage,
	float* impulseMult)
{
	if (!haveUnitPreDamaged)
		return false;

	LUA_CALL_IN_CHECK(L, false);
	luaL_checkstack(L, 2 + 2 + 10, __FUNCTION__);

	static const LuaHashString cmdStr(__FUNCTION__);
	const LuaUtils::ScopedDebugTraceBack traceBack(L);

	if (!cmdStr.GetGlobalFunc(L))
		return false;

	int inArgCount = 5;
	int outArgCount = 2;

	lua_pushnumber(L, unit->id);
	lua_pushnumber(L, unit->unitDef->id);
	lua_pushnumber(L, unit->team);
	lua_pushnumber(L, damage);
	lua_pushboolean(L, paralyzer);
	//FIXME pass impulse too?

	if (GetHandleFullRead(L)) {
		lua_pushnumber(L, weaponDefID); inArgCount += 1;
		lua_pushnumber(L, projectileID); inArgCount += 1;

		if (attacker != NULL) {
			lua_pushnumber(L, attacker->id);
			lua_pushnumber(L, attacker->unitDef->id);
			lua_pushnumber(L, attacker->team);
			inArgCount += 3;
		}
	}

	// call the routine
	// NOTE:
	//   RunCallInTraceback removes the error-handler by default
	//   this has to be disabled when using ScopedDebugTraceBack
	//   or it would mess up the stack
	if (!RunCallInTraceback(cmdStr, inArgCount, outArgCount, traceBack.GetErrFuncIdx(), false))
		return false;

	if (newDamage && lua_isnumber(L, -2)) {
		*newDamage = lua_tonumber(L, -2);
	} else if (!lua_isnumber(L, -2) || lua_isnil(L, -2)) {
		// first value is obligatory, so may not be nil
		LOG_L(L_WARNING, "%s(): 1st return-value should be a number (newDamage)", (cmdStr.GetString()).c_str());
	}

	if (impulseMult && lua_isnumber(L, -1)) {
		*impulseMult = lua_tonumber(L, -1);
	} else if (!lua_isnumber(L, -1) && !lua_isnil(L, -1)) {
		// second value is optional, so nils are OK
		LOG_L(L_WARNING, "%s(): 2nd return-value should be a number (impulseMult)", (cmdStr.GetString()).c_str());
	}

	lua_pop(L, outArgCount);
	return true;
}

bool CLuaRules::FeaturePreDamaged(
	const CFeature* feature,
	const CUnit* attacker,
	float damage,
	int weaponDefID,
	int projectileID,
	float* newDamage,
	float* impulseMult)
{
	if (!haveFeaturePreDamaged)
		return false;

	LUA_CALL_IN_CHECK(L, false);
	luaL_checkstack(L, 2 + 9 + 2, __FUNCTION__);

	static const LuaHashString cmdStr(__FUNCTION__);
	const LuaUtils::ScopedDebugTraceBack traceBack(L);

	if (!cmdStr.GetGlobalFunc(L))
		return false;

	int inArgCount = 4;
	int outArgCount = 2;

	lua_pushnumber(L, feature->id);
	lua_pushnumber(L, feature->def->id);
	lua_pushnumber(L, feature->team);
	lua_pushnumber(L, damage);

	if (GetHandleFullRead(L)) {
		lua_pushnumber(L, weaponDefID); inArgCount += 1;
		lua_pushnumber(L, projectileID); inArgCount += 1;

		if (attacker != NULL) {
			lua_pushnumber(L, attacker->id);
			lua_pushnumber(L, attacker->unitDef->id);
			lua_pushnumber(L, attacker->team);
			inArgCount += 3;
		}
	}

	// call the routine
	if (!RunCallInTraceback(cmdStr, inArgCount, outArgCount, traceBack.GetErrFuncIdx(), false))
		return false;

	if (newDamage && lua_isnumber(L, -2)) {
		*newDamage = lua_tonumber(L, -2);
	} else if (!lua_isnumber(L, -2) || lua_isnil(L, -2)) {
		// first value is obligatory, so may not be nil
		LOG_L(L_WARNING, "%s(): 1st value returned should be a number (newDamage)", (cmdStr.GetString()).c_str());
	}

	if (impulseMult && lua_isnumber(L, -1)) {
		*impulseMult = lua_tonumber(L, -1);
	} else if (!lua_isnumber(L, -1) && !lua_isnil(L, -1)) {
		// second value is optional, so nils are OK
		LOG_L(L_WARNING, "%s(): 2nd value returned should be a number (impulseMult)", (cmdStr.GetString()).c_str());
	}

	lua_pop(L, outArgCount);
	return true;
}

bool CLuaRules::ShieldPreDamaged(
	const CProjectile* projectile,
	const CWeapon* shieldEmitter,
	const CUnit* shieldCarrier,
	bool bounceProjectile
) {
	if (!haveShieldPreDamaged)
		return false;

	LUA_CALL_IN_CHECK(L, false);
	luaL_checkstack(L, 2 + 5 + 1, __FUNCTION__);

	static const LuaHashString cmdStr(__FUNCTION__);
	const LuaUtils::ScopedDebugTraceBack traceBack(L);

	if (!cmdStr.GetGlobalFunc(L))
		return false;

	// push the call-in arguments
	lua_pushnumber(L, projectile->id);
	lua_pushnumber(L, projectile->GetOwnerID());
	lua_pushnumber(L, shieldEmitter->weaponNum);
	lua_pushnumber(L, shieldCarrier->id);
	lua_pushboolean(L, bounceProjectile);

	// call the routine
	if (!RunCallInTraceback(cmdStr, 5, 1, traceBack.GetErrFuncIdx(), false))
		return false;

	// pop the return-value; must be true or false
	const bool ret = (lua_isboolean(L, -1) && lua_toboolean(L, -1));
	lua_pop(L, 1);
	return ret;
}



/******************************************************************************/

int CLuaRules::AllowWeaponTargetCheck(unsigned int attackerID, unsigned int attackerWeaponNum, unsigned int attackerWeaponDefID)
{
	if (!haveAllowWeaponTargetCheck)
		return -1;
	if (!watchWeaponDefs[attackerWeaponDefID])
		return -1;

	LUA_CALL_IN_CHECK(L, -1);
	luaL_checkstack(L, 2 + 3 + 1, __FUNCTION__);

	static const LuaHashString cmdStr(__FUNCTION__);
	const LuaUtils::ScopedDebugTraceBack traceBack(L);

	int ret = -1;

	if (cmdStr.GetGlobalFunc(L)) {
		lua_pushnumber(L, attackerID);
		lua_pushnumber(L, attackerWeaponNum);
		lua_pushnumber(L, attackerWeaponDefID);

		if (!RunCallInTraceback(cmdStr, 3, 1, traceBack.GetErrFuncIdx(), false))
			return ret;

		ret = int(lua_isboolean(L, -1) && lua_toboolean(L, -1));
		lua_pop(L, 1);
	}

	return ret;
}

bool CLuaRules::AllowWeaponTarget(
	unsigned int attackerID,
	unsigned int targetID,
	unsigned int attackerWeaponNum,
	unsigned int attackerWeaponDefID,
	float* targetPriority)
{
	assert(targetPriority != NULL);

	bool ret = true;

	if (!haveAllowWeaponTarget)
		return ret;
	if (!watchWeaponDefs[attackerWeaponDefID])
		return ret;

	LUA_CALL_IN_CHECK(L, true);
	luaL_checkstack(L, 2 + 5 + 2, __FUNCTION__);

	static const LuaHashString cmdStr(__FUNCTION__);
	const LuaUtils::ScopedDebugTraceBack traceBack(L);

	if (cmdStr.GetGlobalFunc(L)) {
		lua_pushnumber(L, attackerID);
		lua_pushnumber(L, targetID);
		lua_pushnumber(L, attackerWeaponNum);
		lua_pushnumber(L, attackerWeaponDefID);
		lua_pushnumber(L, *targetPriority);

		if (!RunCallInTraceback(cmdStr, 5, 2, traceBack.GetErrFuncIdx(), false))
			return ret;

		ret = (lua_isboolean(L, -2) && lua_toboolean(L, -2));

		if (lua_isnumber(L, -1)) {
			*targetPriority = lua_tonumber(L, -1);
		}

		lua_pop(L, 2);
	}

	return ret;
}

bool CLuaRules::AllowWeaponInterceptTarget(
	const CUnit* interceptorUnit,
	const CWeapon* interceptorWeapon,
	const CProjectile* interceptorTarget
) {
	bool ret = true;

	if (!haveAllowWeaponInterceptTarget)
		return ret;
	if (!watchWeaponDefs[interceptorWeapon->weaponDef->id])
		return ret;

	LUA_CALL_IN_CHECK(L, true);
	luaL_checkstack(L, 2 + 3 + 1, __FUNCTION__);

	static const LuaHashString cmdStr(__FUNCTION__);
	const LuaUtils::ScopedDebugTraceBack traceBack(L);

	if (cmdStr.GetGlobalFunc(L)) {
		lua_pushnumber(L, interceptorUnit->id);
		lua_pushnumber(L, interceptorWeapon->weaponNum);
		lua_pushnumber(L, interceptorTarget->id);

		if (!RunCallInTraceback(cmdStr, 3, 1, traceBack.GetErrFuncIdx(), false))
			return ret;

		ret = (lua_isboolean(L, -1) && lua_toboolean(L, -1));
		lua_pop(L, 1);
	}

	return ret;
}

/******************************************************************************/



bool CLuaRules::DrawUnit(const CUnit* unit)
{
	if (!haveDrawUnit)
		return false;

	LUA_CALL_IN_CHECK(L, false);
	luaL_checkstack(L, 4, __FUNCTION__);

	static const LuaHashString cmdStr(__FUNCTION__);

	if (!cmdStr.GetRegistryFunc(L))
		return false;

	const bool oldDrawState = LuaOpenGL::IsDrawingEnabled(L);
	LuaOpenGL::SetDrawingEnabled(L, true);

	lua_pushnumber(L, unit->id);
	lua_pushnumber(L, game->GetDrawMode());

	const bool success = RunCallIn(cmdStr, 2, 1);
	LuaOpenGL::SetDrawingEnabled(L, oldDrawState);

	if (!success)
		return false;
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return-type (bool expected, got %s)", __FUNCTION__, lua_typename(L, lua_type(L, -1)));
		lua_pop(L, 1);
		return false;
	}

	const bool retval = !!lua_toboolean(L, -1);

	lua_pop(L, 1);
	return retval;
}

bool CLuaRules::DrawFeature(const CFeature* feature)
{
	if (!haveDrawFeature)
		return false;

	LUA_CALL_IN_CHECK(L, false);
	luaL_checkstack(L, 4, __FUNCTION__);

	static const LuaHashString cmdStr(__FUNCTION__);

	if (!cmdStr.GetRegistryFunc(L))
		return false;

	const bool oldDrawState = LuaOpenGL::IsDrawingEnabled(L);
	LuaOpenGL::SetDrawingEnabled(L, true);

	lua_pushnumber(L, feature->id);
	lua_pushnumber(L, game->GetDrawMode());

	const bool success = RunCallIn(cmdStr, 2, 1);
	LuaOpenGL::SetDrawingEnabled(L, oldDrawState);

	if (!success)
		return false;
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return-type (bool expected, got %s)", __FUNCTION__, lua_typename(L, lua_type(L, -1)));
		lua_pop(L, 1);
		return false;
	}

	const bool retval = !!lua_toboolean(L, -1);

	lua_pop(L, 1);
	return retval;
}

bool CLuaRules::DrawShield(const CUnit* unit, const CWeapon* weapon)
{
	if (!haveDrawShield)
		return false;

	LUA_CALL_IN_CHECK(L, false);
	luaL_checkstack(L, 5, __FUNCTION__);

	static const LuaHashString cmdStr(__FUNCTION__);

	if (!cmdStr.GetRegistryFunc(L))
		return false;

	const bool oldDrawState = LuaOpenGL::IsDrawingEnabled(L);
	LuaOpenGL::SetDrawingEnabled(L, true);

	lua_pushnumber(L, unit->id);
	lua_pushnumber(L, weapon->weaponNum);
	lua_pushnumber(L, game->GetDrawMode());

	const bool success = RunCallIn(cmdStr, 3, 1);
	LuaOpenGL::SetDrawingEnabled(L, oldDrawState);

	if (!success)
		return false;
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return-type (bool expected, got %s)", __FUNCTION__, lua_typename(L, lua_type(L, -1)));
		lua_pop(L, 1);
		return false;
	}

	const bool retval = !!lua_toboolean(L, -1);

	lua_pop(L, 1);
	return retval;
}

bool CLuaRules::DrawProjectile(const CProjectile* projectile)
{
	if (!haveDrawProjectile)
		return false;
	if (!(projectile->weapon || projectile->piece))
		return false;

	LUA_CALL_IN_CHECK(L, false);
	luaL_checkstack(L, 5, __FUNCTION__);

	static const LuaHashString cmdStr(__FUNCTION__);

	if (!cmdStr.GetRegistryFunc(L))
		return false;

	const bool oldDrawState = LuaOpenGL::IsDrawingEnabled(L);
	LuaOpenGL::SetDrawingEnabled(L, true);

	lua_pushnumber(L, projectile->id);
	lua_pushnumber(L, game->GetDrawMode());

	const bool success = RunCallIn(cmdStr, 2, 1);
	LuaOpenGL::SetDrawingEnabled(L, oldDrawState);

	if (!success)
		return false;
	if (!lua_isboolean(L, -1)) {
		LOG_L(L_WARNING, "%s() bad return-type (bool expected, got %s)", __FUNCTION__, lua_typename(L, lua_type(L, -1)));
		lua_pop(L, 1);
		return false;
	}

	const bool retval = !!lua_toboolean(L, -1);

	lua_pop(L, 1);
	return retval;
}



/******************************************************************************/
/******************************************************************************/

int CLuaRules::UnpackCobArg(lua_State* L)
{
	if (currentCobArgs == NULL) {
		luaL_error(L, "Error in UnpackCobArg(), no current args");
	}
	const int arg = luaL_checkint(L, 1) - 1;
	if ((arg < 0) || (arg >= MAX_LUA_COB_ARGS)) {
		luaL_error(L, "Error in UnpackCobArg(), bad index");
	}
	const int value = currentCobArgs[arg];
	lua_pushnumber(L, UNPACKX(value));
	lua_pushnumber(L, UNPACKZ(value));
	return 2;
}


void CLuaRules::Cob2Lua(const LuaHashString& name, const CUnit* unit,
                        int& argsCount, int args[MAX_LUA_COB_ARGS])
{
	static int callDepth = 0;
	if (callDepth >= 16) {
		LOG_L(L_WARNING, "CLuaRules::Cob2Lua() call overflow: %s",
				name.GetString().c_str());
		args[0] = 0; // failure
		return;
	}

	LUA_CALL_IN_CHECK(L);

	const int top = lua_gettop(L);

	if (!lua_checkstack(L, 1 + 3 + argsCount)) {
		LOG_L(L_WARNING, "CLuaRules::Cob2Lua() lua_checkstack() error: %s",
				name.GetString().c_str());
		args[0] = 0; // failure
		lua_settop(L, top);
		return;
	}

	if (!name.GetGlobalFunc(L)) {
		LOG_L(L_WARNING, "CLuaRules::Cob2Lua() missing function: %s",
				name.GetString().c_str());
		args[0] = 0; // failure
		lua_settop(L, top);
		return;
	}

	lua_pushnumber(L, unit->id);
	lua_pushnumber(L, unit->unitDef->id);
	lua_pushnumber(L, unit->team);
	for (int a = 0; a < argsCount; a++) {
		lua_pushnumber(L, args[a]);
	}

	// call the routine
	callDepth++;
	const int* oldArgs = currentCobArgs;
	currentCobArgs = args;

	const bool error = !RunCallIn(name, 3 + argsCount, LUA_MULTRET);

	currentCobArgs = oldArgs;
	callDepth--;

	// bail on error
	if (error) {
		args[0] = 0; // failure
		lua_settop(L, top);
		return;
	}

	// get the results
	const int retArgs = std::min(lua_gettop(L) - top, (MAX_LUA_COB_ARGS - 1));
	for (int a = 1; a <= retArgs; a++) {
		const int index = (a + top);
		if (lua_isnumber(L, index)) {
			args[a] = lua_toint(L, index);
		}
		else if (lua_isboolean(L, index)) {
			args[a] = lua_toboolean(L, index) ? 1 : 0;
		}
		else if (lua_istable(L, index)) {
			lua_rawgeti(L, index, 1);
			lua_rawgeti(L, index, 2);
			if (lua_isnumber(L, -2) && lua_isnumber(L, -1)) {
				const int x = lua_toint(L, -2);
				const int z = lua_toint(L, -1);
				args[a] = PACKXZ(x, z);
			} else {
				args[a] = 0;
			}
			lua_pop(L, 2);
		}
		else {
			args[a] = 0;
		}
	}

	args[0] = 1; // success
	lua_settop(L, top);
	return;
}


/******************************************************************************/
/******************************************************************************/
//
// LuaRules Call-Outs
//

int CLuaRules::PermitHelperAIs(lua_State* L)
{
	if (!lua_isboolean(L, 1)) {
		luaL_error(L, "Incorrect argument to PermitHelperAIs()");
	}
	gs->noHelperAIs = !lua_toboolean(L, 1);
	LOG("LuaRules has %s helper AIs",
			(gs->noHelperAIs ? "disabled" : "enabled"));
	return 0;
}

/******************************************************************************/
/******************************************************************************/
