#pragma once

class IGameEvent;

enum class CRRoundMode : int
{
	None = 0,
	OneBullet = 1,
	NoSound = 2,
	Invert = 3,
	GrenadeShot = 4,
};

void Rounds_SetMode(CRRoundMode mode);
CRRoundMode Rounds_GetMode();

void Rounds_OnStartupServer();
void Rounds_OnGameFrame();
void Rounds_OnWeaponFire(int iSlot);
void Rounds_OnWeaponReload(int iSlot);
void Rounds_OnWeaponFireEvent(int iSlot, const char* weaponName);
