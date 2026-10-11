#pragma once

#include "cg_demoapi.h"

#define DEMOSCAN_MODE_HEADER		0	// the first gamestate and the first snapshot: what map, who, when
#define DEMOSCAN_MODE_FULL			1	// the whole file: its commands and configstrings, none of what its snapshots hold
#define DEMOSCAN_MODE_DEEP			2	// the whole file with every snapshot decoded, for what only entities say. several times the work.

#define DEMOSCAN_PRIO_IDLE			0
#define DEMOSCAN_PRIO_VISIBLE		1
#define DEMOSCAN_PRIO_SELECTED		2

#define DEMOSCAN_MAX_RANGES			8
#define DEMOSCAN_MAX_TRIGGERS		4
#define DEMOSCAN_MAX_PREFIXES		4
#define DEMOSCAN_MAX_CAPTURES		4
#define DEMOSCAN_MAX_EVENTS			8
#define DEMOSCAN_MAX_BLOB			( 1 << 20 )

typedef struct demoScanRange_s {
	int		first, last;		// configstring indices, inclusive
} demoScanRange_t;

typedef struct demoScanRequest_s {
	int				mode;
	int				priority;

	// LOGGED configstrings: every change of value becomes a DSR_CS_UPDATE stamped with its time.
	// Meant for the few small ones whose history matters (scores, clocks, flags).
	int				numLogRanges;
	demoScanRange_t	logRanges[DEMOSCAN_MAX_RANGES];

	int				numTrackRanges;
	demoScanRange_t	trackRanges[DEMOSCAN_MAX_RANGES];

	// a change of value in one of these writes the tracked set out as it stands (DSR_CAPTURE_CS);
	// only the first DEMOSCAN_MAX_CAPTURES changes do
	int				numTriggers;
	int				triggers[DEMOSCAN_MAX_TRIGGERS];

	// server commands beginning with one of these are returned as DSR_COMMAND: all of them, or
	// with keepLatest only the last one seen, written at the end of the file
	int				numPrefixes;
	char			prefixes[DEMOSCAN_MAX_PREFIXES][DEMOAPI_PREFIX_LEN];
	int				prefixKeepLatest[DEMOSCAN_MAX_PREFIXES];

	int				maxBlobBytes;		// 0 = DEMOSCAN_MAX_BLOB; records past it are dropped and flagged

	// DEEP only: freestanding event entities (eType ET_EVENTS + n) whose event number n is one of
	// these come back as DSR_EVENT, once each, stamped with the snapshot they first appeared in
	int				numEvents;
	int				events[DEMOSCAN_MAX_EVENTS];

	int				flags;				// DEMOSCAN_REQ_*
	int				eventAttacker;		// with DEMOSCAN_REQ_EVENT_ATTACKER: only events whose otherEntityNum2 is this
	int				expectSize;			// with DEMOSCAN_REQ_IFCHANGED
	int				expectTime;
} demoScanRequest_t;

#define DEMOSCAN_REQ_EARLYTIMES		0x1	// also write DSR_EARLYTIMES
#define DEMOSCAN_REQ_EVENT_ATTACKER	0x2
#define DEMOSCAN_REQ_IFCHANGED		0x4	// stop at DEMOSCAN_FLAG_UNCHANGED if the file's size and mtime are the expected ones

#define DEMOSCAN_OK					0
#define DEMOSCAN_NOTFOUND			1
#define DEMOSCAN_IOERROR			2
#define DEMOSCAN_BADFORMAT			3	// no gamestate where a demo has one
#define DEMOSCAN_ABORTED			4

#define DEMOSCAN_FLAG_TRUNCATED		0x1	// records were dropped for room
#define DEMOSCAN_FLAG_CLEAN_END		0x2	// the file ends with its end marker rather than mid-record
#define DEMOSCAN_FLAG_NO_RECORDER	0x4	// recorderClientNum could not be read
#define DEMOSCAN_FLAG_UNCHANGED		0x8	// DEMOSCAN_REQ_IFCHANGED: nothing was read

typedef struct demoScanResultHeader_s {
	int			jobId;
	int			status;
	int			flags;
	int			mode;
	int			fileSize;
	int			fileTime;				// when the file was last written, unix time; 0 unknown
	unsigned	hashLo, hashHi;			// of the file's opening bytes and its size: identifies the CONTENT, so it survives a rename or a copy
	int			recorderClientNum;		// whose demo it is, -1 unknown
	int			firstServerTime;
	int			lastServerTime;
	int			numGamestates;
	int			numRecords;
	int			blobBytes;				// what FETCH will copy
	int			numSnapshots;			// DEEP: snapshots decoded
	int			numBadSnapshots;		// DEEP: snapshots that did not end where a snapshot ends. anything but 0 means the decode cannot be trusted
} demoScanResultHeader_t;

// 1 is retired
#define DSR_GS_CS			2	// a logged or tracked configstring as a gamestate installed it
#define DSR_CS_UPDATE		3	// a logged configstring changed value
#define DSR_CAPTURE_CS		4	// a tracked configstring as it stood when a trigger fired
#define DSR_END_CS			5	// a tracked configstring as it stood at the end of the file
#define DSR_COMMAND			6	// a server command; index is which prefix matched
#define DSR_EVENT			7	// payload: demoScanEvent_t; index is the event number
#define DSR_GAMESTATES		8	// payload: demoApiGamestate_t[index]; written last
#define DSR_LEVELSTARTS		9	// payload: demoApiLevelStart_t[index]; written last
#define DSR_EARLYTIMES		10	// payload: int[index]; written last

typedef struct demoScanRecord_s {
	int		type;
	int		index;				// configstring index, or prefix index for DSR_COMMAND
	int		serverTime;
	int		gamestate;			// 0-based
	int		lsIndex;			// level start changes seen so far under that gamestate
	int		len;
} demoScanRecord_t;

// what an event entity carried. the engine does not know what any of it means to the game.
typedef struct demoScanEvent_s {
	int		entityNum;
	int		eventParm;
	int		otherEntityNum;
	int		otherEntityNum2;
	int		userInt1, userInt2, userInt3;
} demoScanEvent_t;

#define DEMOSCAN_RECORD_SIZE( len )	( (int)sizeof( demoScanRecord_t ) + ( ( ( len ) + 1 + 3 ) & ~3 ) )

#define DEMOSCAN_OP_SETPRIO			0	// ( jobId, priority )
#define DEMOSCAN_OP_CANCEL			1	// ( jobId ): queued, running or finished, it is gone
#define DEMOSCAN_OP_CANCELALL		2
#define DEMOSCAN_OP_PENDING			3	// -> jobs not yet finished
#define DEMOSCAN_OP_PAUSE			4	// ( 0, 1|0 ): the worker parks between records until unpaused; whoever asked for jobs is busy
