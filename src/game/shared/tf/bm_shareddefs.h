//========= Copyright Valve Corporation, All rights reserved. ============//
#ifndef BM_SHAREDDEFS_H
#define BM_SHAREDDEFS_H

#ifdef SOURCESDK

#define TF_FF_MODE_STOCK		0
#define TF_FF_MODE_OW			1
#define TF_FF_MODE_RIM			2
#define TF_FF_MODE_BOMBERMAN	3

#define BM_MAX_SPAWN_SLOTS_PER_TEAM	8
#define BM_MAX_FFA_PLAYERS			12

struct BM_SpawnOffset_t
{
	int m_iDeltaX;
	int m_iDeltaY;
};

static const BM_SpawnOffset_t g_BMRedSpawnOffsets[BM_MAX_SPAWN_SLOTS_PER_TEAM] = {
	{ 0, 0 }, { 1, 0 }, { 0, -1 }, { 1, -1 }, { 0, -2 }, { 1, -2 }, { 2, 0 }, { 2, -1 },
};

static const BM_SpawnOffset_t g_BMBluSpawnOffsets[BM_MAX_SPAWN_SLOTS_PER_TEAM] = {
	{ 0, 0 }, { -1, 0 }, { 0, 1 }, { -1, 1 }, { 0, 2 }, { -1, 2 }, { -2, 0 }, { -2, 1 },
};

inline void BM_GetSpawnCellForSlot( bool bBlueTeam, int iSlot, int iArenaWidth, int iArenaHeight, int &iCellX, int &iCellY )
{
	const int iMaxY = iArenaHeight - 2;
	const int iMaxX = iArenaWidth - 2;
	const int iClampedSlot = clamp( iSlot, 0, BM_MAX_SPAWN_SLOTS_PER_TEAM - 1 );

	if ( bBlueTeam )
	{
		const BM_SpawnOffset_t &offset = g_BMBluSpawnOffsets[iClampedSlot];
		iCellX = iMaxX + offset.m_iDeltaX;
		iCellY = 1 + offset.m_iDeltaY;
	}
	else
	{
		const BM_SpawnOffset_t &offset = g_BMRedSpawnOffsets[iClampedSlot];
		iCellX = 1 + offset.m_iDeltaX;
		iCellY = iMaxY + offset.m_iDeltaY;
	}

	iCellX = clamp( iCellX, 1, iMaxX );
	iCellY = clamp( iCellY, 1, iMaxY );
}

// FFA: one unique spawn cell per player slot (0..11), spread around the four corners (3 per corner).
inline void BM_GetSpawnCellForPlayer( int iPlayerSlot, int iArenaWidth, int iArenaHeight, int &iCellX, int &iCellY )
{
	const int iMaxY = iArenaHeight - 2;
	const int iMaxX = iArenaWidth - 2;
	const int iSlot = clamp( iPlayerSlot, 0, BM_MAX_FFA_PLAYERS - 1 );

	static const int s_iCorner[BM_MAX_FFA_PLAYERS] = { 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3 };
	static const int s_iOffX[BM_MAX_FFA_PLAYERS] = { 0, 1, 2, 0, -1, -2, 0, 1, 2, 0, -1, -2 };
	static const int s_iOffY[BM_MAX_FFA_PLAYERS] = { 0, 0, -1, 0, 0, -1, 0, 0, 1, 0, 0, 1 };

	const int iCorner = s_iCorner[iSlot];
	const int ox = s_iOffX[iSlot];
	const int oy = s_iOffY[iSlot];

	switch ( iCorner )
	{
	case 1:
		iCellX = iMaxX + ox;
		iCellY = iMaxY + oy;
		break;
	case 2:
		iCellX = iMaxX + ox;
		iCellY = 1 + oy;
		break;
	case 3:
		iCellX = 1 + ox;
		iCellY = 1 + oy;
		break;
	default:
		iCellX = 1 + ox;
		iCellY = iMaxY + oy;
		break;
	}

	iCellX = clamp( iCellX, 1, iMaxX );
	iCellY = clamp( iCellY, 1, iMaxY );

	if ( ( iCellX % 2 ) == 0 && ( iCellY % 2 ) == 0 )
	{
		iCellX = clamp( iCellX + 1, 1, iMaxX );
	}
}

// Legacy corner+subslot (deprecated for FFA — use BM_GetSpawnCellForPlayer).
inline void BM_GetSpawnCellForCorner( int iCorner, int iSlot, int iArenaWidth, int iArenaHeight, int &iCellX, int &iCellY )
{
	BM_GetSpawnCellForPlayer( ( clamp( iCorner, 0, 3 ) * 3 ) + ( iSlot % 3 ), iArenaWidth, iArenaHeight, iCellX, iCellY );
}

#endif // SOURCESDK

#endif // BM_SHAREDDEFS_H
