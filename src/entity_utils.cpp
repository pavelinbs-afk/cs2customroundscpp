#include "entity_utils.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <entity2/entitysystem.h>

#include "customrounds.h"
#include "schema.h"
#include "vtable_finder.h"

#ifdef _WIN32
#define SERVER_LIB "server.dll"
#else
#define SERVER_LIB "/libserver.so"
#endif

namespace
{
using CreateEntityByNameFn = CEntityInstance* (*)(const char* classname, int iForceEdictIndex);
using DispatchSpawnFn = void (*)(CEntityInstance* pEntity, void* pKeyValues);
using AcceptInputFn = void (*)(CEntityInstance* pThis, const char* pInputName,
	CEntityInstance* pActivator, CEntityInstance* pCaller, void* value, int nOutputID);
using TeleportFn = void (*)(CEntityInstance* pThis, const CRVec3* pos, const CRQAngle* ang, const CRVec3* vel);

// CHEGrenadeProjectile::Create / CMolotovProjectile::Create / CDecoyProjectile::Create
using GrenadeCreateFn = CEntityInstance* (*)(
	const CRVec3* pos, const CRQAngle* ang,
	const CRVec3* vel, const CRVec3* angVel,
	CEntityInstance* pOwner, int itemDefIndex);

static CreateEntityByNameFn g_fnCreate = nullptr;
static DispatchSpawnFn g_fnDispatchSpawn = nullptr;
static AcceptInputFn g_fnAcceptInput = nullptr;
static GrenadeCreateFn g_fnHECreate = nullptr;
static GrenadeCreateFn g_fnMolotovCreate = nullptr;
static GrenadeCreateFn g_fnDecoyCreate = nullptr;
static GrenadeCreateFn g_fnFlashCreate = nullptr;
static bool g_bGrenadeResolveAttempted = false;

#ifdef _WIN32
static constexpr int kTeleportVtableIndex = 165;
#else
static constexpr int kTeleportVtableIndex = 164;
#endif

static const char* kSigCreateEntityByName =
	"48 8D 05 ? ? ? ? 55 48 89 FA";
static const char* kSigDispatchSpawn =
	"48 85 FF 74 ? 55 48 89 E5 41 55 41 54 49 89 FC";
static const char* kSigAcceptInput =
	"55 48 89 E5 41 57 49 89 FF 41 56 48 8D BD ? ? ? ? 41 55 4C 8D AD";

static constexpr int kItemHE = 44;
static constexpr int kItemMolotov = 46;
static constexpr int kItemDecoy = 47;
static constexpr int kItemIncendiary = 48;
static constexpr int kItemFlash = 43;

// MatchZy / cs2-executes (current + endbr64 + pre-Oct2025).
static const char* kSigsHECreate[] = {
	// current
	"55 4C 89 C1 48 89 E5 41 57 49 89 D7",
	// CET endbr64 + current
	"F3 0F 1E FA 55 4C 89 C1 48 89 E5 41 57 49 89 D7",
	// pre-Oct 2025
	"55 4C 89 C1 48 89 E5 41 57 49 89 FF 41 56 49 89 D6",
	"F3 0F 1E FA 55 4C 89 C1 48 89 E5 41 57 49 89 FF 41 56 49 89 D6",
	nullptr
};

static const char* kSigsMolotovCreate[] = {
	"55 48 8D 05 ? ? ? ? 48 89 E5 41 57 41 56 41 55 41 54 49 89 FC 53 48 81 EC ? ? ? ? 4C 8D 35",
	"F3 0F 1E FA 55 48 8D 05 ? ? ? ? 48 89 E5 41 57 41 56 41 55 41 54 49 89 FC 53 48 81 EC ? ? ? ? 4C 8D 35",
	nullptr
};

static const char* kSigsDecoyCreate[] = {
	// SwiftlyS2 CDecoyProjectile::EmitGrenade — same stem as Flash, ends 45 31 C0 (Flash: 4C 89 EE).
	"55 4C 89 C1 48 89 E5 41 57 45 89 CF 41 56 49 89 FE 41 55 49 89 D5 48 89 F2 48 89 FE 41 54 48 8D 3D ? ? ? ? 4D 89 C4 53 48 83 EC ? E8 ? ? ? ? 45 31 C0",
	"F3 0F 1E FA 55 4C 89 C1 48 89 E5 41 57 45 89 CF 41 56 49 89 FE 41 55 49 89 D5 48 89 F2 48 89 FE 41 54 48 8D 3D ? ? ? ? 4D 89 C4 53 48 83 EC ? E8 ? ? ? ? 45 31 C0",
	nullptr
};

// SwiftlyS2 CFlashbangProjectile::EmitGrenade — ends in 4C 89 EE (not decoy's 45 31 C0).
static const char* kSigsFlashCreate[] = {
	"55 4C 89 C1 48 89 E5 41 57 45 89 CF 41 56 49 89 FE 41 55 49 89 D5 48 89 F2 48 89 FE 41 54 48 8D 3D ? ? ? ? 4D 89 C4 53 48 83 EC ? E8 ? ? ? ? 4C 89 EE",
	"F3 0F 1E FA 55 4C 89 C1 48 89 E5 41 57 45 89 CF 41 56 49 89 FE 41 55 49 89 D5 48 89 F2 48 89 FE 41 54 48 8D 3D ? ? ? ? 4D 89 C4 53 48 83 EC ? E8 ? ? ? ? 4C 89 EE",
	nullptr
};

static void ResolveGrenadeFactoriesInternal(bool force)
{
	if (g_bGrenadeResolveAttempted && !force && EntityUtils_HasGrenadeFactories())
		return;
	g_bGrenadeResolveAttempted = true;

	if (!g_fnHECreate)
		g_fnHECreate = reinterpret_cast<GrenadeCreateFn>(
			FindFunctionSignatureAny(SERVER_LIB, ".text", kSigsHECreate));
	if (!g_fnMolotovCreate)
		g_fnMolotovCreate = reinterpret_cast<GrenadeCreateFn>(
			FindFunctionSignatureAny(SERVER_LIB, ".text", kSigsMolotovCreate));
	if (!g_fnDecoyCreate)
		g_fnDecoyCreate = reinterpret_cast<GrenadeCreateFn>(
			FindFunctionSignatureAny(SERVER_LIB, ".text", kSigsDecoyCreate));
	if (!g_fnFlashCreate)
		g_fnFlashCreate = reinterpret_cast<GrenadeCreateFn>(
			FindFunctionSignatureAny(SERVER_LIB, ".text", kSigsFlashCreate));

	CR_Log("grenade factories: HE=%p Molotov=%p Decoy=%p Flash=%p",
		reinterpret_cast<void*>(g_fnHECreate),
		reinterpret_cast<void*>(g_fnMolotovCreate),
		reinterpret_cast<void*>(g_fnDecoyCreate),
		reinterpret_cast<void*>(g_fnFlashCreate));
}

} // namespace

bool EntityUtils_Init()
{
	g_fnCreate = reinterpret_cast<CreateEntityByNameFn>(
		FindFunctionSignature(SERVER_LIB, ".text", kSigCreateEntityByName));
	g_fnDispatchSpawn = reinterpret_cast<DispatchSpawnFn>(
		FindFunctionSignature(SERVER_LIB, ".text", kSigDispatchSpawn));
	g_fnAcceptInput = reinterpret_cast<AcceptInputFn>(
		FindFunctionSignature(SERVER_LIB, ".text", kSigAcceptInput));

	g_bGrenadeResolveAttempted = false;
	ResolveGrenadeFactoriesInternal(true);

	std::srand(static_cast<unsigned>(time(nullptr)));

	CR_Log("entity utils: Create=%p DispatchSpawn=%p AcceptInput=%p",
		reinterpret_cast<void*>(g_fnCreate),
		reinterpret_cast<void*>(g_fnDispatchSpawn),
		reinterpret_cast<void*>(g_fnAcceptInput));

	// Basic entity helpers are enough to load; grenade factories can resolve later.
	return g_fnCreate && g_fnDispatchSpawn && g_fnAcceptInput;
}

void EntityUtils_Shutdown()
{
	g_fnCreate = nullptr;
	g_fnDispatchSpawn = nullptr;
	g_fnAcceptInput = nullptr;
	g_fnHECreate = nullptr;
	g_fnMolotovCreate = nullptr;
	g_fnDecoyCreate = nullptr;
	g_fnFlashCreate = nullptr;
	g_bGrenadeResolveAttempted = false;
}

void EntityUtils_RetryGrenadeFactories()
{
	if (EntityUtils_HasGrenadeFactories())
		return;
	ResolveGrenadeFactoriesInternal(true);
}

bool EntityUtils_IsReady()
{
	return g_fnCreate && g_fnDispatchSpawn && g_fnAcceptInput;
}

bool EntityUtils_HasCreate()
{
	return g_fnCreate != nullptr;
}

bool EntityUtils_HasDispatchSpawn()
{
	return g_fnDispatchSpawn != nullptr;
}

bool EntityUtils_HasAcceptInput()
{
	return g_fnAcceptInput != nullptr;
}

bool EntityUtils_HasGrenadeFactories()
{
	// Native Create fills VData ("weapon info"). CreateEntityByName alone does NOT.
	return g_fnHECreate || g_fnMolotovCreate || g_fnDecoyCreate || g_fnFlashCreate;
}

bool EntityUtils_HasFactoryForType(CRGrenadeType type)
{
	switch (type)
	{
		case CRGrenadeType::HE: return g_fnHECreate != nullptr;
		case CRGrenadeType::Molotov: return g_fnMolotovCreate != nullptr;
		case CRGrenadeType::Decoy: return g_fnDecoyCreate != nullptr;
		case CRGrenadeType::Flash: return g_fnFlashCreate != nullptr;
		default: return false;
	}
}

static void ApplyGrenadeOwnership(CEntityInstance* pNade, CEntityInstance* pOwner, int teamNum)
{
	if (!pNade)
		return;

	Schema_Set<uint8_t>(pNade, "CBaseEntity", "m_iTeamNum", static_cast<uint8_t>(teamNum));
	if (!pOwner)
		return;

	const CEntityHandle hOwner = pOwner->GetRefEHandle();
	Schema_Set<CEntityHandle>(pNade, "CBaseEntity", "m_hOwnerEntity", hOwner);
	Schema_Set<CEntityHandle>(pNade, "CBaseGrenade", "m_hThrower", hOwner);
	Schema_Set<CEntityHandle>(pNade, "CBaseGrenade", "m_hOriginalThrower", hOwner);
}

CEntityInstance* Entity_CreateByName(const char* classname)
{
	if (!g_fnCreate || !classname)
		return nullptr;
	return g_fnCreate(classname, -1);
}

void Entity_DispatchSpawn(CEntityInstance* pEnt)
{
	if (!g_fnDispatchSpawn || !pEnt)
		return;
	g_fnDispatchSpawn(pEnt, nullptr);
}

void Entity_AcceptInput(CEntityInstance* pEnt, const char* input)
{
	if (!g_fnAcceptInput || !pEnt || !input)
		return;

	alignas(16) uint8_t variantBuf[64] = {};
	g_fnAcceptInput(pEnt, input, nullptr, nullptr, variantBuf, 0);
}

void Entity_Teleport(CEntityInstance* pEnt, const CRVec3* pos, const CRQAngle* ang, const CRVec3* vel)
{
	if (!pEnt)
		return;

	void** vtable = *reinterpret_cast<void***>(pEnt);
	if (!vtable)
		return;

	auto fn = reinterpret_cast<TeleportFn>(vtable[kTeleportVtableIndex]);
	if (!fn)
		return;

	fn(pEnt, pos, ang, vel);
}

static void* GetSceneNode(CEntityInstance* pEnt)
{
	if (!pEnt)
		return nullptr;

	void* pBody = Schema_Get<void*>(pEnt, "CBaseEntity", "m_CBodyComponent", nullptr);
	if (!pBody)
		return nullptr;
	return Schema_Get<void*>(pBody, "CBodyComponent", "m_pSceneNode", nullptr);
}

bool Entity_SetAbsOriginQuiet(CEntityInstance* pEnt, const CRVec3& pos)
{
	void* pNode = GetSceneNode(pEnt);
	if (!pNode)
		return false;

	// No Schema_NetworkStateChanged / Teleport — those trigger WriteEnterPVS on CS2.
	bool ok = Schema_Set<CRVec3>(pNode, "CGameSceneNode", "m_vecAbsOrigin", pos);
	// Local origin too (root node) so physics/server agree.
	Schema_Set<CRVec3>(pNode, "CGameSceneNode", "m_vecOrigin", pos);
	return ok;
}

bool Entity_SetPawnYawQuiet(CEntityInstance* pPawn, float yaw)
{
	if (!pPawn)
		return false;

	void* pNode = GetSceneNode(pPawn);
	if (pNode)
	{
		CRQAngle rot = Schema_Get<CRQAngle>(pNode, "CGameSceneNode", "m_angRotation");
		rot.y = yaw;
		Schema_Set<CRQAngle>(pNode, "CGameSceneNode", "m_angRotation", rot);
	}

	int32_t off = Schema_GetOffset("CCSPlayerPawn", "m_angEyeAngles");
	if (off < 0)
		off = Schema_GetOffset("CCSPlayerPawnBase", "m_angEyeAngles");
	if (off >= 0)
	{
		auto* eye = reinterpret_cast<CRQAngle*>(reinterpret_cast<uintptr_t>(pPawn) + off);
		eye->x = 0.f;
		eye->y = yaw;
		eye->z = 0.f;
	}

	return true;
}

bool Entity_ZeroAbsVelocityQuiet(CEntityInstance* pEnt)
{
	if (!pEnt)
		return false;

	CRVec3 zero{ 0.f, 0.f, 0.f };
	if (Schema_Set<CRVec3>(pEnt, "CBaseEntity", "m_vecAbsVelocity", zero))
		return true;
	return Schema_Set<CRVec3>(pEnt, "CBaseEntity", "m_vecBaseVelocity", zero);
}

CEntityInstance* Entity_CreateGrenadeProjectile(
	CRGrenadeType type,
	const CRVec3& pos,
	const CRQAngle& ang,
	const CRVec3& vel,
	CEntityInstance* pOwner,
	int teamNum)
{
	EntityUtils_RetryGrenadeFactories();

	// ONLY native ::Create — fills CCSWeaponBaseVData. CreateEntityByName causes:
	// "Failing to submit row for a grenade detonation: Grenade has no weapon info!"
	GrenadeCreateFn fn = nullptr;
	int itemDef = kItemHE;

	switch (type)
	{
		case CRGrenadeType::HE:
			fn = g_fnHECreate;
			itemDef = kItemHE;
			break;
		case CRGrenadeType::Molotov:
			fn = g_fnMolotovCreate;
			itemDef = (teamNum == 3) ? kItemIncendiary : kItemMolotov;
			break;
		case CRGrenadeType::Decoy:
			fn = g_fnDecoyCreate;
			itemDef = kItemDecoy;
			break;
		case CRGrenadeType::Flash:
			fn = g_fnFlashCreate;
			itemDef = kItemFlash;
			break;
		default:
			return nullptr;
	}

	if (!fn)
		return nullptr;

	CRVec3 angVel = vel;
	CEntityInstance* pNade = fn(&pos, &ang, &vel, &angVel, pOwner, itemDef);
	if (!pNade)
		return nullptr;

	// Ownership only — do not Teleport after Create (can strip weapon VData linkage).
	ApplyGrenadeOwnership(pNade, pOwner, teamNum);
	return pNade;
}

bool Entity_GetAbsOrigin(CEntityInstance* pEnt, CRVec3& out)
{
	if (!pEnt)
		return false;

	void* pBody = Schema_Get<void*>(pEnt, "CBaseEntity", "m_CBodyComponent", nullptr);
	if (!pBody)
		return false;

	void* pNode = Schema_Get<void*>(pBody, "CBodyComponent", "m_pSceneNode", nullptr);
	if (!pNode)
		return false;

	out = Schema_Get<CRVec3>(pNode, "CGameSceneNode", "m_vecAbsOrigin");
	return true;
}

bool Entity_GetEyeAngles(CEntityInstance* pPawn, CRQAngle& out)
{
	if (!pPawn)
		return false;

	int32_t off = Schema_GetOffset("CCSPlayerPawn", "m_angEyeAngles");
	if (off < 0)
		off = Schema_GetOffset("CCSPlayerPawnBase", "m_angEyeAngles");
	if (off < 0)
		return false;

	out = *reinterpret_cast<CRQAngle*>(reinterpret_cast<uintptr_t>(pPawn) + off);
	return true;
}

bool Entity_GetViewOffset(CEntityInstance* pPawn, CRVec3& out)
{
	if (!pPawn)
		return false;
	out = Schema_Get<CRVec3>(pPawn, "CBaseModelEntity", "m_vecViewOffset");
	return Schema_GetOffset("CBaseModelEntity", "m_vecViewOffset") >= 0;
}

void CR_AngleVectors(const CRQAngle& ang, CRVec3& forward)
{
	const float pitch = ang.x * (3.14159265f / 180.f);
	const float yaw = ang.y * (3.14159265f / 180.f);
	const float cp = std::cos(pitch);
	const float sp = std::sin(pitch);
	const float cy = std::cos(yaw);
	const float sy = std::sin(yaw);
	forward.x = cp * cy;
	forward.y = cp * sy;
	forward.z = -sp;
}

CRVec3 CR_VecAdd(const CRVec3& a, const CRVec3& b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

CRVec3 CR_VecScale(const CRVec3& v, float s)
{
	return {v.x * s, v.y * s, v.z * s};
}
