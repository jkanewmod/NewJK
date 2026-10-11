// cg_demoapi.h -- the demo playback service. Shared verbatim with the game module.

#pragma once

#define DEMOAPI_MAX_GAMESTATES		256
#define DEMOAPI_MAX_MAPNAME			64
#define DEMOAPI_MAX_NAME			256
#define DEMOAPI_PREFIX_LEN			32
#define DEMOAPI_MAX_EARLYTIMES		2048
#define DEMOAPI_MAX_LEVELSTARTS		65536

// one per gamestate record in the file
typedef struct demoApiGamestate_s {
	int		fileOffset;
	int		prevStamp;			// newest snapshot time before this record, 0 none
	int		levelStartTime;
	int		haveLevelStartTime;
	int		numSnapshots;
	int		firstTime;
	int		lastTime;
	int		settleTime;			// first snapshot after the server clock restarted; firstTime if it never did
	int		settleOffset;		// record offset of that snapshot
	int		firstLevelStart;	// into the level start list
	int		numLevelStarts;
	char	mapName[DEMOAPI_MAX_MAPNAME];
} demoApiGamestate_t;

// a CS_LEVEL_START_TIME value change, in file order
typedef struct demoApiLevelStart_s {
	int		value;
	int		recordOffset;
	int		stampTime;			// newest snapshot time before this record, 0 none
	int		nextTime;			// first snapshot at or after this record under the same gamestate, 0 none
	int		nextOffset;			// its record offset
} demoApiLevelStart_t;

// CG_DEMO_GETINFO
typedef struct demoApiInfo_s {
	int		indexDone;
	int		firstServerTime;
	int		lastServerTime;
	int		numGamestates;
	int		loadedGamestate;
	int		resumeGamestate;	// -1 on a fresh open
	int		resumeServerTime;
	int		numLevelStarts;		// CG_DEMO_LEVELSTARTS
	int		numEarlyTimes;
	int		fileBytes;
	char	name[DEMOAPI_MAX_NAME];
	int		earlyTimes[DEMOAPI_MAX_EARLYTIMES];		// the file's first snapshot times
	demoApiGamestate_t	gamestates[DEMOAPI_MAX_GAMESTATES];
} demoApiInfo_t;

// CG_DEMO_ENSURE flags
#define DEMOENSURE_EOF				0x0001
#define DEMOENSURE_AT_START			0x0002
#define DEMOENSURE_SEGMENT_END		0x0004
#define DEMOENSURE_LEVEL_CHANGE		0x0008
#define DEMOENSURE_BUSY				0x0010

typedef struct demoApiEnsure_s {
	int		gamestate;			// -1: the loaded one
	int		wantFrom;
	int		wantTo;
	int		budgetUsec;			// 0: no limit
	int		playheadMs;
	int		cacheFrom;			// out
	int		cacheTo;			// out
	int		decoderTime;		// out
	int		flags;				// out
	int		levelChangeGamestate;	// out
	int		levelChangeTarget;	// out
} demoApiEnsure_t;

// CG_DEMO_FINDSNAPSHOT modes
#define DEMOSNAP_AT_OR_BEFORE		0
#define DEMOSNAP_AFTER				1
#define DEMOSNAP_KEYFRAME_AT_OR_BEFORE	2	// the newest seek index point at or before; -1 if none

// CG_DEMO_COMMANDS modes
#define DEMOCMD_FORWARD				0
#define DEMOCMD_RESYNC				1
#define DEMOCMD_SCAN				2
#define DEMOCMD_BASE				3	// the gamestate's own configstrings, as "cs <index> <value>"

typedef struct demoApiCmdCursor_s {
	int		mode;
	int		gamestate;			// SCAN, BASE: -1 the loaded one
	int		fromTime;
	int		toTime;
	char	prefix[DEMOAPI_PREFIX_LEN];
	int		cursor;
	int		state;
	int		cmdTime;			// out
} demoApiCmdCursor_t;

// CG_DEMO_EVENTS: the entity events the index pass logged (cl_demoIndexEvents names which), by time
#define DEMOAPI_MAX_INDEX_EVENTS	16

typedef struct demoApiEventCursor_s {
	int		gamestate;			// -1: the loaded one
	int		fromTime;
	int		toTime;
	int		event;				// -1: any logged event
	int		entityNum;			// -1: any entity
	int		newestFirst;		// walk from toTime down, else from fromTime up
	int		cursor;
	int		state;
	int		evTime;				// out: the snapshot the state was taken from
	int		evEvent;			// out
} demoApiEventCursor_t;

// CG_DEMO_FRAMECLOCK
#define DEMOCLOCK_VIDEO				0x1
#define DEMOCLOCK_TIMEDEMO			0x2
#define DEMOCLOCK_FIXED				0x4

typedef struct demoApiClock_s {
	float	frameMsec;			// this frame, before timescale
	int		frameNum;
	int		flags;
} demoApiClock_t;
