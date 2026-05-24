//========= Copyright Valve Corporation, All rights reserved. ============//
#include "cbase.h"

#ifdef SOURCESDK

#include "tf_bm_wall.h"
#include "bm_grid.h"
#include "bm_props.h"
#include "props.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

LINK_ENTITY_TO_CLASS( tf_bm_wall, CTFBMWall );

ConVar tf_bm_wall_visible( "tf_bm_wall_visible", "1", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"Bomberman: tall networked props for indestructible hard walls." );
ConVar tf_bm_wall_scale( "tf_bm_wall_scale", "1.05", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"Bomberman: scale for hard-wall props (fills a 64u cell)." );
ConVar tf_bm_wall_stack( "tf_bm_wall_stack", "2", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"Bomberman: stacked prop layers per hard wall (2 = ~Scout height for line-of-sight)." );

static const char *const g_BMWallModels[] = {
	"models/props_junk/wood_crate001a.mdl",
	"models/props_junk/wood_crate002.mdl",
	"models/props_farm/wooden_barrel.mdl",
	"models/props_farm/crate_wood01.mdl",
	"models/props_gameplay/orange_cone001.mdl",
};

//-----------------------------------------------------------------------------
CTFBMWall::CTFBMWall()
{
	m_iCellX = 0;
	m_iCellY = 0;
}

//-----------------------------------------------------------------------------
void CTFBMWall::Precache( void )
{
	BM_PrecacheModelCandidates( g_BMWallModels, ARRAYSIZE( g_BMWallModels ) );
	BaseClass::Precache();
}

//-----------------------------------------------------------------------------
void CTFBMWall::Spawn( void )
{
	Precache();

	AddEffects( EF_NODRAW | EF_NOSHADOW );
	SetSolid( SOLID_NONE );
	SetMoveType( MOVETYPE_NONE );
	SetCollisionGroup( COLLISION_GROUP_NONE );

	BaseClass::Spawn();

	SpawnWallVisual();
}

//-----------------------------------------------------------------------------
void CTFBMWall::SpawnWallVisual( void )
{
	RemoveWallVisual();

	if ( !tf_bm_wall_visible.GetBool() )
	{
		return;
	}

	const char *pszModel = BM_SelectModel( g_BMWallModels, ARRAYSIZE( g_BMWallModels ) );
	if ( !pszModel )
	{
		Warning( "BM wall: no model — mount TF2 VPKs.\n" );
		return;
	}

	const float flCell = BM_GetCellSize();
	const float flScale = clamp( tf_bm_wall_scale.GetFloat(), 0.5f, 2.0f );
	const int nStacks = clamp( tf_bm_wall_stack.GetInt(), 1, 3 );
	const float flLayerStep = flCell * 0.38f;

	Vector vecBase = GetAbsOrigin();
	vecBase.z += 2.0f;

	for ( int iLayer = 0; iLayer < nStacks; ++iLayer )
	{
		CDynamicProp *pProp = dynamic_cast<CDynamicProp *>( CreateEntityByName( "prop_dynamic_override" ) );
		if ( !pProp )
		{
			continue;
		}

		Vector vecOrigin = vecBase;
		vecOrigin.z += iLayer * flLayerStep;

		pProp->SetModel( pszModel );
		pProp->SetAbsOrigin( vecOrigin );
		pProp->SetAbsAngles( QAngle( 0, ( iLayer & 1 ) ? 90.0f : 0.0f, 0 ) );
		pProp->SetModelScale( flScale );
		pProp->SetSolid( SOLID_NONE );
		pProp->SetMoveType( MOVETYPE_NONE );
		pProp->RemoveEffects( EF_NODRAW );
		pProp->AddEffects( EF_NOSHADOW );

		DispatchSpawn( pProp );
		pProp->Activate();

		m_hWallVisuals.AddToTail( pProp );
	}
}

//-----------------------------------------------------------------------------
void CTFBMWall::RemoveWallVisual( void )
{
	for ( int i = 0; i < m_hWallVisuals.Count(); ++i )
	{
		CBaseEntity *pVisual = m_hWallVisuals[i].Get();
		if ( pVisual )
		{
			UTIL_Remove( pVisual );
		}
	}
	m_hWallVisuals.Purge();
}

//-----------------------------------------------------------------------------
void CTFBMWall::UpdateOnRemove( void )
{
	RemoveWallVisual();
	BaseClass::UpdateOnRemove();
}

//-----------------------------------------------------------------------------
CTFBMWall *CTFBMWall::GetWallAtCell( int iCellX, int iCellY )
{
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "tf_bm_wall" );
		pEnt != NULL;
		pEnt = gEntList.FindEntityByClassname( pEnt, "tf_bm_wall" ) )
	{
		CTFBMWall *pWall = assert_cast<CTFBMWall *>( pEnt );
		if ( pWall && pWall->m_iCellX == iCellX && pWall->m_iCellY == iCellY )
		{
			return pWall;
		}
	}

	return NULL;
}

//-----------------------------------------------------------------------------
int CTFBMWall::CountWalls( void )
{
	int nCount = 0;
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "tf_bm_wall" );
		pEnt != NULL;
		pEnt = gEntList.FindEntityByClassname( pEnt, "tf_bm_wall" ) )
	{
		++nCount;
	}
	return nCount;
}

//-----------------------------------------------------------------------------
void CTFBMWall::RemoveAllWalls( void )
{
	CUtlVector<CTFBMWall *> vecWalls;
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "tf_bm_wall" );
		pEnt != NULL;
		pEnt = gEntList.FindEntityByClassname( pEnt, "tf_bm_wall" ) )
	{
		vecWalls.AddToTail( assert_cast<CTFBMWall *>( pEnt ) );
	}

	for ( int i = 0; i < vecWalls.Count(); ++i )
	{
		if ( vecWalls[i] )
		{
			UTIL_Remove( vecWalls[i] );
		}
	}
}

//-----------------------------------------------------------------------------
CTFBMWall *CTFBMWall::CreateAtCell( int iCellX, int iCellY )
{
	if ( GetWallAtCell( iCellX, iCellY ) != NULL )
	{
		return NULL;
	}

	Vector vecCenter;
	BM_CellToWorldCenter( iCellX, iCellY, vecCenter );

	CTFBMWall *pWall = assert_cast<CTFBMWall *>( CreateEntityByName( "tf_bm_wall" ) );
	if ( !pWall )
	{
		return NULL;
	}

	pWall->m_iCellX = iCellX;
	pWall->m_iCellY = iCellY;
	pWall->SetAbsOrigin( vecCenter );
	pWall->SetAbsAngles( vec3_angle );

	DispatchSpawn( pWall );
	pWall->Activate();

	return pWall;
}

#endif // SOURCESDK
