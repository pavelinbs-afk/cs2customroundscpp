#include "sound_hook.h"

#include <cstring>

#include <khook/memory.hpp>
#include <tier0/dbg.h>

#include "customrounds.h"
#include "vtable_finder.h"

#ifdef _WIN32
#define SERVER_LIB "server.dll"
#else
#define SERVER_LIB "/libserver.so"
#endif

namespace
{
struct SoundPatchSite
{
	void* pFunc = nullptr;
	uint8_t origBytes[8] = {0};
	size_t patchLen = 0;
	bool patched = false;
};

// xor rax, rax; ret — return 0 / null guid, drop the sound.
static const uint8_t kMuteRetZero[] = {0x48, 0x31, 0xC0, 0xC3};
static const uint8_t kMuteRetVoid[] = {0xC3};

static constexpr int kMaxSoundSites = 12;
static SoundPatchSite s_Sites[kMaxSoundSites];
static int s_SiteCount = 0;

static const char* kSigEmitSoundFilter =
	"55 48 89 E5 53 48 89 FB 48 83 EC ? E8 ? ? ? ? 48 89 D8 48 8B 5D ? C9 C3 CC CC CC CC CC CC 48 B8";
static const char* kSigEmitSoundFilterFallback =
	"55 48 89 E5 41 56 49 89 D6 41 55 41 89 F5 41 54 48";

static const char* kSigStartSoundEvent =
	"48 B8 ? ? ? ? 08 00 00 C0 55 48 89 E5 41 57 45 89 C7 41 56 41 55 4C 8D 6D C0 41 54 41 89 CC 53 48 89 FB 48 8D 3D";
static const char* kSigStartSoundEventAlt =
	"48 B8 ? ? ? ? 08 00 00 C0 55 48 89 E5 41 57 45 89 C7 41 56 41 55 4C 8D 6D ? 41 54 41 89 CC 53 48 89 FB";

static const char* kSigEmitSoundParams =
	"48 B8 ? ? ? ? ? ? ? ? 55 48 89 E5 41 55 41 54 49 89 FC 53 48 89 F3";
static const char* kSigEmitSoundParamsAlt =
	"48 B8 ? ? ? ? ? ? ? ? 55 48 89 E5 41 55 41 54 49 89 FC 53 48 89 F3 48 83 EC";

static bool BindSite(SoundPatchSite& site, void* pFunc, const uint8_t* patch, size_t patchLen)
{
	if (!pFunc)
		return false;

	for (int i = 0; i < s_SiteCount; i++)
	{
		if (s_Sites[i].pFunc == pFunc)
			return false;
	}

	if (s_SiteCount >= kMaxSoundSites)
		return false;

	site = {};
	site.pFunc = pFunc;
	site.patchLen = patchLen;
	memcpy(site.origBytes, pFunc, patchLen);
	s_Sites[s_SiteCount++] = site;
	return true;
}

static bool TryBindSignature(const char* sig, const uint8_t* patch, size_t patchLen, const char* label)
{
	void* pFunc = FindFunctionSignature(SERVER_LIB, ".text", sig);
	if (!pFunc)
		return false;

	if (!BindSite(s_Sites[s_SiteCount], pFunc, patch, patchLen))
		return false;

	CR_Log("%s @ %p", label, pFunc);
	return true;
}

static void ApplyPatch(SoundPatchSite& site, const uint8_t* patch, size_t patchLen)
{
	if (!site.pFunc || site.patched)
		return;

	KHook::Memory::SetAccess(site.pFunc, patchLen, KHook::Memory::READ | KHook::Memory::WRITE | KHook::Memory::EXECUTE);
	memcpy(site.origBytes, site.pFunc, patchLen);
	memcpy(site.pFunc, patch, patchLen);
	site.patched = true;
}

static void RestorePatch(SoundPatchSite& site)
{
	if (!site.pFunc || !site.patched)
		return;

	KHook::Memory::SetAccess(site.pFunc, site.patchLen, KHook::Memory::READ | KHook::Memory::WRITE | KHook::Memory::EXECUTE);
	memcpy(site.pFunc, site.origBytes, site.patchLen);
	KHook::Memory::SetAccess(site.pFunc, site.patchLen, KHook::Memory::READ | KHook::Memory::EXECUTE);
	site.patched = false;
}

} // namespace

bool SoundHook_Install()
{
	s_SiteCount = 0;

	int found = 0;

	if (TryBindSignature(kSigEmitSoundFilter, kMuteRetVoid, sizeof(kMuteRetVoid), "EmitSoundFilter"))
		found++;
	else if (TryBindSignature(kSigEmitSoundFilterFallback, kMuteRetVoid, sizeof(kMuteRetVoid), "EmitSoundFilterFallback"))
		found++;

	if (TryBindSignature(kSigStartSoundEvent, kMuteRetZero, sizeof(kMuteRetZero), "StartSoundEvent"))
		found++;
	if (TryBindSignature(kSigStartSoundEventAlt, kMuteRetZero, sizeof(kMuteRetZero), "StartSoundEventAlt"))
		found++;

	if (TryBindSignature(kSigEmitSoundParams, kMuteRetZero, sizeof(kMuteRetZero), "EmitSoundParams"))
		found++;
	if (TryBindSignature(kSigEmitSoundParamsAlt, kMuteRetZero, sizeof(kMuteRetZero), "EmitSoundParamsAlt"))
		found++;

	if (!found)
	{
		Warning("[CR] No sound hooks found — NoSound round will not mute sounds\n");
		return false;
	}

	CR_Log("sound hooks bound: %d site(s)", found);
	return true;
}

void SoundHook_SetMuted(bool muted)
{
	for (int i = 0; i < s_SiteCount; i++)
	{
		SoundPatchSite& site = s_Sites[i];
		const bool wantsZero = site.patchLen == sizeof(kMuteRetZero);
		if (muted)
			ApplyPatch(site, wantsZero ? kMuteRetZero : kMuteRetVoid, site.patchLen);
		else
			RestorePatch(site);
	}
}

void SoundHook_Uninstall()
{
	SoundHook_SetMuted(false);
	s_SiteCount = 0;
}
