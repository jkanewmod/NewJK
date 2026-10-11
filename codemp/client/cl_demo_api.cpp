// cl_demo_api.cpp -- demo playback service: the command log, the presented configstrings and the traps

#include <limits.h>
#include "client.h"
#include "cl_cgameapi.h"
#include "cl_uiapi.h"
#include "qcommon/stringed_ingame.h"
#include "cl_demo_local.h"

typedef struct demoCmdEntry_s {
	int		serverTime;
	int		rawSeq;
	int		gamestate;
	int		csIndex;			// "cs" index, else -1
	int		textOffset;
	int		prevSameIndex;		// the previous "cs" entry for the same index, -1 none
} demoCmdEntry_t;

#define DEMOCMD_GROW_ENTRIES	4096
#define DEMOCMD_GROW_POOL		( 512 * 1024 )
#define DEMOCMD_CEIL_ENTRIES	( 1 << 22 )
#define DEMOCMD_CEIL_POOL		( 256 * 1024 * 1024 )

static demoCmdEntry_t	*demoCmdLog;
static int				demoCmdCount, demoCmdMax;
static char				*demoCmdPool;
static int				demoCmdPoolUsed, demoCmdPoolMax;
static int				demoCmdFileSize;		// the log can hold no more than the file does
static qboolean			demoCmdDropped;
static int				demoCmdLastOfIndex[MAX_CONFIGSTRINGS];	// newest "cs" entry per index, -1 none

static void CL_DemoCmdLogFree( void ) {
	int i;

	if ( demoCmdLog ) { free( demoCmdLog ); demoCmdLog = NULL; }
	if ( demoCmdPool ) { free( demoCmdPool ); demoCmdPool = NULL; }
	demoCmdCount = demoCmdMax = 0;
	demoCmdPoolUsed = demoCmdPoolMax = 0;
	demoCmdDropped = qfalse;
	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		demoCmdLastOfIndex[i] = -1;
	}
}

void CL_DemoCmdLogSetFileSize( int bytes ) {
	demoCmdFileSize = bytes > 0 ? bytes : 0;
}

// a logged command takes at least six bytes of file (sequence word, a character, a terminator)
static int CL_DemoCmdMaxEntries( void ) {
	int n = demoCmdFileSize / 6 + DEMOCMD_GROW_ENTRIES;

	return n > DEMOCMD_CEIL_ENTRIES ? DEMOCMD_CEIL_ENTRIES : n;
}

static int CL_DemoCmdMaxPool( void ) {
	int n = demoCmdFileSize + 64 * 1024;

	if ( n < DEMOCMD_GROW_POOL ) {
		n = DEMOCMD_GROW_POOL;
	}
	return n > DEMOCMD_CEIL_POOL ? DEMOCMD_CEIL_POOL : n;
}

static void CL_DemoCmdLogDrop( void ) {
	if ( !demoCmdDropped ) {
		demoCmdDropped = qtrue;
		Com_Printf( S_COLOR_YELLOW "Demo command log is full; configstrings may be wrong after a seek.\n" );
	}
}

// first entry at or after (gs, seq) in stream order
static int CL_DemoCmdAtOrAfter( int gs, int seq ) {
	int lo = 0, hi = demoCmdCount;

	while ( lo < hi ) {
		int						mid = ( lo + hi ) / 2;
		const demoCmdEntry_t	*e = &demoCmdLog[mid];

		if ( e->gamestate < gs || ( e->gamestate == gs && e->rawSeq < seq ) ) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo;
}

void CL_DemoCmdLogAdd( int gs, int serverCommandNumber, int t, const char *s ) {
	demoCmdEntry_t			*e;
	int						len, at, csIndex, prev;
	qboolean				newest = qtrue;		// the newest entry of its index

	if ( !clc.demoplaying || !s || gs < 0 ) {
		return;
	}

	at = demoCmdCount;
	if ( demoCmdCount > 0 ) {
		const demoCmdEntry_t *last = &demoCmdLog[demoCmdCount - 1];

		if ( gs < last->gamestate || ( gs == last->gamestate && serverCommandNumber <= last->rawSeq ) ) {
			at = CL_DemoCmdAtOrAfter( gs, serverCommandNumber );
			if ( at < demoCmdCount && demoCmdLog[at].gamestate == gs && demoCmdLog[at].rawSeq == serverCommandNumber ) {
				return;
			}
		}
	}

	len = (int)strlen( s ) + 1;
	if ( demoCmdCount >= demoCmdMax ) {
		int				newMax = demoCmdMax ? demoCmdMax * 2 : DEMOCMD_GROW_ENTRIES, cap = CL_DemoCmdMaxEntries();
		demoCmdEntry_t	*grown;

		if ( newMax > cap ) {
			newMax = cap;
		}
		if ( demoCmdCount >= newMax ) {
			CL_DemoCmdLogDrop();
			return;
		}
		grown = (demoCmdEntry_t *)realloc( demoCmdLog, newMax * sizeof( demoCmdEntry_t ) );
		if ( !grown ) {
			CL_DemoCmdLogDrop();
			return;
		}
		demoCmdLog = grown;
		demoCmdMax = newMax;
	}
	if ( demoCmdPoolUsed + len > demoCmdPoolMax ) {
		int		newMax = demoCmdPoolMax ? demoCmdPoolMax * 2 : DEMOCMD_GROW_POOL, cap = CL_DemoCmdMaxPool();
		char	*grown;

		while ( newMax < demoCmdPoolUsed + len ) {
			newMax *= 2;
		}
		if ( newMax > cap ) {
			newMax = cap;
		}
		if ( demoCmdPoolUsed + len > newMax ) {
			CL_DemoCmdLogDrop();
			return;
		}
		grown = (char *)realloc( demoCmdPool, newMax );
		if ( !grown ) {
			CL_DemoCmdLogDrop();
			return;
		}
		demoCmdPool = grown;
		demoCmdPoolMax = newMax;
	}

	csIndex = !strncmp( s, "cs ", 3 ) ? atoi( s + 3 ) : -1;
	if ( csIndex >= MAX_CONFIGSTRINGS ) {
		csIndex = -1;
	}
	prev = csIndex >= 0 ? demoCmdLastOfIndex[csIndex] : -1;
	if ( at < demoCmdCount ) {
		qboolean linked = qfalse;
		int		i;

		// an insert out of stream order: shift the tail and relink it in the same pass
		memmove( &demoCmdLog[at + 1], &demoCmdLog[at], ( demoCmdCount - at ) * sizeof( demoCmdEntry_t ) );
		for ( i = at + 1; i <= demoCmdCount; i++ ) {
			demoCmdEntry_t *x = &demoCmdLog[i];

			if ( x->prevSameIndex >= at ) {
				x->prevSameIndex++;
			} else if ( !linked && csIndex >= 0 && x->csIndex == csIndex ) {
				prev = x->prevSameIndex;		// the first later entry of the index now follows this one
				x->prevSameIndex = at;
				linked = qtrue;
			}
		}
		for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
			if ( demoCmdLastOfIndex[i] >= at ) {
				demoCmdLastOfIndex[i]++;
			}
		}
		newest = (qboolean)!linked;
	}
	e = &demoCmdLog[at];
	demoCmdCount++;
	e->serverTime = t;
	e->rawSeq = serverCommandNumber;
	e->gamestate = gs;
	e->textOffset = demoCmdPoolUsed;
	e->csIndex = csIndex;
	e->prevSameIndex = prev;
	if ( csIndex >= 0 && newest ) {
		demoCmdLastOfIndex[csIndex] = at;
	}
	memcpy( demoCmdPool + demoCmdPoolUsed, s, len );
	demoCmdPoolUsed += len;
}

static const char *CL_DemoCmdText( const demoCmdEntry_t *e ) {
	return demoCmdPool + e->textOffset;
}

// [*lo, *hi) holds the gamestate's entries
static void CL_DemoCmdRange( int gs, int *lo, int *hi ) {
	int a = 0, b = demoCmdCount;

	while ( a < b ) {
		int mid = ( a + b ) / 2;

		if ( demoCmdLog[mid].gamestate < gs ) {
			a = mid + 1;
		} else {
			b = mid;
		}
	}
	*lo = a;
	b = demoCmdCount;
	while ( a < b ) {
		int mid = ( a + b ) / 2;

		if ( demoCmdLog[mid].gamestate <= gs ) {
			a = mid + 1;
		} else {
			b = mid;
		}
	}
	*hi = a;
}

// first entry of the gamestate with serverTime > t (inclusive: >= t)
static int CL_DemoCmdTimeSearch( int gs, int t, qboolean inclusive, int *end ) {
	int lo, hi;

	CL_DemoCmdRange( gs, &lo, &hi );
	*end = hi;
	while ( lo < hi ) {
		int mid = ( lo + hi ) / 2;

		if ( inclusive ? demoCmdLog[mid].serverTime < t : demoCmdLog[mid].serverTime <= t ) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo;
}

/*
===================
The entity-event log: the index pass keeps the state of every entity carrying one of the events
the game module asked for (cl_demoIndexEvents), the moment it first appears, so "who killed whom",
a capture's banner data or a body fade can be answered for any time without decoding back to it
===================
*/
typedef struct demoEventEntry_s {
	int				serverTime;
	int				gamestate;
	int				event;
	entityState_t	es;
} demoEventEntry_t;

#define DEMOEV_GROW			1024
#define DEMOEV_FLOOR		32768
#define DEMOEV_CEIL			( 1 << 18 )	// entries hold an entityState_t
#define DEMOEV_REPEAT_MSEC	2000	// the same event on the same slot again this soon is the same event

static demoEventEntry_t	*demoEvLog;
static int				demoEvCount, demoEvMax;
static int				demoEvSet[DEMOAPI_MAX_INDEX_EVENTS];
static int				demoEvSetCount;
static qboolean			demoEvDropped;

static void CL_DemoEventLogFree( void ) {
	if ( demoEvLog ) { free( demoEvLog ); demoEvLog = NULL; }
	demoEvCount = demoEvMax = 0;
	demoEvSetCount = 0;
	demoEvDropped = qfalse;
}

// the game module's ui names the events, as numbers, before any demo opens
static void CL_DemoEventSetRead( void ) {
	const char	*p = Cvar_VariableString( "cl_demoIndexEvents" );

	demoEvSetCount = 0;
	while ( *p && demoEvSetCount < DEMOAPI_MAX_INDEX_EVENTS ) {
		char	*end;
		long	v = strtol( p, &end, 10 );

		if ( end == p ) {
			p++;
			continue;
		}
		if ( v > 0 && v < ( 1 << 16 ) ) {
			demoEvSet[demoEvSetCount++] = (int)v;
		}
		p = end;
	}
}

static qboolean CL_DemoEventWanted( int event ) {
	int i;

	for ( i = 0; i < demoEvSetCount; i++ ) {
		if ( demoEvSet[i] == event ) {
			return qtrue;
		}
	}
	return qfalse;
}

// grows on demand; the bound only scales with the file so a long recording keeps its later events
static int CL_DemoEventMaxEntries( void ) {
	int n = demoCmdFileSize / 2048;

	return n < DEMOEV_FLOOR ? DEMOEV_FLOOR : n > DEMOEV_CEIL ? DEMOEV_CEIL : n;
}

static void CL_DemoEventLogAdd( int gs, int serverTime, int event, const entityState_t *es ) {
	demoEventEntry_t *e;

	if ( demoEvCount >= demoEvMax ) {
		int					newMax = demoEvMax ? demoEvMax * 2 : DEMOEV_GROW;
		demoEventEntry_t	*grown;

		if ( newMax > CL_DemoEventMaxEntries() ) {
			newMax = CL_DemoEventMaxEntries();
		}
		if ( demoEvCount >= newMax ) {
			if ( !demoEvDropped ) {
				demoEvDropped = qtrue;
				Com_Printf( S_COLOR_YELLOW "Demo event log is full; some events after this point are not indexed.\n" );
			}
			return;
		}
		grown = (demoEventEntry_t *)realloc( demoEvLog, newMax * sizeof( demoEventEntry_t ) );
		if ( !grown ) {
			return;
		}
		demoEvLog = grown;
		demoEvMax = newMax;
	}
	e = &demoEvLog[demoEvCount++];
	e->serverTime = serverTime;
	e->gamestate = gs;
	e->event = event;
	e->es = *es;
}

// a decoded snapshot: log each wanted event the first time its entity carries it
void CL_DemoEventScan( demoDecoder_t *d ) {
	int i;

	if ( !demoEvSetCount || !d->snap.valid || d->gamestate < 0 || d->gamestate >= demoRaw.numGs
		|| d->recordOffset < demoRaw.gs[d->gamestate].settleOffset ) {
		return;		// before the level settles the snapshots carry the previous clock
	}
	if ( d->evLastGamestate != d->gamestate ) {
		Com_Memset( d->evLastKey, 0, sizeof( d->evLastKey ) );
		d->evLastGamestate = d->gamestate;
		d->evPrevMessage = -1;
	}
	for ( i = 0; i < d->snap.numEntities; i++ ) {
		const entityState_t	*es = &d->parseEntities[( d->snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];
		int					event, key, num = es->number;

		if ( num < 0 || num >= MAX_GENTITIES ) {
			continue;
		}
		if ( es->eType >= ET_EVENTS ) {
			event = es->eType - ET_EVENTS;
		} else {
			event = es->event & ~EV_EVENT_BITS;
		}
		if ( !event || !CL_DemoEventWanted( event ) ) {
			d->evLastKey[num] = 0;
			continue;
		}
		key = ( event << 20 ) ^ ( ( es->eventParm & 0xff ) << 12 ) ^ ( es->otherEntityNum & 0x3ff ) ^ ( ( es->otherEntityNum2 & 0x3ff ) << 10 ) | 1;
		// a temp entity repeats only while it stays in consecutive decoded snapshots
		if ( d->evLastKey[num] == key && d->snap.serverTime >= d->evLastTime[num]
			&& d->snap.serverTime - d->evLastTime[num] < DEMOEV_REPEAT_MSEC
			&& ( es->eType < ET_EVENTS || d->evLastMessage[num] == d->evPrevMessage ) ) {
			d->evLastTime[num] = d->snap.serverTime;
			d->evLastMessage[num] = d->snap.messageNum;
			continue;
		}
		d->evLastKey[num] = key;
		d->evLastTime[num] = d->snap.serverTime;
		d->evLastMessage[num] = d->snap.messageNum;
		CL_DemoEventLogAdd( d->gamestate, d->snap.serverTime, event, es );
	}
	d->evPrevMessage = d->snap.messageNum;
}

// first entry of the gamestate at or after t (inclusive) / after t
static int CL_DemoEventSearch( int gs, int t, qboolean inclusive ) {
	int lo = 0, hi = demoEvCount;

	while ( lo < hi ) {
		int						mid = ( lo + hi ) / 2;
		const demoEventEntry_t	*e = &demoEvLog[mid];

		if ( e->gamestate < gs || ( e->gamestate == gs && ( inclusive ? e->serverTime < t : e->serverTime <= t ) ) ) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo;
}

int CL_DemoApiEvents( demoApiEventCursor_t *c, entityState_t *out ) {
	int gs;

	if ( !c || !out || !clc.demoplaying || !demoEvLog ) {
		return 0;
	}
	gs = c->gamestate < 0 ? CL_DemoLoadedGamestate() : c->gamestate;
	if ( gs < 0 || gs >= demoRaw.numGs || c->fromTime > c->toTime ) {
		return 0;
	}
	if ( !c->state ) {
		c->cursor = c->newestFirst ? CL_DemoEventSearch( gs, c->toTime, qfalse ) - 1 : CL_DemoEventSearch( gs, c->fromTime, qtrue );
		c->state = 1;
	}
	while ( c->cursor >= 0 && c->cursor < demoEvCount ) {
		const demoEventEntry_t *e = &demoEvLog[c->cursor];

		if ( e->gamestate != gs || e->serverTime < c->fromTime || e->serverTime > c->toTime ) {
			break;
		}
		c->cursor += c->newestFirst ? -1 : 1;
		if ( ( c->event >= 0 && e->event != c->event ) || ( c->entityNum >= 0 && e->es.number != c->entityNum ) ) {
			continue;
		}
		*out = e->es;
		c->evTime = e->serverTime;
		c->evEvent = e->event;
		return 1;
	}
	return 0;
}

static gameState_t	*demoSegBaseGs[DEMOAPI_MAX_GAMESTATES];

static void CL_DemoSegBaseFree( void ) {
	int i;

	for ( i = 0; i < DEMOAPI_MAX_GAMESTATES; i++ ) {
		if ( demoSegBaseGs[i] ) {
			free( demoSegBaseGs[i] );
			demoSegBaseGs[i] = NULL;
		}
	}
}

static void CL_DemoSegBaseSet( int gs, const gameState_t *table ) {
	if ( gs < 0 || gs >= DEMOAPI_MAX_GAMESTATES ) {
		return;
	}
	if ( !demoSegBaseGs[gs] ) {
		demoSegBaseGs[gs] = (gameState_t *)malloc( sizeof( gameState_t ) );
	}
	if ( demoSegBaseGs[gs] ) {
		*demoSegBaseGs[gs] = *table;
	}
}

// the live parser loaded a map: its table is the gamestate's base, if no decoder recorded it yet
void CL_DemoNoteGamestateParsed( void ) {
	int gs = CL_DemoGamestateIndexOf( CL_DemoParsingSegment() );

	if ( gs >= 0 && gs < DEMOAPI_MAX_GAMESTATES && !demoSegBaseGs[gs] ) {
		CL_DemoSegBaseSet( gs, &cl.gameState );
	}
}

void CL_DemoNoteGamestateFrom( const demoDecoder_t *d ) {
	if ( d->gamestate >= 0 && d->gamestate < DEMOAPI_MAX_GAMESTATES && !demoSegBaseGs[d->gamestate] ) {
		CL_DemoSegBaseSet( d->gamestate, &d->gameState );
	}
}

#define CL_DemoGsSet	Demo_GsSet

static void CL_DemoGsApplyEntry( gameState_t *gs, const demoCmdEntry_t *e ) {
	if ( e->csIndex < 0 ) {
		return;
	}
	Cmd_TokenizeString( CL_DemoCmdText( e ) );
	CL_DemoGsSet( gs, atoi( Cmd_Argv( 1 ) ), Cmd_ArgsFrom( 2 ) );
}

static gameState_t	demoPresentedGs;
static int			demoPresentedTime = -1;
static int			demoPresentedPartial = -1;	// >= 0: only the log entries at demoPresentedTime up to this one are in
static int			demoPresentedGamestate = -1;
static qboolean		demoPresentedValid;

// a keyframe's table already holds its record's commands; applying those again is harmless, hence "at or after"
static void CL_DemoPresentRebuild( int gs, int T ) {
	int			from = INT_MIN, i, k, end;
	qboolean	haveBase = qfalse;

	k = CL_DemoKeyframeBefore( gs, T + 1 );
	if ( k >= 0 && demoKeyframes[k].gameState ) {
		demoPresentedGs = *demoKeyframes[k].gameState;
		from = demoKeyframes[k].serverTime;
		haveBase = qtrue;
	}
	if ( !haveBase && gs >= 0 && gs < DEMOAPI_MAX_GAMESTATES && demoSegBaseGs[gs] ) {
		demoPresentedGs = *demoSegBaseGs[gs];
		haveBase = qtrue;
	}
	if ( !haveBase ) {
		demoPresentedGs = cl.gameState;
		demoPresentedTime = T;
		demoPresentedPartial = -1;
		demoPresentedGamestate = gs;
		demoPresentedValid = qtrue;
		return;
	}

	for ( i = CL_DemoCmdTimeSearch( gs, from, qtrue, &end ); i < end; i++ ) {
		if ( demoCmdLog[i].serverTime > T ) {
			break;
		}
		CL_DemoGsApplyEntry( &demoPresentedGs, &demoCmdLog[i] );
	}

	demoPresentedTime = T;
	demoPresentedPartial = -1;
	demoPresentedGamestate = gs;
	demoPresentedValid = qtrue;
}

static int	demoApiDiff[MAX_CONFIGSTRINGS];
static int	demoApiDiffCount;

/*
	Move the presented table from demoPresentedTime to T, either way, by the "cs" entries between
	the two times alone: each index they touch takes the newest value at or before T from its chain
	(the gamestate's base table when it has none), and only those that change are listed as the
	diff. Independent of how far back the nearest keyframe is. qfalse when there is no base table to
	fall back to, in which case the caller rebuilds from a keyframe
*/
static qboolean CL_DemoPresentMove( int gs, int T ) {
	static int	oldestTouched[MAX_CONFIGSTRINGS], newestTouched[MAX_CONFIGSTRINGS];
	static int	touchedList[MAX_CONFIGSTRINGS];
	static qboolean	scratchReady;
	int			lo = T < demoPresentedTime ? T : demoPresentedTime;
	int			hi = T < demoPresentedTime ? demoPresentedTime : T;
	int			i, n, end, numTouched = 0;
	qboolean	backward = (qboolean)( T < demoPresentedTime );

	if ( gs >= DEMOAPI_MAX_GAMESTATES || !demoSegBaseGs[gs] ) {
		return qfalse;
	}
	if ( !scratchReady ) {
		scratchReady = qtrue;
		for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
			oldestTouched[i] = newestTouched[i] = -1;	// every call leaves them this way for the indices it used
		}
	}
	demoApiDiffCount = 0;
	// the "cs" entries in (lo, hi]: going forward the newest of them for an index is its value at T;
	// going back, the entry before the oldest of them (at or before lo, by construction) is
	for ( i = CL_DemoCmdTimeSearch( gs, lo, qfalse, &end ); i < end; i++ ) {
		const demoCmdEntry_t *e = &demoCmdLog[i];

		if ( e->serverTime > hi ) {
			break;
		}
		if ( e->csIndex >= 0 && e->csIndex < MAX_CONFIGSTRINGS ) {
			if ( oldestTouched[e->csIndex] < 0 ) {
				oldestTouched[e->csIndex] = i;
				touchedList[numTouched++] = e->csIndex;
			}
			newestTouched[e->csIndex] = i;
		}
	}
	for ( n = 0; n < numTouched; n++ ) {
		const char	*value;
		int			k;

		i = touchedList[n];
		if ( backward ) {
			// the previous "cs" of this index in the same gamestate, if there is one
			k = demoCmdLog[oldestTouched[i]].prevSameIndex;
			if ( k >= 0 && demoCmdLog[k].gamestate != gs ) {
				k = -1;
			}
		} else {
			k = newestTouched[i];
		}
		oldestTouched[i] = newestTouched[i] = -1;
		if ( k >= 0 ) {
			Cmd_TokenizeString( CL_DemoCmdText( &demoCmdLog[k] ) );
			value = Cmd_ArgsFrom( 2 );
		} else {
			value = demoSegBaseGs[gs]->stringData + demoSegBaseGs[gs]->stringOffsets[i];
		}
		if ( strcmp( demoPresentedGs.stringData + demoPresentedGs.stringOffsets[i], value ) ) {
			CL_DemoGsSet( &demoPresentedGs, i, value );
			demoApiDiff[demoApiDiffCount++] = i;
		}
	}
	demoPresentedTime = T;
	demoPresentedPartial = -1;
	return qtrue;
}

static void CL_DemoPresentAdvance( int gs, int T ) {
	int i, lo, end;

	if ( !demoPresentedValid || T < demoPresentedTime || gs != demoPresentedGamestate ) {
		CL_DemoPresentRebuild( gs, T );
		return;
	}
	if ( demoPresentedPartial >= 0 ) {
		CL_DemoCmdRange( gs, &lo, &end );
		i = demoPresentedPartial + 1;	// the rest of the entries at demoPresentedTime
	} else {
		i = CL_DemoCmdTimeSearch( gs, demoPresentedTime, qfalse, &end );
	}
	for ( ; i < end; i++ ) {
		if ( demoCmdLog[i].serverTime > T ) {
			break;
		}
		CL_DemoGsApplyEntry( &demoPresentedGs, &demoCmdLog[i] );
	}
	demoPresentedTime = T;
	demoPresentedPartial = -1;
}

// the table as the game module should see it when log entry idx is delivered: everything before
// it applied, nothing after it, so two "cs" at one time are seen one at a time, as in live play
static void CL_DemoPresentAdvanceThrough( int gs, int idx ) {
	int T = demoCmdLog[idx].serverTime, i, lo, end;

	if ( !demoPresentedValid || gs != demoPresentedGamestate || T < demoPresentedTime ) {
		CL_DemoPresentRebuild( gs, T - 1 );
	} else if ( demoPresentedTime < T ) {
		CL_DemoPresentAdvance( gs, T - 1 );
	} else if ( demoPresentedPartial < 0 || demoPresentedPartial >= idx ) {
		return;		// in already
	}
	if ( demoPresentedTime == T ) {
		CL_DemoCmdRange( gs, &lo, &end );
		i = demoPresentedPartial + 1;
	} else {
		i = CL_DemoCmdTimeSearch( gs, T, qtrue, &end );
	}
	for ( ; i <= idx && i < end; i++ ) {
		CL_DemoGsApplyEntry( &demoPresentedGs, &demoCmdLog[i] );
	}
	demoPresentedTime = T;
	demoPresentedPartial = idx;
}

static float	demoApiTimeFrac;

qboolean demoApiServiceDemo;		// a game module has driven this demo; the end holds instead of disconnecting

void CL_DemoApiReset( void ) {
	demoApiAttached = qfalse;
	demoApiServiceDemo = qfalse;
	demoApiPlayhead = 0;
	demoApiResumeTarget = -1;
	demoApiResumeGamestate = -1;
	demoApiTimeFrac = 0.0f;
	demoPresentedValid = qfalse;
	demoPresentedTime = -1;
	demoPresentedPartial = -1;
	demoPresentedGamestate = -1;
	demoCacheExtendOnly = qfalse;
	CL_DemoCmdLogFree();
	CL_DemoEventLogFree();
	CL_DemoSegBaseFree();
	Demo_DecoderFree( demoFwd );
	Demo_DecoderFree( demoRefill );
	Demo_DecoderFree( demoIndex );
	demoFwd = demoRefill = demoIndex = NULL;
	demoFwdPath[0] = '\0';
	demoPrescanDone = qfalse;
}

// the forward decoder, at the first gamestate's record; the refill and index decoders come later
void CL_DemoApiOpen( const char *demoPath ) {
	Q_strncpyz( demoFwdPath, demoPath, sizeof( demoFwdPath ) );
	CL_DemoEventSetRead();
	demoFwd = Demo_DecoderAlloc( DEMODEC_FORWARD );
	if ( demoFwd && !Demo_DecoderOpen( demoFwd, demoPath ) ) {
		Demo_DecoderFree( demoFwd );
		demoFwd = NULL;
	}
	if ( demoFwd ) {
		Demo_DecoderReset( demoFwd, demoRaw.numGs > 0 ? demoRaw.gs[0].fileOffset : 0 );
	}
}

// everything playback holds at file scope; released on disconnect
void CL_DemoRelease( void ) {
	CL_DemoCacheFree();
	CL_DemoKeyframesFree();
	CL_DemoApiReset();
	DR_Free( &demoRaw );
}

void CL_DemoApiDetach( void ) {
	demoApiAttached = qfalse;
	Cvar_SetValue( "cl_demoTimeFrac", 0.0f );
	cl_demoFxTimeFrac = 0.0f;
}

qboolean CL_DemoApiSetClientTime( void ) {
	if ( !clc.demoplaying || !demoApiAttached ) {
		return qfalse;
	}
	if ( cl_timedemo && cl_timedemo->integer ) {
		if ( !clc.timeDemoStart ) {
			clc.timeDemoStart = Sys_Milliseconds();
		}
		clc.timeDemoFrames++;
	}
	// a timedemo or a demo chain finishes once the playhead reaches the last frame; here, outside
	// the game module, since finishing unloads it
	// a damaged file ends where its decode stopped, wherever that is
	if ( demoFwd && demoFwd->eof && demoRaw.numGs > 0
		&& ( ( CL_DemoLoadedGamestate() == demoRaw.numGs - 1 && demoApiPlayhead >= demoRaw.gs[demoRaw.numGs - 1].lastTime )
			|| ( demoFwd->faultCount > 3 && demoFwd->snap.valid && demoApiPlayhead >= demoFwd->snap.serverTime ) )
		&& ( ( cl_timedemo && cl_timedemo->integer ) || Cvar_VariableString( "nextdemo" )[0] ) ) {
		CL_DemoFinish();
		return qtrue;
	}
	cl.serverTime = demoApiPlayhead;
	cl.oldServerTime = cl.serverTime;
	cl.serverTimeDelta = cl.serverTime - cls.realtime;
	cl.oldFrameServerTime = cl.snap.serverTime;
	cl.newSnapshots = qfalse;
	return qtrue;
}

qboolean CL_DemoApiGetGameState( gameState_t *gs ) {
	if ( !clc.demoplaying || !demoPresentedValid || demoPresentedGamestate != CL_DemoLoadedGamestate() ) {
		return qfalse;
	}
	*gs = demoPresentedGs;
	return qtrue;
}

float cl_demoFxTimeFrac;

void CL_DemoApiSetTimeFraction( float g2Frac, float fxFrac ) {
	if ( !clc.demoplaying ) {
		g2Frac = fxFrac = 0.0f;	// never shifts a listen server's ghoul2
	}
	if ( !( g2Frac >= 0.0f && g2Frac < 1.0f ) ) {
		g2Frac = 0.0f;
	}
	if ( !( fxFrac >= 0.0f && fxFrac < 1.0f ) ) {
		fxFrac = 0.0f;
	}
	demoApiTimeFrac = g2Frac;
	cl_demoFxTimeFrac = fxFrac;
	Cvar_SetValue( "cl_demoTimeFrac", g2Frac );
}

int CL_DemoApiGetInfo( demoApiInfo_t *out ) {
	if ( !out ) {
		return 0;
	}
	Com_Memset( out, 0, sizeof( *out ) );
	if ( !clc.demoplaying ) {
		return 0;
	}

	out->indexDone = demoPrescanDone;
	out->firstServerTime = demoRaw.firstTime;
	out->lastServerTime = demoRaw.lastTime;
	out->numGamestates = demoRaw.numGs;
	out->fileBytes = demoCmdFileSize;
	out->loadedGamestate = CL_DemoLoadedGamestate();
	if ( demoApiResumeTarget >= 0 ) {
		out->resumeGamestate = demoApiResumeGamestate >= 0 ? demoApiResumeGamestate : out->loadedGamestate;
		out->resumeServerTime = demoApiResumeTarget;
	} else if ( demoApiPlayhead > 0 ) {
		out->resumeGamestate = out->loadedGamestate;
		out->resumeServerTime = demoApiPlayhead;
	} else {
		out->resumeGamestate = -1;
		out->resumeServerTime = ( demoFwd && demoFwd->snap.valid ) ? demoFwd->snap.serverTime : demoRaw.firstTime;
	}
	out->numLevelStarts = demoRaw.numLs;
	out->numEarlyTimes = demoRaw.numEarly;
	Q_strncpyz( out->name, clc.demoName, sizeof( out->name ) );
	Com_Memcpy( out->earlyTimes, demoRaw.early, demoRaw.numEarly * sizeof( int ) );
	Com_Memcpy( out->gamestates, demoRaw.gs, demoRaw.numGs * sizeof( demoApiGamestate_t ) );
	return 1;
}

int CL_DemoApiLevelStarts( int first, demoApiLevelStart_t *out, int max ) {
	int n;

	if ( !clc.demoplaying || !out || first < 0 || first >= demoRaw.numLs || max <= 0 ) {
		return 0;
	}
	n = demoRaw.numLs - first;
	if ( n > max ) {
		n = max;
	}
	Com_Memcpy( out, demoRaw.ls + first, n * sizeof( *out ) );
	return n;
}

int CL_DemoApiFindSnapshot( int serverTime, int mode ) {
	int idx;

	if ( mode == DEMOSNAP_KEYFRAME_AT_OR_BEFORE ) {
		int k;

		if ( !clc.demoplaying ) {
			return -1;
		}
		// strictly before: the point a decode toward serverTime starts from
		k = CL_DemoKeyframeBefore( CL_DemoLoadedGamestate(), serverTime );
		return k >= 0 ? demoKeyframes[k].serverTime : -1;
	}

	if ( !clc.demoplaying || !demoCacheSnaps || demoCacheCount < 1 ) {
		return -1;
	}
	idx = CL_DemoCacheFindAtOrBefore( serverTime );
	if ( mode == DEMOSNAP_AT_OR_BEFORE ) {
		return idx >= 0 ? CL_DemoCacheAt( idx )->snap.serverTime : -1;
	}
	idx++;
	return idx < demoCacheCount ? CL_DemoCacheAt( idx )->snap.serverTime : -1;
}

int CL_DemoApiGetSnapshot( int serverTime, snapshot_t *out ) {
	const demoCacheEntry_t	*e;
	int						idx, i, count;

	if ( !clc.demoplaying || !demoCacheSnaps || demoCacheCount < 1 || !out ) {
		return 0;
	}
	idx = CL_DemoCacheFindAtOrBefore( serverTime );
	if ( idx < 0 ) {
		return 0;
	}
	e = CL_DemoCacheAt( idx );
	if ( e->snap.serverTime != serverTime ) {
		return 0;
	}

	out->snapFlags = e->snap.snapFlags;
	out->serverCommandSequence = e->snap.serverCommandNum;
	out->ping = e->snap.ping;
	out->serverTime = e->snap.serverTime;
	Com_Memcpy( out->areamask, e->snap.areamask, sizeof( out->areamask ) );
	out->ps = e->snap.ps;
	out->vps = e->snap.vps;
	count = e->snap.numEntities;
	if ( count > MAX_ENTITIES_IN_SNAPSHOT ) {
		count = MAX_ENTITIES_IN_SNAPSHOT;
	}
	out->numEntities = count;
	for ( i = 0; i < count; i++ ) {
		out->entities[i] = demoCacheEnts[ ( e->firstEnt + i ) % demoCacheMaxEnts ];
	}
	return 1;
}

int CL_DemoApiCommands( demoApiCmdCursor_t *c ) {
	int loaded = CL_DemoLoadedGamestate();

	if ( !clc.demoplaying || !c ) {
		return 0;
	}
	// the cursor comes from the game module
	c->prefix[DEMOAPI_PREFIX_LEN - 1] = '\0';
	if ( c->cursor < 0 || c->fromTime > c->toTime ) {
		return 0;
	}
	if ( c->state && c->cursor > ( c->mode == DEMOCMD_RESYNC ? demoApiDiffCount
		: c->mode == DEMOCMD_BASE ? MAX_CONFIGSTRINGS : demoCmdCount ) ) {
		return 0;
	}
	if ( ( c->mode == DEMOCMD_SCAN || c->mode == DEMOCMD_BASE ) && c->gamestate >= demoRaw.numGs ) {
		return 0;
	}

	switch ( c->mode ) {
	case DEMOCMD_FORWARD:
		if ( !c->state ) {
			int end;

			c->cursor = CL_DemoCmdTimeSearch( loaded, c->fromTime, qfalse, &end );
			c->state = 1;
		}
		while ( c->cursor < demoCmdCount ) {
			const demoCmdEntry_t *e = &demoCmdLog[c->cursor];

			if ( e->gamestate != loaded || e->serverTime > c->toTime ) {
				break;
			}
			c->cursor++;
			if ( e->csIndex >= 0 ) {
				CL_DemoPresentAdvanceThrough( loaded, c->cursor - 1 );
			}
			Cmd_TokenizeString( CL_DemoCmdText( e ) );
			c->cmdTime = e->serverTime;
			return 1;
		}
		CL_DemoPresentAdvance( loaded, c->toTime );
		return 0;

	case DEMOCMD_RESYNC:
		if ( !c->state ) {
			gameState_t	before;
			int			i;
			qboolean	moved = qfalse;

			// diff against the table the game module holds
			if ( demoPresentedValid && demoPresentedGamestate == loaded ) {
				if ( demoPresentedPartial >= 0 ) {
					// mid-delivery at one time: rebuild, and the diff below carries the rest
				} else if ( c->toTime == demoPresentedTime ) {
					demoApiDiffCount = 0;
					moved = qtrue;
				} else if ( CL_DemoPresentMove( loaded, c->toTime ) ) {
					moved = qtrue;
				}
				before = demoPresentedGs;
			} else if ( loaded < DEMOAPI_MAX_GAMESTATES && demoSegBaseGs[loaded] ) {
				before = *demoSegBaseGs[loaded];
			} else {
				before = cl.gameState;
			}
			if ( !moved ) {
				CL_DemoPresentRebuild( loaded, c->toTime );

				demoApiDiffCount = 0;
				for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
					const char *a = before.stringData + before.stringOffsets[i];
					const char *b = demoPresentedGs.stringData + demoPresentedGs.stringOffsets[i];

					if ( strcmp( a, b ) ) {
						demoApiDiff[demoApiDiffCount++] = i;
					}
				}
			}
			c->cursor = 0;
			c->state = 1;
		}
		if ( c->cursor < demoApiDiffCount ) {
			Cmd_TokenizeString( va( "cs %d", demoApiDiff[c->cursor++] ) );
			c->cmdTime = c->toTime;
			return 1;
		}
		return 0;

	case DEMOCMD_BASE:
		if ( c->gamestate < 0 ) {
			c->gamestate = loaded;
		}
		if ( c->gamestate >= DEMOAPI_MAX_GAMESTATES || !demoSegBaseGs[c->gamestate] ) {
			return 0;
		}
		while ( c->cursor < MAX_CONFIGSTRINGS ) {
			const gameState_t	*gs = demoSegBaseGs[c->gamestate];
			int					i = c->cursor++;

			if ( gs->stringOffsets[i] && gs->stringData[gs->stringOffsets[i]] ) {
				Cmd_TokenizeString( va( "cs %d \"%s\"", i, gs->stringData + gs->stringOffsets[i] ) );
				c->cmdTime = 0;
				return 1;
			}
		}
		return 0;

	case DEMOCMD_SCAN:
		if ( !c->state ) {
			int end;

			if ( c->gamestate < 0 ) {
				c->gamestate = loaded;
			}
			c->cursor = CL_DemoCmdTimeSearch( c->gamestate, c->fromTime, qtrue, &end );
			c->state = 1;
		}
		while ( c->cursor < demoCmdCount ) {
			const demoCmdEntry_t	*e = &demoCmdLog[c->cursor];
			const char				*text;

			if ( e->gamestate != c->gamestate || e->serverTime > c->toTime ) {
				break;
			}
			c->cursor++;
			text = CL_DemoCmdText( e );
			if ( c->prefix[0] && Q_strncmp( text, c->prefix, (int)strlen( c->prefix ) ) ) {
				continue;
			}
			Cmd_TokenizeString( text );
			c->cmdTime = e->serverTime;
			return 1;
		}
		return 0;
	}
	return 0;
}

static int CL_DemoCacheRunTopTime( int atTime ) {
	int idx = CL_DemoCacheFindAtOrBefore( atTime );

	if ( idx < 0 ) {
		return -1;
	}
	while ( idx + 1 < demoCacheCount ) {
		int here = CL_DemoCacheAt( idx )->snap.serverTime;
		int above = CL_DemoCacheAt( idx + 1 )->snap.serverTime;

		if ( above - here > DEMOCACHE_MAX_REVERSE_GAP_MSEC ) {
			break;
		}
		idx++;
	}
	return CL_DemoCacheAt( idx )->snap.serverTime;
}

// the newest keyframe in the loaded gamestate before target, if it is ahead of the decoder
static qboolean CL_DemoKeyframeBeatsDecoder( int target, int decoderTime ) {
	int k = CL_DemoKeyframeBefore( CL_DemoLoadedGamestate(), target );

	return (qboolean)( k >= 0 && demoKeyframes[k].serverTime > decoderTime );
}

static qboolean CL_DemoCacheCovers( int playhead ) {
	int idx;

	if ( demoCacheCount <= 0 ) {
		return qfalse;
	}
	idx = CL_DemoCacheFindAtOrBefore( playhead );
	if ( idx < 0 ) {
		return qfalse;
	}
	if ( idx < demoCacheCount - 1 ) {
		return qtrue;
	}
	return (qboolean)( playhead - CL_DemoCacheAt( idx )->snap.serverTime <= DEMOCACHE_MAX_REVERSE_GAP_MSEC );
}

int CL_DemoApiEnsure( demoApiEnsure_t *e ) {
	int							began, budgetMsec, playhead, loaded, gs;
	int							runFloor, runTop;
	qboolean					covered;
	const demoApiGamestate_t	*g;

	if ( !e || !clc.demoplaying || !demoFwd || !demoCacheSnaps || !demoRaw.numGs ) {
		return 0;
	}

	if ( !demoApiAttached ) {
		demoApiAttached = qtrue;
		demoApiServiceDemo = qtrue;
		demoApiResumeTarget = -1;
		demoApiResumeGamestate = -1;
		if ( cls.state == CA_PRIMED ) {
			CL_BecomeActive();
		}
	}
	loaded = CL_DemoLoadedGamestate();
	g = &demoRaw.gs[loaded];
	playhead = e->playheadMs;
	demoApiPlayhead = playhead;
	e->flags = 0;
	e->levelChangeGamestate = -1;
	e->levelChangeTarget = 0;
	e->cacheFrom = e->cacheTo = -1;

	gs = e->gamestate;
	if ( gs >= 0 && gs < demoRaw.numGs && gs != loaded ) {
		e->flags |= DEMOENSURE_LEVEL_CHANGE;
		e->levelChangeGamestate = gs;
		e->levelChangeTarget = playhead < demoRaw.gs[gs].settleTime ? demoRaw.gs[gs].settleTime : playhead;
		e->decoderTime = demoFwd->snap.valid ? demoFwd->snap.serverTime : -1;
		return 1;
	}

	budgetMsec = e->budgetUsec > 0 ? ( e->budgetUsec + 999 ) / 1000 : 0;
	began = Sys_Milliseconds();

	covered = CL_DemoCacheCovers( playhead );
	runFloor = covered ? CL_DemoCacheRunFloorTime( playhead ) : -1;
	runTop = covered ? CL_DemoCacheRunTopTime( playhead ) : -1;

	// just below the cache floor is still a refill, not a reposition
	if ( !covered && demoCacheCount > 0 ) {
		int oldest = CL_DemoCacheAt( 0 )->snap.serverTime;

		if ( playhead < oldest && oldest - playhead < DEMOCACHE_MAX_REVERSE_GAP_MSEC * 3 ) {
			covered = qtrue;
			runFloor = CL_DemoCacheRunFloorTime( oldest );
			runTop = CL_DemoCacheRunTopTime( oldest );
		}
	}

	// a request outside the cache (a seek) drops a backward refill; the forward decoder never left
	if ( demoRefillActive && !covered ) {
		CL_DemoRefillAbandon();
	}

	// above, or nowhere: drive the forward decoder. a running refill has a decoder of its own, so this
	// never waits behind it
	if ( !covered || e->wantTo > runTop ) {
		int			decoderTime = demoFwd->snap.valid ? demoFwd->snap.serverTime : -1;
		int			guard = 0;
		qboolean	trimmed = (qboolean)( demoCacheTopTrimmed && covered && decoderTime > runTop );

		demoCacheTopTrimmed = qfalse;
		if ( demoFwd->eof && covered && runTop >= 0 && decoderTime >= runTop && !trimmed ) {
			// nothing above exists
		} else {
			int aim = covered ? runTop : e->wantFrom;

			if ( aim < g->settleTime ) {
				aim = g->settleTime;
			}
			if ( decoderTime > e->wantTo || decoderTime < 0 || demoFwd->eof || trimmed
				|| demoFwd->gamestate != loaded || CL_DemoKeyframeBeatsDecoder( aim, decoderTime ) ) {
				Demo_Reposition( demoFwd, loaded, aim );
				CL_DemoCacheBreakIfAbove( demoFwd );
			}

			demoCacheExtendOnly = covered;
			while ( !demoFwd->eof ) {
				if ( demoFwd->snap.valid && !demoFwd->kfRestoring && demoFwd->snap.serverTime >= e->wantTo
					&& demoFwd->recordOffset >= g->settleOffset ) {
					break;
				}
				if ( ++guard > MAX_DEMO_TIME_MESSAGES ) {
					break;
				}
				if ( Demo_NextRecordLeavesSegment( demoFwd ) ) {
					e->flags |= DEMOENSURE_SEGMENT_END;
					break;
				}
				if ( !Demo_ReadRecord( demoFwd ) ) {
					if ( demoFwd->fault ) {
						// a corrupt record: reposition once; a repeat at the same place ends the demo here
						if ( demoFwd->faultCount > 3 ) {
							Com_Printf( S_COLOR_YELLOW "Demo file damaged at offset %d; playback ends there.\n", demoFwd->faultOffset );
							demoFwd->fault = qfalse;
							demoFwd->eof = qtrue;
							break;
						}
						Demo_Reposition( demoFwd, loaded, aim );
						CL_DemoCacheBreakIfAbove( demoFwd );
						continue;
					}
					break;
				}
				if ( demoFwd->kfRestoreFailed ) {
					// the keyframe is quarantined; the next older one, or the map's own record
					demoFwd->kfRestoreFailed = qfalse;
					Demo_Reposition( demoFwd, loaded, aim );
					CL_DemoCacheBreakIfAbove( demoFwd );
					continue;
				}
				if ( budgetMsec > 0 && Sys_Milliseconds() - began >= budgetMsec ) {
					break;
				}
			}
			demoCacheExtendOnly = qfalse;
		}

		covered = CL_DemoCacheCovers( playhead );
		runFloor = covered ? CL_DemoCacheRunFloorTime( playhead ) : -1;
		runTop = covered ? CL_DemoCacheRunTopTime( playhead ) : -1;
	}

	// below: the staged refill, on what is left of the budget. one that is running is stepped to
	// the end even if the request has moved back above the floor
	if ( covered && ( demoRefillActive || ( e->wantFrom < runFloor && runFloor > g->settleTime
		&& !( demoCacheRefillExhausted && runFloor == demoCacheExhaustedFloor ) ) ) ) {
		int left = 0;

		if ( budgetMsec > 0 ) {
			left = budgetMsec - ( Sys_Milliseconds() - began );
			if ( left < 1 ) {
				left = 1;
			}
		}
		if ( !demoRefillActive && CL_DemoCacheRoomBelow( e->wantTo ) ) {
			CL_DemoRefillBegin( runFloor );
		}
		if ( demoRefillActive ) {
			// the playhead below the live run waits on the staged material: splice it in at once
			CL_DemoRefillStep( left, e->wantTo, (qboolean)( playhead < runFloor ) );
			if ( demoRefillActive ) {
				e->flags |= DEMOENSURE_BUSY;
			}
			covered = CL_DemoCacheCovers( playhead );
			runFloor = covered ? CL_DemoCacheRunFloorTime( playhead ) : -1;
			runTop = covered ? CL_DemoCacheRunTopTime( playhead ) : -1;
		}
	}

	// what is left of the budget goes to the index pass
	if ( !demoPrescanDone && budgetMsec > 0 ) {
		int left = budgetMsec - ( Sys_Milliseconds() - began );

		if ( left > 0 ) {
			CL_DemoPrescanStep( left );
		}
	}

	if ( covered ) {
		e->cacheFrom = runFloor;
		e->cacheTo = runTop;
	} else if ( demoCacheCount > 0 ) {
		int at = CL_DemoCacheAt( 0 )->snap.serverTime;

		e->cacheFrom = CL_DemoCacheRunFloorTime( at );
		e->cacheTo = CL_DemoCacheRunTopTime( at );
	}
	e->decoderTime = demoFwd->snap.valid ? demoFwd->snap.serverTime : -1;
	if ( demoFwd->eof ) {
		e->flags |= DEMOENSURE_EOF;
	}
	if ( demoCacheRefillExhausted && e->cacheFrom == demoCacheExhaustedFloor ) {
		e->flags |= DEMOENSURE_AT_START;
	}
	if ( e->cacheFrom >= 0 && e->cacheFrom <= g->settleTime ) {
		e->flags |= DEMOENSURE_AT_START;
	}
	return 1;
}

float	cl_demoFrameMsec;
int		cl_demoFrameNum;
int		cl_demoFrameFlags;

int CL_DemoApiFrameClock( demoApiClock_t *out ) {
	if ( !out ) {
		return 0;
	}
	out->frameMsec = cl_demoFrameMsec;
	out->frameNum = cl_demoFrameNum;
	out->flags = cl_demoFrameFlags;
	return 1;
}
