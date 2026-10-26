#include "grenade_shot_hook.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sys/mman.h>

#include <entity2/entitysystem.h>
#include <khook/memory.hpp>

#include "customrounds.h"
#include "rounds.h"
#include "schema.h"
#include "vtable_finder.h"

#ifdef _WIN32
#define SERVER_LIB "server.dll"
#else
#define SERVER_LIB "/libserver.so"
#endif

namespace
{
static const char* kSigsTakeDamageOldLinux[] = {
	"55 66 0F EF C0 48 89 E5 41 57 41 56 41 55 49 89 FD 31 FF",
	"55 48 89 E5 41 57 41 56 41 55 49 89 FD 31 FF",
	nullptr
};

enum : int
{
	DMG_BULLET = 1 << 1,
};

// CTakeDamageInfo::m_bitsDamageType — cs2-dumper / schema 0x4C
static constexpr int32_t kDumpBits = 0x4C;
static int32_t g_bitsOff = kDumpBits;

using TakeDamageOldFn = int64_t (*)(CEntityInstance* pThis, void* pInfo, void* pResult);

static TakeDamageOldFn g_pTrampoline = nullptr;
static void* g_pTarget = nullptr;
static size_t g_stolen = 0;
static uint8_t g_origBytes[32] = {};
static bool g_hooked = false;
static bool g_installFailed = false;

static size_t StolenFromPrologue(const uint8_t* p)
{
	if (p[0] == 0x55 && p[1] == 0x66 && p[2] == 0x0F && p[3] == 0xEF && p[4] == 0xC0)
		return 19;
	if (p[0] == 0x55 && p[1] == 0x48 && p[2] == 0x89 && p[3] == 0xE5)
		return 15;
	return 0;
}

static bool IsPlayerPawnEntity(CEntityInstance* pEnt)
{
	if (!pEnt)
		return false;
	const char* name = pEnt->GetClassname();
	return name && !strcmp(name, "player");
}

static int32_t ReadBits(void* pInfo)
{
	if (!pInfo || g_bitsOff < 0)
		return 0;
	return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(pInfo) + g_bitsOff);
}

static int64_t Hook_TakeDamageOld(CEntityInstance* pThis, void* pInfo, void* pResult)
{
	if (Rounds_GetMode() == CRRoundMode::GrenadeShot
		&& pThis && pInfo
		&& IsPlayerPawnEntity(pThis)
		&& (ReadBits(pInfo) & DMG_BULLET))
	{
		return 1;
	}

	return g_pTrampoline(pThis, pInfo, pResult);
}

static bool CreateTrampoline(void* pTarget, size_t stolen)
{
	memcpy(g_origBytes, pTarget, stolen);

	void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED)
		return false;

	auto* code = reinterpret_cast<uint8_t*>(page);
	memcpy(code, g_origBytes, stolen);

	code[stolen + 0] = 0xFF;
	code[stolen + 1] = 0x25;
	code[stolen + 2] = 0x00;
	code[stolen + 3] = 0x00;
	code[stolen + 4] = 0x00;
	code[stolen + 5] = 0x00;
	const uint64_t cont = reinterpret_cast<uint64_t>(pTarget) + stolen;
	memcpy(code + stolen + 6, &cont, sizeof(cont));

	g_pTrampoline = reinterpret_cast<TakeDamageOldFn>(code);
	return true;
}

static bool InstallJump(void* pTarget, size_t stolen)
{
	KHook::Memory::SetAccess(pTarget, stolen, KHook::Memory::READ | KHook::Memory::WRITE | KHook::Memory::EXECUTE);

	uint8_t patch[32] = {};
	patch[0] = 0xFF;
	patch[1] = 0x25;
	patch[2] = 0x00;
	patch[3] = 0x00;
	patch[4] = 0x00;
	patch[5] = 0x00;
	const uint64_t dest = reinterpret_cast<uint64_t>(&Hook_TakeDamageOld);
	memcpy(patch + 6, &dest, sizeof(dest));
	for (size_t i = 14; i < stolen; ++i)
		patch[i] = 0x90;

	memcpy(pTarget, patch, stolen);
	KHook::Memory::SetAccess(pTarget, stolen, KHook::Memory::READ | KHook::Memory::EXECUTE);
	return true;
}

} // namespace

bool GrenadeShotHook_Install()
{
	if (g_hooked)
		return true;
	if (g_installFailed)
		return false;

	void* pFunc = FindFunctionSignatureAny(SERVER_LIB, ".text", kSigsTakeDamageOldLinux);
	if (!pFunc)
	{
		Warning("[CR] TakeDamageOld signature not found — GrenadeShot bullet block disabled\n");
		g_installFailed = true;
		return false;
	}

	const size_t stolen = StolenFromPrologue(reinterpret_cast<const uint8_t*>(pFunc));
	if (stolen < 14 || stolen > sizeof(g_origBytes))
	{
		Warning("[CR] TakeDamageOld prologue not recognized — refusing to hook\n");
		g_installFailed = true;
		return false;
	}

	const int32_t schemaBits = Schema_GetOffset("CTakeDamageInfo", "m_bitsDamageType");
	g_bitsOff = schemaBits >= 0 ? schemaBits : kDumpBits;

	g_pTarget = pFunc;
	g_stolen = stolen;

	if (!CreateTrampoline(pFunc, stolen))
	{
		Warning("[CR] GrenadeShot trampoline alloc failed\n");
		g_installFailed = true;
		g_pTarget = nullptr;
		return false;
	}

	if (!InstallJump(pFunc, stolen))
	{
		Warning("[CR] GrenadeShot jump install failed\n");
		g_installFailed = true;
		if (g_pTrampoline)
		{
			munmap(reinterpret_cast<void*>(g_pTrampoline), 4096);
			g_pTrampoline = nullptr;
		}
		g_pTarget = nullptr;
		return false;
	}

	g_hooked = true;
	CR_Log("GrenadeShot TakeDamageOld hooked @ %p stolen=%zu bitsOff=%d", pFunc, stolen, g_bitsOff);
	return true;
}

void GrenadeShotHook_Uninstall()
{
	if (!g_hooked || !g_pTarget)
		return;

	KHook::Memory::SetAccess(g_pTarget, g_stolen, KHook::Memory::READ | KHook::Memory::WRITE | KHook::Memory::EXECUTE);
	memcpy(g_pTarget, g_origBytes, g_stolen);
	KHook::Memory::SetAccess(g_pTarget, g_stolen, KHook::Memory::READ | KHook::Memory::EXECUTE);

	if (g_pTrampoline)
	{
		munmap(reinterpret_cast<void*>(g_pTrampoline), 4096);
		g_pTrampoline = nullptr;
	}

	g_pTarget = nullptr;
	g_stolen = 0;
	g_hooked = false;
	CR_Log("GrenadeShot TakeDamageOld unhooked");
}

bool GrenadeShotHook_IsInstalled()
{
	return g_hooked;
}
