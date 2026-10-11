#pragma once

#include "client.h"

#ifdef DEMO_DEBUG
void DemoDbg( const char *fmt, ... );
#else
#define DemoDbg(...)	( (void)0 )
#endif

extern qboolean demoApiAttached;
extern int demoApiPlayhead;

#include "cl_demo_decode.h"

// the service's decoders (cl_demo_cache.cpp); the live parser in cl_parse.cpp only loads maps
extern demoDecoder_t *demoFwd;
extern demoDecoder_t *demoRefill;
extern demoDecoder_t *demoIndex;
extern char demoFwdPath[MAX_OSPATH];

#define MAX_DEMO_TIME_MESSAGES	(2000000)	// safety stop

#include "cl_demoraw.h"

extern demoRaw_t demoRaw;		// the open demo's structure
int CL_DemoGamestateIndexOf( int fileOffset );
int CL_DemoLoadedGamestate( void );
qboolean CL_DemoNextRecordIsGamestate( void );
qboolean CL_DemoNextRecordLeavesSegment( void );
extern qboolean demoApiServiceDemo;
void CL_DemoFinish( void );

#define DEMOCACHE_MAX_ENTS		81920

typedef struct demoCacheEntry_s {
	clSnapshot_t	snap;			// parseEntitiesNum indexes our own pool, not cl.parseEntities
	int				firstEnt;
} demoCacheEntry_t;

extern demoCacheEntry_t *demoCacheSnaps;
extern entityState_t *demoCacheEnts;
extern int demoCacheMaxSnaps, demoCacheMaxEnts;
extern qboolean demoCacheExtendOnly;
extern int demoCacheCount;
extern qboolean demoCacheRefillExhausted;
extern int demoCacheExhaustedFloor;

extern demoKeyframe_t *demoKeyframes;
extern int demoKfCount;
void CL_DemoKeyframesFree( void );
void CL_DemoCaptureKeyframe( demoDecoder_t *d );
qboolean CL_DemoRestoreKeyframe( demoDecoder_t *d, int gs, int target );
void CL_DemoKeyframeVerify( demoDecoder_t *d );
int CL_DemoKeyframeBefore( int gs, int serverTime );
void CL_DemoKeyframeRestoreFailed( demoDecoder_t *d );

#define DEMOCACHE_MAX_REVERSE_GAP_MSEC		10000
#define DEMOCACHE_SEAM_MSEC					1000

#define DEMO_CTX_MIN_SLOTS			16		// snapshots kept as delta bases at least; more when the stream reaches further

extern qboolean demoPrescanDone;
void CL_DemoPrescanIndex( const char *demoPath );
void CL_DemoPrescanStep( int budgetMsec );
void CL_DemoPrescanAtLoad( void );
qboolean CL_DemoAtScanEnd( void );
void CL_DemoCacheAlloc( void );
void CL_DemoCacheBreakIfAbove( const demoDecoder_t *d );
void CL_DemoCacheFree( void );
void CL_DemoCacheAddSnapshot( const demoDecoder_t *d );
void CL_DemoSetParsingSegment( int fileOffset );
int CL_DemoStreamSegment( void );
demoCacheEntry_t *CL_DemoCacheAt( int i );
int CL_DemoCacheFindAtOrBefore( int serverTime );
extern qboolean demoRefillActive;
extern qboolean demoCacheTopTrimmed;
qboolean CL_DemoRefillBegin( int goBelowServerTime );
void CL_DemoRefillAbandon( void );
qboolean CL_DemoCacheRoomBelow( int keepTo );
qboolean CL_DemoRefillStep( int budgetMsec, int keepTo, qboolean finishCommit );
int CL_DemoCacheRunFloorTime( int atTime );
extern int demoApiResumeTarget;
extern int demoApiResumeGamestate;
void CL_DemoApiReset( void );
void CL_DemoApiOpen( const char *demoPath );
void CL_DemoCmdLogSetFileSize( int bytes );
void CL_DemoCmdLogAdd( int gs, int seq, int serverTime, const char *text );
void CL_DemoNoteGamestateFrom( const demoDecoder_t *d );
void CL_DemoEventScan( demoDecoder_t *d );
int CL_DemoApiEvents( demoApiEventCursor_t *c, entityState_t *out );
