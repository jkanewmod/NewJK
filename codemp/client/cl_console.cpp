/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2005 - 2015, ioquake3 contributors
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

// console.c

#include "client.h"
#include "cl_cgameapi.h"
#include "qcommon/stringed_ingame.h"
#include "qcommon/game_version.h"


int g_console_field_width = 78;

console_t	con;

cvar_t		*con_notifytime;
cvar_t		*cl_consoleOpacity; // background alpha multiplier
cvar_t		*cl_consoleHeight;
cvar_t		*con_autoclear;

#define	DEFAULT_CONSOLE_WIDTH	78

vec4_t	console_color = {0.509f, 0.609f, 0.847f, 1.0f};

void Con_ToggleConsoleFake_f (void) {
	// this is so that the toggleconsole command does nothing without saying unrecognized command
}

/*
================
Con_ToggleConsole_f
================
*/
void Con_ToggleConsole_f (void) {
	// closing a full screen console restarts the demo loop
	if ( cls.state == CA_DISCONNECTED && Key_GetCatcher( ) == KEYCATCH_CONSOLE ) {
		CL_StartDemoLoop();
		return;
	}

	if( con_autoclear->integer )
		Field_Clear( &g_consoleField );
	g_consoleField.widthInChars = g_console_field_width;

	//Con_ClearNotify ();
	Key_SetCatcher( Key_GetCatcher( ) ^ KEYCATCH_CONSOLE );
}

/*
===================
Con_ToggleMenu_f
===================
*/
void Con_ToggleMenu_f( void ) {
	CL_KeyEvent( A_ESCAPE, qtrue, Sys_Milliseconds() );
	CL_KeyEvent( A_ESCAPE, qfalse, Sys_Milliseconds() );
}

/*
================
Con_MessageMode_f
================
*/
void Con_MessageMode_f (void) {	//yell
	if (clc.demoplaying || !cls.cgameStarted)
		return;
	if (cl_keycatchLock->integer && !(Key_GetCatcher() & KEYCATCH_MESSAGE))
		return;
	chat_playerNum = -1;
	chat_team = qfalse;
	Field_Clear( &chatField );
	chatField.widthInChars = 30;

	Key_SetCatcher( Key_GetCatcher( ) ^ KEYCATCH_MESSAGE );
}

/*
================
Con_MessageMode2_f
================
*/
void Con_MessageMode2_f (void) {	//team chat
	if (clc.demoplaying || !cls.cgameStarted)
		return;
	if (cl_keycatchLock->integer && !(Key_GetCatcher() & KEYCATCH_MESSAGE))
		return;
	chat_playerNum = -1;
	chat_team = qtrue;
	Field_Clear( &chatField );
	chatField.widthInChars = 25;
	Key_SetCatcher( Key_GetCatcher( ) ^ KEYCATCH_MESSAGE );
}

/*
================
Con_MessageMode3_f
================
*/
void Con_MessageMode3_f (void)
{		//target chat
	if (clc.demoplaying)
		return;
	if (cl_keycatchLock->integer && !(Key_GetCatcher() & KEYCATCH_MESSAGE))
		return;
	if (!cls.cgameStarted)
	{
		assert(!"null cgvm");
		return;
	}

	chat_playerNum = CGVM_CrosshairPlayer();
	if ( chat_playerNum < 0 || chat_playerNum >= MAX_CLIENTS ) {
		chat_playerNum = -1;
		return;
	}
	chat_team = qfalse;
	Field_Clear( &chatField );
	chatField.widthInChars = 30;
	Key_SetCatcher( Key_GetCatcher( ) ^ KEYCATCH_MESSAGE );
}

/*
================
Con_MessageMode4_f
================
*/
void Con_MessageMode4_f (void)
{	//attacker
	if (clc.demoplaying)
		return;
	if (cl_keycatchLock->integer && !(Key_GetCatcher() & KEYCATCH_MESSAGE))
		return;
	if (!cls.cgameStarted)
	{
		assert(!"null cgvm");
		return;
	}

	chat_playerNum = CGVM_LastAttacker();
	if ( chat_playerNum < 0 || chat_playerNum >= MAX_CLIENTS ) {
		chat_playerNum = -1;
		return;
	}
	chat_team = qfalse;
	Field_Clear( &chatField );
	chatField.widthInChars = 30;
	Key_SetCatcher( Key_GetCatcher( ) ^ KEYCATCH_MESSAGE );
}

/*
================
Con_Clear_f
================
*/
void Con_Clear_f (void) {
	int		i;

	for ( i = 0 ; i < CON_TEXTSIZE ; i++ ) {
		con.text[i] = (ColorIndex(COLOR_WHITE)<<8) | ' ';
	}

	Con_Bottom();		// go to end
}


/*
================
Con_Dump_f

Save the console contents out to a file
================
*/
void Con_Dump_f (void)
{
	int		l, x, i;
	short	*line;
	fileHandle_t	f;
	int		bufferlen;
	char	*buffer;
	char	filename[MAX_QPATH];

	if (Cmd_Argc() != 2)
	{
		Com_Printf ("%s\n", SE_GetString("CON_TEXT_DUMP_USAGE"));
		return;
	}

	Q_strncpyz( filename, Cmd_Argv( 1 ), sizeof( filename ) );
	COM_DefaultExtension( filename, sizeof( filename ), ".txt" );

	if(!COM_CompareExtension(filename, ".txt"))
	{
		Com_Printf( "Con_Dump_f: Only the \".txt\" extension is supported by this command!\n" );
		return;
	}

	f = FS_FOpenFileWrite( filename );
	if (!f)
	{
		Com_Printf ("ERROR: couldn't open %s.\n", filename);
		return;
	}

	Com_Printf ("Dumped console text to %s.\n", filename );

	// skip empty lines
	for (l = con.current - con.totallines + 1 ; l <= con.current ; l++)
	{
		line = con.text + (l%con.totallines)*con.linewidth;
		for (x=0 ; x<con.linewidth ; x++)
			if ((line[x] & 0xff) != ' ')
				break;
		if (x != con.linewidth)
			break;
	}

#ifdef _WIN32
	bufferlen = con.linewidth + 3 * sizeof ( char );
#else
	bufferlen = con.linewidth + 2 * sizeof ( char );
#endif

	buffer = (char *)Hunk_AllocateTempMemory( bufferlen );

	// write the remaining lines
	buffer[bufferlen-1] = 0;
	for ( ; l <= con.current ; l++)
	{
		line = con.text + (l%con.totallines)*con.linewidth;
		for(i=0; i<con.linewidth; i++)
			buffer[i] = (char) (line[i] & 0xff);
		for (x=con.linewidth-1 ; x>=0 ; x--)
		{
			if (buffer[x] == ' ')
				buffer[x] = 0;
			else
				break;
		}
#ifdef _WIN32
		Q_strcat(buffer, bufferlen, "\r\n");
#else
		Q_strcat(buffer, bufferlen, "\n");
#endif
		FS_Write(buffer, strlen(buffer), f);
	}

	Hunk_FreeTempMemory( buffer );
	FS_FCloseFile( f );
}


/*
================
Con_ClearNotify
================
*/
void Con_ClearNotify( void ) {
	int		i;

	for ( i = 0 ; i < NUM_CON_TIMES ; i++ ) {
		con.times[i] = 0;
		con.notifyTime[i] = 0;
		con.notifyIndex[i] = 0;
	}
	con.notifyHead = -1;
}



/*
================
Con_CheckResize

If the line width has changed, reformat the buffer.
================
*/
void Con_CheckResize (void)
{
	int		i, j, width, oldwidth, oldtotallines, numlines, numchars;

//	width = (SCREEN_WIDTH / SMALLCHAR_WIDTH) - 2;
	width = (cls.glconfig.vidWidth / SMALLCHAR_WIDTH) - 2;

	if (width == con.linewidth)
		return;


	if (width < 1)			// video hasn't been initialized yet
	{
		con.xadjust = 1;
		con.yadjust = 1;
		width = DEFAULT_CONSOLE_WIDTH;
		con.linewidth = width;
		con.totallines = CON_TEXTSIZE / con.linewidth;
		for(i=0; i<CON_TEXTSIZE; i++)
		{
			con.text[i] = (ColorIndex(COLOR_WHITE)<<8) | ' ';
		}
	}
	else
	{
		short *tbuf = (short *)malloc(CON_TEXTSIZE * sizeof(short));
		if (!tbuf)
			return; // idk buy a fucking new computer bro this is like a 20 year old game

		// on wide screens, we will center the text
		con.xadjust = 640.0f / cls.glconfig.vidWidth;
		con.yadjust = 480.0f / cls.glconfig.vidHeight;

		oldwidth = con.linewidth;
		con.linewidth = width;
		oldtotallines = con.totallines;
		con.totallines = CON_TEXTSIZE / con.linewidth;
		numlines = oldtotallines;

		if (con.totallines < numlines)
			numlines = con.totallines;

		numchars = oldwidth;

		if (con.linewidth < numchars)
			numchars = con.linewidth;

		Com_Memcpy (tbuf, con.text, CON_TEXTSIZE * sizeof(short));
		for(i=0; i<CON_TEXTSIZE; i++)

			con.text[i] = (ColorIndex(COLOR_WHITE)<<8) | ' ';


		for (i=0 ; i<numlines ; i++)
		{
			for (j=0 ; j<numchars ; j++)
			{
				con.text[(con.totallines - 1 - i) * con.linewidth + j] =
						tbuf[((con.current - i + oldtotallines) %
							  oldtotallines) * oldwidth + j];
			}
		}

		Con_ClearNotify ();
		free(tbuf);
	}

	con.current = con.totallines - 1;
	con.display = con.current;
}


/*
==================
Cmd_CompleteTxtName
==================
*/
void Cmd_CompleteTxtName( char *args, int argNum ) {
	if ( argNum == 2 )
		Field_CompleteFilename( "", "txt", qfalse, qtrue );
}

/*
================
Con_Init
================
*/
void Con_Init (void) {
	int		i;

	con_notifytime = Cvar_Get ("con_notifytime", "3", 0, "How many seconds notify messages should be shown before they fade away");

	cl_consoleOpacity = Cvar_Get ("cl_consoleOpacity", "0.5", CVAR_ARCHIVE, "Opacity of console background");
	cl_consoleHeight = Cvar_Get("cl_consoleHeight", "0.5", CVAR_ARCHIVE, "Console height");
	con_autoclear = Cvar_Get ("con_autoclear", "1", CVAR_ARCHIVE, "Automatically clear console input on close");

	Field_Clear( &g_consoleField );
	g_consoleField.widthInChars = g_console_field_width;
	for ( i = 0 ; i < COMMAND_HISTORY ; i++ ) {
		Field_Clear( &historyEditLines[i] );
		historyEditLines[i].widthInChars = g_console_field_width;
	}

	con.notifyHead = -1;
	memset(con.notifyIndex, 0, sizeof(con.notifyIndex));
	memset(con.notifyTime, 0, sizeof(con.notifyTime));

	Cmd_AddCommand( "toggleconsole", Con_ToggleConsoleFake_f, "" );
	Cmd_AddCommand( "toggleconsole2", Con_ToggleConsole_f, "Toggles the console on/off" );
	Cmd_AddCommand( "togglemenu", Con_ToggleMenu_f, "Show/hide the menu" );
	Cmd_AddCommand( "messagemode", Con_MessageMode_f, "Global Chat" );
	Cmd_AddCommand( "messagemode2", Con_MessageMode2_f, "Team Chat" );
	Cmd_AddCommand( "messagemode3", Con_MessageMode3_f, "Private Chat with Target Player" );
	Cmd_AddCommand( "messagemode4", Con_MessageMode4_f, "Private Chat with Last Attacker" );
	Cmd_AddCommand( "clear", Con_Clear_f, "Clear console text" );
	Cmd_AddCommand( "condump", Con_Dump_f, "Dump console text to file" );
	Cmd_SetCommandCompletionFunc( "condump", Cmd_CompleteTxtName );
}

/*
================
Con_Shutdown
================
*/
void Con_Shutdown(void)
{
	Cmd_RemoveCommand("toggleconsole");
	Cmd_RemoveCommand("togglemenu");
	Cmd_RemoveCommand("messagemode");
	Cmd_RemoveCommand("messagemode2");
	Cmd_RemoveCommand("messagemode3");
	Cmd_RemoveCommand("messagemode4");
	Cmd_RemoveCommand("clear");
	Cmd_RemoveCommand("condump");
}

/*
===============
Con_Linefeed
===============
*/
static void Con_Linefeed (qboolean skipnotify)
{
	int		i;

	// mark time for transparent overlay
	if (con.current >= 0)
	{
		if (skipnotify)
			  con.times[con.current % NUM_CON_TIMES] = 0;
		else
			  con.times[con.current % NUM_CON_TIMES] = cls.realtime;
	}

	if (!skipnotify) {
		con.notifyHead++;
		{
			int nIndex = con.notifyHead % NUM_CON_TIMES;
			con.notifyIndex[nIndex] = con.current;
			con.notifyTime[nIndex]  = cls.realtime;
		}
	}

	con.x = 0;
	if (con.display == con.current)
		con.display++;
	con.current++;
	for(i=0; i<con.linewidth; i++)
		con.text[(con.current%con.totallines)*con.linewidth+i] = (ColorIndex(COLOR_WHITE)<<8) | ' ';
}

/*
================
CL_ConsolePrint

Handles cursor positioning, line wrapping, etc
All console printing must go through this in order to be logged to disk
If no console is visible, the text will appear at the top of the game window
================
*/
void CL_ConsolePrint( const char *txt) {
	int		y;
	int		c, l;
	int		color;
	qboolean skipnotify = qfalse;		// NERVE - SMF
	int prev;							// NERVE - SMF

	// TTimo - prefix for text that shows up in console but not in notify
	// backported from RTCW
	if ( !Q_strncmp( txt, "[skipnotify]", 12 ) ) {
		skipnotify = qtrue;
		txt += 12;
	}
	if ( txt[0] == '*' ) {
		skipnotify = qtrue;
		txt += 1;
	}

	// for some demos we don't want to ever show anything on the console
	if ( cl_noprint && cl_noprint->integer ) {
		return;
	}

	if (!con.initialized) {
		con.color[0] =
		con.color[1] =
		con.color[2] =
		con.color[3] = 1.0f;
		con.linewidth = -1;
		Con_CheckResize ();
		con.initialized = qtrue;
	}

	color = ColorIndex(COLOR_WHITE);

	while ( (c = (unsigned char) *txt) != 0 ) {
		if ( Q_IsColorString( (unsigned char*) txt ) ) {
			color = ColorIndex( *(txt+1) );
			txt += 2;
			continue;
		}

		// count word length
		for (l=0 ; l< con.linewidth ; l++) {
			if ( txt[l] <= ' ') {
				break;
			}
		}

		// word wrap
		if (l != con.linewidth && (con.x + l >= con.linewidth) ) {
			Con_Linefeed(skipnotify);

		}

		txt++;

		switch (c)
		{
		case '\n':
			Con_Linefeed (skipnotify);
			break;
		case '\r':
			con.x = 0;
			break;
		default:	// display character and advance
			y = con.current % con.totallines;
			con.text[y*con.linewidth+con.x] = (short) ((color << 8) | c);
			con.x++;
			if (con.x >= con.linewidth) {
				Con_Linefeed(skipnotify);
			}
			break;
		}
	}


	// mark time for transparent overlay

	if (con.current >= 0 )
	{
		// NERVE - SMF
		if ( skipnotify ) {
			prev = con.current % NUM_CON_TIMES - 1;
			if ( prev < 0 )
				prev = NUM_CON_TIMES - 1;
			con.times[prev] = 0;
		}
		else
		// -NERVE - SMF
			con.times[con.current % NUM_CON_TIMES] = cls.realtime;
	}
}


/*
==============================================================================

DRAWING

==============================================================================
*/

/*
================
Con_DrawInput

Draw the editline after a ] prompt
================
*/
void Con_DrawInput (void) {
	int		y;

	if ( cls.state != CA_DISCONNECTED && !(Key_GetCatcher( ) & KEYCATCH_CONSOLE ) ) {
		return;
	}

	y = con.vislines - ( SMALLCHAR_HEIGHT * (re->Language_IsAsian() ? 1.5 : 2) );

	re->SetColor( con.color );

	SCR_DrawSmallChar( (int)(con.xadjust + 1 * SMALLCHAR_WIDTH), y, CONSOLE_PROMPT_CHAR );

	Field_Draw( &g_consoleField, (int)(con.xadjust + 2 * SMALLCHAR_WIDTH), y,
				SCREEN_WIDTH - 3 * SMALLCHAR_WIDTH, qtrue, qtrue );
}



int ChatStrLen(void) {
	int result = 0;
	for (unsigned char* p = (unsigned char *)chatField.buffer; *p; p++) {
		switch (*p) {
		case 172: // €/¬
			result += 6;
			break;
		case '%':
			result += 3;
			break;
		case '"':
			result += 2;
			break;
		default:
			result += 1;
			break;
		}
	}

	return result;
}

/*
================
Con_DrawNotify

Draws the last few lines of output transparently over the game top
================
*/
void Con_DrawNotify(void)
{
	int		x, v;
	short *text;
	int		i;
	int		time;
	int		currentColor;
	int drawnLines = 0;

	currentColor = 7;
	re->SetColor(g_color_table[currentColor]);

	v = cl_consoleFeedYBase->integer + cl_consoleFeedYOffset->integer;

	{
		int first = con.notifyHead - (NUM_CON_TIMES - 1);
		if (first < 0)
			first = 0;

		for (i = first; i <= con.notifyHead; i++)
		{
			int slot = i % NUM_CON_TIMES;
			if (slot < 0)
				slot += NUM_CON_TIMES;

			time = con.notifyTime[slot];
			if (time == 0)
				continue;
			time = cls.realtime - time;
			if (time > con_notifytime->value * 1000)
				continue;

			text = con.text + ((con.notifyIndex[slot] % con.totallines) * con.linewidth);

			/*if (cl.snap.ps.pm_type != PM_INTERMISSION && Key_GetCatcher() & (KEYCATCH_UI | KEYCATCH_CGAME)) {
				continue;
			}*/

			if (!cl_conXOffset)
			{
				cl_conXOffset = Cvar_Get("cl_conXOffset", "0", 0);
			}

			// asian language needs to use the new font system to print glyphs...
			if (re->Language_IsAsian())
			{
				static int iFontIndex = re->RegisterFont("ocr_a");	// this seems naughty
				const float fFontScale = 0.75f * con.yadjust;
				const int iPixelHeightToAdvance = 2 + (1.3 / con.yadjust) * re->Font_HeightPixels(iFontIndex, fFontScale);

				// concat the text to be printed...
				char sTemp[4096] = { 0 };	// ott
				for (x = 0; x < con.linewidth; x++)
				{
					if (((text[x] >> 8) & Q_COLOR_BITS) != currentColor) {
						currentColor = (text[x] >> 8) & Q_COLOR_BITS;
						strcat(sTemp, va("^%i", (text[x] >> 8) & Q_COLOR_BITS));
					}
					strcat(sTemp, va("%c", text[x] & 0xFF));
				}
				// and print...
				re->Font_DrawString(cl_conXOffset->integer + con.xadjust * (con.xadjust + (1 * SMALLCHAR_NOTIFY_WIDTH/*aesthetics*/)), con.yadjust * (v), sTemp, g_color_table[currentColor], iFontIndex, -1, fFontScale);

				v += iPixelHeightToAdvance;
			}
			else
			{
				for (x = 0; x < con.linewidth; x++) {
					if ((text[x] & 0xff) == ' ') {
						continue;
					}
					if (((text[x] >> 8) & Q_COLOR_BITS) != currentColor) {
						currentColor = (text[x] >> 8) & Q_COLOR_BITS;
						re->SetColor(g_color_table[currentColor]);
					}
					if (!cl_conXOffset)
					{
						cl_conXOffset = Cvar_Get("cl_conXOffset", "0", 0);
					}
					SCR_DrawSmallChar_ConsoleNotify((int)(cl_conXOffset->integer + con.xadjust + (x + 1) * SMALLCHAR_NOTIFY_WIDTH), v, text[x] & 0xff);
				}

				++drawnLines;
				v += SMALLCHAR_NOTIFY_HEIGHT;
			}
		}

		{
			if (drawnLines < NUM_CON_TIMES)
			{
				const int missing = NUM_CON_TIMES - drawnLines;
				v += missing * SMALLCHAR_NOTIFY_HEIGHT;
			}
		}
	}

	re->SetColor(NULL);

	if (Key_GetCatcher() & (KEYCATCH_UI | KEYCATCH_CGAME)) {
		return;
	}

	if (Key_GetCatcher() & KEYCATCH_MESSAGE) {
		// initialize
		static char language[32] = { 0 }, teamPrompt[32] = { 0 }, prompt[32] = { 0 };
		if (Q_stricmp(language, se_language->string)) {
			Q_strncpyz(language, se_language->string, sizeof(language));
			Q_strncpyz(teamPrompt, SE_GetString("MP_SVGAME", "SAY_TEAM"), sizeof(teamPrompt));
			if (teamPrompt[0]) {
				if (!strcmp(teamPrompt, "Say Team:") || !strcmp(teamPrompt, "Team-Text:"))
					Q_strncpyz(teamPrompt, "Team", sizeof(teamPrompt));
				else if (!strcmp(teamPrompt, "A équipe :"))
					Q_strncpyz(teamPrompt, "Équipe", sizeof(teamPrompt));
				else if (!strcmp(teamPrompt, "Dice equipo:"))
					Q_strncpyz(teamPrompt, "Equipo", sizeof(teamPrompt));

				if (char *c = strchr(teamPrompt, ':')) *c = '\0';
			}

			Q_strncpyz(prompt, SE_GetString("MP_SVGAME", "SAY"), sizeof(prompt));

			if (prompt[0]) {
				if (!strcmp(prompt, "Dire :"))
					Q_strncpyz(prompt, "Dire", sizeof(prompt));
				if (char *c = strchr(prompt, ':')) *c = '\0';
			}
		}

		const int chatFont = FONT_MEDIUM | STYLE_DROPSHADOW;
		const float fontSize = 0.75f;
		const int lineH = re->Font_HeightPixels(chatFont, fontSize);
		const int linePad = 2;
		const int linePitch = lineH + linePad;
		const int leftPad = 64;
		const int rightPad = 64;
		const int maxPix = SCREEN_WIDTH - leftPad - rightPad;
		const int textMaxPix = maxPix - 28;

		// prompt line
		const int digitsRemaining = Com_Clampi(0, 149, 149 - ChatStrLen());
		const char *promptBase = chat_team ? teamPrompt : prompt;

		char promptBuf[128] = { 0 };
		if (digitsRemaining <= 50)
			Com_sprintf(promptBuf, sizeof(promptBuf), "^9%s (%d): ", promptBase, digitsRemaining);
		else
			Com_sprintf(promptBuf, sizeof(promptBuf), "^9%s: ", promptBase);

		const char *typed = chatField.buffer;
		int len = strlen(typed);

		auto isColor = [&](int idx)->bool { return (typed[idx] == '^' && idx + 1 < len && typed[idx + 1] >= '0' && typed[idx + 1] <= '9'); };
		auto substrPx = [&](int first, int lastIncl)->float {
			if (lastIncl < first)
				return 0.0f;

			if (first < 0)
				first = 0;

			if (lastIncl >= len)
				lastIncl = len - 1;

			int n = lastIncl - first + 1;
			if (n >= MAX_EDIT_LINE)
				n = MAX_EDIT_LINE - 1;

			char tmp[MAX_EDIT_LINE];
			memcpy(tmp, typed + first, n);
			tmp[n] = '\0';

			return re->ext.Font_StrLenPixels(tmp, chatFont, fontSize);
		};

		// calculate background box height
		int bgLines = 1;
		int lastLineStart = 0;
		{
			int   lineStart = 0;
			bool  after = false;

			while (lineStart < len) {
				lastLineStart = lineStart;
				float width = 0.0f;
				int   i = lineStart, lastSpace = -1;

				while (i < len)
				{
					if (after) {
						after = false;
						++i;
						continue;
					}
					if (isColor(i)) {
						after = true;
						++i;
						continue;
					}
					if (typed[i] == ' ')
						lastSpace = i;

					float w = substrPx(i, i);
					if (width + w > textMaxPix) break;
					width += w;
					++i;
				}

				int wrap = (i == len) ? len : (lastSpace > lineStart) ? lastSpace + 1 : i;

				lineStart = wrap;
				++bgLines;
			}

			float caretW = re->ext.Font_StrLenPixels("|", chatFont, fontSize);
			float caretX = substrPx(lastLineStart, chatField.cursor - 1);
			if (caretX + caretW > textMaxPix)
				++bgLines;
		}
		if (bgLines < 2)
			bgLines = 2;

		// background box
		float bgCol[4] = { 0.25f, 0.25f, 0.25f, 0.5f };
		re->SetColor(bgCol);
		re->DrawStretchPic(leftPad - 4, v, maxPix - 16, bgLines * lineH + (bgLines - 1) * linePad + 8, 0, 0, 0, 0, cls.whiteShader);
		re->SetColor(NULL);

		// prompt text
		re->Font_DrawString_Float(leftPad, v, promptBuf, colorWhite, chatFont, -1, fontSize);

		// chat text
		int lineStart = 0;
		int y = v + linePitch;
		char curColor = chat_team ? '5' : '2';
		bool afterCaret = false, hasColor = false;

		int breaks[64], nLines = 0;
		const int segmentLimit = MAX_EDIT_LINE - 4;

		while (lineStart < len) {
			char  startColor = curColor;
			float width = 0.0f;
			int   i = lineStart, lastSpace = -1;

			while (i < len) {
				if (afterCaret) { curColor = typed[i]; hasColor = true; afterCaret = false; ++i; continue; }
				if (isColor(i)) { afterCaret = true; ++i; continue; }
				if (typed[i] == ' ') lastSpace = i;

				float w = substrPx(i, i);
				if (width + w > textMaxPix) break;
				width += w;
				++i;
			}

			int wrap = (i == len) ? len : (lastSpace > lineStart) ? lastSpace + 1 : i;

			char seg[MAX_EDIT_LINE + 4];
			int  segN = wrap - lineStart;
			if (segN > segmentLimit)
				segN = segmentLimit;

			if (isColor(lineStart)) {
				memcpy(seg, typed + lineStart, segN);
				seg[segN] = '\0';
			}
			else {
				memcpy(seg + 2, typed + lineStart, segN);
				seg[0] = '^'; seg[1] = startColor;
				seg[segN + 2] = '\0';
			}

			re->Font_DrawString_Float(leftPad, y, seg, colorWhite, chatFont, -1, fontSize);

			breaks[nLines++] = lineStart;
			y += linePitch;
			lineStart = wrap;
		}

		if (nLines == 0) {
			breaks[0] = 0;
			nLines = 1;
		}

		// blinking cursor
		if (!(cls.realtime & 0x100)) {
			int curLine = 0;
			while (curLine + 1 < nLines && chatField.cursor >= breaks[curLine + 1])
				++curLine;

			float caretX = substrPx(breaks[curLine], chatField.cursor - 1);
			int caretY = v + (curLine + 1) * linePitch;

			char gStr[4] = { '^', hasColor ? curColor : '9', kg.key_overstrikeMode ? '_' : '|', 0 };

			float glyphW = re->ext.Font_StrLenPixels(gStr, chatFont, fontSize);
			if (glyphW <= 0.0f)
				glyphW = 2.0f;
			if (caretX + glyphW > textMaxPix) {
				caretX = 0.0f;
				caretY += linePitch;
			}

			float correctX = -2;
			if (cl_ratioFix->integer)
				correctX *= cls.widthRatioCoef;
			re->Font_DrawString_Float(leftPad + caretX + correctX, caretY, gStr, colorWhite, chatFont, -1, fontSize);
		}

		v = y;
	}
}

/*
================
Con_DrawSolidConsole

Draws the console with the solid background
================
*/
void Con_DrawSolidConsole( float frac ) {
	int				i, x, y;
	int				rows;
	short			*text;
	int				row;
	int				lines;
//	qhandle_t		conShader;
	int				currentColor;

	lines = (int) (cls.glconfig.vidHeight * frac);
	if (lines <= 0)
		return;

	if (lines > cls.glconfig.vidHeight )
		lines = cls.glconfig.vidHeight;

	// draw the background
	y = (int) (frac * SCREEN_HEIGHT - 2);
	if ( y < 1 ) {
		y = 0;
	}
	else {
		vec4_t con_color;
		MAKERGBA(con_color, 1.0f, 1.0f, 1.0f, Com_Clamp(0.0f, 1.0f, cl_consoleOpacity->value));
		re->SetColor(con_color);
		SCR_DrawPic( 0, 0, SCREEN_WIDTH, (float) y, cls.consoleShader );
	}

	// draw the bottom bar and version number

	re->SetColor( console_color );
	re->DrawStretchPic( 0, y, SCREEN_WIDTH, 2, 0, 0, 0, 0, cls.whiteShader );

	static char version[MAX_STRING_CHARS] = { 0 };
	static int len = 0;
	if (!version[0]) {
		Q_strncpyz(version, va("NewJK version %s", NEWJK_VERSION), sizeof(version));
		len = strlen(version);
	}
	i = len;

	for (x=0 ; x<i ; x++) {
		SCR_DrawSmallChar( cls.glconfig.vidWidth - ( i - x + 1 ) * SMALLCHAR_WIDTH,
			(lines-(SMALLCHAR_HEIGHT+SMALLCHAR_HEIGHT/2)), version[x] );
	}

	// draw the input prompt, user text, and cursor if desired
	Con_DrawInput();

	// draw the text
	con.vislines = lines;
	rows = (lines-SMALLCHAR_WIDTH)/SMALLCHAR_WIDTH;		// rows of text to draw

	y = lines - (SMALLCHAR_HEIGHT*3);

	// draw from the bottom up
	if (con.display != con.current)
	{
	// draw arrows to show the buffer is backscrolled
		re->SetColor(g_color_table[ColorIndex(COLOR_RED)]);
		for (x=0 ; x<con.linewidth ; x+=4)
			SCR_DrawSmallChar( (int) (con.xadjust + (x+1)*SMALLCHAR_WIDTH), y, '^' );
		y -= SMALLCHAR_HEIGHT;
		rows--;
	}

	row = con.display;

	if ( con.x == 0 ) {
		row--;
	}

	currentColor = 7;
	re->SetColor( g_color_table[currentColor] );

	static int iFontIndexForAsian = 0;
	const float fFontScaleForAsian = 0.75f*con.yadjust;
	int iPixelHeightToAdvance = SMALLCHAR_HEIGHT;
	if (re->Language_IsAsian())
	{
		if (!iFontIndexForAsian)
		{
			iFontIndexForAsian = re->RegisterFont("ocr_a");
		}
		iPixelHeightToAdvance = (1.3/con.yadjust) * re->Font_HeightPixels(iFontIndexForAsian, fFontScaleForAsian);	// for asian spacing, since we don't want glyphs to touch.
	}

	for (i=0 ; i<rows ; i++, y -= iPixelHeightToAdvance, row--)
	{
		if (row < 0)
			break;
		if (con.current - row >= con.totallines) {
			// past scrollback wrap point
			continue;
		}

		text = con.text + (row % con.totallines)*con.linewidth;

		// asian language needs to use the new font system to print glyphs...
		//
		// (ignore colours since we're going to print the whole thing as one string)
		//
		if (re->Language_IsAsian())
		{
			char sTemp[4096]={0};	// ott
			for (x = 0 ; x < con.linewidth ; x++)
			{
				if ( ( (text[x]>>8)&Q_COLOR_BITS ) != currentColor ) {
					currentColor = (text[x]>>8)&Q_COLOR_BITS;
					strcat(sTemp,va("^%i", (text[x]>>8)&Q_COLOR_BITS) );
				}
				strcat(sTemp,va("%c",text[x] & 0xFF));
			}
			re->Font_DrawString(con.xadjust*(con.xadjust + (1*SMALLCHAR_WIDTH/*(aesthetics)*/)), con.yadjust*(y), sTemp, g_color_table[currentColor], iFontIndexForAsian, -1, fFontScaleForAsian);
		}
		else
		{
			for (x=0 ; x<con.linewidth ; x++) {
				if ( ( text[x] & 0xff ) == ' ' ) {
					continue;
				}

				if ( ( (text[x]>>8)&Q_COLOR_BITS ) != currentColor ) {
					currentColor = (text[x]>>8)&Q_COLOR_BITS;
					re->SetColor( g_color_table[currentColor] );
				}
				SCR_DrawSmallChar(  (int) (con.xadjust + (x+1)*SMALLCHAR_WIDTH), y, text[x] & 0xff );
			}
		}
	}

	re->SetColor( NULL );
}



/*
==================
Con_DrawConsole
==================
*/
void Con_DrawConsole( void ) {
	// check for console width changes from a vid mode change
	Con_CheckResize ();

	// if disconnected, render console full screen
	if ( cls.state == CA_DISCONNECTED ) {
		if ( !( Key_GetCatcher( ) & (KEYCATCH_UI | KEYCATCH_CGAME)) ) {
			Con_DrawSolidConsole( 1.0 );
			return;
		}
	}

	if ( con.displayFrac ) {
		Con_DrawSolidConsole( con.displayFrac );
	} else {
		// draw notify lines
		if ( cls.state == CA_ACTIVE ) {
			Con_DrawNotify ();
		}
	}
}

//================================================================

/*
==================
Con_RunConsole

Scroll it up or down
==================
*/
void Con_RunConsole (void) {
	float distance = Q_isanumber(cl_consoleHeight->string) ? Com_Clamp(0.1f, 1.0f, cl_consoleHeight->value) : 0.5f;

	// decide on the destination height of the console
	if ( Key_GetCatcher( ) & KEYCATCH_CONSOLE )
		con.finalFrac = distance;		// half screen
	else
		con.finalFrac = 0;				// none visible

	// wtf is this base jka code, just always take the same amount of time no matter what
	float time = 0.5f / 6 * 1000; // 83.333ms == double the default speed with default height
	float speed = distance / time;

	// scroll towards the destination height
	if (con.finalFrac < con.displayFrac)
	{
		con.displayFrac -= speed * (float)cls.realFrametime;
		if (con.finalFrac > con.displayFrac)
			con.displayFrac = con.finalFrac;

	}
	else if (con.finalFrac > con.displayFrac)
	{
		con.displayFrac += speed * (float)cls.realFrametime;
		if (con.finalFrac < con.displayFrac)
			con.displayFrac = con.finalFrac;
	}

}


void Con_PageUp( void ) {
	con.display -= 2;
	if ( con.current - con.display >= con.totallines ) {
		con.display = con.current - con.totallines + 1;
	}
}

void Con_PageDown( void ) {
	con.display += 2;
	if (con.display > con.current) {
		con.display = con.current;
	}
}

void Con_Top( void ) {
	con.display = con.totallines;
	if ( con.current - con.display >= con.totallines ) {
		con.display = con.current - con.totallines + 1;
	}
}

void Con_Bottom( void ) {
	con.display = con.current;
}


void Con_Close( void ) {
	if ( !com_cl_running->integer ) {
		return;
	}
	Field_Clear( &g_consoleField );
	//Con_ClearNotify ();
	Key_SetCatcher( Key_GetCatcher( ) & ~KEYCATCH_CONSOLE );
	con.finalFrac = 0;				// none visible
	con.displayFrac = 0;
}
