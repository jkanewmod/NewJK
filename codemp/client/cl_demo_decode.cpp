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

// cl_demo_decode.cpp -- a demo decoder that owns all of its state: the same parse as cl_parse.cpp,
// over a demoDecoder_t instead of cl and clc, with the same MSG_ readers underneath. It has no side
// effects beyond its own state, the command log and the hooks in cl_demo_cache.cpp, so any number of
// them can read one file, and one can run on a thread.

#include "client.h"
#include "cl_demo_local.h"
#include "cl_demo_decode.h"

static int	demoDecNumEntityFields;
static int	demoDecNumPlayerFields[3];

static void Demo_FieldCounts( void ) {
	static int bits[256];

	if ( demoDecNumEntityFields ) {
		return;
	}
	demoDecNumEntityFields = MSG_EntityFieldLayout( bits, (int)ARRAY_LEN( bits ), NULL );
	demoDecNumPlayerFields[0] = MSG_PlayerFieldLayout( 0, bits, (int)ARRAY_LEN( bits ), NULL, NULL );
	demoDecNumPlayerFields[1] = MSG_PlayerFieldLayout( 1, bits, (int)ARRAY_LEN( bits ), NULL, NULL );
	demoDecNumPlayerFields[2] = MSG_PlayerFieldLayout( 2, bits, (int)ARRAY_LEN( bits ), NULL, NULL );
}

demoDecoder_t *Demo_DecoderAlloc( int role ) {
	demoDecoder_t *d = (demoDecoder_t *)calloc( 1, sizeof( demoDecoder_t ) );

	if ( !d ) {
		return NULL;
	}
	d->parseEntities = (entityState_t *)malloc( sizeof( entityState_t ) * MAX_PARSE_ENTITIES );
	d->baselines = (entityState_t *)calloc( MAX_GENTITIES, sizeof( entityState_t ) );
	d->recordBuf = (byte *)calloc( 1, MAX_MSGLEN + DEMODEC_RECORD_PAD );	// huffman reads overrun a corrupt record's end
	if ( !d->parseEntities || !d->baselines || !d->recordBuf ) {
		Demo_DecoderFree( d );
		return NULL;
	}
	d->role = role;
	d->streamSegment = -1;
	d->gamestate = -1;
	d->fileOffset = 0;
	d->recordOffset = -1;
	d->kfRestoredOffset = -1;
	d->faultOffset = -1;
	Demo_FieldCounts();
	return d;
}

void Demo_DecoderFree( demoDecoder_t *d ) {
	if ( !d ) {
		return;
	}
	CL_DemoKeyframePendingDrop( d );
	if ( d->file ) {
		FS_FCloseFile( d->file );
	}
	if ( d->stdio ) {
		fclose( d->stdio );
	}
	if ( d->parseEntities ) {
		free( d->parseEntities );
	}
	if ( d->baselines ) {
		free( d->baselines );
	}
	if ( d->recordBuf ) {
		free( d->recordBuf );
	}
	free( d );
}

qboolean Demo_DecoderOpen( demoDecoder_t *d, const char *demoPath ) {
	if ( d->file ) {
		FS_FCloseFile( d->file );
		d->file = 0;
	}
	FS_FOpenFileRead( demoPath, &d->file, qtrue );
	d->filePos = 0;
	return (qboolean)( d->file != 0 );
}

// empty decoder state at a record boundary; the file position follows on the next read
void Demo_DecoderReset( demoDecoder_t *d, int fileOffset ) {
	int k;

	for ( k = 0; k < PACKET_BACKUP; k++ ) {
		d->snapshots[k].valid = qfalse;
	}
	Com_Memset( &d->snap, 0, sizeof( d->snap ) );
	d->parseEntitiesNum = 0;
	Com_Memset( &d->gameState, 0, sizeof( d->gameState ) );
	d->serverMessageSequence = 0;
	d->serverCommandSequence = 0;
	d->bigConfigString[0] = '\0';
	d->numRecordCmds = 0;
	d->cmdPoolUsed = 0;
	d->fileOffset = fileOffset;
	d->recordOffset = -1;
	d->streamSegment = -1;
	d->gamestate = -1;
	d->eof = qfalse;
	d->fault = qfalse;
	d->kfRestoring = qfalse;
	d->kfRestoreFailed = qfalse;
	d->kfRestoredOffset = -1;
	CL_DemoKeyframePendingDrop( d );
	Com_Memset( d->evLastKey, 0, sizeof( d->evLastKey ) );
	d->evLastGamestate = -1;
	d->evPrevMessage = -1;
	d->bigConfigString[0] = '\0';
}

/*
===================
File access: a handle of its own per decoder, or stdio on a thread
===================
*/
static qboolean Demo_FileRead( demoDecoder_t *d, void *buf, int len ) {
	if ( d->stdio ) {
		return (qboolean)( fread( buf, 1, len, d->stdio ) == (size_t)len );
	}
	return (qboolean)( FS_Read( buf, len, d->file ) == len );
}

static void Demo_FileSeek( demoDecoder_t *d, int offset ) {
	if ( d->filePos == offset ) {
		return;
	}
	if ( d->stdio ) {
		fseek( d->stdio, offset, SEEK_SET );
	} else {
		FS_Seek( d->file, offset, FS_SEEK_SET );
	}
	d->filePos = offset;
}

/*
===================
Strings: MSG_ReadString answers from one static buffer, which a second reader could share
===================
*/
static void Demo_ReadString( msg_t *msg, char *out, int outSize ) {
	int l = 0, c;

	do {
		c = MSG_ReadByte( msg );		// -1 at the end
		if ( c == -1 || c == 0 ) {
			break;
		}
		if ( c == '%' ) {
			c = '.';					// as MSG_ReadString: a stray format character
		}
		if ( l < outSize - 1 ) {
			out[l++] = (char)c;
		}
	} while ( 1 );
	out[l] = '\0';
}

// the Nth whitespace separated token as Cmd_TokenizeString cuts it, for the few shapes read here
static void Demo_Token( const char *text, int n, char *out, int outSize ) {
	int i;

	out[0] = '\0';
	for ( i = 0; i <= n; i++ ) {
		int l = 0;

		while ( *text && (unsigned char)*text <= ' ' ) {
			text++;
		}
		if ( !*text ) {
			return;
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
}

/*
===================
Configstring tables
===================
*/
void Demo_GsSet( gameState_t *gs, int index, const char *value ) {
	gameState_t	old;
	int			i, len;
	const char	*dup;

	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		return;
	}
	if ( !strcmp( gs->stringData + gs->stringOffsets[index], value ) ) {
		return;
	}
	// in place when it fits; offset 0 is the shared empty string
	if ( gs->stringOffsets[index] && strlen( value ) <= strlen( gs->stringData + gs->stringOffsets[index] ) ) {
		strcpy( gs->stringData + gs->stringOffsets[index], value );
		return;
	}

	old = *gs;
	Com_Memset( gs, 0, sizeof( *gs ) );
	gs->dataCount = 1;

	for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
		dup = ( i == index ) ? value : old.stringData + old.stringOffsets[i];
		if ( !dup[0] ) {
			continue;
		}
		len = (int)strlen( dup );
		if ( len + 1 + gs->dataCount > MAX_GAMESTATE_CHARS ) {
			*gs = old;
			return;
		}
		gs->stringOffsets[i] = gs->dataCount;
		Com_Memcpy( gs->stringData + gs->dataCount, dup, len + 1 );
		gs->dataCount += len + 1;
	}
}

/*
===================
Pre-checks: the delta readers Com_Error on two data-driven conditions. Peek the field count and
restore the read position, so a corrupt record is a fault here, never a drop
===================
*/
static qboolean Demo_EntityFits( msg_t *msg ) {
	int			readcount = msg->readcount, bit = msg->bit;
	qboolean	ok = qtrue;

	if ( MSG_ReadBits( msg, 1 ) != 1 ) {				// not a remove
		if ( MSG_ReadBits( msg, 1 ) != 0 ) {			// a delta: the field count follows
			int lc = MSG_ReadByte( msg );

			ok = (qboolean)( lc >= 0 && lc <= demoDecNumEntityFields );
		}
	}
	msg->readcount = readcount;
	msg->bit = bit;
	return ok;
}

static qboolean Demo_PlayerstateFits( msg_t *msg, qboolean isVehiclePS ) {
	int			readcount = msg->readcount, bit = msg->bit;
	int			table = 2, lc;

	if ( !isVehiclePS ) {
		table = MSG_ReadBits( msg, 1 ) ? 1 : 0;
	}
	lc = MSG_ReadByte( msg );
	msg->readcount = readcount;
	msg->bit = bit;
	return (qboolean)( lc >= 0 && lc <= demoDecNumPlayerFields[table] );
}

/*
===================
The parse, as cl_parse.cpp does it
===================
*/
static void Demo_DeltaEntity( demoDecoder_t *d, msg_t *msg, clSnapshot_t *frame, int newnum, entityState_t *old, qboolean unchanged ) {
	entityState_t *state = &d->parseEntities[d->parseEntitiesNum & ( MAX_PARSE_ENTITIES - 1 )];

	if ( unchanged ) {
		*state = *old;
	} else {
		if ( !Demo_EntityFits( msg ) ) {
			d->fault = qtrue;
			return;
		}
		MSG_ReadDeltaEntity( msg, old, state, newnum );
	}
	if ( state->number == ( MAX_GENTITIES - 1 ) ) {
		return;		// entity was delta removed
	}
	d->parseEntitiesNum++;
	frame->numEntities++;
}

static void Demo_ParsePacketEntities( demoDecoder_t *d, msg_t *msg, clSnapshot_t *oldframe, clSnapshot_t *newframe ) {
	int				newnum, oldindex, oldnum;
	entityState_t	*oldstate;

	newframe->parseEntitiesNum = d->parseEntitiesNum;
	newframe->numEntities = 0;

	oldindex = 0;
	oldstate = NULL;
	if ( !oldframe ) {
		oldnum = 99999;
	} else {
		if ( oldindex >= oldframe->numEntities ) {
			oldnum = 99999;
		} else {
			oldstate = &d->parseEntities[( oldframe->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
			oldnum = oldstate->number;
		}
	}

	while ( 1 ) {
		newnum = MSG_ReadBits( msg, GENTITYNUM_BITS );
		if ( newnum == ( MAX_GENTITIES - 1 ) ) {
			break;
		}
		if ( msg->readcount > msg->cursize ) {
			d->fault = qtrue;
			return;
		}

		while ( oldnum < newnum ) {
			Demo_DeltaEntity( d, msg, newframe, oldnum, oldstate, qtrue );
			oldindex++;
			if ( oldindex >= oldframe->numEntities ) {
				oldnum = 99999;
			} else {
				oldstate = &d->parseEntities[( oldframe->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
				oldnum = oldstate->number;
			}
		}
		if ( oldnum == newnum ) {
			Demo_DeltaEntity( d, msg, newframe, newnum, oldstate, qfalse );
			if ( d->fault ) {
				return;
			}
			oldindex++;
			if ( oldindex >= oldframe->numEntities ) {
				oldnum = 99999;
			} else {
				oldstate = &d->parseEntities[( oldframe->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
				oldnum = oldstate->number;
			}
			continue;
		}
		if ( oldnum > newnum ) {
			Demo_DeltaEntity( d, msg, newframe, newnum, &d->baselines[newnum], qfalse );
			if ( d->fault ) {
				return;
			}
			continue;
		}
	}

	while ( oldnum != 99999 ) {
		Demo_DeltaEntity( d, msg, newframe, oldnum, oldstate, qtrue );
		oldindex++;
		if ( oldindex >= oldframe->numEntities ) {
			oldnum = 99999;
		} else {
			oldstate = &d->parseEntities[( oldframe->parseEntitiesNum + oldindex ) & ( MAX_PARSE_ENTITIES - 1 )];
			oldnum = oldstate->number;
		}
	}
}

static void Demo_ParseSnapshot( demoDecoder_t *d, msg_t *msg ) {
	clSnapshot_t	*old, newSnap;
	int				len, deltaNum, oldMessageNum;

	Com_Memset( &newSnap, 0, sizeof( newSnap ) );
	newSnap.serverCommandNum = d->serverCommandSequence;
	newSnap.serverTime = MSG_ReadLong( msg );
	newSnap.messageNum = d->serverMessageSequence;

	deltaNum = MSG_ReadByte( msg );
	if ( !deltaNum ) {
		newSnap.deltaNum = -1;
	} else {
		newSnap.deltaNum = newSnap.messageNum - deltaNum;
	}
	newSnap.snapFlags = MSG_ReadByte( msg );

	if ( newSnap.deltaNum <= 0 ) {
		newSnap.valid = qtrue;
		old = NULL;
	} else {
		old = &d->snapshots[newSnap.deltaNum & PACKET_MASK];
		if ( !old->valid ) {
			while ( ( newSnap.deltaNum & PACKET_MASK ) != ( newSnap.messageNum & PACKET_MASK ) && !old->valid ) {
				newSnap.deltaNum++;
				old = &d->snapshots[newSnap.deltaNum & PACKET_MASK];
			}
		}
		if ( !old->valid ) {
		} else if ( old->messageNum != newSnap.deltaNum ) {
		} else if ( d->parseEntitiesNum - old->parseEntitiesNum > MAX_PARSE_ENTITIES - 128 ) {
		} else {
			newSnap.valid = qtrue;
		}
	}

	len = MSG_ReadByte( msg );
	if ( (unsigned)len > sizeof( newSnap.areamask ) ) {
		d->fault = qtrue;
		return;
	}
	MSG_ReadData( msg, &newSnap.areamask, len );

	if ( !Demo_PlayerstateFits( msg, qfalse ) ) {
		d->fault = qtrue;
		return;
	}
	MSG_ReadDeltaPlayerstate( msg, old ? &old->ps : NULL, &newSnap.ps );
	if ( newSnap.ps.m_iVehicleNum ) {
		if ( !Demo_PlayerstateFits( msg, qtrue ) ) {
			d->fault = qtrue;
			return;
		}
		MSG_ReadDeltaPlayerstate( msg, old ? &old->vps : NULL, &newSnap.vps, qtrue );
	}

	Demo_ParsePacketEntities( d, msg, old, &newSnap );
	if ( d->fault || !newSnap.valid ) {
		return;
	}

	oldMessageNum = d->snap.messageNum + 1;
	if ( newSnap.messageNum - oldMessageNum >= PACKET_BACKUP ) {
		oldMessageNum = newSnap.messageNum - ( PACKET_BACKUP - 1 );
	}
	for ( ; oldMessageNum < newSnap.messageNum; oldMessageNum++ ) {
		d->snapshots[oldMessageNum & PACKET_MASK].valid = qfalse;
	}

	d->snap = newSnap;
	d->snap.ping = 999;
	d->snapshots[d->snap.messageNum & PACKET_MASK] = d->snap;
	d->snapDecoded = qtrue;
}

// the record's commands are kept until its snapshot time is known, then logged in one go
static void Demo_RecordAddCommand( demoDecoder_t *d, int seq, const char *text ) {
	int len = (int)strlen( text ) + 1;

	if ( d->numRecordCmds >= DEMODEC_MAX_RECORD_CMDS || d->cmdPoolUsed + len > DEMODEC_CMD_POOL ) {
		return;
	}
	d->recordCmdSeq[d->numRecordCmds] = seq;
	d->recordCmdText[d->numRecordCmds] = d->cmdPoolUsed;
	Com_Memcpy( d->cmdPool + d->cmdPoolUsed, text, len );
	d->cmdPoolUsed += len;
	d->numRecordCmds++;
}

static void Demo_ParseCommand( demoDecoder_t *d, msg_t *msg ) {
	char		local[MAX_STRING_CHARS], index[16], piece[MAX_STRING_CHARS];
	int			seq = MSG_ReadLong( msg );
	const char	*s;

	Demo_ReadString( msg, local, sizeof( local ) );
	if ( seq <= d->serverCommandSequence ) {
		return;				// already seen
	}
	d->serverCommandSequence = seq;
	s = local;

	// a configstring too long for one command arrives in pieces, as CL_GetServerCommand puts them back
	if ( !strncmp( s, "bcs", 3 ) && s[3] >= '0' && s[3] <= '2' && s[4] == ' ' ) {
		Demo_Token( s, 1, index, sizeof( index ) );
		Demo_Token( s, 2, piece, sizeof( piece ) );
		if ( s[3] == '0' ) {
			Com_sprintf( d->bigConfigString, sizeof( d->bigConfigString ), "cs %s \"%s", index, piece );
			return;
		}
		if ( strlen( d->bigConfigString ) + strlen( piece ) + 2 >= sizeof( d->bigConfigString ) ) {
			d->bigConfigString[0] = '\0';
			return;
		}
		strcat( d->bigConfigString, piece );
		if ( s[3] == '1' ) {
			return;
		}
		strcat( d->bigConfigString, "\"" );
		s = d->bigConfigString;
	}
	Demo_RecordAddCommand( d, seq, s );
}

static void Demo_ParseGamestate( demoDecoder_t *d, msg_t *msg ) {
	char			s[BIG_INFO_STRING];
	entityState_t	nullstate;
	int				i, k, cmd, newnum, len;

	// commands that came before the gamestate in this record belong to the level being left
	d->recordCmdsBeforeGamestate = d->numRecordCmds;

	d->serverCommandSequence = MSG_ReadLong( msg );

	// the table is torn down below: a fault before the end must not leave it filed under the old level
	d->gamestate = -1;
	d->streamSegment = -1;
	Com_Memset( &d->gameState, 0, sizeof( d->gameState ) );
	Com_Memset( d->baselines, 0, sizeof( entityState_t ) * MAX_GENTITIES );
	d->gameState.dataCount = 1;
	while ( 1 ) {
		cmd = MSG_ReadByte( msg );
		if ( cmd == svc_EOF ) {
			break;
		}
		if ( cmd == svc_configstring ) {
			i = MSG_ReadShort( msg );
			if ( i < 0 || i >= MAX_CONFIGSTRINGS ) {
				d->fault = qtrue;
				return;
			}
			Demo_ReadString( msg, s, sizeof( s ) );
			len = (int)strlen( s );
			if ( len + 1 + d->gameState.dataCount > MAX_GAMESTATE_CHARS ) {
				d->fault = qtrue;
				return;
			}
			d->gameState.stringOffsets[i] = d->gameState.dataCount;
			Com_Memcpy( d->gameState.stringData + d->gameState.dataCount, s, len + 1 );
			d->gameState.dataCount += len + 1;
		} else if ( cmd == svc_baseline ) {
			newnum = MSG_ReadBits( msg, GENTITYNUM_BITS );
			if ( newnum < 0 || newnum >= MAX_GENTITIES ) {
				d->fault = qtrue;
				return;
			}
			Com_Memset( &nullstate, 0, sizeof( nullstate ) );
			if ( !Demo_EntityFits( msg ) ) {
				d->fault = qtrue;
				return;
			}
			MSG_ReadDeltaEntity( msg, &nullstate, &d->baselines[newnum], newnum );
		} else {
			d->fault = qtrue;
			return;
		}
		if ( msg->readcount > msg->cursize ) {
			d->fault = qtrue;
			return;
		}
	}

	MSG_ReadLong( msg );		// clientNum
	MSG_ReadLong( msg );		// checksum feed
	MSG_ReadShort( msg );		// the old RMG system

	// a new level: nothing decoded so far can serve as a delta base
	for ( k = 0; k < PACKET_BACKUP; k++ ) {
		d->snapshots[k].valid = qfalse;
	}
	d->snap.valid = qfalse;
	d->streamSegment = d->recordOffset;
	d->gamestate = CL_DemoGamestateIndexOf( d->recordOffset );
	d->gamestateParsed = qtrue;
	if ( d->gamestate >= 0 ) {
		CL_DemoNoteGamestateFrom( d );
	}
}

static void Demo_ParseMessage( demoDecoder_t *d, msg_t *msg ) {
	int cmd;

	MSG_Bitstream( msg );
	MSG_ReadLong( msg );			// reliableAcknowledge

	while ( !d->fault ) {
		if ( msg->readcount > msg->cursize ) {
			d->fault = qtrue;
			break;
		}
		cmd = MSG_ReadByte( msg );
		if ( cmd == svc_EOF ) {
			break;
		}
		switch ( cmd ) {
		case svc_nop:
			break;
		case svc_serverCommand:
			Demo_ParseCommand( d, msg );
			break;
		case svc_gamestate:
			Demo_ParseGamestate( d, msg );
			break;
		case svc_snapshot:
			Demo_ParseSnapshot( d, msg );
			break;
		case svc_setgame: {
			char	dir[MAX_QPATH];
			int		i = 0, c;

			// the field tables follow the recording's mod, as the live parser sets them
			while ( i < MAX_QPATH - 1 && ( c = MSG_ReadByte( msg ) ) > 0 ) {
				dir[i++] = (char)c;
			}
			dir[i] = 0;
			if ( Q_stricmp( !FS_FilenameCompare( dir, BASEGAME ) ? "" : dir, Cvar_VariableString( "fs_game" ) ) ) {
				CL_SetGameDir( dir );	// only a change: fs_game stays flagged modified afterwards
			}
			break;
		}
		case svc_mapchange:
			break;						// the structure scan placed it; the loop that reads knows where the level ends
		default:
			d->fault = qtrue;				// svc_download and anything illegible
			break;
		}
	}
}

// the record's commands go to the log with its snapshot time, and its "cs" commands into the table
static void Demo_FlushRecordCommands( demoDecoder_t *d ) {
	int		i, t, first = 0, gs = d->gamestate;

	if ( d->gamestateParsed ) {
		first = d->recordCmdsBeforeGamestate;		// those were the old level's; the live parser never ran them either
	}
	if ( gs >= 0 && first < d->numRecordCmds ) {
		t = d->snap.valid ? d->snap.serverTime : 0;
		if ( d->recordOffset < demoRaw.gs[gs].settleOffset || t < demoRaw.gs[gs].settleTime ) {
			t = demoRaw.gs[gs].settleTime;
		}
		for ( i = first; i < d->numRecordCmds; i++ ) {
			const char *text = d->cmdPool + d->recordCmdText[i];

			CL_DemoCmdLogAdd( gs, d->recordCmdSeq[i], t, text );
			if ( !strncmp( text, "cs ", 3 ) ) {
				char value[BIG_INFO_STRING];

				DR_ArgsFrom( text, 2, value, sizeof( value ) );
				Demo_GsSet( &d->gameState, atoi( text + 3 ), value );
			}
		}
	}
	d->numRecordCmds = 0;
	d->cmdPoolUsed = 0;
	d->recordCmdsBeforeGamestate = 0;
}

/*
===================
Records
===================
*/
qboolean Demo_NextRecordLeavesSegment( const demoDecoder_t *d ) {
	int pos = d->fileOffset, gs;

	if ( CL_DemoGamestateIndexOf( pos ) >= 0 ) {
		return (qboolean)( pos != d->streamSegment );
	}
	gs = d->gamestate;
	return (qboolean)( gs >= 0 && gs + 1 < demoRaw.numGs && demoRaw.endOffset[gs] && pos >= demoRaw.endOffset[gs] );
}

qboolean Demo_ReadRecord( demoDecoder_t *d ) {
	byte		*bufData = d->recordBuf;
	msg_t		buf;
	int			seq, len;

	if ( d->eof || d->fault ) {
		return qfalse;
	}

	// past a svc_mapchange nothing belongs to the level until the next gamestate
	if ( d->gamestate >= 0 && demoRaw.endOffset[d->gamestate] && d->fileOffset >= demoRaw.endOffset[d->gamestate]
		&& CL_DemoGamestateIndexOf( d->fileOffset ) < 0 ) {
		if ( d->gamestate + 1 < demoRaw.numGs ) {
			d->fileOffset = demoRaw.gs[d->gamestate + 1].fileOffset;
		} else {
			d->eof = qtrue;
			return qfalse;
		}
	}

	Demo_FileSeek( d, d->fileOffset );
	if ( !Demo_FileRead( d, &seq, 4 ) ) {
		d->eof = qtrue;
		d->filePos = -1;
		return qfalse;
	}
	d->recordOffset = d->fileOffset;
	if ( !Demo_FileRead( d, &len, 4 ) ) {
		d->eof = qtrue;
		d->filePos = -1;
		return qfalse;
	}
	d->filePos = -1;			// unknown until the record is read whole
	len = LittleLong( len );
	if ( len == -1 ) {
		d->eof = qtrue;
		return qfalse;
	}
	if ( len < 0 || len > MAX_MSGLEN ) {
		d->fault = qtrue;
		return qfalse;
	}
	if ( !Demo_FileRead( d, bufData, len ) ) {
		d->eof = qtrue;			// truncated
		d->filePos = -1;
		return qfalse;
	}
	d->fileOffset += 8 + len;
	d->filePos = d->fileOffset;
	d->serverMessageSequence = LittleLong( seq );

	d->numRecordCmds = 0;
	d->cmdPoolUsed = 0;
	d->recordCmdsBeforeGamestate = 0;
	d->snapDecoded = qfalse;
	d->gamestateParsed = qfalse;
	d->guard++;

	MSG_Init( &buf, bufData, MAX_MSGLEN );
	buf.cursize = len;
	buf.readcount = 0;
	Demo_ParseMessage( d, &buf );
	if ( d->fault ) {
		d->faultOffset = d->recordOffset;
		d->faultCount++;
		return qfalse;
	}

	Demo_FlushRecordCommands( d );
	CL_DemoDecoderRecordDone( d );
	return qtrue;
}

// how many snapshots back a later record may delta from: the oldest base the recent stream used,
// and never fewer than DEMO_CTX_MIN_SLOTS
int Demo_DeltaReach( const demoDecoder_t *d ) {
	int k, oldest = d->snap.messageNum - ( DEMO_CTX_MIN_SLOTS - 1 ), reach;

	for ( k = 0; k < DEMO_CTX_MIN_SLOTS; k++ ) {
		const clSnapshot_t *sn = &d->snapshots[( d->snap.messageNum - k ) & PACKET_MASK];

		if ( sn->valid && sn->messageNum == d->snap.messageNum - k && sn->deltaNum > 0 && sn->deltaNum < oldest ) {
			oldest = sn->deltaNum;
		}
	}
	reach = d->snap.messageNum - oldest + 1;
	if ( reach < DEMO_CTX_MIN_SLOTS ) {
		reach = DEMO_CTX_MIN_SLOTS;
	}
	if ( reach > PACKET_BACKUP ) {
		reach = PACKET_BACKUP;
	}
	return reach;
}

/*
	Position the decoder for a decode toward target within a gamestate: from the newest usable
	keyframe before it, else from the gamestate's own record. The decoder's tables must belong to
	that gamestate before a keyframe can be restored into it (a keyframe carries no baselines)
*/
void Demo_Reposition( demoDecoder_t *d, int gamestate, int target ) {
	if ( gamestate < 0 || gamestate >= demoRaw.numGs ) {
		gamestate = 0;
	}
	d->eof = qfalse;
	d->fault = qfalse;
	d->kfRestoring = qfalse;
	d->kfRestoreFailed = qfalse;
	if ( d->gamestate != gamestate ) {
		Demo_DecoderReset( d, demoRaw.gs[gamestate].fileOffset );
		if ( !Demo_ReadRecord( d ) || d->gamestate != gamestate ) {
			return;				// the record could not be read; the loop that drives us sees eof or fault
		}
	}
	if ( CL_DemoRestoreKeyframe( d, gamestate, target ) ) {
		return;
	}
	// from the map's own record: read it, so the decoder stands under its segment
	if ( d->recordOffset != demoRaw.gs[gamestate].fileOffset ) {
		Demo_DecoderReset( d, demoRaw.gs[gamestate].fileOffset );
		Demo_ReadRecord( d );
	}
	DemoDbg( "decoder %d: gamestate %d from its record at %d", d->role, gamestate, d->fileOffset );
}
