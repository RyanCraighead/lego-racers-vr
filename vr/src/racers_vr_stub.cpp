#include "racers_vr.h"

static bool g_requested;
static float g_worldScale = 10.0f;
static float g_seatedHeight;

bool RacersVr_IsCompiled()
{
	return false;
}

void RacersVr_SetRequested(bool p_requested)
{
	g_requested = p_requested;
}

bool RacersVr_IsRequested()
{
	return g_requested;
}

bool RacersVr_IsSessionRunning()
{
	return false;
}

void RacersVr_SetWorldScale(float p_gameUnitsPerMeter)
{
	if (p_gameUnitsPerMeter > 0.0f) {
		g_worldScale = p_gameUnitsPerMeter;
	}
}

float RacersVr_GetWorldScale()
{
	return g_worldScale;
}

void RacersVr_SetSeatedHeight(float p_heightMeters)
{
	g_seatedHeight = p_heightMeters;
}

float RacersVr_GetSeatedHeight()
{
	return g_seatedHeight;
}

void RacersVr_PollEventsAndActions()
{
}

bool RacersVr_ConsumePausePressed()
{
	return false;
}

void RacersVr_RequestRecenter()
{
}

void RacersVr_SetLocalPlayerCount(uint32_t)
{
}

void RacersVr_ClaimPlayer(void*)
{
}

void RacersVr_ReleasePlayer(void*)
{
}

bool RacersVr_GetSteer(void*, float*)
{
	return false;
}

bool RacersVr_IsRaceButtonHeld(void*, uint32_t)
{
	return false;
}

bool RacersVr_PollRaceButtons(void*, bool, uint32_t* p_pressed, uint32_t* p_released)
{
	if (p_pressed) {
		*p_pressed = 0;
	}
	if (p_released) {
		*p_released = 0;
	}
	return false;
}

RacersVrFrameResult RacersVr_BeginFrame(RacersVrFrame*)
{
	return RACERS_VR_FRAME_UNAVAILABLE;
}

bool RacersVr_BeginEye(uint32_t)
{
	return false;
}

void RacersVr_EndEye(uint32_t)
{
}

bool RacersVr_EndFrame()
{
	return false;
}

bool RacersVr_PulseHaptics(uint32_t, float, float, float)
{
	return false;
}

void RacersVr_ShutdownSession()
{
}
