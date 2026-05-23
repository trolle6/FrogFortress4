//========= Copyright Valve Corporation, All rights reserved. ============//
#include "cbase.h"

#ifdef SOURCESDK

#include "tf_bm_crate.h"
#include "bm_grid.h"
#include "bm_props.h"
#include "props.h"
#include "tf_gamerules.h"
#include "tf_player.h"
#include "explode.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

LINK_ENTITY_TO_CLASS( tf_bm_crate, CTFBMCrate );

ConVar tf_bm_crate_visible( "tf_bm_crate_visible", "1", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"Bomberman: networked wood-crate props for soft walls (required to see the maze)." );
ConVar tf_bm_crate_scale( "tf_bm_crate_scale", "0.9", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"Bomberman: scale for soft-wall crate props." );
ConVar tf_bm_crate_collide( "tf_bm_crate_collide", "0", FCVAR_REPLICATED | FCVAR_NOTIFY,
	"Bomberman: unused — crates use grid blocking only (no prop physics)." );

static const char *const g_BMCrateModels[] = {
	"models/props_junk/wood_crate001a.mdl",
	"models/props_farm/wooden_barrel.mdl",
	"models/props_gameplay/orange_cone001.mdl",
	"models/error.mdl",
};

//-----------------------------------------------------------------------------
CTFBMCrate::CTFBMCrate()
{
	m_iCellX = 0;
	m_iCellY = 0;
	m_hCrateVisual.Set( NULL );
}

//-----------------------------------------------------------------------------
void CTFBMCrate::Precache( void )
{
	BM_PrecacheModelCandidates( g_BMCrateModels, ARRAYSIZE( g_BMCrateModels ) );
	BaseClass::Precache();
}

//-----------------------------------------------------------------------------
void CTFBMCrate::Spawn( void )
{
	Precache();

	// Logic-only server entity; clients see m_hCrateVisual (prop_dynamic_override).
	AddEffects( EF_NODRAW | EF_NOSHADOW );
	SetSolid( SOLID_NONE );
	SetMoveType( MOVETYPE_NONE );
	SetCollisionGroup( COLLISION_GROUP_DEBRIS );

	BaseClass::Spawn();

	SpawnCrateVisual();
}

//-----------------------------------------------------------------------------
void CTFBMCrate::SpawnCrateVisual( void )
{
	RemoveCrateVisual();

	const char *pszModel = BM_SelectModel( g_BMCrateModels, ARRAYSIZE( g_BMCrateModels ) );
	if ( !pszModel )
	{
		Warning( "BM crate: no model found — mount TF2 VPKs.\n" );
		return;
	}

	CDynamicProp *pProp = dynamic_cast<CDynamicProp *>( CreateEntityByName( "prop_dynamic_override" ) );
	if ( !pProp )
	{
		return;
	}

	const float flScale = clamp( tf_bm_crate_scale.GetFloat(), 0.25f, 2.0f );
	const float flCell = BM_GetCellSize();
	const float flHalfXY = flCell * 0.42f;
	const float flHalfZ = 36.0f;

	Vector vecOrigin = GetAbsOrigin();
	vecOrigin.z += 4.0f;

	pProp->SetModel( pszModel );
	pProp->SetAbsOrigin( vecOrigin );
	pProp->SetAbsAngles( GetAbsAngles() );
	pProp->SetModelScale( flScale );
	pProp->SetSolid( SOLID_NONE );
	pProp->SetMoveType( MOVETYPE_NONE );
	pProp->RemoveEffects( EF_NODRAW );
	pProp->AddEffects( EF_NOSHADOW );
	// No prop bbox — grid logic blocks movement; avoids standing on crate tops (floating).

	DispatchSpawn( pProp );
	pProp->Activate();

	m_hCrateVisual.Set( pProp );
}

//-----------------------------------------------------------------------------
void CTFBMCrate::RemoveCrateVisual( void )
{
	CBaseEntity *pVisual = m_hCrateVisual.Get();
	if ( pVisual )
	{
		UTIL_Remove( pVisual );
	}
	m_hCrateVisual.Set( NULL );
}

//-----------------------------------------------------------------------------
void CTFBMCrate::UpdateOnRemove( void )
{
	RemoveCrateVisual();
	BaseClass::UpdateOnRemove();
}

//-----------------------------------------------------------------------------
CTFBMCrate *CTFBMCrate::GetCrateAtCell( int iCellX, int iCellY )
{
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "tf_bm_crate" );
		pEnt != NULL;
		pEnt = gEntList.FindEntityByClassname( pEnt, "tf_bm_crate" ) )
	{
		CTFBMCrate *pCrate = assert_cast<CTFBMCrate *>( pEnt );
		if ( pCrate && pCrate->m_iCellX == iCellX && pCrate->m_iCellY == iCellY )
		{
			return pCrate;
		}
	}

	return NULL;
}

//-----------------------------------------------------------------------------
int CTFBMCrate::CountCrates( void )
{
	int nCount = 0;
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "tf_bm_crate" );
		pEnt != NULL;
		pEnt = gEntList.FindEntityByClassname( pEnt, "tf_bm_crate" ) )
	{
		++nCount;
	}
	return nCount;
}

//-----------------------------------------------------------------------------
void CTFBMCrate::RemoveAllCrates( void )
{
	CUtlVector<CTFBMCrate *> vecCrates;
	for ( CBaseEntity *pEnt = gEntList.FindEntityByClassname( NULL, "tf_bm_crate" );
		pEnt != NULL;
		pEnt = gEntList.FindEntityByClassname( pEnt, "tf_bm_crate" ) )
	{
		vecCrates.AddToTail( assert_cast<CTFBMCrate *>( pEnt ) );
	}

	for ( int i = 0; i < vecCrates.Count(); ++i )
	{
		if ( vecCrates[i] )
		{
			UTIL_Remove( vecCrates[i] );
		}
	}
}

//-----------------------------------------------------------------------------
CTFBMCrate *CTFBMCrate::CreateAtCell( int iCellX, int iCellY )
{
	if ( GetCrateAtCell( iCellX, iCellY ) != NULL )
	{
		return NULL;
	}

	if ( BM_FindBombAtCell( iCellX, iCellY ) != NULL )
	{
		return NULL;
	}

	Vector vecCenter;
	BM_CellToWorldCenter( iCellX, iCellY, vecCenter );

	CTFBMCrate *pCrate = assert_cast<CTFBMCrate *>( CreateEntityByName( "tf_bm_crate" ) );
	if ( !pCrate )
	{
		return NULL;
	}

	pCrate->m_iCellX = iCellX;
	pCrate->m_iCellY = iCellY;
	pCrate->SetAbsOrigin( vecCenter );
	pCrate->SetAbsAngles( QAngle( 0, RandomFloat( 0, 360 ), 0 ) );

	DispatchSpawn( pCrate );
	pCrate->Activate();

	return pCrate;
}

//-----------------------------------------------------------------------------
CTFBMCrate *BM_FindCrateAtCell( int iCellX, int iCellY )
{
	return CTFBMCrate::GetCrateAtCell( iCellX, iCellY );
}

//-----------------------------------------------------------------------------
void BM_DestroyCrateAtCell( int iCellX, int iCellY )
{
	CTFBMCrate *pCrate = CTFBMCrate::GetCrateAtCell( iCellX, iCellY );
	if ( !pCrate )
	{
		return;
	}

	Vector vecOrigin = pCrate->GetAbsOrigin();
	ExplosionCreate( vecOrigin, vec3_angle, pCrate, 40, 64, false, 0.0f, false, true, DMG_BLAST );
	UTIL_ScreenShake( vecOrigin, 8.0f, 120.0f, 0.4f, 256.0f, SHAKE_START );
	UTIL_Remove( pCrate );
}

#endif // SOURCESDK
