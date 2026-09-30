#pragma once

#include <cstdint>

class CEntityInstance;

struct CRVec3
{
	float x = 0.f;
	float y = 0.f;
	float z = 0.f;
};

struct CRQAngle
{
	float x = 0.f; // pitch
	float y = 0.f; // yaw
	float z = 0.f; // roll
};

enum class CRGrenadeType : int
{
	HE = 0,
	Molotov = 1,
	Decoy = 2,
	Flash = 3,
};

bool EntityUtils_Init();
void EntityUtils_Shutdown();
void EntityUtils_RetryGrenadeFactories();
bool EntityUtils_IsReady();
bool EntityUtils_HasCreate();
bool EntityUtils_HasDispatchSpawn();
bool EntityUtils_HasAcceptInput();
bool EntityUtils_HasGrenadeFactories();
bool EntityUtils_HasFactoryForType(CRGrenadeType type);

CEntityInstance* Entity_CreateByName(const char* classname);
void Entity_DispatchSpawn(CEntityInstance* pEnt);
void Entity_AcceptInput(CEntityInstance* pEnt, const char* input);
void Entity_Teleport(CEntityInstance* pEnt, const CRVec3* pos, const CRQAngle* ang, const CRVec3* vel);

/// All types via native ::Create / EmitGrenade (fills weapon VData). No CreateEntityByName.
CEntityInstance* Entity_CreateGrenadeProjectile(
	CRGrenadeType type,
	const CRVec3& pos,
	const CRQAngle& ang,
	const CRVec3& vel,
	CEntityInstance* pOwner,
	int teamNum);

bool Entity_GetAbsOrigin(CEntityInstance* pEnt, CRVec3& out);
bool Entity_GetEyeAngles(CEntityInstance* pPawn, CRQAngle& out);
bool Entity_GetViewOffset(CEntityInstance* pPawn, CRVec3& out);

void CR_AngleVectors(const CRQAngle& ang, CRVec3& forward);
CRVec3 CR_VecAdd(const CRVec3& a, const CRVec3& b);
CRVec3 CR_VecScale(const CRVec3& v, float s);
