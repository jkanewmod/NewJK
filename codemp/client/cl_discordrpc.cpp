/*
Author: Blackwolf
Discord Integration with some usefull functions, have fun.
You need to link the static library also 'discord_rpc.lib'.
*/
#if defined DISCORD && !(_DEBUG)
#include <discord_rpc.h>
#include <discord_register.h>

#include "client/client.h"

static const char* APPLICATION_ID = "614098040474697740";

typedef struct statusIcon_s {
	char *string;
	char *icon;
} statusIcon_t;

static statusIcon_t gameTypes[] = {
	{ "FFA",			"ffa"			},
	{ "Holocron",		"holocron"		},
	{ "Jedi Master",	"jedimaster"	},
	{ "Duel",			"duel"			},
	{ "Power Duel",		"powerduel"		},
	{ "SP",				"ffa"			},
	{ "TFFA",			"tffa"			},
	{ "Siege",			"siege"			},
	{ "CTF",			"ctf"			},
	{ "CTY",			"cty"			},
}; static const size_t numGameTypes = ARRAY_LEN(gameTypes);

char *ReturnMapName() {
	char *mapname = cl.discord.mapName;

	if ( cls.state == CA_DISCONNECTED || cls.state == CA_CONNECTING )
	{
		return "menu";
	}

	Q_StripColor( mapname );
	return Q_strlwr(mapname);
}

char *ReturnServerName() {
	char *servername = cl.discord.hostName;

	//Q_StripColor( servername );
	Q_CleanStr(servername);
	return servername;
}

// e.g. "mp/ctf_kejim" ==> "Kejim"
char *GetMapNameForServerState() {
	static char filteredMapname[MAX_QPATH] = { 0 };
	Q_strncpyz(filteredMapname, ReturnMapName(), sizeof(filteredMapname));
	char *p = filteredMapname;

	if (*p && !Q_stricmpn(p, "mp/", 3))
		p += 3;
	if (*p && !Q_stricmpn(p, "ctf_", 4))
		p += 4;
	if (*p && !Q_stricmpn(p, "ffa_", 4))
		p += 4;
	if (*p && !Q_stricmpn(p, "duel_", 5))
		p += 5;
	if (*p && !Q_stricmpn(p, "siege_", 6))
		p += 6;

	if (*p) {
		*p = toupper((unsigned)*p);
		return p;
	}

	return filteredMapname; // map name was just "ctf_" or something?
}

char *ReturnMapIcon() {
	static char filteredMapname[MAX_QPATH] = { 0 };
	Q_strncpyz(filteredMapname, ReturnMapName(), sizeof(filteredMapname));
	Q_strstrip(filteredMapname, "/()", "_"); // e.g. mp/ffa1 ==> mp_ffa1
	return filteredMapname;
}

char *GetState()
{
	usercmd_t cmd;
	CL_GetUserCmd( cl.cmdNumber, &cmd );

	if (cls.state == CA_ACTIVE && cl.snap.valid) {
		if ((cl.snap.ps.pm_flags & PMF_FOLLOW) || cl.snap.ps.pm_type == PM_SPECTATOR || cl.discord.myTeam == TEAM_SPECTATOR)
			return "spectator";
		if (cl.discord.myTeam == TEAM_RED)
			return "red";
		if (cl.discord.myTeam == TEAM_BLUE)
			return "blue";
		if (cl.discord.gametype >= GT_TEAM && cl.discord.myTeam == TEAM_FREE)
			return "racing";
		return "";
	}
	else if (cls.state > CA_DISCONNECTED && cls.state < CA_PRIMED) {
		return "connecting";
	}
	else if (cls.state <= CA_DISCONNECTED) {
		return "menu";
	}

	return "";
}

static char *GetGameType(qboolean imageKey)
{
	if (cl.discord.gametype > GT_FFA)
		return imageKey ? gameTypes[cl.discord.gametype].icon : gameTypes[cl.discord.gametype].string;

	return imageKey ? GetState() : gameTypes[cl.discord.gametype].string;
}

cvar_t *cl_discordSharePassword;
char *joinSecret()
{
	if (clc.demoplaying)
		return NULL;

	if ( cls.state >= CA_LOADING && cls.state <= CA_ACTIVE )
	{
		char *x = (char *)malloc( sizeof( char ) * 128 );
		char *password = Cvar_VariableString("password");

		if (cl_discordSharePassword->integer && cl.discord.needPassword && strlen(password)) {
			Com_sprintf(x, 128, "%s %s %s", cls.servername, cl.discord.fs_game, password);
		}
		else {
			Com_sprintf(x, 128, "%s %s \"\"", cls.servername, cl.discord.fs_game);
		}
		return x;
	}

	return NULL;
}

char *PartyID()
{
	if (clc.demoplaying)
		return NULL;

	if ( cls.state >= CA_LOADING && cls.state <= CA_ACTIVE ) 
	{
		static char x[128] = { 0 };
		Q_strncpyz( x, cls.servername, sizeof(x) );
		Q_strcat(x, sizeof(x), "x" );
		return x;
	}

	return NULL;
}

char *GetServerState() {
	if ( cls.state == CA_ACTIVE ) {
		/*if (cl_discordRichPresence->integer > 1) {
			return va("%d / %d players [%d BOTS]", cl.discord.playerCount, cl.discord.maxPlayers, cl.discord.botCount);
		}*/
		
		/*if (clc.demoplaying)
			return GetMapNameForServerState();*/

		if (cl.discord.gametype >= GT_TEAM) {
			if ((cl.snap.ps.pm_flags & PMF_FOLLOW) || cl.snap.ps.pm_type == PM_SPECTATOR)
				return va("%s - %dv%d (%d-%d)", GetMapNameForServerState(), cl.discord.redTeam, cl.discord.blueTeam, cl.discord.redScore, cl.discord.blueScore);

			if (cl.discord.myTeam == TEAM_RED || cl.discord.myTeam == TEAM_BLUE) {
				int myScore = cl.discord.myTeam == TEAM_RED ? cl.discord.redScore : cl.discord.blueScore;
				int enemyScore = cl.discord.myTeam == TEAM_RED ? cl.discord.blueScore : cl.discord.redScore;
				return va("%s %dv%d | %d-%d", GetMapNameForServerState(), cl.discord.redTeam, cl.discord.blueTeam, myScore, enemyScore);
			}

			return va("%s %dv%d | %d-%d", GetMapNameForServerState(), cl.discord.redTeam, cl.discord.blueTeam, cl.discord.redScore, cl.discord.blueScore);
		}

		return GetMapNameForServerState();
	}

	if ( cls.state <= CA_DISCONNECTED || cls.state == CA_CINEMATIC )
		return "";

	return GetState();
}

int64_t GetStartTimestamp() {
	int64_t elapsed = (cl.snap.serverTime - cl.discord.levelStartTime) / 1000;
	return ((int64_t)time(NULL)) - elapsed;
}

char *GetServerDetails() {
	if ( cls.state == CA_ACTIVE ) {
		if (com_sv_running->integer)
			return "Playing offline";
		
		return va("%s%s | %s", clc.demoplaying ? "(Demo) " : "", ReturnServerName(), GetGameType(qfalse));
	}

	if (cls.state > CA_DISCONNECTED && cls.state < CA_ACTIVE)
		return "";

	if ( cls.state <= CA_DISCONNECTED || cls.state == CA_CINEMATIC )
		return "In Menu";

	return NULL;
}

static void handleDiscordReady( const DiscordUser* connectedUser )
{
	Com_Printf( "*Discord: connected to user %s#%s - %s^7\n", connectedUser->username, connectedUser->discriminator, connectedUser->userId );
}

static void handleDiscordDisconnected( int errcode, const char* message )
{
	Com_Printf( "*Discord: disconnected (%d: %s^7)\n", errcode, message );
}

static void handleDiscordError( int errcode, const char* message )
{
	Com_Printf( "*Discord: Error - (%d: %s^7)\n", errcode, message );
}

static void handleDiscordJoin( const char* secret )
{
	char ip[22] = { 0 }; //xxx.xxx.xxx.xxx:xxxxx\0
	char password[106] = { 0 };
	int i = 0;
	netadr_t adr;
	qboolean skippedFsgame = qfalse;

	Com_Printf("*Discord: joining ^3(%s)^7\n", secret);

	// 
	//This implementation is broken in EJK, when servers do not have fs_game set in their .cfg.
	//If that's the case, then there will be an extra space in the secret in between the ip and password, which leads to the variables being incorrectly set or not being set at all.
	//Ideally, this whole implementation should be scrapped and reimplemented.
	// 
	//Let's do a workaround for this.

	while (*secret && *secret != ' ') //Parse the ip first.
	{
		if (i >= sizeof(ip)) break;

		ip[i++] = *secret;
		secret++;
	}

	ip[i] = '\0';
	i = 0;

	if (*ip)
	{
		if (NET_StringToAdr(ip, &adr))
		{
			if (!strchr(secret, '\"')) //Check for quotes in the string, if they appear, it means we join a passwordless server.
			{
				if (*secret == ' ' && *(secret + 1) == ' ') //No quotes have been found, check whether the string contains 2 spaces between ip and fsgame.
				{
					secret += 2; //We found 2 spaces, which means the server has no fs_game set. move the pointer by 2 to skip the spaces.

					while (*secret)
					{
						if (i >= sizeof(password)) break;

						password[i++] = *secret;	//Everything after the spaces is the server password.
						secret++;
					}
					password[i] = '\0';
				}
				else //1 space is found, which means the server has fs_game set.
				{
					secret++; //Increment the pointer by 1 to skip the space.
					while (*secret)
					{
						while (!skippedFsgame && *secret != ' ' && *secret) //the first word after the space is fs_game, we can skip that.
						{
							secret++;

							if (*secret == ' ') //We found the next space. Increment pointer again to skip it.
							{
								secret++;
								skippedFsgame = qtrue;
							}
						}

						if (i >= sizeof(password)) break;

						password[i++] = *secret; //Every character after the second space, is part of the password. 
						secret++;
					}
					password[i] = '\0';
				}

				Q_strstrip(password, "\";\r\n", NULL);

				Cbuf_AddText(va("set password %s; connect %i.%i.%i.%i:%hu\n", password, adr.ip[0], adr.ip[1], adr.ip[2], adr.ip[3], BigShort(adr.port)));
			}
			else
			{
				Cbuf_AddText(va("connect %i.%i.%i.%i:%hu\n", adr.ip[0], adr.ip[1], adr.ip[2], adr.ip[3], BigShort(adr.port)));
			}
		}
		else
		{
			Com_Printf("*Discord: Invalid IP address (%s)\n", ip);
		}
	}
	Com_Printf("*Discord: Failed to parse server information from join secret\n");
}

static void handleDiscordSpectate( const char* secret )
{
	//Com_Printf( "*Discord: spectating (%s^7)\n", secret );
}

static void handleDiscordJoinRequest( const DiscordUser* request )
{
	int response = -1;

	Com_Printf( "*Discord: join request from %s#%s - %s^7\n", request->username, request->discriminator, request->userId );

	if ( response != -1 ) {
		Discord_Respond( request->userId, response );
	}
}

static DiscordRichPresence discordPresence;
static DiscordEventHandlers handlers;
void CL_DiscordInitialize(void)
{
	Com_Memset( &handlers, 0, sizeof( handlers ) );
	handlers.ready = handleDiscordReady;
	handlers.disconnected = handleDiscordDisconnected;
	handlers.errored = handleDiscordError;
	handlers.joinGame = handleDiscordJoin;
	handlers.spectateGame = handleDiscordSpectate;
	handlers.joinRequest = handleDiscordJoinRequest;
	
	Discord_Initialize( APPLICATION_ID, &handlers, 1, "6020" );
	Discord_Register( APPLICATION_ID, NULL );

	Discord_UpdateHandlers( &handlers );

	cl_discordSharePassword = Cvar_Get("cl_discordSharePassword", "0", CVAR_ARCHIVE, "If set, sends password to Discord friends who request to join your game (warning: sends private client password, if in use!)");

	Q_strncpyz(cl.discord.hostName, "*Jedi*", sizeof(cl.discord.hostName));
	Q_strncpyz(cl.discord.mapName, "menu", sizeof(cl.discord.mapName));
	Q_strncpyz(cl.discord.fs_game, BASEGAME, sizeof(cl.discord.fs_game));
}

void CL_DiscordShutdown(void)
{
	Discord_Shutdown();
}

void CL_DiscordUpdatePresence(void)
{

	if (!cls.discordInitialized)
		return;

	if (cl.discord.gametype < GT_FFA || cl.discord.gametype >= numGameTypes)
		cl.discord.gametype = GT_FFA;

	Com_Memset( &discordPresence, 0, sizeof( discordPresence ) );
	
	if (cls.state == CA_ACTIVE)
		discordPresence.startTimestamp = GetStartTimestamp();
	discordPresence.state = GetServerState();
	discordPresence.details = GetServerDetails();
	discordPresence.largeImageKey = ReturnMapIcon();
	discordPresence.largeImageText = ReturnMapName();
	//if (cl_discordRichPresence->integer > 1 || cls.state < CA_ACTIVE || cl.discord.gametype == GT_FFA) {
		discordPresence.smallImageKey = GetState();
		discordPresence.smallImageText = GetState();
	/*}
	else {
		discordPresence.smallImageKey = GetGameType(qtrue);
		discordPresence.smallImageText = GetGameType(qfalse);
	}*/
	if (!clc.demoplaying && !com_sv_running->integer)
	{ //send join information blank since it won't do anything in this case
		discordPresence.partyId = PartyID(); // send join request in discord chat
		/*if (cl_discordRichPresence->integer > 1) {
			discordPresence.partySize = cls.state == CA_ACTIVE ? 1 : NULL;
			discordPresence.partyMax = cls.state == CA_ACTIVE ? ((cl.discord.maxPlayers - cl.discord.playerCount) + discordPresence.partySize) : NULL;
		}
		else {*/
			discordPresence.partySize = cls.state >= CA_LOADING ? cl.discord.playerCount : NULL;
			discordPresence.partyMax = cls.state >= CA_LOADING ? cl.discord.maxPlayers : NULL;
		//}
		discordPresence.joinSecret = joinSecret(); // server ip for discord join to execute
	}
	Discord_UpdatePresence( &discordPresence );

	Discord_RunCallbacks();
}

#endif