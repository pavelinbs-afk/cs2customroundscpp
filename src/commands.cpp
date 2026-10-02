#include "commands.h"

#include "customrounds.h"
#include "entity_utils.h"
#include "events.h"
#include "grenade_shot_hook.h"
#include "invert_hook.h"
#include "rounds.h"
#include "sound_hook.h"

#include <tier1/convar.h>
#include <icvar.h>
#include <ctime>
#include <cstdio>

#ifndef CR_GIT_COMMIT
#define CR_GIT_COMMIT "unknown"
#endif
#ifndef CR_GIT_BRANCH
#define CR_GIT_BRANCH "unknown"
#endif
#ifndef CR_GIT_COMMIT_DATE
#define CR_GIT_COMMIT_DATE "unknown"
#endif
#ifndef CR_GIT_COMMIT_SUBJECT
#define CR_GIT_COMMIT_SUBJECT "unknown"
#endif
#ifndef CR_BUILD_TIMESTAMP
#define CR_BUILD_TIMESTAMP __DATE__ " " __TIME__
#endif

void Commands_Register()
{
	// Static CON_COMMAND_F objects are queued until ConVar_Register runs.
	ConVar_Register(FCVAR_RELEASE | FCVAR_GAMEDLL);
}

static void FormatLocalNow(char* out, size_t outSize)
{
	if (!out || outSize == 0)
		return;
	std::time_t now = std::time(nullptr);
	std::tm tmLocal{};
#if defined(_WIN32)
	localtime_s(&tmLocal, &now);
#else
	localtime_r(&now, &tmLocal);
#endif
	std::strftime(out, outSize, "%Y-%m-%d %H:%M:%S %z", &tmLocal);
}

static const char* RoundModeName(CRRoundMode mode)
{
	switch (mode)
	{
		case CRRoundMode::None: return "off";
		case CRRoundMode::OneBullet: return "onebullet";
		case CRRoundMode::NoSound: return "nosound";
		case CRRoundMode::Invert: return "invert";
		case CRRoundMode::GrenadeShot: return "grenadeshot";
		default: return "unknown";
	}
}

CON_COMMAND_F(cr_status, "CustomRounds status: version, hooks, mode, git build info", FCVAR_RELEASE | FCVAR_GAMEDLL)
{
	(void)context;
	(void)args;

	char nowBuf[64];
	FormatLocalNow(nowBuf, sizeof(nowBuf));

	const char* map = "?";
	if (CGlobalVars* gv = GetGlobals())
	{
		const char* m = STRING(gv->mapname);
		if (m && m[0])
			map = m;
	}

	const CRRoundMode mode = Rounds_GetMode();

	Msg("========== [customrounds] cr_status ==========\n");
	Msg("  plugin      : %s\n", g_CRPlugin.GetName());
	Msg("  version     : %s\n", g_CRPlugin.GetVersion());
	Msg("  author      : %s\n", g_CRPlugin.GetAuthor());
	Msg("  compile     : %s (plugin GetDate)\n", g_CRPlugin.GetDate());
	Msg("  build_ts    : %s\n", CR_BUILD_TIMESTAMP);
	Msg("  today       : %s\n", nowBuf);
	Msg("  git_branch  : %s\n", CR_GIT_BRANCH);
	Msg("  git_commit  : %s\n", CR_GIT_COMMIT);
	Msg("  git_date    : %s\n", CR_GIT_COMMIT_DATE);
	Msg("  git_subject : %s\n", CR_GIT_COMMIT_SUBJECT);
	Msg("  map         : %s\n", map);
	Msg("  interfaces  : engine=%d server=%d events_mgr=%d entsys=%d cvar=%d\n",
		g_pEngine ? 1 : 0,
		g_pServer ? 1 : 0,
		g_pGameEventManager ? 1 : 0,
		g_pGameEntitySystem ? 1 : 0,
		g_pCVar ? 1 : 0);
	Msg("  events_hook : %s\n", Events_AreRegistered() ? "OK (listeners attached)" : "PENDING / not hooked yet");
	Msg("  round_mode  : %d (%s)\n", static_cast<int>(mode), RoundModeName(mode));
	Msg("  modules     : entity=%s create=%d spawn=%d accept=%d | nade HE=%d Molo=%d Decoy=%d Flash=%d | sound=%s sites=%d muted=%d | invert=%s | grenade_dmg=%s\n",
		EntityUtils_IsReady() ? "OK" : "FAIL",
		EntityUtils_HasCreate() ? 1 : 0,
		EntityUtils_HasDispatchSpawn() ? 1 : 0,
		EntityUtils_HasAcceptInput() ? 1 : 0,
		EntityUtils_HasFactoryForType(CRGrenadeType::HE) ? 1 : 0,
		EntityUtils_HasFactoryForType(CRGrenadeType::Molotov) ? 1 : 0,
		EntityUtils_HasFactoryForType(CRGrenadeType::Decoy) ? 1 : 0,
		EntityUtils_HasFactoryForType(CRGrenadeType::Flash) ? 1 : 0,
		SoundHook_IsInstalled() ? "OK" : "FAIL",
		SoundHook_GetSiteCount(),
		SoundHook_IsMuted() ? 1 : 0,
		InvertHook_IsInstalled() ? "OK" : "FAIL",
		GrenadeShotHook_IsInstalled() ? "OK" : "FAIL");
	Msg("  commands    : customrounds_mm <0|1|2|3|4>, customrounds_tp <slot> x y z yaw, customrounds_tp_ready, cr_status\n");
	Msg("================================================\n");
}
