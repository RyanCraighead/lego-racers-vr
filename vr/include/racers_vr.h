#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	enum RacersVrFrameResult {
		RACERS_VR_FRAME_UNAVAILABLE = 0,
		RACERS_VR_FRAME_SKIP = 1,
		RACERS_VR_FRAME_RENDER = 2,
	};

	enum RacersVrRaceButton {
		RACERS_VR_RACE_THROTTLE = 1u << 0,
		RACERS_VR_RACE_BRAKE = 1u << 1,
		RACERS_VR_RACE_POWERUP = 1u << 2,
	};

	enum RacersVrHand {
		RACERS_VR_HAND_LEFT = 1u << 0,
		RACERS_VR_HAND_RIGHT = 1u << 1,
	};

	typedef struct RacersVrPose {
		// Position is relative to the initial/recentered head pose, in game units,
		// using OpenXR's right-handed axes (+X right, +Y up, -Z forward).
		float position[3];
		// OpenXR quaternion component order: x, y, z, w.
		float orientation[4];
	} RacersVrPose;

	typedef struct RacersVrFov {
		// Signed field-of-view angles in radians.
		float angle_left;
		float angle_right;
		float angle_up;
		float angle_down;
	} RacersVrFov;

	typedef struct RacersVrView {
		RacersVrPose pose;
		RacersVrFov fov;
		uint32_t recommended_width;
		uint32_t recommended_height;
	} RacersVrView;

	typedef struct RacersVrFrame {
		uint32_t view_count;
		RacersVrView views[2];
	} RacersVrFrame;

	// Compile/runtime selection. Request must be set before the first successful frame.
	bool RacersVr_IsCompiled(void);
	void RacersVr_SetRequested(bool requested);
	bool RacersVr_IsRequested(void);
	bool RacersVr_IsSessionRunning(void);

	// Position output is scaled by game_units_per_meter. Seated height is an optional
	// upward offset from the racing-camera anchor and defaults to zero.
	void RacersVr_SetWorldScale(float game_units_per_meter);
	float RacersVr_GetWorldScale(void);
	void RacersVr_SetSeatedHeight(float height_meters);
	float RacersVr_GetSeatedHeight(void);

	// Call once per game tick. Before lazy initialization this is a harmless no-op.
	void RacersVr_PollEventsAndActions(void);
	bool RacersVr_ConsumePausePressed(void);
	void RacersVr_RequestRecenter(void);

	// Additive player input. Ownership follows the existing touch-input contract.
	void RacersVr_SetLocalPlayerCount(uint32_t count);
	void RacersVr_ClaimPlayer(void* owner);
	void RacersVr_ReleasePlayer(void* owner);
	bool RacersVr_GetSteer(void* owner, float* steer);
	bool RacersVr_IsRaceButtonHeld(void* owner, uint32_t button);
	bool RacersVr_PollRaceButtons(void* owner, bool gate, uint32_t* pressed, uint32_t* released);

	// BeginFrame lazily initializes OpenXR once the existing WGL context is current.
	// RACERS_VR_FRAME_SKIP still starts an OpenXR frame and must be balanced by EndFrame.
	enum RacersVrFrameResult RacersVr_BeginFrame(RacersVrFrame* frame);

	// BeginEye acquires the selected runtime swapchain image, binds a direct color FBO
	// with a private depth renderbuffer, sets its viewport, and clears it. The caller
	// renders that eye before EndEye releases the image. Render both indices 0 and 1.
	bool RacersVr_BeginEye(uint32_t view_index);
	void RacersVr_EndEye(uint32_t view_index);
	bool RacersVr_EndFrame(void);

	// Optional output action. hands is a RacersVrHand bit mask; duration is seconds.
	bool RacersVr_PulseHaptics(uint32_t hands, float amplitude, float duration, float frequency);

	void RacersVr_ShutdownSession(void);

#ifdef __cplusplus
}
#endif
