#include "customrounds.h"

#include <schemasystem/schemasystem.h>
#include <interfaces/interfaces.h>

#include "events.h"
#include "commands.h"
#include "entity_utils.h"
#include "grenade_shot_hook.h"
#include "invert_hook.h"
#include "rounds.h"
#include "sound_hook.h"
#include "vtable_finder.h"

CustomRoundsPlugin g_CRPlugin;
PLUGIN_EXPOSE(CustomRoundsPlugin, g_CRPlugin);

IVEngineServer2* g_pEngine = nullptr;
ISource2Server* g_pServer = nullptr;
IGameEventManager2* g_pGameEventManager = nullptr;
INetworkServerService* g_pNetServerService = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;

CGameEntitySystem* GameEntitySystem()
{
	return g_pGameEntitySystem;
}

static void* g_pEventMgrVtbl = nullptr;
static void* g_pEntSysVtbl = nullptr;

#ifdef _WIN32
#define SERVER_LIB "server.dll"
#else
#define SERVER_LIB "/libserver.so"
#endif

// Engine only passes this by reference; KHook needs a complete type for sizeof.
class GameSessionConfiguration_t
{
};

template <typename CLASS, typename RETURN, typename... ARGS>
static void AddGlobalByVtbl(KHook::Virtual<CLASS, RETURN, ARGS...>& hook, void* vtbl)
{
	struct { void* v; } fake{ vtbl };
	hook.AddGlobal(reinterpret_cast<CLASS*>(&fake));
}

template <typename CLASS, typename RETURN, typename... ARGS>
static void RemoveGlobalByVtbl(KHook::Virtual<CLASS, RETURN, ARGS...>& hook, void* vtbl)
{
	if (!vtbl)
		return;
	struct { void* v; } fake{ vtbl };
	hook.RemoveGlobal(reinterpret_cast<CLASS*>(&fake));
}

CustomRoundsPlugin::CustomRoundsPlugin() :
	m_GameFrame(&ISource2Server::GameFrame, this, nullptr, &CustomRoundsPlugin::Hook_GameFrame),
	m_StartupServer(&INetworkServerService::StartupServer, this, nullptr, &CustomRoundsPlugin::Hook_StartupServer),
	m_LoadEventsFromFile(&IGameEventManager2::LoadEventsFromFile, this, &CustomRoundsPlugin::Hook_LoadEventsFromFile, nullptr),
	m_EntitySystemSpawn(&CEntitySystem::Spawn, this, nullptr, &CustomRoundsPlugin::Hook_EntitySystemSpawn)
{
}

void CR_Log(const char* fmt, ...)
{
	char buf[512];
	va_list va;
	va_start(va, fmt);
	V_vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	ConColorMsg(Color(120, 180, 255, 255), "[CR] %s\n", buf);
}

static void Cmd_CustomRoundsMM(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		Msg("Usage: customrounds_mm <0|1|2|3|4>  (0=off, 1=onebullet, 2=nosound, 3=invert, 4=grenadeshot)\n");
		return;
	}

	int mode = atoi(args.Arg(1));
	switch (mode)
	{
		case 0: Rounds_SetMode(CRRoundMode::None); break;
		case 1: Rounds_SetMode(CRRoundMode::OneBullet); break;
		case 2: Rounds_SetMode(CRRoundMode::NoSound); break;
		case 3: Rounds_SetMode(CRRoundMode::Invert); break;
		case 4: Rounds_SetMode(CRRoundMode::GrenadeShot); break;
		default:
			Msg("Invalid mode %d (use 0..4)\n", mode);
			return;
	}

	Msg("[CR] customrounds_mm -> %d\n", mode);
}

bool CustomRoundsPlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pEngine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetServerFactory, g_pServer, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pNetServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);

	g_SMAPI->AddListener(this, this);

	m_GameFrame.Add(g_pServer);
	m_StartupServer.Add(g_pNetServerService);

	g_pEventMgrVtbl = FindVirtualTable(SERVER_LIB, "CGameEventManager");
	if (!g_pEventMgrVtbl)
	{
		V_strncpy(error, "Failed to locate CGameEventManager vtable", maxlen);
		return false;
	}
	AddGlobalByVtbl(m_LoadEventsFromFile, g_pEventMgrVtbl);

	g_pEntSysVtbl = FindVirtualTable(SERVER_LIB, "CGameEntitySystem");
	if (!g_pEntSysVtbl)
	{
		V_strncpy(error, "Failed to locate CGameEntitySystem vtable", maxlen);
		return false;
	}
	AddGlobalByVtbl(m_EntitySystemSpawn, g_pEntSysVtbl);

	Commands_Register();
	new ConCommand("customrounds_mm", Cmd_CustomRoundsMM, "customrounds_mm <0|1|2|3|4>", FCVAR_GAMEDLL);

	const bool bEntityUtils = EntityUtils_Init();
	const bool bSoundHook = SoundHook_Install();
	const bool bInvertHook = InvertHook_Install();
	const bool bGrenadeShotHook = GrenadeShotHook_Install();

	CR_Log("loaded %s v%s (%s)", GetName(), GetVersion(), GetDate());
	CR_Log("modes: 0=off 1=onebullet 2=nosound 3=invert 4=grenadeshot | entity=%s sound=%s invert=%s grenade_dmg=%s",
		bEntityUtils ? "ok" : "FAILED",
		bSoundHook ? "ok" : "FAILED",
		bInvertHook ? "ok" : "FAILED",
		bGrenadeShotHook ? "ok" : "FAILED");
	META_CONPRINTF("[%s] Loaded %s v%s (%s) — customrounds_mm 0|1|2|3|4 entity=%s sound=%s invert=%s grenade=%s\n",
		GetLogTag(), GetName(), GetVersion(), GetDate(),
		bEntityUtils ? "ok" : "FAIL",
		bSoundHook ? "ok" : "FAIL",
		bInvertHook ? "ok" : "FAIL",
		bGrenadeShotHook ? "ok" : "FAIL");

	return true;
}

bool CustomRoundsPlugin::Unload(char* error, size_t maxlen)
{
	Events_Unregister();
	GrenadeShotHook_Uninstall();
	SoundHook_Uninstall();
	InvertHook_Uninstall();
	EntityUtils_Shutdown();

	CR_Log("unloaded");
	META_CONPRINTF("[%s] Unloaded %s\n", GetLogTag(), GetName());

	m_GameFrame.Remove(g_pServer);
	m_StartupServer.Remove(g_pNetServerService);
	RemoveGlobalByVtbl(m_LoadEventsFromFile, g_pEventMgrVtbl);
	RemoveGlobalByVtbl(m_EntitySystemSpawn, g_pEntSysVtbl);
	g_pEventMgrVtbl = nullptr;
	g_pEntSysVtbl = nullptr;

	ConVar_Unregister();
	return true;
}

KHook::Return<void> CustomRoundsPlugin::Hook_GameFrame(ISource2Server*, bool simulating, bool bFirstTick, bool bLastTick)
{
	if (simulating)
	{
		Events_TryRegister();
		Rounds_OnGameFrame();
	}
	return { KHook::Action::Ignore };
}

KHook::Return<void> CustomRoundsPlugin::Hook_StartupServer(INetworkServerService*, const GameSessionConfiguration_t& config, ISource2WorldSession*, const char*)
{
	Events_OnStartupServer();
	return { KHook::Action::Ignore };
}

KHook::Return<int> CustomRoundsPlugin::Hook_LoadEventsFromFile(IGameEventManager2* pThis, const char* filename, bool bSearchAll)
{
	if (!g_pGameEventManager && pThis)
	{
		g_pGameEventManager = pThis;
		CR_Log("captured IGameEventManager2");
	}
	return { KHook::Action::Ignore };
}

KHook::Return<void> CustomRoundsPlugin::Hook_EntitySystemSpawn(CEntitySystem* pThis, int nCount, const EntitySpawnInfo_t* pInfo)
{
	CGameEntitySystem* pSys = reinterpret_cast<CGameEntitySystem*>(pThis);
	if (pSys && g_pGameEntitySystem != pSys)
	{
		g_pGameEntitySystem = pSys;
		CR_Log("captured CGameEntitySystem");
	}
	return { KHook::Action::Ignore };
}
