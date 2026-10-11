// cl_demo_decode.h -- a demo decoder that owns all of its state. The playback service's forward,
// refill and index decoders are instances; the live parser in cl_parse.cpp is not involved.

#pragma once

#include "client.h"
#include <stdio.h>

#define DEMODEC_FORWARD		0
#define DEMODEC_REFILL		1
#define DEMODEC_INDEX		2

#define DEMODEC_MAX_RECORD_CMDS	MAX_RELIABLE_COMMANDS
#define DEMODEC_RECORD_PAD		8192
#define DEMODEC_CMD_POOL		( MAX_RELIABLE_COMMANDS * MAX_STRING_CHARS )	// a record can carry the whole reliable window

// a seek index point: the decoder state a decode can resume from at a record boundary
typedef struct {
	int				fileOffset;			// the record boundary
	int				segmentOffset;		// file offset of the gamestate this keyframe lives under
	int				gamestate;
	int				serverTime;			// snapshot serverTime here -- monotonic, unlike the match clock
	int				messageSequence;
	int				commandSequence;
	int				parseEntitiesNum;
	int				numSnaps;
	int				entsBase;			// absolute parseEntitiesNum of the first captured entity
	int				numEnts;
	gameState_t		*gameState;
	clSnapshot_t	*snaps;			// numSnaps entries, newest last, matching stream order
	entityState_t	*ents;
	int				bad;			// a restore from it failed verification; skipped from then on
} demoKeyframe_t;

typedef struct demoDecoder_s {
	int				role;

	// the file: one handle of its own, so decoders never move each other's position
	fileHandle_t	file;
	FILE			*stdio;				// instead of the handle when a thread reads
	int				fileOffset;			// the next record
	int				recordOffset;		// the record last read
	int				streamSegment;		// offset of the gamestate record the decoder is under, -1 none
	int				gamestate;			// its index, -1 none

	// decoder state (what cl and clc hold for the live parser)
	clSnapshot_t	snapshots[PACKET_BACKUP];
	clSnapshot_t	snap;
	int				parseEntitiesNum;
	entityState_t	*parseEntities;		// MAX_PARSE_ENTITIES
	entityState_t	*baselines;			// MAX_GENTITIES
	byte			*recordBuf;			// MAX_MSGLEN
	gameState_t		gameState;
	int				serverMessageSequence;
	int				serverCommandSequence;
	char			bigConfigString[BIG_INFO_STRING];

	// this record's commands, logged once its snapshot time is known
	int				recordCmdSeq[DEMODEC_MAX_RECORD_CMDS];
	int				recordCmdText[DEMODEC_MAX_RECORD_CMDS];
	int				numRecordCmds;
	int				recordCmdsBeforeGamestate;
	char			cmdPool[DEMODEC_CMD_POOL];
	int				cmdPoolUsed;
	qboolean		snapDecoded;		// this record carried a snapshot that was stored
	qboolean		gamestateParsed;	// this record was a gamestate

	// where it stands
	qboolean		eof;
	qboolean		fault;				// the decoder lost its place; reposition before reading on
	int				faultOffset;		// where, so a repeat can be given up on
	int				faultCount;

	// keyframe verification
	qboolean		kfRestoring;
	qboolean		kfRestoreFailed;
	int				kfRestoreSnapTime;
	int				kfVerifyReads;
	int				kfRestoredOffset;

	// a keyframe taken at a record, kept whole until later records show how far back their
	// deltas reach; stored trimmed to that
	demoKeyframe_t	pendingKf;
	int				pendingMinBase;
	qboolean		pendingValid;

	// entity events seen, per slot, so each is logged once (the index pass)
	int				evLastKey[MAX_GENTITIES];
	int				evLastTime[MAX_GENTITIES];
	int				evLastMessage[MAX_GENTITIES];
	int				evLastGamestate;		// the level the slots above belong to
	int				evPrevMessage;			// messageNum of the previous snapshot scanned

	int				filePos;			// where the handle stands, so a read in sequence does not seek

	int				guard;				// records read; the loops' safety stop
} demoDecoder_t;

demoDecoder_t	*Demo_DecoderAlloc( int role );
void			Demo_DecoderFree( demoDecoder_t *d );
qboolean		Demo_DecoderOpen( demoDecoder_t *d, const char *demoPath );	// the engine path ("demos/x.dm_26")
void			Demo_DecoderReset( demoDecoder_t *d, int fileOffset );			// empty state, positioned at a record boundary
qboolean		Demo_ReadRecord( demoDecoder_t *d );							// one record; qfalse at eof or on a fault
qboolean		Demo_NextRecordLeavesSegment( const demoDecoder_t *d );
void			Demo_Reposition( demoDecoder_t *d, int gamestate, int target );	// a keyframe before target, else the map's record
int				Demo_DeltaReach( const demoDecoder_t *d );
void			Demo_GsSet( gameState_t *gs, int index, const char *value );

// hooks the service provides (cl_demo_cache.cpp)
void			CL_DemoDecoderRecordDone( demoDecoder_t *d );
void			CL_DemoKeyframePendingDrop( demoDecoder_t *d );
