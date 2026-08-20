#include "racers_vr.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// openxr_platform.h's Win32 OpenGL binding requires the native handle types first.
// clang-format off
#include <windows.h>
#include <unknwn.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{

constexpr uint32_t kViewCount = 2;
constexpr float kDefaultWorldScale = 10.0f;
constexpr float kButtonThreshold = 0.1f;

struct Vec3 {
	float x;
	float y;
	float z;
};

struct Quat {
	float x;
	float y;
	float z;
	float w;
};

Quat Normalize(Quat value)
{
	const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w);
	if (length <= 0.000001f) {
		return {0.0f, 0.0f, 0.0f, 1.0f};
	}

	const float inverse = 1.0f / length;
	return {value.x * inverse, value.y * inverse, value.z * inverse, value.w * inverse};
}

Quat Conjugate(const Quat& value)
{
	return {-value.x, -value.y, -value.z, value.w};
}

Quat Multiply(const Quat& left, const Quat& right)
{
	return {
		left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
		left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
		left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
		left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z,
	};
}

Vec3 Rotate(const Quat& rotation, const Vec3& value)
{
	const Quat vector = {value.x, value.y, value.z, 0.0f};
	const Quat rotated = Multiply(Multiply(rotation, vector), Conjugate(rotation));
	return {rotated.x, rotated.y, rotated.z};
}

Quat ToQuat(const XrQuaternionf& value)
{
	return {value.x, value.y, value.z, value.w};
}

Vec3 ToVec3(const XrVector3f& value)
{
	return {value.x, value.y, value.z};
}

using GlGenFramebuffers = void(APIENTRYP)(GLsizei count, GLuint* framebuffers);
using GlDeleteFramebuffers = void(APIENTRYP)(GLsizei count, const GLuint* framebuffers);
using GlBindFramebuffer = void(APIENTRYP)(GLenum target, GLuint framebuffer);
using GlFramebufferTexture2D =
	void(APIENTRYP)(GLenum target, GLenum attachment, GLenum texture_target, GLuint texture, GLint level);
using GlCheckFramebufferStatus = GLenum(APIENTRYP)(GLenum target);
using GlGenRenderbuffers = void(APIENTRYP)(GLsizei count, GLuint* renderbuffers);
using GlDeleteRenderbuffers = void(APIENTRYP)(GLsizei count, const GLuint* renderbuffers);
using GlBindRenderbuffer = void(APIENTRYP)(GLenum target, GLuint renderbuffer);
using GlRenderbufferStorage = void(APIENTRYP)(GLenum target, GLenum internal_format, GLsizei width, GLsizei height);
using GlFramebufferRenderbuffer =
	void(APIENTRYP)(GLenum target, GLenum attachment, GLenum renderbuffer_target, GLuint renderbuffer);

struct GlFunctions {
	GlGenFramebuffers gen_framebuffers = nullptr;
	GlDeleteFramebuffers delete_framebuffers = nullptr;
	GlBindFramebuffer bind_framebuffer = nullptr;
	GlFramebufferTexture2D framebuffer_texture_2d = nullptr;
	GlCheckFramebufferStatus check_framebuffer_status = nullptr;
	GlGenRenderbuffers gen_renderbuffers = nullptr;
	GlDeleteRenderbuffers delete_renderbuffers = nullptr;
	GlBindRenderbuffer bind_renderbuffer = nullptr;
	GlRenderbufferStorage renderbuffer_storage = nullptr;
	GlFramebufferRenderbuffer framebuffer_renderbuffer = nullptr;

	template <typename T>
	bool Load(T& function, const char* name)
	{
		function = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
		if (function == nullptr) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenXR: missing OpenGL function %s", name);
			return false;
		}
		return true;
	}

	bool LoadAll()
	{
		return Load(gen_framebuffers, "glGenFramebuffers") && Load(delete_framebuffers, "glDeleteFramebuffers") &&
			   Load(bind_framebuffer, "glBindFramebuffer") && Load(framebuffer_texture_2d, "glFramebufferTexture2D") &&
			   Load(check_framebuffer_status, "glCheckFramebufferStatus") &&
			   Load(gen_renderbuffers, "glGenRenderbuffers") && Load(delete_renderbuffers, "glDeleteRenderbuffers") &&
			   Load(bind_renderbuffer, "glBindRenderbuffer") && Load(renderbuffer_storage, "glRenderbufferStorage") &&
			   Load(framebuffer_renderbuffer, "glFramebufferRenderbuffer");
	}
};

struct EyeSwapchain {
	XrSwapchain swapchain = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageOpenGLKHR> images;
	uint32_t width = 0;
	uint32_t height = 0;
	GLuint framebuffer = 0;
	GLuint depth_renderbuffer = 0;
	bool acquired = false;
	bool waited = false;
	bool bound = false;
	bool rendered = false;
	uint32_t image_index = 0;
	GLint previous_framebuffer = 0;
	GLint previous_viewport[4] = {0, 0, 0, 0};
};

struct BindingDefinition {
	XrAction action;
	const char* path;
};

class VrRuntime {
public:
	bool IsRequested() const { return m_requested; }
	bool IsSessionRunning() const { return m_session_running; }
	float GetWorldScale() const { return m_world_scale; }
	float GetSeatedHeight() const { return m_seated_height; }

	void SetRequested(bool requested)
	{
		if (requested && !m_requested) {
			// A deliberate off/on transition retries a runtime that was unavailable earlier.
			m_initialization_failed = false;
		}
		m_requested = requested;
		if (!requested) {
			ShutdownRuntime();
		}
	}

	void SetWorldScale(float scale)
	{
		if (std::isfinite(scale) && scale > 0.0f) {
			m_world_scale = scale;
		}
	}

	void SetSeatedHeight(float height)
	{
		if (std::isfinite(height)) {
			m_seated_height = height;
		}
	}

	void PollEventsAndActions()
	{
		if (m_instance == XR_NULL_HANDLE) {
			return;
		}

		PollEvents();
		if (!m_session_running || m_session == XR_NULL_HANDLE) {
			ClearActionState();
			return;
		}

		XrActiveActionSet active_action_set{};
		active_action_set.actionSet = m_action_set;
		active_action_set.subactionPath = XR_NULL_PATH;

		XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
		sync_info.countActiveActionSets = 1;
		sync_info.activeActionSets = &active_action_set;
		const XrResult sync_result = xrSyncActions(m_session, &sync_info);
		if (XR_FAILED(sync_result)) {
			if (sync_result != XR_SESSION_NOT_FOCUSED) {
				LogResult("xrSyncActions", sync_result);
			}
			ClearActionState();
			return;
		}

		m_steer = ReadLargestMagnitude(m_steer_action);
		m_throttle = std::max(0.0f, ReadLargestValue(m_throttle_action));
		m_brake = std::max(0.0f, ReadLargestValue(m_brake_action));
		const bool powerup = ReadBoolean(m_powerup_action);
		const bool pause = ReadBoolean(m_pause_action);
		const bool recenter = ReadBoolean(m_recenter_action);

		if (pause && !m_previous_pause) {
			m_pause_pressed = true;
		}
		if (recenter && !m_previous_recenter) {
			m_recenter_pending = true;
		}
		m_previous_pause = pause;
		m_previous_recenter = recenter;

		m_race_buttons = 0;
		if (m_throttle > kButtonThreshold) {
			m_race_buttons |= RACERS_VR_RACE_THROTTLE;
		}
		if (m_brake > kButtonThreshold) {
			m_race_buttons |= RACERS_VR_RACE_BRAKE;
		}
		if (powerup) {
			m_race_buttons |= RACERS_VR_RACE_POWERUP;
		}
	}

	bool ConsumePausePressed()
	{
		const bool pressed = m_pause_pressed;
		m_pause_pressed = false;
		return pressed;
	}

	void RequestRecenter() { m_recenter_pending = true; }

	void SetLocalPlayerCount(uint32_t count)
	{
		m_local_player_count = count;
		if (count != 1) {
			m_player_owner = nullptr;
		}
	}

	void ClaimPlayer(void* owner)
	{
		if (owner != nullptr && m_local_player_count == 1 && (m_player_owner == nullptr || m_player_owner == owner)) {
			m_player_owner = owner;
			m_previous_race_buttons = m_race_buttons;
		}
	}

	void ReleasePlayer(void* owner)
	{
		if (m_player_owner == owner) {
			m_player_owner = nullptr;
			m_previous_race_buttons = 0;
			m_button_gate = false;
		}
	}

	bool GetSteer(void* owner, float* steer) const
	{
		if (steer == nullptr || !OwnsPlayer(owner)) {
			return false;
		}
		// Match the game-facing steering convention used by the DirectInput axis.
		const float value = std::clamp(-m_steer, -1.0f, 1.0f);
		if (std::abs(value) <= kButtonThreshold) {
			return false;
		}
		*steer = value;
		return true;
	}

	bool IsRaceButtonHeld(void* owner, uint32_t button) const
	{
		return OwnsPlayer(owner) && (m_race_buttons & button) != 0;
	}

	bool PollRaceButtons(void* owner, bool gate, uint32_t* pressed, uint32_t* released)
	{
		if (pressed == nullptr || released == nullptr || owner == nullptr || owner != m_player_owner ||
			m_local_player_count != 1) {
			return false;
		}

		*pressed = 0;
		*released = 0;
		if (!m_session_running) {
			// Release anything the game saw held before focus/runtime loss so a
			// disconnected controller cannot leave throttle or brake latched.
			*released = m_previous_race_buttons;
			m_previous_race_buttons = 0;
			m_button_gate = false;
			return true;
		}
		if (!gate) {
			m_button_gate = false;
			return false;
		}

		if (!m_button_gate) {
			m_button_gate = true;
			// ReleaseAllInputs runs while the gate is closed. Rebuild the game-side
			// state from zero so a trigger held through countdown/pause re-fires.
			m_previous_race_buttons = 0;
		}

		*pressed = m_race_buttons & ~m_previous_race_buttons;
		*released = m_previous_race_buttons & ~m_race_buttons;
		m_previous_race_buttons = m_race_buttons;
		return (*pressed | *released) != 0;
	}

	RacersVrFrameResult BeginFrame(RacersVrFrame* output)
	{
		if (output == nullptr) {
			return RACERS_VR_FRAME_UNAVAILABLE;
		}
		std::memset(output, 0, sizeof(*output));

		if (!m_requested || m_frame_begun) {
			return RACERS_VR_FRAME_UNAVAILABLE;
		}
		if (!EnsureInitialized()) {
			return RACERS_VR_FRAME_UNAVAILABLE;
		}

		PollEventsAndActions();
		if (!m_session_running || m_exit_requested) {
			return RACERS_VR_FRAME_UNAVAILABLE;
		}

		XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
		m_frame_state = {XR_TYPE_FRAME_STATE};
		XrResult result = xrWaitFrame(m_session, &wait_info, &m_frame_state);
		if (XR_FAILED(result)) {
			LogResult("xrWaitFrame", result);
			return RACERS_VR_FRAME_UNAVAILABLE;
		}

		XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
		result = xrBeginFrame(m_session, &begin_info);
		if (XR_FAILED(result)) {
			LogResult("xrBeginFrame", result);
			return RACERS_VR_FRAME_UNAVAILABLE;
		}
		m_frame_begun = true;
		ResetEyeFrameState();

		if (m_frame_state.shouldRender != XR_TRUE) {
			return RACERS_VR_FRAME_SKIP;
		}

		XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
		locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate_info.displayTime = m_frame_state.predictedDisplayTime;
		locate_info.space = m_local_space;

		XrViewState view_state{XR_TYPE_VIEW_STATE};
		uint32_t view_count = 0;
		for (XrView& view : m_views) {
			view = {XR_TYPE_VIEW};
		}
		result = xrLocateViews(m_session, &locate_info, &view_state, kViewCount, &view_count, m_views.data());
		if (XR_FAILED(result)) {
			LogResult("xrLocateViews", result);
			return RACERS_VR_FRAME_SKIP;
		}

		const XrViewStateFlags required_flags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
		if (view_count != kViewCount || (view_state.viewStateFlags & required_flags) != required_flags) {
			return RACERS_VR_FRAME_SKIP;
		}

		if (!m_origin_valid || m_recenter_pending) {
			CaptureOrigin();
		}

		output->view_count = kViewCount;
		for (uint32_t index = 0; index < kViewCount; ++index) {
			FillRelativeView(index, &output->views[index]);
		}
		return RACERS_VR_FRAME_RENDER;
	}

	bool BeginEye(uint32_t view_index)
	{
		if (!m_frame_begun || m_frame_state.shouldRender != XR_TRUE || view_index >= kViewCount) {
			return false;
		}

		EyeSwapchain& eye = m_eyes[view_index];
		if (eye.acquired || eye.waited || eye.bound) {
			return false;
		}

		XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		XrResult result = xrAcquireSwapchainImage(eye.swapchain, &acquire_info, &eye.image_index);
		if (XR_FAILED(result)) {
			LogResult("xrAcquireSwapchainImage", result);
			return false;
		}
		eye.acquired = true;

		XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		wait_info.timeout = XR_INFINITE_DURATION;
		result = xrWaitSwapchainImage(eye.swapchain, &wait_info);
		if (XR_FAILED(result)) {
			LogResult("xrWaitSwapchainImage", result);
			// xrReleaseSwapchainImage is only valid after a successful wait. Leave
			// this image for swapchain teardown and stand down the failed session.
			m_session_running = false;
			m_exit_requested = true;
			return false;
		}
		eye.waited = true;

		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &eye.previous_framebuffer);
		glGetIntegerv(GL_VIEWPORT, eye.previous_viewport);
		m_gl.bind_framebuffer(GL_FRAMEBUFFER, eye.framebuffer);
		m_gl.framebuffer_texture_2d(
			GL_FRAMEBUFFER,
			GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D,
			eye.images[eye.image_index].image,
			0
		);
		if (m_gl.check_framebuffer_status(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			SDL_LogError(
				SDL_LOG_CATEGORY_APPLICATION,
				"OpenXR: runtime swapchain framebuffer is incomplete for eye %u",
				view_index
			);
			m_gl.bind_framebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(eye.previous_framebuffer));
			ReleaseEye(eye, false);
			return false;
		}

		eye.bound = true;
		glViewport(0, 0, static_cast<GLsizei>(eye.width), static_cast<GLsizei>(eye.height));

		const GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
		GLboolean depth_write_enabled = GL_TRUE;
		glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_write_enabled);
		if (scissor_enabled == GL_TRUE) {
			glDisable(GL_SCISSOR_TEST);
		}
		if (depth_write_enabled != GL_TRUE) {
			glDepthMask(GL_TRUE);
		}
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClearDepth(1.0);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		if (depth_write_enabled != GL_TRUE) {
			glDepthMask(depth_write_enabled);
		}
		if (scissor_enabled == GL_TRUE) {
			glEnable(GL_SCISSOR_TEST);
		}
		return true;
	}

	void EndEye(uint32_t view_index)
	{
		if (view_index < kViewCount) {
			ReleaseEye(m_eyes[view_index], true);
		}
	}

	bool EndFrame()
	{
		if (!m_frame_begun) {
			return false;
		}

		for (EyeSwapchain& eye : m_eyes) {
			if (eye.acquired || eye.bound) {
				ReleaseEye(eye, false);
			}
		}

		std::array<XrCompositionLayerProjectionView, kViewCount> projection_views{};
		XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		const XrCompositionLayerBaseHeader* layers[1] = {};
		uint32_t layer_count = 0;

		if (m_frame_state.shouldRender == XR_TRUE && m_eyes[0].rendered && m_eyes[1].rendered) {
			for (uint32_t index = 0; index < kViewCount; ++index) {
				projection_views[index] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
				projection_views[index].pose = m_views[index].pose;
				projection_views[index].fov = m_views[index].fov;
				projection_views[index].subImage.swapchain = m_eyes[index].swapchain;
				projection_views[index].subImage.imageRect.offset = {0, 0};
				projection_views[index].subImage.imageRect.extent = {
					static_cast<int32_t>(m_eyes[index].width),
					static_cast<int32_t>(m_eyes[index].height),
				};
				projection_views[index].subImage.imageArrayIndex = 0;
			}

			projection.space = m_local_space;
			projection.viewCount = kViewCount;
			projection.views = projection_views.data();
			layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
			layer_count = 1;
		}

		XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
		end_info.displayTime = m_frame_state.predictedDisplayTime;
		end_info.environmentBlendMode = m_blend_mode;
		end_info.layerCount = layer_count;
		end_info.layers = layer_count == 0 ? nullptr : layers;
		const XrResult result = xrEndFrame(m_session, &end_info);
		m_frame_begun = false;
		ResetEyeFrameState();
		if (XR_FAILED(result)) {
			LogResult("xrEndFrame", result);
			return false;
		}
		return true;
	}

	bool PulseHaptics(uint32_t hands, float amplitude, float duration, float frequency)
	{
		if (!m_session_running || m_haptic_action == XR_NULL_HANDLE || hands == 0 || !std::isfinite(amplitude) ||
			!std::isfinite(duration) || !std::isfinite(frequency)) {
			return false;
		}

		XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
		vibration.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
		vibration.duration =
			duration <= 0.0f ? XR_MIN_HAPTIC_DURATION : static_cast<XrDuration>(duration * 1000000000.0f);
		vibration.frequency = frequency <= 0.0f ? XR_FREQUENCY_UNSPECIFIED : frequency;

		bool applied = false;
		for (uint32_t index = 0; index < m_hand_paths.size(); ++index) {
			const uint32_t mask = index == 0 ? RACERS_VR_HAND_LEFT : RACERS_VR_HAND_RIGHT;
			if ((hands & mask) == 0) {
				continue;
			}

			XrHapticActionInfo action_info{XR_TYPE_HAPTIC_ACTION_INFO};
			action_info.action = m_haptic_action;
			action_info.subactionPath = m_hand_paths[index];
			const XrResult result =
				xrApplyHapticFeedback(m_session, &action_info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
			if (XR_SUCCEEDED(result)) {
				applied = true;
			}
			else {
				LogResult("xrApplyHapticFeedback", result);
			}
		}
		return applied;
	}

	void ShutdownSession()
	{
		ShutdownRuntime();
		m_initialization_failed = false;
	}

private:
	bool EnsureInitialized()
	{
		if (m_instance != XR_NULL_HANDLE) {
			return true;
		}
		if (m_initialization_failed) {
			return false;
		}

		const HDC device_context = wglGetCurrentDC();
		const HGLRC render_context = wglGetCurrentContext();
		if (device_context == nullptr || render_context == nullptr) {
			return false;
		}
		if (!m_gl.LoadAll()) {
			m_initialization_failed = true;
			return false;
		}

		if (!CreateInstance() || !CreateSystem() || !CheckGraphicsRequirements() || !CreateActions() ||
			!CreateSession(device_context, render_context) || !CreateReferenceSpace() || !CreateSwapchains() ||
			!AttachActions()) {
			m_initialization_failed = true;
			ShutdownRuntime();
			m_initialization_failed = true;
			return false;
		}

		SDL_Log("OpenXR: initialized stereo OpenGL session");
		return true;
	}

	bool CreateInstance()
	{
		uint32_t extension_count = 0;
		XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr);
		if (XR_FAILED(result)) {
			LogResult("xrEnumerateInstanceExtensionProperties", result);
			return false;
		}

		std::vector<XrExtensionProperties> extensions(
			extension_count,
			XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES}
		);
		result = xrEnumerateInstanceExtensionProperties(nullptr, extension_count, &extension_count, extensions.data());
		if (XR_FAILED(result)) {
			LogResult("xrEnumerateInstanceExtensionProperties", result);
			return false;
		}

		const bool supports_opengl =
			std::any_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties& extension) {
				return std::strcmp(extension.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0;
			});
		if (!supports_opengl) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenXR: runtime does not support XR_KHR_opengl_enable");
			return false;
		}

		const char* enabled_extensions[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
		XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
		std::strncpy(
			create_info.applicationInfo.applicationName,
			"LEGO Racers Portable",
			XR_MAX_APPLICATION_NAME_SIZE - 1
		);
		std::strncpy(create_info.applicationInfo.engineName, "racers_vr", XR_MAX_ENGINE_NAME_SIZE - 1);
		create_info.applicationInfo.applicationVersion = 1;
		create_info.applicationInfo.engineVersion = 1;
		create_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
		create_info.enabledExtensionCount = 1;
		create_info.enabledExtensionNames = enabled_extensions;
		result = xrCreateInstance(&create_info, &m_instance);
		if (XR_FAILED(result)) {
			LogResult("xrCreateInstance", result);
			return false;
		}
		return true;
	}

	bool CreateSystem()
	{
		XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
		system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		const XrResult result = xrGetSystem(m_instance, &system_info, &m_system_id);
		if (XR_FAILED(result)) {
			LogResult("xrGetSystem", result);
			return false;
		}
		return true;
	}

	bool CheckGraphicsRequirements()
	{
		PFN_xrVoidFunction untyped_function = nullptr;
		XrResult result = xrGetInstanceProcAddr(m_instance, "xrGetOpenGLGraphicsRequirementsKHR", &untyped_function);
		if (XR_FAILED(result) || untyped_function == nullptr) {
			LogResult("xrGetInstanceProcAddr(xrGetOpenGLGraphicsRequirementsKHR)", result);
			return false;
		}

		const auto get_requirements = reinterpret_cast<PFN_xrGetOpenGLGraphicsRequirementsKHR>(untyped_function);
		XrGraphicsRequirementsOpenGLKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
		result = get_requirements(m_instance, m_system_id, &requirements);
		if (XR_FAILED(result)) {
			LogResult("xrGetOpenGLGraphicsRequirementsKHR", result);
			return false;
		}

		GLint major = 0;
		GLint minor = 0;
		glGetIntegerv(GL_MAJOR_VERSION, &major);
		glGetIntegerv(GL_MINOR_VERSION, &minor);
		const XrVersion current = XR_MAKE_VERSION(static_cast<uint64_t>(major), static_cast<uint64_t>(minor), 0);
		if (major < 3 || (major == 3 && minor < 3) || current < requirements.minApiVersionSupported ||
			current > requirements.maxApiVersionSupported) {
			SDL_LogError(
				SDL_LOG_CATEGORY_APPLICATION,
				"OpenXR: current OpenGL %d.%d is outside the required range %u.%u-%u.%u",
				major,
				minor,
				XR_VERSION_MAJOR(requirements.minApiVersionSupported),
				XR_VERSION_MINOR(requirements.minApiVersionSupported),
				XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
				XR_VERSION_MINOR(requirements.maxApiVersionSupported)
			);
			return false;
		}
		return true;
	}

	bool CreateActions()
	{
		XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
		std::strncpy(set_info.actionSetName, "racing", XR_MAX_ACTION_SET_NAME_SIZE - 1);
		std::strncpy(set_info.localizedActionSetName, "Racing", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
		set_info.priority = 0;
		XrResult result = xrCreateActionSet(m_instance, &set_info, &m_action_set);
		if (XR_FAILED(result)) {
			LogResult("xrCreateActionSet", result);
			return false;
		}

		if (!StringToPath("/user/hand/left", &m_hand_paths[0]) || !StringToPath("/user/hand/right", &m_hand_paths[1])) {
			return false;
		}

		if (!CreateAction("steer", "Steer", XR_ACTION_TYPE_FLOAT_INPUT, &m_steer_action) ||
			!CreateAction("throttle", "Throttle", XR_ACTION_TYPE_FLOAT_INPUT, &m_throttle_action) ||
			!CreateAction("brake", "Brake", XR_ACTION_TYPE_FLOAT_INPUT, &m_brake_action) ||
			!CreateAction("powerup", "Use power-up", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_powerup_action) ||
			!CreateAction("pause", "Pause", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_pause_action) ||
			!CreateAction("recenter", "Recenter", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_recenter_action) ||
			!CreateAction("haptics", "Haptics", XR_ACTION_TYPE_VIBRATION_OUTPUT, &m_haptic_action)) {
			return false;
		}

		SuggestBindings(
			"/interaction_profiles/oculus/touch_controller",
			{
				{m_steer_action, "/user/hand/left/input/thumbstick/x"},
				{m_steer_action, "/user/hand/right/input/thumbstick/x"},
				{m_throttle_action, "/user/hand/right/input/trigger/value"},
				{m_brake_action, "/user/hand/left/input/trigger/value"},
				{m_powerup_action, "/user/hand/right/input/a/click"},
				{m_pause_action, "/user/hand/left/input/menu/click"},
				{m_recenter_action, "/user/hand/right/input/thumbstick/click"},
				{m_haptic_action, "/user/hand/left/output/haptic"},
				{m_haptic_action, "/user/hand/right/output/haptic"},
			}
		);
		SuggestBindings(
			"/interaction_profiles/valve/index_controller",
			{
				{m_steer_action, "/user/hand/left/input/thumbstick/x"},
				{m_steer_action, "/user/hand/right/input/thumbstick/x"},
				{m_throttle_action, "/user/hand/right/input/trigger/value"},
				{m_brake_action, "/user/hand/left/input/trigger/value"},
				{m_powerup_action, "/user/hand/right/input/a/click"},
				{m_pause_action, "/user/hand/left/input/b/click"},
				{m_recenter_action, "/user/hand/right/input/thumbstick/click"},
				{m_haptic_action, "/user/hand/left/output/haptic"},
				{m_haptic_action, "/user/hand/right/output/haptic"},
			}
		);
		SuggestBindings(
			"/interaction_profiles/microsoft/motion_controller",
			{
				{m_steer_action, "/user/hand/left/input/thumbstick/x"},
				{m_steer_action, "/user/hand/right/input/thumbstick/x"},
				{m_throttle_action, "/user/hand/right/input/trigger/value"},
				{m_brake_action, "/user/hand/left/input/trigger/value"},
				{m_powerup_action, "/user/hand/right/input/squeeze/click"},
				{m_pause_action, "/user/hand/left/input/menu/click"},
				{m_recenter_action, "/user/hand/right/input/thumbstick/click"},
				{m_haptic_action, "/user/hand/left/output/haptic"},
				{m_haptic_action, "/user/hand/right/output/haptic"},
			}
		);
		SuggestBindings(
			"/interaction_profiles/khr/simple_controller",
			{
				{m_powerup_action, "/user/hand/right/input/select/click"},
				{m_pause_action, "/user/hand/left/input/menu/click"},
				{m_haptic_action, "/user/hand/left/output/haptic"},
				{m_haptic_action, "/user/hand/right/output/haptic"},
			}
		);
		return true;
	}

	bool CreateAction(const char* name, const char* localized_name, XrActionType type, XrAction* action)
	{
		XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
		action_info.actionType = type;
		std::strncpy(action_info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
		std::strncpy(action_info.localizedActionName, localized_name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
		action_info.countSubactionPaths = static_cast<uint32_t>(m_hand_paths.size());
		action_info.subactionPaths = m_hand_paths.data();
		const XrResult result = xrCreateAction(m_action_set, &action_info, action);
		if (XR_FAILED(result)) {
			LogResult("xrCreateAction", result);
			return false;
		}
		return true;
	}

	void SuggestBindings(const char* profile, const std::vector<BindingDefinition>& definitions)
	{
		XrPath profile_path = XR_NULL_PATH;
		if (!StringToPath(profile, &profile_path)) {
			return;
		}

		std::vector<XrActionSuggestedBinding> bindings;
		bindings.reserve(definitions.size());
		for (const BindingDefinition& definition : definitions) {
			XrPath binding_path = XR_NULL_PATH;
			if (StringToPath(definition.path, &binding_path)) {
				bindings.push_back({definition.action, binding_path});
			}
		}

		XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		suggested.interactionProfile = profile_path;
		suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
		suggested.suggestedBindings = bindings.data();
		const XrResult result = xrSuggestInteractionProfileBindings(m_instance, &suggested);
		if (XR_FAILED(result)) {
			SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "OpenXR: bindings rejected for %s (%d)", profile, result);
		}
	}

	bool CreateSession(HDC device_context, HGLRC render_context)
	{
		XrGraphicsBindingOpenGLWin32KHR graphics_binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
		graphics_binding.hDC = device_context;
		graphics_binding.hGLRC = render_context;

		XrSessionCreateInfo create_info{XR_TYPE_SESSION_CREATE_INFO};
		create_info.next = &graphics_binding;
		create_info.systemId = m_system_id;
		const XrResult result = xrCreateSession(m_instance, &create_info, &m_session);
		if (XR_FAILED(result)) {
			LogResult("xrCreateSession", result);
			return false;
		}
		return ChooseBlendMode();
	}

	bool ChooseBlendMode()
	{
		uint32_t count = 0;
		XrResult result = xrEnumerateEnvironmentBlendModes(
			m_instance,
			m_system_id,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
			0,
			&count,
			nullptr
		);
		if (XR_FAILED(result)) {
			LogResult("xrEnumerateEnvironmentBlendModes", result);
			return false;
		}
		if (count == 0) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenXR: runtime reported no stereo environment blend modes");
			return false;
		}
		std::vector<XrEnvironmentBlendMode> modes(count);
		result = xrEnumerateEnvironmentBlendModes(
			m_instance,
			m_system_id,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
			count,
			&count,
			modes.data()
		);
		if (XR_FAILED(result)) {
			LogResult("xrEnumerateEnvironmentBlendModes", result);
			return false;
		}
		const auto opaque = std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
		m_blend_mode = opaque == modes.end() ? modes.front() : *opaque;
		return true;
	}

	bool CreateReferenceSpace()
	{
		XrReferenceSpaceCreateInfo create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		create_info.poseInReferenceSpace.orientation.w = 1.0f;
		const XrResult result = xrCreateReferenceSpace(m_session, &create_info, &m_local_space);
		if (XR_FAILED(result)) {
			LogResult("xrCreateReferenceSpace", result);
			return false;
		}
		return true;
	}

	bool CreateSwapchains()
	{
		uint32_t view_count = 0;
		XrResult result = xrEnumerateViewConfigurationViews(
			m_instance,
			m_system_id,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
			0,
			&view_count,
			nullptr
		);
		if (XR_FAILED(result)) {
			LogResult("xrEnumerateViewConfigurationViews", result);
			return false;
		}
		if (view_count != kViewCount) {
			SDL_LogError(
				SDL_LOG_CATEGORY_APPLICATION,
				"OpenXR: PRIMARY_STEREO reported %u views instead of two",
				view_count
			);
			return false;
		}

		std::array<XrViewConfigurationView, kViewCount> view_configuration{};
		for (XrViewConfigurationView& view : view_configuration) {
			view = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
		}
		result = xrEnumerateViewConfigurationViews(
			m_instance,
			m_system_id,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
			kViewCount,
			&view_count,
			view_configuration.data()
		);
		if (XR_FAILED(result)) {
			LogResult("xrEnumerateViewConfigurationViews", result);
			return false;
		}

		int64_t color_format = 0;
		if (!ChooseSwapchainFormat(&color_format)) {
			return false;
		}

		for (uint32_t index = 0; index < kViewCount; ++index) {
			EyeSwapchain& eye = m_eyes[index];
			eye.width = view_configuration[index].recommendedImageRectWidth;
			eye.height = view_configuration[index].recommendedImageRectHeight;

			XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
			create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
			create_info.format = color_format;
			create_info.sampleCount = 1;
			create_info.width = eye.width;
			create_info.height = eye.height;
			create_info.faceCount = 1;
			create_info.arraySize = 1;
			create_info.mipCount = 1;
			result = xrCreateSwapchain(m_session, &create_info, &eye.swapchain);
			if (XR_FAILED(result)) {
				LogResult("xrCreateSwapchain", result);
				return false;
			}

			uint32_t image_count = 0;
			result = xrEnumerateSwapchainImages(eye.swapchain, 0, &image_count, nullptr);
			if (XR_FAILED(result)) {
				LogResult("xrEnumerateSwapchainImages", result);
				return false;
			}
			if (image_count == 0) {
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenXR: runtime returned an empty swapchain image list");
				return false;
			}
			eye.images.resize(image_count, XrSwapchainImageOpenGLKHR{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
			result = xrEnumerateSwapchainImages(
				eye.swapchain,
				image_count,
				&image_count,
				reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data())
			);
			if (XR_FAILED(result)) {
				LogResult("xrEnumerateSwapchainImages", result);
				return false;
			}
			if (!CreateEyeFramebuffer(eye)) {
				return false;
			}
		}
		return true;
	}

	bool ChooseSwapchainFormat(int64_t* selected_format)
	{
		uint32_t format_count = 0;
		XrResult result = xrEnumerateSwapchainFormats(m_session, 0, &format_count, nullptr);
		if (XR_FAILED(result)) {
			LogResult("xrEnumerateSwapchainFormats", result);
			return false;
		}
		if (format_count == 0) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenXR: runtime returned no OpenGL swapchain formats");
			return false;
		}

		std::vector<int64_t> formats(format_count);
		result = xrEnumerateSwapchainFormats(m_session, format_count, &format_count, formats.data());
		if (XR_FAILED(result)) {
			LogResult("xrEnumerateSwapchainFormats", result);
			return false;
		}

		// The existing GL3 renderer writes to unsized-linear RGBA8 without
		// GL_FRAMEBUFFER_SRGB, so preserve that color-state contract when possible.
		const std::array<int64_t, 2> preferred = {GL_RGBA8, GL_SRGB8_ALPHA8};
		for (int64_t candidate : preferred) {
			if (std::find(formats.begin(), formats.end(), candidate) != formats.end()) {
				*selected_format = candidate;
				return true;
			}
		}
		*selected_format = formats.front();
		SDL_LogWarn(
			SDL_LOG_CATEGORY_APPLICATION,
			"OpenXR: using runtime fallback swapchain format %lld",
			static_cast<long long>(*selected_format)
		);
		return true;
	}

	bool CreateEyeFramebuffer(EyeSwapchain& eye)
	{
		GLint previous_framebuffer = 0;
		GLint previous_renderbuffer = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
		glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous_renderbuffer);

		m_gl.gen_framebuffers(1, &eye.framebuffer);
		m_gl.gen_renderbuffers(1, &eye.depth_renderbuffer);
		m_gl.bind_framebuffer(GL_FRAMEBUFFER, eye.framebuffer);
		m_gl.bind_renderbuffer(GL_RENDERBUFFER, eye.depth_renderbuffer);
		m_gl.renderbuffer_storage(
			GL_RENDERBUFFER,
			GL_DEPTH_COMPONENT24,
			static_cast<GLsizei>(eye.width),
			static_cast<GLsizei>(eye.height)
		);
		m_gl.framebuffer_renderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, eye.depth_renderbuffer);

		m_gl.bind_renderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previous_renderbuffer));
		m_gl.bind_framebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_framebuffer));
		return eye.framebuffer != 0 && eye.depth_renderbuffer != 0;
	}

	bool AttachActions()
	{
		XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
		attach_info.countActionSets = 1;
		attach_info.actionSets = &m_action_set;
		const XrResult result = xrAttachSessionActionSets(m_session, &attach_info);
		if (XR_FAILED(result)) {
			LogResult("xrAttachSessionActionSets", result);
			return false;
		}
		return true;
	}

	void PollEvents()
	{
		XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
		for (;;) {
			const XrResult result = xrPollEvent(m_instance, &event);
			if (result == XR_EVENT_UNAVAILABLE) {
				break;
			}
			if (XR_FAILED(result)) {
				LogResult("xrPollEvent", result);
				break;
			}

			if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				const auto* state = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
				HandleSessionState(state->state);
			}
			else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
				m_exit_requested = true;
				m_session_running = false;
			}
			event = {XR_TYPE_EVENT_DATA_BUFFER};
		}
	}

	void HandleSessionState(XrSessionState state)
	{
		m_session_state = state;
		if (state == XR_SESSION_STATE_READY && !m_session_running) {
			XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
			begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			const XrResult result = xrBeginSession(m_session, &begin_info);
			if (XR_SUCCEEDED(result)) {
				m_session_running = true;
				m_exit_requested = false;
				m_origin_valid = false;
				m_recenter_pending = true;
			}
			else {
				LogResult("xrBeginSession", result);
			}
		}
		else if (state == XR_SESSION_STATE_STOPPING && m_session_running) {
			const XrResult result = xrEndSession(m_session);
			if (XR_FAILED(result)) {
				LogResult("xrEndSession", result);
			}
			m_session_running = false;
			m_origin_valid = false;
			m_recenter_pending = true;
		}
		else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
			m_session_running = false;
			m_exit_requested = true;
			m_origin_valid = false;
			m_recenter_pending = true;
		}
	}

	float ReadLargestMagnitude(XrAction action) const
	{
		float selected = 0.0f;
		for (XrPath hand_path : m_hand_paths) {
			const float value = ReadFloat(action, hand_path);
			if (std::abs(value) > std::abs(selected)) {
				selected = value;
			}
		}
		return selected;
	}

	float ReadLargestValue(XrAction action) const
	{
		float selected = 0.0f;
		for (XrPath hand_path : m_hand_paths) {
			selected = std::max(selected, ReadFloat(action, hand_path));
		}
		return selected;
	}

	float ReadFloat(XrAction action, XrPath subaction_path) const
	{
		XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
		get_info.action = action;
		get_info.subactionPath = subaction_path;
		XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
		if (XR_FAILED(xrGetActionStateFloat(m_session, &get_info, &state)) || state.isActive != XR_TRUE) {
			return 0.0f;
		}
		return state.currentState;
	}

	bool ReadBoolean(XrAction action) const
	{
		for (XrPath hand_path : m_hand_paths) {
			XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
			get_info.action = action;
			get_info.subactionPath = hand_path;
			XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
			if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &get_info, &state)) && state.isActive == XR_TRUE &&
				state.currentState == XR_TRUE) {
				return true;
			}
		}
		return false;
	}

	void CaptureOrigin()
	{
		const Vec3 left = ToVec3(m_views[0].pose.position);
		const Vec3 right = ToVec3(m_views[1].pose.position);
		m_origin_position = {
			(left.x + right.x) * 0.5f,
			(left.y + right.y) * 0.5f,
			(left.z + right.z) * 0.5f,
		};

		Quat left_rotation = Normalize(ToQuat(m_views[0].pose.orientation));
		Quat right_rotation = Normalize(ToQuat(m_views[1].pose.orientation));
		const float dot = left_rotation.x * right_rotation.x + left_rotation.y * right_rotation.y +
						  left_rotation.z * right_rotation.z + left_rotation.w * right_rotation.w;
		if (dot < 0.0f) {
			right_rotation = {-right_rotation.x, -right_rotation.y, -right_rotation.z, -right_rotation.w};
		}
		const Quat averaged_orientation = Normalize({
			left_rotation.x + right_rotation.x,
			left_rotation.y + right_rotation.y,
			left_rotation.z + right_rotation.z,
			left_rotation.w + right_rotation.w,
		});
		// Seated recenter preserves the runtime's gravity/up axis. Zero yaw only;
		// capturing pitch/roll here would tilt the road and the height offset.
		const Vec3 forward = Rotate(averaged_orientation, {0.0f, 0.0f, -1.0f});
		const float horizontal_length = std::sqrt(forward.x * forward.x + forward.z * forward.z);
		if (horizontal_length > 0.000001f) {
			const float yaw = std::atan2(-forward.x / horizontal_length, -forward.z / horizontal_length);
			m_origin_orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
		}
		else {
			m_origin_orientation = {0.0f, 0.0f, 0.0f, 1.0f};
		}
		m_origin_valid = true;
		m_recenter_pending = false;
	}

	void FillRelativeView(uint32_t index, RacersVrView* output) const
	{
		const XrPosef& absolute_pose = m_views[index].pose;
		const Vec3 difference = {
			absolute_pose.position.x - m_origin_position.x,
			absolute_pose.position.y - m_origin_position.y,
			absolute_pose.position.z - m_origin_position.z,
		};
		const Quat inverse_origin = Conjugate(m_origin_orientation);
		Vec3 relative_position = Rotate(inverse_origin, difference);
		relative_position.y += m_seated_height;
		const Quat relative_orientation = Normalize(Multiply(inverse_origin, ToQuat(absolute_pose.orientation)));

		output->pose.position[0] = relative_position.x * m_world_scale;
		output->pose.position[1] = relative_position.y * m_world_scale;
		output->pose.position[2] = relative_position.z * m_world_scale;
		output->pose.orientation[0] = relative_orientation.x;
		output->pose.orientation[1] = relative_orientation.y;
		output->pose.orientation[2] = relative_orientation.z;
		output->pose.orientation[3] = relative_orientation.w;
		output->fov.angle_left = m_views[index].fov.angleLeft;
		output->fov.angle_right = m_views[index].fov.angleRight;
		output->fov.angle_up = m_views[index].fov.angleUp;
		output->fov.angle_down = m_views[index].fov.angleDown;
		output->recommended_width = m_eyes[index].width;
		output->recommended_height = m_eyes[index].height;
	}

	void ReleaseEye(EyeSwapchain& eye, bool rendered)
	{
		if (eye.bound) {
			glFlush();
			m_gl.bind_framebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(eye.previous_framebuffer));
			glViewport(
				eye.previous_viewport[0],
				eye.previous_viewport[1],
				eye.previous_viewport[2],
				eye.previous_viewport[3]
			);
			eye.bound = false;
		}
		if (eye.acquired && eye.waited) {
			XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			const XrResult result = xrReleaseSwapchainImage(eye.swapchain, &release_info);
			if (XR_FAILED(result)) {
				LogResult("xrReleaseSwapchainImage", result);
				rendered = false;
			}
		}
		eye.acquired = false;
		eye.waited = false;
		if (rendered) {
			eye.rendered = true;
		}
	}

	void ResetEyeFrameState()
	{
		for (EyeSwapchain& eye : m_eyes) {
			eye.acquired = false;
			eye.waited = false;
			eye.bound = false;
			eye.rendered = false;
			eye.image_index = 0;
		}
	}

	void ClearActionState()
	{
		m_steer = 0.0f;
		m_throttle = 0.0f;
		m_brake = 0.0f;
		m_race_buttons = 0;
		m_previous_pause = false;
		m_previous_recenter = false;
	}

	bool OwnsPlayer(void* owner) const
	{
		return owner != nullptr && owner == m_player_owner && m_local_player_count == 1 && m_session_running;
	}

	bool StringToPath(const char* text, XrPath* path) const
	{
		const XrResult result = xrStringToPath(m_instance, text, path);
		if (XR_FAILED(result)) {
			LogResult("xrStringToPath", result);
			return false;
		}
		return true;
	}

	void LogResult(const char* operation, XrResult result) const
	{
		char text[XR_MAX_RESULT_STRING_SIZE] = {};
		if (m_instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(m_instance, result, text))) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenXR: %s failed: %s", operation, text);
		}
		else {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenXR: %s failed: %d", operation, result);
		}
	}

	void ShutdownRuntime()
	{
		if (m_frame_begun && m_session != XR_NULL_HANDLE) {
			for (EyeSwapchain& eye : m_eyes) {
				if (eye.acquired || eye.bound) {
					ReleaseEye(eye, false);
				}
			}
			XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
			end_info.displayTime = m_frame_state.predictedDisplayTime;
			end_info.environmentBlendMode = m_blend_mode;
			xrEndFrame(m_session, &end_info);
			m_frame_begun = false;
		}

		if (wglGetCurrentContext() != nullptr) {
			for (EyeSwapchain& eye : m_eyes) {
				if (eye.depth_renderbuffer != 0 && m_gl.delete_renderbuffers != nullptr) {
					m_gl.delete_renderbuffers(1, &eye.depth_renderbuffer);
				}
				if (eye.framebuffer != 0 && m_gl.delete_framebuffers != nullptr) {
					m_gl.delete_framebuffers(1, &eye.framebuffer);
				}
				eye.depth_renderbuffer = 0;
				eye.framebuffer = 0;
			}
		}

		if (m_local_space != XR_NULL_HANDLE) {
			xrDestroySpace(m_local_space);
			m_local_space = XR_NULL_HANDLE;
		}
		for (EyeSwapchain& eye : m_eyes) {
			if (eye.swapchain != XR_NULL_HANDLE) {
				xrDestroySwapchain(eye.swapchain);
			}
			eye = {};
		}
		if (m_session != XR_NULL_HANDLE) {
			xrDestroySession(m_session);
			m_session = XR_NULL_HANDLE;
		}
		if (m_action_set != XR_NULL_HANDLE) {
			xrDestroyActionSet(m_action_set);
			m_action_set = XR_NULL_HANDLE;
		}
		if (m_instance != XR_NULL_HANDLE) {
			xrDestroyInstance(m_instance);
			m_instance = XR_NULL_HANDLE;
		}

		m_system_id = XR_NULL_SYSTEM_ID;
		m_session_state = XR_SESSION_STATE_UNKNOWN;
		m_session_running = false;
		m_exit_requested = false;
		m_origin_valid = false;
		m_recenter_pending = true;
		m_frame_begun = false;
		m_steer_action = XR_NULL_HANDLE;
		m_throttle_action = XR_NULL_HANDLE;
		m_brake_action = XR_NULL_HANDLE;
		m_powerup_action = XR_NULL_HANDLE;
		m_pause_action = XR_NULL_HANDLE;
		m_recenter_action = XR_NULL_HANDLE;
		m_haptic_action = XR_NULL_HANDLE;
		m_hand_paths = {XR_NULL_PATH, XR_NULL_PATH};
		m_pause_pressed = false;
		m_previous_race_buttons = 0;
		m_button_gate = false;
		ClearActionState();
	}

	bool m_requested = false;
	bool m_initialization_failed = false;
	bool m_session_running = false;
	bool m_exit_requested = false;
	bool m_frame_begun = false;
	bool m_origin_valid = false;
	bool m_recenter_pending = true;
	bool m_pause_pressed = false;
	bool m_previous_pause = false;
	bool m_previous_recenter = false;
	bool m_button_gate = false;
	float m_world_scale = kDefaultWorldScale;
	float m_seated_height = 0.0f;
	float m_steer = 0.0f;
	float m_throttle = 0.0f;
	float m_brake = 0.0f;
	uint32_t m_race_buttons = 0;
	uint32_t m_previous_race_buttons = 0;
	uint32_t m_local_player_count = 1;
	void* m_player_owner = nullptr;
	GlFunctions m_gl;
	XrInstance m_instance = XR_NULL_HANDLE;
	XrSystemId m_system_id = XR_NULL_SYSTEM_ID;
	XrSession m_session = XR_NULL_HANDLE;
	XrSpace m_local_space = XR_NULL_HANDLE;
	XrSessionState m_session_state = XR_SESSION_STATE_UNKNOWN;
	XrEnvironmentBlendMode m_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	XrActionSet m_action_set = XR_NULL_HANDLE;
	XrAction m_steer_action = XR_NULL_HANDLE;
	XrAction m_throttle_action = XR_NULL_HANDLE;
	XrAction m_brake_action = XR_NULL_HANDLE;
	XrAction m_powerup_action = XR_NULL_HANDLE;
	XrAction m_pause_action = XR_NULL_HANDLE;
	XrAction m_recenter_action = XR_NULL_HANDLE;
	XrAction m_haptic_action = XR_NULL_HANDLE;
	std::array<XrPath, 2> m_hand_paths = {XR_NULL_PATH, XR_NULL_PATH};
	std::array<EyeSwapchain, kViewCount> m_eyes;
	std::array<XrView, kViewCount> m_views = {XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
	XrFrameState m_frame_state{XR_TYPE_FRAME_STATE};
	Vec3 m_origin_position{0.0f, 0.0f, 0.0f};
	Quat m_origin_orientation{0.0f, 0.0f, 0.0f, 1.0f};
};

VrRuntime& Runtime()
{
	static VrRuntime runtime;
	return runtime;
}

} // namespace

extern "C"
{

	bool RacersVr_IsCompiled(void)
	{
		return true;
	}

	void RacersVr_SetRequested(bool requested)
	{
		Runtime().SetRequested(requested);
	}

	bool RacersVr_IsRequested(void)
	{
		return Runtime().IsRequested();
	}

	bool RacersVr_IsSessionRunning(void)
	{
		return Runtime().IsSessionRunning();
	}

	void RacersVr_SetWorldScale(float game_units_per_meter)
	{
		Runtime().SetWorldScale(game_units_per_meter);
	}

	float RacersVr_GetWorldScale(void)
	{
		return Runtime().GetWorldScale();
	}

	void RacersVr_SetSeatedHeight(float height_meters)
	{
		Runtime().SetSeatedHeight(height_meters);
	}

	float RacersVr_GetSeatedHeight(void)
	{
		return Runtime().GetSeatedHeight();
	}

	void RacersVr_PollEventsAndActions(void)
	{
		Runtime().PollEventsAndActions();
	}

	bool RacersVr_ConsumePausePressed(void)
	{
		return Runtime().ConsumePausePressed();
	}

	void RacersVr_RequestRecenter(void)
	{
		Runtime().RequestRecenter();
	}

	void RacersVr_SetLocalPlayerCount(uint32_t count)
	{
		Runtime().SetLocalPlayerCount(count);
	}

	void RacersVr_ClaimPlayer(void* owner)
	{
		Runtime().ClaimPlayer(owner);
	}

	void RacersVr_ReleasePlayer(void* owner)
	{
		Runtime().ReleasePlayer(owner);
	}

	bool RacersVr_GetSteer(void* owner, float* steer)
	{
		return Runtime().GetSteer(owner, steer);
	}

	bool RacersVr_IsRaceButtonHeld(void* owner, uint32_t button)
	{
		return Runtime().IsRaceButtonHeld(owner, button);
	}

	bool RacersVr_PollRaceButtons(void* owner, bool gate, uint32_t* pressed, uint32_t* released)
	{
		return Runtime().PollRaceButtons(owner, gate, pressed, released);
	}

	enum RacersVrFrameResult RacersVr_BeginFrame(RacersVrFrame* frame)
	{
		return Runtime().BeginFrame(frame);
	}

	bool RacersVr_BeginEye(uint32_t view_index)
	{
		return Runtime().BeginEye(view_index);
	}

	void RacersVr_EndEye(uint32_t view_index)
	{
		Runtime().EndEye(view_index);
	}

	bool RacersVr_EndFrame(void)
	{
		return Runtime().EndFrame();
	}

	bool RacersVr_PulseHaptics(uint32_t hands, float amplitude, float duration, float frequency)
	{
		return Runtime().PulseHaptics(hands, amplitude, duration, frequency);
	}

	void RacersVr_ShutdownSession(void)
	{
		Runtime().ShutdownSession();
	}

} // extern "C"
