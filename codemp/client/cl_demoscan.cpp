// cl_demoscan.cpp -- reads demo files that are not playing, on a worker thread, for the ui module.
// The interface and what it is for are described in cgame/cg_demoscan.h.

#include <sys/stat.h>
#include "client.h"
#include "cgame/cg_demoscan.h"
#include "cl_demoraw.h"

#include <SDL_thread.h>
#include <SDL_mutex.h>
#include <SDL_atomic.h>
#include <SDL_timer.h>

#define DS_HASH_BYTES			65536
#define DS_MAX_RECORD_CMDS		MAX_RELIABLE_COMMANDS	// more than this cannot be pending in one message
#define DS_EXEMPT_RESERVE		( 64 * 1024 )			// room kept under the hard cap for the structural records
#define DS_MAX_FIELDS			256

#define DS_INTEREST_LOG			0x1
#define DS_INTEREST_TRACK		0x2
#define DS_INTEREST_TRIGGER		0x4

// the scan in cl_demo.cpp defines these for the demo being played; the two must agree on where a
// demo's segments fall, so the rules and their constants are the same

typedef enum {
	DSJ_QUEUED,
	DSJ_RUNNING,
	DSJ_DONE
} dsJobState_t;

typedef struct dsJob_s {
	struct dsJob_s			*next;
	int						id;
	dsJobState_t			state;
	int						priority;
	qboolean				reported;		// POLL has handed its header out
	qboolean				cancelled;		// running when cancelled: the worker frees it
	qboolean				requeue;		// running when something more urgent arrived: the worker puts it back
	SDL_atomic_t			abort;
	char					*osPath;
	demoScanRequest_t		req;
	demoScanResultHeader_t	hdr;
	byte					*blob;
} dsJob_t;

static SDL_mutex	*dsMutex;
static SDL_cond		*dsCond;
static SDL_Thread	*dsThread;
static SDL_atomic_t	dsQuit;
static SDL_atomic_t	dsPaused;				// DEMOSCAN_OP_PAUSE: the worker waits between records
static dsJob_t		*dsJobs;				// every live job, oldest first
static dsJob_t		*dsRunning;
static dsJob_t		*dsSuspendedJob;		// pre-empted with its scan state kept, to go on where it stopped
static struct dsScan_s	*dsSuspendedScan;
static int			dsNextId = 1;			// never reused within a process, so a restarted ui cannot fetch a stale job
static qboolean		dsReady;

// fixed before the worker exists and never written again
static const node_t	*dsHuffTree;
static int			dsFieldBits[DS_MAX_FIELDS];
static int			dsNumFields;
static int			dsFloatIntBits;

// DEEP: the few entity fields followed by value, and the three player state tables
enum { DST_ETYPE, DST_EVENTPARM, DST_OTHER, DST_OTHER2, DST_USER1, DST_USER2, DST_USER3, DST_NUM };
static int			dsTrackedSlot[DS_MAX_FIELDS];	// entity field index -> DST_*, or -1
static int			dsPsBits[3][DS_MAX_FIELDS];
static int			dsPsNumFields[3];
static int			dsPsVehicleField[3];			// where m_iVehicleNum sits in each table
static qboolean		dsDeepReady;

// the main thread's copy, taken again when a mod's field overrides load; the worker adopts it between jobs
typedef struct {
	int			fieldBits[DS_MAX_FIELDS];
	int			numFields;
	int			floatIntBits;
	int			trackedSlot[DS_MAX_FIELDS];
	int			psBits[3][DS_MAX_FIELDS];
	int			psNumFields[3];
	int			psVehicleField[3];
	qboolean	deepReady;
} dsLayout_t;
static dsLayout_t	dsStaged;
static int			dsStagedGen, dsAppliedGen;
static void			DS_ApplyLayout( void );

typedef struct dsBits_s {
	const byte	*data;
	int			bit;
	int			totalBits;
	qboolean	overflow;
} dsBits_t;

static int DS_GetBit( dsBits_t *b ) {
	int v;

	if ( b->bit >= b->totalBits ) {
		b->overflow = qtrue;
		return 0;
	}
	v = ( b->data[b->bit >> 3] >> ( b->bit & 7 ) ) & 1;
	b->bit++;
	return v;
}

static int DS_ReadSymbol( dsBits_t *b ) {
	const node_t *node = dsHuffTree;

	while ( node && node->symbol == INTERNAL_NODE ) {
		node = DS_GetBit( b ) ? node->right : node->left;
		if ( b->overflow ) {
			return 0;
		}
	}
	return node ? node->symbol : 0;
}

// MSG_ReadBits for a compressed message: the odd low bits raw, the rest as Huffman coded bytes
static int DS_ReadBits( dsBits_t *b, int bits ) {
	int			value = 0;
	int			i, nbits = 0;
	qboolean	sgn = qfalse;

	if ( bits < 0 ) {
		bits = -bits;
		sgn = qtrue;
	}
	if ( bits & 7 ) {
		nbits = bits & 7;
		for ( i = 0; i < nbits; i++ ) {
			value |= DS_GetBit( b ) << i;
		}
		bits -= nbits;
	}
	for ( i = 0; i < bits; i += 8 ) {
		value |= DS_ReadSymbol( b ) << ( i + nbits );
	}
	if ( sgn && bits > 0 && bits < 32 ) {
		if ( value & ( 1 << ( bits - 1 ) ) ) {
			value |= -1 ^ ( ( 1 << bits ) - 1 );
		}
	}
	return value;
}

static int DS_ReadByte( dsBits_t *b ) {
	int c = (unsigned char)DS_ReadBits( b, 8 );

	return b->overflow ? -1 : c;
}

static int DS_ReadShort( dsBits_t *b ) {
	int c = (short)DS_ReadBits( b, 16 );

	return b->overflow ? -1 : c;
}

static int DS_ReadLong( dsBits_t *b ) {
	int c = DS_ReadBits( b, 32 );

	return b->overflow ? -1 : c;
}

static void DS_ReadString( dsBits_t *b, char *out, int outSize, int consumeMax ) {
	int l = 0;

	while ( l < consumeMax ) {
		int c = DS_ReadByte( b );

		if ( c == -1 || c == 0 ) {
			break;
		}
		if ( c == '%' ) {
			c = '.';
		}
		if ( l < outSize - 1 ) {
			out[l] = (char)c;
		}
		l++;
	}
	out[l < outSize - 1 ? l : outSize - 1] = '\0';
}

// steps over what MSG_ReadDeltaEntity would read from a null state, which is what a baseline is
static void DS_SkipBaselineEntity( dsBits_t *b ) {
	int i, lc;

	if ( DS_ReadBits( b, 1 ) == 1 ) {
		return;							// remove
	}
	if ( DS_ReadBits( b, 1 ) == 0 ) {
		return;							// no delta
	}
	lc = DS_ReadByte( b );
	if ( lc < 0 || lc > dsNumFields ) {
		b->overflow = qtrue;
		return;
	}
	for ( i = 0; i < lc && !b->overflow; i++ ) {
		if ( !DS_ReadBits( b, 1 ) ) {
			continue;					// unchanged
		}
		if ( dsFieldBits[i] == 0 ) {
			if ( DS_ReadBits( b, 1 ) == 0 ) {
				continue;				// float zero
			}
			if ( DS_ReadBits( b, 1 ) == 0 ) {
				DS_ReadBits( b, dsFloatIntBits );
			} else {
				DS_ReadBits( b, 32 );
			}
		} else {
			if ( DS_ReadBits( b, 1 ) == 0 ) {
				continue;				// integer zero
			}
			DS_ReadBits( b, dsFieldBits[i] < 0 ? -dsFieldBits[i] : dsFieldBits[i] );
		}
	}
}

typedef struct dsEnt_s {
	int		number;
	int		v[DST_NUM];
} dsEnt_t;

typedef struct dsFrame_s {
	qboolean	valid;
	int			messageNum;
	int			vehicleNum;
	int			numEnts;
	dsEnt_t		ents[MAX_SNAPSHOT_ENTITIES];
} dsFrame_t;

typedef struct dsDeep_s {
	dsFrame_t	frames[PACKET_BACKUP];
	dsEnt_t		baselines[MAX_GENTITIES];
	int			eventMessage[MAX_GENTITIES];	// the last snapshot each entity number was an event in,
	int			eventType[MAX_GENTITIES];		// and which: an event entity lives several snapshots and is ONE event
	int			lastMessage;					// the snapshot before this one
} dsDeep_t;

// MSG_ReadDeltaEntity, keeping only the tracked fields
static void DS_ReadEntity( dsBits_t *b, const dsEnt_t *from, dsEnt_t *to, int number ) {
	int i, lc;

	if ( DS_ReadBits( b, 1 ) == 1 ) {
		memset( to, 0, sizeof( *to ) );
		to->number = MAX_GENTITIES - 1;		// removed
		return;
	}
	*to = *from;
	to->number = number;
	if ( DS_ReadBits( b, 1 ) == 0 ) {
		return;								// no delta
	}
	lc = DS_ReadByte( b );
	if ( lc < 0 || lc > dsNumFields ) {
		b->overflow = qtrue;
		return;
	}
	for ( i = 0; i < lc && !b->overflow; i++ ) {
		int value = 0;

		if ( !DS_ReadBits( b, 1 ) ) {
			continue;
		}
		if ( dsFieldBits[i] == 0 ) {
			if ( DS_ReadBits( b, 1 ) != 0 ) {
				if ( DS_ReadBits( b, 1 ) == 0 ) {
					DS_ReadBits( b, dsFloatIntBits );
				} else {
					DS_ReadBits( b, 32 );
				}
			}
			continue;						// no float is tracked
		}
		if ( DS_ReadBits( b, 1 ) != 0 ) {
			value = DS_ReadBits( b, dsFieldBits[i] );
		}
		if ( dsTrackedSlot[i] >= 0 ) {
			to->v[dsTrackedSlot[i]] = value;
		}
	}
}

// MSG_ReadDeltaPlayerstate, keeping only m_iVehicleNum. Returns it.
static int DS_ReadPlayerstate( dsBits_t *b, int fromVehicleNum, qboolean isVehiclePS ) {
	int i, lc, bits, table = 0, vehicleNum = fromVehicleNum;

#ifdef _OPTIMIZED_VEHICLE_NETWORKING
	if ( isVehiclePS ) {
		table = 2;
	} else if ( DS_ReadBits( b, 1 ) ) {
		table = 1;							// a pilot inside a vehicle
	}
#endif

	lc = DS_ReadByte( b );
	if ( lc < 0 || lc > dsPsNumFields[table] ) {
		b->overflow = qtrue;
		return vehicleNum;
	}
	for ( i = 0; i < lc && !b->overflow; i++ ) {
		if ( !DS_ReadBits( b, 1 ) ) {
			continue;
		}
		if ( dsPsBits[table][i] == 0 ) {
			if ( DS_ReadBits( b, 1 ) == 0 ) {
				DS_ReadBits( b, dsFloatIntBits );
			} else {
				DS_ReadBits( b, 32 );
			}
		} else {
			int value = DS_ReadBits( b, dsPsBits[table][i] );

			if ( i == dsPsVehicleField[table] ) {
				vehicleNum = value;
			}
		}
	}

	if ( DS_ReadBits( b, 1 ) ) {
		if ( DS_ReadBits( b, 1 ) ) {
			bits = DS_ReadBits( b, MAX_STATS );
			for ( i = 0; i < MAX_STATS; i++ ) {
				if ( bits & ( 1 << i ) ) {
					DS_ReadBits( b, i == STAT_WEAPONS ? MAX_WEAPONS : 16 );
				}
			}
		}
		if ( DS_ReadBits( b, 1 ) ) {
			bits = DS_ReadBits( b, MAX_PERSISTANT );
			for ( i = 0; i < MAX_PERSISTANT; i++ ) {
				if ( bits & ( 1 << i ) ) {
					DS_ReadBits( b, 16 );
				}
			}
		}
		if ( DS_ReadBits( b, 1 ) ) {
			bits = DS_ReadBits( b, MAX_AMMO_TRANSMIT );
			for ( i = 0; i < MAX_AMMO_TRANSMIT; i++ ) {
				if ( bits & ( 1 << i ) ) {
					DS_ReadBits( b, 16 );
				}
			}
		}
		if ( DS_ReadBits( b, 1 ) ) {
			bits = DS_ReadBits( b, MAX_POWERUPS );
			for ( i = 0; i < MAX_POWERUPS; i++ ) {
				if ( bits & ( 1 << i ) ) {
					DS_ReadBits( b, 32 );
				}
			}
		}
	}
	return vehicleNum;
}

typedef struct dsScan_s {
	const demoScanRequest_t	*req;
	SDL_atomic_t			*abort;
	demoScanResultHeader_t	*hdr;

	byte			*blob;
	int				blobLen, blobCap, blobMax;

	byte			interest[MAX_CONFIGSTRINGS];
	char			*cs[MAX_CONFIGSTRINGS];		// current value of every configstring of interest, NULL = empty

	demoRaw_t		*raw;
	int				exempt;						// structural records: only the hard cap applies
	int				exemptDropped;
	long			resumeOffset;				// where a pre-empted scan stopped, -1 when it cannot go on from there
	qboolean		resumeHaveFirst;
	int				numCaptures;

	dsDeep_t		*deep;						// DEEP scans only
	dsFrame_t		*building;					// the snapshot being read, which may be delta'd from the slot it will land in

	char			*latest[DEMOSCAN_MAX_PREFIXES];
	int				latestTime[DEMOSCAN_MAX_PREFIXES];
	int				latestGs[DEMOSCAN_MAX_PREFIXES];
	int				latestLs[DEMOSCAN_MAX_PREFIXES];

	int				lastCmdSeq;
	qboolean		haveCmdSeq;
	int				lastTime;

	int				numCmds;
	char			cmds[DS_MAX_RECORD_CMDS][MAX_STRING_CHARS];
	int				cmdSeqs[DS_MAX_RECORD_CMDS];
	char			bigCs[BIG_INFO_STRING];
	char			bigValue[BIG_INFO_STRING];
} dsScan_t;

static void DS_EmitUnder( dsScan_t *s, int type, int index, int serverTime, int gamestate, int lsIndex, const void *payload, int len ) {
	demoScanRecord_t	rec;
	int					size = DEMOSCAN_RECORD_SIZE( len );

	if ( s->blobLen + size > ( s->exempt ? DEMOSCAN_MAX_BLOB : s->blobMax ) ) {
		s->hdr->flags |= DEMOSCAN_FLAG_TRUNCATED;
		if ( s->exempt ) {
			s->exemptDropped = 1;	// without its structure the result cannot be used
		}
		return;
	}
	if ( s->blobLen + size > s->blobCap ) {
		int		cap = s->blobCap ? s->blobCap : 16384;
		byte	*grown;

		while ( cap < s->blobLen + size ) {
			cap *= 2;
		}
		grown = (byte *)realloc( s->blob, cap );
		if ( !grown ) {
			s->hdr->flags |= DEMOSCAN_FLAG_TRUNCATED;
			return;
		}
		s->blob = grown;
		s->blobCap = cap;
	}

	rec.type = type;
	rec.index = index;
	rec.serverTime = serverTime;
	rec.gamestate = gamestate;
	rec.lsIndex = lsIndex;
	rec.len = len;
	memcpy( s->blob + s->blobLen, &rec, sizeof( rec ) );
	memset( s->blob + s->blobLen + sizeof( rec ), 0, size - sizeof( rec ) );
	memcpy( s->blob + s->blobLen + sizeof( rec ), payload, len );
	s->blobLen += size;
	s->hdr->numRecords++;
}

static int DS_CurGs( const dsScan_t *s ) {
	return s->raw->numGs > 0 ? s->raw->numGs - 1 : 0;
}

static int DS_CurLs( const dsScan_t *s ) {
	return s->raw->numGs > 0 ? s->raw->gs[s->raw->numGs - 1].numLevelStarts : 0;
}

static void DS_Emit( dsScan_t *s, int type, int index, int serverTime, const void *payload, int len ) {
	DS_EmitUnder( s, type, index, serverTime, DS_CurGs( s ), DS_CurLs( s ), payload, len );
}

static void DS_SetCs( dsScan_t *s, int index, const char *value ) {
	free( s->cs[index] );
	s->cs[index] = NULL;
	if ( value[0] ) {
		size_t n = strlen( value ) + 1;

		s->cs[index] = (char *)malloc( n );
		if ( s->cs[index] ) {
			memcpy( s->cs[index], value, n );
		}
	}
}

static void DS_EmitTracked( dsScan_t *s, int type, int serverTime ) {
	int i;

	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		if ( ( s->interest[i] & DS_INTEREST_TRACK ) && s->cs[i] ) {
			DS_Emit( s, type, i, serverTime, s->cs[i], (int)strlen( s->cs[i] ) );
		}
	}
}

// CL_ParseSnapshot from the byte after serverTime on
static void DS_Snapshot( dsScan_t *s, dsBits_t *b, int messageNum, int serverTime ) {
	dsDeep_t	*deep = s->deep;
	dsFrame_t	*frame = s->building;
	dsFrame_t	*old = NULL;
	int			deltaNum, deltaMessage, len, i;
	int			oldindex = 0, oldnum = 99999, next;

	memset( frame, 0, sizeof( int ) * 4 );
	frame->messageNum = messageNum;

	deltaNum = DS_ReadByte( b );
	DS_ReadByte( b );						// snapFlags
	if ( deltaNum <= 0 || messageNum - deltaNum <= 0 ) {
		frame->valid = qtrue;
	} else {
		deltaMessage = messageNum - deltaNum;
		old = &deep->frames[deltaMessage & PACKET_MASK];
		if ( !old->valid ) {
			while ( ( deltaMessage & PACKET_MASK ) != ( messageNum & PACKET_MASK ) && !old->valid ) {
				deltaMessage++;
				old = &deep->frames[deltaMessage & PACKET_MASK];
			}
		}
		frame->valid = (qboolean)( old->valid && old->messageNum == deltaMessage );
	}

	len = DS_ReadByte( b );					// areamask
	if ( len < 0 || len > 32 ) {
		b->overflow = qtrue;	// counted and stored invalid below, so later deltas from it fail too
		len = 0;
	}
	for ( i = 0; i < len; i++ ) {
		DS_ReadByte( b );
	}

	frame->vehicleNum = DS_ReadPlayerstate( b, old ? old->vehicleNum : 0, qfalse );
	if ( frame->vehicleNum ) {
		DS_ReadPlayerstate( b, 0, qtrue );
	}

	// CL_ParsePacketEntities
	if ( old && old->numEnts > 0 ) {
		oldnum = old->ents[0].number;
	}
#define DS_NEXT_OLD()	( oldindex++, oldnum = ( oldindex >= old->numEnts ) ? 99999 : old->ents[oldindex].number )
#define DS_KEEP( ent )	do { if ( frame->numEnts < MAX_SNAPSHOT_ENTITIES ) { frame->ents[frame->numEnts++] = ( ent ); } } while ( 0 )
	while ( !b->overflow ) {
		int		newnum = DS_ReadBits( b, GENTITYNUM_BITS );
		dsEnt_t	ent;

		if ( newnum == MAX_GENTITIES - 1 || b->overflow ) {
			break;
		}
		while ( oldnum < newnum ) {
			DS_KEEP( old->ents[oldindex] );	// unchanged from the old frame
			DS_NEXT_OLD();
		}
		if ( oldnum == newnum ) {
			DS_ReadEntity( b, &old->ents[oldindex], &ent, newnum );
			DS_NEXT_OLD();
		} else {
			DS_ReadEntity( b, &deep->baselines[newnum], &ent, newnum );
		}
		if ( ent.number != MAX_GENTITIES - 1 ) {
			DS_KEEP( ent );
		}
	}
	while ( oldnum != 99999 && !b->overflow ) {
		DS_KEEP( old->ents[oldindex] );
		DS_NEXT_OLD();
	}
#undef DS_NEXT_OLD
#undef DS_KEEP

	// a snapshot is the last thing in its message: anything else after it means the walk went wrong
	next = DS_ReadByte( b );
	s->hdr->numSnapshots++;
	if ( b->overflow || next != svc_EOF ) {
		s->hdr->numBadSnapshots++;
		frame->valid = qfalse;
	} else if ( !frame->valid ) {
		s->hdr->numBadSnapshots++;	// a delta from a frame that never decoded
	}

	if ( frame->valid ) {
		for ( i = 0; i < frame->numEnts; i++ ) {
			const dsEnt_t	*ent = &frame->ents[i];
			int				eType = ent->v[DST_ETYPE], event, e;
			qboolean		fresh;

			if ( eType <= ET_EVENTS || ent->number < 0 || ent->number >= MAX_GENTITIES ) {
				continue;
			}
			fresh = (qboolean)!( deep->eventMessage[ent->number] == deep->lastMessage && deep->eventType[ent->number] == eType );
			deep->eventMessage[ent->number] = messageNum;
			deep->eventType[ent->number] = eType;
			if ( !fresh ) {
				continue;
			}
			event = ( eType - ET_EVENTS ) & ~EV_EVENT_BITS;
			for ( e = 0; e < s->req->numEvents; e++ ) {
				if ( s->req->events[e] == event ) {
					demoScanEvent_t out;

					if ( ( s->req->flags & DEMOSCAN_REQ_EVENT_ATTACKER ) && ent->v[DST_OTHER2] != s->req->eventAttacker ) {
						break;
					}

					out.entityNum = ent->number;
					out.eventParm = ent->v[DST_EVENTPARM];
					out.otherEntityNum = ent->v[DST_OTHER];
					out.otherEntityNum2 = ent->v[DST_OTHER2];
					out.userInt1 = ent->v[DST_USER1];
					out.userInt2 = ent->v[DST_USER2];
					out.userInt3 = ent->v[DST_USER3];
					DS_Emit( s, DSR_EVENT, event, serverTime, &out, (int)sizeof( out ) );
					break;
				}
			}
		}
		deep->lastMessage = messageNum;
	}

	deep->frames[messageNum & PACKET_MASK] = *frame;
}

static void DS_ConfigstringChanged( dsScan_t *s, int index, const char *value, int serverTime ) {
	int flags = s->interest[index];

	if ( !flags ) {
		return;
	}
	if ( !strcmp( s->cs[index] ? s->cs[index] : "", value ) ) {
		return;							// re-sent with the value it already had
	}
	DS_SetCs( s, index, value );

	if ( flags & DS_INTEREST_LOG ) {
		DS_Emit( s, DSR_CS_UPDATE, index, serverTime, value, (int)strlen( value ) );
	}
	if ( ( flags & DS_INTEREST_TRIGGER ) && s->numCaptures < DEMOSCAN_MAX_CAPTURES ) {
		s->numCaptures++;
		DS_EmitTracked( s, DSR_CAPTURE_CS, serverTime );
	}
}

// the Nth whitespace separated token the way Cmd_TokenizeString would cut it, quotes and all,
// for the two shapes that matter here: `cs <index> "<value>"` and `bcs<n> <index> "<piece>"`
static const char *DS_Token( const char *text, int n, char *out, int outSize ) {
	int i;

	out[0] = '\0';
	for ( i = 0; i <= n; i++ ) {
		int l = 0;

		while ( *text && (unsigned char)*text <= ' ' ) {
			text++;
		}
		if ( !*text ) {
			return text;
		}
		if ( *text == '"' ) {
			text++;
			while ( *text && *text != '"' ) {
				if ( i == n && l < outSize - 1 ) {
					out[l++] = *text;
				}
				text++;
			}
			if ( *text == '"' ) {
				text++;
			}
		} else {
			while ( *text && (unsigned char)*text > ' ' ) {
				if ( i == n && l < outSize - 1 ) {
					out[l++] = *text;
				}
				text++;
			}
		}
		if ( i == n ) {
			out[l] = '\0';
		}
	}
	return text;
}

static void DS_ServerCommand( dsScan_t *s, const char *text, int serverTime, int recordStart ) {
	int i;

	// a configstring too long for one command arrives in pieces; put it back together the way
	// CL_GetServerCommand does and carry on as if it had come whole
	if ( !strncmp( text, "bcs", 3 ) && text[3] >= '0' && text[3] <= '2' && text[4] == ' ' ) {
		char	index[16];
		size_t	have;

		DS_Token( text, 1, index, sizeof( index ) );
		DS_Token( text, 2, s->bigValue, sizeof( s->bigValue ) );

		if ( text[3] == '0' ) {
			if ( strlen( index ) + strlen( s->bigValue ) + 8 >= sizeof( s->bigCs ) ) {
				s->bigCs[0] = '\0';
				return;
			}
			strcpy( s->bigCs, "cs " );
			strcat( s->bigCs, index );
			strcat( s->bigCs, " \"" );
			strcat( s->bigCs, s->bigValue );
			return;
		}
		have = strlen( s->bigCs );
		if ( !have || have + strlen( s->bigValue ) + 2 >= sizeof( s->bigCs ) ) {
			s->bigCs[0] = '\0';			// a piece with no beginning, or too much of it
			return;
		}
		strcat( s->bigCs, s->bigValue );
		if ( text[3] == '1' ) {
			return;
		}
		strcat( s->bigCs, "\"" );
		text = s->bigCs;
	}

	if ( !strncmp( text, "cs ", 3 ) ) {
		int index = atoi( text + 3 );

		if ( index >= 0 && index < MAX_CONFIGSTRINGS ) {
			if ( s->interest[index] ) {
				DR_ArgsFrom( text, 2, s->bigValue, sizeof( s->bigValue ) );
				DS_ConfigstringChanged( s, index, s->bigValue, serverTime );
			}
		}
	}

	for ( i = 0; i < s->req->numPrefixes; i++ ) {
		const char	*prefix = s->req->prefixes[i];
		size_t		n = strlen( prefix );

		if ( !n || strncmp( text, prefix, n ) ) {
			continue;
		}
		if ( s->req->prefixKeepLatest[i] ) {
			size_t len = strlen( text ) + 1;

			free( s->latest[i] );
			s->latest[i] = (char *)malloc( len );
			if ( s->latest[i] ) {
				memcpy( s->latest[i], text, len );
			}
			s->latestTime[i] = serverTime;
			s->latestGs[i] = DS_CurGs( s );
			s->latestLs[i] = DS_CurLs( s );
		} else {
			DS_Emit( s, DSR_COMMAND, i, serverTime, text, (int)strlen( text ) );
		}
		break;
	}
}

// Info_ValueForKey answers from rotating static buffers the main thread is using too
static void DS_InfoValue( const char *info, const char *key, char *out, int outSize ) {
	size_t keyLen = strlen( key );

	out[0] = '\0';
	while ( *info ) {
		const char *k, *v;

		if ( *info == '\\' ) {
			info++;
		}
		k = info;
		while ( *info && *info != '\\' ) {
			info++;
		}
		if ( !*info ) {
			return;
		}
		v = ++info;
		while ( *info && *info != '\\' ) {
			info++;
		}
		if ( (size_t)( v - 1 - k ) == keyLen && !Q_stricmpn( k, key, (int)keyLen ) ) {
			int n = (int)( info - v );

			if ( n > outSize - 1 ) {
				n = outSize - 1;
			}
			memcpy( out, v, n );
			out[n] = '\0';
			return;
		}
	}
}

// a gamestate: a new map, a clean configstring table, and at its end the recorder's client number
static void DS_Gamestate( dsScan_t *s, dsBits_t *b, int recordStart ) {
	char		mapName[MAX_QPATH];
	int			levelStart = 0;
	qboolean	haveLevelStart = qfalse;
	qboolean	firstGamestate = (qboolean)( s->raw->numGs == 0 );
	qboolean	sawEnd = qfalse;
	int			i;

	mapName[0] = '\0';

	s->lastCmdSeq = DS_ReadLong( b );
	s->haveCmdSeq = qtrue;
	DR_Gamestate( s->raw, recordStart, "", 0, 0, s->lastCmdSeq );

	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		if ( s->cs[i] ) {
			free( s->cs[i] );
			s->cs[i] = NULL;
		}
	}
	s->bigCs[0] = '\0';
	if ( s->deep ) {
		memset( s->deep, 0, sizeof( *s->deep ) );
	}

	while ( !b->overflow ) {
		int cmd = DS_ReadByte( b );

		if ( cmd == svc_EOF ) {
			sawEnd = qtrue;
			break;
		}
		if ( cmd == svc_configstring ) {
			int index = DS_ReadShort( b );

			DS_ReadString( b, s->bigValue, sizeof( s->bigValue ), BIG_INFO_STRING - 1 );
			if ( b->overflow || index < 0 || index >= MAX_CONFIGSTRINGS ) {
				break;
			}
			if ( index == CS_SERVERINFO ) {
				DS_InfoValue( s->bigValue, "mapname", mapName, sizeof( mapName ) );
			} else if ( index == CS_LEVEL_START_TIME ) {
				levelStart = atoi( s->bigValue );
				haveLevelStart = qtrue;
			}
			if ( s->interest[index] ) {
				DS_SetCs( s, index, s->bigValue );
				DS_Emit( s, DSR_GS_CS, index, s->lastTime, s->bigValue, (int)strlen( s->bigValue ) );
			}
		} else if ( cmd == svc_baseline ) {
			int number = DS_ReadBits( b, GENTITYNUM_BITS );

			if ( s->deep && number >= 0 && number < MAX_GENTITIES ) {
				dsEnt_t none;

				memset( &none, 0, sizeof( none ) );
				DS_ReadEntity( b, &none, &s->deep->baselines[number], number );
			} else {
				DS_SkipBaselineEntity( b );
			}
		} else {
			break;
		}
	}

	if ( s->raw->numGs > 0 && !s->raw->capped ) {
		demoApiGamestate_t *g = &s->raw->gs[s->raw->numGs - 1];

		Q_strncpyz( g->mapName, mapName, sizeof( g->mapName ) );
		g->levelStartTime = levelStart;
		g->haveLevelStartTime = haveLevelStart;
	}
	s->raw->lsValue = levelStart;
	s->raw->lsHave = haveLevelStart;

	if ( firstGamestate ) {
		int clientNum = sawEnd ? DS_ReadLong( b ) : -1;

		if ( sawEnd && !b->overflow && clientNum >= 0 && clientNum < MAX_CLIENTS ) {
			s->hdr->recorderClientNum = clientNum;
		} else {
			s->hdr->flags |= DEMOSCAN_FLAG_NO_RECORDER;
		}
	}
}

static void DS_HashFile( FILE *f, int fileSize, demoScanResultHeader_t *hdr, byte *scratch ) {
	// FNV-1a, 64 bit
	unsigned long long	h = 14695981039346656037ULL;
	int					want = fileSize < DS_HASH_BYTES ? fileSize : DS_HASH_BYTES;
	int					got = 0, i;

	while ( got < want ) {
		int chunk = want - got < MAX_MSGLEN ? want - got : MAX_MSGLEN;
		int n = (int)fread( scratch, 1, chunk, f );

		if ( n <= 0 ) {
			break;
		}
		for ( i = 0; i < n; i++ ) {
			h = ( h ^ scratch[i] ) * 1099511628211ULL;
		}
		got += n;
	}
	for ( i = 0; i < 4; i++ ) {
		h = ( h ^ ( ( (unsigned)fileSize >> ( i * 8 ) ) & 0xff ) ) * 1099511628211ULL;
	}
	hdr->hashLo = (unsigned)( h & 0xffffffffULL );
	hdr->hashHi = (unsigned)( h >> 32 );
}

// frees what a scan allocated; a finished job takes its blob
static void DS_ScanRelease( dsJob_t *job, dsScan_t *s ) {
	int i;

	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		free( s->cs[i] );
	}
	for ( i = 0; i < DEMOSCAN_MAX_PREFIXES; i++ ) {
		free( s->latest[i] );
	}
	if ( s->raw ) {
		DR_Free( s->raw );
	}
	free( s->raw );
	free( s->deep );
	free( s->building );

	if ( job->hdr.status == DEMOSCAN_OK ) {
		job->blob = s->blob;
		job->hdr.blobBytes = s->blobLen;
	} else {
		free( s->blob );
		job->hdr.blobBytes = 0;
		job->hdr.numRecords = 0;
	}
	memset( s, 0, sizeof( *s ) );
}

// qtrue: stopped on an abort with its state and place kept, for the worker to release or resume
static qboolean DS_ScanFile( dsJob_t *job, dsScan_t *s, byte *recordBuf, qboolean resume ) {
	FILE					*f = NULL;
	demoScanResultHeader_t	*hdr = &job->hdr;
	qboolean				haveFirst = qfalse;
	qboolean				aborted = qfalse;
	int						i;
	long					size;

	if ( resume ) {
		struct stat st;

		f = fopen( job->osPath, "rb" );
		if ( !f || stat( job->osPath, &st ) != 0 || (int)st.st_mtime != hdr->fileTime
			|| ( st.st_size > 0x7fffffffL ? 0x7fffffff : (int)st.st_size ) != hdr->fileSize ) {
			if ( f ) {
				fclose( f );
				f = NULL;
			}
			DS_ScanRelease( job, s );	// not the file it was: start over
			memset( hdr, 0, sizeof( *hdr ) );
			resume = qfalse;
		} else {
			fseek( f, s->resumeOffset, SEEK_SET );
			haveFirst = s->resumeHaveFirst;
		}
	}
	if ( !resume ) {
		memset( s, 0, sizeof( *s ) );
		s->req = &job->req;
		s->abort = &job->abort;
		s->hdr = hdr;
		// content stops short of the hard cap so the structural records at the end have room
		s->blobMax = job->req.maxBlobBytes > 0 && job->req.maxBlobBytes < DEMOSCAN_MAX_BLOB - DS_EXEMPT_RESERVE ? job->req.maxBlobBytes : DEMOSCAN_MAX_BLOB - DS_EXEMPT_RESERVE;

		hdr->recorderClientNum = -1;

		s->raw = (demoRaw_t *)malloc( sizeof( demoRaw_t ) );
		if ( !s->raw ) {
			hdr->status = DEMOSCAN_IOERROR;
			return qfalse;
		}
		DR_Init( s->raw );
		if ( job->req.mode == DEMOSCAN_MODE_DEEP && dsDeepReady ) {
			s->deep = (dsDeep_t *)calloc( 1, sizeof( dsDeep_t ) );
			s->building = (dsFrame_t *)malloc( sizeof( dsFrame_t ) );
			if ( !s->deep || !s->building ) {
				free( s->deep );
				free( s->building );
				free( s->raw );
				hdr->status = DEMOSCAN_IOERROR;
				return qfalse;
			}
		}

		for ( i = 0; i < job->req.numLogRanges && i < DEMOSCAN_MAX_RANGES; i++ ) {
			int c;

			for ( c = job->req.logRanges[i].first; c <= job->req.logRanges[i].last; c++ ) {
				if ( c >= 0 && c < MAX_CONFIGSTRINGS ) {
					s->interest[c] |= DS_INTEREST_LOG;
				}
			}
		}
		for ( i = 0; i < job->req.numTrackRanges && i < DEMOSCAN_MAX_RANGES; i++ ) {
			int c;

			for ( c = job->req.trackRanges[i].first; c <= job->req.trackRanges[i].last; c++ ) {
				if ( c >= 0 && c < MAX_CONFIGSTRINGS ) {
					s->interest[c] |= DS_INTEREST_TRACK;
				}
			}
		}
		for ( i = 0; i < job->req.numTriggers && i < DEMOSCAN_MAX_TRIGGERS; i++ ) {
			int c = job->req.triggers[i];

			if ( c >= 0 && c < MAX_CONFIGSTRINGS ) {
				s->interest[c] |= DS_INTEREST_TRIGGER;
			}
		}

		f = fopen( job->osPath, "rb" );
		if ( !f ) {
			hdr->status = DEMOSCAN_NOTFOUND;
			free( s->raw );
			free( s->deep );
			free( s->building );
			return qfalse;
		}
		fseek( f, 0, SEEK_END );
		size = ftell( f );
		fseek( f, 0, SEEK_SET );
		hdr->fileSize = size > 0x7fffffffL ? 0x7fffffff : (int)size;
		{
			struct stat st;

			hdr->fileTime = ( job->osPath && stat( job->osPath, &st ) == 0 ) ? (int)st.st_mtime : 0;
		}

		if ( ( job->req.flags & DEMOSCAN_REQ_IFCHANGED ) && hdr->fileSize == job->req.expectSize && hdr->fileTime == job->req.expectTime ) {
			fclose( f );
			DR_Free( s->raw );
			free( s->raw );
			free( s->deep );
			free( s->building );
			hdr->flags |= DEMOSCAN_FLAG_UNCHANGED;
			hdr->status = DEMOSCAN_OK;
			hdr->blobBytes = 0;
			return qfalse;
		}

		DS_HashFile( f, hdr->fileSize, hdr, recordBuf );
		fseek( f, 0, SEEK_SET );
	}
	s->resumeOffset = -1;

	while ( 1 ) {
		int			seq, len, serverTime = 0;
		long		recordStart = ftell( f );
		qboolean	gotTime = qfalse;
		dsBits_t	b;

		if ( SDL_AtomicGet( &dsPaused ) ) {
			struct stat	st;
			qboolean	changed;

			fclose( f );
			f = NULL;
			SDL_LockMutex( dsMutex );
			while ( SDL_AtomicGet( &dsPaused ) && !SDL_AtomicGet( s->abort ) && !SDL_AtomicGet( &dsQuit ) ) {
				SDL_CondWait( dsCond, dsMutex );
			}
			SDL_UnlockMutex( dsMutex );
			if ( SDL_AtomicGet( s->abort ) || SDL_AtomicGet( &dsQuit ) ) {
				s->resumeOffset = recordStart;
				s->resumeHaveFirst = haveFirst;
				aborted = qtrue;
				break;
			}
			changed = (qboolean)( stat( job->osPath, &st ) != 0 || (int)st.st_mtime != hdr->fileTime
				|| ( st.st_size > 0x7fffffffL ? 0x7fffffff : (int)st.st_size ) != hdr->fileSize );
			f = changed ? NULL : fopen( job->osPath, "rb" );
			if ( !f ) {
				SDL_LockMutex( dsMutex );
				job->requeue = qtrue;
				SDL_UnlockMutex( dsMutex );
				aborted = qtrue;
				break;
			}
			fseek( f, recordStart, SEEK_SET );
		}
		if ( SDL_AtomicGet( s->abort ) || SDL_AtomicGet( &dsQuit ) ) {
			s->resumeOffset = recordStart;
			s->resumeHaveFirst = haveFirst;
			aborted = qtrue;
			break;
		}
		if ( fread( &seq, 1, 4, f ) != 4 || fread( &len, 1, 4, f ) != 4 ) {
			break;
		}
		len = LittleLong( len );
		if ( len == -1 ) {
			hdr->flags |= DEMOSCAN_FLAG_CLEAN_END;
			break;
		}
		if ( len <= 0 || len > MAX_MSGLEN ) {
			break;
		}
		if ( (int)fread( recordBuf, 1, len, f ) != len ) {
			break;
		}

		memset( &b, 0, sizeof( b ) );
		b.data = recordBuf;
		b.totalBits = len * 8;

		DS_ReadLong( &b );				// reliableAcknowledge
		s->numCmds = 0;

		while ( !b.overflow ) {
			int cmd = DS_ReadByte( &b );

			if ( cmd == svc_nop ) {
				continue;
			}
			if ( cmd == svc_serverCommand ) {
				int		cmdSeq = DS_ReadLong( &b );
				char	*slot = s->numCmds < DS_MAX_RECORD_CMDS ? s->cmds[s->numCmds] : s->bigValue;	// past the cap it is read only to get by it

				DS_ReadString( &b, slot, MAX_STRING_CHARS, MAX_STRING_CHARS );
				if ( b.overflow ) {
					break;
				}
				// reliable commands are re-sent until acknowledged, so a demo holds most of them
				// several times over
				if ( s->haveCmdSeq && cmdSeq <= s->lastCmdSeq ) {
					continue;
				}
				s->lastCmdSeq = cmdSeq;
				s->haveCmdSeq = qtrue;
				if ( s->numCmds < DS_MAX_RECORD_CMDS ) {
					s->cmdSeqs[s->numCmds] = cmdSeq;
					s->numCmds++;
				}
				continue;
			}
			if ( cmd == svc_mapchange ) {
				for ( i = 0; i < s->numCmds; i++ ) {
					if ( DR_Command( s->raw, s->cmdSeqs[i], s->cmds[i], (int)recordStart ) ) {
						DS_ServerCommand( s, s->cmds[i], s->lastTime, (int)recordStart );
					}
				}
				s->numCmds = 0;
				DR_MapChange( s->raw, (int)recordStart );
				continue;
			}
			if ( cmd == svc_gamestate ) {
				// commands ahead of it in the same message belong to the map being left
				for ( i = 0; i < s->numCmds; i++ ) {
					if ( DR_Command( s->raw, s->cmdSeqs[i], s->cmds[i], (int)recordStart ) ) {
						DS_ServerCommand( s, s->cmds[i], s->lastTime, (int)recordStart );
					}
				}
				s->numCmds = 0;
				DS_Gamestate( s, &b, (int)recordStart );
				break;
			}
			if ( cmd == svc_snapshot ) {
				serverTime = DS_ReadLong( &b );
				gotTime = (qboolean)!b.overflow;
				if ( gotTime && s->deep && !s->raw->levelLeft ) {
					DS_Snapshot( s, &b, LittleLong( seq ), serverTime );
				}
			}
			break;						// the snapshot's entities, or something not decoded here; this record is done
		}

		// the commands in a message come before its snapshot, so they wait until its time is known
		if ( s->raw->levelLeft ) {
			s->numCmds = 0;
			continue;	// the next level's, before its gamestate
		}
		for ( i = 0; i < s->numCmds; i++ ) {
			if ( DR_Command( s->raw, s->cmdSeqs[i], s->cmds[i], (int)recordStart ) ) {
				DS_ServerCommand( s, s->cmds[i], gotTime ? serverTime : s->lastTime, (int)recordStart );
			}
		}

		if ( gotTime ) {
			if ( !haveFirst ) {
				hdr->firstServerTime = serverTime;
				haveFirst = qtrue;
			}
			DR_Snapshot( s->raw, serverTime, (int)recordStart );
			s->lastTime = s->raw->run;
			if ( job->req.mode == DEMOSCAN_MODE_HEADER && s->raw->numGs > 0 ) {
				break;
			}
		}
	}

	if ( f ) {
		fclose( f );
	}

	hdr->lastServerTime = s->lastTime;
	hdr->numGamestates = s->raw->numGs;

	if ( aborted ) {
		hdr->status = DEMOSCAN_ABORTED;
		if ( s->resumeOffset >= 0 ) {
			return qtrue;
		}
	} else if ( !s->raw->numGs ) {
		hdr->status = DEMOSCAN_BADFORMAT;
	} else {
		hdr->status = DEMOSCAN_OK;

		if ( job->req.mode != DEMOSCAN_MODE_HEADER ) {
			DS_EmitTracked( s, DSR_END_CS, s->lastTime );
			for ( i = 0; i < DEMOSCAN_MAX_PREFIXES; i++ ) {
				if ( s->latest[i] ) {
					// under the segment it was said in, not the one the file ends in
					DS_EmitUnder( s, DSR_COMMAND, i, s->latestTime[i], s->latestGs[i], s->latestLs[i], s->latest[i], (int)strlen( s->latest[i] ) );
				}
			}
		}
		s->exempt = 1;
		DS_EmitUnder( s, DSR_GAMESTATES, s->raw->numGs, 0, 0, 0, s->raw->gs, s->raw->numGs * (int)sizeof( demoApiGamestate_t ) );
		if ( s->raw->numLs ) {
			DS_EmitUnder( s, DSR_LEVELSTARTS, s->raw->numLs, 0, 0, 0, s->raw->ls, s->raw->numLs * (int)sizeof( demoApiLevelStart_t ) );
		}
		if ( job->req.flags & DEMOSCAN_REQ_EARLYTIMES ) {
			DS_EmitUnder( s, DSR_EARLYTIMES, s->raw->numEarly, 0, 0, 0, s->raw->early, s->raw->numEarly * (int)sizeof( int ) );
		}
		s->exempt = 0;
		if ( s->exemptDropped ) {
			hdr->status = DEMOSCAN_BADFORMAT;
		}
	}

	DS_ScanRelease( job, s );
	return qfalse;
}

// dsMutex held
static void DS_FreeJob( dsJob_t *job ) {
	if ( job == dsSuspendedJob ) {
		DS_ScanRelease( job, dsSuspendedScan );
		dsSuspendedJob = NULL;
		dsSuspendedScan = NULL;
	}
	free( job->osPath );
	free( job->blob );
	free( job );
}

// dsMutex held
static void DS_Unlink( dsJob_t *job ) {
	dsJob_t **p;

	for ( p = &dsJobs; *p; p = &( *p )->next ) {
		if ( *p == job ) {
			*p = job->next;
			return;
		}
	}
}

// dsMutex held. The most urgent queued job, the oldest among equals.
static dsJob_t *DS_PickJob( void ) {
	dsJob_t *job, *best = NULL;

	if ( SDL_AtomicGet( &dsPaused ) ) {
		return NULL;
	}
	for ( job = dsJobs; job; job = job->next ) {
		if ( job->state != DSJ_QUEUED ) {
			continue;
		}
		// a header answers "what is this file" for a whole list at a glance; it goes before any
		// full scan of the same urgency
		if ( !best || job->priority > best->priority
			|| ( job->priority == best->priority && job->req.mode < best->req.mode ) ) {
			best = job;
		}
	}
	return best;
}

static int DS_WorkerMain( void *unused ) {
	qboolean	resume, holding;
	byte		*recordBuf = (byte *)malloc( MAX_MSGLEN );
	dsScan_t	*scan = (dsScan_t *)malloc( sizeof( dsScan_t ) );
	dsScan_t	*spare = (dsScan_t *)malloc( sizeof( dsScan_t ) );	// holds a pre-empted job's state while something more urgent runs

	if ( !recordBuf || !scan ) {
		free( recordBuf );
		free( scan );
		free( spare );
		return 0;
	}

	SDL_SetThreadPriority( SDL_THREAD_PRIORITY_LOW );

	SDL_LockMutex( dsMutex );
	while ( !SDL_AtomicGet( &dsQuit ) ) {
		dsJob_t *job = DS_PickJob();

		if ( !job ) {
			SDL_CondWait( dsCond, dsMutex );
			continue;
		}
		if ( dsAppliedGen != dsStagedGen ) {
			DS_ApplyLayout();
			if ( dsSuspendedJob ) {
				// decoded with the old field widths: it starts over
				DS_ScanRelease( dsSuspendedJob, dsSuspendedScan );
				memset( &dsSuspendedJob->hdr, 0, sizeof( dsSuspendedJob->hdr ) );
				dsSuspendedJob = NULL;
				dsSuspendedScan = NULL;
			}
		}
		resume = (qboolean)( job == dsSuspendedJob );
		if ( resume ) {
			spare = scan;
			scan = dsSuspendedScan;
			dsSuspendedJob = NULL;
			dsSuspendedScan = NULL;
		}
		job->state = DSJ_RUNNING;
		dsRunning = job;
		SDL_UnlockMutex( dsMutex );

		holding = DS_ScanFile( job, scan, recordBuf, resume );

		SDL_LockMutex( dsMutex );
		dsRunning = NULL;
		if ( holding && job->requeue && !job->cancelled && spare && !dsSuspendedJob && !SDL_AtomicGet( &dsQuit ) ) {
			// pre-empted: it goes on from where it stopped once the urgent job is done
			dsSuspendedJob = job;
			dsSuspendedScan = scan;
			scan = spare;
			spare = NULL;
			job->requeue = qfalse;
			SDL_AtomicSet( &job->abort, 0 );
			job->state = DSJ_QUEUED;
		} else if ( job->cancelled ) {
			if ( holding ) {
				DS_ScanRelease( job, scan );
			}
			DS_Unlink( job );
			DS_FreeJob( job );
		} else if ( job->requeue && job->hdr.status == DEMOSCAN_ABORTED ) {
			if ( holding ) {
				DS_ScanRelease( job, scan );
			}
			job->requeue = qfalse;
			SDL_AtomicSet( &job->abort, 0 );
			memset( &job->hdr, 0, sizeof( job->hdr ) );
			job->state = DSJ_QUEUED;
		} else {
			if ( holding ) {
				DS_ScanRelease( job, scan );
			}
			job->requeue = qfalse;
			job->state = DSJ_DONE;
		}
	}
	if ( dsSuspendedJob ) {
		DS_ScanRelease( dsSuspendedJob, dsSuspendedScan );
		dsSuspendedJob = NULL;
		dsSuspendedScan = NULL;
	}
	SDL_UnlockMutex( dsMutex );

	free( recordBuf );
	free( scan );
	free( spare );
	return 0;
}

#ifdef DEMO_DEBUG
static void CL_DemoScanDump_f( void );
#endif

static void DS_ComputeLayout( dsLayout_t *l ) {
	static const char	*tracked[DST_NUM] = { "eType", "eventParm", "otherEntityNum", "otherEntityNum2", "userInt1", "userInt2", "userInt3" };
	int					i, table;

	memset( l, 0, sizeof( *l ) );
	l->numFields = MSG_EntityFieldLayout( l->fieldBits, DS_MAX_FIELDS, &l->floatIntBits );
	l->deepReady = qtrue;
	for ( i = 0; i < DS_MAX_FIELDS; i++ ) {
		l->trackedSlot[i] = -1;
	}
	for ( i = 0; i < DST_NUM; i++ ) {
		int field = MSG_EntityFieldIndex( tracked[i] );

		if ( field < 0 || field >= DS_MAX_FIELDS ) {
			l->deepReady = qfalse;
		} else {
			l->trackedSlot[field] = i;
		}
	}
	for ( table = 0; table < 3; table++ ) {
		l->psNumFields[table] = MSG_PlayerFieldLayout( table, l->psBits[table], DS_MAX_FIELDS, "m_iVehicleNum", &l->psVehicleField[table] );
		if ( l->psNumFields[table] <= 0 ) {
			l->deepReady = qfalse;
		}
	}
}

// worker between jobs (or init, before the worker exists)
static void DS_ApplyLayout( void ) {
	memcpy( dsFieldBits, dsStaged.fieldBits, sizeof( dsFieldBits ) );
	dsNumFields = dsStaged.numFields;
	dsFloatIntBits = dsStaged.floatIntBits;
	memcpy( dsTrackedSlot, dsStaged.trackedSlot, sizeof( dsTrackedSlot ) );
	memcpy( dsPsBits, dsStaged.psBits, sizeof( dsPsBits ) );
	memcpy( dsPsNumFields, dsStaged.psNumFields, sizeof( dsPsNumFields ) );
	memcpy( dsPsVehicleField, dsStaged.psVehicleField, sizeof( dsPsVehicleField ) );
	dsDeepReady = dsStaged.deepReady;
	dsAppliedGen = dsStagedGen;
}

void CL_DemoScan_RefreshLayout( void ) {
	if ( !dsMutex ) {
		return;
	}
	SDL_LockMutex( dsMutex );
	DS_ComputeLayout( &dsStaged );
	dsStagedGen++;
	SDL_UnlockMutex( dsMutex );
}

void CL_DemoScan_Init( void ) {
	dsHuffTree = MSG_HuffDecodeTree();
	DS_ComputeLayout( &dsStaged );
	DS_ApplyLayout();
	dsMutex = SDL_CreateMutex();
	dsCond = SDL_CreateCond();
	SDL_AtomicSet( &dsQuit, 0 );
	SDL_AtomicSet( &dsPaused, 0 );
	dsReady = (qboolean)( dsHuffTree && dsNumFields > 0 && dsMutex && dsCond );
#ifdef DEMO_DEBUG
	Cmd_AddCommand( "demoScanDump", CL_DemoScanDump_f, "Scan a demo file the way the ui module would and print what came back" );
#endif
}

// dsMutex held
static void DS_AbortRunning( qboolean requeue ) {
	if ( dsRunning ) {
		dsRunning->requeue = requeue;
		SDL_AtomicSet( &dsRunning->abort, 1 );
		SDL_CondBroadcast( dsCond );	// it may be parked by a pause
	}
}

void CL_DemoScan_CancelAll( void ) {
	dsJob_t *job, *next;

	if ( !dsReady ) {
		return;
	}
	SDL_LockMutex( dsMutex );
	SDL_AtomicSet( &dsPaused, 0 );
	SDL_CondBroadcast( dsCond );
	for ( job = dsJobs; job; job = next ) {
		next = job->next;
		if ( job == dsRunning ) {
			job->cancelled = qtrue;
			DS_AbortRunning( qfalse );
			continue;					// the worker frees it when it notices
		}
		DS_Unlink( job );
		DS_FreeJob( job );
	}
	SDL_UnlockMutex( dsMutex );
}

void CL_DemoScan_Shutdown( void ) {
	if ( !dsReady ) {
		return;
	}
	CL_DemoScan_CancelAll();

	SDL_LockMutex( dsMutex );
	SDL_AtomicSet( &dsQuit, 1 );
	SDL_CondBroadcast( dsCond );
	SDL_UnlockMutex( dsMutex );

	if ( dsThread ) {
		SDL_WaitThread( dsThread, NULL );
		dsThread = NULL;
	}
	while ( dsJobs ) {
		dsJob_t *job = dsJobs;

		dsJobs = job->next;
		DS_FreeJob( job );
	}
	dsReady = qfalse;
	SDL_DestroyCond( dsCond );
	SDL_DestroyMutex( dsMutex );
	dsCond = NULL;
	dsMutex = NULL;
}

// where a demo lives on disk. A demo inside a pk3 has no such place and cannot be scanned.
static qboolean DS_ResolvePath( const char *demoPath, char *out, int outSize ) {
	char		qpath[MAX_OSPATH];
	const char	*p;

	// stays under demos/: no absolute path, and no ".." component (a NAME may well contain dots)
	if ( demoPath[0] == '/' || demoPath[0] == '\\' ) {
		return qfalse;
	}
	for ( p = demoPath; *p; ) {
		const char *end = p;

		while ( *end && *end != '/' && *end != '\\' ) {
			end++;
		}
		if ( end - p == 2 && p[0] == '.' && p[1] == '.' ) {
			return qfalse;
		}
		p = *end ? end + 1 : end;
	}
	Com_sprintf( qpath, sizeof( qpath ), "demos/%s", demoPath );
	return FS_ResolveDirFile( qpath, out, outSize );	// the same directory the demo command finds first
}

int CL_DemoScan_Submit( const char *demoPath, const demoScanRequest_t *req ) {
	char	osPath[MAX_OSPATH];
	dsJob_t	*job, **tail;
	size_t	n;

	if ( !dsReady || !demoPath || !req || !DS_ResolvePath( demoPath, osPath, sizeof( osPath ) ) ) {
		return 0;
	}

	job = (dsJob_t *)calloc( 1, sizeof( *job ) );
	n = strlen( osPath ) + 1;
	if ( job ) {
		job->osPath = (char *)malloc( n );
	}
	if ( !job || !job->osPath ) {
		free( job );
		return 0;
	}
	memcpy( job->osPath, osPath, n );
	job->req = *req;
	job->priority = req->priority;
	job->state = DSJ_QUEUED;
	SDL_AtomicSet( &job->abort, 0 );

	// a request comes from a module and is not to be trusted with the worker's array bounds
	job->req.numLogRanges = Com_Clampi( 0, DEMOSCAN_MAX_RANGES, job->req.numLogRanges );
	job->req.numTrackRanges = Com_Clampi( 0, DEMOSCAN_MAX_RANGES, job->req.numTrackRanges );
	job->req.numTriggers = Com_Clampi( 0, DEMOSCAN_MAX_TRIGGERS, job->req.numTriggers );
	job->req.numPrefixes = Com_Clampi( 0, DEMOSCAN_MAX_PREFIXES, job->req.numPrefixes );
	job->req.numEvents = Com_Clampi( 0, DEMOSCAN_MAX_EVENTS, job->req.numEvents );
	for ( n = 0; n < DEMOSCAN_MAX_PREFIXES; n++ ) {
		job->req.prefixes[n][DEMOAPI_PREFIX_LEN - 1] = '\0';
	}
	for ( n = 0; n < DEMOSCAN_MAX_RANGES; n++ ) {
		job->req.logRanges[n].first = Com_Clampi( 0, MAX_CONFIGSTRINGS - 1, job->req.logRanges[n].first );
		job->req.logRanges[n].last = Com_Clampi( 0, MAX_CONFIGSTRINGS - 1, job->req.logRanges[n].last );
		job->req.trackRanges[n].first = Com_Clampi( 0, MAX_CONFIGSTRINGS - 1, job->req.trackRanges[n].first );
		job->req.trackRanges[n].last = Com_Clampi( 0, MAX_CONFIGSTRINGS - 1, job->req.trackRanges[n].last );
	}

	SDL_LockMutex( dsMutex );
	job->id = dsNextId++;
	for ( tail = &dsJobs; *tail; tail = &( *tail )->next ) {
	}
	*tail = job;

	if ( dsRunning && dsRunning->priority < job->priority && dsRunning->req.mode >= DEMOSCAN_MODE_FULL
		&& dsRunning->priority == DEMOSCAN_PRIO_IDLE ) {
		DS_AbortRunning( qtrue );
	}

	if ( !dsThread ) {
		dsThread = SDL_CreateThread( DS_WorkerMain, "demoscan", NULL );
		if ( !dsThread ) {
			DS_Unlink( job );
			SDL_UnlockMutex( dsMutex );
			DS_FreeJob( job );
			return 0;
		}
	}
	SDL_CondSignal( dsCond );
	SDL_UnlockMutex( dsMutex );

	return job->id;
}

int CL_DemoScan_Poll( demoScanResultHeader_t *out, int max ) {
	dsJob_t	*job;
	int		n = 0;

	if ( !dsReady || !out || max <= 0 ) {
		return 0;
	}
	SDL_LockMutex( dsMutex );
	for ( job = dsJobs; job && n < max; job = job->next ) {
		if ( job->state == DSJ_DONE && !job->reported ) {
			job->reported = qtrue;
			job->hdr.jobId = job->id;
			job->hdr.mode = job->req.mode;
			out[n++] = job->hdr;
		}
	}
	SDL_UnlockMutex( dsMutex );
	return n;
}

#ifdef DEMO_DEBUG
// a debug command's own job, without taking the ui's results from under it
int CL_DemoScan_WaitJob( int jobId, demoScanResultHeader_t *out, int tries ) {
	int i;

	for ( i = 0; i < tries; i++ ) {
		dsJob_t *job;

		SDL_Delay( 5 );
		SDL_LockMutex( dsMutex );
		for ( job = dsJobs; job; job = job->next ) {
			if ( job->id == jobId && job->state == DSJ_DONE ) {
				job->reported = qtrue;
				job->hdr.jobId = job->id;
				job->hdr.mode = job->req.mode;
				*out = job->hdr;
				break;
			}
		}
		SDL_UnlockMutex( dsMutex );
		if ( job ) {
			return 1;
		}
	}
	return 0;
}
#endif

int CL_DemoScan_Fetch( int jobId, void *buf, int bufSize ) {
	dsJob_t	*job;
	int		copied = 0;

	if ( !dsReady ) {
		return 0;
	}
	SDL_LockMutex( dsMutex );
	for ( job = dsJobs; job; job = job->next ) {
		if ( job->id == jobId ) {
			break;
		}
	}
	if ( job && job->state == DSJ_DONE ) {
		DS_Unlink( job );
	} else {
		job = NULL;
	}
	SDL_UnlockMutex( dsMutex );

	if ( job ) {
		if ( buf && job->blob && job->hdr.blobBytes <= bufSize ) {
			memcpy( buf, job->blob, job->hdr.blobBytes );
			copied = job->hdr.blobBytes;
		}
		DS_FreeJob( job );
	}
	return copied;
}

int CL_DemoScan_Control( int op, int jobId, int value ) {
	dsJob_t	*job;
	int		result = 0;

	if ( !dsReady ) {
		return 0;
	}
	if ( op == DEMOSCAN_OP_CANCELALL ) {
		CL_DemoScan_CancelAll();
		return 1;
	}
	if ( op == DEMOSCAN_OP_PAUSE ) {
		SDL_LockMutex( dsMutex );
		SDL_AtomicSet( &dsPaused, value ? 1 : 0 );
		SDL_CondBroadcast( dsCond );
		SDL_UnlockMutex( dsMutex );
		return 1;
	}

	SDL_LockMutex( dsMutex );
	if ( op == DEMOSCAN_OP_PENDING ) {
		for ( job = dsJobs; job; job = job->next ) {
			if ( job->state != DSJ_DONE ) {
				result++;
			}
		}
	} else {
		for ( job = dsJobs; job; job = job->next ) {
			if ( job->id == jobId ) {
				break;
			}
		}
		if ( job && op == DEMOSCAN_OP_SETPRIO ) {
			job->priority = value;
			if ( job->state == DSJ_QUEUED && dsRunning && dsRunning->priority < value
				&& dsRunning->req.mode >= DEMOSCAN_MODE_FULL && dsRunning->priority == DEMOSCAN_PRIO_IDLE ) {
				DS_AbortRunning( qtrue );
			}
			result = 1;
		} else if ( job && op == DEMOSCAN_OP_CANCEL ) {
			if ( job == dsRunning ) {
				job->cancelled = qtrue;
				DS_AbortRunning( qfalse );
			} else {
				DS_Unlink( job );
				DS_FreeJob( job );
			}
			result = 1;
		}
	}
	SDL_UnlockMutex( dsMutex );
	return result;
}

#ifdef DEMO_DEBUG
static void CL_DemoScanDump_f( void ) {
	static byte				blob[DEMOSCAN_MAX_BLOB];
	static const char		*typeNames[] = { "?", "SEGMENT", "GS_CS", "CS_UPDATE", "CAPTURE_CS", "END_CS", "COMMAND", "EVENT" };
	demoScanRequest_t		req;
	demoScanResultHeader_t	hdr;
	qboolean				verbose = qfalse;
	int						id, i, got, bytes, counts[11] = { 0 };
	int						start = Sys_Milliseconds();

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "usage: demoScanDump <path under demos/, with extension> [header] [verbose]\n" );
		return;
	}

	memset( &req, 0, sizeof( req ) );
	req.mode = DEMOSCAN_MODE_FULL;
	req.priority = DEMOSCAN_PRIO_SELECTED;
	for ( i = 2; i < Cmd_Argc(); i++ ) {
		if ( !Q_stricmp( Cmd_Argv( i ), "header" ) ) {
			req.mode = DEMOSCAN_MODE_HEADER;
		} else if ( !Q_stricmp( Cmd_Argv( i ), "deep" ) ) {
			req.mode = DEMOSCAN_MODE_DEEP;
			req.numEvents = 1;
			req.events[0] = EV_OBITUARY;
		} else if ( !Q_stricmp( Cmd_Argv( i ), "verbose" ) ) {
			verbose = qtrue;
		}
	}
	req.numLogRanges = 3;
	req.logRanges[0].first = CS_SCORES1;			req.logRanges[0].last = CS_SCORES2;
	req.logRanges[1].first = CS_LEVEL_START_TIME;	req.logRanges[1].last = CS_INTERMISSION;
	req.logRanges[2].first = 35;					req.logRanges[2].last = 35;
	req.numTrackRanges = 2;
	req.trackRanges[0].first = CS_SERVERINFO;		req.trackRanges[0].last = CS_SERVERINFO;
	req.trackRanges[1].first = CS_PLAYERS;			req.trackRanges[1].last = CS_PLAYERS + MAX_CLIENTS - 1;
	req.numTriggers = 1;
	req.triggers[0] = CS_INTERMISSION;
	req.numPrefixes = 2;
	Q_strncpyz( req.prefixes[0], "kls -1 -1 mfs", sizeof( req.prefixes[0] ) );
	Q_strncpyz( req.prefixes[1], "scores", sizeof( req.prefixes[1] ) );
	req.prefixKeepLatest[1] = 1;

	id = CL_DemoScan_Submit( Cmd_Argv( 1 ), &req );
	if ( !id ) {
		Com_Printf( "[demoscan] cannot scan '%s'\n", Cmd_Argv( 1 ) );
		return;
	}
	got = CL_DemoScan_WaitJob( id, &hdr, 2000 );
	if ( !got ) {
		Com_Printf( "[demoscan] timed out\n" );
		CL_DemoScan_Control( DEMOSCAN_OP_CANCEL, id, 0 );
		return;
	}
	bytes = CL_DemoScan_Fetch( hdr.jobId, blob, sizeof( blob ) );

	Com_Printf( "[demoscan] status=%d flags=%d mode=%d size=%d hash=%08x%08x recorder=%d first=%d last=%d segments=%d records=%d blob=%d ms=%d\n",
		hdr.status, hdr.flags, hdr.mode, hdr.fileSize, hdr.hashHi, hdr.hashLo, hdr.recorderClientNum,
		hdr.firstServerTime, hdr.lastServerTime, hdr.numGamestates, hdr.numRecords, bytes, Sys_Milliseconds() - start );

	for ( i = 0; i + (int)sizeof( demoScanRecord_t ) <= bytes; ) {
		const demoScanRecord_t	*rec = (const demoScanRecord_t *)( blob + i );
		const char				*payload = (const char *)( rec + 1 );

		if ( rec->type > 0 && rec->type < 11 ) {
			counts[rec->type]++;
		}
		if ( rec->type == DSR_EVENT ) {
			const demoScanEvent_t *ev = (const demoScanEvent_t *)payload;

			if ( verbose || ev->userInt2 ) {
				Com_Printf( "[demoscan] EVENT %d t=%d seg=%d: ent=%d parm=%d victim=%d attacker=%d user=%d,%d,%d%s\n", rec->index, rec->serverTime, rec->gamestate,
					ev->entityNum, ev->eventParm, ev->otherEntityNum, ev->otherEntityNum2, ev->userInt1, ev->userInt2, ev->userInt3,
					ev->otherEntityNum2 == hdr.recorderClientNum ? " (recorder)" : "" );
			}
		} else if ( rec->type == DSR_GAMESTATES ) {
			const demoApiGamestate_t *g = (const demoApiGamestate_t *)payload;
			int k;

			for ( k = 0; k < rec->index; k++ ) {
				Com_Printf( "[demoscan] gs %d: map=%s offset=%d first=%d last=%d settle=%d levelstarts=%d\n", k, g[k].mapName,
					g[k].fileOffset, g[k].firstTime, g[k].lastTime, g[k].settleTime, g[k].numLevelStarts );
			}
		} else if ( verbose || rec->type == DSR_CS_UPDATE || rec->type == DSR_COMMAND
			|| ( rec->type == DSR_GS_CS && rec->index != CS_SERVERINFO && rec->index < CS_PLAYERS ) ) {
			Com_Printf( "[demoscan] %s index=%d t=%d seg=%d: %.200s\n", typeNames[rec->type > 0 && rec->type < 8 ? rec->type : 0],
				rec->index, rec->serverTime, rec->gamestate, payload );
		}
		i += DEMOSCAN_RECORD_SIZE( rec->len );
	}
	Com_Printf( "[demoscan] counts: segment=%d gs_cs=%d cs_update=%d capture_cs=%d end_cs=%d command=%d event=%d snapshots=%d bad=%d\n",
		counts[DSR_GAMESTATES], counts[DSR_GS_CS], counts[DSR_CS_UPDATE], counts[DSR_CAPTURE_CS], counts[DSR_END_CS], counts[DSR_COMMAND], counts[DSR_EVENT],
		hdr.numSnapshots, hdr.numBadSnapshots );
}
#endif
