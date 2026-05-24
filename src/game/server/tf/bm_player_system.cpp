//========= Copyright Valve Corporation, All rights reserved. ============//
#include "cbase.h"

#ifdef SOURCESDK

#include "bm_player_system.h"
#include "bm_shareddefs.h"
#include "bm_grid.h"
#include "tf_bm_bomb.h"
#include "tf_bm_crate.h"
#include "tf_bm_wall.h"
#include "bm_arena.h"

extern ConVar tf_bm_hard_walls;
extern ConVar tf_bm_maze_crates;
#include "tf_player.h"
#include "tf_gamerules.h"
#include "tf_weaponbase.h"
#include "in_buttons.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern ConVar tf_bm_bomb_fuse;
extern ConVar tf_bm_bomb_range;
extern ConVar tf_bm_max_bombs;

ConVar tf_bm_cell_size( "tf_bm_cell_size", "64", FCVAR_REPLICATED | FCVAR_NOTIFY, "Bomberman: grid cell size in Hammer units." );
ConVar tf_bm_grid_origin( "tf_bm_grid_origin", "0 0 0", FCVAR_REPLICATED | FCVAR_NOTIFY, "Bomberman: cell (0,0) world origin (server-written on build; do not set manually)." );
ConVar tf_bm_sky_arena( "tf_bm_sky_arena", "0", FCVAR_REPLICATED | FCVAR_NOTIFY, "Bomberman: legacy sky layer above the map (0=ground floor on itemtest)." );
ConVar tf_bm_sky_height( "tf_bm_sky_height", "3072", FCVAR_REPLICATED | FCVAR_NOTIFY, "Bomberman: sky play plane height above map reference Z." );
ConVar tf_bm_play_z_offset( "tf_bm_play_z_offset", "8", FCVAR_REPLICATED | FCVAR_NOTIFY, "Bomberman: player feet offset above grid origin Z." );
ConVar tf_bm_floor_drop( "tf_bm_floor_drop", "288", FCVAR_REPLICATED | FCVAR_NOTIFY, "itemtest: legacy target depth below spawn (auto uses floor_deck)." );
ConVar tf_bm_floor_deck( "tf_bm_floor_deck", "1", FCVAR_REPLICATED | FCVAR_NOTIFY, "itemtest: which floor below spawns (0=first, 1=basement deck, 2=deeper)." );
ConVar tf_bm_floor_z_override( "tf_bm_floor_z_override", "-135", FCVAR_REPLICATED | FCVAR_NOTIFY, "Grid floor Z. -135 = room floor (~feet at -127). 0 = auto trace." );
ConVar tf_bm_arena_center_x( "tf_bm_arena_center_x", "1664", FCVAR_REPLICATED | FCVAR_NOTIFY, "itemtest: unused — grid uses tf_bm_room_* / hardcoded play room." );
ConVar tf_bm_arena_center_y( "tf_bm_arena_center_y", "-1408", FCVAR_REPLICATED | FCVAR_NOTIFY, "itemtest: unused — grid uses tf_bm_room_* / hardcoded play room." );
ConVar tf_bm_room_min_x( "tf_bm_room_min_x", "1304.03125", FCVAR_REPLICATED | FCVAR_NOTIFY, "itemtest play room AABB min X (SE/SW corners)." );
ConVar tf_bm_room_min_y( "tf_bm_room_min_y", "-2535.97412", FCVAR_REPLICATED | FCVAR_NOTIFY, "itemtest play room AABB min Y (south corners)." );
ConVar tf_bm_room_max_x( "tf_bm_room_max_x", "2023.96875", FCVAR_REPLICATED | FCVAR_NOTIFY, "itemtest play room AABB max X (NE/NW corners)." );
ConVar tf_bm_room_max_y( "tf_bm_room_max_y", "-280.03979", FCVAR_REPLICATED | FCVAR_NOTIFY, "itemtest play room AABB max Y (north corners)." );
ConVar tf_bm_move_speed( "tf_bm_move_speed", "320", FCVAR_REPLICATED | FCVAR_NOTIFY, "Bomberman: movement speed along one axis." );
ConVar tf_bm_arena_lock( "tf_bm_arena_lock", "0", FCVAR_REPLICATED | FCVAR_NOTIFY, "1=enforce arena warp when leaving play area. 0=off." );
ConVar tf_bm_free_move( "tf_bm_free_move", "1", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"1=standard letgo: no floor snap / grid clamp (bm_lock overrides per player)." );
ConVar tf_bm_grid_move( "tf_bm_grid_move", "0", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"1=classic 4-way grid steps. 0=normal TF movement in the play area (itemtest default)." );
ConVar tf_bm_ffa( "tf_bm_ffa", "1", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"Bomberman: free-for-all (1=everyone can hurt everyone except self; teams only used for joining)." );
//-----------------------------------------------------------------------------
bool BM_IsFreeForAll( void )
{
	return tf_bm_ffa.GetBool();
}

//-----------------------------------------------------------------------------
int BM_GetPlayerSpawnSlot( CTFPlayer *pPlayer )
{
	if ( !pPlayer )
	{
		return 0;
	}

	int iSlot = 0;
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *pOther = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( !pOther || !pOther->IsConnected() || pOther == pPlayer )
		{
			continue;
		}

		if ( BM_IsFreeForAll() )
		{
			if ( pOther->GetTeamNumber() >= FIRST_GAME_TEAM && pOther->GetUserID() < pPlayer->GetUserID() )
			{
				++iSlot;
			}
		}
		else if ( pOther->GetTeamNumber() == pPlayer->GetTeamNumber() && pOther->GetUserID() < pPlayer->GetUserID() )
		{
			++iSlot;
		}
	}

	if ( BM_IsFreeForAll() )
	{
		return clamp( iSlot, 0, BM_MAX_FFA_PLAYERS - 1 );
	}

	return clamp( iSlot, 0, BM_MAX_SPAWN_SLOTS_PER_TEAM - 1 );
}

//-----------------------------------------------------------------------------
void BM_EnsurePlayerJoinedMatch( CTFPlayer *pPlayer )
{
	if ( !pPlayer || !BM_IsFreeForAll() )
	{
		return;
	}

	if ( pPlayer->GetTeamNumber() >= FIRST_GAME_TEAM )
	{
		return;
	}

	pPlayer->ChangeTeam( TF_TEAM_RED, true, true );
}
static bool s_bBMGridAligned = false;
static bool s_bBMLetGo[MAX_PLAYERS + 1];
static bool s_bBMForceLock[MAX_PLAYERS + 1];

//-----------------------------------------------------------------------------
static void BM_ReleaseArenaMovementLocks( CTFPlayer *pPlayer )
{
	if ( !pPlayer )
	{
		return;
	}

	pPlayer->SetMoveType( MOVETYPE_WALK );
	pPlayer->SetGravity( 1.0f );
	pPlayer->RemoveFlag( FL_FLY );
	pPlayer->SetGroundEntity( NULL );
}

//-----------------------------------------------------------------------------
bool BM_UseGridMovement( CTFPlayer *pPlayer )
{
	if ( !pPlayer || !pPlayer->IsAlive() || !TFGameRules() || !TFGameRules()->IsBombermanMode() )
	{
		return false;
	}

	if ( BM_IsPlayerMovementUnlocked( pPlayer ) )
	{
		return false;
	}

	return tf_bm_grid_move.GetBool();
}

//-----------------------------------------------------------------------------
bool BM_IsPlayerMovementUnlocked( CTFPlayer *pPlayer )
{
	if ( !pPlayer || !pPlayer->IsAlive() )
	{
		return false;
	}

	if ( !TFGameRules() || !TFGameRules()->IsBombermanMode() )
	{
		return true;
	}

	const int iIndex = pPlayer->entindex();
	if ( iIndex >= 0 && iIndex <= MAX_PLAYERS && s_bBMForceLock[iIndex] )
	{
		return false;
	}

	// Standard letgo: no per-frame floor snap (stops floor bounce). bm_lock sets force-lock.
	if ( tf_bm_free_move.GetBool() )
	{
		return true;
	}

	if ( iIndex >= 0 && iIndex <= MAX_PLAYERS && s_bBMLetGo[iIndex] )
	{
		return true;
	}

	if ( pPlayer->GetMoveType() == MOVETYPE_NOCLIP || pPlayer->GetMoveType() == MOVETYPE_OBSERVER )
	{
		return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
void BM_SetPlayerMovementUnlocked( CTFPlayer *pPlayer, bool bUnlocked )
{
	if ( !pPlayer )
	{
		return;
	}

	const int iIndex = pPlayer->entindex();
	if ( iIndex < 0 || iIndex > MAX_PLAYERS )
	{
		return;
	}

	s_bBMLetGo[iIndex] = bUnlocked;
	if ( bUnlocked )
	{
		s_bBMForceLock[iIndex] = false;
		BM_ReleaseArenaMovementLocks( pPlayer );
	}
}

//-----------------------------------------------------------------------------
void BM_ApplyDefaultFreeMove( CTFPlayer *pPlayer )
{
	if ( !pPlayer || !TFGameRules() || !TFGameRules()->IsBombermanMode() )
	{
		return;
	}

	if ( tf_bm_free_move.GetBool() )
	{
		BM_SetPlayerMovementUnlocked( pPlayer, true );
	}
}

//-----------------------------------------------------------------------------
void BM_ReleaseAllPlayersForFreeMove( void )
{
	if ( !TFGameRules() || !TFGameRules()->IsBombermanMode() )
	{
		return;
	}

	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *pPlayer = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( pPlayer && pPlayer->IsConnected() )
		{
			BM_ApplyDefaultFreeMove( pPlayer );
		}
	}
}

//-----------------------------------------------------------------------------
void BM_MarkGridAligned( void )
{
	s_bBMGridAligned = true;
}

//-----------------------------------------------------------------------------
void BM_GetGridOrigin( Vector &vecGridOrigin )
{
	sscanf( tf_bm_grid_origin.GetString(), "%f %f %f", &vecGridOrigin.x, &vecGridOrigin.y, &vecGridOrigin.z );
}

//-----------------------------------------------------------------------------
float BM_GetCellSize( void )
{
	return clamp( tf_bm_cell_size.GetFloat(), 32.0f, 128.0f );
}

//-----------------------------------------------------------------------------
bool BM_UseSkyPlayPlane( void )
{
	return tf_bm_sky_arena.GetBool();
}

//-----------------------------------------------------------------------------
bool BM_UseIsolatedPlayPlane( void )
{
	return BM_UseSkyPlayPlane() || BM_GetEffectiveArenaLift() > 0.0f || BM_UseVoidArenaPlatform();
}

//-----------------------------------------------------------------------------
float BM_GetPlayPlaneZ( void )
{
	Vector vecGridOrigin;
	BM_GetGridOrigin( vecGridOrigin );
	return vecGridOrigin.z + tf_bm_play_z_offset.GetFloat();
}

//-----------------------------------------------------------------------------
void BM_WorldToCell( const Vector &vecWorld, int &iCellX, int &iCellY )
{
	Vector vecGridOrigin;
	BM_GetGridOrigin( vecGridOrigin );
	const float flCell = BM_GetCellSize();

	iCellX = Floor2Int( ( vecWorld.x - vecGridOrigin.x ) / flCell );
	iCellY = Floor2Int( ( vecWorld.y - vecGridOrigin.y ) / flCell );
}

//-----------------------------------------------------------------------------
void BM_CellToWorldCenter( int iCellX, int iCellY, Vector &vecCenter )
{
	Vector vecGridOrigin;
	BM_GetGridOrigin( vecGridOrigin );
	const float flCell = BM_GetCellSize();

	vecCenter.x = vecGridOrigin.x + ( iCellX + 0.5f ) * flCell;
	vecCenter.y = vecGridOrigin.y + ( iCellY + 0.5f ) * flCell;
	vecCenter.z = BM_GetPlayPlaneZ();
}

//-----------------------------------------------------------------------------
static void BM_CollectFloorHeightsBelow( const Vector &vecXY, float flSpawnZ, CTFPlayer *pPlayer, CUtlVector<float> &vecFloors )
{
	vecFloors.RemoveAll();

	float flZ = flSpawnZ + 192.0f;
	float flLastHitZ = flSpawnZ + 512.0f;

	for ( int iStep = 0; iStep < 48 && flZ > -4096.0f; ++iStep )
	{
		Vector vecStart( vecXY.x, vecXY.y, flZ );
		Vector vecEnd( vecXY.x, vecXY.y, flZ - 128.0f );

		trace_t trace;
		UTIL_TraceHull( vecStart, vecEnd, VEC_HULL_MIN, VEC_HULL_MAX, MASK_PLAYERSOLID, pPlayer, COLLISION_GROUP_PLAYER, &trace );

		if ( trace.DidHit() && trace.endpos.z < flLastHitZ - 20.0f )
		{
			vecFloors.AddToTail( trace.endpos.z );
			flLastHitZ = trace.endpos.z;
			flZ = trace.endpos.z - 48.0f;
		}
		else
		{
			flZ -= 128.0f;
		}
	}
}

//-----------------------------------------------------------------------------
// itemtest basement walkable top (Hammer units). Tune with tf_bm_floor_z_override.
//-----------------------------------------------------------------------------
static const float BM_ITEMTEST_BASEMENT_FLOOR_Z = -135.0f;

//-----------------------------------------------------------------------------
static float BM_PickItemtestPlayFloorZ( const Vector &vecXY, float flSpawnZ, CTFPlayer *pPlayer )
{
	const float flOverride = tf_bm_floor_z_override.GetFloat();
	if ( flOverride < -128.0f )
	{
		Msg( "BM arena: itemtest floor override Z=%.0f.\n", flOverride );
		return flOverride;
	}

	CUtlVector<float> vecFloors;
	BM_CollectFloorHeightsBelow( vecXY, flSpawnZ, pPlayer, vecFloors );

	// Keep only decks below the upper teamspawns, not void under the basement slab.
	CUtlVector<float> vecDecks;
	for ( int i = 0; i < vecFloors.Count(); ++i )
	{
		const float flZ = vecFloors[i];
		if ( flZ <= flSpawnZ - 48.0f && flZ >= flSpawnZ - tf_bm_floor_drop.GetFloat() )
		{
			vecDecks.AddToTail( flZ );
		}
	}

	int iDeck = clamp( tf_bm_floor_deck.GetInt(), 0, Max( 0, vecDecks.Count() - 1 ) );

	if ( vecDecks.Count() > 0 )
	{
		// vecDecks is collected top-to-bottom; index 0 = highest deck below spawns.
		const float flBestZ = vecDecks[iDeck];
		Msg( "BM arena: itemtest decks=%d using deck[%d] Z=%.0f (spawn Z=%.0f, highest=%.0f).\n",
			vecDecks.Count(), iDeck, flBestZ, flSpawnZ, vecDecks[0] );
		return flBestZ;
	}

	// Trace missed — use tuned constant (top of basement floor, not void below).
	Warning( "BM arena: no itemtest decks traced — default basement Z=%.0f (spawn %.0f).\n",
		BM_ITEMTEST_BASEMENT_FLOOR_Z, flSpawnZ );
	return BM_ITEMTEST_BASEMENT_FLOOR_Z;
}

//-----------------------------------------------------------------------------
static bool BM_IsPlayerOnGroundNearPlayPlane( CTFPlayer *pPlayer )
{
	if ( !pPlayer )
	{
		return false;
	}

	const Vector vecPos = pPlayer->GetAbsOrigin();
	Vector vecStart( vecPos.x, vecPos.y, vecPos.z + 8.0f );
	Vector vecEnd( vecPos.x, vecPos.y, vecPos.z - 96.0f );

	trace_t trace;
	UTIL_TraceHull( vecStart, vecEnd, VEC_HULL_MIN, VEC_HULL_MAX, MASK_PLAYERSOLID, pPlayer, COLLISION_GROUP_PLAYER, &trace );

	if ( !trace.DidHit() )
	{
		return false;
	}

	const float flPlayZ = BM_GetPlayPlaneZ();
	return ( fabsf( trace.endpos.z - flPlayZ ) <= 72.0f && fabsf( vecPos.z - trace.endpos.z ) <= 24.0f );
}

//-----------------------------------------------------------------------------
void BM_ClearHullFromWorld( Vector &vecDest, CTFPlayer *pPlayer )
{
	const float flPlayZ = BM_GetPlayPlaneZ();

	// itemtest: multiple brush decks above the basement — traces hit the wrong floor.
	if ( BM_IsMapFloorArena() )
	{
		vecDest.z = flPlayZ;
		return;
	}

	Vector vecStart( vecDest.x, vecDest.y, flPlayZ + 72.0f );
	Vector vecEnd( vecDest.x, vecDest.y, flPlayZ - 128.0f );

	trace_t trace;
	UTIL_TraceHull( vecStart, vecEnd, VEC_HULL_MIN, VEC_HULL_MAX, MASK_PLAYERSOLID_BRUSHONLY, pPlayer, COLLISION_GROUP_PLAYER, &trace );
	vecDest.z = trace.DidHit() ? trace.endpos.z : flPlayZ;
}

//-----------------------------------------------------------------------------
static void BM_SnapPlayerToPlayFloor( CTFPlayer *pPlayer )
{
	if ( !pPlayer || !pPlayer->IsAlive() || !BM_IsMapFloorArena() || BM_IsPlayerMovementUnlocked( pPlayer ) )
	{
		return;
	}

	if ( tf_bm_sky_arena.GetBool() )
	{
		return;
	}

	const float flPlayZ = BM_GetPlayPlaneZ();
	Vector vecOrigin = pPlayer->GetAbsOrigin();

	if ( BM_IsMapFloorArena() )
	{
		if ( fabsf( vecOrigin.z - flPlayZ ) <= 2.0f )
		{
			return;
		}
		vecOrigin.z = flPlayZ;
	}
	else
	{
		trace_t trace;
		Vector vecStart( vecOrigin.x, vecOrigin.y, vecOrigin.z + 8.0f );
		Vector vecEnd( vecOrigin.x, vecOrigin.y, flPlayZ - 48.0f );
		UTIL_TraceHull( vecStart, vecEnd, VEC_HULL_MIN, VEC_HULL_MAX, MASK_PLAYERSOLID_BRUSHONLY, pPlayer, COLLISION_GROUP_PLAYER, &trace );

		if ( !trace.DidHit() )
		{
			return;
		}

		if ( fabsf( vecOrigin.z - trace.endpos.z ) < 2.0f && pPlayer->GetGroundEntity() != NULL )
		{
			return;
		}

		vecOrigin.z = trace.endpos.z;
	}
	pPlayer->SetAbsVelocity( Vector( pPlayer->GetAbsVelocity().x, pPlayer->GetAbsVelocity().y, 0.0f ) );
	pPlayer->SetGroundEntity( NULL );
	pPlayer->Teleport( &vecOrigin, &pPlayer->EyeAngles(), &vec3_origin );
	pPlayer->SetLocalOrigin( vecOrigin );
}

//-----------------------------------------------------------------------------
void BM_FindFloorAtXY( const Vector &vecXY, float flRefZ, CTFPlayer *pPlayer, Vector &vecFloor )
{
	vecFloor = vecXY;

	if ( BM_IsArenaActive() )
	{
		vecFloor.z = BM_GetPlayPlaneZ() - tf_bm_play_z_offset.GetFloat();
		return;
	}

	if ( BM_UseIsolatedPlayPlane() && TFGameRules() && TFGameRules()->IsBombermanMode() )
	{
		vecFloor.z = BM_GetPlayPlaneZ() - tf_bm_play_z_offset.GetFloat();
		return;
	}

	float flFloorZ = flRefZ;
	if ( BM_IsMapFloorArena() )
	{
		flFloorZ = BM_PickItemtestPlayFloorZ( vecXY, flRefZ, pPlayer );
	}
	else
	{
		Vector vecStart( vecXY.x, vecXY.y, flRefZ + 512.0f );
		Vector vecEnd( vecXY.x, vecXY.y, flRefZ - 4096.0f );

		trace_t trace;
		UTIL_TraceHull( vecStart, vecEnd, VEC_HULL_MIN, VEC_HULL_MAX, MASK_PLAYERSOLID, pPlayer, COLLISION_GROUP_PLAYER, &trace );
		if ( trace.DidHit() )
		{
			flFloorZ = trace.endpos.z;
		}
	}

	vecFloor.z = flFloorZ;
}

//-----------------------------------------------------------------------------
CTFBMBomb *BM_FindBombAtCell( int iCellX, int iCellY )
{
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "tf_bm_bomb" );
		pEnt != NULL;
		pEnt = gEntList.FindEntityByClassname( pEnt, "tf_bm_bomb" ) )
	{
		CTFBMBomb *pBomb = assert_cast<CTFBMBomb *>( pEnt );
		if ( pBomb && pBomb->m_iCellX == iCellX && pBomb->m_iCellY == iCellY )
		{
			return pBomb;
		}
	}

	return NULL;
}

//-----------------------------------------------------------------------------
bool BM_IsBlastBlockedToCell( int iFromCellX, int iFromCellY, int iToCellX, int iToCellY, CTFPlayer *pPlayer )
{
	Vector vecFrom;
	Vector vecTo;
	BM_CellToWorldCenter( iFromCellX, iFromCellY, vecFrom );
	BM_CellToWorldCenter( iToCellX, iToCellY, vecTo );

	Vector vecStart = vecFrom + Vector( 0, 0, 32.0f );
	Vector vecEnd = vecTo + Vector( 0, 0, 32.0f );

	trace_t trace;
	UTIL_TraceHull( vecStart, vecEnd, Vector( -8, -8, -8 ), Vector( 8, 8, 8 ), MASK_SOLID_BRUSHONLY, pPlayer, COLLISION_GROUP_NONE, &trace );

	return ( trace.fraction < 0.95f );
}

//-----------------------------------------------------------------------------
void BM_ResetGridAlign( void )
{
	s_bBMGridAligned = false;
}

//-----------------------------------------------------------------------------
void BM_AutoAlignGridFromSpawns( void )
{
	if ( BM_EnsureArenaBuilt() )
	{
		BM_MarkGridAligned();
	}
}

//-----------------------------------------------------------------------------
void BM_ConfigureMatch( void )
{
	if ( !TFGameRules() || !TFGameRules()->IsBombermanMode() )
	{
		return;
	}

	extern ConVar tf_bot_quota;
	extern ConVar mp_autoteambalance;
	extern ConVar friendlyfire;
	tf_bot_quota.SetValue( 0 );
	mp_autoteambalance.SetValue( 0 );

	if ( BM_IsFreeForAll() )
	{
		friendlyfire.SetValue( 1 );
	}

	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *pPlayer = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( pPlayer && pPlayer->IsConnected() )
		{
			BM_EnsurePlayerJoinedMatch( pPlayer );
		}
	}

	if ( !BM_IsArenaActive() )
	{
		BM_EnsureArenaBuilt();
	}
	else if ( BM_IsMapFloorArena() && BM_IsArenaGameplayReady()
		&& ( ( tf_bm_hard_walls.GetBool() && CTFBMWall::CountWalls() <= 0 )
			|| ( tf_bm_maze_crates.GetBool() && !tf_bm_hard_walls.GetBool() && CTFBMCrate::CountCrates() <= 0 ) ) )
	{
		extern void BM_BuildArena( bool bWarpAllPlayers, bool bForceRebuild );
		BM_BuildArena( false, true );
	}

	CBaseEntity::PrecacheModel( "models/soldier_statue/soldier_statue.mdl", false );

	Msg( "BM: round running — cell=%.0f origin=(%s) fuse=%.1fs range=%d (arena %s)\n",
		BM_GetCellSize(), tf_bm_grid_origin.GetString(), tf_bm_bomb_fuse.GetFloat(), tf_bm_bomb_range.GetInt(),
		BM_IsArenaActive() ? "ready" : "pending post-map build" );

	BM_ReleaseAllPlayersForFreeMove();
}

//-----------------------------------------------------------------------------
void BM_RespawnAllPlayers( void )
{
	if ( !TFGameRules() || !TFGameRules()->IsBombermanMode() )
	{
		return;
	}

	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *pPlayer = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( !pPlayer || !pPlayer->IsConnected() || pPlayer->GetTeamNumber() < FIRST_GAME_TEAM )
		{
			continue;
		}

		pPlayer->m_iBMActiveBombs = 0;
		pPlayer->m_bBMSpawnConfigured = false;
		BM_ResetArenaSpawnDebounce( pPlayer );
		pPlayer->ForceRespawn();
	}
}

//-----------------------------------------------------------------------------
void BM_FixMatch( void )
{
	if ( !TFGameRules() || !TFGameRules()->IsBombermanMode() )
	{
		Warning( "bm_fix: not in bomber mode.\n" );
		return;
	}

	extern void BM_RemoveStrayArenaProps( void );
	extern ConVar tf_bm_build_id;
	BM_RemoveStrayArenaProps();
	BM_BuildArena( true, true );
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *pPlayer = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( !pPlayer || !pPlayer->IsConnected() || !pPlayer->IsAlive() )
		{
			continue;
		}

		if ( pPlayer->GetTeamNumber() < FIRST_GAME_TEAM )
		{
			continue;
		}

		pPlayer->m_bBMSpawnConfigured = false;
		BM_PlacePlayerAtArenaSpawn( pPlayer, true );
		BM_ApplyDefaultFreeMove( pPlayer );
		pPlayer->m_bBMSpawnConfigured = true;
	}
	UTIL_ClientPrintAll( HUD_PRINTTALK, CFmtStr( "Frog Bomber: %d blowable walls — MOUSE1 to blast.",
		CTFBMCrate::CountCrates() ) );
	Msg( "BM: bm_fix — arena rebuilt.\n" );
}

static void CC_BM_Fix( const CCommand &args )
{
	BM_FixMatch();
}

static ConCommand bm_fix( "bm_fix", CC_BM_Fix, "Rebuild bomber arena and warp/respawn players.", FCVAR_GAMEDLL );

//-----------------------------------------------------------------------------
static void CC_BM_LetGo( const CCommand &args )
{
	CTFPlayer *pIssuer = ToTFPlayer( UTIL_GetCommandClient() );

	if ( pIssuer )
	{
		const int iIndex = pIssuer->entindex();
		if ( iIndex >= 0 && iIndex <= MAX_PLAYERS )
		{
			s_bBMForceLock[iIndex] = false;
		}
		BM_SetPlayerMovementUnlocked( pIssuer, true );
		ClientPrint( pIssuer, HUD_PRINTTALK, "bm_letgo: free move — no floor snap. bm_lock for grid clamp." );
		Msg( "BM: bm_letgo — released %s (arena warp/grid snap disabled).\n", pIssuer->GetPlayerName() );
		return;
	}

	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *pPlayer = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( pPlayer && pPlayer->IsConnected() )
		{
			const int iIndex = pPlayer->entindex();
			if ( iIndex >= 0 && iIndex <= MAX_PLAYERS )
			{
				s_bBMForceLock[iIndex] = false;
			}
			BM_SetPlayerMovementUnlocked( pPlayer, true );
		}
	}

	UTIL_ClientPrintAll( HUD_PRINTTALK, "bm_letgo: all players — free move (no floor snap)." );
	Msg( "BM: bm_letgo — all players released.\n" );
}

//-----------------------------------------------------------------------------
static void CC_BM_Lock( const CCommand &args )
{
	CTFPlayer *pIssuer = ToTFPlayer( UTIL_GetCommandClient() );

	if ( pIssuer )
	{
		const int iIndex = pIssuer->entindex();
		if ( iIndex >= 0 && iIndex <= MAX_PLAYERS )
		{
			s_bBMForceLock[iIndex] = true;
		}
		BM_SetPlayerMovementUnlocked( pIssuer, false );
		ClientPrint( pIssuer, HUD_PRINTTALK, "bm_lock: grid clamp + floor snap ON." );
		Msg( "BM: bm_lock — %s locked to grid.\n", pIssuer->GetPlayerName() );
		return;
	}

	for ( int i = 0; i <= MAX_PLAYERS; ++i )
	{
		s_bBMLetGo[i] = false;
		s_bBMForceLock[i] = true;
	}

	UTIL_ClientPrintAll( HUD_PRINTTALK, "bm_lock: grid clamp + floor snap for everyone." );
	Msg( "BM: bm_lock — all players.\n" );
}

static ConCommand bm_letgo( "bm_letgo", CC_BM_LetGo, "Stop bomber warp/grid snap so noclip and fly work (host/cheat).", FCVAR_GAMEDLL );
static ConCommand bm_lock( "bm_lock", CC_BM_Lock, "Re-enable bomber arena warp/grid snap.", FCVAR_GAMEDLL );

//-----------------------------------------------------------------------------
static bool BM_PlayerReadyForGameplay( CTFPlayer *pPlayer )
{
	if ( !pPlayer || !pPlayer->IsAlive() )
	{
		return false;
	}

	return ( pPlayer->GetTeamNumber() >= FIRST_GAME_TEAM );
}

//-----------------------------------------------------------------------------
static bool BM_IsValidBombCell( int iCellX, int iCellY )
{
	if ( !BM_IsInsideArenaCell( iCellX, iCellY ) || BM_IsHardWallCell( iCellX, iCellY ) )
	{
		return false;
	}

	if ( CTFBMBomb::GetBombAtCell( iCellX, iCellY ) != NULL )
	{
		return false;
	}

	if ( BM_FindCrateAtCell( iCellX, iCellY ) != NULL )
	{
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
static bool BM_GetBombPlacementCell( CTFPlayer *pPlayer, int &iCellX, int &iCellY )
{
	if ( !pPlayer || !BM_IsArenaActive() )
	{
		return false;
	}

	BM_WorldToCell( pPlayer->GetAbsOrigin(), iCellX, iCellY );
	if ( BM_IsValidBombCell( iCellX, iCellY ) )
	{
		return true;
	}

	int iWidth = 0;
	int iHeight = 0;
	BM_GetArenaSize( iWidth, iHeight );

	const Vector vecFeet = pPlayer->GetAbsOrigin();
	float flBestDistSq = FLT_MAX;
	int iBestX = -1;
	int iBestY = -1;

	for ( int iY = 1; iY < iHeight - 1; ++iY )
	{
		for ( int iX = 1; iX < iWidth - 1; ++iX )
		{
			if ( !BM_IsValidBombCell( iX, iY ) )
			{
				continue;
			}

			Vector vecCenter;
			BM_CellToWorldCenter( iX, iY, vecCenter );
			const float flDistSq = ( vecCenter - vecFeet ).Length2DSqr();
			if ( flDistSq < flBestDistSq )
			{
				flBestDistSq = flDistSq;
				iBestX = iX;
				iBestY = iY;
			}
		}
	}

	if ( iBestX < 0 )
	{
		return false;
	}

	iCellX = iBestX;
	iCellY = iBestY;
	return true;
}

//-----------------------------------------------------------------------------
bool BM_TryPlaceBomb( CTFPlayer *pPlayer )
{
	if ( !pPlayer || !TFGameRules() || !TFGameRules()->IsBombermanMode() )
	{
		return false;
	}

	if ( !BM_PlayerReadyForGameplay( pPlayer ) )
	{
		if ( !pPlayer->IsBot() )
		{
			ClientPrint( pPlayer, HUD_PRINTCENTER, BM_IsFreeForAll()
				? "Join a team (auto on spawn) to play Frog Bomber."
				: "Join RED or BLU to play Frog Bomber." );
		}
		return false;
	}

	if ( !BM_IsArenaGameplayReady() )
	{
		if ( !pPlayer->IsBot() )
		{
			ClientPrint( pPlayer, HUD_PRINTCENTER, "Arena still building — wait a moment." );
		}
		return false;
	}

	const int iMaxBombs = Max( 1, tf_bm_max_bombs.GetInt() );
	if ( pPlayer->m_iBMActiveBombs >= iMaxBombs )
	{
		if ( !pPlayer->IsBot() )
		{
			ClientPrint( pPlayer, HUD_PRINTCENTER, "Max bombs out — wait for detonation." );
		}
		return false;
	}

	int iCellX = 0;
	int iCellY = 0;
	if ( !BM_GetBombPlacementCell( pPlayer, iCellX, iCellY ) )
	{
		if ( !pPlayer->IsBot() )
		{
			ClientPrint( pPlayer, HUD_PRINTCENTER, "No open grid cell for a bomb nearby." );
		}
		return false;
	}

	CTFBMBomb *pBomb = CTFBMBomb::PlaceAtCell( pPlayer, iCellX, iCellY );
	if ( !pBomb )
	{
		return false;
	}

	if ( !pPlayer->IsBot() )
	{
		ClientPrint( pPlayer, HUD_PRINTCENTER, "Bomb placed!" );
	}

	return true;
}

//-----------------------------------------------------------------------------
static void CC_BM_Place( void )
{
	CTFPlayer *pPlayer = ToTFPlayer( UTIL_GetCommandClient() );
	if ( pPlayer )
	{
		BM_TryPlaceBomb( pPlayer );
	}
}

static ConCommand bm_place( "bm_place", CC_BM_Place, "Place a Frog Bomber bomb at the nearest open grid cell.", FCVAR_GAMEDLL );

static void CC_BM_FireDown( void )
{
	CTFPlayer *pPlayer = ToTFPlayer( UTIL_GetCommandClient() );
	if ( pPlayer )
	{
		BM_TryPlaceBomb( pPlayer );
	}
}

static void CC_BM_FireUp( void )
{
}

static ConCommand bm_fire( "+bm_fire", CC_BM_FireDown, "Frog Bomber: place bomb (bind MOUSE1 to +bm_fire).", FCVAR_GAMEDLL );
static ConCommand bm_fire_up( "-bm_fire", CC_BM_FireUp, "", FCVAR_GAMEDLL );

//-----------------------------------------------------------------------------
void BM_ApplySkyPlayMovement( CTFPlayer *pPlayer )
{
	if ( !pPlayer || !BM_UseSkyPlayPlane() || !tf_bm_sky_arena.GetBool() )
	{
		return;
	}

	pPlayer->SetMoveType( MOVETYPE_FLY );
	pPlayer->SetGravity( 0.0f );
	pPlayer->AddFlag( FL_FLY );
	pPlayer->SetGroundEntity( NULL );
}

//-----------------------------------------------------------------------------
void BM_EnsurePlayerInArena( CTFPlayer *pPlayer )
{
	if ( !pPlayer || !pPlayer->IsAlive() || !TFGameRules() || !TFGameRules()->IsBombermanMode() )
	{
		return;
	}

	if ( BM_IsPlayerMovementUnlocked( pPlayer ) || !tf_bm_arena_lock.GetBool() )
	{
		return;
	}

	if ( !BM_EnsureArenaBuilt() )
	{
		return;
	}

	const float flPlayZ = BM_GetPlayPlaneZ();
	const Vector &vecPos = pPlayer->GetAbsOrigin();

	// Only recover from a fall — never warp for touching grid/map walls.
	if ( vecPos.z >= flPlayZ - 96.0f )
	{
		if ( BM_IsMapFloorArena() && BM_IsInsideItemtestPlayRoom( vecPos ) )
		{
			return;
		}

		int iCellX = 0;
		int iCellY = 0;
		BM_WorldToCell( vecPos, iCellX, iCellY );
		if ( BM_IsInsideArenaCell( iCellX, iCellY ) && BM_IsPlayerOnGroundNearPlayPlane( pPlayer ) )
		{
			return;
		}
	}

	BM_WarpPlayerToArenaSpawn( pPlayer );

	if ( tf_bm_sky_arena.GetBool() )
	{
		BM_ApplySkyPlayMovement( pPlayer );
	}
}

//-----------------------------------------------------------------------------
float BM_GetItemtestPlayFloorGridZ( void )
{
	const float flOverride = tf_bm_floor_z_override.GetFloat();
	if ( flOverride <= -64.0f || flOverride >= 64.0f )
	{
		return flOverride;
	}

	return BM_ITEMTEST_BASEMENT_FLOOR_Z;
}

//-----------------------------------------------------------------------------
bool BM_OnPlayerSpawn( CTFPlayer *pPlayer )
{
	extern bool BM_IsBomberGameplayActive( void );

	if ( !pPlayer || !BM_IsBomberGameplayActive() )
	{
		return false;
	}

	BM_EnsurePlayerJoinedMatch( pPlayer );
	BM_EnsureArenaBuilt();
	pPlayer->m_iBMActiveBombs = 0;

	BM_ResetArenaSpawnDebounce( pPlayer );
	if ( pPlayer->GetMoveType() == MOVETYPE_NOCLIP )
	{
		pPlayer->SetMoveType( MOVETYPE_WALK );
	}

	if ( pPlayer->GetPlayerClass()->GetClassIndex() != TF_CLASS_SCOUT )
	{
		pPlayer->SetDesiredPlayerClassIndex( TF_CLASS_SCOUT );
		if ( pPlayer->GetPlayerClass()->GetClassIndex() != TF_CLASS_UNDEFINED )
		{
			pPlayer->ForceRespawn();
		}
		return false;
	}

	for ( int i = 0; i < MAX_WEAPONS; ++i )
	{
		CTFWeaponBase *pWpn = assert_cast<CTFWeaponBase *>( pPlayer->GetWeapon( i ) );
		if ( !pWpn )
		{
			continue;
		}

		pPlayer->Weapon_Detach( pWpn );
		UTIL_Remove( pWpn );
	}

	const float flSpeed = tf_bm_move_speed.GetFloat();
	pPlayer->SetMaxSpeed( flSpeed );

	if ( !pPlayer->IsBot() )
	{
		extern ConVar tf_bm_build_id;
		if ( BM_IsFreeForAll() )
		{
			ClientPrint( pPlayer, HUD_PRINTCENTER, CFmtStr( "Frog Bomber [%s] — spawn %d/12. MOUSE1 = statue bomb.",
				tf_bm_build_id.GetString(), BM_GetPlayerSpawnSlot( pPlayer ) + 1 ) );
		}
		else
		{
			ClientPrint( pPlayer, HUD_PRINTCENTER, CFmtStr( "Frog Bomber [%s] — Scout on arena. MOUSE1 = bomb.",
				tf_bm_build_id.GetString() ) );
		}
	}

	BM_ApplyDefaultFreeMove( pPlayer );
	return true;
}

//-----------------------------------------------------------------------------
static void BM_StripMeleeAttackButtons( CUserCmd *ucmd )
{
	if ( ucmd )
	{
		ucmd->buttons &= ~( IN_ATTACK | IN_ATTACK2 );
	}
}

//-----------------------------------------------------------------------------
void BM_PlayerRunCommand( CTFPlayer *pPlayer, CUserCmd *ucmd )
{
	if ( !pPlayer || !ucmd || !TFGameRules() || !TFGameRules()->IsBombermanMode() )
	{
		return;
	}

	if ( !pPlayer->IsAlive() )
	{
		return;
	}

	const int iPlaceButtons = IN_ATTACK | IN_ATTACK2 | IN_USE;
	const bool bPlacePressed = ( ucmd->buttons & iPlaceButtons ) && !( pPlayer->m_nBMPreviousButtons & iPlaceButtons );

	if ( bPlacePressed && BM_PlayerReadyForGameplay( pPlayer ) )
	{
		BM_TryPlaceBomb( pPlayer );
	}

	if ( BM_IsPlayerMovementUnlocked( pPlayer ) )
	{
		pPlayer->m_nBMPreviousButtons = ucmd->buttons;
		BM_StripMeleeAttackButtons( ucmd );
		return;
	}

	if ( !BM_PlayerReadyForGameplay( pPlayer ) )
	{
		return;
	}

	const bool bGridMove = BM_UseGridMovement( pPlayer );

	if ( !BM_IsArenaActive() )
	{
		static float s_flNextArenaBuildTry = 0.0f;
		if ( gpGlobals->curtime >= s_flNextArenaBuildTry )
		{
			s_flNextArenaBuildTry = gpGlobals->curtime + 1.0f;
			BM_EnsureArenaBuilt();
		}

		pPlayer->m_nBMPreviousButtons = ucmd->buttons;
		BM_StripMeleeAttackButtons( ucmd );
		return;
	}

	if ( !bGridMove )
	{
		BM_SnapPlayerToPlayFloor( pPlayer );

		const float flFwd = ucmd->forwardmove;
		const float flSide = ucmd->sidemove;
		if ( fabsf( flFwd ) >= 1.0f || fabsf( flSide ) >= 1.0f )
		{
			Vector vecForward;
			Vector vecRight;
			pPlayer->EyeVectors( &vecForward, &vecRight );
			vecForward.z = 0.0f;
			vecRight.z = 0.0f;
			vecForward.NormalizeInPlace();
			vecRight.NormalizeInPlace();

			Vector vecWish = vecForward * flFwd + vecRight * flSide;
			if ( vecWish.LengthSqr() > 1.0f )
			{
				vecWish.NormalizeInPlace();
			}

			const float flProbe = BM_GetCellSize() * 0.45f;
			const Vector vecTest = pPlayer->GetAbsOrigin() + vecWish * flProbe;
			int iTargetX = 0;
			int iTargetY = 0;
			BM_WorldToCell( vecTest, iTargetX, iTargetY );
			if ( BM_CellBlocksMovement( iTargetX, iTargetY ) )
			{
				ucmd->forwardmove = 0.0f;
				ucmd->sidemove = 0.0f;
				Vector vecVel = pPlayer->GetAbsVelocity();
				vecVel.x = 0.0f;
				vecVel.y = 0.0f;
				pPlayer->SetAbsVelocity( vecVel );
			}
		}

		pPlayer->m_nBMPreviousButtons = ucmd->buttons;
		BM_StripMeleeAttackButtons( ucmd );
		return;
	}

	Vector vecVelocity = pPlayer->GetAbsVelocity();
	if ( BM_UseSkyPlayPlane() && vecVelocity.z != 0.0f )
	{
		vecVelocity.z = 0.0f;
		pPlayer->SetAbsVelocity( vecVelocity );
	}

	ucmd->upmove = 0;
	ucmd->buttons &= ~( IN_JUMP | IN_DUCK );

	const float flFwd = ucmd->forwardmove;
	const float flSide = ucmd->sidemove;
	const bool bMoving = ( fabsf( flFwd ) >= 1.0f || fabsf( flSide ) >= 1.0f );

	if ( !bMoving )
	{
		ucmd->forwardmove = 0.0f;
		ucmd->sidemove = 0.0f;

		if ( pPlayer->m_bBMWasMoving )
		{
			BM_SnapPlayerToGrid( pPlayer );
		}

		pPlayer->m_bBMWasMoving = false;
		pPlayer->m_nBMPreviousButtons = ucmd->buttons;
		BM_StripMeleeAttackButtons( ucmd );
		return;
	}

	int iCellX = 0;
	int iCellY = 0;
	BM_WorldToCell( pPlayer->GetAbsOrigin(), iCellX, iCellY );

	if ( BM_IsArenaActive() && !BM_IsInsideArenaCell( iCellX, iCellY ) )
	{
		ucmd->forwardmove = 0.0f;
		ucmd->sidemove = 0.0f;
		pPlayer->m_bBMWasMoving = false;
		pPlayer->m_nBMPreviousButtons = ucmd->buttons;
		BM_StripMeleeAttackButtons( ucmd );
		return;
	}

	int iTargetX = iCellX;
	int iTargetY = iCellY;
	if ( flFwd > 1.0f )
	{
		++iTargetY;
	}
	else if ( flFwd < -1.0f )
	{
		--iTargetY;
	}
	else if ( flSide > 1.0f )
	{
		++iTargetX;
	}
	else if ( flSide < -1.0f )
	{
		--iTargetX;
	}

	if ( !BM_IsInsideArenaCell( iTargetX, iTargetY ) || BM_CellBlocksMovement( iTargetX, iTargetY ) )
	{
		ucmd->forwardmove = 0.0f;
		ucmd->sidemove = 0.0f;
		pPlayer->m_bBMWasMoving = false;
		pPlayer->m_nBMPreviousButtons = ucmd->buttons;
		BM_StripMeleeAttackButtons( ucmd );
		return;
	}

		pPlayer->m_bBMWasMoving = true;

	pPlayer->m_nBMPreviousButtons = ucmd->buttons;

	const float flSpeed = tf_bm_move_speed.GetFloat();
	if ( fabsf( flFwd ) >= fabsf( flSide ) )
	{
		ucmd->sidemove = 0.0f;
		ucmd->forwardmove = ( flFwd > 0.0f ) ? flSpeed : -flSpeed;
	}
	else
	{
		ucmd->forwardmove = 0.0f;
		ucmd->sidemove = ( flSide > 0.0f ) ? flSpeed : -flSpeed;
	}

	// MOUSE1 places bombs above — never swing melee in bomber mode.
	BM_StripMeleeAttackButtons( ucmd );
}

//-----------------------------------------------------------------------------
void BM_SnapPlayerToGrid( CTFPlayer *pPlayer )
{
	if ( !pPlayer || !pPlayer->IsAlive() )
	{
		return;
	}

	if ( !s_bBMGridAligned && !BM_IsArenaActive() )
	{
		BM_AutoAlignGridFromSpawns();
	}

	const float flCell = BM_GetCellSize();
	Vector vecGridOrigin;
	BM_GetGridOrigin( vecGridOrigin );

	Vector vecOrigin = pPlayer->GetAbsOrigin();
	const int iCellX = Floor2Int( ( vecOrigin.x - vecGridOrigin.x ) / flCell );
	const int iCellY = Floor2Int( ( vecOrigin.y - vecGridOrigin.y ) / flCell );

	Vector vecSnapped;
	vecSnapped.x = vecGridOrigin.x + ( iCellX + 0.5f ) * flCell;
	vecSnapped.y = vecGridOrigin.y + ( iCellY + 0.5f ) * flCell;

	vecSnapped.z = BM_GetPlayPlaneZ();

	Vector vecVelocity = pPlayer->GetAbsVelocity();
	vecVelocity.z = 0.0f;
	if ( vecVelocity.LengthSqr() > 1.0f )
	{
		pPlayer->SetAbsVelocity( Vector( 0, 0, pPlayer->GetAbsVelocity().z ) );
	}

	pPlayer->SetAbsOrigin( vecSnapped );
	pPlayer->SetLocalOrigin( vecSnapped );
}

//-----------------------------------------------------------------------------
void BM_TickMatch( void )
{
	// Bombs tick via CTFBMBomb::BombThink. Grid snap runs on stop, not every frame.
}

#endif // SOURCESDK
