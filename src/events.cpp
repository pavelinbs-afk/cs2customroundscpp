#include "events.h"

#include <igameevents.h>

#include "customrounds.h"
#include "entity_utils.h"
#include "rounds.h"

static bool s_bRegistered = false;
static int s_iRetryThrottle = 0;

static const char* s_EventNames[] = {
	"weapon_fire",
	"weapon_reload",
	"round_start",
	"round_end",
};

static int EventSlot(IGameEvent* event, const char* key)
{
	CPlayerSlot slot = event->GetPlayerSlot(key);
	int i = slot.Get();
	return (i >= 0 && i < CR_MAXPLAYERS) ? i : -1;
}

class CREventListener final : public IGameEventListener2
{
public:
	void FireGameEvent(IGameEvent* event) override
	{
		if (!event)
			return;

		const char* name = event->GetName();
		if (!name)
			return;

		if (!strcmp(name, "weapon_fire"))
		{
			int iSlot = EventSlot(event, "userid");
			if (iSlot >= 0)
			{
				const char* weapon = event->GetString("weapon");
				Rounds_OnWeaponFireEvent(iSlot, weapon);
			}
			return;
		}

		if (!strcmp(name, "weapon_reload"))
		{
			int iSlot = EventSlot(event, "userid");
			if (iSlot >= 0)
				Rounds_OnWeaponReload(iSlot);
			return;
		}

		if (!strcmp(name, "round_end"))
		{
			Rounds_SetMode(CRRoundMode::None);
			return;
		}
	}
};

static CREventListener s_Listener;

void Events_TryRegister()
{
	if (s_bRegistered || !g_pGameEventManager)
		return;
	if (s_iRetryThrottle++ % 64 != 0)
		return;

	bool allOk = true;
	for (const char* name : s_EventNames)
	{
		if (g_pGameEventManager->FindListener(&s_Listener, name))
			continue;
		if (!g_pGameEventManager->AddListener(&s_Listener, name, true))
			allOk = false;
	}

	if (allOk)
	{
		s_bRegistered = true;
		CR_Log("game events hooked");
	}
}

void Events_Unregister()
{
	if (g_pGameEventManager)
		g_pGameEventManager->RemoveListener(&s_Listener);
	s_bRegistered = false;
}

void Events_OnStartupServer()
{
	s_bRegistered = false;
	s_iRetryThrottle = 0;
	EntityUtils_RetryGrenadeFactories();
	Rounds_OnStartupServer();
}

bool Events_AreRegistered()
{
	return s_bRegistered;
}
