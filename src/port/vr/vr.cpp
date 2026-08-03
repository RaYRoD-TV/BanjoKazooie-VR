// OpenXR VR support for Lighthouse (Banjo-Kazooie PC port on libultraship / Fast3D).
//
// Boots an OpenXR session bound to the GL
// context that libultraship's OpenGL backend creates (XR_USE_GRAPHICS_API_OPENGL + WGL on Windows),
// creates stereo swapchains, and renders the game's display list ONCE PER EYE into each eye's swapchain
// image (with a depth buffer), injecting a per-eye camera-space -> eye-clip matrix into Fast3D so the
// two eyes differ -> real stereoscopic depth. A head-locked quad carries the 2D HUD; non-gameplay
// screens go on a flat panel.
//
// GL-backend only (OpenXR is bound to WGL); VR forces the FAST3D_SDL_OPENGL backend at startup.

#include "vr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

// --- request flag (platform-agnostic, set by the --vr CLI handler) ----------
static bool sRequested = false;
extern "C" void vr_request_enable(void) { sRequested = true; }
extern "C" bool vr_is_requested(void) { return sRequested; }

// ENABLE_VR as well as _WIN32: with the VR option off there is no OpenXR loader to link (and no
// openxr headers on the include path), so a non-VR Windows build takes the stub block at the bottom
// exactly like the other platforms do.
#if defined(ENABLE_VR) && defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h> // IUnknown - referenced by openxr_platform.h's XR_USE_PLATFORM_WIN32 structs

#include <GL/glew.h> // libultraship's GL backend uses GLEW (glewInit); reuse its loaded pointers

#define XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// libultraship console-variable bridge (extern "C") - lets the in-game VR options menu drive the
// tunables below by reading the CVars each frame.
extern "C" float CVarGetFloat(const char* name, float defaultValue);
extern "C" int   CVarGetInteger(const char* name, int defaultValue);
extern "C" void  CVarSetInteger(const char* name, int value);
extern "C" void  CVarClear(const char* name);

// ---- OpenXR state -----------------------------------------------------------
static XrInstance     sInstance   = XR_NULL_HANDLE;
static XrSystemId     sSystemId   = XR_NULL_SYSTEM_ID;
static XrSession      sSession    = XR_NULL_HANDLE;
static XrSpace        sLocalSpace = XR_NULL_HANDLE; // world-locked reference space
static XrSpace        sViewSpace  = XR_NULL_HANDLE; // VIEW reference space (head-locked) for overlays
static XrSessionState sState      = XR_SESSION_STATE_UNKNOWN;

// XR_FB_passthrough (Mixed Reality): show the real room behind the game. Created at boot if the runtime
// supports it; the layer is submitted BEHIND the projection when MR is toggled on (gVRPassthrough).
static bool                 sHasPassthrough = false;
static XrPassthroughFB      sPassthrough    = XR_NULL_HANDLE;
static XrPassthroughLayerFB sPtLayer        = XR_NULL_HANDLE;
static PFN_xrCreatePassthroughFB       pfnCreatePassthrough       = NULL;
static PFN_xrDestroyPassthroughFB      pfnDestroyPassthrough      = NULL;
static PFN_xrCreatePassthroughLayerFB  pfnCreatePassthroughLayer  = NULL;
static PFN_xrDestroyPassthroughLayerFB pfnDestroyPassthroughLayer = NULL;
static PFN_xrPassthroughStartFB        pfnPassthroughStart        = NULL;
static PFN_xrPassthroughLayerResumeFB  pfnPassthroughLayerResume  = NULL;
static PFN_xrPassthroughLayerPauseFB   pfnPassthroughLayerPause   = NULL;
static bool sHasAlphaBlend = false; // runtime offers ALPHA_BLEND env mode (passthrough without XR_FB_passthrough)
static bool sPassthroughOn = false; // MR toggled on this frame (gVRPassthrough && (FB-passthrough || alpha-blend))

static bool sBootTried  = false;
static bool sRunning    = false;
static bool sFrameBegun = false;
static bool sViewsValid = false;
static bool sPoseTracked = false;

static XrFrameState            sFrameState;
static uint32_t                sViewCount = 0;
static XrViewConfigurationView sViewConfigs[2];
static XrView                  sViews[2];

typedef struct {
    XrSwapchain handle;
    uint32_t w, h, imgCount;
    XrSwapchainImageOpenGLKHR* images;
} VrSwapchain;
static VrSwapchain sEye[2];

static GLuint   sEyeFbo     = 0; // DRAW FBO: swapchain image as color attachment (blit target)
static GLuint   sEyeDepthRB = 0; // shared depth renderbuffer (legacy direct-render path)
static GLuint   sBlitReadFbo = 0; // READ FBO: the interpreter's managed VR texture is attached here to blit
static XrCompositionLayerProjectionView sProjViews[2];
static uint32_t sEyeImgIdx[2];

static VrSwapchain sHud;                      // HUD/menu swapchain (head-locked / panel)
static GLuint      sOverlayFbo     = 0;
static GLuint      sOverlayDepthRB = 0;
static uint32_t    sHudImgIdx      = 0;
static bool        sHudReady       = false;
static bool        sPanelMode      = false;
static int         sEyesSubmitted  = 0; // eyes blitted into swapchains this frame (gates the proj layer)
static int         sStereoRamp     = 0;  // eases eye separation in from flat when eye rendering resumes
static const int   kStereoRampLen  = 45; // ~0.5-0.6s: lets the course-open chase cam settle before full depth
static const int   sOverlayW = 1920;
static const int   sOverlayH = 1080;

// --- framing tunables (see vr.h) ----------------------------------------------
static float sWorldScale = 100.0f; // game units per meter for THIRD PERSON (lower = bigger world).
                                   // Banjo-Kazooie runs at SM64-family scale: the chase camera sits
                                   // ~500-700 units behind Banjo and reads as ~5-7 m, and the bear
                                   // stands ~150 units ≈ 1.5 m. Live-tunable (gVRWorldScale).
static float sStereoScale = 1.0f;  // stereo separation strength
static float sHeadScale   = 0.1f;  // 6DoF head-motion amount (1 = full positional)
static float sEyeHeight   = -1.09f; // meters above the head anchor for THIRD PERSON (re-trimmed after
                                    // the origin normalization - tuned in-headset on the fixed build)
static float sMenuDist    = 3.4f; // menu/title panel distance (m) - comfortable, not in your face
static float sMenuSize    = 3.6f; // menu/title panel width (m)

// VR view mode (gVRViewMode). 0=Third Person (chase cam, life-size), 1=First Person (eye pushed to the
// head), 2=unused here (Cockpit in the flying ports), 3=Diorama (world shrunk to a tabletop),
// 4=Theater (flat frame on a head-locked screen). The eye-matrix builder branches on this; Engine.cpp
// routes Theater to the flat panel path. Tunables below are read from CVars each frame so they apply live.
static int   sViewMode           = 1;       // VrViewMode - First Person ships as the default
static float sFirstPersonForward = 0.00f;   // meters of extra forward push in First Person. The game
                                            // camera itself is parked at Banjo's eyes in VR FP
                                            // (port_vrFirstPerson_override), so this is a fine-tune
                                            // nudge, not the mechanism - 0 = exactly at his head.
static float sFPForwardCur       = 0.0f;    // eased First-Person push - see vr_begin_frame.
static float sFPEyeHeightCur     = 0.0f;    // eased First-Person eye height, same treatment
static bool  sFpFramingOn        = true;    // false while the GAME owns the camera (see vr_set_fp_framing)
static float sFirstPersonScale   = 100.0f;  // First Person world scale (units/m), its OWN knob (like
                                            // Diorama) so it doesn't shrink Third Person.
static float sFirstPersonEyeHeight = 0.0f;  // First Person eye height (m), its OWN knob
static float sThirdPersonDist    = 0.0f;    // Third Person camera distance offset (m): + = further behind
                                            // Banjo, - = closer. 0 = stock chase distance.
static float sCockpitForward     = 0.0f;    // slot 2 unused in Banjo (donor cockpit mode); kept so the
static float sCockpitHeight      = -0.15f;  // shared eye-matrix code stays identical across the ports.
static float sDioramaWorldScale  = 8000.0f; // Diorama world scale (game units/m), INDEPENDENT of the global
                                            // World Scale so tuning the tabletop never touches Third/First
                                            // person. Higher = smaller tabletop. 8000 with the forward and
                                            // downward placement below reads as a MODEL on a table in front
                                            // of you rather than a world you happen to be huge in.
static float sDioramaDist        = 0.05f;   // meters the tabletop sits in front of you (Diorama)
static float sDioramaHeight      = -0.40f;  // meters the tabletop is offset vertically (Diorama; - = below eye)
static float sFlipRad            = 0.0f;    // first person flip cam: eye-view pitch, radians (0 = upright)
static float sFlipTargetRad      = 0.0f;    // ...and where it is heading, set per game tick
static float sMenuOpacity        = 0.85f;   // VR menu/HUD panel opacity (1 = opaque, lower = see game through)
static float sImGuiOpacity       = 0.95f;   // settings (ImGui) menu opacity - its OWN knob, decoupled from the panel

// World-distance fog for the stereo modes (gVRFogMode = 1). Near/far are METERS AT LIFE SIZE (the default
// 100 game-units-per-meter mapping), so the fogged distance stays anchored to the WORLD - changing a view
// mode's scale knob never shifts which objects are fogged. See vr_fog_linear_coeffs().
// Banjo's worlds are enclosed and its fog is atmosphere (Clanker's Cavern murk, Freezeezy haze), not a
// draw-limit curtain like the flying games - so the default range sits further out relative to the level
// span and mostly leaves clear levels clear while still taming the saturated stock formula where fog is on.
static int   sFogMode  = 1;      // gVRFogMode: 0 = no fog, 1 = world-distance fog, 2 = stock N64 fog
static float sFogNearM = 60.0f;  // where fog starts (m at life size)
static float sFogFarM  = 250.0f; // where fog saturates (m at life size)

// The damped head offset from the rest pose, in METRES, camera space (+x right, +y up, -z forward).
// The game side turns this into a collision probe - the VR layer has no collision of its own.
static float sHeadOffsetM[3] = { 0.0f, 0.0f, 0.0f };
extern "C" void vr_get_head_offset_m(float out[3]) {
    out[0] = sHeadOffsetM[0];
    out[1] = sHeadOffsetM[1];
    out[2] = sHeadOffsetM[2];
}

// 6DoF damping rest pose (capture after warmup so tracking has settled).
static float sHeadRest[3] = { 0, 0, 0 };
static bool  sHeadRestSet = false;
static int   sHeadWarmup  = 0;
static XrPosef sRenderPose[2];
static XrFovf  sRenderFov[2];

// Per-eye camera-space -> eye-clip matrices (row-vector; clip = p_cam * sEyeVP).
static float sEyeVP[2][4][4];
static float sSkyVP[2][4][4]; // rotation-only sky view-proj (zero translation -> infinity); reserved for a
                              // future Banjo sky dome (unused while vr_sky_dome_active() stays false)
// Game-sky angular half-extents (radians) the interpreter's per-vertex sky remap would read back. Reserved
// with sane defaults (~50 deg H, ~37.5 deg V) until a dome is built; the pre-3D 2D background rides
// the full-FOV head-locked plane instead.
static float sSkyFovH = 0.8727f;
static float sSkyFovV = 0.6545f;

// Menu panel world-lock anchor (captured at menu open, yaw-only).
static bool  sPanelAnchorValid = false;
static float sPanelAnchorPos[3] = { 0, 0, 0 };
static float sPanelAnchorQy = 0.0f, sPanelAnchorQw = 1.0f;

// App-side recenter: "bring the world to my gaze", available from inside the game (hold the left
// stick click, or the RECENTER row in the VR menu) - the runtime's own recenter lives in a system
// dash many players never open, and a LOCAL space that comes up rotated (or flat-out upside down on
// a bad tracking boot) otherwise stays wrong for the whole session. The correction is a yaw+position
// remap applied to every located view pose, so the eyes, the HUD plane, and the panel anchor all
// agree. Captured ABSOLUTE from the raw runtime pose each time (never composed with itself), and
// dropped entirely when the runtime itself recenters (its new LOCAL space supersedes ours).
static bool  sRecenterSet = false;
static bool  sRecenterRequest = false;
static bool  sAutoRecentered = false;    // one automatic recenter at the first FOCUSED frame
static bool  sSpaceChangePending = false; // runtime moved LOCAL space: re-capture once the head is calm
static float sRcQy = 0.0f, sRcQw = 1.0f; // conjugate of the captured head yaw twist
static float sRcPos[3] = { 0, 0, 0 };    // captured head x/Y/z - height included: runtimes disagree
                                         // about where LOCAL's origin sits (eye level, floor, a
                                         // standing calibration), and every eye-height default was
                                         // tuned assuming origin = the player's eyes. Recentering
                                         // the height makes the defaults mean the same thing on
                                         // every setup ("all three modes start in the ground" was
                                         // a seated player under a standing-height origin).

// Head-locked HUD plane: the in-game 2D HUD (item box, positions, lap, etc.) is repositioned in the eye
// render onto a view-space quad so it sits at a comfortable distance instead of at your face. Built per
// eye from the symmetric fov; zero eye-offset -> zero disparity (relaxed, far-feeling), head-locked.
static float sHudVP[2][4][4];
static float sHudVPFlat[2][4][4]; // same plane, Z fully flattened - for 3D drawn INSIDE the HUD, whose
                                  // perspective clip-Z through the ordering scale reads as metres of depth
static float sFull2DVP[2][4][4]; // full-FOV head-locked plane for screen-space 2D in the eye pass (anti-double)
static float sHudScale = 0.35f; // fraction of the eye FOV the HUD fills (smaller = tighter, more central)
// HUD LOCK = WORLD (gVRHudWorldLock): yaw-pin the in-race HUD plane to the room direction it was facing
// when enabled, so you can turn your head and look AROUND it instead of it being glued dead-centre. It
// still follows your head POSITION (no drift while driving); only the head's yaw is decoupled.
static float sHudYawAnchor   = 0.0f;   // room yaw the HUD is pinned to (captured on enable)
static bool  sHudYawAnchored = false;
static float sHudYawOffset   = 0.0f;   // yaw applied to the HUD plane this frame (0 = head-locked)
static int   sHudMenuMode = 0;  // pause/overlay up: the HUD plane carries menu content -> use the
                                // SCREEN knobs (sMenuDist/sMenuSize) so HUD sliders never move menus
static float sHudDistM = 2.90f; // HUD plane distance (meters); pushed back so the wide course-name banner
                                // (shares this plane) isn't in your face. Live-tunable via gVRHudDist.

static PFN_xrGetOpenGLGraphicsRequirementsKHR pfnGetGLReq = NULL;

extern "C" const float* vr_hud_viewproj(int eye) {
    return (eye >= 0 && eye < 2) ? &sHudVP[eye][0][0] : NULL;
}
extern "C" const float* vr_hud_viewproj_flat(int eye) {
    return (eye >= 0 && eye < 2) ? &sHudVPFlat[eye][0][0] : NULL;
}
extern "C" const float* vr_full2d_viewproj(int eye) {
    return (eye >= 0 && eye < 2) ? &sFull2DVP[eye][0][0] : NULL;
}

extern "C" bool vr_is_active(void) { return sRunning; }

// Mixed Reality (passthrough). _supported = the runtime created a passthrough layer at boot; _active =
// it is toggled on this frame (transparent background + room composited behind the game).
extern "C" bool vr_passthrough_supported(void) { return sHasPassthrough || sHasAlphaBlend; }
extern "C" bool vr_passthrough_active(void)    { return sRunning && sPassthroughOn && sViewMode == VR_VIEW_DIORAMA; }

extern "C" int  vr_eye_count(void) { return (int)sViewCount; }
extern "C" int  vr_eye_width(int eye)  { return (eye >= 0 && eye < 2) ? (int)sEye[eye].w : 0; }
extern "C" int  vr_eye_height(int eye) { return (eye >= 0 && eye < 2) ? (int)sEye[eye].h : 0; }
extern "C" int  vr_overlay_width(void)  { return sOverlayW; }
extern "C" int  vr_overlay_height(void) { return sOverlayH; }
extern "C" const float* vr_eye_viewproj(int eye) {
    return (eye >= 0 && eye < 2) ? &sEyeVP[eye][0][0] : NULL;
}
extern "C" const float* vr_sky_viewproj(int eye) {
    return (eye >= 0 && eye < 2) ? &sSkyVP[eye][0][0] : NULL;
}
// Game-sky angular half-extents (radians). Guarded to a sane range; reserved for a future sky dome.
extern "C" void vr_set_sky_fov(float halfH, float halfV) {
    if (halfH > 0.01f) sSkyFovH = (halfH < 1.55f) ? halfH : 1.55f;
    if (halfV > 0.01f) sSkyFovV = (halfV < 1.55f) ? halfV : 1.55f;
}
extern "C" float vr_sky_fov_h(void) { return sSkyFovH; }
extern "C" float vr_sky_fov_v(void) { return sSkyFovV; }
extern "C" float vr_sky_decouple_rad(void) { return 0.0f; }
// Banjo draws its sky as a filled backdrop rect plus world geometry, so there is no gradient dome SPHERE
// starfield / backdrop sprites themselves), so the dome path - which paints a sphere and suppresses
// vr_sky_dome_active() is now defined game-side (src/engine/vr_skydome.c) - true when the CVar is on and a
// dome DL has been built. It drives the interpreter's dome pass + flat-sky suppression AND is what the game
// checks to skip drawing its own flat starfield/backdrop. The old 2D remap is retired in favour of the real
// 3D dome, so vr_sky_remap_active stays off (the pre-3D 2D that remains rides the head-locked full-2D plane).
// No VR sky dome is built for Banjo. The game's sky is a filled backdrop rect plus ordinary world
// geometry (mountains, Grunty's lair) that already carries real depth, so there is nothing to replace
// with a sphere - unlike the flying ports, whose "sky" was scrolling 2D sprites. Both sky paths stay
// off and the pre-3D 2D rides the head-locked full-FOV plane instead.
extern "C" bool vr_sky_dome_active(void) { return false; }
extern "C" bool vr_sky_remap_active(void) { return false; }

// The game pushes the player camera each frame (world-space eye/at/up, game units) so the sky VP can carry
// the SHIP's orientation as well as the head's - the dome then sweeps as the ship turns and stays put as you
// look around, anchored on yaw AND pitch. Stored as the world->camera ROTATION (translation dropped: the
// dome is at infinity).
static float sSkyCamRot[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
static bool  sSkyCamValid = false;
extern "C" void vr_set_sky_camera(const float eye[3], const float at[3], const float up[3]) {
    // Build the world->camera rotation the same way guLookAt does (row-vector: world dir * R = camera dir).
    float fx = at[0] - eye[0], fy = at[1] - eye[1], fz = at[2] - eye[2];
    float fl = sqrtf(fx * fx + fy * fy + fz * fz);
    if (fl < 1e-4f) { sSkyCamValid = false; return; }
    fx /= fl; fy /= fl; fz /= fl;                 // forward (camera looks along +f)
    float sx = fy * up[2] - fz * up[1];           // s = f x up (right)
    float sy = fz * up[0] - fx * up[2];
    float sz = fx * up[1] - fy * up[0];
    float sl = sqrtf(sx * sx + sy * sy + sz * sz);
    if (sl < 1e-4f) { sSkyCamValid = false; return; }
    sx /= sl; sy /= sl; sz /= sl;
    float ux = sy * fz - sz * fy;                 // u = s x f (true up)
    float uy = sz * fx - sx * fz;
    float uz = sx * fy - sy * fx;
    // Columns = camera basis in world; world dir * R gives (dot with s, u, -f) = camera-space dir. Camera
    // looks down -Z, so the third column is -f.
    sSkyCamRot[0][0] = sx; sSkyCamRot[0][1] = ux; sSkyCamRot[0][2] = -fx; sSkyCamRot[0][3] = 0.0f;
    sSkyCamRot[1][0] = sy; sSkyCamRot[1][1] = uy; sSkyCamRot[1][2] = -fy; sSkyCamRot[1][3] = 0.0f;
    sSkyCamRot[2][0] = sz; sSkyCamRot[2][1] = uz; sSkyCamRot[2][2] = -fz; sSkyCamRot[2][3] = 0.0f;
    sSkyCamRot[3][0] = 0.0f; sSkyCamRot[3][1] = 0.0f; sSkyCamRot[3][2] = 0.0f; sSkyCamRot[3][3] = 1.0f;
    sSkyCamValid = true;
}

// --- matrix helpers (row-vector convention: clip = v * M, M[row][col]) -------
// Row-vector-convention axis rotations (same convention as the A/V matrices here).
static void mat_rot_y(float m[4][4], float a) {
    float c = cosf(a), s = sinf(a);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) m[i][j] = (i == j) ? 1.0f : 0.0f;
    m[0][0] = c; m[0][2] = -s;
    m[2][0] = s; m[2][2] = c;
}
static void mat_rot_x(float m[4][4], float a) {
    float c = cosf(a), s = sinf(a);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) m[i][j] = (i == j) ? 1.0f : 0.0f;
    m[1][1] = c; m[1][2] = s;
    m[2][1] = -s; m[2][2] = c;
}

static void mat_mul(float out[4][4], const float a[4][4], const float b[4][4]) {
    float t[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(out, t, sizeof(t));
}

// OpenXR fov -> projection (row-vector, OpenGL clip z in [-1, 1]).
static void mat_proj_fov(float m[4][4], XrFovf fov, float zn, float zf) {
    float l = tanf(fov.angleLeft), r = tanf(fov.angleRight);
    float dn = tanf(fov.angleDown), up = tanf(fov.angleUp);
    float w = r - l, h = up - dn;
    memset(m, 0, sizeof(float) * 16);
    m[0][0] = 2.0f / w;
    m[1][1] = 2.0f / h;
    m[2][0] = (r + l) / w;
    m[2][1] = (up + dn) / h;
    m[2][2] = -(zf + zn) / (zf - zn);
    m[2][3] = -1.0f;
    m[3][2] = -(2.0f * zf * zn) / (zf - zn);
}

// XrPosef -> world->eye view matrix (row-vector): rigid inverse of the eye's world transform.
static void mat_view_from_pose(float m[4][4], XrPosef pose) {
    float x = pose.orientation.x, y = pose.orientation.y, z = pose.orientation.z, w = pose.orientation.w;
    float Rrv[3][3] = {
        { 1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + z * w),       2.0f * (x * z - y * w) },
        { 2.0f * (x * y - z * w),        1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + x * w) },
        { 2.0f * (x * z + y * w),        2.0f * (y * z - x * w),        1.0f - 2.0f * (x * x + y * y) },
    };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m[i][j] = Rrv[j][i];
    m[0][3] = m[1][3] = m[2][3] = 0.0f;
    float px = pose.position.x, py = pose.position.y, pz = pose.position.z;
    m[3][0] = -(px * m[0][0] + py * m[1][0] + pz * m[2][0]);
    m[3][1] = -(px * m[0][1] + py * m[1][1] + pz * m[2][1]);
    m[3][2] = -(px * m[0][2] + py * m[1][2] + pz * m[2][2]);
    m[3][3] = 1.0f;
}

// FLIP CAM: post-multiply an eye-space PITCH onto a view matrix, so the whole view somersaults with
// Banjo (V' = V * Rx). Eye-side, so it is orthogonal to head tracking - you keep looking around
// normally while the world turns over. No-op at rest and outside first person, which is what keeps
// third person and the tabletop diorama untouched.
static void mat_flip_apply(float V[4][4]) {
    if (sFlipRad == 0.0f || sViewMode != 1) {
        return;
    }
    const float cp = cosf(sFlipRad), sp = sinf(sFlipRad);
    float Rx[4][4] = { { 0 } };
    Rx[0][0] = 1.0f; Rx[3][3] = 1.0f;
    Rx[1][1] = cp;   Rx[1][2] = sp;
    Rx[2][1] = -sp;  Rx[2][2] = cp;
    float out[4][4];
    mat_mul(out, V, Rx);
    memcpy(V, out, sizeof(out));
}

// Build sEyeVP[eye] = A (camera-space game units -> meters, with eye-height offset) * V (world->eye,
// meters) * P (eye projection). The renderer composes it after the game's world->camera lookAt:
//   P_matrix_vr = lookAt_game * sEyeVP[eye]
// so a vertex is v_obj * modelview * lookAt * A * V_head * P_eye.
static void vr_build_eye_matrix(int eye) {
    XrPosef pose = sViews[eye].pose;
    float cx = 0.5f * (sViews[0].pose.position.x + sViews[1].pose.position.x);
    float cy = 0.5f * (sViews[0].pose.position.y + sViews[1].pose.position.y);
    float cz = 0.5f * (sViews[0].pose.position.z + sViews[1].pose.position.z);

    // 6DoF damping: scale the head's offset from a captured rest pose by sHeadScale (1 = full).
    if (eye == 0 && !sHeadRestSet && ++sHeadWarmup >= 15) {
        sHeadRest[0] = cx; sHeadRest[1] = cy; sHeadRest[2] = cz; sHeadRestSet = true;
        printf("[VR] 6DoF rest captured at (%.2f, %.2f, %.2f)\n", cx, cy, cz);
    }
    float dcx = cx, dcy = cy, dcz = cz;
    if (sHeadRestSet) {
        dcx = sHeadRest[0] + (cx - sHeadRest[0]) * sHeadScale;
        dcy = sHeadRest[1] + (cy - sHeadRest[1]) * sHeadScale;
        dcz = sHeadRest[2] + (cz - sHeadRest[2]) * sHeadScale;
        sHeadOffsetM[0] = dcx - sHeadRest[0];
        sHeadOffsetM[1] = dcy - sHeadRest[1];
        sHeadOffsetM[2] = dcz - sHeadRest[2];
    }

    // Stereo comfort: keep the IPD offset (scaled by sStereoScale) around the damped center. The ramp
    // (sStereoRamp, driven in vr_begin_frame) eases separation in from flat for ~0.5s when eye rendering
    // resumes at level start, so a camera settling close to the ship can't briefly over-separate.
    float stereoEase = (float)sStereoRamp / (float)kStereoRampLen;
    if (stereoEase > 1.0f) stereoEase = 1.0f;
    float effStereo = sStereoScale * stereoEase;

    // Separation adapts to how big the world is set to LOOK. Disparity is separation over distance,
    // so shrinking the world (a higher units-per-metre scale) brings everything apparently closer
    // and a fixed IPD starts to cross the eyes - which is exactly what happens at the far end of
    // the World Scale slider. Scaling separation by (life-size / current scale) holds the felt
    // disparity constant across the whole range: comfortable at 100, still comfortable at 400, and
    // never more separation than a real IPD. This is why the depth stays balanced without the
    // player having to re-tune Stereo every time they change scale.
    {
        const float kLifeSizeScale = 100.0f;
        float modeScale = (sViewMode == 3) ? sDioramaWorldScale
                        : (sViewMode == 1) ? sFirstPersonScale
                        : sWorldScale;
        if (modeScale < 1.0f) {
            modeScale = 1.0f;
        }
        float adapt = kLifeSizeScale / modeScale;
        if (adapt > 1.0f) {
            adapt = 1.0f;   // never exaggerate past a true human IPD
        }
        if (adapt < 0.12f) {
            adapt = 0.12f;  // keep a little depth even on a tabletop
        }
        effStereo *= adapt;
    }
    pose.position.x = dcx + (pose.position.x - cx) * effStereo;
    pose.position.y = dcy + (pose.position.y - cy) * effStereo;
    pose.position.z = dcz + (pose.position.z - cz) * effStereo;
    sRenderPose[eye] = pose;

    // A: scale camera-space game units into meters, plus per-mode framing. The game's camera transform
    // rotation lives on the PROJECTION stack in Banjo (and the camera POSITION is baked per-object into the
    // modelview), so camera space here has the eye at the origin looking -Z with Banjo ahead.
    //  - Third Person (0): scale + eye-height. Eye sits at the chase cam (the classic view).
    //  - First Person (1): also push the eye forward (+Z) toward Banjo for an over-the-shoulder/head view.
    //  - Diorama   (3): shrink harder and place the tiny world at a fixed play-space spot in front of
    //                   you (-Z by sDioramaDist, raised by sDioramaHeight) -> a tabletop you can lean
    //                   around (V_head still tracks the head for 6DoF parallax).
    //  - Theater   (2): never reaches here (Engine.cpp renders it flat on the panel).
    float A[4][4] = { { 0 } };
    // Each in-world mode has its OWN scale so tuning one never changes another: Diorama (3) shrinks to a
    // tabletop, First Person (1) uses its own life-feel scale, Third Person (0) uses the global World Scale.
    float effScale = (sViewMode == 3) ? sDioramaWorldScale
                   : (sViewMode == 1) ? sFirstPersonScale
                   : sWorldScale;
    float invS = 1.0f / (effScale < 1.0f ? 1.0f : effScale);
    A[0][0] = invS; A[1][1] = invS; A[2][2] = invS; A[3][3] = 1.0f;
    if (sViewMode == 3) {           // Diorama: shrunk tabletop placed in front of you
        A[3][1] = sDioramaHeight;
        A[3][2] = -sDioramaDist;
    } else if (sViewMode == 1) {    // First Person: own eye height + push the eye toward Banjo
        A[3][1] = sFPEyeHeightCur;  // both eased, and both fade out while the GAME frames the shot
        A[3][2] = sFPForwardCur;    // eased toward the cockpit (see vr_begin_frame)
    } else if (sViewMode == 2) {    // Slot 2: no Banjo equivalent (the flying ports' cockpit view). The
        A[3][1] = sEyeHeight + sCockpitHeight; // in-game cycle skips it; the offsets stay wired so the
        A[3][2] = sCockpitForward;  // eye-matrix builder is identical across the ports in this family.
    } else {                        // Third Person: own eye height. The camera DISTANCE is applied game-side
        A[3][1] = sEyeHeight;       // in the game's own orbit distance (port_vrCamDistScale) so it moves
    }                               // camera HORIZONTALLY (closer/further), not up/down along the tilted view.

    // Clip planes: effectively INFINITE draw distance. The 2 km base far plane is ten times the span
    // of the biggest Banjo world at life size, so no in-game geometry ever hits it; the near plane sits
    // at 5 cm (headsets cannot focus closer, and the extra depth precision goes where geometry lives).
    float zn = 0.05f;
    float drawDistMul = CVarGetFloat("gVRDrawDistance", 4.0f); // max headroom by default - nothing in the game clips either way
    if (drawDistMul < 1.0f) drawDistMul = 1.0f;
    float zf = 2000.0f * drawDistMul;

    // Use the runtime's NATIVE per-eye FOV. Canted-display headsets (Quest 3 / Quest Pro) angle the two
    // panels inward, and the OpenXR runtime reports a strongly ASYMMETRIC per-eye FOV to match. We used to
    // force the FOV symmetric here; combined with the eye's (also canted) orientation, that points both
    // eyes' frustum centers inward -> the images converge -> CROSS-EYED. It only surfaced on Meta Horizon
    // Link's native runtime: SteamVR / Virtual Desktop hand the app a virtualized near-symmetric stereo
    // config (the cant lives in their compositor), so the old symmetrize was a harmless no-op there but
    // broke the native canted views. The stereo-comfort separation is applied to the eye POSITION above
    // (IPD scaling), which is independent of FOV/orientation and correct on any display geometry; infinity
    // still fuses because at infinity disparity comes from view direction (native fov + orientation), not
    // position. mat_proj_fov() already builds a correct off-center frustum from an asymmetric FOV.
    XrFovf fov = sViews[eye].fov;
    sRenderFov[eye] = fov;

    float V[4][4], P[4][4], AV[4][4];
    mat_view_from_pose(V, pose);
    mat_flip_apply(V); // first person flip cam: the whole eye view rotates with Banjo's somersault
    mat_proj_fov(P, fov, zn, zf);
    mat_mul(AV, A, V);
    mat_mul(sEyeVP[eye], AV, P);

    // ZERO-SEPARATION eye matrix, substituted for the game's SKY perspective load (sky.c registers
    // it, vr_sky_persp_match routes it). Banjo's sky is world models drawn AT the camera position,
    // so any inter-eye offset hands it parallax it cannot have - depth on a thing that must read as
    // infinity. Built exactly like the world eye matrix except the view position is the shared head
    // CENTRE (rotation and lean kept, IPD dropped) and the A matrix keeps only its SCALE - the
    // per-mode framing translations would shift a sky that should stay centred on the viewer. Same
    // per-eye orientation and fov as the submitted pose, so the compositor's reprojection cancels
    // cleanly and both eyes fuse it at infinity while the world keeps full stereo.
    {
        // Sky, third construction - the one that satisfies the ACTUAL invariant. The sky mesh is
        // SMALL and only reads as sky when the viewer sits at its exact centre; the flat game
        // guarantees that by drawing it AT the camera every frame. Neither previous attempt kept
        // that guarantee: scale-only (v1) let the head's room position walk the eye out of the
        // dome (the floating faceted UFO), and full-A (v2) fed the per-mode framing offsets in -
        // third person's eye height pushed the eye through the dome wall (black sky) and
        // Diorama's scale shrank the whole dome to centimetres (sky "all wrong"). First person
        // was fine both times because its offsets are zero, which is the tell. So: FIXED
        // life-size scale, NO translations, rotation-only view. The eye is pinned to the dome's
        // centre in every mode, both eyes fuse it at infinity (no position = no separation), and
        // the game's own projection MULs still turn it with the camera.
        XrPosef skyPose = pose;
        skyPose.position.x = skyPose.position.y = skyPose.position.z = 0.0f;
        float Asky[4][4] = { { 0 } };
        const float invLife = 1.0f / 100.0f;
        Asky[0][0] = invLife; Asky[1][1] = invLife; Asky[2][2] = invLife; Asky[3][3] = 1.0f;
        float Vsky[4][4], Psky[4][4], AVsky[4][4];
        mat_view_from_pose(Vsky, skyPose);
        // The sky takes the SAME flip rotation as the world. Leave it out and the world somersaults
        // while the sky hangs still, which reads as the sky sliding around the level - the sm64 port
        // hit exactly that and rotates its dome with the eye for the same reason.
        mat_flip_apply(Vsky);
        mat_proj_fov(Psky, fov, zn, zf);
        mat_mul(AVsky, Asky, Vsky);
        mat_mul(sSkyVP[eye], AVsky, Psky);
    }
    (void) sSkyCamValid; // camera rotation is never folded in - the game's own projection MULs carry it

    // ARWING-ANCHORED HUD (world space): map the game HUD's ortho output (NDC in [-1,1]) onto a quad placed
    // in the VR reference space (which the game's chase cam sits at the origin of, so the quad tracks the
    // ship), then project through the per-eye HEAD pose (V) - NOT the eye's own frame. Because V carries the
    // head rotation, turning your head does NOT drag the HUD (you look around it), and because V carries the
    // stereo eye offset the two eyes see it with real disparity -> it sits at a true depth of sHudDistM in
    // front of the ship instead of pasted flat on your face. This is the difference from a head-locked panel:
    // there the quad lived in the eye's own frame (M * P); here it lives in reference space and rides V * P.
    {
        float shAH = 0.0f, shAV = 0.0f;
        for (int k = 0; k < 2; k++) {
            shAH = fmaxf(shAH, fmaxf(fabsf(sViews[k].fov.angleLeft), fabsf(sViews[k].fov.angleRight)));
            shAV = fmaxf(shAV, fmaxf(fabsf(sViews[k].fov.angleUp),   fabsf(sViews[k].fov.angleDown)));
        }
        // Menu mode: the plane carries the pause menu / VR overlay, so it sits at the SCREEN
        // distance and width - decoupled from the gameplay HUD knobs entirely.
        float D  = sHudMenuMode ? ((sMenuDist > 0.3f) ? sMenuDist : 3.2f)
                                : ((sHudDistM > 0.05f) ? sHudDistM : 2.0f);
        // Quad half-extents so it fills sHudScale of the FOV at distance D. Sizing at the live D (not a fixed
        // reference) is correct here: the quad is a real object at D metres, so pushing it out genuinely makes
        // it recede. hudY drops it slightly so the radar/gauges sit below the aiming centre like a dashboard.
        float hw, hh, hudY;
        if (sHudMenuMode) {
            hw = sMenuSize * 0.5f;   // physical width from the SCREEN SIZE knob
            hh = hw * 0.75f;         // 4:3, the game frame's own shape
            hudY = 0.0f;             // menus sit centred, not dropped like the gameplay HUD
        } else {
            hw = D * tanf(shAH) * sHudScale;
            hh = D * tanf(shAV) * sHudScale;
            hudY = -D * tanf(shAV) * sHudScale * 0.15f;
        }
        float M[4][4] = { { 0 } };
        M[0][0] = hw; M[1][1] = hh;
        // NOT zero: Banjo's HUD is real geometry with authored Z (text -10, icons at their
        // own depths). Flattening it all to one depth made overlapping widget layers
        // z-fight - re-rolled every sub-frame as the plane follows the head - which is the
        // note-counter / life-bar / air-meter flicker. A few millimetres per ortho unit
        // keeps the authored ordering without visibly leaving the plane.
        M[2][2] = 0.002f;
        M[3][1] = hudY; M[3][2] = -D; M[3][3] = 1.0f; // quad centre D metres ahead in reference space

        // The plane uses the PER-EYE pose in menu mode too. The old head-centre build gave menu
        // text ZERO disparity - which is an INFINITY depth cue - on a panel 3.4 m away floating
        // over a live stereo world whose geometry carries real disparity. Two contradictory depth
        // cues in one glance is exactly what a player reports as "the pause text is cross-eyed".
        // Correct disparity AT the panel's distance (6.4 cm over 3.4 m is about a degree) fuses
        // easily and finally agrees with the world behind it.
        XrPosef hudPose = pose;
        float Vhud[4][4], Phud[4][4], MV[4][4];
        mat_view_from_pose(Vhud, hudPose);
        mat_proj_fov(Phud, sRenderFov[eye], 0.05f, 100.0f);
        mat_mul(MV, M, Vhud);
        mat_mul(sHudVP[eye], MV, Phud);
        // Flat-Z twin for 3D content drawn inside the HUD (a pause portrait's own perspective
        // load): its clip-Z through the ordering scale would read as metres of depth on the
        // plane and go cross-eyed. 2D HUD keeps the ordered plane above.
        M[2][2] = 0.0f;
        mat_mul(MV, M, Vhud);
        mat_mul(sHudVPFlat[eye], MV, Phud);
    }

    // Full-FOV head-locked plane for SCREEN-SPACE 2D drawn during the eye pass (course-open intro overlays,
    // and the 2D sky on tracks without the VR dome). Such 2D is otherwise emitted raw (plain ortho), identical
    // in both eyes, and then the per-eye ASYMMETRIC submission fov shoves the two copies apart -> double
    // vision. Mapping it through this plane instead places it head-locked, FILLING the FOV, with NO inter-eye
    // disparity (both eyes recover the same view direction), so it can't double - while the 3D world keeps its
    // real per-eye stereo. Same construction as the HUD plane, but full-size and not yaw-locked.
    {
        float shAH = fmaxf(fmaxf(fabsf(sViews[0].fov.angleLeft), fabsf(sViews[0].fov.angleRight)),
                           fmaxf(fabsf(sViews[1].fov.angleLeft), fabsf(sViews[1].fov.angleRight)));
        float shAV = fmaxf(fmaxf(fabsf(sViews[0].fov.angleUp),   fabsf(sViews[0].fov.angleDown)),
                           fmaxf(fabsf(sViews[1].fov.angleUp),   fabsf(sViews[1].fov.angleDown)));
        float D = 2.0f;
        float hw = D * tanf(shAH);   // quad fills the full (symmetric) FOV at distance D -> covers the view
        float hh = D * tanf(shAV);
        float M2[4][4] = { { 0 } };
        M2[0][0] = hw; M2[1][1] = hh; M2[2][2] = 0.002f; M2[3][2] = -D; M2[3][3] = 1.0f; // Z kept, same flicker law as the HUD plane
        float P2[4][4];
        mat_proj_fov(P2, sRenderFov[eye], 0.05f, 100.0f);
        mat_mul(sFull2DVP[eye], M2, P2);
    }
}

static void vr_sync_tunables(void); // defined next to vr_begin_frame; the harness needs it too
static void vr_flip_ease(void);     // likewise: the harness is its own per-frame heartbeat

// ---- headless verification seam ----------------------------------------------
// Build the same matrices vr_build_eye_matrix would for an identity head pose with a symmetric
// 50-degree half-fov, so RunVrEye is provable with no headset attached. Same A-matrix policy
// (per-mode world scale + eye height), same plane constructions - and the same PER-EYE separation
// math (half-IPD around the centre, scaled by the Stereo knob and the world-scale adaptive term;
// the ramp is left at full since there is no session to ease in from). Without the separation the
// harness cannot see stereo AT ALL: a zero-separation sky fix would measure 0.00 against 0.00,
// which is the round-13 blind-seam trap wearing a new hat. eye: 0 = left, 1 = right.
extern "C" void vr_debug_synth_matrices(int eye, float eyeVP[16], float skyVP[16], float hudVP[16],
                                        float hudFlatVP[16], float full2D[16]) {
    vr_sync_tunables(); // so eye dumps reflect the CVars, not the compile-time defaults
    if (eye == 0) {
        vr_flip_ease(); // the harness's per-frame heartbeat: same flip-cam ease as the live path
        static int sSynthLog = -1;
        if (sSynthLog < 0) {
            sSynthLog = (getenv("BK_VR_EYEDUMP") != NULL) ? 1 : 0;
        }
        if (sSynthLog == 1) {
            static float pf = -999.0f, pe = -999.0f, pr = -999.0f;
            if (sFPForwardCur != pf || sFPEyeHeightCur != pe || sFlipRad != pr) {
                printf("[VRDBG] synth A: mode=%d fpFwd=%.4f fpEye=%.4f flip=%.4f framing=%d\n", sViewMode,
                       sFPForwardCur, sFPEyeHeightCur, sFlipRad, sFpFramingOn ? 1 : 0);
                fflush(stdout);
                pf = sFPForwardCur; pe = sFPEyeHeightCur; pr = sFlipRad;
            }
        }
    }
    XrFovf fov;
    fov.angleLeft = -0.8727f;
    fov.angleRight = 0.8727f;
    fov.angleUp = 0.8727f;
    fov.angleDown = -0.8727f;
    // BK_VR_FOVTEST="l,r,u,d" (signed degrees): an ASYMMETRIC synthetic fov, mirrored per eye like a
    // canted-display headset reports. The one live geometry variable a symmetric synth never
    // exercises - added while hunting a live-only black sky.
    {
        static int sFovParsed = 0;
        static float fl, fr, fu, fd;
        if (sFovParsed == 0) {
            const char* t = getenv("BK_VR_FOVTEST");
            sFovParsed = (t != NULL && sscanf(t, "%f,%f,%f,%f", &fl, &fr, &fu, &fd) == 4) ? 1 : -1;
        }
        if (sFovParsed == 1) {
            const float d2r = 0.01745329252f;
            if (eye == 0) {
                fov.angleLeft = fl * d2r; fov.angleRight = fr * d2r;
            } else {
                fov.angleLeft = -fr * d2r; fov.angleRight = -fl * d2r; // mirrored cant
            }
            fov.angleUp = fu * d2r;
            fov.angleDown = fd * d2r;
        }
    }

    float effScale = (sViewMode == 3) ? sDioramaWorldScale : (sViewMode == 1) ? sFirstPersonScale : sWorldScale;
    float invS = 1.0f / (effScale < 1.0f ? 1.0f : effScale);
    float A[4][4] = { { 0 } };
    A[0][0] = invS; A[1][1] = invS; A[2][2] = invS; A[3][3] = 1.0f;
    // Mirror the LIVE per-mode offsets. The harness used to hardcode the third-person eye height,
    // which made it structurally incapable of testing the First Person and Diorama framing knobs -
    // a verification seam that cannot see a knob is how "it does nothing" goes unproven for rounds.
    if (sViewMode == 3) {
        A[3][1] = sDioramaHeight;
        A[3][2] = -sDioramaDist;
    } else if (sViewMode == 1) {
        A[3][1] = sFPEyeHeightCur; // the eased pair, exactly as the live builder uses them
        A[3][2] = sFPForwardCur;
    } else {
        A[3][1] = sEyeHeight;
    }

    // The live separation, mirrored: adapt = life-size / mode scale, clamped exactly like the
    // eye builder, times the Stereo knob, around a shared centre at the origin.
    float adapt = 100.0f / (effScale < 1.0f ? 1.0f : effScale);
    if (adapt > 1.0f) {
        adapt = 1.0f;
    }
    if (adapt < 0.12f) {
        adapt = 0.12f;
    }
    XrPosef pose;
    pose.orientation.x = pose.orientation.y = pose.orientation.z = 0.0f;
    pose.orientation.w = 1.0f;
    pose.position.x = 0.032f * sStereoScale * adapt * ((eye == 0) ? -1.0f : 1.0f);
    pose.position.y = 0.0f;
    pose.position.z = 0.0f;

    // Mirror the live far plane (same CVar, same default) - the synth path must stay a faithful
    // twin of the live construction or the harness tests a different frustum than players get.
    float zf = 2000.0f * CVarGetFloat("gVRDrawDistance", 4.0f);
    float V[4][4], P[4][4], AV[4][4];
    mat_view_from_pose(V, pose);
    mat_flip_apply(V); // faithful twin of the live build, flip cam included
    mat_proj_fov(P, fov, 0.05f, zf);
    float out[4][4];
    mat_mul(AV, A, V);
    mat_mul(out, AV, P);
    memcpy(eyeVP, out, sizeof(out));

    // Sky: fixed life-size scale, no translations, identity view - mirroring the live third
    // construction. By definition identical for both eyes, which IS the zero-separation contract
    // under test, and the viewer is pinned to the dome centre in every mode.
    {
        float Asky[4][4] = { { 0 } };
        const float invLife = 1.0f / 100.0f;
        Asky[0][0] = invLife; Asky[1][1] = invLife; Asky[2][2] = invLife; Asky[3][3] = 1.0f;
        float skyOut[4][4];
        mat_mul(skyOut, Asky, P);
        memcpy(skyVP, skyOut, sizeof(skyOut));
    }

    {
        float D = (sHudDistM > 0.05f) ? sHudDistM : 2.0f;
        float hw = D * tanf(0.8727f) * sHudScale;
        float hh = D * tanf(0.8727f) * sHudScale;
        float M[4][4] = { { 0 } };
        M[0][0] = hw; M[1][1] = hh;
        // NOT zero: Banjo's HUD is real geometry with authored Z (text -10, icons at their
        // own depths). Flattening it all to one depth made overlapping widget layers
        // z-fight - re-rolled every sub-frame as the plane follows the head - which is the
        // note-counter / life-bar / air-meter flicker. A few millimetres per ortho unit
        // keeps the authored ordering without visibly leaving the plane.
        M[2][2] = 0.002f;
        M[3][1] = -D * tanf(0.8727f) * sHudScale * 0.15f; M[3][2] = -D; M[3][3] = 1.0f;
        float Ph[4][4], MP[4][4];
        mat_proj_fov(Ph, fov, 0.05f, 100.0f);
        mat_mul(MP, M, Ph);
        memcpy(hudVP, MP, sizeof(MP));
        M[2][2] = 0.0f; // flat twin for perspective-in-HUD content, mirroring the live build
        mat_mul(MP, M, Ph);
        memcpy(hudFlatVP, MP, sizeof(MP));
    }
    {
        float D = 2.0f;
        float hw = D * tanf(0.8727f), hh = D * tanf(0.8727f);
        float M2[4][4] = { { 0 } };
        M2[0][0] = hw; M2[1][1] = hh; M2[2][2] = 0.002f; M2[3][2] = -D; M2[3][3] = 1.0f; // Z kept, same flicker law as the HUD plane
        float P2[4][4], MP2[4][4];
        mat_proj_fov(P2, fov, 0.05f, 100.0f);
        mat_mul(MP2, M2, P2);
        memcpy(full2D, MP2, sizeof(MP2));
    }
}

// Read an RGBA texture back and write a 24-bit BMP. Returns 1 on success. GL-thread only.
extern "C" int vr_debug_dump_texture(unsigned int glTextureId, int w, int h, const char* path) {
    if (glTextureId == 0 || w <= 0 || h <= 0) {
        return 0;
    }
    unsigned char* rgba = (unsigned char*)malloc((size_t)w * h * 4);
    if (!rgba) {
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, glTextureId);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);

    const int rowBytes = ((w * 3 + 3) / 4) * 4;
    const int dataSize = rowBytes * h;
    unsigned char hdr[54] = { 0 };
    hdr[0] = 'B'; hdr[1] = 'M';
    const int fileSize = 54 + dataSize;
    memcpy(hdr + 2, &fileSize, 4);
    const int dataOfs = 54;
    memcpy(hdr + 10, &dataOfs, 4);
    const int hdrSize = 40;
    memcpy(hdr + 14, &hdrSize, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;       // planes
    hdr[28] = 24;      // bpp
    memcpy(hdr + 34, &dataSize, 4);

    FILE* f = fopen(path, "wb");
    if (!f) {
        free(rgba);
        return 0;
    }
    fwrite(hdr, 1, 54, f);
    unsigned char* row = (unsigned char*)malloc(rowBytes);
    // The managed VR fb renders invert-Y (content upright in the texture), and BMP is bottom-up:
    // walking the texture top-down lands the image upright in the file.
    for (int y = h - 1; y >= 0; y--) {
        const unsigned char* src = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            row[x * 3 + 0] = src[x * 4 + 2];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 0];
        }
        memset(row + w * 3, 0, rowBytes - w * 3);
        fwrite(row, 1, rowBytes, f);
    }
    free(row);
    fclose(f);
    free(rgba);
    printf("[VR] eye dump written: %s (%dx%d)\n", path, w, h);
    return 1;
}

// ---- helpers ----------------------------------------------------------------
static bool xrok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    char buf[XR_MAX_RESULT_STRING_SIZE] = { 0 };
    if (sInstance != XR_NULL_HANDLE) xrResultToString(sInstance, r, buf);
    else snprintf(buf, sizeof buf, "%d", (int)r);
    printf("[VR] %s failed: %s\n", what, buf);
    return false;
}

// ---- Motion-controller input (OpenXR actions) --------------------------------
// One gameplay action set covering both thumbsticks,
// the face buttons, the menu button, stick clicks, triggers, grips, and a haptic output per hand.
// Suggested bindings cover Quest Touch (and the Quest 3 / Pro native Touch Plus profile), Index,
// Reverb G2, WMR wands, Vive wands, and the khr simple fallback. main.c merges this state into the
// first N64 pad so the controllers work everywhere a gamepad does.
static bool     sHasTouchPlus  = false;          // runtime knows the Quest 3 / Pro native profile
static bool     sHasHpMR       = false;          // runtime knows the HP Reverb G2 profile
static XrActionSet sActionSet  = XR_NULL_HANDLE;
static XrAction sActMove       = XR_NULL_HANDLE; // left thumbstick (vector2)
static XrAction sActCam        = XR_NULL_HANDLE; // right thumbstick (vector2)
static XrAction sActBtnA       = XR_NULL_HANDLE;
static XrAction sActBtnB       = XR_NULL_HANDLE;
static XrAction sActBtnX       = XR_NULL_HANDLE;
static XrAction sActBtnY       = XR_NULL_HANDLE;
static XrAction sActMenuBtn    = XR_NULL_HANDLE;
static XrAction sActLStick     = XR_NULL_HANDLE; // thumbstick clicks
static XrAction sActRStick     = XR_NULL_HANDLE;
static XrAction sActLTrigger   = XR_NULL_HANDLE; // analog 0..1, turned digital here with hysteresis
static XrAction sActRTrigger   = XR_NULL_HANDLE;
static XrAction sActLGrip      = XR_NULL_HANDLE;
static XrAction sActRGrip      = XR_NULL_HANDLE;
static XrAction sActHaptic     = XR_NULL_HANDLE; // vibration output, per-hand subactions
static XrPath   sHandPath[2]   = { XR_NULL_PATH, XR_NULL_PATH };
static bool     sInputAttached = false;          // action set attached to the session
static unsigned sCtrlButtons   = 0;              // VR_BTN_* mask, refreshed each xrSyncActions
static float    sCtrlStick[2][2] = {{ 0 }};      // [hand][x,y], +x right +y up
static float    sRumbleAmp     = 0.0f;           // armed rumble amplitude (0 = off)
static XrTime   sRumbleUntil   = 0;              // stop re-arming past this time

static XrAction vr_make_action(XrActionType type, const char* name, const char* localized, bool perHand) {
    XrActionCreateInfo aci = { XR_TYPE_ACTION_CREATE_INFO };
    aci.actionType = type;
    strncpy(aci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(aci.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (perHand) { aci.countSubactionPaths = 2; aci.subactionPaths = sHandPath; }
    XrAction a = XR_NULL_HANDLE;
    if (!xrok(xrCreateAction(sActionSet, &aci, &a), name)) { return XR_NULL_HANDLE; }
    return a;
}

struct VrBind { XrAction action; const char* path; };
static void vr_suggest_profile(const char* profilePath, const VrBind* binds, int count) {
    XrPath profile = XR_NULL_PATH;
    if (XR_FAILED(xrStringToPath(sInstance, profilePath, &profile))) { return; }
    XrActionSuggestedBinding sb[32];
    uint32_t n = 0;
    for (int i = 0; i < count && n < 32; i++) {
        if (binds[i].action == XR_NULL_HANDLE) { continue; }
        XrPath p = XR_NULL_PATH;
        if (XR_FAILED(xrStringToPath(sInstance, binds[i].path, &p))) { continue; }
        sb[n].action  = binds[i].action;
        sb[n].binding = p;
        n++;
    }
    if (n == 0) { return; }
    XrInteractionProfileSuggestedBinding spb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    spb.interactionProfile = profile;
    spb.suggestedBindings = sb;
    spb.countSuggestedBindings = n;
    // Not fatal if the runtime rejects a profile it doesn't know; the others still apply.
    xrok(xrSuggestInteractionProfileBindings(sInstance, &spb), profilePath);
}

// Create the action set, suggest the per-device bindings and attach to the session. Runs once at
// the end of vr_boot; any failure leaves VR fully functional, just without motion controllers.
static void vr_input_create(void) {
    sInputAttached = false;
    sCtrlButtons = 0;
    memset(sCtrlStick, 0, sizeof(sCtrlStick));
    xrStringToPath(sInstance, "/user/hand/left",  &sHandPath[0]);
    xrStringToPath(sInstance, "/user/hand/right", &sHandPath[1]);

    XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strncpy(asci.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(asci.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (!xrok(xrCreateActionSet(sInstance, &asci, &sActionSet), "xrCreateActionSet")) {
        sActionSet = XR_NULL_HANDLE;
        return;
    }

    sActMove     = vr_make_action(XR_ACTION_TYPE_VECTOR2F_INPUT,  "move",          "Steer",               false);
    sActCam      = vr_make_action(XR_ACTION_TYPE_VECTOR2F_INPUT,  "camera",        "Camera",              false);
    sActBtnA     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "button_a",      "A Button",            false);
    sActBtnB     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "button_b",      "B Button",            false);
    sActBtnX     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "button_x",      "X Button",            false);
    sActBtnY     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "button_y",      "Y Button",            false);
    sActMenuBtn  = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "menu",          "Pause",               false);
    sActLStick   = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "left_stick",    "Left Stick Click",    false);
    sActRStick   = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "right_stick",   "Right Stick Click",   false);
    sActLTrigger = vr_make_action(XR_ACTION_TYPE_FLOAT_INPUT,     "left_trigger",  "Left Trigger",        false);
    sActRTrigger = vr_make_action(XR_ACTION_TYPE_FLOAT_INPUT,     "right_trigger", "Right Trigger",       false);
    sActLGrip    = vr_make_action(XR_ACTION_TYPE_FLOAT_INPUT,     "left_grip",     "Left Grip",           false);
    sActRGrip    = vr_make_action(XR_ACTION_TYPE_FLOAT_INPUT,     "right_grip",    "Right Grip",          false);
    sActHaptic   = vr_make_action(XR_ACTION_TYPE_VIBRATION_OUTPUT,"rumble",        "Rumble",              true);

    // Quest Touch (the right controller's Oculus button is reserved by the system, so it isn't bound).
    const VrBind touch[] = {
        { sActMove,     "/user/hand/left/input/thumbstick" },
        { sActCam,      "/user/hand/right/input/thumbstick" },
        { sActBtnA,     "/user/hand/right/input/a/click" },
        { sActBtnB,     "/user/hand/right/input/b/click" },
        { sActBtnX,     "/user/hand/left/input/x/click" },
        { sActBtnY,     "/user/hand/left/input/y/click" },
        { sActMenuBtn,  "/user/hand/left/input/menu/click" },
        { sActLStick,   "/user/hand/left/input/thumbstick/click" },
        { sActRStick,   "/user/hand/right/input/thumbstick/click" },
        { sActLTrigger, "/user/hand/left/input/trigger/value" },
        { sActRTrigger, "/user/hand/right/input/trigger/value" },
        { sActLGrip,    "/user/hand/left/input/squeeze/value" },
        { sActRGrip,    "/user/hand/right/input/squeeze/value" },
        { sActHaptic,   "/user/hand/left/output/haptic" },
        { sActHaptic,   "/user/hand/right/output/haptic" },
    };
    vr_suggest_profile("/interaction_profiles/oculus/touch_controller", touch, (int)(sizeof(touch) / sizeof(touch[0])));

    // Valve Index: same layout, but A/B exist on both hands and there's no menu button.
    const VrBind index[] = {
        { sActMove,     "/user/hand/left/input/thumbstick" },
        { sActCam,      "/user/hand/right/input/thumbstick" },
        { sActBtnA,     "/user/hand/right/input/a/click" },
        { sActBtnB,     "/user/hand/right/input/b/click" },
        { sActBtnX,     "/user/hand/left/input/a/click" },
        { sActBtnY,     "/user/hand/left/input/b/click" },
        { sActLStick,   "/user/hand/left/input/thumbstick/click" },
        { sActRStick,   "/user/hand/right/input/thumbstick/click" },
        { sActLTrigger, "/user/hand/left/input/trigger/value" },
        { sActRTrigger, "/user/hand/right/input/trigger/value" },
        { sActLGrip,    "/user/hand/left/input/squeeze/value" },
        { sActRGrip,    "/user/hand/right/input/squeeze/value" },
        { sActHaptic,   "/user/hand/left/output/haptic" },
        { sActHaptic,   "/user/hand/right/output/haptic" },
    };
    vr_suggest_profile("/interaction_profiles/valve/index_controller", index, (int)(sizeof(index) / sizeof(index[0])));

    // Quest 3 / Quest Pro native profile: identical layout to Touch, so the same table applies.
    // Suggesting it explicitly matters: with only the older Touch bindings suggested, the runtime
    // auto-translates them onto Touch Plus and that translation can land buttons on the wrong hand.
    if (sHasTouchPlus) {
        vr_suggest_profile("/interaction_profiles/meta/touch_controller_plus", touch, (int)(sizeof(touch) / sizeof(touch[0])));
    }

    // HP Reverb G2: same control set as Touch (a/b right, x/y left, sticks, analog squeeze).
    if (sHasHpMR) {
        vr_suggest_profile("/interaction_profiles/hp/mixed_reality_controller", touch, (int)(sizeof(touch) / sizeof(touch[0])));
    }

    // Windows Mixed Reality wands: sticks and triggers as usual; no face buttons, so the trackpad
    // clicks stand in for A (right) and B (left). Squeeze is a click on these, not analog.
    const VrBind wmr[] = {
        { sActMove,     "/user/hand/left/input/thumbstick" },
        { sActCam,      "/user/hand/right/input/thumbstick" },
        { sActBtnA,     "/user/hand/right/input/trackpad/click" },
        { sActBtnB,     "/user/hand/left/input/trackpad/click" },
        { sActMenuBtn,  "/user/hand/left/input/menu/click" },
        { sActLStick,   "/user/hand/left/input/thumbstick/click" },
        { sActRStick,   "/user/hand/right/input/thumbstick/click" },
        { sActLTrigger, "/user/hand/left/input/trigger/value" },
        { sActRTrigger, "/user/hand/right/input/trigger/value" },
        { sActLGrip,    "/user/hand/left/input/squeeze/click" },
        { sActRGrip,    "/user/hand/right/input/squeeze/click" },
        { sActHaptic,   "/user/hand/left/output/haptic" },
        { sActHaptic,   "/user/hand/right/output/haptic" },
    };
    vr_suggest_profile("/interaction_profiles/microsoft/motion_controller", wmr, (int)(sizeof(wmr) / sizeof(wmr[0])));

    // Vive wands: no sticks at all, so the trackpads steer and drive the camera. Best-effort.
    const VrBind vive[] = {
        { sActMove,     "/user/hand/left/input/trackpad" },
        { sActCam,      "/user/hand/right/input/trackpad" },
        { sActBtnA,     "/user/hand/right/input/trackpad/click" },
        { sActBtnB,     "/user/hand/left/input/trackpad/click" },
        { sActMenuBtn,  "/user/hand/left/input/menu/click" },
        { sActLTrigger, "/user/hand/left/input/trigger/value" },
        { sActRTrigger, "/user/hand/right/input/trigger/value" },
        { sActLGrip,    "/user/hand/left/input/squeeze/click" },
        { sActRGrip,    "/user/hand/right/input/squeeze/click" },
        { sActHaptic,   "/user/hand/left/output/haptic" },
        { sActHaptic,   "/user/hand/right/output/haptic" },
    };
    vr_suggest_profile("/interaction_profiles/htc/vive_controller", vive, (int)(sizeof(vive) / sizeof(vive[0])));

    // Bare-minimum fallback profile every runtime understands (select + menu only).
    const VrBind simple[] = {
        { sActBtnA,    "/user/hand/right/input/select/click" },
        { sActBtnB,    "/user/hand/left/input/select/click" },
        { sActMenuBtn, "/user/hand/left/input/menu/click" },
        { sActHaptic,  "/user/hand/left/output/haptic" },
        { sActHaptic,  "/user/hand/right/output/haptic" },
    };
    vr_suggest_profile("/interaction_profiles/khr/simple_controller", simple, (int)(sizeof(simple) / sizeof(simple[0])));

    XrSessionActionSetsAttachInfo sai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    sai.countActionSets = 1;
    sai.actionSets = &sActionSet;
    if (!xrok(xrAttachSessionActionSets(sSession, &sai), "xrAttachSessionActionSets")) { return; }
    sInputAttached = true;
    printf("[VR] motion controllers ready (Touch / Touch Plus / Index / G2 / WMR / Vive profiles suggested).\n");
}

// Print which interaction profile the runtime actually bound for each hand. Fires whenever the
// runtime reports a profile change; this line is the first thing to check when a controller
// behaves oddly (wrong hand, dead buttons), since it shows what the runtime matched us to.
static void vr_log_active_profiles(void) {
    if (!sInputAttached || sSession == XR_NULL_HANDLE) { return; }
    static const char* handName[2] = { "left", "right" };
    for (int h = 0; h < 2; h++) {
        char buf[XR_MAX_PATH_LENGTH];
        snprintf(buf, sizeof(buf), "none (not bound)");
        XrInteractionProfileState ips = { XR_TYPE_INTERACTION_PROFILE_STATE };
        if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(sSession, sHandPath[h], &ips))
            && ips.interactionProfile != XR_NULL_PATH) {
            uint32_t len = 0;
            xrPathToString(sInstance, ips.interactionProfile, sizeof(buf), &len, buf);
        }
        printf("[VR] %s controller profile: %s\n", handName[h], buf);
    }
}

static bool vr_action_bool(XrAction a) {
    if (a == XR_NULL_HANDLE) { return false; }
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
    return XR_SUCCEEDED(xrGetActionStateBoolean(sSession, &gi, &st)) && st.isActive && st.currentState;
}

static float vr_action_float(XrAction a) {
    if (a == XR_NULL_HANDLE) { return 0.0f; }
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateFloat st = { XR_TYPE_ACTION_STATE_FLOAT };
    if (XR_FAILED(xrGetActionStateFloat(sSession, &gi, &st)) || !st.isActive) { return 0.0f; }
    return st.currentState;
}

static void vr_action_vec2(XrAction a, float out[2]) {
    out[0] = out[1] = 0.0f;
    if (a == XR_NULL_HANDLE) { return; }
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_FAILED(xrGetActionStateVector2f(sSession, &gi, &st)) || !st.isActive) { return; }
    out[0] = st.currentState.x;
    out[1] = st.currentState.y;
}

// Analog trigger/grip to a digital button with hysteresis: press past 60%, release under 40%,
// so a finger resting lightly on the trigger can't flicker the bound action.
static bool vr_analog_latch(float v, bool held) {
    return held ? (v > 0.4f) : (v > 0.6f);
}

// Pull fresh controller state from the runtime. Once per begun frame, from vr_begin_frame.
static void vr_input_sync(void) {
    if (!sInputAttached) { return; }
    XrActiveActionSet active = { sActionSet, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    // XR_SESSION_NOT_FOCUSED is a success code that means "no input for you" (headset off, runtime
    // menu up). Release everything so a button held at that moment can't stay stuck down, and
    // disarm rumble so the controllers don't keep buzzing in their holders.
    if (xrSyncActions(sSession, &si) != XR_SUCCESS) {
        sCtrlButtons = 0;
        memset(sCtrlStick, 0, sizeof(sCtrlStick));
        sRumbleAmp = 0.0f;
        return;
    }
    unsigned b = 0;
    if (vr_action_bool(sActBtnA))   { b |= VR_BTN_A; }
    if (vr_action_bool(sActBtnB))   { b |= VR_BTN_B; }
    if (vr_action_bool(sActBtnX))   { b |= VR_BTN_X; }
    if (vr_action_bool(sActBtnY))   { b |= VR_BTN_Y; }
    if (vr_action_bool(sActMenuBtn)){ b |= VR_BTN_MENU; }
    if (vr_action_bool(sActLStick)) { b |= VR_BTN_LSTICK; }
    if (vr_action_bool(sActRStick)) { b |= VR_BTN_RSTICK; }
    if (vr_analog_latch(vr_action_float(sActLTrigger), sCtrlButtons & VR_BTN_LTRIGGER)) { b |= VR_BTN_LTRIGGER; }
    if (vr_analog_latch(vr_action_float(sActRTrigger), sCtrlButtons & VR_BTN_RTRIGGER)) { b |= VR_BTN_RTRIGGER; }
    if (vr_analog_latch(vr_action_float(sActLGrip),    sCtrlButtons & VR_BTN_LGRIP))    { b |= VR_BTN_LGRIP; }
    if (vr_analog_latch(vr_action_float(sActRGrip),    sCtrlButtons & VR_BTN_RGRIP))    { b |= VR_BTN_RGRIP; }
    sCtrlButtons = b;
    vr_action_vec2(sActMove, sCtrlStick[0]);
    vr_action_vec2(sActCam,  sCtrlStick[1]);

    // Haptics: while armed, re-arm a SHORT burst every frame instead of ever submitting one long
    // vibration. Runtimes don't all honor stop requests promptly (or at all, over wireless) - a
    // long one-shot buzz that misses its stop can never be cancelled. Short bursts die on their
    // own right after the last re-arm, so a lost stop can't strand the motor.
    if (sRumbleAmp > 0.0f && sActHaptic != XR_NULL_HANDLE
        && sFrameState.predictedDisplayTime < sRumbleUntil) {
        XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };
        vib.duration  = 60000000; // 60 ms: outlasts one frame, dies fast once re-arming stops
        vib.frequency = XR_FREQUENCY_UNSPECIFIED;
        vib.amplitude = sRumbleAmp;
        for (int h = 0; h < 2; h++) {
            XrHapticActionInfo hai = { XR_TYPE_HAPTIC_ACTION_INFO };
            hai.action = sActHaptic;
            hai.subactionPath = sHandPath[h];
            xrApplyHapticFeedback(sSession, &hai, (const XrHapticBaseHeader*)&vib);
        }
    }
}

extern "C" bool vr_controllers_active(void) {
    return sInputAttached && sRunning && sState == XR_SESSION_STATE_FOCUSED;
}

extern "C" unsigned vr_controller_buttons(void) {
    return vr_controllers_active() ? sCtrlButtons : 0;
}

extern "C" void vr_controller_stick(int hand, float out[2]) {
    if (!vr_controllers_active() || hand < 0 || hand > 1) { out[0] = out[1] = 0.0f; return; }
    out[0] = sCtrlStick[hand][0];
    out[1] = sCtrlStick[hand][1];
}

// Arm the rumble. No vibration is submitted here: vr_input_sync re-arms a short burst each
// frame while armed, so a runtime that mishandles stop requests can't strand the motor on.
extern "C" void vr_controller_rumble(float strength, float seconds) {
    if (!vr_controllers_active() || sActHaptic == XR_NULL_HANDLE) { return; }
    if (strength < 0.0f) { strength = 0.0f; }
    if (strength > 1.0f) { strength = 1.0f; }
    sRumbleAmp   = strength;
    sRumbleUntil = sFrameState.predictedDisplayTime + (XrTime)(seconds * 1e9);
}

extern "C" void vr_controller_rumble_stop(void) {
    sRumbleAmp = 0.0f;
    sRumbleUntil = 0;
}

extern "C" float vr_head_yaw_rad(void) {
    if (!sRunning || !sViewsValid) return 0.0f;
    XrQuaternionf q = sViews[0].pose.orientation;
    float fx = -2.0f * (q.x * q.z + q.w * q.y);
    float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    return atan2f(fx, -fz);
}
extern "C" float vr_head_pitch_rad(void) {
    if (!sRunning || !sViewsValid) return 0.0f;
    XrQuaternionf q = sViews[0].pose.orientation;
    float fy = -2.0f * (q.y * q.z - q.w * q.x);
    if (fy >  1.0f) fy =  1.0f;
    if (fy < -1.0f) fy = -1.0f;
    return asinf(fy);
}
extern "C" int vr_display_refresh_hz(void) {
    if (sRunning && sFrameState.predictedDisplayPeriod > 0) {
        double hz = 1.0e9 / (double)sFrameState.predictedDisplayPeriod;
        if (hz >= 30.0 && hz <= 1000.0) return (int)(hz + 0.5);
    }
    return 90; // sane default until the first frame's display period is known
}

static int64_t vr_choose_swapchain_format(void) {
    uint32_t n = 0;
    if (!XR_SUCCEEDED(xrEnumerateSwapchainFormats(sSession, 0, &n, NULL)) || n == 0)
        return GL_SRGB8_ALPHA8;
    int64_t* fmts = (int64_t*)calloc(n, sizeof(int64_t));
    xrEnumerateSwapchainFormats(sSession, n, &n, fmts);
    const int64_t prefs[] = { GL_SRGB8_ALPHA8, GL_RGBA8 };
    int64_t chosen = fmts[0];
    bool found = false;
    for (uint32_t p = 0; p < 2 && !found; p++)
        for (uint32_t i = 0; i < n; i++)
            if (fmts[i] == prefs[p]) { chosen = prefs[p]; found = true; break; }
    free(fmts);
    printf("[VR] swapchain format: 0x%llx\n", (unsigned long long)chosen);
    return chosen;
}

// Lightweight startup probe: is a VR headset connected right now?
extern "C" bool vr_headset_present(void) {
    const char* exts[1] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    strncpy(ici.applicationInfo.applicationName, "Lighthouse", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0; // VirtualDesktopXR etc. are OpenXR 1.0
    XrInstance inst = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateInstance(&ici, &inst)) || inst == XR_NULL_HANDLE) return false;
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    XrResult r = xrGetSystem(inst, &sgi, &sys);
    xrDestroyInstance(inst);
    return XR_SUCCEEDED(r) && sys != XR_NULL_SYSTEM_ID;
}

static void vr_boot(void) {
    setvbuf(stdout, NULL, _IONBF, 0); // unbuffered so [VR] logs flush immediately (visible even if killed)
    printf("[VR] booting OpenXR...\n");
    // Probe the optional controller-profile extensions before creating the instance: the Quest 3 /
    // Pro native Touch Plus profile and the HP Reverb G2 profile only exist behind extensions, and
    // suggesting the native profile avoids the runtime's auto-translation (which can land buttons
    // on the wrong hand).
    sHasTouchPlus = false;
    sHasHpMR = false;
    {
        uint32_t ec = 0;
        if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, 0, &ec, NULL)) && ec > 0) {
            XrExtensionProperties* ep = (XrExtensionProperties*)calloc(ec, sizeof(XrExtensionProperties));
            for (uint32_t i = 0; i < ec; i++) ep[i].type = XR_TYPE_EXTENSION_PROPERTIES;
            if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, ec, &ec, ep))) {
                for (uint32_t i = 0; i < ec; i++) {
                    if (strcmp(ep[i].extensionName, "XR_META_touch_controller_plus")      == 0) sHasTouchPlus = true;
                    if (strcmp(ep[i].extensionName, "XR_EXT_hp_mixed_reality_controller") == 0) sHasHpMR = true;
                    if (strcmp(ep[i].extensionName, XR_FB_PASSTHROUGH_EXTENSION_NAME)      == 0) sHasPassthrough = true;
                }
            }
            free(ep);
        }
    }
    const char* exts[4];
    uint32_t nexts = 0;
    exts[nexts++] = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
    if (sHasTouchPlus)   exts[nexts++] = "XR_META_touch_controller_plus";
    if (sHasHpMR)        exts[nexts++] = "XR_EXT_hp_mixed_reality_controller";
    if (sHasPassthrough) exts[nexts++] = XR_FB_PASSTHROUGH_EXTENSION_NAME;
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    ici.enabledExtensionCount = nexts;
    ici.enabledExtensionNames = exts;
    strncpy(ici.applicationInfo.applicationName, "Lighthouse", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ici.applicationInfo.applicationVersion = 1;
    strncpy(ici.applicationInfo.engineName, "fast3d", XR_MAX_ENGINE_NAME_SIZE - 1);
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    if (!xrok(xrCreateInstance(&ici, &sInstance), "xrCreateInstance")) { vr_shutdown(); return; }

    XrInstanceProperties props = { XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(xrGetInstanceProperties(sInstance, &props)))
        printf("[VR] runtime: %s\n", props.runtimeName);

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xrok(xrGetSystem(sInstance, &sgi, &sSystemId), "xrGetSystem")) { vr_shutdown(); return; }

    // Enumerate environment blend modes. ALPHA_BLEND lets runtimes that DON'T expose XR_FB_passthrough
    // (e.g. some streaming runtimes like Virtual Desktop) still composite the real room behind our
    // transparent eye images - a cross-runtime Mixed Reality fallback.
    {
        uint32_t bc = 0;
        if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(sInstance, sSystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &bc, NULL)) && bc > 0) {
            XrEnvironmentBlendMode* bm = (XrEnvironmentBlendMode*)calloc(bc, sizeof(XrEnvironmentBlendMode));
            if (bm && XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(sInstance, sSystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, bc, &bc, bm))) {
                for (uint32_t i = 0; i < bc; i++) {
                    if (bm[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) sHasAlphaBlend = true;
                    printf("[VR] env blend mode offered: %d\n", (int)bm[i]);
                }
            }
            free(bm);
        }
        printf("[VR] MR ALPHA_BLEND fallback: %s\n", sHasAlphaBlend ? "AVAILABLE" : "not available");
    }

    if (!xrok(xrGetInstanceProcAddr(sInstance, "xrGetOpenGLGraphicsRequirementsKHR",
            (PFN_xrVoidFunction*)&pfnGetGLReq), "get xrGetOpenGLGraphicsRequirementsKHR")) { vr_shutdown(); return; }
    XrGraphicsRequirementsOpenGLKHR glReq = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
    if (!xrok(pfnGetGLReq(sInstance, sSystemId, &glReq), "xrGetOpenGLGraphicsRequirementsKHR")) { vr_shutdown(); return; }

    HDC   hdc  = wglGetCurrentDC();
    HGLRC glrc = wglGetCurrentContext();
    if (!hdc || !glrc) { printf("[VR] no current WGL context - is the OpenGL backend active? (VR needs it)\n"); vr_shutdown(); return; }

    XrGraphicsBindingOpenGLWin32KHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
    gb.hDC = hdc;
    gb.hGLRC = glrc;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &gb;
    sci.systemId = sSystemId;
    if (!xrok(xrCreateSession(sInstance, &sci, &sSession), "xrCreateSession")) { vr_shutdown(); return; }

    XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    if (!xrok(xrCreateReferenceSpace(sSession, &rsci, &sLocalSpace), "xrCreateReferenceSpace")) { vr_shutdown(); return; }

    // XR_FB_passthrough: resolve the entry points and create a full-screen reconstruction layer (started),
    // so the room can be composited behind the game when MR is toggled on. Non-fatal - any failure just
    // leaves sHasPassthrough false and the game runs as normal opaque VR.
    if (sHasPassthrough) {
        bool ptok =
            xrok(xrGetInstanceProcAddr(sInstance, "xrCreatePassthroughFB",       (PFN_xrVoidFunction*)&pfnCreatePassthrough),       "get xrCreatePassthroughFB")
         && xrok(xrGetInstanceProcAddr(sInstance, "xrDestroyPassthroughFB",      (PFN_xrVoidFunction*)&pfnDestroyPassthrough),      "get xrDestroyPassthroughFB")
         && xrok(xrGetInstanceProcAddr(sInstance, "xrCreatePassthroughLayerFB",  (PFN_xrVoidFunction*)&pfnCreatePassthroughLayer),  "get xrCreatePassthroughLayerFB")
         && xrok(xrGetInstanceProcAddr(sInstance, "xrDestroyPassthroughLayerFB", (PFN_xrVoidFunction*)&pfnDestroyPassthroughLayer), "get xrDestroyPassthroughLayerFB")
         && xrok(xrGetInstanceProcAddr(sInstance, "xrPassthroughStartFB",        (PFN_xrVoidFunction*)&pfnPassthroughStart),        "get xrPassthroughStartFB")
         && xrok(xrGetInstanceProcAddr(sInstance, "xrPassthroughLayerResumeFB",  (PFN_xrVoidFunction*)&pfnPassthroughLayerResume),  "get xrPassthroughLayerResumeFB")
         && xrok(xrGetInstanceProcAddr(sInstance, "xrPassthroughLayerPauseFB",   (PFN_xrVoidFunction*)&pfnPassthroughLayerPause),   "get xrPassthroughLayerPauseFB");
        if (ptok) {
            XrPassthroughCreateInfoFB pci = { XR_TYPE_PASSTHROUGH_CREATE_INFO_FB };
            if (xrok(pfnCreatePassthrough(sSession, &pci, &sPassthrough), "xrCreatePassthroughFB")) {
                XrPassthroughLayerCreateInfoFB lci = { XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB };
                lci.passthrough = sPassthrough;
                lci.purpose     = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
                lci.flags       = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
                if (xrok(pfnCreatePassthroughLayer(sSession, &lci, &sPtLayer), "xrCreatePassthroughLayerFB")) {
                    xrok(pfnPassthroughStart(sPassthrough), "xrPassthroughStartFB");
                    printf("[VR] passthrough available (Mixed Reality ready).\n");
                }
            }
        }
        if (sPtLayer == XR_NULL_HANDLE) sHasPassthrough = false; // setup failed -> normal opaque VR
    }

    XrViewConfigurationType vct = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    uint32_t n = 0;
    if (!xrok(xrEnumerateViewConfigurationViews(sInstance, sSystemId, vct, 0, &n, NULL), "enum view count")) { vr_shutdown(); return; }
    if (n > 2) n = 2;
    for (uint32_t i = 0; i < n; i++) sViewConfigs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    if (!xrok(xrEnumerateViewConfigurationViews(sInstance, sSystemId, vct, n, &n, sViewConfigs), "enum views")) { vr_shutdown(); return; }
    sViewCount = n;
    printf("[VR] %u eyes, %ux%u per eye recommended\n", n,
        sViewConfigs[0].recommendedImageRectWidth, sViewConfigs[0].recommendedImageRectHeight);

    int64_t fmt = vr_choose_swapchain_format();
    for (uint32_t e = 0; e < sViewCount; e++) {
        XrSwapchainCreateInfo scci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        scci.format = fmt;
        scci.sampleCount = 1;
        scci.faceCount = 1; scci.arraySize = 1; scci.mipCount = 1;
        // Try the recommended size, then fall back if GPU memory is tight (so VR still boots).
        // Start below the runtime's (often supersampled) recommended size: rendering the full scene
        // twice per eye per interpolation step is heavy, and falling behind the headset frame budget
        // slows the game tick + underruns the audio (crackling). 0.8x trades a little sharpness for the
        // headroom to keep pace; falls back further under GPU-memory pressure.
        static const float scEyeScales[] = { 0.8f, 0.7f, 0.6f, 0.5f, 0.4f };
        XrResult scRes = XR_ERROR_RUNTIME_FAILURE;
        for (int s = 0; s < (int)(sizeof(scEyeScales) / sizeof(scEyeScales[0])); s++) {
            scci.width  = (uint32_t)(sViewConfigs[e].recommendedImageRectWidth  * scEyeScales[s]);
            scci.height = (uint32_t)(sViewConfigs[e].recommendedImageRectHeight * scEyeScales[s]);
            scRes = xrCreateSwapchain(sSession, &scci, &sEye[e].handle);
            if (XR_SUCCEEDED(scRes)) {
                if (scEyeScales[s] < 1.0f)
                    printf("[VR] eye %u swapchain fell back to %ux%u\n", e, scci.width, scci.height);
                break;
            }
        }
        if (!XR_SUCCEEDED(scRes)) {
            printf("[VR] xrCreateSwapchain failed at every size - VR disabled.\n");
            vr_shutdown(); return;
        }
        sEye[e].w = scci.width;
        sEye[e].h = scci.height;

        uint32_t imgN = 0;
        xrEnumerateSwapchainImages(sEye[e].handle, 0, &imgN, NULL);
        sEye[e].images = (XrSwapchainImageOpenGLKHR*)calloc(imgN, sizeof(XrSwapchainImageOpenGLKHR));
        for (uint32_t i = 0; i < imgN; i++) sEye[e].images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        xrEnumerateSwapchainImages(sEye[e].handle, imgN, &imgN, (XrSwapchainImageBaseHeader*)sEye[e].images);
        sEye[e].imgCount = imgN;
    }

    glGenFramebuffers(1, &sEyeFbo);
    glGenFramebuffers(1, &sBlitReadFbo);
    glGenRenderbuffers(1, &sEyeDepthRB);
    glBindRenderbuffer(GL_RENDERBUFFER, sEyeDepthRB);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)sEye[0].w, (GLsizei)sEye[0].h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    XrReferenceSpaceCreateInfo vrci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    vrci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vrci.poseInReferenceSpace.orientation.w = 1.0f;
    xrok(xrCreateReferenceSpace(sSession, &vrci, &sViewSpace), "xrCreateReferenceSpace(VIEW)");

    XrSwapchainCreateInfo oci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    oci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    oci.format = fmt;
    oci.sampleCount = 1;
    oci.width  = sHud.w = (uint32_t)sOverlayW;
    oci.height = sHud.h = (uint32_t)sOverlayH;
    oci.faceCount = 1; oci.arraySize = 1; oci.mipCount = 1;
    if (xrok(xrCreateSwapchain(sSession, &oci, &sHud.handle), "xrCreateSwapchain(hud)")) {
        uint32_t hn = 0;
        xrEnumerateSwapchainImages(sHud.handle, 0, &hn, NULL);
        sHud.images = (XrSwapchainImageOpenGLKHR*)calloc(hn, sizeof(XrSwapchainImageOpenGLKHR));
        for (uint32_t i = 0; i < hn; i++) sHud.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        xrEnumerateSwapchainImages(sHud.handle, hn, &hn, (XrSwapchainImageBaseHeader*)sHud.images);
        sHud.imgCount = hn;
        glGenFramebuffers(1, &sOverlayFbo);
        glGenRenderbuffers(1, &sOverlayDepthRB);
        glBindRenderbuffer(GL_RENDERBUFFER, sOverlayDepthRB);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)sOverlayW, (GLsizei)sOverlayH);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }

    // Motion controllers: action set + per-device suggested bindings. Failure here is non-fatal
    // (VR keeps rendering, the gamepad keeps working).
    vr_input_create();

    printf("[VR] OpenXR ready; waiting for session to start.\n");
}

static void vr_poll_events(void) {
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(sInstance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged* e = (const XrEventDataSessionStateChanged*)&ev;
            sState = e->state;
            if (e->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo sbi = { XR_TYPE_SESSION_BEGIN_INFO };
                sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (xrok(xrBeginSession(sSession, &sbi), "xrBeginSession")) {
                    sRunning = true;
                    printf("[VR] session running.\n");
                }
            } else if (e->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(sSession);
                sRunning = false;
                printf("[VR] session stopped.\n");
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            vr_log_active_profiles(); // which controller profile each hand actually bound to
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // The user recentered (or the runtime re-established tracking): LOCAL space is about to move,
            // so every pose captured in the old space is stale. Without this the theater/menu panel keeps
            // its old yaw - it can end up flat-out BEHIND the player after a recenter - and the 6DoF rest
            // pose damps against a position that no longer means "seated here".
            sPanelAnchorValid = false;
            sHeadRestSet = false;
            sHeadWarmup = 0;
            // The runtime's new LOCAL space replaces any app-side correction (ours was captured in
            // the old space and would double-rotate the fresh one).
            sRecenterSet = false;
            sRcQy = 0.0f;
            sRcQw = 1.0f;
            // Re-capture OUR correction on the new space - but NOT this instant. Virtual Desktop
            // fires this event on tracking blips as well as deliberate recenters, and an
            // immediate re-capture mid-head-motion snapped the world to wherever the head
            // happened to be ("the game uncenters sometimes"). The capture is deferred until the
            // head has been near-stationary for a beat: a deliberate recenter (head held still)
            // lands almost instantly, a mid-motion blip waits for calm.
            sSpaceChangePending = true;
            printf("[VR] reference space recentered; anchors reset, origin re-capture pending.\n");
        }
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

// --- in-game VR menu accessors ----------------------------------------------
extern "C" float vr_get_world_scale(void)  { return sWorldScale; }
extern "C" void  vr_set_world_scale(float v){ sWorldScale = (v < 1.0f) ? 1.0f : v; }
extern "C" float vr_get_stereo(void)        { return sStereoScale; }
extern "C" void  vr_set_stereo(float v)     { sStereoScale = (v < 0.0f) ? 0.0f : v; }
extern "C" float vr_get_head_scale(void)    { return sHeadScale; }
extern "C" void  vr_set_head_scale(float v) { sHeadScale = (v < 0.0f) ? 0.0f : (v > 1.5f ? 1.5f : v); }
extern "C" float vr_get_eye_height(void)    { return sEyeHeight; }
extern "C" void  vr_set_eye_height(float v) { sEyeHeight = v; }
extern "C" float vr_get_menu_dist(void)     { return sMenuDist; }
extern "C" void  vr_set_menu_dist(float v)  { sMenuDist = v; }
extern "C" float vr_get_menu_size(void)     { return sMenuSize; }
extern "C" void  vr_set_menu_size(float v)  { sMenuSize = v; }
extern "C" float vr_get_hud_scale(void)     { return sHudScale; }
extern "C" void  vr_set_hud_scale(float v)  { sHudScale = (v < 0.1f) ? 0.1f : (v > 1.5f ? 1.5f : v); }
extern "C" float vr_get_hud_dist(void)      { return sHudDistM; }
extern "C" void  vr_set_hud_dist(float v)   { sHudDistM = (v < 0.3f) ? 0.3f : (v > 20.0f ? 20.0f : v); }
// Read the live CVar (not the cached sViewMode) so Engine's per-frame stereo/Theater gate matches the
// eye-matrix build on the SAME frame - avoids a one-frame mismatched render when switching modes. Clamped.
extern "C" int   vr_get_view_mode(void)     { int m = CVarGetInteger("gVRViewMode", sViewMode); return (m < 0) ? 0 : (m > 4 ? 4 : m); }
// Setter also writes the CVar, since vr_get_view_mode reads the CVar first - lets the right-stick
// mode-cycle (sys_joybus) and the menus all share one source of truth.
extern "C" void  vr_set_view_mode(int m)    { sViewMode = (m < 0) ? 0 : (m > 4 ? 4 : m); CVarSetInteger("gVRViewMode", sViewMode); }
// How far (GAME units) to push the camera back along the horizontal eye->at direction in Third Person
// VR. The game's camera lookAt build moves the eye by this so distance reads as closer/further
// (horizontal), not up/down. Converts the meters slider via the Third Person world scale. 0 unless VR
// is live AND in Third Person, so flat play and the other modes are untouched.
// How far (GAME units) the First Person eye currently sits ahead of the chase camera along the
// camera->ship ray. 0 outside First Person.
extern "C" float vr_fp_forward_game_units(void) {
    if (!sRunning || vr_get_view_mode() != VR_VIEW_FIRST_PERSON) return 0.0f;
    return sFPForwardCur * (sFirstPersonScale < 1.0f ? 1.0f : sFirstPersonScale);
}
// --- VR fog (the Fast3D interpreter consults these on every fog command) ------------------------
// Stock N64 fog keys off the PROJECTED depth z/w, which the substituted per-eye projection breaks:
// the factor saturates and drowns the life-size modes. The replacement factor is linear in clip-space
// w instead - in a perspective projection w IS the eye-space view depth, so it is projection-
// independent. Theater renders flat and Diorama's shrunk world pushes the stock range past anything
// visible; both already look right, so they keep stock fog.
// How the current pass should treat fog: 0 = no fog, 1 = world-distance fog, 2 = stock fog untouched.
extern "C" int vr_fog_mode(void) {
    // Only the life-size stereo modes (Third / First) substitute a projection that breaks the stock
    // formula; Diorama and Theater keep stock fog.
    if (!sRunning || sViewMode > VR_VIEW_UNUSED_2) {
        return 2;
    }
    return sFogMode;
}
// Coefficients for the world-distance factor: fog(0..255) = clamp(clip_w * mul + off). The eye matrix
// bakes 1/scale into clip space, so a world point d life-size meters out lands at
// clip_w = d * kLifeSizeScale / effScale - the near/far sliders convert through the active mode's scale.
extern "C" void vr_fog_linear_coeffs(float* mul, float* off) {
    const float kLifeSizeScale = 100.0f; // the default units-per-meter mapping the sliders assume
    // Same scale the eye matrix uses per mode (Third Person uses the global World Scale).
    float effScale = (sViewMode == VR_VIEW_FIRST_PERSON) ? sFirstPersonScale : sWorldScale;
    if (effScale < 1.0f) effScale = 1.0f;
    float nearW = sFogNearM * kLifeSizeScale / effScale;
    float farW  = sFogFarM * kLifeSizeScale / effScale;
    if (farW < nearW + 0.01f) farW = nearW + 0.01f;
    *mul = 255.0f / (farW - nearW);
    *off = -nearW * (*mul);
}
extern "C" void  vr_reset_defaults(void) {
    // Deliberately does NOT touch the view mode: a reset should restore numbers, not teleport the
    // player into a different way of seeing the world.
    sWorldScale = 100.0f; sStereoScale = 1.0f; sHeadScale = 0.1f; sEyeHeight = -1.09f;
    sMenuDist = 3.4f; sMenuSize = 3.6f;
    sHeadRestSet = false; sHeadWarmup = 0; sPanelAnchorValid = false;
    printf("[VR] reset to defaults.\n");
}

// Ask for an in-game recenter; it lands on the next located frame (see vr_begin_frame).
extern "C" void vr_recenter(void) {
    sRecenterRequest = true;
}

// FLIP CAM target angle, fed once per GAME tick from the state machine's animation clock. Only the
// TARGET lands here; the eased value follows it per HEADSET frame (vr_flip_ease, called from
// vr_begin_frame). The game ticks far slower than the headset renders, so easing at tick rate would
// leave the somersault stair-stepped in the rendered frames - the ease has to run where the frames
// are. (Same treatment, same reason, as the sm64 port's flip cam.)
extern "C" void vr_set_flip_angle(float radians) {
    sFlipTargetRad = radians;
}

// Is VR First Person actually DRIVING the camera this tick? When the game takes it for a scripted
// shot the answer is no, and the First Person framing offsets fade out so the authored composition
// is presented as composed (head tracking still applies - you can look around inside the shot).
extern "C" void vr_set_fp_framing(int on) {
    sFpFramingOn = (on != 0);
}

// Per headset frame: follow the target, and snap when close so the view settles dead level between
// flips instead of creeping.
static void vr_flip_ease(void) {
    const float kTwoPi = 6.28318530718f;
    float target = sFlipTargetRad;
    float rate = 0.22f;
    if (target == 0.0f && sFlipRad != 0.0f) {
        // THE MOVE ENDED. Do not unwind: land on the nearest WHOLE revolution and carry on the way
        // the body was already turning. A flip that lands with 40 degrees still to run finishes
        // those 40; one barely begun returns to level. Unwinding instead rolled the view backwards
        // down through the floor for a few frames on touchdown - reported as the camera sinking
        // into the ground on landing. Faster too, because the move is over and the view should be
        // level by the time the player has landed, not a beat later.
        target = kTwoPi * floorf(sFlipRad / kTwoPi + 0.5f);
        rate = 0.35f;
    }
    sFlipRad += (target - sFlipRad) * rate;
    if (fabsf(target - sFlipRad) < 0.0008f) {
        // A completed revolution IS upright, so fold it back to zero: same rotation, and the next
        // flip starts from a clean slate instead of accumulating turns.
        sFlipRad = (sFlipTargetRad == 0.0f) ? 0.0f : target;
    }
}

extern "C" void vr_set_hud_menu_mode(int on) {
    sHudMenuMode = on ? 1 : 0;
}

// Pull every live tunable from its CVar. Called at the top of vr_begin_frame AND by the headless
// harness: the sync used to live inline in vr_begin_frame, which never runs without an OpenXR
// session, so eye dumps silently rendered compile-time defaults and could not test a single
// framing knob. A verification seam that cannot see the knob it is meant to check is worse than
// no seam - it reports 'no difference' and looks like proof.
static void vr_sync_tunables(void) {
    // Live VR tunables from the in-game Enhancements > VR menu (CVars). Read each frame so slider
    // changes apply instantly. Default = the current value, so an unset CVar keeps the built-in default.
    sWorldScale  = CVarGetFloat("gVRWorldScale", sWorldScale);
    sStereoScale = CVarGetFloat("gVRStereo",     sStereoScale);
    sEyeHeight   = CVarGetFloat("gVREyeHeight",   sEyeHeight);
    sHeadScale   = CVarGetFloat("gVRHeadScale",   sHeadScale); // physical lean amount (immersive cam)
    sHudScale    = CVarGetFloat("gVRHudScale",    sHudScale);
    sHudDistM    = CVarGetFloat("gVRHudDist",     sHudDistM);
    sMenuDist    = CVarGetFloat("gVRMenuDist",    sMenuDist);
    sMenuSize    = CVarGetFloat("gVRMenuSize",    sMenuSize);
    {
        const int prevMode = sViewMode;
        sViewMode = CVarGetInteger("gVRViewMode", sViewMode);
        if (sViewMode < 0) sViewMode = 0; else if (sViewMode > 4) sViewMode = 4;
        if (sViewMode != prevMode) {
            // A mode switch changes world scale, eye offsets and separation all at once. Landing
            // that in one frame is the "glitch": the compositor reprojects a frame whose geometry
            // jumped, which reads as a tear or a double image. Re-running the stereo ramp eases the
            // separation in from flat over ~0.5 s, and parking the eased First Person push stops it
            // sweeping forward from wherever the previous mode left it.
            sStereoRamp = 0;
            sFPForwardCur = sFirstPersonForward;
        }
    }
    // Mixed Reality is a DIORAMA-ONLY experience (the shrunk tabletop in your room). While MR is enabled and
    // the runtime supports it, force Diorama so the other view modes can't fight it - the dropdown / D-pad are
    // effectively locked to Diorama until MR is switched off. Write the CVar back so the menu reflects it.
    if ((sHasPassthrough || sHasAlphaBlend) && CVarGetInteger("gVRPassthrough", 0) != 0) {
        sViewMode = VR_VIEW_DIORAMA;
        if (CVarGetInteger("gVRViewMode", 0) != VR_VIEW_DIORAMA) {
            CVarSetInteger("gVRViewMode", VR_VIEW_DIORAMA);
        }
    }
    sFirstPersonForward   = CVarGetFloat("gVRFirstPersonFwd",       sFirstPersonForward);
    // First Person sits at the cockpit from the very first frame. sFPForwardCur is what the eye matrix
    // uses (A[3][2]); while eyes aren't rendering (menus, loading) it PARKS at its target so gameplay
    // opens with the eye already in the cockpit on frame 1 - a visible forward sweep under headset
    // reprojection reads as a doubled / cross-eyed image for a second. The ease only runs across live
    // slider changes mid-flight.
    sFirstPersonEyeHeight = CVarGetFloat("gVRFirstPersonEyeHeight", sFirstPersonEyeHeight);
    {
        // While the GAME owns the camera - a Bottles conversation, a switch reveal, any scripted
        // shot - BOTH First Person framing offsets fade to zero, because that shot was composed by
        // the game and our offsets are measured from a camera that is supposed to be parked in
        // Banjo's head. Riding a metre of forward push and an eye-height drop on top of an authored
        // close-up is what put the view inside walls during dialogue. Faded rather than switched:
        // a metre of instant dolly would read as a jump cut in a headset.
        const float fpTarget = sFpFramingOn ? sFirstPersonForward : 0.0f;
        const float ehTarget = sFpFramingOn ? sFirstPersonEyeHeight : 0.0f;
        if (sEyesSubmitted < 2) { // parked (no eyes last frame): snap, so gameplay opens in the cockpit
            sFPForwardCur = fpTarget;
            sFPEyeHeightCur = ehTarget;
        } else {
            sFPForwardCur += (fpTarget - sFPForwardCur) * 0.12f;      // ease across live changes
            sFPEyeHeightCur += (ehTarget - sFPEyeHeightCur) * 0.12f;
        }
    }
    sFirstPersonScale     = CVarGetFloat("gVRFirstPersonScale",     sFirstPersonScale);
    sThirdPersonDist      = CVarGetFloat("gVRThirdPersonDist",      sThirdPersonDist);
    sCockpitForward       = CVarGetFloat("gVRCockpitFwd",           sCockpitForward);
    sCockpitHeight        = CVarGetFloat("gVRCockpitHeight",        sCockpitHeight);
    sDioramaWorldScale    = CVarGetFloat("gVRDioramaWorldScale",    sDioramaWorldScale);
    sDioramaDist        = CVarGetFloat("gVRDioramaDist",      sDioramaDist);
    sDioramaHeight      = CVarGetFloat("gVRDioramaHeight",    sDioramaHeight);
    sMenuOpacity        = CVarGetFloat("gVRMenuOpacity",      sMenuOpacity);
    sImGuiOpacity       = CVarGetFloat("gVRImGuiOpacity",     sImGuiOpacity);
    sFogMode            = CVarGetInteger("gVRFogMode",        sFogMode);
    if (sFogMode < 0) sFogMode = 0; else if (sFogMode > 2) sFogMode = 2;
    sFogNearM           = CVarGetFloat("gVRFogNear",          sFogNearM);
    sFogFarM            = CVarGetFloat("gVRFogFar",           sFogFarM);
}

extern "C" void vr_begin_frame(void) {
    if (!sRequested) return;
    if (!sBootTried) { sBootTried = true; vr_boot(); }
    if (sSession == XR_NULL_HANDLE) return;

    vr_poll_events();

    // SAVED SETTINGS MIGRATION. A shipped default only reaches players who have never written the
    // key - everyone else keeps whatever their config captured, forever, which is why "it defaults
    // on" and "it is on for me" keep disagreeing. Version the settings and migrate once.
    //   1 = Immersive Camera is ON. It is the switch the whole first-person feel hangs off, and it
    //       now also enables the view following Banjo's body, so a config that captured it while it
    //       was optional leaves the player with a first person that does none of it.
    //   2 = Invert look folded into the port's own camera invert. The VR-only gVRFpInvertX turned
    //       first-person yaw and nothing else, and there was never a vertical partner for it. One
    //       pair of settings now inverts every camera, so the old key is carried across and
    //       retired - a player who had it ticked would otherwise quietly lose their inverted look.
    // Each step is gated on the version the config ARRIVED with, so a later step can never re-run
    // an earlier one over a choice the player has since made.
    {
        static bool sMigrated = false;
        if (!sMigrated) {
            sMigrated = true;
            const int kSettingsVer = 2;
            const int have = CVarGetInteger("gVRSettingsVer", 0);
            if (have < 1) {
                CVarSetInteger("gVRFpImmersive", 1);
            }
            if (have < 2) {
                if (CVarGetInteger("gVRFpInvertX", 0) != 0) {
                    CVarSetInteger("gEnhancements.Camera.InvertX", 1);
                }
                CVarClear("gVRFpInvertX");
            }
            if (have < kSettingsVer) {
                CVarSetInteger("gVRSettingsVer", kSettingsVer);
                printf("[VR] settings migrated from v%d to v%d.\n", have, kSettingsVer);
                fflush(stdout);
            }
        }
    }

    vr_sync_tunables();

    vr_flip_ease(); // first person flip cam: follow the game's flip angle at the headset's frame rate

    // Mixed Reality toggle. Resume/pause the passthrough layer to match (it is created RUNNING, but MR
    // defaults OFF, so the first frame pauses it). Only meaningful when the runtime supports passthrough.
    sPassthroughOn = (sHasPassthrough || sHasAlphaBlend) && (CVarGetInteger("gVRPassthrough", 0) != 0);
    {
        static bool sPtRunning = true; // matches XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB
        if (sHasPassthrough && sPassthroughOn != sPtRunning) {
            if (sPassthroughOn) { if (pfnPassthroughLayerResume) pfnPassthroughLayerResume(sPtLayer); }
            else                { if (pfnPassthroughLayerPause)  pfnPassthroughLayerPause(sPtLayer); }
            sPtRunning = sPassthroughOn;
        }
    }

    // Stereo ease-in / hold. sEyesSubmitted still holds the PREVIOUS frame's eye count here (reset below).
    // When both eyes rendered, ramp up to full. When they didn't, DECAY instead of hard-resetting to flat:
    // the headset intermittently reports shouldRender=false (reprojection, proximity, focus blips) for a
    // frame or a handful, and a hard reset turned each of those into a full ~0.5s collapse to mono - a
    // stereo "breathing" where the separation flipped 0.5<->0.0. A gentle decay rides through brief drops
    // while a genuinely long gap (menu/panel, level load) still eases down to 0 so it re-eases in cleanly.
    if (sEyesSubmitted >= 2) {
        if (sStereoRamp < kStereoRampLen) sStereoRamp++;
    } else if (sStereoRamp > 3) {
        sStereoRamp -= 3;
    } else {
        sStereoRamp = 0;
    }

    sViewsValid = false;
    sPoseTracked = false;
    sHudReady = false;
    sPanelMode = false;
    sEyesSubmitted = 0;
    if (!sRunning) return;

    XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
    sFrameState.type = XR_TYPE_FRAME_STATE;
    sFrameState.next = NULL;
    if (!xrok(xrWaitFrame(sSession, &fwi, &sFrameState), "xrWaitFrame")) return;

    // Say the headset's rate out loud once (and again if the runtime changes it mid-session, which
    // Quest does when you switch refresh in its settings): this is the number the whole frame pace
    // is built on, and "why am I capped at 60" is answered by one log line instead of a slider hunt.
    {
        static int sPrevHz = 0;
        const int hz = vr_display_refresh_hz();
        if (hz != sPrevHz) {
            printf("[VR] headset refresh %d Hz - pacing the game to match.\n", hz);
            fflush(stdout);
            sPrevHz = hz;
        }
    }

    XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
    if (!xrok(xrBeginFrame(sSession, &fbi), "xrBeginFrame")) return;
    sFrameBegun = true;

    // Fresh motion-controller state for this frame (and the haptic re-arm) - after xrWaitFrame so
    // the rumble timing reads the new predicted display time.
    vr_input_sync();

    if (sFrameState.shouldRender) {
        XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = sFrameState.predictedDisplayTime;
        vli.space = sLocalSpace;
        XrViewState vs = { XR_TYPE_VIEW_STATE };
        sViews[0].type = XR_TYPE_VIEW;
        sViews[1].type = XR_TYPE_VIEW;
        uint32_t got = 0;
        if (XR_SUCCEEDED(xrLocateViews(sSession, &vli, &vs, 2, &got, sViews)) && got == 2) {
            sViewsValid = true;
            sPoseTracked = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0
                        && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)    != 0;
            // In-game recenter: capture from the RAW runtime pose (before any correction), so the
            // stored remap is absolute and repeated recenters can't compound. Yaw twist = the
            // normalized (y, w) of the head quaternion, the same extraction the panel anchor uses.
            // First FOCUSED frame = the headset is actually on a head: recenter automatically, so
            // the STARTING view is correct on every runtime and every seating position without the
            // player knowing a recenter exists (gamepad players have no trigger combo). Manual
            // recenter (both triggers / the menu row) still re-zeros any time.
            if (!sAutoRecentered && sState == XR_SESSION_STATE_FOCUSED) {
                sAutoRecentered = true;
                sRecenterRequest = true;
                printf("[VR] first focus: auto-recenter.\n");
            }
            // A runtime space change re-captures only once the head is calm (~0.3 s under
            // 15 cm/s): deliberate recenters land immediately, tracking blips wait for stillness
            // instead of snapping the world mid-motion.
            if (sSpaceChangePending) {
                static float sCalmPrev[3] = { 0, 0, 0 };
                static int sCalmFrames = 0;
                const float hx = 0.5f * (sViews[0].pose.position.x + sViews[1].pose.position.x);
                const float hy = 0.5f * (sViews[0].pose.position.y + sViews[1].pose.position.y);
                const float hz = 0.5f * (sViews[0].pose.position.z + sViews[1].pose.position.z);
                const float dx = hx - sCalmPrev[0], dy = hy - sCalmPrev[1], dz = hz - sCalmPrev[2];
                const float kMaxStep = 0.15f / 72.0f; // 15 cm/s at a conservative frame rate
                if (dx * dx + dy * dy + dz * dz < kMaxStep * kMaxStep) {
                    sCalmFrames++;
                } else {
                    sCalmFrames = 0;
                }
                sCalmPrev[0] = hx; sCalmPrev[1] = hy; sCalmPrev[2] = hz;
                if (sCalmFrames >= 20) {
                    sSpaceChangePending = false;
                    sCalmFrames = 0;
                    sRecenterRequest = true;
                }
            }
            if (sRecenterRequest) {
                sRecenterRequest = false;
                float ty = sViews[0].pose.orientation.y, tw = sViews[0].pose.orientation.w;
                float tn = sqrtf(ty * ty + tw * tw);
                if (tn > 1e-6f) {
                    sRcQy = -ty / tn; // conjugate: undo the captured yaw
                    sRcQw = tw / tn;
                    sRcPos[0] = 0.5f * (sViews[0].pose.position.x + sViews[1].pose.position.x);
                    sRcPos[1] = 0.5f * (sViews[0].pose.position.y + sViews[1].pose.position.y);
                    sRcPos[2] = 0.5f * (sViews[0].pose.position.z + sViews[1].pose.position.z);
                    sRecenterSet = true;
                    sPanelAnchorValid = false; // panel re-parks in front of the new forward
                    sHeadRestSet = false;      // 6DoF rest re-captures against the new origin
                    sHeadWarmup = 0;
                    vr_controller_rumble(0.5f, 0.1f); // the confirmation tick in the hands
                    printf("[VR] in-game recenter: view yaw + position + height re-zeroed.\n");
                }
            }
            // Apply the active correction to both view poses; everything downstream (eye matrices,
            // HUD plane, panel anchor) reads sViews, so one remap keeps them all consistent.
            if (sRecenterSet) {
                const float c = 1.0f - 2.0f * sRcQy * sRcQy; // rotation-about-Y terms for quat (0,qy,0,qw)
                const float s = 2.0f * sRcQy * sRcQw;
                for (int i = 0; i < 2; i++) {
                    XrPosef* p = &sViews[i].pose;
                    // position: translate to the captured origin (height included - the neutral head
                    // becomes y=0, which is what the per-mode eye-height offsets are tuned against;
                    // crouching and standing still track as deltas on top), then yaw about it.
                    float px = p->position.x - sRcPos[0];
                    float pz = p->position.z - sRcPos[2];
                    p->position.y -= sRcPos[1];
                    p->position.x = c * px + s * pz;
                    p->position.z = -s * px + c * pz;
                    // orientation: q' = qc * q, with qc = (0, sRcQy, 0, sRcQw)
                    float qx = p->orientation.x, qy = p->orientation.y, qz = p->orientation.z,
                          qw = p->orientation.w;
                    p->orientation.x = sRcQw * qx + sRcQy * qz;
                    p->orientation.y = sRcQw * qy + sRcQy * qw;
                    p->orientation.z = sRcQw * qz - sRcQy * qx;
                    p->orientation.w = sRcQw * qw - sRcQy * qy;
                }
            }
            vr_build_eye_matrix(0);
            vr_build_eye_matrix(1);
        }
    }
}

extern "C" bool vr_begin_eye(int eye) {
    if (!sRunning || !sFrameBegun || !sFrameState.shouldRender || !sViewsValid) return false;
    if (eye < 0 || eye >= (int)sViewCount || sEyeFbo == 0) return false;

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XR_SUCCEEDED(xrAcquireSwapchainImage(sEye[eye].handle, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    swi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sEye[eye].handle, &swi);
    sEyeImgIdx[eye] = idx;

    glBindFramebuffer(GL_FRAMEBUFFER, sEyeFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sEye[eye].images[idx].image, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sEyeDepthRB);
    glViewport(0, 0, (GLsizei)sEye[eye].w, (GLsizei)sEye[eye].h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, vr_passthrough_active() ? 0.0f : 1.0f); // MR: transparent void -> room shows
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return true;
}

extern "C" void vr_end_eye(int eye) {
    if (eye < 0 || eye >= (int)sViewCount) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(sEye[eye].handle, &ri);

    memset(&sProjViews[eye], 0, sizeof(sProjViews[eye]));
    sProjViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
    sProjViews[eye].pose = sRenderPose[eye];
    sProjViews[eye].fov  = sRenderFov[eye];
    sProjViews[eye].subImage.swapchain = sEye[eye].handle;
    sProjViews[eye].subImage.imageRect.offset.x = 0;
    sProjViews[eye].subImage.imageRect.offset.y = 0;
    sProjViews[eye].subImage.imageRect.extent.width  = (int32_t)sEye[eye].w;
    sProjViews[eye].subImage.imageRect.extent.height = (int32_t)sEye[eye].h;
}

extern "C" bool vr_begin_overlay(bool sky) {
    (void)sky;
    if (!sRunning || !sFrameBegun || !sFrameState.shouldRender) return false;
    if (sOverlayFbo == 0 || sHud.handle == XR_NULL_HANDLE) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XR_SUCCEEDED(xrAcquireSwapchainImage(sHud.handle, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    swi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sHud.handle, &swi);
    sHudImgIdx = idx;

    glBindFramebuffer(GL_FRAMEBUFFER, sOverlayFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sHud.images[idx].image, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sOverlayDepthRB);
    glViewport(0, 0, sOverlayW, sOverlayH);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return true;
}

extern "C" void vr_end_overlay(bool sky) {
    (void)sky;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(sHud.handle, &ri);
    sHudReady = true;
}

extern "C" void vr_set_panel_mode(bool on) { sPanelMode = on; }
extern "C" bool vr_begin_panel(void) { sPanelMode = true; return vr_begin_overlay(false); }
extern "C" void vr_end_panel(void) { vr_end_overlay(false); }

// Blit a rendered texture (from the interpreter's managed VR fb, sized srcW x srcH) into the given
// swapchain image, flipped vertically. The managed fb is rendered invert-Y=true like mGameFb; the
// desktop present (CopyFramebuffer) flips it to the screen because screen invertY=false, so the
// OpenXR swapchain (same bottom-left GL convention as the screen) needs the same flip - otherwise the
// image is upside down. The flip is whole-framebuffer, so it applies uniformly to both the stereo eyes
// and the 2D menu panel. sEyeFbo is reused as a scratch DRAW fbo for the swapchain image.
static bool vr_blit_into(VrSwapchain* sc, unsigned int glTex, int srcW, int srcH, uint32_t* outIdx) {
    if (sc->handle == XR_NULL_HANDLE) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XR_SUCCEEDED(xrAcquireSwapchainImage(sc->handle, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    swi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sc->handle, &swi);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, sBlitReadFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)glTex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sEyeFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc->images[idx].image, 0);
    glDisable(GL_SCISSOR_TEST);
    GLenum filt = (srcW == (int)sc->w && srcH == (int)sc->h) ? GL_NEAREST : GL_LINEAR;
    // Flip vertically: src bottom->dst top, src top->dst bottom (dst Y0/Y1 swapped).
    glBlitFramebuffer(0, 0, srcW, srcH, 0, (int)sc->h, (int)sc->w, 0, GL_COLOR_BUFFER_BIT, filt);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(sc->handle, &ri);
    *outIdx = idx;
    return true;
}

extern "C" bool vr_submit_eye_texture(int eye, unsigned int glTex, int w, int h) {
    if (!sRunning || !sFrameBegun || !sFrameState.shouldRender || !sViewsValid) return false;
    if (eye < 0 || eye >= (int)sViewCount || sEyeFbo == 0 || glTex == 0) return false;
    uint32_t idx = 0;
    if (!vr_blit_into(&sEye[eye], glTex, w, h, &idx)) return false;
    sEyeImgIdx[eye] = idx;
    sEyesSubmitted++;
    memset(&sProjViews[eye], 0, sizeof(sProjViews[eye]));
    sProjViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
    sProjViews[eye].pose = sRenderPose[eye];
    sProjViews[eye].fov  = sRenderFov[eye];
    sProjViews[eye].subImage.swapchain = sEye[eye].handle;
    sProjViews[eye].subImage.imageRect.offset.x = 0;
    sProjViews[eye].subImage.imageRect.offset.y = 0;
    sProjViews[eye].subImage.imageRect.extent.width  = (int32_t)sEye[eye].w;
    sProjViews[eye].subImage.imageRect.extent.height = (int32_t)sEye[eye].h;
    return true;
}

extern "C" bool vr_submit_panel_texture(unsigned int glTex, int w, int h) {
    if (!sRunning || !sFrameBegun || !sFrameState.shouldRender) return false;
    if (sEyeFbo == 0 || glTex == 0) return false;
    uint32_t idx = 0;
    if (!vr_blit_into(&sHud, glTex, w, h, &idx)) return false;
    sHudImgIdx = idx;
    sHudReady = true;
    sPanelMode = true;
    return true;
}

// Present the desktop default framebuffer (window back buffer, with the ImGui menu drawn into it) on
// the head-locked panel, so the in-game menu is usable inside the headset. srcW/srcH = window pixel
// size (the region of fb0 to copy). fb0 is invert-Y=false (unlike the managed fb), so a STRAIGHT blit
// (no vertical flip) lands upright in the swapchain.
extern "C" bool vr_present_desktop_panel(int srcW, int srcH) {
    if (!sRunning || !sFrameBegun || !sFrameState.shouldRender) return false;
    if (sEyeFbo == 0 || sHud.handle == XR_NULL_HANDLE) return false;
    if (srcW <= 0 || srcH <= 0) { srcW = sOverlayW; srcH = sOverlayH; }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XR_SUCCEEDED(xrAcquireSwapchainImage(sHud.handle, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    swi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sHud.handle, &swi);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sEyeFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sHud.images[idx].image, 0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, srcW, srcH, 0, 0, (int)sHud.w, (int)sHud.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(sHud.handle, &ri);
    sHudImgIdx = idx;
    sHudReady = true;
    sPanelMode = true;
    return true;
}

// ---- VR menu: render ImGui into a dedicated, cleared offscreen FBO, then present that STABLE texture on
// the head-locked panel. vr_present_desktop_panel read GL_BACK of the double-buffered window, which
// alternates 2-frame-stale content -> flicker. A private FBO removes the double-buffer dependency. ----
static GLuint sMenuFbo = 0, sMenuTex = 0, sMenuDepth = 0;
static int    sMenuW = 0, sMenuH = 0;

// Bind + clear the menu FBO and leave it bound so the caller's ImGui render lands inside it.
extern "C" void vr_menu_render_begin(int w, int h) {
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (sMenuFbo == 0) {
        glGenFramebuffers(1, &sMenuFbo);
        glGenTextures(1, &sMenuTex);
        glGenRenderbuffers(1, &sMenuDepth);
    }
    if (w != sMenuW || h != sMenuH) {
        glBindTexture(GL_TEXTURE_2D, sMenuTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindRenderbuffer(GL_RENDERBUFFER, sMenuDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, sMenuFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sMenuTex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, sMenuDepth);
        sMenuW = w;
        sMenuH = h;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, sMenuFbo);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, vr_passthrough_active() ? 0.0f : 1.0f); // MR: transparent void -> room shows
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// Make the ImGui/Enhancements menu panel see-through by writing a uniform alpha (gVRImGuiOpacity) into the menu texture, then
// the head-locked quad layer (which blends on source alpha when a game is rendered behind it - see vr_submit)
// shows the race through the menu. Call AFTER the menu's ImGui has rendered into the FBO and BEFORE the panel
// blit. Alpha-only clear leaves the RGB (the menu image) untouched, so the desktop mirror still shows the
// full-color menu. No-op at full opacity. Floored at 0.3 so the menu can never become unreadable.
extern "C" void vr_menu_apply_opacity(void) {
    if (sMenuFbo == 0) return;
    // The ImGui/Enhancements (settings) menu has its OWN opacity (gVRImGuiOpacity), DECOUPLED from the
    // game's VR pause-menu/HUD panel opacity (gVRMenuOpacity) - so making the pause panel see-through no
    // longer dims this settings menu. Defaults opaque (1.0). Fresh CVar read (cache refreshes after this).
    // Default just barely see-through (0.95): 0.85 read as "too transparent" for a menu whose
    // whole job is small readable text - the game showing through is a hint here, not a feature.
    float op = CVarGetFloat("gVRImGuiOpacity", 0.95f);
    if (op < 0.3f) op = 0.3f;
    if (op > 0.999f) return; // fully opaque: keep the FBO's cleared alpha (1.0) as-is
    glBindFramebuffer(GL_FRAMEBUFFER, sMenuFbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, op);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

// Mirror the menu texture onto the desktop window (fb0). Call ONCE per frame AFTER the eye loop - the eye
// renders call mRapi->StartFrame() which churns fb0, so mirroring per-step (interleaved with the eyes) made
// the flatscreen flicker. One mirror right before the engine's SwapBuffers keeps the desktop steady.
extern "C" void vr_menu_mirror_desktop(int w, int h) {
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (sMenuTex == 0) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sBlitReadFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sMenuTex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, sMenuW, sMenuH, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

// Mirror the rendered VR game frame (the interpreter's managed fb texture - last eye, or the panel render)
// onto the desktop window (fb0) when the MENU IS CLOSED. Without this, the flat window's double buffer just
// swaps between stale back-buffers each frame (the game renders into the OpenXR swapchains, never fb0) and
// flickers. Call ONCE per frame AFTER the eye loop, right before the engine's SwapBuffers - mirroring
// per-step fights the eye renders churning fb0 (same reason as vr_menu_mirror_desktop). The managed fb is
// invert-Y=true (bottom-left origin) like the eyes, so flip vertically into fb0 (top-left) - identical flip
// to vr_blit_into. srcW/srcH = the managed fb's rendered size (eye or panel); dstW/dstH = window pixels.
extern "C" void vr_mirror_game_desktop(unsigned int glTex, int srcW, int srcH, int dstW, int dstH,
                                       int cropForEye) {
    if (glTex == 0 || srcW <= 0 || srcH <= 0) return;
    if (dstW <= 0) dstW = 1;
    if (dstH <= 0) dstH = 1;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sBlitReadFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)glTex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    int sx0 = 0, sy0 = 0, sx1 = srcW, sy1 = srcH;
    if (cropForEye) {
        // The eye texture covers a 100+ degree asymmetric frustum: blitted whole it reads as a
        // fisheye and the interesting middle is tiny. Crop the central region at the window's
        // aspect so every stereo mode gives a watchable flat picture (the periphery only exists
        // for the headset anyway).
        const float kKeepV = 0.62f; // central fraction of the eye's height to show
        float ch = srcH * kKeepV;
        float cw = ch * ((float)dstW / (float)dstH);
        if (cw > (float)srcW) {
            cw = (float)srcW;
            ch = cw * ((float)dstH / (float)dstW);
        }
        sx0 = (int)((srcW - cw) * 0.5f);
        sy0 = (int)((srcH - ch) * 0.5f);
        sx1 = sx0 + (int)cw;
        sy1 = sy0 + (int)ch;
    }
    // Flip vertically (src bottom -> dst top): dst Y runs dstH..0, like vr_blit_into's swapchain blit.
    glBlitFramebuffer(sx0, sy0, sx1, sy1, 0, dstH, dstW, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

// ImGui has rendered into the menu FBO. Present the stable texture on the head-locked panel (per step). The
// desktop mirror is done separately, once after the loop (vr_menu_mirror_desktop).
extern "C" void vr_menu_render_present(int w, int h) {
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    glBindFramebuffer(GL_FRAMEBUFFER, 0); // unbind the menu FBO (gui->EndDraw left it bound)

    if (!sRunning || !sFrameBegun || !sFrameState.shouldRender) return;
    if (sEyeFbo == 0 || sHud.handle == XR_NULL_HANDLE || sMenuTex == 0) return;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XR_SUCCEEDED(xrAcquireSwapchainImage(sHud.handle, &ai, &idx))) return;
    XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    swi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sHud.handle, &swi);
    // Straight blit (no vertical flip): sMenuTex is top-left origin like fb0, matching vr_present_desktop_panel.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sBlitReadFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sMenuTex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sEyeFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sHud.images[idx].image, 0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, w, h, 0, 0, (int)sHud.w, (int)sHud.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(sHud.handle, &ri);
    sHudImgIdx = idx;
    sHudReady = true;
    sPanelMode = true;
}

extern "C" void vr_submit(void) {
    if (!sRunning || !sFrameBegun) return;
    sFrameBegun = false;

    if (!sPanelMode) sPanelAnchorValid = false;

    XrCompositionLayerProjection proj    = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerQuad       hudQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    const XrCompositionLayerFlags kBlend = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
                                         | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    const XrCompositionLayerBaseHeader* layers[3];
    uint32_t layerCount = 0;

    // Mixed Reality: submit the passthrough layer at the BOTTOM so the real room shows wherever the eye
    // images are transparent (alpha 0); the projection + HUD layers composite on top. ptLayer must stay
    // in scope until xrEndFrame (it does - declared here in vr_submit).
    XrCompositionLayerPassthroughFB ptLayer = { XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB };
    if (vr_passthrough_active() && sPtLayer != XR_NULL_HANDLE) {
        ptLayer.layerHandle = sPtLayer;
        ptLayer.flags = 0;
        ptLayer.space = XR_NULL_HANDLE;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&ptLayer;
    }

    // The 3D game projection layer is shown whenever both eyes were rendered this frame - INCLUDING when
    // the menu is open over a live race, so the world stays visible behind the menu (it no longer blacks
    // out the game).
    const bool haveEyes = (sFrameState.shouldRender && sViewsValid && sViewCount == 2 && sEyesSubmitted == 2);
    if (haveEyes) {
        // MR: blend the 3D projection layer on its own texture alpha so the transparent (alpha 0) background
        // reveals the passthrough layer beneath while opaque (alpha 1) geometry still occludes the room.
        // Opaque (flags 0) otherwise - the eye image fully covers the view in normal VR.
        proj.layerFlags = vr_passthrough_active() ? kBlend : 0;
        proj.space = sLocalSpace;
        proj.viewCount = 2;
        proj.views = sProjViews;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&proj;
    }

    if (sHudReady) {
        if (sPanelMode) {
            // Menu / non-gameplay flat panel: world-locked quad anchored in front of you. Drawn ON TOP of the
            // game projection layer when one exists, so the menu floats over the live world. When there IS a
            // game behind it (haveEyes), the panel content is the SETTINGS (ImGui) menu, whose alpha
            // vr_menu_apply_opacity wrote from gVRImGuiOpacity - so THAT knob gates the blend. The old gate
            // read the PAUSE-PANEL knob (gVRMenuOpacity): set that one to 1.0 and the settings slider went
            // dead ("the ImGui opacity slider should work in VR" - it was gated on the wrong variable).
            // Without eyes (Theater / title) the panel stays an opaque virtual screen: the game content's
            // own alpha channel is not ours and blending on it would punch holes in the screen.
            hudQuad.layerFlags = (haveEyes && sImGuiOpacity < 0.999f) ? kBlend : 0;
            hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            hudQuad.subImage.swapchain = sHud.handle;
            hudQuad.subImage.imageRect.offset.x = 0;
            hudQuad.subImage.imageRect.offset.y = 0;
            hudQuad.subImage.imageRect.extent.width  = (int32_t)sHud.w;
            hudQuad.subImage.imageRect.extent.height = (int32_t)sHud.h;
            hudQuad.size.width  = sMenuSize;
            hudQuad.size.height = sMenuSize * (float)sHud.h / (float)sHud.w;
            if (sLocalSpace != XR_NULL_HANDLE && sViewsValid) {
                float cqy = sViews[0].pose.orientation.y, cqw = sViews[0].pose.orientation.w;
                float cn = sqrtf(cqy * cqy + cqw * cqw);
                if (cn < 1e-6f) { cqy = 0.0f; cqw = 1.0f; cn = 1.0f; }
                cqy /= cn; cqw /= cn;
                float hfx = -2.0f * cqw * cqy, hfz = -(1.0f - 2.0f * cqy * cqy);
                float afx = -2.0f * sPanelAnchorQw * sPanelAnchorQy, afz = -(1.0f - 2.0f * sPanelAnchorQy * sPanelAnchorQy);
                float dot = hfx * afx + hfz * afz;
                (void) dot;
                // World-lock the panel where it opened so you can look around it. The enclosing branch
                // already requires valid located views, so DON'T gate the anchor on sPoseTracked (some
                // runtimes never report POSITION_VALID, which left the anchor invalid -> re-anchoring every
                // frame -> the panel stayed glued to the face). Re-anchors only on (re)open.
                bool reAnchor = !sPanelAnchorValid;
                if (reAnchor) {
                    sPanelAnchorPos[0] = 0.5f * (sViews[0].pose.position.x + sViews[1].pose.position.x);
                    sPanelAnchorPos[1] = 0.5f * (sViews[0].pose.position.y + sViews[1].pose.position.y);
                    sPanelAnchorPos[2] = 0.5f * (sViews[0].pose.position.z + sViews[1].pose.position.z);
                    sPanelAnchorQy = cqy; sPanelAnchorQw = cqw;
                    sPanelAnchorValid = true;
                }
                float qy = sPanelAnchorQy, qw = sPanelAnchorQw;
                float fwdx = -2.0f * qw * qy;
                float fwdz = -(1.0f - 2.0f * qy * qy);
                hudQuad.space = sLocalSpace;
                hudQuad.pose.orientation.x = 0.0f; hudQuad.pose.orientation.y = qy;
                hudQuad.pose.orientation.z = 0.0f; hudQuad.pose.orientation.w = qw;
                hudQuad.pose.position.x = sPanelAnchorPos[0] + sMenuDist * fwdx;
                hudQuad.pose.position.y = sPanelAnchorPos[1];
                hudQuad.pose.position.z = sPanelAnchorPos[2] + sMenuDist * fwdz;
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&hudQuad;
            }
        } else if (sViewSpace != XR_NULL_HANDLE) {
            // Head-locked alpha-blended HUD quad (legacy overlay path).
            hudQuad.layerFlags = kBlend;
            hudQuad.space = sViewSpace;
            hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            hudQuad.subImage.swapchain = sHud.handle;
            hudQuad.subImage.imageRect.extent.width  = (int32_t)sHud.w;
            hudQuad.subImage.imageRect.extent.height = (int32_t)sHud.h;
            hudQuad.pose.orientation.w = 1.0f;
            hudQuad.pose.position.z = -2.0f;
            hudQuad.size.width  = 2.4f;
            hudQuad.size.height = 2.4f * (float)sOverlayH / (float)sOverlayW;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&hudQuad;
        }
    }

    XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
    fei.displayTime = sFrameState.predictedDisplayTime;
    // MR via ALPHA_BLEND (runtimes without XR_FB_passthrough): composite our transparent eye images over the
    // runtime's real-world view. When XR_FB_passthrough IS used, the passthrough layer provides the room and
    // the blend stays OPAQUE.
    fei.environmentBlendMode = (vr_passthrough_active() && !sHasPassthrough && sHasAlphaBlend)
                             ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
                             : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layerCount;
    fei.layers = layerCount ? layers : NULL;
    xrok(xrEndFrame(sSession, &fei), "xrEndFrame");
}

extern "C" void vr_shutdown(void) {
    // END the session before destroying anything. A hard xrDestroySession on a live session - with
    // an xrBeginFrame still unmatched - leaves VirtualDesktopXR compositing the last frame forever
    // and parks the process in a kernel wait: the window closes, the headset view sticks, and the
    // zombie survives taskkill until the streamer restarts. Submit the missing empty end-frame,
    // ask the runtime to exit, drain events until STOPPING, end the session properly - every step
    // bounded so a dead runtime can't hang shutdown either.
    if (sSession != XR_NULL_HANDLE && sRunning) {
        if (sFrameBegun) {
            XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
            fei.displayTime = sFrameState.predictedDisplayTime;
            fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            fei.layerCount = 0;
            xrEndFrame(sSession, &fei);
            sFrameBegun = false;
        }
        xrRequestExitSession(sSession);
        bool ended = false;
        for (int tries = 0; tries < 100 && !ended; tries++) { // ~1 s ceiling
            XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
            while (xrPollEvent(sInstance, &ev) == XR_SUCCESS) {
                if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                    const auto* sc = (const XrEventDataSessionStateChanged*)&ev;
                    if (sc->state == XR_SESSION_STATE_STOPPING) {
                        xrEndSession(sSession);
                        ended = true;
                    } else if (sc->state == XR_SESSION_STATE_EXITING || sc->state == XR_SESSION_STATE_IDLE) {
                        ended = true;
                    }
                }
                ev = { XR_TYPE_EVENT_DATA_BUFFER };
            }
            if (!ended) {
                Sleep(10);
            }
        }
    }
    if (sPtLayer != XR_NULL_HANDLE && pfnDestroyPassthroughLayer) { pfnDestroyPassthroughLayer(sPtLayer); sPtLayer = XR_NULL_HANDLE; }
    if (sPassthrough != XR_NULL_HANDLE && pfnDestroyPassthrough)  { pfnDestroyPassthrough(sPassthrough);  sPassthrough = XR_NULL_HANDLE; }
    sHasPassthrough = false;
    if (sEyeFbo)     { glDeleteFramebuffers(1, &sEyeFbo); sEyeFbo = 0; }
    if (sBlitReadFbo){ glDeleteFramebuffers(1, &sBlitReadFbo); sBlitReadFbo = 0; }
    if (sEyeDepthRB) { glDeleteRenderbuffers(1, &sEyeDepthRB); sEyeDepthRB = 0; }
    if (sOverlayFbo)     { glDeleteFramebuffers(1, &sOverlayFbo); sOverlayFbo = 0; }
    if (sOverlayDepthRB) { glDeleteRenderbuffers(1, &sOverlayDepthRB); sOverlayDepthRB = 0; }
    if (sHud.handle != XR_NULL_HANDLE) { xrDestroySwapchain(sHud.handle); sHud.handle = XR_NULL_HANDLE; }
    if (sHud.images) { free(sHud.images); sHud.images = NULL; }
    if (sViewSpace != XR_NULL_HANDLE) { xrDestroySpace(sViewSpace); sViewSpace = XR_NULL_HANDLE; }
    for (int e = 0; e < 2; e++) {
        if (sEye[e].handle != XR_NULL_HANDLE) { xrDestroySwapchain(sEye[e].handle); sEye[e].handle = XR_NULL_HANDLE; }
        if (sEye[e].images) { free(sEye[e].images); sEye[e].images = NULL; }
        sEye[e].imgCount = 0;
    }
    if (sLocalSpace != XR_NULL_HANDLE) { xrDestroySpace(sLocalSpace); sLocalSpace = XR_NULL_HANDLE; }
    if (sSession    != XR_NULL_HANDLE) { xrDestroySession(sSession);  sSession    = XR_NULL_HANDLE; }
    if (sInstance   != XR_NULL_HANDLE) { xrDestroyInstance(sInstance); sInstance   = XR_NULL_HANDLE; }
    sRunning = false;
    sFrameBegun = false;
    sViewCount = 0;
}

#else // no VR layer (non-Windows, or ENABLE_VR off) - stubs so the flat build links unchanged.

extern "C" bool vr_is_active(void) { return false; }
extern "C" int  vr_display_refresh_hz(void) { return 0; }
extern "C" bool vr_headset_present(void) { return false; }
extern "C" void vr_begin_frame(void) {}
extern "C" int  vr_eye_count(void) { return 0; }
extern "C" int  vr_eye_width(int e)  { (void)e; return 0; }
extern "C" int  vr_eye_height(int e) { (void)e; return 0; }
extern "C" bool vr_begin_eye(int e) { (void)e; return false; }
extern "C" void vr_end_eye(int e) { (void)e; }
extern "C" bool vr_submit_eye_texture(int e, unsigned int t, int w, int h) { (void)e; (void)t; (void)w; (void)h; return false; }
extern "C" bool vr_submit_panel_texture(unsigned int t, int w, int h) { (void)t; (void)w; (void)h; return false; }
extern "C" bool vr_present_desktop_panel(int w, int h) { (void)w; (void)h; return false; }
extern "C" void vr_menu_render_begin(int w, int h) { (void)w; (void)h; }
extern "C" void vr_menu_render_present(int w, int h) { (void)w; (void)h; }
extern "C" void vr_menu_mirror_desktop(int w, int h) { (void)w; (void)h; }
extern "C" void vr_menu_apply_opacity(void) {}
extern "C" void vr_mirror_game_desktop(unsigned int t, int sw, int sh, int dw, int dh, int c) { (void)t; (void)sw; (void)sh; (void)dw; (void)dh; (void)c; }
extern "C" const float* vr_eye_viewproj(int e) { (void)e; return 0; }
extern "C" const float* vr_sky_viewproj(int e) { (void)e; return 0; }
extern "C" void vr_set_sky_fov(float a, float b) { (void)a; (void)b; }
extern "C" float vr_sky_fov_h(void) { return 0.0f; }
extern "C" float vr_sky_fov_v(void) { return 0.0f; }
extern "C" float vr_sky_decouple_rad(void) { return 0.0f; }
extern "C" bool vr_sky_dome_active(void) { return false; }
extern "C" bool vr_sky_remap_active(void) { return false; }
extern "C" void vr_set_sky_camera(const float e[3], const float a[3], const float u[3]) { (void)e; (void)a; (void)u; }
extern "C" bool vr_passthrough_supported(void) { return false; }
extern "C" bool vr_passthrough_active(void) { return false; }
extern "C" int  vr_overlay_width(void) { return 0; }
extern "C" int  vr_overlay_height(void) { return 0; }
extern "C" bool vr_begin_overlay(bool s) { (void)s; return false; }
extern "C" void vr_end_overlay(bool s) { (void)s; }
extern "C" void vr_set_panel_mode(bool on) { (void)on; }
extern "C" bool vr_begin_panel(void) { return false; }
extern "C" void vr_end_panel(void) {}
extern "C" void vr_submit(void) {}
extern "C" float vr_get_world_scale(void) { return 0; } extern "C" void vr_set_world_scale(float v) { (void)v; }
extern "C" float vr_get_stereo(void) { return 0; }      extern "C" void vr_set_stereo(float v) { (void)v; }
extern "C" float vr_get_head_scale(void) { return 0; }  extern "C" void vr_set_head_scale(float v) { (void)v; }
extern "C" float vr_get_eye_height(void) { return 0; }  extern "C" void vr_set_eye_height(float v) { (void)v; }
extern "C" float vr_get_menu_dist(void) { return 0; }   extern "C" void vr_set_menu_dist(float v) { (void)v; }
extern "C" float vr_get_menu_size(void) { return 0; }   extern "C" void vr_set_menu_size(float v) { (void)v; }
extern "C" float vr_get_hud_scale(void) { return 0; }   extern "C" void vr_set_hud_scale(float v) { (void)v; }
extern "C" float vr_get_hud_dist(void)  { return 0; }   extern "C" void vr_set_hud_dist(float v) { (void)v; }
extern "C" int   vr_get_view_mode(void) { return 0; }   extern "C" void vr_set_view_mode(int m) { (void)m; }
extern "C" float vr_fp_forward_game_units(void) { return 0.0f; }
extern "C" const float* vr_hud_viewproj(int e) { (void)e; return 0; }
extern "C" const float* vr_hud_viewproj_flat(int e) { (void)e; return 0; }
extern "C" const float* vr_full2d_viewproj(int e) { (void)e; return 0; }
extern "C" void  vr_reset_defaults(void) {}
extern "C" void  vr_recenter(void) {}
extern "C" void  vr_set_hud_menu_mode(int on) { (void)on; }
extern "C" float vr_head_yaw_rad(void) { return 0; }
extern "C" float vr_head_pitch_rad(void) { return 0; }
extern "C" void vr_debug_synth_matrices(int eye, float e[16], float s[16], float h[16], float hf[16], float f[16]) { (void)eye; (void)e; (void)s; (void)h; (void)hf; (void)f; }
extern "C" int  vr_debug_dump_texture(unsigned int t, int w, int hh, const char* p) { (void)t; (void)w; (void)hh; (void)p; return 0; }
extern "C" bool  vr_controllers_active(void) { return false; }
extern "C" unsigned vr_controller_buttons(void) { return 0; }
extern "C" void  vr_controller_stick(int hand, float out[2]) { (void) hand; out[0] = out[1] = 0.0f; }
extern "C" void  vr_controller_rumble(float strength, float seconds) { (void) strength; (void) seconds; }
extern "C" void  vr_controller_rumble_stop(void) {}
extern "C" void  vr_shutdown(void) {}
extern "C" void  vr_set_flip_angle(float radians) { (void)radians; }
extern "C" void  vr_set_fp_framing(int on) { (void)on; }

#endif
