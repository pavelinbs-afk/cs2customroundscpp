#pragma once

#include <ISmmPlugin.h>
#include <tier0/dbg.h>
#include <tier1/strtools.h>
#include <eiface.h>
#include <iserver.h>
#include <igameevents.h>
#include <icvar.h>
#include <entity2/entitysystem.h>

#define CR_MAXPLAYERS 64

class CustomRoundsPlugin final : public ISmmPlugin, public IMetamodListener
{
public:
	CustomRoundsPlugin();

	bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) override;
	bool Unload(char* error, size_t maxlen) override;

	const char* GetAuthor() override		{ return "pRfect"; }
	const char* GetName() override			{ return "Custom Rounds MM"; }
	const char* GetDescription() override	{ return "One bullet / no sound / WASD invert / grenade shot custom rounds"; }
	const char* GetURL() override			{ return ""; }
	const char* GetLicense() override		{ return "GPL"; }
	const char* GetVersion() override		{ return "1.3.0"; }
	const char* GetDate() override			{ return __DATE__; }
	const char* GetLogTag() override		{ return "CR"; }

public:
	KHook::Return<void> Hook_GameFrame(ISource2Server*, bool simulating, bool bFirstTick, bool bLastTick);
	KHook::Return<void> Hook_StartupServer(INetworkServerService*, const GameSessionConfiguration_t& config, ISource2WorldSession*, const char*);
	KHook::Return<int>  Hook_LoadEventsFromFile(IGameEventManager2* pThis, const char* filename, bool bSearchAll);
	KHook::Return<void> Hook_EntitySystemSpawn(CEntitySystem* pThis, int nCount, const EntitySpawnInfo_t* pInfo);

protected:
	KHook::Virtual<ISource2Server, void, bool, bool, bool> m_GameFrame;
	KHook::Virtual<INetworkServerService, void, const GameSessionConfiguration_t&, ISource2WorldSession*, const char*> m_StartupServer;
	KHook::Virtual<IGameEventManager2, int, const char*, bool> m_LoadEventsFromFile;
	KHook::Virtual<CEntitySystem, void, int, const EntitySpawnInfo_t*> m_EntitySystemSpawn;
};

extern CustomRoundsPlugin g_CRPlugin;

extern IVEngineServer2* g_pEngine;
extern ISource2Server* g_pServer;
extern IGameEventManager2* g_pGameEventManager;
extern INetworkServerService* g_pNetServerService;
extern CGameEntitySystem* g_pGameEntitySystem;

inline CGlobalVars* GetGlobals()
{
	return g_pEngine ? g_pEngine->GetServerGlobals() : nullptr;
}

void CR_Log(const char* fmt, ...);
