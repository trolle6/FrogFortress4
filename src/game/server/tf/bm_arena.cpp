//========= Copyright Valve Corporation, All rights reserved. ============//
#include "cbase.h"

#ifdef SOURCESDK

#include "bm_arena.h"
#include "bm_grid.h"
#include "bm_player_system.h"
#include "bm_shareddefs.h"
#include "tf_bm_bomb.h"
#include "tf_bm_crate.h"
#include "tf_bm_wall.h"
#include "tf_bm_floor.h"
#include "bm_props.h"
#include "tf_gamerules.h"
#include "tf_player.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar tf_bm_arena_width( "tf_bm_arena_width", "0", FCVAR_REPLICATED | FCVAR_NOTIFY, "Arena width in cells (odd). 0 = auto-fit inside tf_bm_room_*." );
ConVar tf_bm_arena_height( "tf_bm_arena_height", "0", FCVAR_REPLICATED | FCVAR_NOTIFY, "Arena height in cells (odd). 0 = auto-fit inside tf_bm_room_*." );
ConVar tf_bm_room_square( "tf_bm_room_square", "0", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"itemtest: 0=use full Hammer room rectangle. 1=inscribed square maze (fits inside room, never expands into map)." );
ConVar tf_bm_arena_soft_fill( "tf_bm_arena_soft_fill", "0.0", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"Bomberman: random crate fill (0 when tf_bm_maze_crates 1)." );
ConVar tf_bm_hard_walls( "tf_bm_hard_walls", "1", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"Bomberman: 1=indestructible interior pillar islands (no outer ring). 0=soft crate maze only." );
ConVar tf_bm_maze_crates( "tf_bm_maze_crates", "1", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"Bomberman: 1=DFS maze with blowable wood crates between hard walls. 0=random soft_fill." );
ConVar tf_bm_arena_lift( "tf_bm_arena_lift", "0", FCVAR_REPLICATED | FCVAR_NOTIFY, "Bomberman: legacy relative lift above spawns." );
ConVar tf_bm_arena_offset( "tf_bm_arena_offset", "2048 2048", FCVAR_REPLICATED | FCVAR_NOTIFY, "Bomberman: XY offset from map spawns for floating arena (stock maps)." );
ConVar tf_bm_void_arena( "tf_bm_void_arena", "0", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"Bomberman: park arena in empty space (+8192 on itemtest). Off = build on map floor (recommended)." );
ConVar tf_bm_platform_height( "tf_bm_platform_height", "512", FCVAR_REPLICATED | FCVAR_NOTIFY, "Bomberman: platform height above highest map spawn Z (void arena only)." );
ConVar tf_bm_platform_z( "tf_bm_platform_z", "0", FCVAR_REPLICATED | FCVAR_NOTIFY, "Bomberman: absolute platform Z override (0 = use platform_height above spawns)." );

extern ConVar tf_bm_grid_origin;
extern ConVar tf_bm_sky_arena;
extern ConVar tf_bm_sky_height;
extern ConVar tf_bm_play_z_offset;
extern ConVar tf_ff_game_mode;

static bool s_bArenaActive = false;
static bool s_bBMPostMapArenaReady = false;
static int s_iArenaWidth = 0;
static int s_iArenaHeight = 0;
static int s_nArenaSoftCrates = 0;
static CBaseEntity *s_pBMSkySpawn = NULL;

#define BM_MAZE_MAX_CELLS 51

//-----------------------------------------------------------------------------
// Arena lifecycle (single source of truth — do not stack rebuilds):
//   1) FF_TickPostMapSetup pass 0: exec mode_bomber.cfg
//   2) pass 1: BM_BuildArena( force ) — only authoritative itemtest placement
//   3) gameplay: BM_EnsureArenaBuilt() — reuse grid, no ClearArena
//   4) bm_fix: BM_BuildArena( warp, force )
// SPAWN POLICY (Frog Bomber): players only spawn on the arena grid via
// BM_PlacePlayerAtArenaSpawn / BM_GetSkySpawnEntity (info_target at grid cell).
// Never info_player_teamspawn for gameplay — map spawns are only used to size void arenas.
//-----------------------------------------------------------------------------

// itemtest: ONE play volume = tf_bm_room_* (Hammer basement). Grid + crates never leave this box.
static const float BM_ITEMTEST_ROOM_MIN_X = 1304.03125f;
static const float BM_ITEMTEST_ROOM_MIN_Y = -2535.97412f;
static const float BM_ITEMTEST_ROOM_MAX_X = 2023.96875f;
static const float BM_ITEMTEST_ROOM_MAX_Y = -280.03979f;

static bool BM_IsNearSpawnCell( int iCellX, int iCellY );
static bool BM_IsFFAPlayerSpawnCell( int iCellX, int iCellY );
static CBaseEntity *BM_FindMapTeamSpawn( CTFPlayer *pPlayer );
static bool BM_IsArenaConfigValid( void );
struct BM_ItemtestPlayVolume_t
{
	float flHammerMinX;
	float flHammerMinY;
	float flHammerMaxX;
	float flHammerMaxY;
	Vector vecGridOrigin;
	int iWidth;
	int iHeight;
};

static void BM_GetItemtestHammerRoomBounds( float &flMinX, float &flMinY, float &flMaxX, float &flMaxY, float &flCenterX, float &flCenterY );
static void BM_ComputeItemtestPlayVolume( BM_ItemtestPlayVolume_t &vol );
static bool BM_WorldPosInsideHammerRoom( const Vector &vecPos, float flMargin );
static bool BM_CellCanPlaceCrate( int iCellX, int iCellY );
static void BM_GetItemtestSpawnWorldPos( int iPlayerSlot, Vector &vecWorld );
static void BM_GetArenaDimensions( int &iWidth, int &iHeight );
static void BM_GetItemtestFitArenaDimensions( int &iWidth, int &iHeight );
static void BM_ComputeItemtestExpectedGridOrigin( int iWidth, int iHeight, float flCell, Vector &vecGridOrigin, Vector &vecCenter );

//-----------------------------------------------------------------------------
// itemtest only: align grid to team spawns on map floor geometry.
//-----------------------------------------------------------------------------
bool BM_IsMapFloorArena( void )
{
	const char *pszMap = STRING( gpGlobals->mapname );
	return ( pszMap && Q_stricmp( pszMap, "itemtest" ) == 0 );
}

//-----------------------------------------------------------------------------
void BM_GetPlayAreaWorldBounds( float &flMinX, float &flMinY, float &flMaxX, float &flMaxY )
{
	if ( s_bArenaActive && s_iArenaWidth > 0 && s_iArenaHeight > 0 )
	{
		Vector vecGridOrigin;
		BM_GetGridOrigin( vecGridOrigin );
		const float flCell = BM_GetCellSize();
		flMinX = vecGridOrigin.x;
		flMinY = vecGridOrigin.y;
		flMaxX = vecGridOrigin.x + s_iArenaWidth * flCell;
		flMaxY = vecGridOrigin.y + s_iArenaHeight * flCell;
		return;
	}

	float flCenterX = 0.0f;
	float flCenterY = 0.0f;
	BM_GetItemtestHammerRoomBounds( flMinX, flMinY, flMaxX, flMaxY, flCenterX, flCenterY );
}

//-----------------------------------------------------------------------------
bool BM_IsInsideItemtestPlayRoom( const Vector &vecPos )
{
	if ( !BM_IsMapFloorArena() )
	{
		return false;
	}

	float flMinX = 0.0f;
	float flMinY = 0.0f;
	float flMaxX = 0.0f;
	float flMaxY = 0.0f;
	float flCenterX = 0.0f;
	float flCenterY = 0.0f;
	BM_GetItemtestHammerRoomBounds( flMinX, flMinY, flMaxX, flMaxY, flCenterX, flCenterY );

	const float flMargin = 8.0f;
	return ( vecPos.x >= flMinX + flMargin && vecPos.x <= flMaxX - flMargin
		&& vecPos.y >= flMinY + flMargin && vecPos.y <= flMaxY - flMargin );
}

//-----------------------------------------------------------------------------
static int BM_MakeOddCellCountForSpan( float flSpan, float flCell )
{
	if ( flSpan < flCell * 3.0f )
	{
		return 7;
	}

	int nCells = (int)floorf( flSpan / flCell );
	if ( nCells % 2 == 0 )
	{
		--nCells;
	}

	return clamp( nCells, 7, BM_MAZE_MAX_CELLS );
}

//-----------------------------------------------------------------------------
static void BM_ComputeItemtestPlayVolume( BM_ItemtestPlayVolume_t &vol )
{
	float flCenterX = 0.0f;
	float flCenterY = 0.0f;
	BM_GetItemtestHammerRoomBounds( vol.flHammerMinX, vol.flHammerMinY, vol.flHammerMaxX, vol.flHammerMaxY, flCenterX, flCenterY );
	vol.vecGridOrigin.z = BM_GetItemtestPlayFloorGridZ();

	const float flCell = BM_GetCellSize();
	const float flSpanX = vol.flHammerMaxX - vol.flHammerMinX;
	const float flSpanY = vol.flHammerMaxY - vol.flHammerMinY;
	const float flMargin = flCell * 0.5f;

	int iWidth = BM_MakeOddCellCountForSpan( flSpanX - flMargin * 2.0f, flCell );
	int iHeight = BM_MakeOddCellCountForSpan( flSpanY - flMargin * 2.0f, flCell );

	if ( tf_bm_room_square.GetBool() )
	{
		const int iSquare = Min( iWidth, iHeight );
		iWidth = iSquare;
		iHeight = iSquare;
	}

	const int iCfgW = tf_bm_arena_width.GetInt();
	const int iCfgH = tf_bm_arena_height.GetInt();
	if ( iCfgW >= 7 && iCfgH >= 7 )
	{
		iWidth = ( iCfgW % 2 == 0 ) ? ( iCfgW + 1 ) : iCfgW;
		iHeight = ( iCfgH % 2 == 0 ) ? ( iCfgH + 1 ) : iCfgH;
		iWidth = clamp( iWidth, 7, BM_MAZE_MAX_CELLS );
		iHeight = clamp( iHeight, 7, BM_MAZE_MAX_CELLS );
	}

	for ( int iShrinkPass = 0; iShrinkPass < 24; ++iShrinkPass )
	{
		const float flGridW = iWidth * flCell;
		const float flGridH = iHeight * flCell;
		vol.vecGridOrigin.x = flCenterX - flGridW * 0.5f;
		vol.vecGridOrigin.y = flCenterY - flGridH * 0.5f;

		const float flMaxGridX = vol.vecGridOrigin.x + flGridW;
		const float flMaxGridY = vol.vecGridOrigin.y + flGridH;
		if ( vol.vecGridOrigin.x >= vol.flHammerMinX + flMargin
			&& vol.vecGridOrigin.y >= vol.flHammerMinY + flMargin
			&& flMaxGridX <= vol.flHammerMaxX - flMargin
			&& flMaxGridY <= vol.flHammerMaxY - flMargin )
		{
			vol.iWidth = iWidth;
			vol.iHeight = iHeight;
			return;
		}

		if ( iWidth >= iHeight && iWidth > 7 )
		{
			iWidth -= 2;
		}
		else if ( iHeight > 7 )
		{
			iHeight -= 2;
		}
		else
		{
			break;
		}
	}

	vol.iWidth = clamp( iWidth, 7, BM_MAZE_MAX_CELLS );
	vol.iHeight = clamp( iHeight, 7, BM_MAZE_MAX_CELLS );
	const float flGridW = vol.iWidth * flCell;
	const float flGridH = vol.iHeight * flCell;
	vol.vecGridOrigin.x = flCenterX - flGridW * 0.5f;
	vol.vecGridOrigin.y = flCenterY - flGridH * 0.5f;
}

//-----------------------------------------------------------------------------
static void BM_GetItemtestFitArenaDimensions( int &iWidth, int &iHeight )
{
	BM_ItemtestPlayVolume_t vol;
	BM_ComputeItemtestPlayVolume( vol );
	iWidth = vol.iWidth;
	iHeight = vol.iHeight;
}

//-----------------------------------------------------------------------------
static bool BM_WorldPosInsideHammerRoom( const Vector &vecPos, float flMargin )
{
	float flMinX = 0.0f;
	float flMinY = 0.0f;
	float flMaxX = 0.0f;
	float flMaxY = 0.0f;
	float flCenterX = 0.0f;
	float flCenterY = 0.0f;
	BM_GetItemtestHammerRoomBounds( flMinX, flMinY, flMaxX, flMaxY, flCenterX, flCenterY );

	return ( vecPos.x >= flMinX + flMargin && vecPos.x <= flMaxX - flMargin
		&& vecPos.y >= flMinY + flMargin && vecPos.y <= flMaxY - flMargin );
}

//-----------------------------------------------------------------------------
static void BM_GetVoidPlatformOffset( float &flOffX, float &flOffY )
{
	flOffX = 2048.0f;
	flOffY = 2048.0f;
	sscanf( tf_bm_arena_offset.GetString(), "%f %f", &flOffX, &flOffY );

	if ( tf_bm_void_arena.GetBool() && flOffX == 0.0f && flOffY == 0.0f )
	{
		const char *pszMap = STRING( gpGlobals->mapname );
		if ( pszMap && Q_stricmp( pszMap, "itemtest" ) == 0 )
		{
			flOffX = 8192.0f;
			flOffY = 8192.0f;
		}
	}
}

//-----------------------------------------------------------------------------
float BM_GetEffectiveArenaLift( void )
{
	if ( tf_bm_sky_arena.GetBool() || BM_IsMapFloorArena() )
	{
		return 0.0f;
	}

	return Max( 256.0f, tf_bm_arena_lift.GetFloat() );
}

//-----------------------------------------------------------------------------
bool BM_UseVoidArenaPlatform( void )
{
	return ( !BM_IsMapFloorArena() && !tf_bm_sky_arena.GetBool() );
}

//-----------------------------------------------------------------------------
static void BM_GetArenaDimensions( int &iWidth, int &iHeight )
{
	if ( s_bArenaActive && s_iArenaWidth > 0 && s_iArenaHeight > 0 )
	{
		iWidth = s_iArenaWidth;
		iHeight = s_iArenaHeight;
		return;
	}

	iWidth = clamp( tf_bm_arena_width.GetInt(), 7, 51 );
	iHeight = clamp( tf_bm_arena_height.GetInt(), 7, 51 );
	if ( iWidth % 2 == 0 )
	{
		++iWidth;
	}
	if ( iHeight % 2 == 0 )
	{
		++iHeight;
	}
}

//-----------------------------------------------------------------------------
static void BM_GetArenaSpawnCell( CTFPlayer *pPlayer, int &iCellX, int &iCellY )
{
	iCellX = 1;
	iCellY = 1;

	if ( !pPlayer )
	{
		return;
	}

	int iWidth = 0;
	int iHeight = 0;
	BM_GetArenaDimensions( iWidth, iHeight );

	const int iSlot = BM_GetPlayerSpawnSlot( pPlayer );
	if ( BM_IsFreeForAll() )
	{
		BM_GetSpawnCellForPlayer( iSlot, iWidth, iHeight, iCellX, iCellY );
	}
	else
	{
		const bool bBlueTeam = ( pPlayer->GetTeamNumber() == TF_TEAM_BLUE );
		BM_GetSpawnCellForSlot( bBlueTeam, iSlot, iWidth, iHeight, iCellX, iCellY );
	}
}

//-----------------------------------------------------------------------------
void BM_GetArenaSize( int &iWidth, int &iHeight )
{
	iWidth = s_iArenaWidth;
	iHeight = s_iArenaHeight;
}

//-----------------------------------------------------------------------------
bool BM_IsArenaActive( void )
{
	return s_bArenaActive;
}

//-----------------------------------------------------------------------------
bool BM_IsArenaGameplayReady( void )
{
	return s_bArenaActive && s_bBMPostMapArenaReady;
}

//-----------------------------------------------------------------------------
bool BM_IsInsideArenaCell( int iCellX, int iCellY )
{
	if ( !s_bArenaActive )
	{
		return false;
	}

	return ( iCellX >= 0 && iCellY >= 0 && iCellX < s_iArenaWidth && iCellY < s_iArenaHeight );
}

//-----------------------------------------------------------------------------
bool BM_IsHardWallCell( int iCellX, int iCellY )
{
	if ( !tf_bm_hard_walls.GetBool() )
	{
		return false;
	}

	if ( !s_bArenaActive || !BM_IsInsideArenaCell( iCellX, iCellY ) )
	{
		return false;
	}

	// Interior pillar lattice only — no perimeter ring of hard props.
	if ( iCellX > 0 && iCellY > 0 && iCellX < s_iArenaWidth - 1 && iCellY < s_iArenaHeight - 1
		&& ( iCellX % 2 ) == 0 && ( iCellY % 2 ) == 0 )
	{
		return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
bool BM_IsSpawnSafeCell( int iCellX, int iCellY )
{
	if ( !BM_IsInsideArenaCell( iCellX, iCellY ) )
	{
		return false;
	}

	return BM_IsNearSpawnCell( iCellX, iCellY );
}

//-----------------------------------------------------------------------------
static bool s_bMazePassage[BM_MAZE_MAX_CELLS][BM_MAZE_MAX_CELLS];

static void BM_MazeShuffleDirs( int order[4] )
{
	order[0] = 0;
	order[1] = 1;
	order[2] = 2;
	order[3] = 3;

	for ( int i = 3; i > 0; --i )
	{
		const int j = RandomInt( 0, i );
		const int iTmp = order[i];
		order[i] = order[j];
		order[j] = iTmp;
	}
}

static void BM_MazeCarveDFS( int iWidth, int iHeight, int iCellX, int iCellY )
{
	s_bMazePassage[iCellX][iCellY] = true;

	static const int kDirs[4][2] = { { 0, 2 }, { 2, 0 }, { 0, -2 }, { -2, 0 } };
	int order[4];
	BM_MazeShuffleDirs( order );

	for ( int o = 0; o < 4; ++o )
	{
		const int iDir = order[o];
		const int iNextX = iCellX + kDirs[iDir][0];
		const int iNextY = iCellY + kDirs[iDir][1];

		if ( iNextX <= 0 || iNextY <= 0 || iNextX >= iWidth - 1 || iNextY >= iHeight - 1 )
		{
			continue;
		}

		if ( s_bMazePassage[iNextX][iNextY] )
		{
			continue;
		}

		const int iMidX = ( iCellX + iNextX ) / 2;
		const int iMidY = ( iCellY + iNextY ) / 2;
		s_bMazePassage[iMidX][iMidY] = true;
		BM_MazeCarveDFS( iWidth, iHeight, iNextX, iNextY );
	}
}

static void BM_MazeMarkSpawnPassages( int iWidth, int iHeight )
{
	for ( int iSlot = 0; iSlot < BM_MAX_FFA_PLAYERS; ++iSlot )
	{
		int iCellX = 0;
		int iCellY = 0;
		BM_GetSpawnCellForPlayer( iSlot, iWidth, iHeight, iCellX, iCellY );
		s_bMazePassage[iCellX][iCellY] = true;

		static const int kRing[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
		for ( int r = 0; r < 4; ++r )
		{
			const int iNX = iCellX + kRing[r][0];
			const int iNY = iCellY + kRing[r][1];
			if ( iNX > 0 && iNY > 0 && iNX < iWidth - 1 && iNY < iHeight - 1 )
			{
				s_bMazePassage[iNX][iNY] = true;
			}
		}
	}
}

static void BM_BuildMazePassages( int iWidth, int iHeight )
{
	for ( int iX = 0; iX < BM_MAZE_MAX_CELLS; ++iX )
	{
		for ( int iY = 0; iY < BM_MAZE_MAX_CELLS; ++iY )
		{
			s_bMazePassage[iX][iY] = false;
		}
	}

	BM_MazeMarkSpawnPassages( iWidth, iHeight );
	BM_MazeCarveDFS( iWidth, iHeight, 1, 1 );
}

static bool BM_MazeCellIsPassage( int iCellX, int iCellY )
{
	if ( !BM_IsInsideArenaCell( iCellX, iCellY ) )
	{
		return true;
	}

	return s_bMazePassage[iCellX][iCellY];
}

// Classic Bomberman: soft crates fill carved maze cells that are not hard pillars.
static bool BM_MazeCellGetsCrate( int iCellX, int iCellY )
{
	if ( BM_MazeCellIsPassage( iCellX, iCellY ) )
	{
		return false;
	}

	if ( BM_IsHardWallCell( iCellX, iCellY ) )
	{
		return false;
	}

	if ( BM_IsFFAPlayerSpawnCell( iCellX, iCellY ) || BM_IsNearSpawnCell( iCellX, iCellY ) )
	{
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
static bool BM_IsFFAPlayerSpawnCell( int iCellX, int iCellY )
{
	if ( !s_bArenaActive || s_iArenaWidth <= 0 || s_iArenaHeight <= 0 )
	{
		return false;
	}

	for ( int iSlot = 0; iSlot < BM_MAX_FFA_PLAYERS; ++iSlot )
	{
		int iSpawnX = 0;
		int iSpawnY = 0;
		BM_GetSpawnCellForPlayer( iSlot, s_iArenaWidth, s_iArenaHeight, iSpawnX, iSpawnY );
		if ( iCellX == iSpawnX && iCellY == iSpawnY )
		{
			return true;
		}
	}

	return false;
}

//-----------------------------------------------------------------------------
static bool BM_IsNearSpawnCell( int iCellX, int iCellY )
{
	if ( !BM_IsInsideArenaCell( iCellX, iCellY ) )
	{
		return false;
	}

	const int iMaxY = s_iArenaHeight - 2;
	const int iMaxX = s_iArenaWidth - 2;
	int iMargin = 3;

	// Keep crates (and solid props) clear of FFA corner spawns on itemtest.
	if ( BM_IsMapFloorArena() )
	{
		iMargin = 3;

		if ( iCellX <= 1 + iMargin && iCellY >= iMaxY - iMargin )
		{
			return true;
		}
		if ( iCellX >= iMaxX - iMargin && iCellY >= iMaxY - iMargin )
		{
			return true;
		}
		if ( iCellX >= iMaxX - iMargin && iCellY <= 1 + iMargin )
		{
			return true;
		}
		if ( iCellX <= 1 + iMargin && iCellY <= 1 + iMargin )
		{
			return true;
		}
		return false;
	}

	if ( iCellX <= 1 + iMargin && iCellY >= iMaxY - iMargin )
	{
		return true;
	}

	if ( iCellX >= iMaxX - iMargin && iCellY <= 1 + iMargin )
	{
		return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
bool BM_CellBlocksMovement( int iCellX, int iCellY )
{
	if ( BM_IsHardWallCell( iCellX, iCellY ) )
	{
		return true;
	}

	if ( BM_FindCrateAtCell( iCellX, iCellY ) != NULL )
	{
		return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
bool BM_CellBlocksBlast( int iCellX, int iCellY )
{
	if ( !s_bArenaActive )
	{
		return false;
	}

	if ( !BM_IsInsideArenaCell( iCellX, iCellY ) )
	{
		return true;
	}

	if ( BM_IsHardWallCell( iCellX, iCellY ) )
	{
		return true;
	}

	return ( BM_FindCrateAtCell( iCellX, iCellY ) != NULL );
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
static CBaseEntity *BM_FindMapTeamSpawn( CTFPlayer *pPlayer )
{
	if ( !pPlayer || !BM_IsMapFloorArena() )
	{
		return NULL;
	}

	const int iTeam = pPlayer->GetTeamNumber();
	if ( iTeam != TF_TEAM_RED && iTeam != TF_TEAM_BLUE )
	{
		return NULL;
	}

	CUtlVector<CBaseEntity *> vecSpawns;
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "info_player_teamspawn" );
		pEnt != NULL;
		pEnt = gEntList.FindEntityByClassname( pEnt, "info_player_teamspawn" ) )
	{
		if ( pEnt->GetTeamNumber() == iTeam )
		{
			vecSpawns.AddToTail( pEnt );
		}
	}

	if ( vecSpawns.Count() == 0 )
	{
		return NULL;
	}

	for ( int i = 0; i < vecSpawns.Count(); ++i )
	{
		for ( int j = i + 1; j < vecSpawns.Count(); ++j )
		{
			const Vector &a = vecSpawns[i]->GetAbsOrigin();
			const Vector &b = vecSpawns[j]->GetAbsOrigin();
			if ( a.x > b.x || ( a.x == b.x && a.y > b.y ) )
			{
				V_swap( vecSpawns[i], vecSpawns[j] );
			}
		}
	}

	const int iSlot = BM_GetPlayerSpawnSlot( pPlayer );
	return vecSpawns[ iSlot % vecSpawns.Count() ];
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
static void BM_ComputeItemtestExpectedGridOrigin( int iWidth, int iHeight, float flCell, Vector &vecGridOrigin, Vector &vecCenter )
{
	BM_ItemtestPlayVolume_t vol;
	BM_ComputeItemtestPlayVolume( vol );
	vecGridOrigin = vol.vecGridOrigin;
	vecCenter.x = vecGridOrigin.x + ( vol.iWidth * flCell ) * 0.5f;
	vecCenter.y = vecGridOrigin.y + ( vol.iHeight * flCell ) * 0.5f;
	vecCenter.z = vecGridOrigin.z;
}

//-----------------------------------------------------------------------------
static bool BM_IsArenaConfigValid( void )
{
	if ( !s_bArenaActive || s_iArenaWidth <= 0 || s_iArenaHeight <= 0 )
	{
		return false;
	}

	int iWantW = clamp( tf_bm_arena_width.GetInt(), 7, 51 );
	int iWantH = clamp( tf_bm_arena_height.GetInt(), 7, 51 );
	if ( BM_IsMapFloorArena() )
	{
		BM_GetItemtestFitArenaDimensions( iWantW, iWantH );
	}
	else
	{
		if ( iWantW % 2 == 0 )
		{
			++iWantW;
		}
		if ( iWantH % 2 == 0 )
		{
			++iWantH;
		}
	}

	if ( iWantW != s_iArenaWidth || iWantH != s_iArenaHeight )
	{
		return false;
	}

	if ( BM_IsMapFloorArena() )
	{
		Vector vecExpectedOrigin;
		Vector vecExpectedCenter;
		BM_ComputeItemtestExpectedGridOrigin( s_iArenaWidth, s_iArenaHeight, BM_GetCellSize(), vecExpectedOrigin, vecExpectedCenter );

		Vector vecCurrentOrigin;
		BM_GetGridOrigin( vecCurrentOrigin );

		const float flXYTol = BM_GetCellSize() * 0.25f;
		if ( fabsf( vecCurrentOrigin.x - vecExpectedOrigin.x ) > flXYTol
			|| fabsf( vecCurrentOrigin.y - vecExpectedOrigin.y ) > flXYTol
			|| fabsf( vecCurrentOrigin.z - vecExpectedOrigin.z ) > 2.0f )
		{
			Warning( "BM arena: grid origin (%.0f %.0f %.0f) != play room (%.0f %.0f %.0f) — needs rebuild.\n",
				vecCurrentOrigin.x, vecCurrentOrigin.y, vecCurrentOrigin.z,
				vecExpectedOrigin.x, vecExpectedOrigin.y, vecExpectedOrigin.z );
			return false;
		}
	}

	if ( BM_IsMapFloorArena() && tf_bm_hard_walls.GetBool() && CTFBMWall::CountWalls() <= 0 )
	{
		return false;
	}

	if ( BM_IsMapFloorArena() && tf_bm_maze_crates.GetBool() && !tf_bm_hard_walls.GetBool() && CTFBMCrate::CountCrates() <= 0 )
	{
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
bool BM_EnsureArenaBuilt( void )
{
	if ( !BM_IsBomberGameplayActive() )
	{
		return false;
	}

	if ( BM_IsArenaConfigValid() )
	{
		return true;
	}

	if ( !s_bBMPostMapArenaReady )
	{
		return false;
	}

	BM_BuildArena( false, false );

	if ( BM_IsMapFloorArena()
		&& ( ( tf_bm_hard_walls.GetBool() && CTFBMWall::CountWalls() <= 0 )
			|| ( tf_bm_maze_crates.GetBool() && !tf_bm_hard_walls.GetBool() && CTFBMCrate::CountCrates() <= 0 ) ) )
	{
		BM_BuildArena( false, true );
	}

	return s_bArenaActive;
}

//-----------------------------------------------------------------------------
static void BM_GetItemtestHammerRoomBounds( float &flMinX, float &flMinY, float &flMaxX, float &flMaxY, float &flCenterX, float &flCenterY )
{
	flMinX = BM_ITEMTEST_ROOM_MIN_X;
	flMinY = BM_ITEMTEST_ROOM_MIN_Y;
	flMaxX = BM_ITEMTEST_ROOM_MAX_X;
	flMaxY = BM_ITEMTEST_ROOM_MAX_Y;

	if ( g_pCVar )
	{
		ConVar *pMinX = g_pCVar->FindVar( "tf_bm_room_min_x" );
		ConVar *pMinY = g_pCVar->FindVar( "tf_bm_room_min_y" );
		ConVar *pMaxX = g_pCVar->FindVar( "tf_bm_room_max_x" );
		ConVar *pMaxY = g_pCVar->FindVar( "tf_bm_room_max_y" );
		if ( pMinX && pMinY && pMaxX && pMaxY )
		{
			const float flCfgMinX = pMinX->GetFloat();
			const float flCfgMinY = pMinY->GetFloat();
			const float flCfgMaxX = pMaxX->GetFloat();
			const float flCfgMaxY = pMaxY->GetFloat();
			if ( flCfgMaxX > flCfgMinX + 128.0f && flCfgMaxY > flCfgMinY + 128.0f )
			{
				flMinX = flCfgMinX;
				flMinY = flCfgMinY;
				flMaxX = flCfgMaxX;
				flMaxY = flCfgMaxY;
			}
		}
	}

	flCenterX = 0.5f * ( flMinX + flMaxX );
	flCenterY = 0.5f * ( flMinY + flMaxY );
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
static bool BM_CellCanPlaceCrate( int iCellX, int iCellY )
{
	if ( !s_bArenaActive || !BM_IsInsideArenaCell( iCellX, iCellY ) )
	{
		return false;
	}

	Vector vecCenter;
	BM_CellToWorldCenter( iCellX, iCellY, vecCenter );

	if ( !BM_WorldPosInsideHammerRoom( vecCenter, BM_GetCellSize() * 0.45f ) )
	{
		return false;
	}

	const float flPlayZ = BM_GetPlayPlaneZ();
	trace_t trace;
	Vector vecStart( vecCenter.x, vecCenter.y, flPlayZ + 24.0f );
	Vector vecEnd( vecCenter.x, vecCenter.y, flPlayZ - 96.0f );
	UTIL_TraceHull( vecStart, vecEnd, VEC_HULL_MIN, VEC_HULL_MAX, MASK_PLAYERSOLID_BRUSHONLY, NULL, COLLISION_GROUP_NONE, &trace );
	if ( !trace.DidHit() || fabsf( trace.endpos.z - flPlayZ ) > 12.0f )
	{
		return false;
	}

	UTIL_TraceHull( vecCenter, vecCenter, VEC_HULL_MIN, VEC_HULL_MAX, MASK_PLAYERSOLID_BRUSHONLY, NULL, COLLISION_GROUP_NONE, &trace );
	if ( trace.startsolid || trace.allsolid )
	{
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// FFA spawns at the four corners of the Hammer-measured room (tf_bm_room_*), not grid index corners.
static void BM_GetItemtestSpawnWorldPos( int iPlayerSlot, Vector &vecWorld )
{
	float flMinX = 0.0f;
	float flMinY = 0.0f;
	float flMaxX = 0.0f;
	float flMaxY = 0.0f;
	float flCenterX = 0.0f;
	float flCenterY = 0.0f;
	BM_GetItemtestHammerRoomBounds( flMinX, flMinY, flMaxX, flMaxY, flCenterX, flCenterY );

	const int iSlot = clamp( iPlayerSlot, 0, BM_MAX_FFA_PLAYERS - 1 );
	const float flCell = BM_GetCellSize();
	const float flInset = flCell * 1.5f;

	static const int s_iCorner[BM_MAX_FFA_PLAYERS] = { 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3 };
	static const int s_iOffX[BM_MAX_FFA_PLAYERS] = { 0, 1, 2, 0, -1, -2, 0, 1, 2, 0, -1, -2 };
	static const int s_iOffY[BM_MAX_FFA_PLAYERS] = { 0, 0, -1, 0, 0, -1, 0, 0, 1, 0, 0, 1 };

	const int iCorner = s_iCorner[iSlot];
	const float flStep = flCell * 0.5f;

	switch ( iCorner )
	{
	case 1:
		vecWorld.x = flMaxX - flInset + s_iOffX[iSlot] * flStep;
		vecWorld.y = flMaxY - flInset + s_iOffY[iSlot] * flStep;
		break;
	case 2:
		vecWorld.x = flMaxX - flInset + s_iOffX[iSlot] * flStep;
		vecWorld.y = flMinY + flInset + s_iOffY[iSlot] * flStep;
		break;
	case 3:
		vecWorld.x = flMinX + flInset + s_iOffX[iSlot] * flStep;
		vecWorld.y = flMinY + flInset + s_iOffY[iSlot] * flStep;
		break;
	default:
		vecWorld.x = flMinX + flInset + s_iOffX[iSlot] * flStep;
		vecWorld.y = flMaxY - flInset + s_iOffY[iSlot] * flStep;
		break;
	}

	vecWorld.x = clamp( vecWorld.x, flMinX + 32.0f, flMaxX - 32.0f );
	vecWorld.y = clamp( vecWorld.y, flMinY + 32.0f, flMaxY - 32.0f );
	vecWorld.z = BM_GetPlayPlaneZ();
}

//-----------------------------------------------------------------------------
CBaseEntity *BM_GetSkySpawnEntity( CTFPlayer *pPlayer )
{
	if ( !BM_PlayerUsesArenaGridSpawn( pPlayer ) || !pPlayer->IsAlive() )
	{
		return NULL;
	}

	BM_EnsureArenaBuilt();

	Vector vecSkySpawn;
	if ( !BM_ComputeArenaSpawnWorldPos( pPlayer, vecSkySpawn ) )
	{
		return NULL;
	}

	vecSkySpawn.z = BM_GetPlayPlaneZ();

	if ( !s_pBMSkySpawn )
	{
		s_pBMSkySpawn = CreateEntityByName( "info_target" );
		if ( s_pBMSkySpawn )
		{
			s_pBMSkySpawn->AddEffects( EF_NODRAW );
			DispatchSpawn( s_pBMSkySpawn );
			s_pBMSkySpawn->Activate();
		}
	}

	if ( !s_pBMSkySpawn )
	{
		return NULL;
	}

	if ( !BM_IsMapFloorArena() && !BM_UseVoidArenaPlatform() )
	{
		BM_ClearHullFromWorld( vecSkySpawn, pPlayer );
	}
	s_pBMSkySpawn->SetAbsOrigin( vecSkySpawn );
	s_pBMSkySpawn->SetAbsAngles( vec3_angle );
	s_pBMSkySpawn->ChangeTeam( pPlayer->GetTeamNumber() );

	return s_pBMSkySpawn;
}

//-----------------------------------------------------------------------------
bool BM_IsBomberGameplayActive( void )
{
	if ( !TFGameRules() )
	{
		return false;
	}

	if ( tf_ff_game_mode.GetInt() == TF_FF_MODE_BOMBERMAN )
	{
		return true;
	}

	return TFGameRules()->IsBombermanMode();
}

//-----------------------------------------------------------------------------
bool BM_PlayerUsesArenaGridSpawn( CTFPlayer *pPlayer )
{
	if ( !pPlayer || !BM_IsBomberGameplayActive() )
	{
		return false;
	}

	return ( pPlayer->GetTeamNumber() == TF_TEAM_RED || pPlayer->GetTeamNumber() == TF_TEAM_BLUE );
}

//-----------------------------------------------------------------------------
void BM_ResetArenaSpawnDebounce( CTFPlayer *pPlayer )
{
	(void)pPlayer;
}

//-----------------------------------------------------------------------------
bool BM_ComputeArenaSpawnWorldPos( CTFPlayer *pPlayer, Vector &vecDest )
{
	if ( !pPlayer || !BM_IsBomberGameplayActive() )
	{
		return false;
	}

	if ( pPlayer->GetTeamNumber() != TF_TEAM_RED && pPlayer->GetTeamNumber() != TF_TEAM_BLUE )
	{
		return false;
	}

	if ( BM_IsMapFloorArena() )
	{
		const int iSlot = BM_GetPlayerSpawnSlot( pPlayer );
		BM_GetItemtestSpawnWorldPos( iSlot, vecDest );

		if ( s_bArenaActive )
		{
			int iCellX = 0;
			int iCellY = 0;
			BM_WorldToCell( vecDest, iCellX, iCellY );
			if ( BM_IsInsideArenaCell( iCellX, iCellY ) )
			{
				BM_CellToWorldCenter( iCellX, iCellY, vecDest );
			}
		}

		vecDest.z = BM_GetPlayPlaneZ();
		return true;
	}

	if ( !s_bArenaActive )
	{
		return false;
	}

	int iCellX = 0;
	int iCellY = 0;
	BM_GetArenaSpawnCell( pPlayer, iCellX, iCellY );
	BM_CellToWorldCenter( iCellX, iCellY, vecDest );
	return true;
}

//-----------------------------------------------------------------------------
bool BM_IsPlayerAtArenaSpawn( CTFPlayer *pPlayer )
{
	if ( !pPlayer || !pPlayer->IsAlive() )
	{
		return false;
	}

	Vector vecExpected;
	if ( !BM_ComputeArenaSpawnWorldPos( pPlayer, vecExpected ) )
	{
		return false;
	}

	vecExpected.z = BM_GetPlayPlaneZ();
	const Vector vecPos = pPlayer->GetAbsOrigin();
	const float flCell = BM_GetCellSize();
	const float flXYTol = flCell * 0.55f;
	const float flZTol = 20.0f;

	return ( fabsf( vecPos.x - vecExpected.x ) <= flXYTol
		&& fabsf( vecPos.y - vecExpected.y ) <= flXYTol
		&& fabsf( vecPos.z - vecExpected.z ) <= flZTol );
}

//-----------------------------------------------------------------------------
bool BM_PlacePlayerAtArenaSpawn( CTFPlayer *pPlayer, bool bForcePlacement )
{
	if ( !pPlayer || !pPlayer->IsAlive() )
	{
		return false;
	}

	if ( !bForcePlacement && BM_IsPlayerMovementUnlocked( pPlayer ) )
	{
		return false;
	}

	if ( pPlayer->GetTeamNumber() != TF_TEAM_RED && pPlayer->GetTeamNumber() != TF_TEAM_BLUE )
	{
		return false;
	}

	if ( !BM_EnsureArenaBuilt() )
	{
		return false;
	}

	Vector vecDest;
	if ( !BM_ComputeArenaSpawnWorldPos( pPlayer, vecDest ) )
	{
		return false;
	}

	vecDest.z = BM_GetPlayPlaneZ();
	if ( !BM_IsMapFloorArena() && !BM_UseVoidArenaPlatform() )
	{
		BM_ClearHullFromWorld( vecDest, pPlayer );
	}

	pPlayer->SetGroundEntity( NULL );

	const QAngle angEyes = pPlayer->EyeAngles();
	pPlayer->Teleport( &vecDest, &angEyes, &vec3_origin );
	pPlayer->SetLocalOrigin( vecDest );
	pPlayer->SetAbsOrigin( vecDest );
	pPlayer->SetAbsVelocity( vec3_origin );

	if ( BM_IsMapFloorArena() )
	{
		pPlayer->SetGroundEntity( NULL );
	}

	if ( tf_bm_sky_arena.GetBool() )
	{
		extern void BM_ApplySkyPlayMovement( CTFPlayer *pPlayer );
		BM_ApplySkyPlayMovement( pPlayer );
	}
	else
	{
		pPlayer->SetMoveType( MOVETYPE_WALK );
		pPlayer->SetGravity( 1.0f );
		pPlayer->RemoveFlag( FL_FLY );
	}

	int iLogCellX = 0;
	int iLogCellY = 0;
	BM_WorldToCell( vecDest, iLogCellX, iLogCellY );
	Msg( "BM spawn: %s slot %d -> %.0f %.0f %.0f (cell %d,%d playZ=%.0f)\n",
		pPlayer->GetPlayerName(), BM_GetPlayerSpawnSlot( pPlayer ),
		vecDest.x, vecDest.y, vecDest.z, iLogCellX, iLogCellY, BM_GetPlayPlaneZ() );
	return true;
}

//-----------------------------------------------------------------------------
bool BM_ApplyArenaSpawnToPlayer( CTFPlayer *pPlayer )
{
	return BM_PlacePlayerAtArenaSpawn( pPlayer );
}

//-----------------------------------------------------------------------------
void BM_WarpPlayerToArenaSpawn( CTFPlayer *pPlayer )
{
	if ( BM_PlacePlayerAtArenaSpawn( pPlayer, true ) )
	{
		extern void BM_ApplyDefaultFreeMove( CTFPlayer *pPlayer );
		BM_ApplyDefaultFreeMove( pPlayer );
	}
}

//-----------------------------------------------------------------------------
void BM_WarpAllPlayersToArenaSpawns( void )
{
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *pPlayer = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( pPlayer && pPlayer->IsConnected() && pPlayer->IsAlive() )
		{
			BM_WarpPlayerToArenaSpawn( pPlayer );
		}
	}

	extern void BM_ReleaseAllPlayersForFreeMove( void );
	BM_ReleaseAllPlayersForFreeMove();
}

//-----------------------------------------------------------------------------
static void BM_AccumulateSpawnOrigin( const Vector &vecOrigin, float &flMinX, float &flMinY, float &flMaxX, float &flMaxY, float &flMaxZ, int &nPoints )
{
	flMinX = Min( flMinX, vecOrigin.x );
	flMinY = Min( flMinY, vecOrigin.y );
	flMaxX = Max( flMaxX, vecOrigin.x );
	flMaxY = Max( flMaxY, vecOrigin.y );
	flMaxZ = Max( flMaxZ, vecOrigin.z );
	++nPoints;
}

static bool BM_FindArenaCenter( Vector &vecCenter, float &flRefZ );

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// Stock maps: huge platform offset from spawns (beside map, in PVS — not basement, not Z=4096 void).
//-----------------------------------------------------------------------------
static bool BM_ResolveArenaGridOrigin( Vector &vecGridOrigin, int iWidth, int iHeight, float flCell, Vector &vecCenter )
{
	if ( tf_bm_sky_arena.GetBool() )
	{
		float flRefZ = 0.0f;
		if ( !BM_FindArenaCenter( vecCenter, flRefZ ) )
		{
			return false;
		}

		vecGridOrigin.x = vecCenter.x - ( iWidth * flCell ) * 0.5f;
		vecGridOrigin.y = vecCenter.y - ( iHeight * flCell ) * 0.5f;
		vecGridOrigin.z = flRefZ + Max( 512.0f, tf_bm_sky_height.GetFloat() );
		return true;
	}

	if ( BM_IsMapFloorArena() )
	{
		BM_ItemtestPlayVolume_t vol;
		BM_ComputeItemtestPlayVolume( vol );
		vecGridOrigin = vol.vecGridOrigin;
		vecCenter.x = vecGridOrigin.x + ( vol.iWidth * flCell ) * 0.5f;
		vecCenter.y = vecGridOrigin.y + ( vol.iHeight * flCell ) * 0.5f;
		vecCenter.z = vecGridOrigin.z;

		Msg( "BM arena: Hammer room (%.0f,%.0f)-(%.0f,%.0f) — grid %dx%d inside room, origin (%.0f %.0f %.0f) playZ=%.0f square=%d.\n",
			vol.flHammerMinX, vol.flHammerMinY, vol.flHammerMaxX, vol.flHammerMaxY,
			vol.iWidth, vol.iHeight,
			vecGridOrigin.x, vecGridOrigin.y, vecGridOrigin.z,
			vecGridOrigin.z + tf_bm_play_z_offset.GetFloat(),
			tf_bm_room_square.GetInt() );
		return true;
	}

	Vector vecSpawnCenter;
	float flRefZ = 0.0f;
	if ( !BM_FindArenaCenter( vecSpawnCenter, flRefZ ) )
	{
		vecSpawnCenter = vec3_origin;
		flRefZ = 0.0f;
	}

	float flOffX = 2048.0f;
	float flOffY = 2048.0f;
	BM_GetVoidPlatformOffset( flOffX, flOffY );

	vecCenter.x = vecSpawnCenter.x + flOffX;
	vecCenter.y = vecSpawnCenter.y + flOffY;
	vecGridOrigin.x = vecCenter.x - ( iWidth * flCell ) * 0.5f;
	vecGridOrigin.y = vecCenter.y - ( iHeight * flCell ) * 0.5f;

	if ( tf_bm_platform_z.GetFloat() > 256.0f )
	{
		vecGridOrigin.z = tf_bm_platform_z.GetFloat();
	}
	else
	{
		vecGridOrigin.z = flRefZ + Max( 128.0f, tf_bm_platform_height.GetFloat() );
	}

	return true;
}

//-----------------------------------------------------------------------------
static bool BM_FindArenaCenter( Vector &vecCenter, float &flRefZ )
{
	float flMinX = FLT_MAX;
	float flMinY = FLT_MAX;
	float flMaxX = -FLT_MAX;
	float flMaxY = -FLT_MAX;
	float flMaxZ = -FLT_MAX;
	int nPoints = 0;

	static const char *s_pszSpawnClasses[] = {
		"info_player_teamspawn",
		"info_player_start",
		"info_player_deathmatch",
	};

	for ( int iClass = 0; iClass < ARRAYSIZE( s_pszSpawnClasses ); ++iClass )
	{
		for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, s_pszSpawnClasses[iClass] );
			pEnt != NULL;
			pEnt = gEntList.FindEntityByClassname( pEnt, s_pszSpawnClasses[iClass] ) )
		{
			BM_AccumulateSpawnOrigin( pEnt->GetAbsOrigin(), flMinX, flMinY, flMaxX, flMaxY, flMaxZ, nPoints );
		}
	}

	if ( nPoints == 0 )
	{
		for ( int i = 1; i <= gpGlobals->maxClients; ++i )
		{
			CTFPlayer *pPlayer = ToTFPlayer( UTIL_PlayerByIndex( i ) );
			if ( !pPlayer || !pPlayer->IsConnected() )
			{
				continue;
			}

			BM_AccumulateSpawnOrigin( pPlayer->GetAbsOrigin(), flMinX, flMinY, flMaxX, flMaxY, flMaxZ, nPoints );
		}
	}

	if ( nPoints > 0 )
	{
		vecCenter.x = ( flMinX + flMaxX ) * 0.5f;
		vecCenter.y = ( flMinY + flMaxY ) * 0.5f;
		vecCenter.z = flMaxZ;
		flRefZ = flMaxZ;
		return true;
	}

	// Last resort: trace near world origin (itemtest and other flat maps).
	vecCenter = Vector( 0.0f, 0.0f, 0.0f );
	flRefZ = 0.0f;
	Vector vecFloor;
	BM_FindFloorAtXY( vecCenter, 4096.0f, NULL, vecFloor );
	vecCenter.z = vecFloor.z;
	flRefZ = vecFloor.z;
	Warning( "BM arena: no spawns — using traced floor at (%.0f %.0f %.0f).\n", vecCenter.x, vecCenter.y, vecCenter.z );
	return true;
}

//-----------------------------------------------------------------------------
static void BM_ClearSkySpawn( void )
{
	if ( s_pBMSkySpawn )
	{
		UTIL_Remove( s_pBMSkySpawn );
		s_pBMSkySpawn = NULL;
	}
}

//-----------------------------------------------------------------------------
void BM_RemoveAllBombs( void )
{
	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *pPlayer = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( pPlayer )
		{
			pPlayer->m_iBMActiveBombs = 0;
		}
	}

	CUtlVector<EHANDLE> hBombs;
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "tf_bm_bomb" );
		pEnt != NULL;
		pEnt = gEntList.FindEntityByClassname( pEnt, "tf_bm_bomb" ) )
	{
		hBombs.AddToTail( pEnt );
	}

	for ( int i = 0; i < hBombs.Count(); ++i )
	{
		CTFBMBomb *pBomb = dynamic_cast<CTFBMBomb *>( hBombs[i].Get() );
		if ( pBomb )
		{
			UTIL_Remove( pBomb );
		}
	}
}

//-----------------------------------------------------------------------------
void BM_ClearArena( void )
{
	BM_RemoveAllBombs();

	CTFBMWall::RemoveAllWalls();
	CTFBMCrate::RemoveAllCrates();
	CTFBMFloor::RemoveAllFloors();
	BM_RemoveStrayArenaProps();
	BM_ClearSkySpawn();
	s_bArenaActive = false;
	s_bBMPostMapArenaReady = false;
	s_iArenaWidth = 0;
	s_iArenaHeight = 0;
}

//-----------------------------------------------------------------------------
void BM_BuildArena( bool bWarpAllPlayers, bool bForceRebuild )
{
	if ( !BM_IsBomberGameplayActive() )
	{
		return;
	}

	if ( !bForceRebuild && BM_IsArenaConfigValid() )
	{
		if ( bWarpAllPlayers )
		{
			BM_WarpAllPlayersToArenaSpawns();
		}
		return;
	}

	if ( !bForceRebuild && !s_bBMPostMapArenaReady )
	{
		return;
	}

	BM_ClearArena();

	if ( BM_IsMapFloorArena() )
	{
		BM_GetItemtestFitArenaDimensions( s_iArenaWidth, s_iArenaHeight );
	}
	else
	{
		s_iArenaWidth = clamp( tf_bm_arena_width.GetInt(), 7, 51 );
		if ( s_iArenaWidth % 2 == 0 )
		{
			++s_iArenaWidth;
		}

		s_iArenaHeight = clamp( tf_bm_arena_height.GetInt(), 7, 51 );
		if ( s_iArenaHeight % 2 == 0 )
		{
			++s_iArenaHeight;
		}
	}

	const float flCell = BM_GetCellSize();
	const float flFill = clamp( tf_bm_arena_soft_fill.GetFloat(), 0.0f, 1.0f );
	const bool bHardWalls = tf_bm_hard_walls.GetBool();
	const bool bMazeCrates = tf_bm_maze_crates.GetBool();

	Vector vecCenter;
	Vector vecGridOrigin;
	if ( !BM_ResolveArenaGridOrigin( vecGridOrigin, s_iArenaWidth, s_iArenaHeight, flCell, vecCenter ) )
	{
		return;
	}

	char szOrigin[64];
	Q_snprintf( szOrigin, sizeof( szOrigin ), "%.1f %.1f %.1f", vecGridOrigin.x, vecGridOrigin.y, vecGridOrigin.z );
	tf_bm_grid_origin.SetValue( szOrigin );
	tf_bm_arena_width.SetValue( s_iArenaWidth );
	tf_bm_arena_height.SetValue( s_iArenaHeight );

	extern void BM_ResetGridAlign( void );
	extern void BM_MarkGridAligned( void );
	BM_ResetGridAlign();
	BM_MarkGridAligned();
	s_bArenaActive = true;

	// DFS corridors when blowable fill is used (classic hard pillars + soft crates, or soft-only maze).
	if ( bMazeCrates )
	{
		BM_BuildMazePassages( s_iArenaWidth, s_iArenaHeight );
	}

	int nWalls = 0;
	int nCrates = 0;

	for ( int iCellX = 0; iCellX < s_iArenaWidth; ++iCellX )
	{
		for ( int iCellY = 0; iCellY < s_iArenaHeight; ++iCellY )
		{
			if ( BM_IsHardWallCell( iCellX, iCellY ) )
			{
				if ( CTFBMWall::CreateAtCell( iCellX, iCellY ) != NULL )
				{
					++nWalls;
				}
				continue;
			}

			if ( bMazeCrates && BM_IsFFAPlayerSpawnCell( iCellX, iCellY ) )
			{
				continue;
			}

			bool bPlaceCrate = false;
			if ( bMazeCrates )
			{
				bPlaceCrate = BM_MazeCellGetsCrate( iCellX, iCellY );
			}
			else if ( flFill > 0.0f && RandomFloat( 0.0f, 1.0f ) <= flFill )
			{
				bPlaceCrate = true;
			}

			if ( bPlaceCrate && BM_CellCanPlaceCrate( iCellX, iCellY ) )
			{
				if ( CTFBMCrate::CreateAtCell( iCellX, iCellY ) != NULL )
				{
					++nCrates;
				}
			}
		}
	}

	const float flArenaW = s_iArenaWidth * flCell;
	const float flArenaD = s_iArenaHeight * flCell;
	Vector vecArenaCenter(
		vecGridOrigin.x + flArenaW * 0.5f,
		vecGridOrigin.y + flArenaD * 0.5f,
		vecGridOrigin.z );
	const float flPlayZ = vecGridOrigin.z + tf_bm_play_z_offset.GetFloat();
	if ( CTFBMFloor::CreateForArena( vecArenaCenter, flArenaW + flCell * 2.0f, flArenaD + flCell * 2.0f, flPlayZ ) != NULL )
	{
		Msg( "BM arena: solid play floor at Z=%.0f.\n", flPlayZ );
	}

	BM_SpawnArenaVisuals( vecArenaCenter, flArenaW, flArenaD, flPlayZ );

	Msg( "BM arena: %dx%d at %s — %d hard walls, %d soft crates (maze=%d hard=%d sky=%d).\n",
		s_iArenaWidth, s_iArenaHeight, szOrigin, nWalls, nCrates, bMazeCrates ? 1 : 0, bHardWalls ? 1 : 0, tf_bm_sky_arena.GetInt() );
	if ( tf_bm_sky_arena.GetBool() )
	{
		UTIL_ClientPrintAll( HUD_PRINTTALK, CFmtStr( "Frog Bomber: %dx%d sky layer (Z=%.0f) — legacy mode.", s_iArenaWidth, s_iArenaHeight, vecGridOrigin.z ) );
	}
	else if ( BM_UseVoidArenaPlatform() )
	{
		const char *pszMap = STRING( gpGlobals->mapname );
		if ( pszMap && Q_stricmp( pszMap, "itemtest" ) == 0 )
		{
			UTIL_ClientPrintAll( HUD_PRINTTALK, CFmtStr( "Frog Bomber: void arena at %.0f %.0f Z=%.0f (tf_bm_void_arena 1). Use 0 for itemtest floor.",
				vecCenter.x, vecCenter.y, flPlayZ ) );
		}
		else
		{
			UTIL_ClientPrintAll( HUD_PRINTTALK, CFmtStr( "Frog Bomber: isolated %dx%d platform at %.0f %.0f Z=%.0f — JOIN RED/BLU Scout!",
				s_iArenaWidth, s_iArenaHeight, vecCenter.x, vecCenter.y, flPlayZ ) );
		}
	}
	else
	{
		const char *pszMap = STRING( gpGlobals->mapname );
		if ( pszMap && Q_stricmp( pszMap, "itemtest" ) == 0 )
		{
			if ( bHardWalls && !bMazeCrates && nWalls > 0 )
			{
				UTIL_ClientPrintAll( HUD_PRINTTALK, CFmtStr( "Frog Bomber: %dx%d pillar islands — %d hard stacks, open floor (soft fill later).",
					s_iArenaWidth, s_iArenaHeight, nWalls ) );
			}
			else if ( bHardWalls && bMazeCrates && nWalls > 0 )
			{
				UTIL_ClientPrintAll( HUD_PRINTTALK, CFmtStr( "Frog Bomber: %dx%d classic maze — %d hard pillars, %d wood crates (MOUSE1 blasts crates).",
					s_iArenaWidth, s_iArenaHeight, nWalls, nCrates ) );
			}
			else if ( bMazeCrates && nCrates > 0 )
			{
				UTIL_ClientPrintAll( HUD_PRINTTALK, CFmtStr( "Frog Bomber: %dx%d maze — %d blowable crates (MOUSE1).",
					s_iArenaWidth, s_iArenaHeight, nCrates ) );
			}
			else if ( bMazeCrates || bHardWalls )
			{
				UTIL_ClientPrintAll( HUD_PRINTTALK, "Frog Bomber: maze props failed — run bm_fix after rebuilding server.dll." );
			}
			else
			{
				UTIL_ClientPrintAll( HUD_PRINTTALK, CFmtStr( "Frog Bomber: %dx%d on itemtest floor at %.0f %.0f Z=%.0f — join RED/BLU Scout.",
					s_iArenaWidth, s_iArenaHeight, vecCenter.x, vecCenter.y, flPlayZ ) );
			}
		}
		else
		{
			UTIL_ClientPrintAll( HUD_PRINTTALK, CFmtStr( "Frog Bomber: %dx%d on map floor (Z=%.0f).", s_iArenaWidth, s_iArenaHeight, flPlayZ ) );
		}
	}

	s_nArenaSoftCrates = nCrates;
	s_bBMPostMapArenaReady = true;

	if ( bMazeCrates && nCrates <= 0 )
	{
		Warning( "BM arena: maze enabled but 0 soft crates spawned — check models / bm_fix.\n" );
	}

	if ( bWarpAllPlayers )
	{
		BM_WarpAllPlayersToArenaSpawns();
	}
}

#endif // SOURCESDK
