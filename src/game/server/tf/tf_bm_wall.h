//========= Copyright Valve Corporation, All rights reserved. ============//
#ifndef TF_BM_WALL_H
#define TF_BM_WALL_H

#ifdef SOURCESDK

class CTFBMWall : public CBaseAnimating
{
	DECLARE_CLASS( CTFBMWall, CBaseAnimating );

public:
	CTFBMWall();

	virtual void Spawn( void );
	virtual void Precache( void );
	virtual void UpdateOnRemove( void );

	static CTFBMWall *CreateAtCell( int iCellX, int iCellY );
	static CTFBMWall *GetWallAtCell( int iCellX, int iCellY );
	static int CountWalls( void );
	static void RemoveAllWalls( void );

	int m_iCellX;
	int m_iCellY;

	void SpawnWallVisual( void );
	void RemoveWallVisual( void );

	CUtlVector<EHANDLE> m_hWallVisuals;
};

#endif // SOURCESDK

#endif // TF_BM_WALL_H
