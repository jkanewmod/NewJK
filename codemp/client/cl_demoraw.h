// cl_demoraw.h -- facts about a demo file's structure, gathered record by record. Thread safe.

#pragma once

#include "cgame/cg_demoapi.h"

typedef struct demoRaw_s {
	demoApiGamestate_t	gs[DEMOAPI_MAX_GAMESTATES];
	int					numGs;
	demoApiLevelStart_t	*ls;
	int					numLs, maxLs;
	int					early[DEMOAPI_MAX_EARLYTIMES];
	int					numEarly;
	int					numSnaps;
	int					firstTime, lastTime;
	int					run;
	int					lastSeq, haveSeq;
	int					lsValue, lsHave;
	int					pendingLs;
	int					levelLeft;							// svc_mapchange seen since the last gamestate
	int					capped;								// more gamestates than the table holds: nothing past the last one counts
	int					endOffset[DEMOAPI_MAX_GAMESTATES];	// record of a gamestate's first svc_mapchange, 0 none
} demoRaw_t;

void	DR_Init( demoRaw_t *r );
void	DR_Free( demoRaw_t *r );
void	DR_Gamestate( demoRaw_t *r, int fileOffset, const char *mapName, int levelStartTime, int haveLevelStartTime, int cmdSeq );
int		DR_Command( demoRaw_t *r, int seq, const char *text, int recordOffset );	// 0: a repeat, drop it
void	DR_Snapshot( demoRaw_t *r, int serverTime, int recordOffset );
void	DR_MapChange( demoRaw_t *r, int recordOffset );
int		DR_GamestateIndex( const demoRaw_t *r, int fileOffset );
void	DR_ArgsFrom( const char *text, int arg, char *out, int outSize );	// Cmd_ArgsFrom over the same tokenizing, without the shared buffers
