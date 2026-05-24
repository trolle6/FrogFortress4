//========= Copyright Valve Corporation, All rights reserved. ============//
#include "cbase.h"

#ifdef SOURCESDK

#include "tf_bm_bomb.h"
#include "bm_grid.h"
#include "bm_props.h"
#include "props.h"
#include "tf_bm_crate.h"
#include "bm_arena.h"
#include "bm_player_system.h"
#include "bm_shareddefs.h"
#include "tf_gamerules.h"

extern ConVar tf_ff_game_mode;
#include "tf_player.h"
#include "explode.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

LINK_ENTITY_TO_CLASS( tf_bm_bomb, CTFBMBomb );

#define BM_BOMB_STATUE_MODEL "models/soldier_statue/soldier_statue.mdl"

static const char *const g_BMBombFallbackModels[] = {
	"models/props_gameplay/orange_cone001.mdl",
	"models/props_halloween/pumpkin_loot.mdl",
	"models/props_farm/wooden_barrel.mdl",
};

ConVar tf_bm_bomb_fuse( "tf_bm_bomb_fuse", "2.5", FCVAR_REPLICATED | FCVAR_NOTIFY, "Bomberman: seconds until a placed bomb explodes." );
ConVar tf_bm_bomb_range( "tf_bm_bomb_range", "2", FCVAR_REPLICATED | FCVAR_NOTIFY, "Bomberman: blast length in grid cells (each arm, not counting center)." );
ConVar tf_bm_max_bombs( "tf_bm_max_bombs", "2", FCVAR_REPLICATED | FCVAR_NOTIFY, "Bomberman: max active bombs per player." );
ConVar tf_bm_bomb_damage( "tf_bm_bomb_damage", "500", FCVAR_REPLICATED | FCVAR_NOTIFY, "Bomberman: blast damage to players." );
ConVar tf_bm_bomb_visible( "tf_bm_bomb_visible", "1", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"Bomberman: spawn a networked prop_dynamic mini soldier statue at each bomb." );
ConVar tf_bm_bomb_scale( "tf_bm_bomb_scale", "0.2", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"Bomberman: scale for bomb statue (~0.2 fits a 64u cell)." );
ConVar tf_bm_bomb_spin_speed( "tf_bm_bomb_spin_speed", "240", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"Bomberman: statue spin speed (degrees/sec) while fuse runs." );

//-----------------------------------------------------------------------------
static float BM_GetBombVisualBaseScale( void )
{
	return clamp( tf_bm_bomb_scale.GetFloat(), 0.05f, 1.0f );
}

//-----------------------------------------------------------------------------
static const char *BM_ResolveBombModel( void )
{
	CBaseEntity::PrecacheModel( BM_BOMB_STATUE_MODEL, false );
	if ( modelinfo->GetModelIndex( BM_BOMB_STATUE_MODEL ) > 0 )
	{
		return BM_BOMB_STATUE_MODEL;
	}

	BM_PrecacheModelCandidates( g_BMBombFallbackModels, ARRAYSIZE( g_BMBombFallbackModels ) );
	return BM_SelectModel( g_BMBombFallbackModels, ARRAYSIZE( g_BMBombFallbackModels ) );
}

//-----------------------------------------------------------------------------
CTFBMBomb::CTFBMBomb()
{
	m_iCellX = 0;
	m_iCellY = 0;
	m_flPlaceTime = 0.0f;
	m_flDetonateTime = 0.0f;
	m_iBlastRange = 2;
	m_bDetonating = false;
	m_hBombVisual.Set( NULL );
}

//-----------------------------------------------------------------------------
void CTFBMBomb::Precache( void )
{
	CBaseEntity::PrecacheModel( BM_BOMB_STATUE_MODEL, false );
	BM_PrecacheModelCandidates( g_BMBombFallbackModels, ARRAYSIZE( g_BMBombFallbackModels ) );
	PrecacheScriptSound( "Weapon_Grenade.Tick" );
	PrecacheScriptSound( "BaseGrenade.Explode" );

	BaseClass::Precache();
}

//-----------------------------------------------------------------------------
void CTFBMBomb::Spawn( void )
{
	Precache();

	SetSolid( SOLID_NONE );
	SetMoveType( MOVETYPE_NONE );
	AddEffects( EF_NOSHADOW | EF_NODRAW );
	SetCollisionGroup( COLLISION_GROUP_DEBRIS );

	BaseClass::Spawn();

	SpawnBombVisual();

	InitFuseFromCurrentTime();

	SetThink( &CTFBMBomb::BombThink );
	SetNextThink( gpGlobals->curtime + 0.05f );
}

//-----------------------------------------------------------------------------
void CTFBMBomb::InitFuseFromCurrentTime( void )
{
	const float flFuse = Max( 0.5f, tf_bm_bomb_fuse.GetFloat() );
	m_flPlaceTime = gpGlobals->curtime;
	m_flDetonateTime = m_flPlaceTime + flFuse;
}

//-----------------------------------------------------------------------------
void CTFBMBomb::UpdateOnRemove( void )
{
	RemoveBombVisual();

	if ( !m_bDetonating )
	{
		CTFPlayer *pOwner = ToTFPlayer( m_hOwnerPlayer.Get() );
		if ( pOwner && pOwner->m_iBMActiveBombs > 0 )
		{
			pOwner->m_iBMActiveBombs--;
		}
	}

	BaseClass::UpdateOnRemove();
}

//-----------------------------------------------------------------------------
void CTFBMBomb::SpawnBombVisual( void )
{
	RemoveBombVisual();

	if ( !tf_bm_bomb_visible.GetBool() )
	{
		return;
	}

	const char *pszModel = BM_ResolveBombModel();
	if ( !pszModel )
	{
		Warning( "BM bomb: no model (statue missing — mount TF2 VPKs).\n" );
		return;
	}

	CDynamicProp *pProp = dynamic_cast<CDynamicProp *>( CreateEntityByName( "prop_dynamic_override" ) );
	if ( !pProp )
	{
		return;
	}

	const float flBaseScale = BM_GetBombVisualBaseScale();
	const bool bStatue = ( Q_stristr( pszModel, "soldier_statue" ) != NULL );

	Vector vecOrigin = GetAbsOrigin();
	if ( bStatue )
	{
		vecOrigin.z -= BM_GetCellSize() * 0.1f * flBaseScale;
	}

	pProp->SetModel( pszModel );
	pProp->SetAbsOrigin( vecOrigin );
	pProp->SetAbsAngles( vec3_angle );
	pProp->SetModelScale( flBaseScale );
	pProp->SetSolid( SOLID_NONE );
	pProp->SetMoveType( MOVETYPE_NONE );
	pProp->AddEffects( EF_NOSHADOW );
	pProp->RemoveEffects( EF_NODRAW );

	if ( bStatue )
	{
		pProp->SetSequence( 0 );
		pProp->SetPlaybackRate( 0.0f );
	}

	DispatchSpawn( pProp );
	pProp->Activate();

	m_hBombVisual.Set( pProp );
}

//-----------------------------------------------------------------------------
void CTFBMBomb::RemoveBombVisual( void )
{
	CBaseEntity *pVisual = m_hBombVisual.Get();
	if ( pVisual )
	{
		UTIL_Remove( pVisual );
	}
	m_hBombVisual.Set( NULL );
}

//-----------------------------------------------------------------------------
void CTFBMBomb::BombThink( void )
{
	if ( !TFGameRules() || tf_ff_game_mode.GetInt() != TF_FF_MODE_BOMBERMAN )
	{
		UTIL_Remove( this );
		return;
	}

	if ( m_flDetonateTime <= 0.0f )
	{
		InitFuseFromCurrentTime();
	}

	if ( !m_bDetonating && gpGlobals->curtime >= m_flDetonateTime )
	{
		Detonate();
		return;
	}

	const float flBaseScale = BM_GetBombVisualBaseScale();
	CBaseAnimating *pVisual = dynamic_cast<CBaseAnimating *>( m_hBombVisual.Get() );
	if ( pVisual )
	{
		pVisual->SetModelScale( flBaseScale, 0.0f );

		const float flSpin = tf_bm_bomb_spin_speed.GetFloat();
		const float flYaw = fmodf( ( gpGlobals->curtime - m_flPlaceTime ) * flSpin, 360.0f );
		const QAngle angSpin( 0.0f, flYaw, 0.0f );
		pVisual->SetAbsAngles( angSpin );
		pVisual->SetLocalAngles( angSpin );
	}

	SetNextThink( gpGlobals->curtime + 0.05f );
}

//-----------------------------------------------------------------------------
CTFBMBomb *CTFBMBomb::GetBombAtCell( int iCellX, int iCellY )
{
	return BM_FindBombAtCell( iCellX, iCellY );
}

//-----------------------------------------------------------------------------
CTFBMBomb *CTFBMBomb::PlaceAtCell( CTFPlayer *pOwner, int iCellX, int iCellY )
{
	if ( !pOwner || !TFGameRules() || !TFGameRules()->IsBombermanMode() )
	{
		return NULL;
	}

	if ( GetBombAtCell( iCellX, iCellY ) != NULL )
	{
		return NULL;
	}

	Vector vecCenter;
	BM_CellToWorldCenter( iCellX, iCellY, vecCenter );

	CTFBMBomb *pBomb = assert_cast<CTFBMBomb *>( CreateEntityByName( "tf_bm_bomb" ) );
	if ( !pBomb )
	{
		return NULL;
	}

	pBomb->m_iCellX = iCellX;
	pBomb->m_iCellY = iCellY;
	pBomb->m_hOwnerPlayer = pOwner;
	pBomb->m_iBlastRange = clamp( tf_bm_bomb_range.GetInt(), 1, 8 );

	pBomb->SetAbsOrigin( vecCenter );
	pBomb->SetAbsAngles( vec3_angle );

	DispatchSpawn( pBomb );
	pBomb->Activate();

	pOwner->m_iBMActiveBombs++;

	pBomb->EmitSound( "Weapon_Grenade.Tick" );

	Msg( "BM bomb: %s placed at cell %d,%d (statue visual %s)\n",
		pOwner->GetPlayerName(), iCellX, iCellY,
		pBomb->m_hBombVisual.Get() ? "yes" : "no" );

	return pBomb;
}

//-----------------------------------------------------------------------------
static void BM_HurtPlayersAtCell( int iCellX, int iCellY, CTFPlayer *pOwner, CTFBMBomb *pBomb )
{
	Vector vecCenter;
	BM_CellToWorldCenter( iCellX, iCellY, vecCenter );

	const float flCell = BM_GetCellSize();
	const float flRadius = flCell * 0.55f;

	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *pPlayer = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( !pPlayer || !pPlayer->IsAlive() )
		{
			continue;
		}

		Vector vecDelta = pPlayer->GetAbsOrigin() - vecCenter;
		vecDelta.z = 0.0f;
		if ( vecDelta.Length() > flRadius )
		{
			continue;
		}

		if ( BM_IsFreeForAll() && pOwner && pPlayer == pOwner )
		{
			continue;
		}

		CTakeDamageInfo info( pBomb, pOwner, tf_bm_bomb_damage.GetFloat(), DMG_BLAST );
		pPlayer->TakeDamage( info );
	}
}

//-----------------------------------------------------------------------------
void CTFBMBomb::Detonate( void )
{
	if ( m_bDetonating )
	{
		return;
	}

	m_bDetonating = true;
	RemoveBombVisual();

	CTFPlayer *pOwner = ToTFPlayer( m_hOwnerPlayer.Get() );
	if ( pOwner && pOwner->m_iBMActiveBombs > 0 )
	{
		pOwner->m_iBMActiveBombs--;
	}

	Vector vecCenter;
	BM_CellToWorldCenter( m_iCellX, m_iCellY, vecCenter );

	ExplosionCreate( vecCenter, vec3_angle, this, 120, 180, true, 0.0f, false, false, DMG_BLAST );
	EmitSound( "BaseGrenade.Explode" );

	static const int s_aiDirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

	BM_DestroyCrateAtCell( m_iCellX, m_iCellY );
	BM_HurtPlayersAtCell( m_iCellX, m_iCellY, pOwner, this );

	for ( int iDir = 0; iDir < 4; ++iDir )
	{
		const int iDirX = s_aiDirs[iDir][0];
		const int iDirY = s_aiDirs[iDir][1];

		for ( int iDist = 1; iDist <= m_iBlastRange; ++iDist )
		{
			const int iCellX = m_iCellX + iDirX * iDist;
			const int iCellY = m_iCellY + iDirY * iDist;

			if ( BM_CellBlocksBlast( iCellX, iCellY ) ||
				 BM_IsBlastBlockedToCell( m_iCellX + iDirX * ( iDist - 1 ), m_iCellY + iDirY * ( iDist - 1 ), iCellX, iCellY, pOwner ) )
			{
				break;
			}

			BM_DestroyCrateAtCell( iCellX, iCellY );

			CTFBMBomb *pOther = GetBombAtCell( iCellX, iCellY );
			if ( pOther && pOther != this && !pOther->m_bDetonating )
			{
				pOther->Detonate();
			}

			BM_HurtPlayersAtCell( iCellX, iCellY, pOwner, this );

			if ( BM_IsBlastBlockedToCell( iCellX, iCellY, iCellX + iDirX, iCellY + iDirY, pOwner ) )
			{
				break;
			}
		}
	}

	UTIL_Remove( this );
}

#endif // SOURCESDK
