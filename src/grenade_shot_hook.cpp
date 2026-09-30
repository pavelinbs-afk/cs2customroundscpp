#include "grenade_shot_hook.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sys/mman.h>

#include <entity2/entitysystem.h>
#include <khook/memory.hpp>

#include "customrounds.h"
#include "rounds.h"
#include "vtable_finder.h"

#ifdef _WIN32
#define SERVER_LIB "server.dll"
#else
#define SERVER_LIB "/libserver.so"
#endif

namespace
{
// CBaseEntity::TakeDamageOld — CS2Fixes gamedata
static const char* kSigTakeDamageOldLinux =
	"55 66 0F EF C0 48 89 E5 41 57 41 56 41 55 49 89 FD 31 FF";

enum : int
{
	DMG_BULLET = 1 << 1, // 2
};

// Minimal CTakeDamageInfo layout (DumpSource2 / CS2Fixes).
struct CRTakeDamageInfo
{
	uint8_t pad0[0x4C];
	int32_t bitsDamageType; // 0x4C
};

static_assert(offsetof(CRTakeDamageInfo, bitsDamageType) == 0x4C, "CTakeDamageInfo bits offset");

using TakeDamageOldFn = int64_t (*)(CEntityInstance* pThis, CRTakeDamageInfo* pInfo, void* pResult);

static TakeDamageOldFn g_pTrampoline = nullptr;
static void* g_pTarget = nullptr;
// Full instructions through `xor edi, edi` — enough for 14-byte abs jmp.
static constexpr size_t kStolen = 19;
static uint8_t g_origBytes[kStolen] = {};
static bool g_hooked = false;

static bool IsPlayerPawnEntity(CEntityInstance* pEnt)
{
	if (!pEnt)
		return false;
	const char* name = pEnt->GetClassname();
	return name && !strcmp(name, "player");
}

static int64_t Hook_TakeDamageOld(CEntityInstance* pThis, CRTakeDamageInfo* pInfo, void* pResult)
{
	if (Rounds_GetMode() == CRRoundMode::GrenadeShot
		&& pThis && pInfo
		&& IsPlayerPawnEntity(pThis)
		&& (pInfo->bitsDamageType & DMG_BULLET))
	{
		// Valve returns 1 since 2025-10-15; supersede without applying bullet dmg.
		return 1;
	}

	return g_pTrampoline(pThis, pInfo, pResult);
}

static bool CreateTrampoline(void* pTarget)
{
	memcpy(g_origBytes, pTarget, kStolen);

	void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED)
		return false;

	auto* code = reinterpret_cast<uint8_t*>(page);
	memcpy(code, g_origBytes, kStolen);

	code[kStolen + 0] = 0xFF;
	code[kStolen + 1] = 0x25;
	code[kStolen + 2] = 0x00;
	code[kStolen + 3] = 0x00;
	code[kStolen + 4] = 0x00;
	code[kStolen + 5] = 0x00;
	const uint64_t cont = reinterpret_cast<uint64_t>(pTarget) + kStolen;
	memcpy(code + kStolen + 6, &cont, sizeof(cont));

	g_pTrampoline = reinterpret_cast<TakeDamageOldFn>(code);
	return true;
}

static bool InstallJump(void* pTarget)
{
	KHook::Memory::SetAccess(pTarget, kStolen, KHook::Memory::READ | KHook::Memory::WRITE | KHook::Memory::EXECUTE);

	uint8_t patch[kStolen] = {};
	patch[0] = 0xFF;
	patch[1] = 0x25;
	patch[2] = 0x00;
	patch[3] = 0x00;
	patch[4] = 0x00;
	patch[5] = 0x00;
	const uint64_t dest = reinterpret_cast<uint64_t>(&Hook_TakeDamageOld);
	memcpy(patch + 6, &dest, sizeof(dest));
	for (size_t i = 14; i < kStolen; ++i)
		patch[i] = 0x90;

	memcpy(pTarget, patch, kStolen);
	KHook::Memory::SetAccess(pTarget, kStolen, KHook::Memory::READ | KHook::Memory::EXECUTE);
	return true;
}

} // namespace

bool GrenadeShotHook_Install()
{
	if (g_hooked)
		return true;

	void* pFunc = FindFunctionSignature(SERVER_LIB, ".text", kSigTakeDamageOldLinux);
	if (!pFunc)
	{
		Warning("[CR] TakeDamageOld signature not found — GrenadeShot bullet block disabled\n");
		return false;
	}

	g_pTarget = pFunc;

	if (!CreateTrampoline(pFunc))
	{
		Warning("[CR] GrenadeShot trampoline alloc failed\n");
		return false;
	}

	if (!InstallJump(pFunc))
	{
		Warning("[CR] GrenadeShot jump install failed\n");
		return false;
	}

	g_hooked = true;
	CR_Log("GrenadeShot TakeDamageOld hooked @ %p", pFunc);
	return true;
}

void GrenadeShotHook_Uninstall()
{
	if (!g_hooked || !g_pTarget)
		return;

	KHook::Memory::SetAccess(g_pTarget, kStolen, KHook::Memory::READ | KHook::Memory::WRITE | KHook::Memory::EXECUTE);
	memcpy(g_pTarget, g_origBytes, kStolen);
	KHook::Memory::SetAccess(g_pTarget, kStolen, KHook::Memory::READ | KHook::Memory::EXECUTE);

	if (g_pTrampoline)
	{
		munmap(reinterpret_cast<void*>(g_pTrampoline), 4096);
		g_pTrampoline = nullptr;
	}

	g_pTarget = nullptr;
	g_hooked = false;
	CR_Log("GrenadeShot TakeDamageOld unhooked");
}

bool GrenadeShotHook_IsInstalled()
{
	return g_hooked;
}
