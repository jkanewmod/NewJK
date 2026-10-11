// cl_demo_cache.cpp -- demo playback: the decoded-snapshot cache and its staged backward refill,
// the keyframe index, and the three decoders (forward, refill, index) the service drives.

#include "client.h"
#include "cl_cgameapi.h"
#include "cl_uiapi.h"
#include "qcommon/stringed_ingame.h"
#include "cl_demo_local.h"

demoDecoder_t	*demoFwd;		// the forward decoder: what the viewer watches comes from it
char			demoFwdPath[MAX_OSPATH];	// the file they all read
demoDecoder_t	*demoRefill;	// backward material, when something is rewound
demoDecoder_t	*demoIndex;		// the whole-file index pass

#define DEMOCACHE_MAX_SNAPS		2048
#define DEMOCACHE_MIN_SNAPS		256
#define DEMOCACHE_COMMIT_ENTS	8192	// a refill commit copies at most this many entities per frame (~4 MB)

demoCacheEntry_t	*demoCacheSnaps;
entityState_t	*demoCacheEnts;
int				demoCacheMaxSnaps, demoCacheMaxEnts;		// ring sizes, from the demo's length at open
static int		demoCacheLowMaxSnaps, demoCacheLowMaxEnts;
static int		demoCacheEntsUsed;			// entities the live run holds; the ring positions alone cannot tell full from empty
static int		demoCacheLowEntsUsed;

// the staging run a backward refill builds; see the staging section further down
static demoCacheEntry_t	*demoCacheLowSnaps;
static entityState_t	*demoCacheLowEnts;
static int				demoCacheLowOldest;		// ring head within the staging buffer
static int				demoCacheLowCount;		// staged entries, always ascending in time
static int				demoCacheLowEntStart;	// oldest staged entity still held
static int				demoCacheLowEntWrite;	// next free slot in the staging entity buffer
static qboolean			demoCacheLowActive;		// a refill is currently building a run

static void CL_DemoCacheLowReset( void );
static void CL_DemoRefillEnd( void );
static int				demoRefillFloor;		// the time a staged run must stay strictly below
qboolean			demoCacheExtendOnly;	// see CL_DemoCacheAddSnapshot: a catch-up decode may only append

int				demoCacheCount;			// entries in use
qboolean			demoCacheRefillExhausted;	// backward refill has reached the start of the demo
int				demoCacheExhaustedFloor;	// ...below THIS floor; a different floor gets a fresh try

static int				demoCacheOldest;		// ring index of the oldest entry
static int				demoCacheEntWrite;		// next free slot in the entity pool

#define DEMOKF_MAX_BUDGET_MB	1024	// keeps the byte figure inside a 32-bit int

demoKeyframe_t	*demoKeyframes;
int				demoKfCount;
static int				demoKfMax;
static int				demoKfSpacingMsec;		// grows when the budget is hit
static int				demoKfBytes;

cvar_t	*cl_demoKeyframeMB;

void CL_DemoKeyframesFree( void ) {
	int i;

	if ( demoKeyframes ) {
		for ( i = 0; i < demoKfCount; i++ ) {
			if ( demoKeyframes[i].ents ) free( demoKeyframes[i].ents );
			if ( demoKeyframes[i].snaps ) free( demoKeyframes[i].snaps );
			if ( demoKeyframes[i].gameState ) free( demoKeyframes[i].gameState );
		}
		free( demoKeyframes );
		demoKeyframes = NULL;
	}
	demoKfCount = demoKfMax = demoKfBytes = 0;
	demoKfSpacingMsec = 0;
}

static void CL_DemoKeyframeHalve( void ) {
	int i, keep = 0;

	for ( i = 0; i < demoKfCount; i++ ) {
		if ( ( i & 1 ) == 0 ) {
			if ( keep != i ) {
				demoKeyframes[keep] = demoKeyframes[i];
			}
			keep++;
		} else {
			demoKfBytes -= demoKeyframes[i].numEnts * sizeof( entityState_t )
				+ demoKeyframes[i].numSnaps * sizeof( clSnapshot_t ) + sizeof( gameState_t );
			if ( demoKeyframes[i].ents ) free( demoKeyframes[i].ents );
			if ( demoKeyframes[i].snaps ) free( demoKeyframes[i].snaps );
			if ( demoKeyframes[i].gameState ) free( demoKeyframes[i].gameState );
		}
	}
	demoKfCount = keep;
	if ( demoKfSpacingMsec < ( 1 << 28 ) ) {
		demoKfSpacingMsec *= 2;
	}
	DemoDbg( "keyframes: budget reached, thinned to %d entries, spacing now %dms",
		demoKfCount, demoKfSpacingMsec );
}

// index a keyframe at this file position would take
static int CL_DemoKeyframeInsertAt( int fileOffset ) {
	int lo = 0, hi = demoKfCount;

	while ( lo < hi ) {
		int mid = ( lo + hi ) / 2;

		if ( demoKeyframes[mid].fileOffset < fileOffset ) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo;
}

// no neighbor in the same gamestate closer than the spacing
static qboolean CL_DemoKeyframeRoom( int at, int gs, int serverTime, int fileOffset ) {
	if ( at < demoKfCount && demoKeyframes[at].fileOffset == fileOffset ) {
		return qfalse;
	}
	if ( at > 0 && demoKeyframes[at - 1].gamestate == gs
		&& serverTime - demoKeyframes[at - 1].serverTime < demoKfSpacingMsec ) {
		return qfalse;
	}
	if ( at < demoKfCount && demoKeyframes[at].gamestate == gs
		&& demoKeyframes[at].serverTime - serverTime < demoKfSpacingMsec ) {
		return qfalse;
	}
	return qtrue;
}

// spacing for the whole demo: the budget spread evenly, never tighter than 2 s or wider than 10
static int CL_DemoKeyframeInitialSpacing( void ) {
	int duration = demoRaw.lastTime - demoRaw.firstTime, spacing;

	spacing = duration > 0 ? duration / 500 : 5000;
	if ( spacing < 2000 ) {
		spacing = 2000;
	}
	if ( spacing > 10000 ) {
		spacing = 10000;
	}
	return spacing;
}

void CL_DemoKeyframePendingDrop( demoDecoder_t *d ) {
	if ( !d->pendingValid ) {
		return;
	}
	if ( d->pendingKf.ents ) free( d->pendingKf.ents );
	if ( d->pendingKf.snaps ) free( d->pendingKf.snaps );
	if ( d->pendingKf.gameState ) free( d->pendingKf.gameState );
	Com_Memset( &d->pendingKf, 0, sizeof( d->pendingKf ) );
	d->pendingValid = qfalse;
}

/*
	The pending keyframe knows now how far back the records after it reach (every snapshot since
	has named its delta base; bases only move forward, so once one passes the keyframe's snapshot
	nothing later can reach behind it): keep that many snapshots and their entities, no more
*/
static void CL_DemoKeyframeFinalize( demoDecoder_t *d ) {
	demoKeyframe_t	*kf = &d->pendingKf, out;
	int				reach, first, i, at, budget;

	if ( !d->pendingValid ) {
		return;
	}
	d->pendingValid = qfalse;

	reach = kf->messageSequence - d->pendingMinBase + 1;
	if ( reach < 1 ) {
		reach = 1;
	}
	if ( reach > kf->numSnaps ) {
		reach = kf->numSnaps;
	}
	first = kf->numSnaps - reach;

	out = *kf;
	out.numSnaps = reach;
	out.entsBase = kf->snaps[first].parseEntitiesNum;
	out.numEnts = kf->parseEntitiesNum - out.entsBase;
	if ( out.numEnts < 0 || out.numEnts > kf->numEnts ) {
		out.entsBase = kf->entsBase;
		out.numEnts = kf->numEnts;
	}
	out.snaps = (clSnapshot_t *)malloc( reach * sizeof( clSnapshot_t ) );
	out.ents = (entityState_t *)malloc( ( out.numEnts ? out.numEnts : 1 ) * sizeof( entityState_t ) );
	if ( !out.snaps || !out.ents ) {
		if ( out.snaps ) free( out.snaps );
		if ( out.ents ) free( out.ents );
		if ( kf->ents ) free( kf->ents );
		if ( kf->snaps ) free( kf->snaps );
		if ( kf->gameState ) free( kf->gameState );
		Com_Memset( kf, 0, sizeof( *kf ) );
		return;
	}
	for ( i = 0; i < reach; i++ ) {
		out.snaps[i] = kf->snaps[first + i];
	}
	for ( i = 0; i < out.numEnts; i++ ) {
		out.ents[i] = kf->ents[( out.entsBase - kf->entsBase ) + i];
	}
	free( kf->ents );
	free( kf->snaps );
	Com_Memset( kf, 0, sizeof( *kf ) );		// the table is out's now

	budget = cl_demoKeyframeMB ? cl_demoKeyframeMB->integer : DEMOKF_BUDGET_MB;
	if ( budget > DEMOKF_MAX_BUDGET_MB ) {
		budget = DEMOKF_MAX_BUDGET_MB;
	}
	budget *= 1024 * 1024;

	at = CL_DemoKeyframeInsertAt( out.fileOffset );
	if ( !demoKeyframes || !CL_DemoKeyframeRoom( at, out.gamestate, out.serverTime, out.fileOffset ) ) {
		free( out.ents ); free( out.snaps ); free( out.gameState );
		return;
	}
	if ( demoKfBytes >= budget || demoKfCount >= demoKfMax ) {
		if ( demoKfCount < 2 ) {
			free( out.ents ); free( out.snaps ); free( out.gameState );
			return;
		}
		CL_DemoKeyframeHalve();
		at = CL_DemoKeyframeInsertAt( out.fileOffset );
		if ( !CL_DemoKeyframeRoom( at, out.gamestate, out.serverTime, out.fileOffset ) ) {
			free( out.ents ); free( out.snaps ); free( out.gameState );
			return;
		}
	}
	demoKfBytes += out.numEnts * sizeof( entityState_t ) + out.numSnaps * sizeof( clSnapshot_t ) + sizeof( gameState_t );
	if ( at < demoKfCount ) {
		memmove( &demoKeyframes[at + 1], &demoKeyframes[at], ( demoKfCount - at ) * sizeof( demoKeyframe_t ) );
	}
	demoKeyframes[at] = out;
	demoKfCount++;
	if ( ( demoKfCount % 20 ) == 0 ) {
		DemoDbg( "keyframes: %d entries, %d KB, spacing %dms", demoKfCount,
			demoKfBytes / 1024, demoKfSpacingMsec );
	}
}

// a record's snapshot came in: does the pending keyframe know its reach yet
static void CL_DemoKeyframeTrack( demoDecoder_t *d ) {
	int base;

	if ( !d->pendingValid || !d->snapDecoded ) {
		return;
	}
	base = d->snap.deltaNum;
	if ( base <= 0 || base > d->pendingKf.messageSequence ) {
		CL_DemoKeyframeFinalize( d );		// nothing from here on reaches behind the keyframe's snapshot
		return;
	}
	if ( base < d->pendingMinBase ) {
		d->pendingMinBase = base;
	}
}

// at a record boundary: is this a seek index point; if so, hold it whole until its reach is known
void CL_DemoCaptureKeyframe( demoDecoder_t *d ) {
	demoKeyframe_t	*kf;
	int				i, oldest, first, count, gs, at, reach;

	if ( !d || !d->snap.valid || d->kfRestoring || d->role == DEMODEC_REFILL ) {
		return;
	}
	gs = d->gamestate;
	if ( gs < 0 || gs >= demoRaw.numGs || d->recordOffset < demoRaw.gs[gs].settleOffset ) {
		return;
	}
	if ( cl_demoKeyframeMB && cl_demoKeyframeMB->integer <= 0 ) {
		return;
	}
	if ( !demoKeyframes ) {
		demoKfMax = 4096;
		demoKeyframes = (demoKeyframe_t *)malloc( demoKfMax * sizeof( demoKeyframe_t ) );
		if ( !demoKeyframes ) {
			return;
		}
		demoKfCount = 0;
		demoKfBytes = 0;
		demoKfSpacingMsec = CL_DemoKeyframeInitialSpacing();
	}

	// sorted by file position; within a gamestate that is also time order
	at = CL_DemoKeyframeInsertAt( d->fileOffset );
	if ( !CL_DemoKeyframeRoom( at, gs, d->snap.serverTime, d->fileOffset ) ) {
		return;
	}
	if ( d->pendingValid ) {
		if ( d->snap.serverTime - d->pendingKf.serverTime < demoKfSpacingMsec ) {
			return;
		}
		CL_DemoKeyframeFinalize( d );		// its reach is whatever has been seen so far
	}

	// the snapshots the delta chain can still reach back to
	reach = Demo_DeltaReach( d );
	oldest = d->snap.messageNum - ( reach - 1 );
	if ( oldest < 0 ) {
		oldest = 0;
	}
	first = -1;
	count = 0;
	for ( i = oldest; i <= d->snap.messageNum; i++ ) {
		clSnapshot_t *sn = &d->snapshots[i & PACKET_MASK];

		if ( !sn->valid || sn->messageNum != i ) {
			continue;				// a hole: anything older cannot serve as a base anyway
		}
		if ( first < 0 ) {
			first = sn->parseEntitiesNum;
		}
		count++;
	}
	if ( first < 0 || count < 1 ) {
		return;
	}

	kf = &d->pendingKf;
	memset( kf, 0, sizeof( *kf ) );
	kf->numEnts = d->parseEntitiesNum - first;
	if ( kf->numEnts < 0 || kf->numEnts > MAX_PARSE_ENTITIES ) {
		return;						// the ring wrapped past what we hold; not a usable keyframe
	}
	kf->ents = (entityState_t *)malloc( ( kf->numEnts ? kf->numEnts : 1 ) * sizeof( entityState_t ) );
	kf->snaps = (clSnapshot_t *)malloc( count * sizeof( clSnapshot_t ) );
	kf->gameState = (gameState_t *)malloc( sizeof( gameState_t ) );
	if ( !kf->ents || !kf->snaps || !kf->gameState ) {
		if ( kf->ents ) free( kf->ents );
		if ( kf->snaps ) free( kf->snaps );
		if ( kf->gameState ) free( kf->gameState );
		memset( kf, 0, sizeof( *kf ) );
		return;
	}
	for ( i = 0; i < kf->numEnts; i++ ) {
		kf->ents[i] = d->parseEntities[( first + i ) & ( MAX_PARSE_ENTITIES - 1 )];
	}
	kf->entsBase = first;

	kf->numSnaps = 0;
	for ( i = oldest; i <= d->snap.messageNum; i++ ) {
		clSnapshot_t *sn = &d->snapshots[i & PACKET_MASK];

		if ( !sn->valid || sn->messageNum != i ) {
			continue;
		}
		kf->snaps[kf->numSnaps++] = *sn;
	}

	kf->fileOffset = d->fileOffset;
	kf->segmentOffset = d->streamSegment;
	kf->gamestate = gs;
	kf->serverTime = d->snap.serverTime;
	kf->messageSequence = d->serverMessageSequence;
	kf->commandSequence = d->serverCommandSequence;
	kf->parseEntitiesNum = d->parseEntitiesNum;
	*kf->gameState = d->gameState;
	d->pendingMinBase = d->snap.messageNum;
	d->pendingValid = qtrue;
}

// the newest usable keyframe of gamestate gs strictly before serverTime, or -1. the table is in
// file order, which is gamestate order and, within a gamestate, time order
int CL_DemoKeyframeBefore( int gs, int serverTime ) {
	int lo = 0, hi = demoKfCount, k;

	if ( !demoKeyframes ) {
		return -1;
	}
	while ( lo < hi ) {
		int						mid = ( lo + hi ) / 2;
		const demoKeyframe_t	*kf = &demoKeyframes[mid];

		if ( kf->gamestate < gs || ( kf->gamestate == gs && kf->serverTime < serverTime ) ) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	for ( k = lo - 1; k >= 0 && demoKeyframes[k].gamestate == gs; k-- ) {
		if ( !demoKeyframes[k].bad ) {
			return k;
		}
	}
	return -1;
}

static int CL_DemoFindKeyframe( int gs, int target ) {
	return CL_DemoKeyframeBefore( gs, target );
}

// a keyframe whose restore did not take is never tried again; the next older one is
static void CL_DemoKeyframeQuarantine( int fileOffset ) {
	int at;

	if ( !demoKeyframes || fileOffset < 0 ) {
		return;
	}
	at = CL_DemoKeyframeInsertAt( fileOffset );
	if ( at < demoKfCount && demoKeyframes[at].fileOffset == fileOffset ) {
		demoKeyframes[at].bad = qtrue;
		DemoDbg( "keyframes: #%d at offset %d quarantined", at, fileOffset );
	}
}

void CL_DemoKeyframeRestoreFailed( demoDecoder_t *d ) {
	d->kfRestoring = qfalse;
	d->kfRestoreFailed = qtrue;
	CL_DemoKeyframeQuarantine( d->kfRestoredOffset );
}

qboolean CL_DemoRestoreKeyframe( demoDecoder_t *d, int gs, int target ) {
	demoKeyframe_t	*kf;
	int				i;

	if ( !demoKeyframes || demoKfCount < 1 || !d || d->gamestate != gs ) {
		return qfalse;
	}
	i = CL_DemoFindKeyframe( gs, target );
	if ( i < 0 ) {
		return qfalse;
	}
	kf = &demoKeyframes[i];

	// the decoder jumps: a keyframe it was still measuring the reach of would measure the wrong
	// stream, and a configstring piece from before the jump has no end here
	CL_DemoKeyframePendingDrop( d );
	d->bigConfigString[0] = '\0';

	for ( i = 0; i < PACKET_BACKUP; i++ ) {
		d->snapshots[i].valid = qfalse;
	}
	for ( i = 0; i < kf->numSnaps; i++ ) {
		d->snapshots[kf->snaps[i].messageNum & PACKET_MASK] = kf->snaps[i];
	}
	d->serverMessageSequence = kf->messageSequence;
	d->serverCommandSequence = kf->commandSequence;
	d->snap = d->snapshots[kf->snaps[kf->numSnaps - 1].messageNum & PACKET_MASK];

	for ( i = 0; i < kf->numEnts; i++ ) {
		d->parseEntities[( kf->entsBase + i ) & ( MAX_PARSE_ENTITIES - 1 )] = kf->ents[i];
	}
	d->parseEntitiesNum = kf->parseEntitiesNum;
	d->gameState = *kf->gameState;
	d->streamSegment = kf->segmentOffset;
	d->fileOffset = kf->fileOffset;
	d->recordOffset = kf->fileOffset;
	d->eof = qfalse;
	d->fault = qfalse;

	d->kfRestoring = qtrue;			// armed until the next parsed snapshot proves the chain took
	d->kfRestoreFailed = qfalse;
	d->kfRestoreSnapTime = d->snap.serverTime;
	d->kfVerifyReads = 0;
	d->kfRestoredOffset = kf->fileOffset;

	DemoDbg( "decoder %d: restored keyframe #%d of %d (serverTime=%d offset=%d snaps=%d ents=%d)", d->role,
		(int)( kf - demoKeyframes ), demoKfCount, kf->serverTime,
		kf->fileOffset, kf->numSnaps, kf->numEnts );
	return qtrue;
}

#define DEMOCACHE_REFILL_MARGIN_MSEC		1000
#define DEMOCACHE_REFILL_MIN_SNAPS			16
// give up extending backwards once a refill fails to lower the floor by at least this much; without
// it, reaching the true start of the demo would retry a full decode every single frame

// how much demo must remain below the playhead before a refill is started in the background

#define DEMOKF_VERIFY_READS		64

void CL_DemoKeyframeVerify( demoDecoder_t *d ) {
	if ( !d->kfRestoring ) {
		return;
	}
	if ( d->snap.valid && d->snap.serverTime > d->kfRestoreSnapTime ) {
		d->kfRestoring = qfalse;		// the chain re-established
		return;
	}
	if ( ++d->kfVerifyReads > DEMOKF_VERIFY_READS ) {
		CL_DemoKeyframeRestoreFailed( d );
		DemoDbg( "decoder %d: keyframe restore did NOT re-establish the delta chain -- falling back", d->role );
	}
}

/*
===================
The index pass: the whole file once, keyframes and the command log, beside playback
===================
*/
#define DEMOKF_PRESCAN_OPEN_MSEC	120		// the opening slice, before the map loads
#define DEMOKF_PRESCAN_LOAD_MSEC	6000	// behind the loading screen: the whole file for anything but a huge one

qboolean			demoPrescanDone;
static int			demoPrescanCpuMsec;

static void CL_DemoPrescanSlice( int budgetMsec ) {
	int began = Sys_Milliseconds();

	while ( demoIndex && !demoIndex->eof && !demoIndex->fault ) {
		if ( demoIndex->guard > MAX_DEMO_TIME_MESSAGES ) {
			demoIndex->eof = qtrue;		// the same safety stop the decode loops use
			break;
		}
		Demo_ReadRecord( demoIndex );
		if ( budgetMsec > 0 && Sys_Milliseconds() - began >= budgetMsec ) {
			break;
		}
	}
	demoPrescanCpuMsec += Sys_Milliseconds() - began;

	if ( demoIndex && ( demoIndex->eof || demoIndex->fault ) && !demoPrescanDone ) {
		CL_DemoKeyframeFinalize( demoIndex );
		if ( demoIndex->fault ) {
			Com_Printf( S_COLOR_YELLOW "Demo file damaged at offset %d; the seek index ends there.\n", demoIndex->faultOffset );
		}
		demoPrescanDone = qtrue;
#ifdef _DEBUG
		Com_Printf( "Indexed demo in %dms: %d seek points, %d KB, %.1fs apart.\n",
			demoPrescanCpuMsec, demoKfCount, demoKfBytes / 1024, demoKfSpacingMsec / 1000.0f );
#endif
		Demo_DecoderFree( demoIndex );
		demoIndex = NULL;
	}
}

void CL_DemoPrescanIndex( const char *demoPath ) {
	if ( demoIndex ) {
		Demo_DecoderFree( demoIndex );
		demoIndex = NULL;
	}
	demoPrescanDone = qfalse;
	demoPrescanCpuMsec = 0;

	// keyframes may be off (the capture checks the cvar); the command and event logs are still needed
	demoIndex = Demo_DecoderAlloc( DEMODEC_INDEX );
	if ( !demoIndex || !Demo_DecoderOpen( demoIndex, demoPath ) ) {
		Demo_DecoderFree( demoIndex );
		demoIndex = NULL;
		demoPrescanDone = qtrue;		// no room for a second decoder; seeks index as they go
		return;
	}
	Demo_DecoderReset( demoIndex, 0 );
	CL_DemoPrescanSlice( DEMOKF_PRESCAN_OPEN_MSEC );
}

void CL_DemoPrescanStep( int budgetMsec ) {
	if ( demoPrescanDone || !demoIndex ) {
		return;
	}
	CL_DemoPrescanSlice( budgetMsec );
}

// the map is loaded and its loading screen is up: index the rest of the file now, within reason
void CL_DemoPrescanAtLoad( void ) {
	if ( demoPrescanDone || !demoIndex ) {
		return;
	}
	CL_DemoPrescanSlice( DEMOKF_PRESCAN_LOAD_MSEC );
}

void CL_DemoCacheFree( void ) {
	if ( demoCacheSnaps ) {
		free( demoCacheSnaps );
		demoCacheSnaps = NULL;
	}
	if ( demoCacheEnts ) {
		free( demoCacheEnts );
		demoCacheEnts = NULL;
	}
	if ( demoCacheLowSnaps ) {
		free( demoCacheLowSnaps );
		demoCacheLowSnaps = NULL;
	}
	if ( demoCacheLowEnts ) {
		free( demoCacheLowEnts );
		demoCacheLowEnts = NULL;
	}
	demoCacheLowCount = demoCacheLowEntWrite = 0;
	demoCacheLowActive = qfalse;
	CL_DemoRefillEnd();
	CL_DemoCacheLowReset();
	demoCacheCount = demoCacheOldest = demoCacheEntWrite = 0;
	demoCacheEntsUsed = 0;
	demoCacheRefillExhausted = qfalse;
}

static int	demoRecordOffset = -1;			// record currently being read
static int	demoStreamSegment = -1;			// gamestate the decoder is under
static int	demoLoadedSegmentOffset = -1;	// gamestate the loaded world came from

void CL_DemoSetParsingSegment( int fileOffset ) {
	demoRecordOffset = fileOffset;
}

int CL_DemoParsingSegment( void ) {
	return demoRecordOffset;
}

void CL_DemoNoteStreamSegment( void ) {
	demoStreamSegment = demoRecordOffset;
}

int CL_DemoStreamSegment( void ) {
	return demoStreamSegment;
}

static void CL_DemoSetStreamSegment( int fileOffset ) {
	demoStreamSegment = fileOffset;
}

int CL_DemoLoadedSegment( void ) {
	return demoLoadedSegmentOffset;
}

int CL_DemoSettleOffset( void ) {
	int gs = CL_DemoLoadedGamestate();

	return gs < demoRaw.numGs ? demoRaw.gs[gs].settleOffset : 0;
}

// the decoder stands within DEMO_EOF_SLACK_MSEC of the file's final snapshot, under the final gamestate
#define DEMO_EOF_SLACK_MSEC		(1500)

qboolean CL_DemoAtScanEnd( void ) {
	if ( !demoRaw.numGs || !demoFwd ) {
		return qtrue;
	}
	if ( demoFwd->streamSegment != demoRaw.gs[demoRaw.numGs - 1].fileOffset ) {
		return qfalse;
	}
	return (qboolean)( !demoFwd->snap.valid || demoFwd->snap.serverTime >= demoRaw.lastTime - DEMO_EOF_SLACK_MSEC );
}

void CL_DemoSetLoadedSegment( int fileOffset ) {
	demoLoadedSegmentOffset = fileOffset;
	CL_DemoApiDetach();
}

void CL_DemoCacheClear( void ) {
	CL_DemoRefillEnd();
	demoCacheTopTrimmed = qfalse;
	CL_DemoCacheLowReset();
	demoCacheCount = demoCacheOldest = demoCacheEntWrite = 0;
	demoCacheEntsUsed = 0;
}

// the decoder was moved above the cached run; what lies between will never be decoded into it
void CL_DemoCacheBreakIfAbove( const demoDecoder_t *d ) {
	if ( demoCacheCount > 0 && d && d->snap.valid
		&& d->snap.serverTime - CL_DemoCacheAt( demoCacheCount - 1 )->snap.serverTime > DEMOCACHE_SEAM_MSEC ) {
		CL_DemoCacheClear();
	}
}

// the live run: as many snapshots as the demo has, up to the ceiling
void CL_DemoCacheAlloc( void ) {
	CL_DemoCacheFree();

	demoCacheMaxSnaps = demoRaw.numSnaps + 64;
	if ( demoCacheMaxSnaps < DEMOCACHE_MIN_SNAPS ) {
		demoCacheMaxSnaps = DEMOCACHE_MIN_SNAPS;
	}
	if ( demoCacheMaxSnaps > DEMOCACHE_MAX_SNAPS ) {
		demoCacheMaxSnaps = DEMOCACHE_MAX_SNAPS;
	}
	// room for dense snapshots, so a short demo is held whole; the cap is what a long demo gets anyway
	demoCacheMaxEnts = demoCacheMaxSnaps * ( MAX_ENTITIES_IN_SNAPSHOT / 2 );
	if ( demoCacheMaxEnts > DEMOCACHE_MAX_ENTS ) {
		demoCacheMaxEnts = DEMOCACHE_MAX_ENTS;
	}
	demoCacheLowMaxSnaps = demoCacheMaxSnaps / 2;
	demoCacheLowMaxEnts = demoCacheMaxEnts * 3 / 5;

	demoCacheSnaps = (demoCacheEntry_t *)malloc( sizeof( demoCacheEntry_t ) * demoCacheMaxSnaps );
	demoCacheEnts = (entityState_t *)malloc( sizeof( entityState_t ) * demoCacheMaxEnts );

	if ( !demoCacheSnaps || !demoCacheEnts ) {
		// a cache is optional -- without it rewind simply has nothing to play, which is
		// far better than refusing to open the demo at all
		Com_Printf( "demo cache: allocation failed, backward playback unavailable\n" );
		CL_DemoCacheFree();
		return;
	}

	DemoDbg( "cache: %d snapshots + %d entities = %.1f MB",
		demoCacheMaxSnaps, demoCacheMaxEnts,
		( sizeof( demoCacheEntry_t ) * demoCacheMaxSnaps
		+ sizeof( entityState_t ) * demoCacheMaxEnts ) / ( 1024.0 * 1024.0 ) );
}

// the staging run and the refill decoder, only once something is rewound
static qboolean CL_DemoCacheAllocLow( void ) {
	if ( demoCacheLowSnaps && demoCacheLowEnts && demoRefill ) {
		return qtrue;
	}
	if ( !demoCacheLowSnaps ) {
		demoCacheLowSnaps = (demoCacheEntry_t *)malloc( sizeof( demoCacheEntry_t ) * demoCacheLowMaxSnaps );
	}
	if ( !demoCacheLowEnts ) {
		demoCacheLowEnts = (entityState_t *)malloc( sizeof( entityState_t ) * demoCacheLowMaxEnts );
	}
	if ( !demoRefill ) {
		demoRefill = Demo_DecoderAlloc( DEMODEC_REFILL );
		if ( demoRefill && !Demo_DecoderOpen( demoRefill, demoFwdPath ) ) {
			Demo_DecoderFree( demoRefill );
			demoRefill = NULL;
		}
	}
	if ( !demoCacheLowSnaps || !demoCacheLowEnts || !demoRefill ) {
		Com_Printf( "demo cache: allocation failed, backward playback unavailable\n" );
		return qfalse;
	}
	CL_DemoCacheLowReset();
	return qtrue;
}

// ring index of entry i counted from the oldest
demoCacheEntry_t *CL_DemoCacheAt( int i ) {
	return &demoCacheSnaps[ ( demoCacheOldest + i ) % demoCacheMaxSnaps ];
}

static void CL_DemoCacheDropOldest( void ) {
	if ( demoCacheCount <= 0 ) {
		return;
	}
	demoCacheEntsUsed -= CL_DemoCacheAt( 0 )->snap.numEntities;
	demoCacheOldest = ( demoCacheOldest + 1 ) % demoCacheMaxSnaps;
	demoCacheCount--;
}

// free entity slots between the write cursor and the oldest entry still using the pool
static int CL_DemoCacheEntsFree( void ) {
	if ( demoCacheCount <= 0 ) {
		return demoCacheMaxEnts;
	}
	return demoCacheMaxEnts - demoCacheEntsUsed;
}

static void CL_DemoCacheLowReset( void ) {
	demoCacheLowOldest = demoCacheLowCount = 0;
	demoCacheLowEntStart = demoCacheLowEntWrite = 0;
	demoCacheLowEntsUsed = 0;
}

static demoCacheEntry_t *CL_DemoCacheLowAt( int i ) {
	return &demoCacheLowSnaps[ ( demoCacheLowOldest + i ) % demoCacheLowMaxSnaps ];
}

static int CL_DemoCacheLowEntsFree( void ) {
	if ( demoCacheLowCount <= 0 ) {
		return demoCacheLowMaxEnts;
	}
	return demoCacheLowMaxEnts - demoCacheLowEntsUsed;
}

static void CL_DemoCacheLowDropOldest( void ) {
	if ( demoCacheLowCount <= 0 ) {
		return;
	}
	demoCacheLowEntsUsed -= CL_DemoCacheLowAt( 0 )->snap.numEntities;
	demoCacheLowOldest = ( demoCacheLowOldest + 1 ) % demoCacheLowMaxSnaps;
	demoCacheLowCount--;
	demoCacheLowEntStart = ( demoCacheLowCount > 0 ) ? CL_DemoCacheLowAt( 0 )->firstEnt : demoCacheLowEntWrite;
}

// drop the NEWEST staged entry: the end of the staged run, the part a commit takes first
static void CL_DemoCacheLowDropNewest( void ) {
	if ( demoCacheLowCount <= 0 ) {
		return;
	}
	demoCacheLowCount--;
	demoCacheLowEntsUsed -= CL_DemoCacheLowAt( demoCacheLowCount )->snap.numEntities;
	demoCacheLowEntWrite = CL_DemoCacheLowAt( demoCacheLowCount )->firstEnt;
}

// drop the NEWEST live entry. During a rewind this is already-watched material the playhead will
// never revisit, which makes it the right thing to sacrifice for room to extend downward.
static void CL_DemoCacheDropNewest( void ) {
	if ( demoCacheCount <= 0 ) {
		return;
	}
	demoCacheCount--;
	demoCacheEntsUsed -= CL_DemoCacheAt( demoCacheCount )->snap.numEntities;
	demoCacheEntWrite = CL_DemoCacheAt( demoCacheCount )->firstEnt;
}

static qboolean CL_DemoCacheStageSnapshot( const demoDecoder_t *d ) {
	demoCacheEntry_t	*e;
	int					i, n = d->snap.numEntities;

	if ( !demoCacheLowSnaps || !demoCacheLowEnts || n > demoCacheLowMaxEnts ) {
		return qfalse;
	}
	// the spliced run must be sorted; a replay can re-present a time we already staged
	if ( demoCacheLowCount > 0
		&& d->snap.serverTime <= CL_DemoCacheLowAt( demoCacheLowCount - 1 )->snap.serverTime ) {
		return qtrue;
	}

	if ( d->snap.serverTime >= demoRefillFloor ) {
		return qtrue;
	}

	while ( demoCacheLowCount >= demoCacheLowMaxSnaps || CL_DemoCacheLowEntsFree() <= n ) {
		if ( demoCacheLowCount <= 0 ) {
			return qfalse;
		}
		CL_DemoCacheLowDropOldest();
	}

	e = &demoCacheLowSnaps[ ( demoCacheLowOldest + demoCacheLowCount ) % demoCacheLowMaxSnaps ];
	e->snap = d->snap;
	e->firstEnt = demoCacheLowEntWrite;

	for ( i = 0; i < n; i++ ) {
		int from = ( d->snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 );

		demoCacheLowEnts[ ( demoCacheLowEntWrite + i ) % demoCacheLowMaxEnts ] = d->parseEntities[from];
	}
	demoCacheLowEntWrite = ( demoCacheLowEntWrite + n ) % demoCacheLowMaxEnts;
	demoCacheLowEntsUsed += n;
	demoCacheLowCount++;
	return qtrue;
}

qboolean demoCacheTopTrimmed;		// a commit dropped entries the forward decoder had already passed
static qboolean	demoRefillNoRoom;		// the commit stopped for want of room, not of material
static int	demoCacheCommitJoin;		// the live run's oldest time the next chunk must abut, -1 none
static qboolean	demoRefillCommitting;	// the staged run is being spliced in, a chunk a frame

/*
	Splice the newest chunk of the staged run under the live run: the staged entries just below
	the live run's oldest, newest first, so the run is contiguous after every chunk and the copy
	per frame stays small. Returns qfalse when nothing more can be committed.
*/
static qboolean CL_DemoCacheCommitChunk( int keepTo ) {
	int	i, first, need = 0, base, oldOldestEnt, written = 0;

	if ( demoCacheLowCount <= 0 ) {
		return qfalse;
	}
	// the live run must still begin where the last chunk (or the floor) left it
	if ( demoCacheCount <= 0 || CL_DemoCacheAt( 0 )->snap.serverTime != demoCacheCommitJoin ) {
		DemoDbg( "refill: join %d left the cache; %d staged snapshots discarded", demoCacheCommitJoin, demoCacheLowCount );
		CL_DemoCacheLowReset();
		return qfalse;
	}

	// the newest staged entries that fit the chunk
	first = demoCacheLowCount;
	while ( first > 0 ) {
		int n = CL_DemoCacheLowAt( first - 1 )->snap.numEntities;

		if ( need + n > DEMOCACHE_COMMIT_ENTS && first < demoCacheLowCount ) {
			break;
		}
		need += n;
		first--;
	}

	while ( demoCacheCount + ( demoCacheLowCount - first ) > demoCacheMaxSnaps || CL_DemoCacheEntsFree() <= need ) {
		if ( demoCacheCount > 1 && CL_DemoCacheAt( demoCacheCount - 1 )->snap.serverTime > keepTo ) {
			CL_DemoCacheDropNewest();
			demoCacheTopTrimmed = qtrue;
		} else if ( demoCacheLowCount - first > 1 ) {
			need -= CL_DemoCacheLowAt( first )->snap.numEntities;	// a smaller chunk, still abutting
			first++;
		} else {
			break;
		}
	}
	if ( demoCacheCount + ( demoCacheLowCount - first ) > demoCacheMaxSnaps || CL_DemoCacheEntsFree() <= need ) {
		DemoDbg( "refill: chunk of %d (%d ents) does not fit; %d staged discarded", demoCacheLowCount - first, need, demoCacheLowCount );
		demoRefillNoRoom = qtrue;
		CL_DemoCacheLowReset();
		return qfalse;
	}

	oldOldestEnt = CL_DemoCacheAt( 0 )->firstEnt;
	base = ( oldOldestEnt - need + demoCacheMaxEnts ) % demoCacheMaxEnts;

	for ( i = first; i < demoCacheLowCount; i++ ) {
		demoCacheEntry_t	*src = CL_DemoCacheLowAt( i );
		int					dstIdx = ( demoCacheOldest - ( demoCacheLowCount - first ) + ( i - first ) + demoCacheMaxSnaps * 2 ) % demoCacheMaxSnaps;
		demoCacheEntry_t	*dst = &demoCacheSnaps[dstIdx];
		int					j, n = src->snap.numEntities;
		int					at = ( base + written ) % demoCacheMaxEnts;

		*dst = *src;
		dst->firstEnt = at;
		for ( j = 0; j < n; j++ ) {
			demoCacheEnts[ ( at + j ) % demoCacheMaxEnts ] =
				demoCacheLowEnts[ ( src->firstEnt + j ) % demoCacheLowMaxEnts ];
		}
		written += n;
	}

	demoCacheOldest = ( demoCacheOldest - ( demoCacheLowCount - first ) + demoCacheMaxSnaps * 2 ) % demoCacheMaxSnaps;
	demoCacheCount += demoCacheLowCount - first;
	demoCacheEntsUsed += need;
	demoCacheCommitJoin = CL_DemoCacheAt( 0 )->snap.serverTime;

	DemoDbg( "refill: committed %d staged snapshots (%d entities), %d left, cache now %d spanning %d..%d",
		demoCacheLowCount - first, need, first, demoCacheCount,
		CL_DemoCacheAt( 0 )->snap.serverTime,
		CL_DemoCacheAt( demoCacheCount - 1 )->snap.serverTime );

	while ( demoCacheLowCount > first ) {
		CL_DemoCacheLowDropNewest();
	}
	if ( demoCacheLowCount <= 0 ) {
		CL_DemoCacheLowReset();
		return qfalse;
	}
	return qtrue;
}

// what a decoder's record left: the forward decoder feeds the live run, the refill the staging run,
// and both may capture keyframes
void CL_DemoDecoderRecordDone( demoDecoder_t *d ) {
	if ( d->snapDecoded ) {
		CL_DemoCacheAddSnapshot( d );
		if ( d->role == DEMODEC_INDEX ) {
			CL_DemoEventScan( d );
		}
	}
	CL_DemoKeyframeVerify( d );
	CL_DemoKeyframeTrack( d );
	CL_DemoCaptureKeyframe( d );
}

void CL_DemoCacheAddSnapshot( const demoDecoder_t *d ) {
	demoCacheEntry_t	*e;
	int					i, n;

	if ( !demoCacheSnaps || !demoCacheEnts || !clc.demoplaying || !d->snap.valid || d->role == DEMODEC_INDEX ) {
		return;
	}
	{
		int gs = d->gamestate;

		if ( gs < 0 || gs != CL_DemoLoadedGamestate() || d->recordOffset < demoRaw.gs[gs].settleOffset ) {
			return;
		}
	}

	if ( d->role == DEMODEC_REFILL ) {
		if ( demoCacheLowActive ) {
			CL_DemoCacheStageSnapshot( d );
		}
		return;
	}

	n = d->snap.numEntities;
	if ( n > demoCacheMaxEnts ) {
		return;			// cannot happen with MAX_ENTITIES_IN_SNAPSHOT at 256, but do not corrupt the ring if it did
	}

	{
		int have = CL_DemoCacheFindAtOrBefore( d->snap.serverTime );

		if ( have >= 0 && CL_DemoCacheAt( have )->snap.serverTime == d->snap.serverTime ) {
			return;
		}
		if ( demoCacheExtendOnly && have < 0 && demoCacheCount > 0 ) {
			return;
		}
	}

	while ( demoCacheCount > 0 && CL_DemoCacheAt( demoCacheCount - 1 )->snap.serverTime >= d->snap.serverTime ) {
		CL_DemoCacheDropNewest();
	}

	if ( demoCacheCount > 0 && d->snap.serverTime - CL_DemoCacheAt( demoCacheCount - 1 )->snap.serverTime > DEMOCACHE_MAX_REVERSE_GAP_MSEC ) {
		demoCacheOldest = 0;
		demoCacheCount = 0;
		demoCacheEntWrite = 0;
		demoCacheEntsUsed = 0;
	}

	while ( demoCacheCount >= demoCacheMaxSnaps || CL_DemoCacheEntsFree() <= n ) {
		if ( demoCacheCount <= 0 ) {
			return;
		}
		CL_DemoCacheDropOldest();
	}

	e = &demoCacheSnaps[ ( demoCacheOldest + demoCacheCount ) % demoCacheMaxSnaps ];
	e->snap = d->snap;
	e->firstEnt = demoCacheEntWrite;

	for ( i = 0; i < n; i++ ) {
		int from = ( d->snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 );
		int to = ( demoCacheEntWrite + i ) % demoCacheMaxEnts;

		demoCacheEnts[to] = d->parseEntities[from];
	}

	demoCacheEntWrite = ( demoCacheEntWrite + n ) % demoCacheMaxEnts;
	demoCacheEntsUsed += n;
	demoCacheCount++;
}

// newest cached entry at or before serverTime, or -1
int CL_DemoCacheFindAtOrBefore( int serverTime ) {
	int lo = 0, hi = demoCacheCount - 1, best = -1;

	while ( lo <= hi ) {
		int mid = ( lo + hi ) / 2;

		if ( CL_DemoCacheAt( mid )->snap.serverTime <= serverTime ) {
			best = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return best;
}

qboolean		demoRefillActive;
static int			demoRefillBeganAt;

static void CL_DemoRefillEnd( void ) {
	demoRefillActive = qfalse;
	demoCacheLowActive = qfalse;
	demoRefillCommitting = qfalse;
}

void CL_DemoRefillAbandon( void ) {
	CL_DemoRefillEnd();
	CL_DemoCacheLowReset();
}

/*
	Whether a refill could add anything below the live run: the free ring plus what may be dropped
	above keepTo. Without room every refill would decode material only to discard it.
*/
qboolean CL_DemoCacheRoomBelow( int keepTo ) {
	int i, snaps = demoCacheMaxSnaps - demoCacheCount, ents = CL_DemoCacheEntsFree();

	for ( i = demoCacheCount - 1; i > 0 && CL_DemoCacheAt( i )->snap.serverTime > keepTo; i-- ) {
		snaps++;
		ents += CL_DemoCacheAt( i )->snap.numEntities;
	}
	return (qboolean)( snaps >= DEMOCACHE_REFILL_MIN_SNAPS && ents >= DEMOCACHE_REFILL_MIN_SNAPS * MAX_ENTITIES_IN_SNAPSHOT / 2 );
}

qboolean CL_DemoRefillBegin( int goBelowServerTime ) {
	int gs, aim;

	if ( !clc.demofile || demoCacheCount < 1 || demoRefillActive || !CL_DemoCacheAllocLow() ) {
		return qfalse;
	}

	demoRefillFloor = goBelowServerTime;
	demoRefillNoRoom = qfalse;
	CL_DemoCacheLowReset();

	gs = CL_DemoLoadedGamestate();
	aim = demoRefillFloor - DEMOCACHE_REFILL_MARGIN_MSEC;
	if ( gs < demoRaw.numGs && aim < demoRaw.gs[gs].settleTime ) {
		aim = demoRaw.gs[gs].settleTime;
	}

	demoRefill->guard = 0;
	Demo_Reposition( demoRefill, gs, aim );
	if ( demoRefill->eof || demoRefill->fault ) {
		return qfalse;
	}

	demoRefillActive = qtrue;
	demoRefillBeganAt = Sys_Milliseconds();
	DemoDbg( "refill: begin, need material below %d", demoRefillFloor );
	return qtrue;
}

// after the last chunk: did the run get any lower
static void CL_DemoRefillFinish( void ) {
	int below = CL_DemoCacheFindAtOrBefore( demoRefillFloor - 1 );
	int floorAfter = ( below >= 0 ) ? CL_DemoCacheAt( below )->snap.serverTime : demoRefillFloor;

	CL_DemoRefillEnd();
	if ( demoRefillNoRoom ) {
		DemoDbg( "refill: cache full, floor %d -> %d", demoRefillFloor, floorAfter );
	} else if ( below < 0 || demoRefillFloor - floorAfter > DEMOCACHE_MAX_REVERSE_GAP_MSEC ) {
		DemoDbg( "refill: FAILED to get material below %d (best %d)", demoRefillFloor, floorAfter );
		demoCacheRefillExhausted = qtrue;
		demoCacheExhaustedFloor = demoRefillFloor;
	} else {
		DemoDbg( "refill: floor %d -> %d (gained %d)", demoRefillFloor, floorAfter,
			demoRefillFloor - floorAfter );
	}
}

// finishCommit: the viewer is waiting on the staged material, so splice all of it in now
qboolean CL_DemoRefillStep( int budgetMsec, int keepTo, qboolean finishCommit ) {
	int			began = Sys_Milliseconds();
	qboolean	done = qfalse, failed = qfalse;

	if ( !demoRefillActive ) {
		return qtrue;
	}
	if ( demoRefillCommitting ) {
		qboolean	more;
		int			chunks = 0;

		do {
			more = CL_DemoCacheCommitChunk( keepTo );
		} while ( more && finishCommit && ++chunks < 2 );
		if ( !more ) {
			CL_DemoRefillFinish();
			return qtrue;
		}
		return qfalse;
	}

	demoCacheLowActive = qtrue;

	while ( 1 ) {
		if ( demoRefill->eof || demoRefill->guard > MAX_DEMO_TIME_MESSAGES ) {
			done = qtrue;
			break;
		}
		if ( demoRefill->snap.valid && !demoRefill->kfRestoring && demoRefill->snap.serverTime >= demoRefillFloor
			&& demoRefill->recordOffset >= CL_DemoSettleOffset() ) {
			done = qtrue;
			break;			// the run now reaches the floor, which is what makes it abut
		}
		if ( Demo_NextRecordLeavesSegment( demoRefill ) ) {
			done = qtrue;
			break;			// never cross into another map
		}
		if ( !Demo_ReadRecord( demoRefill ) ) {
			if ( demoRefill->fault ) {
				failed = qtrue;
			} else {
				done = qtrue;
			}
			break;
		}
		if ( demoRefill->kfRestoreFailed ) {
			failed = qtrue;
			break;
		}
		if ( budgetMsec > 0 && Sys_Milliseconds() - began >= budgetMsec ) {
			break;			// more to do next frame
		}
	}

	demoCacheLowActive = qfalse;

	if ( failed ) {
		DemoDbg( "refill: decoder lost its place; dropped" );
		CL_DemoRefillAbandon();
		return qtrue;
	}
	if ( !done ) {
		return qfalse;
	}

	DemoDbg( "refill: decode done in %dms wall, %d messages, staged %d, last decoded %d (floor %d)",
		Sys_Milliseconds() - demoRefillBeganAt, demoRefill->guard, demoCacheLowCount,
		demoCacheLowCount > 0 ? CL_DemoCacheLowAt( demoCacheLowCount - 1 )->snap.serverTime : -1,
		demoRefillFloor );

	// the decode is done; the staged run is spliced in a chunk a frame, abutting the floor first
	demoRefillCommitting = qtrue;
	demoCacheCommitJoin = demoRefillFloor;
	if ( !CL_DemoCacheCommitChunk( keepTo ) ) {
		CL_DemoRefillFinish();
		return qtrue;
	}
	return qfalse;
}

int CL_DemoCacheRunFloorTime( int atTime ) {
	int idx = CL_DemoCacheFindAtOrBefore( atTime );

	if ( idx < 0 ) {
		return atTime;
	}
	while ( idx > 0 ) {
		int here = CL_DemoCacheAt( idx )->snap.serverTime;
		int below = CL_DemoCacheAt( idx - 1 )->snap.serverTime;

		if ( here - below > DEMOCACHE_MAX_REVERSE_GAP_MSEC ) {
			break;			// a hole, not the next frame
		}
		idx--;
	}
	return CL_DemoCacheAt( idx )->snap.serverTime;
}
