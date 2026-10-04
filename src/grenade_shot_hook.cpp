#include "grenade_shot_hook.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sys/mman.h>

#include <entity2/entitysystem.h>
#include <khook/memory.hpp>

#include "customrounds.h"
#include "entity_utils.h"
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
// CSS gamedata (xorps in prologue) + CS2Fixes/Swiftly AG2 (no xorps).
static const char* kSigsTakeDamageOldLinux[] = {
	"55 66 0F EF C0 48 89 E5 41 57 41 56 41 55 49 89 FD 31 FF",
	"55 48 89 E5 41 57 41 56 41 55 49 89 FD 31 FF",
	nullptr
};

enum : int
{
	DMG_BULLET = 1 << 1, // 2
};

// a2x/cs2-dumper server.dll CTakeDamageInfo — vptr at 0, then:
// force 0x08, pos 0x14, reported 0x20, dir 0x2C, inflictor 0x38, attacker 0x3C, bits 0x4C.
enum : ptrdiff_t
{
	kDumpForce = 0x08,
	kDumpPos = 0x14,
	kDumpReported = 0x20,
	kDumpDir = 0x2C,
	kDumpInflictor = 0x38,
	kDumpAttacker = 0x3C,
	kDumpBits = 0x4C,
};

struct DmgOffs
{
	int32_t force = kDumpForce;
	int32_t pos = kDumpPos;
	int32_t reported = kDumpReported;
	int32_t dir = kDumpDir;
	int32_t inflictor = kDumpInflictor;
	int32_t attacker = kDumpAttacker;
	int32_t bits = kDumpBits;
	bool resolved = false;
};

static DmgOffs g_off;

static int32_t SchemaOrDump(const char* field, int32_t dump)
{
	int32_t o = Schema_GetOffset("CTakeDamageInfo", field);
	return o >= 0 ? o : dump;
}

static const DmgOffs& InfoOffs()
{
	if (!g_off.resolved)
	{
		g_off.force = SchemaOrDump("m_vecDamageForce", kDumpForce);
		g_off.pos = SchemaOrDump("m_vecDamagePosition", kDumpPos);
		g_off.reported = SchemaOrDump("m_vecReportedPosition", kDumpReported);
		g_off.dir = SchemaOrDump("m_vecDamageDirection", kDumpDir);
		g_off.inflictor = SchemaOrDump("m_hInflictor", kDumpInflictor);
		g_off.attacker = SchemaOrDump("m_hAttacker", kDumpAttacker);
		g_off.bits = SchemaOrDump("m_bitsDamageType", kDumpBits);
		g_off.resolved = true;
		CR_Log("CTakeDamageInfo offs force=%d pos=%d bits=%d atk=%d",
			g_off.force, g_off.pos, g_off.bits, g_off.attacker);
	}
	return g_off;
}

static uint8_t* InfoBytes(void* pInfo)
{
	return reinterpret_cast<uint8_t*>(pInfo);
}

static CRVec3 ReadVec(void* pInfo, int32_t off)
{
	return *reinterpret_cast<CRVec3*>(InfoBytes(pInfo) + off);
}

static void WriteVec(void* pInfo, int32_t off, const CRVec3& v)
{
	*reinterpret_cast<CRVec3*>(InfoBytes(pInfo) + off) = v;
}

static bool IsZeroVec(const CRVec3& v)
{
	return v.x == 0.f && v.y == 0.f && v.z == 0.f;
}

static CEntityInstance* InfoEntity(void* pInfo, int32_t handleOff)
{
	if (!g_pGameEntitySystem)
		return nullptr;
	const CEntityHandle h = *reinterpret_cast<CEntityHandle*>(InfoBytes(pInfo) + handleOff);
	if (!h.IsValid())
		return nullptr;
	return g_pGameEntitySystem->GetEntityInstance(h);
}

// Engine logs: damagetype N with GetDamageForce/Position == vZero.
// Reconstruct from victim + attacker/inflictor origins (schema/dump layout).
static void RepairDamageInfo(CEntityInstance* pVictim, void* pInfo)
{
	const DmgOffs& o = InfoOffs();
	CRVec3 force = ReadVec(pInfo, o.force);
	CRVec3 pos = ReadVec(pInfo, o.pos);
	if (!IsZeroVec(force) && !IsZeroVec(pos))
		return;

	CRVec3 victimOrigin{};
	const bool haveVictim = Entity_GetAbsOrigin(pVictim, victimOrigin);

	CEntityInstance* pSrc = InfoEntity(pInfo, o.attacker);
	if (!pSrc)
		pSrc = InfoEntity(pInfo, o.inflictor);

	CRVec3 srcOrigin{};
	const bool haveSrc = pSrc && pSrc != pVictim && Entity_GetAbsOrigin(pSrc, srcOrigin);

	if (IsZeroVec(pos))
	{
		if (haveVictim)
			pos = victimOrigin;
		else if (haveSrc)
			pos = srcOrigin;
		if (!IsZeroVec(pos))
		{
			WriteVec(pInfo, o.pos, pos);
			CRVec3 reported = ReadVec(pInfo, o.reported);
			if (IsZeroVec(reported))
				WriteVec(pInfo, o.reported, pos);
		}
	}

	if (IsZeroVec(force))
	{
		if (haveSrc && haveVictim)
		{
			force.x = victimOrigin.x - srcOrigin.x;
			force.y = victimOrigin.y - srcOrigin.y;
			force.z = victimOrigin.z - srcOrigin.z;
		}
		if (IsZeroVec(force))
			force = { 0.f, 0.f, 1.f };
		WriteVec(pInfo, o.force, force);

		CRVec3 dir = ReadVec(pInfo, o.dir);
		if (IsZeroVec(dir))
		{
			const float mag = force.x * force.x + force.y * force.y + force.z * force.z;
			if (mag > 0.0001f)
			{
				const float inv = 1.f / sqrtf(mag);
				dir = { force.x * inv, force.y * inv, force.z * inv };
			}
			else
			{
				dir = { 0.f, 0.f, 1.f };
			}
			WriteVec(pInfo, o.dir, dir);
		}
	}
}

static int32_t ReadBits(void* pInfo)
{
	return *reinterpret_cast<int32_t*>(InfoBytes(pInfo) + InfoOffs().bits);
}

using TakeDamageOldFn = int64_t (*)(CEntityInstance* pThis, void* pInfo, void* pResult);

static TakeDamageOldFn g_pTrampoline = nullptr;
static void* g_pTarget = nullptr;
static size_t g_stolen = 0;
static uint8_t g_origBytes[32] = {};
static bool g_hooked = false;

static size_t StolenFromPrologue(const uint8_t* p)
{
	// 55 66 0F EF C0 48 89 E5 ... xor edi,edi  = 19
	if (p[0] == 0x55 && p[1] == 0x66 && p[2] == 0x0F && p[3] == 0xEF && p[4] == 0xC0)
		return 19;
	// 55 48 89 E5 ... xor edi,edi = 15
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

static int64_t Hook_TakeDamageOld(CEntityInstance* pThis, void* pInfo, void* pResult)
{
	if (pThis && pInfo)
		RepairDamageInfo(pThis, pInfo);

	if (Rounds_GetMode() == CRRoundMode::GrenadeShot
		&& pThis && pInfo
		&& IsPlayerPawnEntity(pThis)
		&& (ReadBits(pInfo) & DMG_BULLET))
	{
		// Valve returns 1 since 2025-10-15; supersede without applying bullet dmg.
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

	void* pFunc = FindFunctionSignatureAny(SERVER_LIB, ".text", kSigsTakeDamageOldLinux);
	if (!pFunc)
	{
		Warning("[CR] TakeDamageOld signature not found — GrenadeShot bullet block disabled\n");
		return false;
	}

	const size_t stolen = StolenFromPrologue(reinterpret_cast<const uint8_t*>(pFunc));
	if (stolen < 14 || stolen > sizeof(g_origBytes))
	{
		Warning("[CR] TakeDamageOld prologue not recognized — refusing to hook\n");
		return false;
	}

	g_pTarget = pFunc;
	g_stolen = stolen;

	if (!CreateTrampoline(pFunc, stolen))
	{
		Warning("[CR] GrenadeShot trampoline alloc failed\n");
		return false;
	}

	if (!InstallJump(pFunc, stolen))
	{
		Warning("[CR] GrenadeShot jump install failed\n");
		return false;
	}

	g_hooked = true;
	CR_Log("GrenadeShot TakeDamageOld hooked @ %p stolen=%zu", pFunc, stolen);
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
