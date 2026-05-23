//========= Copyright Valve Corporation, All rights reserved. ============//
#ifndef TF_BM_CRATE_H
#define TF_BM_CRATE_H

#ifdef SOURCESDK

class CTFBMCrate : public CBaseAnimating
{
	DECLARE_CLASS( CTFBMCrate, CBaseAnimating );

public:
	CTFBMCrate();

	virtual void Spawn( void );
	virtual void Precache( void );
	virtual void UpdateOnRemove( void );

	static CTFBMCrate *CreateAtCell( int iCellX, int iCellY );
	static CTFBMCrate *GetCrateAtCell( int iCellX, int iCellY );
	static int CountCrates( void );
	static void RemoveAllCrates( void );

	void SpawnCrateVisual( void );
	void RemoveCrateVisual( void );

	int m_iCellX;
	int m_iCellY;
	EHANDLE m_hCrateVisual;
};

#endif // SOURCESDK

#endif // TF_BM_CRATE_H
