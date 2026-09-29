#include "invert_hook.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sys/mman.h>

#include <khook/memory.hpp>
#include <tier0/dbg.h>

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
// CCSPlayer_MovementServices::SetupMove — cs2kz / cs2ac
static const char* kSigSetupMoveLinux[] = {
	"55 48 89 E5 41 57 41 56 41 55 49 89 F5 41 54 49 89 D4 53 48 89 FB 48 83 EC 48 E8 ? ? ? ? 48 8B 43 38",
	nullptr
};

// Head of CMoveData (cs2ac Linux layout). Only fields we touch.
struct CRVec3
{
	float x, y, z;
};

struct CRMoveData
{
	uint32_t bitfieldStorage; // m_bHasZeroFrametime / m_bIsLateCommand
	uint32_t playerHandle;
	CRVec3 absViewAngles;
	CRVec3 viewAngles;
	CRVec3 lastMovementImpulses;
	float forwardMove;
	float sideMove;
	float upMove;
};

static_assert(offsetof(CRMoveData, forwardMove) == 44, "CMoveData forwardMove offset");
static_assert(offsetof(CRMoveData, sideMove) == 48, "CMoveData sideMove offset");

using SetupMoveFn = void (*)(void* services, void* command, CRMoveData* move);

static SetupMoveFn g_pTrampoline = nullptr;
static void* g_pTarget = nullptr;
static constexpr size_t kStolen = 26; // full instructions through `sub rsp, 0x48`
static uint8_t g_origBytes[kStolen] = {};
static bool g_hooked = false;

static void InvertMoveData(CRMoveData* move)
{
	if (!move)
		return;

	move->forwardMove = -move->forwardMove;
	move->sideMove = -move->sideMove;
	move->lastMovementImpulses.x = -move->lastMovementImpulses.x;
	move->lastMovementImpulses.y = -move->lastMovementImpulses.y;
}

static void Hook_SetupMove(void* services, void* command, CRMoveData* move)
{
	g_pTrampoline(services, command, move);

	if (Rounds_GetMode() == CRRoundMode::Invert)
		InvertMoveData(move);
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

	g_pTrampoline = reinterpret_cast<SetupMoveFn>(code);
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
	const uint64_t dest = reinterpret_cast<uint64_t>(&Hook_SetupMove);
	memcpy(patch + 6, &dest, sizeof(dest));
	for (size_t i = 14; i < kStolen; ++i)
		patch[i] = 0x90;

	memcpy(pTarget, patch, kStolen);
	KHook::Memory::SetAccess(pTarget, kStolen, KHook::Memory::READ | KHook::Memory::EXECUTE);
	return true;
}

} // namespace

bool InvertHook_Install()
{
	if (g_hooked)
		return true;

	void* pFunc = nullptr;
	for (int i = 0; kSigSetupMoveLinux[i]; ++i)
	{
		pFunc = FindFunctionSignature(SERVER_LIB, ".text", kSigSetupMoveLinux[i]);
		if (pFunc)
		{
			CR_Log("SetupMove matched sig #%d", i);
			break;
		}
	}
	if (!pFunc)
	{
		Warning("[CR] SetupMove signature not found — Invert round disabled\n");
		return false;
	}

	g_pTarget = pFunc;

	if (!CreateTrampoline(pFunc))
	{
		Warning("[CR] Invert trampoline alloc failed\n");
		return false;
	}

	if (!InstallJump(pFunc))
	{
		Warning("[CR] Invert jump install failed\n");
		return false;
	}

	g_hooked = true;
	CR_Log("Invert SetupMove hooked @ %p (fwd@%zu side@%zu)",
		pFunc, offsetof(CRMoveData, forwardMove), offsetof(CRMoveData, sideMove));
	return true;
}

void InvertHook_Uninstall()
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
	InvertHook_ResetPlayerState();
	CR_Log("Invert SetupMove unhooked");
}

void InvertHook_ResetPlayerState()
{
}
