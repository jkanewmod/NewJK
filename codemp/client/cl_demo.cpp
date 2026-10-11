/*
===========================================================================
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

#include "client.h"
#include "cl_cgameapi.h"
#include "cl_uiapi.h"
#include "qcommon/stringed_ingame.h"
#include "cl_demo_local.h"
#ifdef DEMO_DEBUG
#include "cgame/cg_demoscan.h"
#include <SDL_timer.h>
#endif

qboolean demoApiAttached;		// cgame has taken the playhead through cg_demoapi.h
int		demoApiPlayhead;		// where cgame last said the viewer is

// the live parser reached the end of the file: only classic playback ends the demo on that; a demo
// a game module drives holds its last frame (CL_DemoApiSetClientTime finishes a timedemo or a chain)
void CL_DemoCompleted( void ) {
	DemoDbg( "DEMO COMPLETED state=%d demoplaying=%d demofile=%d snapTime=%d",
		(int)cls.state, (int)clc.demoplaying, (int)(clc.demofile != 0), cl.snap.serverTime );

	if ( demoApiAttached || demoApiServiceDemo ) {
		return;
	}

	CL_DemoFinish();
}

void CL_DemoFinish( void ) {
	DemoDbg( "disconnecting to main menu" );

	CL_DemoCacheFree();
	CL_DemoKeyframesFree();
	if (cl_timedemo && cl_timedemo->integer) {
		int	time;

		time = Sys_Milliseconds() - clc.timeDemoStart;
		if ( time > 0 ) {
			Com_Printf ("%i frames, %3.1f seconds: %3.1f fps\n", clc.timeDemoFrames,
			time/1000.0, clc.timeDemoFrames*1000.0 / time);
		}
	}

	CL_Disconnect_f();
	S_StopAllSounds();
	UIVM_SetActiveMenu( UIMENU_MAIN );

	CL_NextDemo();
}

void CL_ReadDemoMessage( void ) {
	int			r;
	msg_t		buf;
	byte		bufData[ MAX_MSGLEN ];
	int			s;
	int			pendingSequence;

	if ( !clc.demofile ) {
		CL_DemoCompleted ();
		return;
	}

	// past a svc_mapchange nothing belongs to the level until the next gamestate
	{
		int gs = CL_DemoGamestateIndexOf( CL_DemoStreamSegment() ), pos = FS_FTell( clc.demofile );

		if ( gs >= 0 && demoRaw.endOffset[gs] && pos >= demoRaw.endOffset[gs] && CL_DemoGamestateIndexOf( pos ) < 0 ) {
			if ( gs + 1 < demoRaw.numGs ) {
				FS_Seek( clc.demofile, demoRaw.gs[gs + 1].fileOffset, FS_SEEK_SET );
			} else {
				CL_DemoCompleted();
				return;
			}
		}
	}

	// get the sequence number
	r = FS_Read( &s, 4, clc.demofile);
	if ( r != 4 ) {
		CL_DemoCompleted ();
		return;
	}

	pendingSequence = LittleLong( s );

	CL_DemoSetParsingSegment( FS_FTell( clc.demofile ) - 4 );

	// init the message
	MSG_Init( &buf, bufData, sizeof( bufData ) );

	// get the length
	r = FS_Read (&buf.cursize, 4, clc.demofile);
	if ( r != 4 ) {
		CL_DemoCompleted ();
		return;
	}
	buf.cursize = LittleLong( buf.cursize );
	if ( buf.cursize == -1 ) {
		CL_DemoCompleted ();
		return;
	}
	if ( buf.cursize < 0 || buf.cursize > buf.maxsize ) {
		Com_Printf( S_COLOR_YELLOW "Demo record out of sync at offset %d (length %d); recovering.\n",
			(int)FS_FTell( clc.demofile ), buf.cursize );
		CL_DemoCompleted();
		return;
	}
	r = FS_Read( buf.data, buf.cursize, clc.demofile );
	if ( r != buf.cursize ) {
#ifdef _DEBUG
		Com_Printf( "Demo file was truncated.\n");
#endif
		CL_DemoCompleted ();
		return;
	}

	// the record is complete, so the sequence it carries is real
	clc.serverMessageSequence = pendingSequence;

	clc.lastPacketTime = cls.realtime;
	buf.readcount = 0;
	CL_ParseServerMessage( &buf );
}

#define DEMOSCAN_PEEK_BYTES		(1024)

demoRaw_t	demoRaw;

static qboolean CL_ScanReadGamestateInfo( msg_t *buf, char *outMap, int outMapSize, int *outLevelStart, int *outHaveLevelStart, int *outCmdSeq ) {
	qboolean	haveMap = qfalse;

	*outLevelStart = 0;
	*outHaveLevelStart = 0;
	outMap[0] = '\0';

	*outCmdSeq = MSG_ReadLong( buf );

	while ( buf->readcount <= buf->cursize ) {
		int cmd = MSG_ReadByte( buf );

		if ( cmd == svc_configstring ) {
			int			index = MSG_ReadShort( buf );
			const char	*value = MSG_ReadBigString( buf );

			if ( buf->readcount > buf->cursize ) {
				break;
			}
			if ( index == CS_SERVERINFO ) {
				Q_strncpyz( outMap, Info_ValueForKey( value, "mapname" ), outMapSize );
				haveMap = qtrue;
			} else if ( index == CS_LEVEL_START_TIME ) {
				*outLevelStart = atoi( value );
				*outHaveLevelStart = 1;
			}
			if ( haveMap && *outHaveLevelStart ) {
				break;
			}
			continue;
		}
		break;
	}
	return haveMap;
}

static qboolean CL_ScanDemoFile( const char *demoPath, demoRaw_t *r ) {
	fileHandle_t	f = 0;
	static byte		bufData[MAX_MSGLEN];

	DR_Free( r );
	DR_Init( r );

	FS_FOpenFileRead( demoPath, &f, qtrue );
	if ( !f ) {
		return qfalse;
	}

	while ( 1 ) {
		int			seq, len, peek, cmd, serverTime = 0;
		msg_t		buf;
		qboolean	gotTime = qfalse;
		int			recordStart = FS_FTell( f );

		if ( FS_Read( &seq, 4, f ) != 4 || FS_Read( &len, 4, f ) != 4 ) {
			break;
		}
		len = LittleLong( len );
		if ( len == -1 || len <= 0 || len > MAX_MSGLEN ) {
			break;
		}
		peek = ( len > DEMOSCAN_PEEK_BYTES ) ? len : DEMOSCAN_PEEK_BYTES;
		if ( peek > len ) {
			peek = len;
		}
		if ( FS_Read( bufData, peek, f ) != peek ) {
			break;
		}
		if ( len > peek && FS_Seek( f, len - peek, FS_SEEK_CUR ) ) {
			break;
		}

		MSG_Init( &buf, bufData, sizeof( bufData ) );
		buf.cursize = peek;
		buf.readcount = 0;
		MSG_Bitstream( &buf );
		MSG_ReadLong( &buf );

		while ( buf.readcount <= buf.cursize ) {
			cmd = MSG_ReadByte( &buf );
			if ( cmd == svc_nop ) {
				continue;
			}
			if ( cmd == svc_serverCommand ) {
				int			cmdSeq = MSG_ReadLong( &buf );
				const char	*sc = MSG_ReadString( &buf );

				if ( buf.readcount <= buf.cursize && sc ) {
					DR_Command( r, cmdSeq, sc, recordStart );
				}
				continue;
			}
			if ( cmd == svc_mapchange ) {
				DR_MapChange( r, recordStart );
				continue;
			}
			if ( cmd == svc_gamestate ) {
				char	mapName[MAX_QPATH];
				int		levelStart, haveLevelStart, cmdSeq;

				CL_ScanReadGamestateInfo( &buf, mapName, sizeof( mapName ), &levelStart, &haveLevelStart, &cmdSeq );
				DR_Gamestate( r, recordStart, mapName, levelStart, haveLevelStart, cmdSeq );
				break;
			}
			if ( cmd == svc_snapshot ) {
				serverTime = MSG_ReadLong( &buf );
				gotTime = ( buf.readcount <= buf.cursize ) ? qtrue : qfalse;
			}
			break;
		}

		if ( gotTime ) {
			DR_Snapshot( r, serverTime, recordStart );
		}
	}

	FS_FCloseFile( f );
	return (qboolean)( r->numSnaps > 0 && r->numGs > 0 );
}

int CL_DemoGamestateIndexOf( int fileOffset ) {
	return DR_GamestateIndex( &demoRaw, fileOffset );
}

// the next record belongs to a later gamestate than the live parser's: that gamestate's own record,
// or the tail past a svc_mapchange that CL_ReadDemoMessage would skip into it
qboolean CL_DemoNextRecordLeavesSegment( void ) {
	int pos = FS_FTell( clc.demofile ), gs;

	if ( CL_DemoGamestateIndexOf( pos ) >= 0 ) {
		return (qboolean)( pos != CL_DemoStreamSegment() );
	}
	gs = CL_DemoGamestateIndexOf( CL_DemoStreamSegment() );
	return (qboolean)( gs >= 0 && gs + 1 < demoRaw.numGs && demoRaw.endOffset[gs] && pos >= demoRaw.endOffset[gs] );
}

int CL_DemoLoadedGamestate( void ) {
	int gs = CL_DemoGamestateIndexOf( CL_DemoLoadedSegment() );

	return gs >= 0 ? gs : 0;
}

#ifdef DEMO_DEBUG
void DemoDbg( const char *fmt, ... ) {
	va_list	argptr;
	char	text[1024];

	if ( !cl_demoTimeDebug || !cl_demoTimeDebug->integer ) {
		return;
	}

	va_start( argptr, fmt );
	Q_vsnprintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	Com_Printf( "[seek] %s\n", text );
}
#endif

int demoApiResumeGamestate = -1;	// a level-change seek's target, for the game module that comes up after the load
int demoApiResumeTarget = -1;

// reposition for (gamestate, time) and decode up to it, performing the level load on the way
static void CL_DemoSeekToServerTime( int gamestate, int targetServer ) {
	const demoApiGamestate_t	*g;
	int							guard = 0;

	if ( gamestate < 0 || gamestate >= demoRaw.numGs ) {
		gamestate = CL_DemoLoadedGamestate();
	}
	g = &demoRaw.gs[gamestate];
	if ( targetServer < g->settleTime ) {
		targetServer = g->settleTime;
	}
	demoApiResumeGamestate = gamestate;
	demoApiResumeTarget = targetServer;

	// the live parser reads the gamestate record, which loads the map (or finds it loaded)
	FS_Seek( clc.demofile, g->fileOffset, FS_SEEK_SET );
	clc.demoRewinding = qtrue;
	cl.snap.valid = qfalse;
	CL_ReadDemoMessage();
	clc.demoRewinding = qfalse;

	if ( demoApiServiceDemo && demoFwd ) {
		// a game module drives the demo: its decoder reaches the target, with the map's keyframes
		Demo_Reposition( demoFwd, gamestate, targetServer );
		while ( clc.demoplaying && !demoFwd->eof ) {
			if ( demoFwd->snap.valid && !demoFwd->kfRestoring && demoFwd->gamestate == gamestate
				&& demoFwd->recordOffset >= g->settleOffset && demoFwd->snap.serverTime >= targetServer ) {
				break;
			}
			if ( Demo_NextRecordLeavesSegment( demoFwd ) || ++guard > MAX_DEMO_TIME_MESSAGES ) {
				break;
			}
			if ( !Demo_ReadRecord( demoFwd ) ) {
				if ( demoFwd->fault && demoFwd->faultCount <= 3 ) {
					Demo_Reposition( demoFwd, gamestate, targetServer );
					continue;
				}
				break;
			}
			if ( demoFwd->kfRestoreFailed ) {
				demoFwd->kfRestoreFailed = qfalse;
				Demo_Reposition( demoFwd, gamestate, targetServer );
			}
		}
		clc.demoTimescaleRemainder = 0.0f;
		return;
	}

	// classic playback: the live parser itself decodes up to the target, running the commands on
	// the way so the stock module's configstrings keep up
	while ( clc.demoplaying && clc.demofile ) {
		if ( cl.snap.valid && CL_DemoStreamSegment() == g->fileOffset
			&& CL_DemoParsingSegment() >= g->settleOffset && cl.snap.serverTime >= targetServer ) {
			break;
		}
		if ( CL_DemoStreamSegment() == g->fileOffset && CL_DemoNextRecordLeavesSegment() ) {
			break;
		}
		if ( ++guard > MAX_DEMO_TIME_MESSAGES ) {
			break;
		}
		CL_ReadDemoMessage();
		{
			int oldest = clc.serverCommandSequence - MAX_RELIABLE_COMMANDS + 1;

			if ( clc.lastExecutedServerCommand < oldest ) {
				clc.lastExecutedServerCommand = oldest - 1;
			}
			while ( clc.lastExecutedServerCommand < clc.serverCommandSequence ) {
				int next = clc.lastExecutedServerCommand + 1;

				CL_GetServerCommand( next );
				if ( clc.lastExecutedServerCommand < next ) {
					clc.lastExecutedServerCommand = next;
				}
			}
		}
	}
	clc.demoTimescaleRemainder = 0.0f;
}

#ifdef DEMO_DEBUG
static void CL_DemoGoldenPrintRaw( const char *side, const demoApiGamestate_t *gs, int numGs, const demoApiLevelStart_t *ls, int numLs, int numEarly ) {
	int i;

	Com_Printf( "[golden] %s gamestates=%d levelstarts=%d early=%d\n", side, numGs, numLs, numEarly );
	for ( i = 0; i < numGs; i++ ) {
		const demoApiGamestate_t *g = &gs[i];

		Com_Printf( "[golden] %s gs=%d offset=%d prev=%d lst=%d have=%d snaps=%d first=%d last=%d settle=%d settleOffset=%d ls=%d+%d map=%s\n",
			side, i, g->fileOffset, g->prevStamp, g->levelStartTime, g->haveLevelStartTime, g->numSnapshots, g->firstTime,
			g->lastTime, g->settleTime, g->settleOffset, g->firstLevelStart, g->numLevelStarts, g->mapName[0] ? g->mapName : "-" );
	}
	for ( i = 0; i < numLs; i++ ) {
		Com_Printf( "[golden] %s ls=%d value=%d rec=%d stamp=%d next=%d nextOffset=%d\n", side, i, ls[i].value, ls[i].recordOffset,
			ls[i].stampTime, ls[i].nextTime, ls[i].nextOffset );
	}
}

void CL_DemoRawGolden_f( void ) {
	static byte				blob[DEMOSCAN_MAX_BLOB];
	static demoRaw_t		r;
	char					*list = NULL;
	char					*p, *line;
	int						n = 0, bad = 0;

	if ( Cmd_Argc() < 2 || clc.demoplaying ) {
		Com_Printf( "usage: demoRawGolden <listfile> (not while a demo plays)\n" );
		return;
	}
	if ( FS_ReadFile( Cmd_Argv( 1 ), (void **)&list ) <= 0 || !list ) {
		Com_Printf( "demoRawGolden: cannot read %s\n", Cmd_Argv( 1 ) );
		return;
	}

	for ( p = list; *p; ) {
		demoScanRequest_t			req;
		demoScanResultHeader_t		hdr;
		const demoApiGamestate_t	*sgs = NULL;
		const demoApiLevelStart_t	*sls = NULL;
		const int					*searly = NULL;
		int							i, id, got, bytes, nsgs = 0, nsls = 0, nse = 0, match;

		line = p;
		while ( *p && *p != '\n' && *p != '\r' ) {
			p++;
		}
		while ( *p == '\n' || *p == '\r' ) {
			*p++ = '\0';
		}
		if ( !line[0] ) {
			continue;
		}
		n++;

		CL_ScanDemoFile( va( "demos/%s", line ), &r );
		Com_Printf( "[golden] file=%s\n", line );
		CL_DemoGoldenPrintRaw( "open", r.gs, r.numGs, r.ls, r.numLs, r.numEarly );
		{
			int k;

			for ( k = 0; k < r.numEarly; k += 200 ) {
				int j;
				char line[2400];

				line[0] = '\0';
				for ( j = k; j < r.numEarly && j < k + 200; j++ ) {
					Q_strcat( line, sizeof( line ), va( " %d", r.early[j] ) );
				}
				Com_Printf( "[golden] early%s\n", line );
			}
		}

		Com_Memset( &req, 0, sizeof( req ) );
		req.mode = DEMOSCAN_MODE_FULL;
		req.priority = DEMOSCAN_PRIO_SELECTED;
		req.flags = DEMOSCAN_REQ_EARLYTIMES;
		id = CL_DemoScan_Submit( line, &req );
		got = id ? CL_DemoScan_WaitJob( id, &hdr, 12000 ) : 0;
		if ( !got ) {
			Com_Printf( "[golden] scan failed\n" );
			bad++;
			continue;
		}
		bytes = CL_DemoScan_Fetch( hdr.jobId, blob, sizeof( blob ) );
		for ( i = 0; i + (int)sizeof( demoScanRecord_t ) <= bytes; ) {
			const demoScanRecord_t *rec = (const demoScanRecord_t *)( blob + i );

			if ( rec->type == DSR_GAMESTATES ) {
				sgs = (const demoApiGamestate_t *)( rec + 1 );
				nsgs = rec->index;
			} else if ( rec->type == DSR_LEVELSTARTS ) {
				sls = (const demoApiLevelStart_t *)( rec + 1 );
				nsls = rec->index;
			} else if ( rec->type == DSR_EARLYTIMES ) {
				searly = (const int *)( rec + 1 );
				nse = rec->index;
			}
			i += DEMOSCAN_RECORD_SIZE( rec->len );
		}
		match = ( nsgs == r.numGs && nsls == r.numLs && nse == r.numEarly
			&& ( !nsgs || !memcmp( sgs, r.gs, nsgs * sizeof( *sgs ) ) )
			&& ( !nsls || !memcmp( sls, r.ls, nsls * sizeof( *sls ) ) )
			&& ( !nse || !memcmp( searly, r.early, nse * sizeof( int ) ) ) );
		if ( !match ) {
			bad++;
			CL_DemoGoldenPrintRaw( "scan", sgs, nsgs, sls, nsls, nse );
		}
		Com_Printf( "[golden] result match=%d status=%d blob=%d records=%d\n", match, hdr.status, bytes, hdr.numRecords );
	}
	FS_FreeFile( list );
	DR_Free( &r );
	Com_Printf( "[golden] done files=%d mismatched=%d\n", n, bad );
}
#endif

void CL_DemoSeekServer_f( void ) {
	int target, gamestate;

	if ( !clc.demoplaying ) {
		Com_Printf( "Only available during demo playback.\n" );
		return;
	}
	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "Usage: demoSeekServer [gamestate] <serverTime>\n" );
		return;
	}
	if ( Cmd_Argc() >= 3 ) {
		gamestate = atoi( Cmd_Argv( 1 ) );
		target = atoi( Cmd_Argv( 2 ) );
	} else {
		gamestate = CL_DemoLoadedGamestate();
		target = atoi( Cmd_Argv( 1 ) );
	}
	if ( demoApiAttached && ( gamestate < 0 || gamestate >= demoRaw.numGs || gamestate == CL_DemoLoadedGamestate() ) ) {
		Com_Printf( "The game module drives this demo; seek with its controls.\n" );
		return;
	}
	demoApiAttached = qfalse;
	CL_DemoSeekToServerTime( gamestate, target );

	// the classic clock resumes from the landing until a game module takes the playhead again
	if ( cl.snap.valid ) {
		cl.oldFrameServerTime = cl.snap.serverTime;
		cl.serverTime = cl.oldServerTime = cl.snap.serverTime;
		cl.serverTimeDelta = cl.snap.serverTime - cls.realtime;
	}
}

void CL_PlayDemo_f( void ) {
	char		name[MAX_OSPATH], extension[32];
	char		*arg;
	int			demoFileLen = 0;

	Cvar_Set("demoname", "");

	if (Cmd_Argc() < 2) {
		Com_Printf ("demo <demoname>\n");
		return;
	}

	// open the demo file
	arg = Cmd_Args();

	// try exact string typed
	{
		char rawArg[MAX_OSPATH] = { 0 };
		char *cmd = Cmd_Cmd();
		char *p = Com_SkipTokens(cmd, 1, " \t"); // skip first token ("demo")
		while (*p && isspace((unsigned char)*p)) {
			p++;
		}
		Q_strncpyz(rawArg, p, sizeof(rawArg));

		if (rawArg[0]) {
			Com_sprintf(extension, sizeof(extension), ".dm_%d", PROTOCOL_LEGACY);
			if (!Q_stricmp(rawArg + strlen(rawArg) - strlen(extension), extension)) {
				Com_sprintf(name, sizeof(name), "demos/%s", rawArg);
			}
			else {
				Com_sprintf(name, sizeof(name), "demos/%s.dm_%d", rawArg, PROTOCOL_LEGACY);
			}

			if (!FS_FileExists(name)) {
				Com_sprintf(extension, sizeof(extension), ".dm_%d", PROTOCOL_VERSION);
				if (!Q_stricmp(rawArg + strlen(rawArg) - strlen(extension), extension)) {
					Com_sprintf(name, sizeof(name), "demos/%s", rawArg);
				}
				else {
					Com_sprintf(name, sizeof(name), "demos/%s.dm_%d", rawArg, PROTOCOL_VERSION);
				}
			}

			if (!FS_FileExists(name)) {
				Com_sprintf(name, sizeof(name), "demos/%s", rawArg);
			}

			// test if this file actually exists
			{
				fileHandle_t f;
				FS_FOpenFileRead(name, &f, qtrue);
				if (f) {
					FS_FCloseFile(f);
					arg = rawArg; // only use rawArg if it definitely exists
				}
			}
		}
	}

	//could probably be alot cleaner
	//look for the old protocol first
	Com_sprintf(extension, sizeof(extension), ".dm_%d", PROTOCOL_LEGACY);
	if (!Q_stricmp(arg + strlen(arg) - strlen(extension), extension)) {
		Com_sprintf(name, sizeof(name), "demos/%s", arg);
	}
	else {
		Com_sprintf(name, sizeof(name), "demos/%s.dm_%d", arg, PROTOCOL_LEGACY);
	}

	if (!FS_FileExists(name)) {//start over w/ normal protocol idk
		Com_sprintf(extension, sizeof(extension), ".dm_%d", PROTOCOL_VERSION);
		if (!Q_stricmp(arg + strlen(arg) - strlen(extension), extension)) {
			Com_sprintf(name, sizeof(name), "demos/%s", arg);
		}
		else {
			Com_sprintf(name, sizeof(name), "demos/%s.dm_%d", arg, PROTOCOL_VERSION);
		}
	}

	if (!FS_FileExists(name)) { //couldn't find a file with either extension?
		Com_sprintf(name, sizeof(name), "demos/%s", arg);
	}

	// don't overwrite clc.demofile unless we're sure it's valid
	{
		fileHandle_t f;
		FS_FOpenFileRead(name, &f, qtrue);
		if (!f) {
			char errorMsg[1024];
			if (!Q_stricmp(arg, "(null)"))
				Com_sprintf(errorMsg, sizeof(errorMsg), "%s\n", SE_GetString("CON_TEXT_NO_DEMO_SELECTED"));
			else
				Com_sprintf(errorMsg, sizeof(errorMsg), "Unable to open %s.\n", name);

			Com_Printf(errorMsg);
			return;
		}
		FS_FCloseFile(f);
	}

	// make sure a local server is killed
	// 2 means don't force disconnect of local client
	Cvar_Set("sv_killserver", "2");

	CL_Disconnect(qtrue);

	demoFileLen = FS_FOpenFileRead( name, &clc.demofile, qtrue );
	if (!clc.demofile) {
		// theoretically shouldn't happen since we tested above,
		// but just in case
		char errorMsg[1024];
		if (!Q_stricmp(arg, "(null)"))
			Com_sprintf(errorMsg, sizeof(errorMsg), "%s\n", SE_GetString("CON_TEXT_NO_DEMO_SELECTED"));
		else
			Com_sprintf(errorMsg, sizeof(errorMsg), "Unable to open %s.\n", name);

		Com_Printf(errorMsg);
		return;
	}

	Q_strncpyz( clc.demoName, arg, sizeof( clc.demoName ) );
	Cvar_Set("demoname", arg);

	Con_Close();

	cls.state = CA_CONNECTED;
	clc.demoplaying = qtrue;
	Cvar_Set("cl_demoPlaying", "1");

	Q_strncpyz( cls.servername, arg, sizeof( cls.servername ) );

	CL_DemoKeyframesFree();
	CL_DemoApiReset();
	CL_DemoCmdLogSetFileSize( demoFileLen );

	if ( !CL_ScanDemoFile( name, &demoRaw ) ) {
		DR_Free( &demoRaw );
	}
	if ( demoRaw.capped ) {
		Com_Printf( "Demo has more than %d map loads; playback ends at the last one indexed\n", DEMOAPI_MAX_GAMESTATES );
	}
	DemoDbg( "OPEN \"%s\" gamestates=%d levelstarts=%d snaps=%d first=%d last=%d", name,
		demoRaw.numGs, demoRaw.numLs, demoRaw.numSnaps, demoRaw.firstTime, demoRaw.lastTime );

	// the cache and the forward decoder exist before anything is decoded, so the game module's first
	// frames (which run during the load itself) find the service ready rather than absent
	CL_DemoCacheAlloc();
	CL_DemoApiOpen( name );

	CL_DemoPrescanIndex( name );

	// read demo messages until connected
	while ( cls.state >= CA_CONNECTED && cls.state < CA_PRIMED ) {
		CL_ReadDemoMessage();
	}
	// the loading screen is up: the whole-file index and command log, so seeks and the seek bar
	// are complete from the first frame
	CL_DemoPrescanAtLoad();
	// don't get the first snapshot this frame, to prevent the long
	// time from the gamestate load from messing causing a time skip
	clc.firstDemoFrameSkipped = qfalse;
	clc.demoTimescaleRemainder = 0.0f;

}

void CL_DemoRestart_f(void) {
	char *demoname = Cvar_VariableString("demoname");
	if (!VALIDSTRING(demoname)) {
		Com_Printf("No demo available to restart.\n");
		return;
	}

	Com_Printf("Restarting demo \"%s\".\n", demoname);
	Cbuf_ExecuteText(EXEC_APPEND, va("demo \"%s\"\n", demoname));
}
