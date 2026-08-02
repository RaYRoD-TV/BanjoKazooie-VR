// OpenXR VR support for Lighthouse (Banjo-Kazooie PC port on libultraship / Fast3D).
//
// Public interface is
// platform-agnostic and C-linkage so it can be called from libultraship's C++ Fast3D and from
// the game's C code. No OpenXR / GL types leak out of this header.
#ifndef BK_VR_H
#define BK_VR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// VR view modes (CVar gVRViewMode). The eye-matrix builder and the Engine render path branch on this.
// Slot 2 (the donor layout's Cockpit) has no Banjo equivalent and is skipped by the in-game cycle;
// the numbering is kept so the shared eye-matrix/menu code stays identical across the ports.
typedef enum {
    VR_VIEW_THIRD_PERSON = 0, // game chase cam, life-size stereo (default - the classic view)
    VR_VIEW_FIRST_PERSON = 1, // eye pushed forward to Banjo's head, life-size stereo (bear hidden)
    VR_VIEW_UNUSED_2     = 2, // (cockpit in the flying ports; skipped here)
    VR_VIEW_DIORAMA      = 3, // world shrunk to a tabletop miniature anchored in front of you
    VR_VIEW_THEATER      = 4, // flat game frame on a head-locked screen, no stereo (max comfort)
} VrViewMode;

// --- request / probe ---------------------------------------------------------
// Requested via the --vr CLI flag (or auto-enabled when a headset is detected).
void vr_request_enable(void);
bool vr_is_requested(void);

// Lightweight startup probe: is a VR headset actually connected right now? Creates and tears down a
// throwaway OpenXR instance (no GL context needed) and asks for an HMD system. Lets the same exe
// auto-enable VR when a headset is present and stay flat otherwise.
bool vr_headset_present(void);

// Is an OpenXR session live and actively rendering? Everything VR-specific in the renderer and game
// is gated on this; false == stock flat game.
bool vr_is_active(void);

// Mixed Reality (XR_FB_passthrough). _supported: the runtime created a passthrough layer at boot.
// _active: MR is toggled on this frame -> the eye background is transparent and the real room is
// composited behind the game. Renderer/engine branch on _active (clear color, background suppression).
bool vr_passthrough_supported(void);
bool vr_passthrough_active(void);

// The headset's display refresh rate in Hz (from the runtime's predicted display period). Drives the
// render frame-rate target so the headset runs at its native refresh. ~90 until the first frame.
int  vr_display_refresh_hz(void);

// --- per-frame loop ----------------------------------------------------------
// Lazy-boot OpenXR, poll events, xrWaitFrame/xrBeginFrame, locate the per-eye views and build the
// per-eye camera-space -> eye-clip matrices. Call once before the eye loop.
void vr_begin_frame(void);

int  vr_eye_count(void);
int  vr_eye_width(int eye);
int  vr_eye_height(int eye);

// Camera-space -> eye-clip matrix (16 floats, row-vector: clip = p_cam * M). The renderer substitutes
// this for the game's perspective projection load; SF64's camera lives on the modelview stack, so the
// composition is v_obj * modelview(incl. camera) * A * V_head * P_eye.
const float* vr_eye_viewproj(int eye);
// ZERO-SEPARATION eye view-projection (16 floats, row-vector): identical to vr_eye_viewproj in
// every term except the eye position is the shared head centre - no per-eye IPD offset. The
// interpreter substitutes it for the game-registered SKY perspective load (vr_sky_persp_match),
// so the sky fuses at infinity while the world keeps full stereo.
const float* vr_sky_viewproj(int eye);
// The game-sky angular half-extents (radians) the renderer's per-vertex sky remap would read back;
// reserved with sane defaults until a dome is built.
void vr_set_sky_fov(float halfH, float halfV);
float vr_sky_fov_h(void);
float vr_sky_fov_v(void);
float vr_sky_decouple_rad(void);
// True when the 3D sky dome (src/engine/vr_skydome.c) replaces the flat 2D background: the CVar gVRSkyDome
// is on and a dome DL has been built. Drives the interpreter's dome pass and tells the game to skip its
// own flat starfield/backdrop.
bool vr_sky_dome_active(void);
// Retired 2D-remap path (the real 3D dome above replaces it) - stays false.
bool vr_sky_remap_active(void);
// The game pushes the player camera each frame (world-space eye/at/up, game units) so the dome's view-proj
// carries the SHIP's orientation as well as the head's - it sweeps as the ship turns and holds still as you
// look around, anchored on yaw and pitch.
void vr_set_sky_camera(const float eye[3], const float at[3], const float up[3]);

// LIVE render path: the interpreter renders the scene into its managed off-screen fb, then these blit
// that fb's color texture into the OpenXR swapchain image (eye projection layer / head-locked panel).
// Returns false to skip this frame.
bool vr_submit_eye_texture(int eye, unsigned int glTextureId, int w, int h);
bool vr_submit_panel_texture(unsigned int glTextureId, int w, int h);
// Present the desktop window (with the ImGui menu) on the head-locked panel so the menu is usable in
// VR. srcW/srcH = current window pixel size.
bool vr_present_desktop_panel(int srcW, int srcH);

// Stable VR menu render: bind + clear a private offscreen FBO (caller renders ImGui into it), then mirror
// it to the desktop window and present the stable texture on the head-locked panel. Avoids the GL_BACK
// double-buffer flicker. w/h = window pixel size.
void vr_menu_render_begin(int w, int h);
void vr_menu_render_present(int w, int h);
// Mirror the menu onto the desktop window - call ONCE after the eye loop (avoids flatscreen flicker).
void vr_menu_mirror_desktop(int w, int h);
// Mirror the rendered VR game frame (managed fb texture: last eye / panel) onto the desktop window when the
// MENU IS CLOSED, so the flat window shows the game instead of flickering stale back-buffers. Call ONCE per
// frame after the eye loop, before the engine SwapBuffers. srcW/srcH = managed fb size; dstW/dstH = window.
// cropForEye: the stereo eye texture carries a very wide asymmetric frustum that reads as a fisheye
// on a flat monitor - crop the central region to the window's aspect so every view mode gives a
// watchable desktop picture. Pass 0 for panel/menu sources (already flat-framed).
void vr_mirror_game_desktop(unsigned int glTex, int srcW, int srcH, int dstW, int dstH, int cropForEye);
// Apply gVRImGuiOpacity to the VR menu panel (alpha into the menu texture). Call after the menu's ImGui
// has rendered into the FBO and before the panel is presented. No-op at full opacity.
void vr_menu_apply_opacity(void);

// Head-locked HUD/menu overlay (its own swapchain layer).
int  vr_overlay_width(void);
int  vr_overlay_height(void);
bool vr_begin_overlay(bool sky);
void vr_end_overlay(bool sky);

// FLATSCREEN-ON-A-PANEL: for non-gameplay screens (title/menus/map), render the flat frame once into
// the panel swapchain and present it on one large head-locked quad.
void vr_set_panel_mode(bool on);
bool vr_begin_panel(void);
void vr_end_panel(void);

// xrEndFrame with the stereo projection layer (+ HUD quad / panel).
void vr_submit(void);

// --- live-tunable framing knobs (Enhancements > VR menu, read from CVars each frame) ----------
// VR view mode (Third Person / First Person / Theater / Diorama). See VrViewMode. Engine routes
// Theater to the flat panel path, the others to the per-eye stereo path.
int  vr_get_view_mode(void);     void vr_set_view_mode(int mode);
// (Retired: the camera-space push was never consumed by the game, so the distance knob did
// nothing. Camera distance now scales the game's own orbit distance - port_vrCamDistScale.)
// Game units the First Person eye currently sits ahead of the chase camera (the forward push, eased).
// 0 outside First Person.
float vr_fp_forward_game_units(void);
// How the Fast3D interpreter should treat fog on the current pass: 0 = no fog, 1 = world-distance fog
// (factor linear in clip w, i.e. real view depth - projection-independent), 2 = stock fog untouched.
// Returns 2 outside VR and in Diorama / Theater, which render correctly with stock fog. gVRFogMode.
int  vr_fog_mode(void);
// World-distance fog coefficients: fogFactor(0..255) = clamp(clip_w * mul + off). Near/far come from
// gVRFogNear / gVRFogFar (meters at life size) through the active view mode's world scale.
void vr_fog_linear_coeffs(float* mul, float* off);
// Game units per meter: how big the world feels. Bigger = world appears smaller.
float vr_get_world_scale(void);  void vr_set_world_scale(float v);
// Stereo separation strength (0 = mono, 1 = full IPD). Lower is gentler / less cross-eye.
float vr_get_stereo(void);       void vr_set_stereo(float v);
// 6DoF head-motion amount (0 = orientation only / locked translation, 1 = full positional).
float vr_get_head_scale(void);   void vr_set_head_scale(float v);
// Eye height offset above the head anchor (meters).
float vr_get_eye_height(void);   void vr_set_eye_height(float v);
// Head-locked menu panel placement.
float vr_get_menu_dist(void);    void vr_set_menu_dist(float v);
float vr_get_menu_size(void);    void vr_set_menu_size(float v);
// In-game 2D HUD (radar, boost gauge, radio, reticles): repositioned onto a head-locked plane in the
// eye render.
float vr_get_hud_scale(void);    void vr_set_hud_scale(float v); // fraction of FOV the HUD fills
float vr_get_hud_dist(void);     void vr_set_hud_dist(float v);  // HUD plane distance (meters)
// While set, the "HUD" plane carries MENU content (the pause menu / VR overlay): it is built from
// the SCREEN distance/size knobs instead of the gameplay HUD's, so tuning the in-game HUD never
// moves the menus and vice versa. The game-side glue sets this each tick from the pause state.
void vr_set_hud_menu_mode(int on);
// Damped head offset from the rest pose, METRES, camera space (+x right, +y up, -z forward).
// The game side collides this against the world - the VR layer has no collision of its own.
void vr_get_head_offset_m(float out[3]);
// Head-locked HUD view-projection for an eye (16 floats, row-vector). The renderer multiplies the game's
// 2D ortho matrix by this for post-3D HUD draws so the HUD sits at a comfortable distance. The plane
// keeps a few millimetres of authored ortho-Z ordering (widget layers z-fought when flattened).
const float* vr_hud_viewproj(int eye);
// Same plane with Z fully flattened - for 3D content drawn INSIDE the HUD (its own perspective load):
// perspective clip-Z through the ordering scale reads as metres of depth and goes cross-eyed.
const float* vr_hud_viewproj_flat(int eye);
// Full-FOV head-locked view-projection for an eye (16 floats, row-vector). The renderer multiplies a
// screen-space 2D ortho matrix by this (pre-3D background / intro overlays) so that 2D fills the view
// head-locked instead of being emitted raw - raw screen-space 2D doubles under the per-eye asymmetric
// submit.
const float* vr_full2d_viewproj(int eye);

void  vr_reset_defaults(void);

// Head orientation offsets (radians) from facing forward.
float vr_head_yaw_rad(void);
float vr_head_pitch_rad(void);

// --- motion controllers (OpenXR actions) --------------------------------------
// One gameplay action set: both thumbsticks, face buttons, menu button, stick clicks, triggers,
// grips, and rumble. Suggested bindings cover Quest Touch (incl. the Quest 3 / Pro Touch Plus
// profile), Valve Index, HP Reverb G2, WMR wands, Vive wands and the khr simple fallback.
// The pad-read path merges this state into the first N64 pad ahead of its edge detection, so the
// controllers drive gameplay and every menu exactly like a gamepad.
enum {
    VR_BTN_A        = (1 <<  0), // right controller A
    VR_BTN_B        = (1 <<  1), // right controller B
    VR_BTN_X        = (1 <<  2), // left controller X
    VR_BTN_Y        = (1 <<  3), // left controller Y
    VR_BTN_MENU     = (1 <<  4), // left controller menu button
    VR_BTN_LSTICK   = (1 <<  5), // left thumbstick click
    VR_BTN_RSTICK   = (1 <<  6), // right thumbstick click
    VR_BTN_LTRIGGER = (1 <<  7), // left index trigger (analog, latched digital with hysteresis)
    VR_BTN_RTRIGGER = (1 <<  8), // right index trigger
    VR_BTN_LGRIP    = (1 <<  9), // left grip squeeze
    VR_BTN_RGRIP    = (1 << 10), // right grip squeeze
};
// True while the session is focused and the action set is attached (controllers deliver input).
bool vr_controllers_active(void);
// VR_BTN_* mask of currently-held controls (0 when inactive).
unsigned vr_controller_buttons(void);
// Thumbstick state, -1..1 with +x right and +y up. hand: 0 = left, 1 = right.
void vr_controller_stick(int hand, float out[2]);
// Arm the rumble on both hands: short bursts re-arm each frame while armed, so a runtime that
// drops a stop request can't strand the motors buzzing.
void vr_controller_rumble(float strength, float seconds);
void vr_controller_rumble_stop(void);

// In-game recenter ("bring the world to my gaze"): re-zeroes the view yaw + position + HEIGHT on
// the next frame (the neutral head becomes the origin the eye-height defaults are tuned against)
// and re-parks the HUD/menu panel in front of the new forward. Fires automatically once at the
// first FOCUSED frame so the starting view is correct on any runtime; manually, hold both
// triggers (~0.5 s, haptic tick confirms) or use the RECENTER row in the VR menu.
void vr_recenter(void);

// First person FLIP CAM: the angle (radians) the eye view - and the sky with it - is rotated by, so
// the headset somersaults with Banjo's flip jump and beak buster. Fed per game tick from the state
// machine's own animation clock; 0 is upright, positive pitches forward (nose down). Eased to the
// headset's frame rate inside, and ignored outside first person.
void vr_set_flip_angle(float radians);

// Tell the eye builder whether VR First Person is actually driving the camera this tick. While the
// GAME owns it (dialogue, scripted reveals), the First Person framing offsets fade out so an
// authored shot is shown as authored instead of being pushed a metre into the nearest wall.
void vr_set_fp_framing(int on);

void vr_shutdown(void);

// --- headless verification seam ------------------------------------------------
// The dev machine has no headset, so the per-eye render path must be provable WITHOUT one: these
// build the same matrix set vr_begin_frame would (identity head orientation, symmetric fov, current
// scale knobs, and the LIVE per-eye separation math around the centre) so the interpreter's
// RunVrEye can run headless per eye, and dump its texture to a BMP on disk. Driven by the
// BK_VR_EYEDUMP env var (see Engine.cpp); no effect on normal play. eye: 0 = left, 1 = right.
// BK_VR_FOVTEST="l,r,u,d" (degrees, signed) overrides the synthetic fov with an asymmetric one, so
// canted-display geometry (the one live variable a symmetric synth never exercises) is testable headless.
void vr_debug_synth_matrices(int eye, float eyeVP[16], float skyVP[16], float hudVP[16], float hudFlatVP[16],
                             float full2D[16]);
int  vr_debug_dump_texture(unsigned int glTextureId, int w, int h, const char* path);

#ifdef __cplusplus
}
#endif

#endif // BK_VR_H
