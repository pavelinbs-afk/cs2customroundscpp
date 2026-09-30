#pragma once

// Hooks CBaseEntity::EmitSoundFilter via signature — blocks all server-side sound emit.
bool SoundHook_Install();
void SoundHook_Uninstall();
void SoundHook_SetMuted(bool muted);
bool SoundHook_IsInstalled();
bool SoundHook_IsMuted();
int SoundHook_GetSiteCount();
