// cl_demoraw.cpp

#include "client.h"
#include "cl_demoraw.h"

#define DR_SETTLE_WINDOW	64

void DR_Init( demoRaw_t *r ) {
	memset( r, 0, sizeof( *r ) );
}

void DR_Free( demoRaw_t *r ) {
	free( r->ls );
	memset( r, 0, sizeof( *r ) );
}

void DR_Gamestate( demoRaw_t *r, int fileOffset, const char *mapName, int levelStartTime, int haveLevelStartTime, int cmdSeq ) {
	demoApiGamestate_t *g;

	r->lastSeq = cmdSeq;
	r->haveSeq = 1;
	r->lsValue = levelStartTime;
	r->lsHave = haveLevelStartTime;
	r->pendingLs = r->numLs;
	if ( r->capped || r->numGs >= DEMOAPI_MAX_GAMESTATES ) {
		r->capped = 1;
		r->levelLeft = 1;
		return;
	}
	r->levelLeft = 0;
	r->endOffset[r->numGs] = 0;
	g = &r->gs[r->numGs++];
	memset( g, 0, sizeof( *g ) );
	g->fileOffset = fileOffset;
	g->prevStamp = r->run;
	g->levelStartTime = levelStartTime;
	g->haveLevelStartTime = haveLevelStartTime;
	g->firstLevelStart = r->numLs;
	Q_strncpyz( g->mapName, mapName ? mapName : "", sizeof( g->mapName ) );
}

int DR_Command( demoRaw_t *r, int seq, const char *text, int recordOffset ) {
	const char	*q;
	int			value;

	if ( r->haveSeq && seq <= r->lastSeq ) {
		return 0;
	}
	r->lastSeq = seq;
	r->haveSeq = 1;
	if ( r->levelLeft ) {
		return 0;
	}
	if ( !r->numGs || strncmp( text, "cs ", 3 ) || atoi( text + 3 ) != CS_LEVEL_START_TIME ) {
		return 1;
	}
	q = strchr( text, '"' );
	value = q ? atoi( q + 1 ) : 0;
	if ( r->lsHave && value == r->lsValue ) {
		return 1;
	}
	r->lsValue = value;
	r->lsHave = 1;
	if ( r->numLs >= DEMOAPI_MAX_LEVELSTARTS ) {
		return 1;
	}
	if ( r->numLs >= r->maxLs ) {
		int					grow = r->maxLs ? r->maxLs * 2 : 64;
		demoApiLevelStart_t	*p = (demoApiLevelStart_t *)realloc( r->ls, grow * sizeof( *p ) );

		if ( !p ) {
			return 1;
		}
		r->ls = p;
		r->maxLs = grow;
	}
	memset( &r->ls[r->numLs], 0, sizeof( r->ls[0] ) );
	r->ls[r->numLs].value = value;
	r->ls[r->numLs].recordOffset = recordOffset;
	r->ls[r->numLs].stampTime = r->run;
	r->numLs++;
	r->gs[r->numGs - 1].numLevelStarts++;
	return 1;
}

// the argument list from token arg on, joined by single spaces, tokenized the way Cmd_TokenizeString
// does it (quotes group, // and /* end the line): what Cmd_ArgsFrom gives the live client
void DR_ArgsFrom( const char *text, int arg, char *out, int outSize ) {
	int n = 0, used = 0;

	out[0] = '\0';
	while ( used < outSize - 1 ) {
		const char	*start, *end;
		int			len;

		while ( *text && *(const unsigned char *)text <= ' ' ) {
			text++;
		}
		if ( !*text || ( text[0] == '/' && ( text[1] == '/' || text[1] == '*' ) ) ) {
			break;
		}
		if ( *text == '"' ) {
			start = ++text;
			while ( *text && *text != '"' ) {
				text++;
			}
			end = text;
			if ( *text ) {
				text++;
			}
		} else {
			start = text;
			while ( *(const unsigned char *)text > ' ' && *text != '"' && !( text[0] == '/' && ( text[1] == '/' || text[1] == '*' ) ) ) {
				text++;
			}
			end = text;
		}
		if ( n++ < arg ) {
			continue;
		}
		if ( used && used < outSize - 1 ) {
			out[used++] = ' ';
		}
		len = (int)( end - start );
		if ( len > outSize - 1 - used ) {
			len = outSize - 1 - used;
		}
		memcpy( out + used, start, len );
		used += len;
		out[used] = '\0';
	}
}

void DR_MapChange( demoRaw_t *r, int recordOffset ) {
	if ( !r->levelLeft && r->numGs ) {
		r->levelLeft = 1;
		r->endOffset[r->numGs - 1] = recordOffset;
	}
}

void DR_Snapshot( demoRaw_t *r, int serverTime, int recordOffset ) {
	demoApiGamestate_t *g = r->numGs ? &r->gs[r->numGs - 1] : NULL;

	if ( r->levelLeft ) {
		return;	// the next level's, before its gamestate
	}

	if ( !r->numSnaps ) {
		r->firstTime = serverTime;
	}
	r->lastTime = serverTime;
	r->numSnaps++;
	if ( r->numEarly < DEMOAPI_MAX_EARLYTIMES ) {
		r->early[r->numEarly++] = serverTime;
	}

	if ( g ) {
		if ( !g->numSnapshots ) {
			g->firstTime = serverTime;
			g->settleTime = serverTime;
			g->settleOffset = recordOffset;
			if ( serverTime < r->run ) {
				r->run = serverTime;
			}
		} else if ( r->run && serverTime < r->run && g->numSnapshots <= DR_SETTLE_WINDOW ) {
			// the server restarted its clock once the client took the new gamestate
			g->settleTime = serverTime;
			g->settleOffset = recordOffset;
			r->run = serverTime;
		}
		g->lastTime = serverTime;
		g->numSnapshots++;
		for ( ; r->pendingLs < r->numLs; r->pendingLs++ ) {
			r->ls[r->pendingLs].nextTime = serverTime;
			r->ls[r->pendingLs].nextOffset = recordOffset;
		}
	}

	if ( serverTime >= r->run || !r->run ) {
		r->run = serverTime;
	}
}

int DR_GamestateIndex( const demoRaw_t *r, int fileOffset ) {
	int i;

	for ( i = r->numGs - 1; i >= 0; i-- ) {
		if ( r->gs[i].fileOffset == fileOffset ) {
			return i;
		}
	}
	return -1;
}
