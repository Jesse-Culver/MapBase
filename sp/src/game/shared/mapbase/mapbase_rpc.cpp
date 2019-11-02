//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Mapbase's RPC implementation.
//
// $NoKeywords: $
//=============================================================================//

#include "cbase.h"

#ifdef CLIENT_DLL

#ifdef STEAM_RPC
#include "clientsteamcontext.h"
#include "steam/steamclientpublic.h"
#endif

#ifdef DISCORD_RPC
#include "discord_rpc.h"
#include <time.h>
#endif

#include "c_playerresource.h"

#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// From mapbase_shared.cpp
extern const char *g_MapName;

// The game's name found in gameinfo.txt. Mostly used for Discord RPC.
extern char g_iszGameName[128];

#ifdef MAPBASE_RPC
void MapbaseRPC_CVarToggle( IConVar *var, const char *pOldString, float flOldValue );

ConVar mapbase_rpc_enabled("mapbase_rpc_enabled", "1", FCVAR_ARCHIVE, "Controls whether Mapbase's RPC stuff is enabled on this client.", MapbaseRPC_CVarToggle);

//-----------------------------------------------------------------------------
// RPC Stuff
// 
// Mapbase has some special "RPC" integration stuff for things like Discord.
// There's a section that goes into more detail below.
//-----------------------------------------------------------------------------

void MapbaseRPC_Init();
void MapbaseRPC_Shutdown();

void MapbaseRPC_Update( int iType, const char *pMapName );
void MapbaseRPC_Update( int iRPCMask, int iType, const char *pMapName );

#ifdef STEAM_RPC
void MapbaseRPC_UpdateSteam( int iType, const char *pMapName );
#endif

#ifdef DISCORD_RPC
void MapbaseRPC_UpdateDiscord( int iType, const char *pMapName );
void MapbaseRPC_GetDiscordParameters( DiscordRichPresence &discordPresence, int iType, const char *pMapName );
#endif

enum RPCClients_t
{
	RPC_STEAM,
	RPC_DISCORD,

	NUM_RPCS,
};

static const char *g_pszRPCNames[] = {
	"Steam",
	"Discord",
};

// This is a little dodgy, but it stops us from having to add spawnflag definitions for each RPC.
#define RPCFlag(rpc) (1 << rpc)

// The global game_metadata entity.
// There can be only one...for each RPC.
static EHANDLE g_Metadata[NUM_RPCS];

#define CMapbaseMetadata C_MapbaseMetadata

// Don't update constantly
#define RPC_UPDATE_COOLDOWN 5.0f

// How long to wait before updating in case multiple variables are changing
#define RPC_UPDATE_WAIT 0.25f
#endif

class CMapbaseMetadata : public CBaseEntity
{
public:
	DECLARE_DATADESC();
	DECLARE_NETWORKCLASS();
	DECLARE_CLASS( CMapbaseMetadata, CBaseEntity );

#ifdef CLIENT_DLL
	~C_MapbaseMetadata()
	{
		for (int i = 0; i < NUM_RPCS; i++)
		{
			if (g_Metadata[i] == this)
			{
				g_Metadata[i] = NULL;
			}
		}
	}

	void OnDataChanged( DataUpdateType_t updateType )
	{
		if (updateType == DATA_UPDATE_CREATED)
		{
			for (int i = 0; i < NUM_RPCS; i++)
			{
				// See if we're updating this RPC.
				if (m_spawnflags & RPCFlag(i))
				{
					if (g_Metadata[i])
					{
						Warning("Warning: Metadata entity for %s already exists, replacing with new one\n", g_pszRPCNames[i]);

						// Inherit their update timer
						m_flRPCUpdateTimer = static_cast<C_MapbaseMetadata*>(g_Metadata[i].Get())->m_flRPCUpdateTimer;

						g_Metadata[i].Get()->Remove();
					}

					DevMsg("Becoming metadata entity for %s\n", g_pszRPCNames[i]);
					g_Metadata[i] = this;
				}
			}
		}

		// Avoid spamming updates
		if (gpGlobals->curtime > (m_flRPCUpdateTimer - RPC_UPDATE_WAIT))
		{
			// Multiple variables might be changing, wait until they're probably all finished
			m_flRPCUpdateTimer = gpGlobals->curtime + RPC_UPDATE_WAIT;
		}

		DevMsg("Metadata changed; updating in %f\n", m_flRPCUpdateTimer - gpGlobals->curtime);

		// Update when the cooldown is over
		SetNextClientThink( m_flRPCUpdateTimer );
	}

	void ClientThink()
	{
		// NOTE: Client thinking should be limited by the update timer!
		UpdateRPCThink();

		// Wait until our data is changed again
		SetNextClientThink( CLIENT_THINK_NEVER );
	}

	void UpdateRPCThink()
	{
		DevMsg("Global metadata entity: %s\n", g_Metadata != NULL ? "Valid" : "Invalid!?");

		MapbaseRPC_Update(m_spawnflags, RPCSTATE_UPDATE, g_MapName);

		m_flRPCUpdateTimer = gpGlobals->curtime + RPC_UPDATE_COOLDOWN;
	}
#else
	int UpdateTransmitState()	// always send to all clients
	{
		return SetTransmitState( FL_EDICT_ALWAYS );
	}
#endif

#ifdef CLIENT_DLL
	// Built-in update spam limiter
	float m_flRPCUpdateTimer = RPC_UPDATE_COOLDOWN;

	char m_iszRPCState[128];
	char m_iszRPCDetails[128];

	int		m_spawnflags;
#else
	CNetworkVar( string_t, m_iszRPCState );
	CNetworkVar( string_t, m_iszRPCDetails );
#endif

	// TODO: Player-specific control
	//CNetworkVar( int, m_iLimitingID );
};

LINK_ENTITY_TO_CLASS( game_metadata, CMapbaseMetadata );

IMPLEMENT_NETWORKCLASS_ALIASED(MapbaseMetadata, DT_MapbaseMetadata)

BEGIN_NETWORK_TABLE_NOBASE(CMapbaseMetadata, DT_MapbaseMetadata)

#ifdef CLIENT_DLL
	RecvPropString(RECVINFO(m_iszRPCState)),
	RecvPropString(RECVINFO(m_iszRPCDetails)),
	RecvPropInt( RECVINFO( m_spawnflags ) ),
#else
	SendPropStringT(SENDINFO(m_iszRPCState) ),
	SendPropStringT(SENDINFO(m_iszRPCDetails) ),
	SendPropInt( SENDINFO(m_spawnflags), 8, SPROP_UNSIGNED ),
#endif

END_NETWORK_TABLE()

BEGIN_DATADESC( CMapbaseMetadata )

	// Inputs
	DEFINE_INPUT( m_iszRPCState, FIELD_STRING, "SetRPCState" ),
	DEFINE_INPUT( m_iszRPCDetails, FIELD_STRING, "SetRPCDetails" ),

END_DATADESC()

#ifdef MAPBASE_RPC
//-----------------------------------------------------------------------------
// Purpose: Mapbase's special integration with rich presence clients, most notably Discord.
// 
// This only has Discord as of writing, but similar/derived implementaton could expand to
// other clients in the future, maybe even Steam.
//-----------------------------------------------------------------------------

//-----------------------------------------
// !!! FOR MODS !!!
// 
// Create your own Discord "application" if you want to change what info/images show up, etc.
// You can find the convar that controls this in cdll_client_int.cpp.
// 
// This code automatically shows the mod's title in the details, but it's easy to change if you want things to be chapter-specific, etc.
// 
//-----------------------------------------

static ConVar cl_discord_appid("cl_discord_appid", "637699494229835787", FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT);
static int64_t startTimestamp = time(0);

//

int MapbaseRPC_GetPlayerCount()
{
	int iNumPlayers = 0;

	if (g_PR)
	{
		for (; iNumPlayers <= gpGlobals->maxClients; iNumPlayers++)
		{
			if (!g_PR->IsConnected( iNumPlayers ))
				break;
		}
	}

	return iNumPlayers;
}

//-----------------------------------------------------------------------------
// Discord RPC handlers
//-----------------------------------------------------------------------------
static void HandleDiscordReady(const DiscordUser* connectedUser)
{
	DevMsg("Discord: Connected to user %s#%s - %s\n",
		connectedUser->username,
		connectedUser->discriminator,
		connectedUser->userId);
}

static void HandleDiscordDisconnected(int errcode, const char* message)
{
	DevMsg("Discord: Disconnected (%d: %s)\n", errcode, message);
}

static void HandleDiscordError(int errcode, const char* message)
{
	DevMsg("Discord: Error (%d: %s)\n", errcode, message);
}

static void HandleDiscordJoin(const char* secret)
{
	// Not implemented
}

static void HandleDiscordSpectate(const char* secret)
{
	// Not implemented
}

static void HandleDiscordJoinRequest(const DiscordUser* request)
{
	// Not implemented
}

void MapbaseRPC_Init()
{
	// Steam RPC
	if (steamapicontext->SteamFriends())
		steamapicontext->SteamFriends()->ClearRichPresence();

	// Discord RPC
	DiscordEventHandlers handlers;
	memset(&handlers, 0, sizeof(handlers));
	
	handlers.ready = HandleDiscordReady;
	handlers.disconnected = HandleDiscordDisconnected;
	handlers.errored = HandleDiscordError;
	handlers.joinGame = HandleDiscordJoin;
	handlers.spectateGame = HandleDiscordSpectate;
	handlers.joinRequest = HandleDiscordJoinRequest;

	char appid[255];
	sprintf(appid, "%d", engine->GetAppID());
	Discord_Initialize(cl_discord_appid.GetString(), &handlers, 1, appid);

	if (!g_bTextMode)
	{
		DiscordRichPresence discordPresence;
		memset(&discordPresence, 0, sizeof(discordPresence));

		MapbaseRPC_GetDiscordParameters(discordPresence, RPCSTATE_INIT, NULL);

		discordPresence.startTimestamp = startTimestamp;

		Discord_UpdatePresence(&discordPresence);
	}
}

void MapbaseRPC_Shutdown()
{
	// Discord RPC
	Discord_Shutdown();

	// Steam RPC
	if (steamapicontext->SteamFriends())
		steamapicontext->SteamFriends()->ClearRichPresence();
}

void MapbaseRPC_Update( int iType, const char *pMapName )
{
	// All RPCs
	MapbaseRPC_Update( INT_MAX, iType, pMapName );
}

void MapbaseRPC_Update( int iRPCMask, int iType, const char *pMapName )
{
	if (iRPCMask & RPCFlag(RPC_STEAM))
		MapbaseRPC_UpdateSteam(iType, pMapName);
	if (iRPCMask & RPCFlag(RPC_DISCORD))
		MapbaseRPC_UpdateDiscord(iType, pMapName);
}

#ifdef STEAM_RPC
void MapbaseRPC_UpdateSteam( int iType, const char *pMapName )
{
	const char *pszStatus = NULL;

	if (g_Metadata[RPC_STEAM] != NULL)
	{
		C_MapbaseMetadata *pMetadata = static_cast<C_MapbaseMetadata*>(g_Metadata[RPC_STEAM].Get());

		if (pMetadata->m_iszRPCDetails[0] != NULL)
			pszStatus = pMetadata->m_iszRPCDetails;
		else if (pMetadata->m_iszRPCState[0] != NULL)
			pszStatus = pMetadata->m_iszRPCState;
		else
		{
			if (engine->IsLevelMainMenuBackground())
				pszStatus = VarArgs("Main Menu (%s)", pMapName ? pMapName : "N/A");
			else
				pszStatus = VarArgs("Map: %s", pMapName ? pMapName : "N/A");
		}
	}
	else
	{
		switch (iType)
		{
			case RPCSTATE_INIT:
			case RPCSTATE_LEVEL_SHUTDOWN:
				{
					pszStatus = "Main Menu";
				} break;
			case RPCSTATE_LEVEL_INIT:
			default:
				{
					// Say we're in the main menu if it's a background map
					if (engine->IsLevelMainMenuBackground())
					{
						pszStatus = VarArgs("Main Menu (%s)", pMapName ? pMapName : "N/A");
					}
					else
					{
						pszStatus = VarArgs("Map: %s", pMapName ? pMapName : "N/A");
					}
				} break;
		}
	}

	DevMsg( "Updating Steam\n" );

	if (pszStatus)
	{
		steamapicontext->SteamFriends()->SetRichPresence( "gamestatus", pszStatus );
		steamapicontext->SteamFriends()->SetRichPresence( "steam_display", "#SteamRPC_Status" );

		if (gpGlobals->maxClients > 1)
		{
			// Players in server
			const CSteamID *serverID = serverengine->GetGameServerSteamID();
			if (serverID)
			{
				char szGroupID[32];
				Q_snprintf(szGroupID, sizeof(szGroupID), "%i", serverID->GetAccountID());

				char szGroupSize[8];
				Q_snprintf(szGroupSize, sizeof(szGroupSize), "%i", MapbaseRPC_GetPlayerCount());

				steamapicontext->SteamFriends()->SetRichPresence( "steam_player_group", szGroupID );
				steamapicontext->SteamFriends()->SetRichPresence( "steam_player_group_size", szGroupSize );
			}
			else
			{
				DevWarning("Steam RPC cannot update player count (no server ID)\n");
			}
		}
	}
}
#endif

#ifdef DISCORD_RPC
// Game-Specfic Image
// These are specific to the Mapbase Discord application, so you'll want to modify this for your own mod.
#if HL2_EPISODIC
#define DISCORD_GAME_IMAGE "default_icon"
#define DISCORD_GAME_IMAGE_TEXT "discord.gg/SourceEngine"
#elif HL2_CLIENT_DLL
#define DISCORD_GAME_IMAGE "default_icon"
#define DISCORD_GAME_IMAGE_TEXT "discord.gg/SourceEngine"
#else
#define DISCORD_GAME_IMAGE "default_icon"
#define DISCORD_GAME_IMAGE_TEXT "discord.gg/SourceEngine"
#endif

void MapbaseRPC_GetDiscordParameters( DiscordRichPresence &discordPresence, int iType, const char *pMapName )
{
	static char details[128];
	static char state[128];

	details[0] = '\0';
	state[0] = '\0';

	if (g_Metadata[RPC_DISCORD] != NULL)
	{
		C_MapbaseMetadata *pMetadata = static_cast<C_MapbaseMetadata*>(g_Metadata[RPC_DISCORD].Get());

		if (pMetadata->m_iszRPCState[0] != NULL)
			Q_strncpy( state, pMetadata->m_iszRPCState, sizeof(state) );
		else
			Q_strncpy( state, g_iszGameName, sizeof(state) );

		if (pMetadata->m_iszRPCDetails[0] != NULL)
			Q_strncpy( details, pMetadata->m_iszRPCDetails, sizeof(details) );
		else
		{
			if (engine->IsLevelMainMenuBackground())
				Q_snprintf( details, sizeof(details), "Main Menu (%s)", pMapName ? pMapName : "N/A" );
			else
				Q_snprintf( details, sizeof(details), "%s", pMapName ? pMapName : "N/A" );
		}
	}
	else
	{
		Q_strncpy( state, g_iszGameName, sizeof(state) );

		switch (iType)
		{
			case RPCSTATE_INIT:
			case RPCSTATE_LEVEL_SHUTDOWN:
				{
					Q_strncpy( details, "Main Menu", sizeof(details) );
				} break;
			case RPCSTATE_LEVEL_INIT:
			default:
				{
					// Say we're in the main menu if it's a background map
					if (engine->IsLevelMainMenuBackground())
					{
						Q_snprintf( details, sizeof(details), "Main Menu (%s)", pMapName ? pMapName : "N/A" );
					}
					else
					{
						Q_snprintf( details, sizeof(details), "%s", pMapName ? pMapName : "N/A" );
					}
				} break;
		}
	}

	if (gpGlobals->maxClients > 1)
	{
		Q_snprintf( details, sizeof(details), "%s (%i/%i)", details, MapbaseRPC_GetPlayerCount(), gpGlobals->maxClients );
	}

	if (state[0] != '\0')
		discordPresence.state = state;
	if (details[0] != '\0')
		discordPresence.details = details;

	// Generic Mapbase logo. Specific to the Mapbase Discord application.
	discordPresence.smallImageKey = "";
	discordPresence.smallImageText = "discord.gg/SourceEngine";

#ifdef DISCORD_GAME_IMAGE
	discordPresence.largeImageKey = DISCORD_GAME_IMAGE;
	discordPresence.largeImageText = DISCORD_GAME_IMAGE_TEXT;
#endif
}

void MapbaseRPC_UpdateDiscord( int iType, const char *pMapName )
{
	DiscordRichPresence discordPresence;
	memset(&discordPresence, 0, sizeof(discordPresence));

	DevMsg("Updating Discord\n");

	discordPresence.startTimestamp = startTimestamp;

	MapbaseRPC_GetDiscordParameters( discordPresence, iType, pMapName );

	Discord_UpdatePresence(&discordPresence);
}

void MapbaseRPC_CVarToggle( IConVar *var, const char *pOldString, float flOldValue )
{
	if (flOldValue <= 0 && mapbase_rpc_enabled.GetInt() > 0)
	{
		// Turning on
		MapbaseRPC_Init();
		MapbaseRPC_Update( g_MapName != NULL ? RPCSTATE_UPDATE : RPCSTATE_INIT, g_MapName );
	}
	else if (mapbase_rpc_enabled.GetInt() <= 0)
	{
		// Turning off
		MapbaseRPC_Shutdown();
	}
}
#endif

#endif
