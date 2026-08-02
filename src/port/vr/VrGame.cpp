// Game-side VR glue for Lighthouse: the small set of decisions that need to know what
// Banjo-Kazooie is currently doing. Everything OpenXR-shaped lives in vr.cpp; everything here is
// game state, controller mapping, and the knobs the two menus drive.
//
// Windows-only, and every symbol is compiled out (or stubbed) without ENABLE_VR, so a flat build is
// byte-identical to stock.

#include "vr.h"

#if defined(ENABLE_VR) && defined(_WIN32)

#include <libultraship/libultraship.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include "port/UI/cvar_prefixes.h"          // CVAR_SETTING - the control-scheme check in the pad merge
#include "port/Controller/ControlSchemes.h" // CONTROL_SCHEME_MODERN

extern "C" {
#include "enums.h"      // game_mode_e, GameMap
#include <libultraship/libultra/controller.h>

s32 getGameMode(void);
s32 bs_getState(void); // the player state machine - the crouch gate for the Y = C-Left trot entry
enum map_e gsworld_getMap(void);
s32 gsworld_getEnableDraw(void);
void func_8028E9C4(s32 mode, f32 out[3]); // mode 5 = player EYE position (foot pos + per-transform head height)
void player_getRotation(f32 dst[3]);
bool player_inWater(void); // swimming or diving - the swim-follow gate
int bsbswim_inSet(int state); // nonzero for the UNDERWATER (dive) state family - surface paddling excluded
bool func_802A73BC(void); // at/near the water SURFACE - the swim state machine's own boundary predicate
f32 floor_getCurrentFloorYPosition(void); // the physics floor under the player - the FP eye's hard deck
// The player's animation clock, for the flip cam. AnimCtrl is opaque here - only the normalized
// 0..1 position matters, and C linkage cares about the name, not the pointer's type.
void* baanim_getAnimCtrlPtr(void);
f32   anctrl_getAnimTimer(void* animCtrl);

// Freshness window for the player state machine. bs_updateState stamps every LIVE player tick;
// MergePad decrements once per input tick. Nothing clears bs_getState() or player_inWater() when
// gameplay ENDS (both reset only inside bsmethods_reset, on the NEXT spawn), and the file select
// screen runs at GAME_MODE_3_NORMAL - measured, not assumed - so neither the mode nor those
// globals can tell "swimming" from "staring at the save screen with a drowned corpse's state".
// Only the tick stamp can: no live player, no stamp, and the window dies in 3 input ticks.
static int sBsFresh = 0;
extern "C" void port_vrBsTicked(void) {
    sBsFresh = 3;
}
static bool BsStateIsLive(void) {
    return sBsFresh > 0;
}
void playerPosition_get(f32 dst[3]);
bool player_isStable(void); // standing on ground
void controller_getRightStick(s32 controller_index, f32 dst[2]); // merged pad right stick, normalized +-1
void controller_getJoystick(s32 controller_index, f32 dst[2]);   // merged pad LEFT stick, normalized +-1
void yaw_set(f32 yaw);      // the player's actual body yaw (degrees)
void yaw_setIdeal(f32 yaw); // the yaw the body springs toward - set both or the spring pulls back
f32 time_getDelta(void);
// The game's own camera state - the anti-clip guard reads its collision result rather than
// duplicating collision code.
void ncDynamicCamera_getPosition(f32 dst[3]);
void func_802C0490(f32 focus[3]); // camera focus point (Banjo)
f32 func_802BD8D4(void);          // the camera's DESIRED orbit distance, before wall collision
void ncDynamicCamera_getRotation(f32 dst[3]);
void viewport_setPosition_vec3f(f32 pos[3]);
// The game's own collision raycast. Using it (rather than a second collision system) keeps the VR
// anti-clip agreeing with the walls the game itself believes in. The real return is a collision
// triangle whose type this file has no need of - only "did it hit" matters here.
void* func_80320B98(f32 from[3], f32 to[3], f32 hitOut[3], u32 mask);
// The game's own rotate-by-pitch-then-yaw helper - the same one the frustum planes and the free
// look orbit are built with, so directions derived here can never disagree with the game's.
void func_80256E24(f32 dst[3], f32 pitch, f32 yaw, f32 x, f32 y, f32 z);
}

#include <math.h>
#include <stdio.h>
#include <string.h>

// ---- what the game is doing right now ---------------------------------------

// The cutscene maps: scripted camera sweeps (the intro, Grunty's reveals, the endings). Those camera
// moves are authored for a flat screen and read as motion sickness in stereo, so they play on the flat
// panel instead - the same call the flying ports make for their level intros.
static bool VrGame_IsCutsceneMap(int map) {
    switch (map) {
        case MAP_1E_CS_START_NINTENDO:
        case MAP_1F_CS_START_RAREWARE:
        case MAP_20_CS_END_NOT_100:
        case MAP_7B_CS_INTRO_GL_DINGPOT_1:
        case MAP_7C_CS_INTRO_BANJOS_HOUSE_1:
        case MAP_7D_CS_SPIRAL_MOUNTAIN_1:
        case MAP_7E_CS_SPIRAL_MOUNTAIN_2:
        case MAP_81_CS_INTRO_GL_DINGPOT_2:
        case MAP_82_CS_ENTERING_GL_MACHINE_ROOM:
        case MAP_83_CS_GAME_OVER_MACHINE_ROOM:
        case MAP_84_CS_UNUSED_MACHINE_ROOM:
        case MAP_85_CS_SPIRAL_MOUNTAIN_3:
        case MAP_86_CS_SPIRAL_MOUNTAIN_4:
        case MAP_87_CS_SPIRAL_MOUNTAIN_5:
        case MAP_88_CS_SPIRAL_MOUNTAIN_6:
        case MAP_89_CS_INTRO_BANJOS_HOUSE_2:
        case MAP_8A_CS_INTRO_BANJOS_HOUSE_3:
        case MAP_94_CS_INTRO_SPIRAL_7:
        case MAP_95_CS_END_ALL_100:
        case MAP_96_CS_END_BEACH_1:
        case MAP_97_CS_END_BEACH_2:
        case MAP_98_CS_END_SPIRAL_MOUNTAIN_1:
        case MAP_99_CS_END_SPIRAL_MOUNTAIN_2:
        case MAP_91_FILE_SELECT:
            return true;
        default:
            return false;
    }
}

// The stereo gate. Per-eye rendering only while the player is actually IN a world being played:
// GAME_MODE_3_NORMAL with the world drawing. Everything else - the file select, cutscene maps, and
// Theater mode - renders once onto the head-locked panel. Pause (GAME_MODE_4_PAUSED) deliberately
// stays stereo: Banjo's pause menu draws over the live world, and dropping to a flat panel every time
// the player pauses would yank them out of the world.
// The gate minus the live-session requirement: also drives the headless eye-dump harness, which
// exercises the stereo render path with no OpenXR session at all.
extern "C" bool VrGame_StereoEligible(void) {
    if (vr_get_view_mode() == VR_VIEW_THEATER) {
        return false;
    }
    const s32 mode = getGameMode();
    if (mode != GAME_MODE_3_NORMAL && mode != GAME_MODE_4_PAUSED) {
        return false;
    }
    if (!gsworld_getEnableDraw()) {
        return false; // between maps: the game draws a black rect and a bare perspective
    }
    if (CVarGetInteger("gVRCutscenes", 0) == 0 && VrGame_IsCutsceneMap((int)gsworld_getMap())) {
        return false;
    }
    return true;
}

extern "C" bool VrGame_StereoActive(void) {
    // The headless eye-dump harness (BK_VR_EYEDUMP) counts as an active stereo session for every
    // game-side gate, so the harness verifies the same code path the headset exercises - a gate that
    // only fires with a live session is a gate that ships unverified.
    static int sHarness = -1;
    if (sHarness < 0) {
        sHarness = (getenv("BK_VR_EYEDUMP") != NULL) ? 1 : 0;
    }
    return (vr_is_active() || sHarness) && VrGame_StereoEligible();
}

// Cycle the VR view mode with the right stick click: Third -> First -> Diorama -> wrap. Slot 2 is
// the flying ports' Cockpit (no Banjo equivalent) and THEATER is deliberately not in the cycle - it
// is a comfort/menu choice, not something to trip into mid-jump; select it in Enhancements -> VR.
extern "C" void VrGame_CycleViewMode(void) {
    // The order players expect: FIRST PERSON -> THIRD PERSON -> DIORAMA -> wrap. Theater stays a
    // menu-only choice, and anything unexpected wraps home to First Person.
    const int m = vr_get_view_mode();
    const int next = (m == VR_VIEW_FIRST_PERSON) ? VR_VIEW_THIRD_PERSON
                   : (m == VR_VIEW_THIRD_PERSON) ? VR_VIEW_DIORAMA
                                                 : VR_VIEW_FIRST_PERSON;
    vr_set_view_mode(next);
}

// ---- immersive first person --------------------------------------------------

// Per-tick clock, incremented in VrGame_SyncFrame. Freshness reference for the sky-pass Mtx
// registry and the "is the FP override actually driving the camera" gate below.
static int sVrTick = 0;

// True while VR First Person should own the camera and hide the bear: an OpenXR session is live, the
// mode is First Person, and the player is actually in a playable world (the same stereo gate the
// renderer uses - cutscene maps and the file select keep their own cameras and their own Banjo).
static bool VrFp_Active(void) {
    return vr_get_view_mode() == VR_VIEW_FIRST_PERSON && VrGame_StereoActive();
}

// The tick the FP override last actually drove the camera. When the GAME takes the camera - a
// dialogue framing Banjo and an NPC, a drone shot - the override stops running, this goes stale,
// and the bear must come back: an invisible Banjo in a conversation shot was the wild report
// "start a dialogue and Banjo is invisible". Pause is the exception (the camera update stops but
// the eye is still parked inside his head).
static int sFpCamDriveTick = -100;

extern "C" int port_vrFirstPerson_hidePlayer(void) {
    if (!VrFp_Active()) {
        return 0;
    }
    if (getGameMode() == GAME_MODE_4_PAUSED) {
        return 1; // camera update is stopped but the view is still from inside his head
    }
    return (sVrTick - sFpCamDriveTick) <= 2 ? 1 : 0;
}

// VR stereo pause renders the live (frozen) world behind the pause menu instead of the flat game's
// 2D framebuffer snapshot - a flat plate cannot back a stereo scene. Consumed by gcpausemenu_draw.
extern "C" int port_vrPauseKeepsWorldLive(void) {
    return (VrGame_StereoActive() && getGameMode() == GAME_MODE_4_PAUSED) ? 1 : 0;
}

// How far the chase camera sits from Banjo, as a multiplier on the game's own orbit distance.
// Third Person and Diorama only - and only while VR is actually driving, so flat play is untouched.
// (The previous "push the eye back in camera space" approach was never wired to anything, which is
// why the slider did nothing; scaling the game's own distance moves the camera for real, keeps
// collision working, and every camera mode inherits it.)
extern "C" f32 port_vrCamDistScale(void) {
    if (!VrGame_StereoActive()) {
        return 1.0f;
    }
    const int mode = vr_get_view_mode();
    if (mode != VR_VIEW_THIRD_PERSON && mode != VR_VIEW_DIORAMA) {
        return 1.0f;
    }
    float m = CVarGetFloat("gVRThirdPersonDist", 0.7f);
    if (m < 0.3f) {
        m = 0.3f;
    }
    if (m > 3.0f) {
        m = 3.0f;
    }
    return m;
}

// Pull the chase camera in toward Banjo (or push it out) by scaling the FINAL camera position about
// the focus point. This runs after every camera state and after the game's own wall collision, so
// one multiplier covers all of them - the earlier attempt scaled the orbit-distance GETTER, which
// only a couple of states actually consult, and measured as barely any movement.
extern "C" void port_vrCamDist_apply(f32 position[3]) {
    const float m = port_vrCamDistScale();
    if (m > 0.999f && m < 1.001f) {
        return;
    }
    f32 focus[3];
    func_802C0490(focus);
    position[0] = focus[0] + (position[0] - focus[0]) * m;
    position[1] = focus[1] + (position[1] - focus[1]) * m;
    position[2] = focus[2] + (position[2] - focus[2]) * m;
}

// Diorama look damping: the tabletop sits inches from your face, so the same stick push sweeps a
// far larger visual angle than it does in a life-size world. Half rate plus the squared stick
// response at the call site is what makes orbiting a miniature feel like turning a model on a
// turntable instead of whipping the room around.
extern "C" f32 port_vrDioramaLookScale(void) {
    if (!VrGame_StereoActive() || vr_get_view_mode() != VR_VIEW_DIORAMA) {
        return 1.0f;
    }
    return 0.5f;
}

// Plain C bridge for game files that only need the stereo yes/no (transition.c and friends).
extern "C" int port_vrStereoActive(void) {
    return VrGame_StereoActive() ? 1 : 0;
}

// ---- stereo culling ----------------------------------------------------------
//
// The game's frustum planes (viewport_update) are built for the flat 40-degree window: half-angles
// of about 27 degrees horizontally and 20 vertically, facing wherever the GAME camera faces. A VR
// eye sees 50+ degrees per side, and with the world-stable First Person base the head can face a
// long way off the game camera - so geometry the eyes can plainly see gets culled, and it pops in
// and out as you turn. This hook, called from viewport_update, rebuilds the cull inputs for stereo:
//
//   - direction = game camera composed with the HEAD's yaw/pitch (the eyes' actual view axis);
//   - half-angles widened to 88 degrees. Two planes can only describe a convex wedge, so ~90 is
//     the geometric ceiling - at 88 the four planes collapse to almost a single "behind the eyes"
//     plane, which is exactly the set a headset user can never see without turning (and turning
//     re-aims the planes next tick, far faster than a head can sweep the 33-degree margin);
//   - a plane pad backing everything off by how far the eyes can sit from the camera anchor
//     (lean, the First Person forward push, Diorama placement), so a displaced eye still never
//     sees a culled edge.
//
// Sign notes, derived once and pinned: game yaw is CCW-positive (yaw+ = view LEFT - the same fact
// that makes the FP stick sign "-="), vr_head_yaw_rad is + when the head turns RIGHT, so the yaw
// composes with a MINUS. Game pitch through func_80256E24 is + = up, matching vr_head_pitch_rad.
extern "C" void port_vrCullAdjust(f32* pitchDeg, f32* yawDeg, f32* frustumX, f32* frustumY,
                                  f32* padWorldUnits) {
    if (!VrGame_StereoActive()) {
        return;
    }
    // Harness A/B seam: BK_VR_CULLTEST=off leaves the flat planes in place with everything else
    // identical, so an eye-dump pair isolates exactly what the widening admits. =pad and =ang
    // apply one component alone, for bisecting a regression to the pad or the angles.
    static int sCullMode = -1; // 0 full, 1 off, 2 pad only, 3 angles only
    if (sCullMode < 0) {
        const char* t = getenv("BK_VR_CULLTEST");
        sCullMode = (t == NULL)                ? 0
                    : (strcmp(t, "off") == 0)  ? 1
                    : (strcmp(t, "pad") == 0)  ? 2
                    : (strcmp(t, "ang") == 0)  ? 3
                                               : 0;
    }
    if (sCullMode == 1) {
        return;
    }
    const float kRad2Deg = 57.29577951308232f;
    // The game stores camera angles 0..360, so a chase camera pitched DOWN reads as ~340 - and a
    // clamp that trusts the raw number pins the cull axis at the sky, which culls every small
    // object below the camera plane. Banjo included: that was the round-18 "Banjo is invisible".
    // Normalize to signed degrees FIRST, then compose the head, then clamp.
    float p = fmodf(*pitchDeg, 360.0f);
    if (p > 180.0f) {
        p -= 360.0f;
    }
    if (p < -180.0f) {
        p += 360.0f;
    }
    p += vr_head_pitch_rad() * kRad2Deg;
    if (p > 89.0f) {
        p = 89.0f;
    }
    if (p < -89.0f) {
        p = -89.0f;
    }
    *pitchDeg = p;
    *yawDeg -= vr_head_yaw_rad() * kRad2Deg;

    // The plane components are cotangent-form: lateral = Z / tan(half angle), with Z pinned to
    // viewport.c's constants (45.1685 horizontal, 34.2020 vertical). Shrinking the lateral
    // component widens the wedge; the planes are normalized after, so only the ratio matters.
    if (sCullMode != 2) {
        const float kHalfAngleRad = 88.0f * 0.01745329252f;
        *frustumX = 45.168514251708984f / tanf(kHalfAngleRad);
        *frustumY = 34.20201110839844f / tanf(kHalfAngleRad);
    }
    if (sCullMode == 3) {
        return; // angles only: leave the pad at zero
    }

    // Pad: the eyes are not AT the camera anchor. Lean can put them half a metre out, First
    // Person's forward push is game units already, and Diorama's placement offsets are metres at
    // the tabletop scale. Computed live from where the eyes actually sit, plus margin for a tick
    // of motion and the IPD itself.
    const int mode = vr_get_view_mode();
    const float unitsPerM = (mode == VR_VIEW_DIORAMA)      ? CVarGetFloat("gVRDioramaWorldScale", 6200.0f)
                          : (mode == VR_VIEW_FIRST_PERSON) ? CVarGetFloat("gVRFirstPersonScale", 100.0f)
                                                           : CVarGetFloat("gVRWorldScale", 100.0f);
    float off[3];
    vr_get_head_offset_m(off);
    float padM = 0.75f + sqrtf(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
    if (mode == VR_VIEW_DIORAMA) {
        padM += fabsf(CVarGetFloat("gVRDioramaDist", 0.0f)) + fabsf(CVarGetFloat("gVRDioramaHeight", -0.06f));
    }
    *padWorldUnits = padM * unitsPerM + fabsf(vr_fp_forward_game_units());
}

// ---- sky pass registration ----------------------------------------------------
//
// The sky is world models drawn AT the camera position (sky.c), so per-eye separation hands it
// parallax it cannot have - depth on something that must read as infinity. The game registers the
// sky pass's perspective Mtx pointer here each frame it draws one; the interpreter asks on every
// perspective LOAD during eye replay and substitutes the zero-separation eye matrix for exactly
// that load. Pointer identity is the same mechanism frame interpolation already relies on, so
// there is no heuristic to misfire (the "draws at the origin are sky" classifiers this family has
// been burned by). Freshness is a tick-stamped two-slot ring: a stale pointer from a map that
// stopped drawing sky can never claim some other pass's matrix.
static void* sSkyPerspMtx[2] = { NULL, NULL };
static int sSkyPerspTick[2] = { -100, -100 };
static int sSkyPerspSlot = 0;

extern "C" void port_vrMarkSkyPerspMtx(void* mtx) {
    sSkyPerspSlot ^= 1;
    sSkyPerspMtx[sSkyPerspSlot] = mtx;
    sSkyPerspTick[sSkyPerspSlot] = sVrTick;
}

extern "C" bool vr_sky_persp_match(const void* mtxAddr) {
    for (int i = 0; i < 2; i++) {
        const int age = sVrTick - sSkyPerspTick[i];
        if (sSkyPerspMtx[i] == mtxAddr && age >= -2 && age <= 2) {
            return true;
        }
    }
    return false;
}

// ---- mouse look ----------------------------------------------------------------
//
// The port had no mouse-look path at all: libultraship maps mouse BUTTONS and the wheel, and its
// relative-motion accumulator (Window::GetMouseDelta) had no consumer. This is that consumer. The
// feel is the sm64coopdx / SRB2 model exactly: raw counts multiplied by a constant, applied to the
// view the same tick they happened - no dt scaling (counts are already time-integrated; adding dt
// makes sensitivity change with framerate), no easing, no acceleration. Sampled once per game tick,
// consumed once by whichever look path owns the camera (First Person base or Free Look orbit).
static bool VrImGuiMenuVisible(void);          // defined with the pad merge below
extern "C" int port_vrNativeMenu_isOpen(void); // VrNativeMenu.cpp

static float sMouseDX = 0.0f;
static float sMouseDY = 0.0f;

static void VrGame_SampleMouse(void) {
    // Headless seam: BK_VR_MOUSETEST="dx,dy" feeds a constant per-tick delta so the whole chain
    // (sample -> look path -> camera -> rendered frame) is provable by panel dump with no mouse.
    static int sTestParsed = 0;
    static float sTestDx = 0.0f, sTestDy = 0.0f;
    if (sTestParsed == 0) {
        const char* t = getenv("BK_VR_MOUSETEST");
        sTestParsed = (t != NULL && sscanf(t, "%f,%f", &sTestDx, &sTestDy) == 2) ? 1 : -1;
    }
    if (sTestParsed == 1) {
        sMouseDX = sTestDx;
        sMouseDY = sTestDy;
        return;
    }

    auto ctx = Ship::Context::GetRawInstance();
    if (ctx == nullptr || ctx->GetWindow() == nullptr) {
        sMouseDX = sMouseDY = 0.0f;
        return;
    }
    auto wnd = ctx->GetWindow();
    // ALWAYS drain the accumulator - skipping reads while a menu is open banks the motion and
    // releases it as one view-snap the moment the menu closes.
    Ship::Coords d = wnd->GetMouseDelta();
    float dx = (float)d.x;
    float dy = (float)d.y;
    const bool use = wnd->IsMouseCaptured() && CVarGetInteger("gVRMouseLook", 1) != 0 &&
                     !VrImGuiMenuVisible() && !port_vrNativeMenu_isOpen() &&
                     getGameMode() == GAME_MODE_3_NORMAL;
    // First reads after boot or a capture toggle can carry a banked slug of motion.
    if (!use || dx > 400.0f || dx < -400.0f || dy > 400.0f || dy < -400.0f) {
        dx = dy = 0.0f;
    }
    sMouseDX = dx;
    sMouseDY = dy;
}

// Peek: the Free Look enter-trigger looks without consuming, so the first swipe both engages the
// orbit AND turns it. Take: the owning look path consumes, which also makes a per-sub-frame caller
// apply each tick's motion exactly once.
extern "C" void port_vrMouseDelta_peek(float* dx, float* dy) {
    *dx = sMouseDX;
    *dy = sMouseDY;
}
extern "C" void port_vrMouseDelta_take(float* dx, float* dy) {
    *dx = sMouseDX;
    *dy = sMouseDY;
    sMouseDX = sMouseDY = 0.0f;
}

// The in-game HUD stands down when hidden by choice (gVRHideHud, stereo only) or while the VR
// options overlay owns the screen. Consumed by the gameloop's HUD draw gates - game-side, so the
// overlay's own print-buffer text and dialogue boxes are never collateral.
extern "C" int port_vrNativeMenu_isOpen(void);
extern "C" int port_vrHudHidden(void) {
    if (port_vrNativeMenu_isOpen()) {
        return 1;
    }
    // The hide toggle applies to live GAMEPLAY only: the pause menu exists to show your totals, so
    // its widgets always render there.
    return (VrGame_StereoActive() && getGameMode() == GAME_MODE_3_NORMAL && CVarGetInteger("gVRHideHud", 0) != 0)
               ? 1
               : 0;
}

// The First Person camera, third design and the one that matches how it actually feels in a
// headset (round-3 rate look read as jarring, round-4 snap turns got rejected outright):
//
//   - The view base is WORLD-STABLE at rest: it never follows Banjo's facing, so the bear turning
//     can't sweep the horizon under you. Captured once when First Person engages.
//   - The right stick is DIRECT SMOOTH LOOK, 1:1 with deflection and framerate-independent: yaw at
//     up to 160 deg/s, pitch at up to 100 deg/s clamped to +-75 (behind FP STICK LOOK for players
//     who keep vertical on the head). No acceleration curves, no easing tail - the view moves
//     exactly while the stick is deflected and holds the instant it releases. What made round 3
//     sick was the base ALSO gliding on its own; a view that only ever moves 1:1 with the thumb
//     reads as self-motion, not vection.
//   - The base NEVER auto-recenters. Brief gate flickers (loading zones, dialogue) keep the base;
//     only leaving First Person for a real stretch (~1 s) re-captures from Banjo's facing.
// The First Person look base lives at file scope so the aim-alignment export below can read it.
static float sYaw = 0.0f;
static float sPitch = 0.0f;
static bool sBaseValid = false;
static int sInactiveFrames = 0;

// Face Banjo where the player is LOOKING: body yaw = view yaw (base + head) flipped through the
// model's 180-degree camera convention. The state machine calls this as an aim-driven state
// begins (barge, claw, peck, eggs), so an attack launched from inside his head goes where the
// eyes point instead of wherever the body last walked. Setting actual AND ideal yaw together
// keeps the yaw spring from pulling him back.
extern "C" void port_vrFpFaceViewYaw(void) {
    if (!VrFp_Active() || !sBaseValid) {
        return;
    }
    const float viewYaw = sYaw - vr_head_yaw_rad() * 57.29577951308232f;
    const float bodyYaw = fmodf(viewYaw - 180.0f + 720.0f, 360.0f);
    yaw_set(bodyYaw);
    yaw_setIdeal(bodyYaw);
}

extern "C" void port_vrFirstPerson_override(f32 position[3], f32 rotation[3]) {
    if (!VrFp_Active()) {
        // Tolerate short inactive stretches so a loading zone or dialogue can't recenter the view.
        if (sBaseValid && ++sInactiveFrames > 30) {
            sBaseValid = false;
        }
        return;
    }
    sInactiveFrames = 0;
    sFpCamDriveTick = sVrTick; // the override IS driving the camera this tick - keep the bear hidden

    f32 eye[3];
    func_8028E9C4(5, eye);
    position[0] = eye[0];
    position[1] = eye[1];
    position[2] = eye[2];

    // First person LIFE (gVRFpViewBob, the sm64 port's feel): a small stride bob while walking and
    // a damped dip on landing, driven by the body's REAL motion so it reads as self-motion rather
    // than camera waggle. Amplitudes are centimetres at life scale - presence cues, kept far under
    // the vection threshold, and off entirely in menus or while airborne.
    if (CVarGetInteger("gVRFpViewBob", 0) != 0) {
        static float sBobPhase = 0.0f;
        static float sPrevPos[3] = { 0.0f, 0.0f, 0.0f };
        static bool sPrevPosValid = false;
        static float sPrevVy = 0.0f;
        static float sDip = 0.0f;
        static float sDipVel = 0.0f;
        const float dtb = time_getDelta();
        f32 pp[3];
        playerPosition_get(pp);
        float vy = 0.0f, speed = 0.0f;
        if (sPrevPosValid && dtb > 0.0f) {
            const float vx = (pp[0] - sPrevPos[0]) / dtb;
            vy = (pp[1] - sPrevPos[1]) / dtb;
            const float vz = (pp[2] - sPrevPos[2]) / dtb;
            speed = sqrtf(vx * vx + vz * vz);
        }
        const bool grounded = player_isStable();
        if (grounded && speed > 40.0f) {
            sBobPhase += dtb * (5.0f + speed * 0.010f); // stride rate rises gently with speed
            position[1] += sinf(sBobPhase * 6.2831853f) * 2.0f; // ~2 cm at life scale
        } else if (!grounded) {
            sBobPhase = 0.0f;
        }
        // Landing: downward speed at the moment of touching ground becomes a spring-damped dip.
        if (grounded && sPrevVy < -250.0f) {
            sDipVel -= (-sPrevVy) * 0.012f;
            sPrevVy = 0.0f;
        }
        if (dtb > 0.0f) {
            sDip += sDipVel * dtb;
            sDipVel += (-sDip * 60.0f - sDipVel * 10.0f) * dtb;
            if (sDip < -8.0f) {
                sDip = -8.0f;
            }
            if (sDip > 2.0f) {
                sDip = 2.0f;
            }
        }
        position[1] += sDip;
        sPrevVy = grounded ? 0.0f : vy;
        sPrevPos[0] = pp[0];
        sPrevPos[1] = pp[1];
        sPrevPos[2] = pp[2];
        sPrevPosValid = true;
    }

    // FLOOR CLAMP, after every Y modifier: the eye is playerPosition + a FIXED head height
    // (func_8028E9C4 mode 5 - no animation in it), and the game legitimately drives the player
    // POSITION under the floor in some states: the beak buster slam buries the body on impact by
    // design, and pressing Z on a bumpy uphill walk is exactly how a buster fires by accident
    // ("crouching up a hill put my cam through the floor" - position dips, terrain rises, and
    // position+100 lands inside the hill). The eye never sinks below the game's own tracked
    // floor plus 0.3 m; max() is continuous, so the hold engages and releases without a pop.
    // Never in water: there the tracked "floor" is the SURFACE, and this clamp would pin the
    // camera above every dive. And only while the player machine is LIVE - the floor tracker is
    // another reset-only global (round-31 law), and on the front-end screen it holds garbage
    // that lifted the whole boot-screen camera (caught by the byte-regression dump).
    if (BsStateIsLive() && !player_inWater()) {
        const f32 floorY = floor_getCurrentFloorYPosition();
        const f32 minEyeY = floorY + 30.0f;
        const int engaged = (position[1] < minEyeY) ? 1 : 0;
        // Engagement edge print under the harness/live log - an image diff without a mechanism
        // counter cannot tell "clamp fired" from "scene drifted" (the round-17 law, relearned).
        static int sPrevEngaged = -1;
        if (engaged != sPrevEngaged) {
            static int sLog = -1;
            if (sLog < 0) {
                sLog = (getenv("BK_VR_LIVELOG") != NULL || getenv("BK_VR_EYEDUMP") != NULL) ? 1 : 0;
            }
            if (sLog == 1) {
                printf("[VR] fp floor clamp %s: eyeY=%.1f floorY=%.1f\n", engaged ? "ENGAGED" : "released",
                       position[1], floorY);
                fflush(stdout);
            }
            sPrevEngaged = engaged;
        }
        if (engaged) {
            position[1] = minEyeY;
        }
    }

    if (!sBaseValid) {
        // Capture the base ONCE, from Banjo's facing at the moment First Person engages (+180: the
        // model faces away from the chase camera - same flip the game's own C-up look applies).
        f32 playerRot[3];
        player_getRotation(playerRot);
        sYaw = playerRot[1] + 180.0f;
        sPitch = 0.0f;
        sBaseValid = true;
    }

    // SWIM FOLLOW: underwater the game steers Banjo like a vehicle on the LEFT stick, so a
    // world-stable base means chasing his heading with the right stick - with the same thumb
    // that holds the swim buttons (the first wild feature request). While IN WATER and ACTIVELY
    // steering, the base follows his yaw change 1:1. That stays inside the comfort law: the view
    // only ever moves while the player is commanding the turn themselves (the same
    // only-while-input rule as stick look, and how the sm64 port's first person follows Mario),
    // dead-stops with the stick, and never follows on land - the world-stable base is untouched
    // everywhere else.
    {
        static float sPrevPlayerYaw = 0.0f;
        static bool sPrevYawValid = false;
        f32 pr[3];
        player_getRotation(pr);
        float dyaw = fmodf(pr[1] - sPrevPlayerYaw + 540.0f, 360.0f) - 180.0f;
        // UNDERWATER only. The dive-family state check alone was NOT enough: the surface strokes
        // run THROUGH genuine dive states (bsbdiveb/bsswim_divea eject to SWIM_IDLE only via the
        // near-surface check, so every B stroke at the surface is a few ticks of BS_2C_DIVE_B),
        // which engaged the follow in bursts while steering - "the left stick auto-turns at the
        // surface", reported twice. The state machine's OWN boundary predicate (func_802A73BC,
        // the same call it uses to leave the dive states near the surface) is the missing term:
        // follow only when genuinely submerged AWAY from the surface. Liveness-stamped like every
        // bs-state consumer (round 31). The follow stays CAPPED per tick - 2.5 degrees (~75
        // deg/s) tracks a real swim turn while staying under the vection threshold.
        if (sPrevYawValid && BsStateIsLive() && player_inWater() && bsbswim_inSet(bs_getState()) &&
            !func_802A73BC() &&
            CVarGetInteger("gVRFpSwimFollow", 1) != 0) {
            f32 move[2];
            controller_getJoystick(0, move);
            const float mag = sqrtf(move[0] * move[0] + move[1] * move[1]);
            if (mag > 0.35f && dyaw > -15.0f && dyaw < 15.0f) {
                if (dyaw > 2.5f) {
                    dyaw = 2.5f;
                }
                if (dyaw < -2.5f) {
                    dyaw = -2.5f;
                }
                sYaw += dyaw;
            }
        }
        sPrevPlayerYaw = pr[1];
        sPrevYawValid = true;
    }

    f32 rs[2];
    controller_getRightStick(0, rs);
    const float dt = time_getDelta();
    const float kDead = 0.15f;
    const float lookSpeed = CVarGetFloat("gVRFpLookSpeed", 220.0f);
    const float invertX = CVarGetInteger("gVRFpInvertX", 0) ? -1.0f : 1.0f;

    // DIRECT look, the round-5 contract restored: the view moves exactly while the thumb does and
    // dead-stops the instant it releases. The velocity easing added later for "comfort" was the
    // reported sludge - input-to-motion lag IS the discomfort in a headset, not the cure. The
    // quadratic response stays: fine aim near centre, full rate at full tilt.
    auto curve = [](float v, float dead) {
        if (v > dead) {
            const float t = (v - dead) / (1.0f - dead);
            return t * t;
        }
        if (v < -dead) {
            const float t = (-v - dead) / (1.0f - dead);
            return -(t * t);
        }
        return 0.0f;
    };

    // Stick right looks RIGHT. Verified in the headset; the sign that reads "correct" from the
    // guRotate(-yaw) math is the one that feels inverted, so the headset wins - do not re-derive.
    sYaw -= curve(rs[0], kDead) * lookSpeed * invertX * dt;

    if (CVarGetInteger("gVRFpVerticalLook", 1) != 0) {
        sPitch += curve(rs[1], kDead) * lookSpeed * 0.625f * dt;
    } else {
        sPitch = 0.0f;
    }

    // Mouse look, raw: counts to degrees the tick they happen, through the same invert flag as the
    // stick. The stick keeps its comfort easing above; the mouse gets NONE - 1:1 position response
    // is the whole sm64coopdx / SRB2 feel, and any filter here reads as lag under the hand.
    {
        float mdx, mdy;
        port_vrMouseDelta_take(&mdx, &mdy);
        if (mdx != 0.0f || mdy != 0.0f) {
            const float mouseSens = CVarGetFloat("gVRMouseSens", 0.10f);
            sYaw -= mdx * mouseSens * invertX; // mouse right looks right (same minus law as the stick)
            if (CVarGetInteger("gVRFpVerticalLook", 1) != 0) {
                sPitch += -mdy * mouseSens; // mouse up looks up
            }
        }
    }
    if (sPitch > 75.0f) {
        sPitch = 75.0f;
    }
    if (sPitch < -75.0f) {
        sPitch = -75.0f;
    }

    rotation[0] = sPitch;
    rotation[1] = fmodf(sYaw + 360.0f, 360.0f);
    rotation[2] = 0.0f;
}

// ---- motion controllers -> N64 pad ------------------------------------------

// A FIXED layout (not user bindings), matched to how Banjo actually plays. The merge happens in
// port_shapeControllerInput, ahead of the port's edge detection, so the controllers drive gameplay
// and every menu exactly like a gamepad would.
//
//   left stick        analog stick (move / menu navigation)
//   right trigger     B - ATTACK (the trigger is the attack, shooter-style)
//   left trigger      Z - crouch / Talon Trot / egg aim (the trigger IS the crouch)
//   right grip        A - jump (squeeze to hop; A on the face button does the same)
//   left grip         R - camera modifier / centre
//   A / B buttons     A / B directly (so either hand works in menus)
//   X                 Z (crouch from a face button too)
//   Y                 C-Left WHILE CROUCHED ONLY - the Talon Trot entry (Z + C-Left). Unbound
//                     otherwise: a bare C-press kicks Free Look back to the auto camera (read as
//                     "the camera recenters while I look around"). The close-up look IS the right
//                     stick: free-look pitch in third person, smooth look in first person.
//   menu button       Start (pause)
//   right stick       camera (Modern scheme orbit; C-flicks under Retro; look in First Person)
//   right stick click cycle VR view mode (handled by the caller: it is a mode switch, not a pad bit)
//
// WHILE PAUSED the triggers produce NO buttons: the right trigger is the VR-options toggle there,
// and a trigger that doubled as A would "select" the highlighted pause row on the very press that
// opens the overlay - unpausing the game underneath it (the round-3 lockup).
extern "C" int port_vrNativeMenu_isOpen(void); // VrNativeMenu.cpp

// True while the port (ImGui) menu is on screen - its pointer and nav own the sticks then.
static bool VrImGuiMenuVisible(void) {
    auto ctx = Ship::Context::GetRawInstance();
    if (ctx == nullptr || ctx->GetWindow() == nullptr || ctx->GetWindow()->GetGui() == nullptr) {
        return false;
    }
    return ctx->GetWindow()->GetGui()->GetMenuOrMenubarVisible();
}

// C bridge for the native overlay: while the ImGui menu is up, its pointer owns the right trigger
// (click), so the SAME press must not also toggle the native overlay underneath it.
extern "C" int port_vrImGuiMenuVisible(void) {
    return VrImGuiMenuVisible() ? 1 : 0;
}

extern "C" void port_vrNativeMenu_feedPad(unsigned short button, signed char stickX, signed char stickY);

// Underwater the game gives you TWO strokes, and both matter: B is the beak DASH (bsbdiveb kicks
// a 600-unit velocity impulse that decays) and A is the slow controlled PADDLE (bsswim_divea
// holds a steady 120). Dashing is how you cross open water; paddling is how you line up a note
// without overshooting it.
//
// VR wants the dash on A - reaching for B mid-swim feels wrong with the thumb already holding a
// stroke - so the two buttons SWAP in the dive states. The first version was a one-way A -> B
// remap, which handed the dash to both buttons and deleted the paddle entirely: the precise
// stroke simply stopped existing (reported from the field within a day). A swap keeps the pair
// intact, just relabelled: A dashes, B paddles.
//
// NOT IDEMPOTENT - that is why this runs exactly ONCE per tick, from the wrapper below, after
// every input source has merged. Applying a swap twice restores the original mapping, so the old
// two-call-site shape (safe for a one-way remap) would silently undo itself here.
//
// Every term of the gate is LIVE: machine ticked this window, water flag current, state in the
// dive family, real gameplay mode. Surface states are not in that family, so the surface A press
// - which IS the dive - is untouched. The first shipped version gated only on "not paused" and
// ate every A press on the save select screen after a water death: the stale-state ghost.
static void SwimButtonSwap(OSContPad* pad) {
    if (CVarGetInteger("gVRSwimADash", 1) == 0) {
        return; // stock layout: A paddles, B dashes
    }
    if (!BsStateIsLive() || getGameMode() != GAME_MODE_3_NORMAL || !player_inWater() ||
        !bsbswim_inSet(bs_getState())) {
        return;
    }
    const u16 held = pad->button;
    const u16 swapped = (u16)((held & ~(A_BUTTON | B_BUTTON)) | ((held & A_BUTTON) ? B_BUTTON : 0) |
                              ((held & B_BUTTON) ? A_BUTTON : 0));
    pad->button = swapped;
}

// The merge itself. Wrapped (see VrGame_MergePad) so the swim swap lands once, after every source.
static void MergePadSources(OSContPad* pad) {
    // The freshness window ages HERE, once per input tick, before anything consults it below.
    if (sBsFresh > 0) {
        sBsFresh--;
    }
    // BK_VR_LIVELOG=1: print game mode, player state, and the liveness gates ON CHANGE. This is
    // how "which mode is the screen I am stuck on" gets answered without a debugger - the
    // save-select A-eater hid behind exactly that question (stale dive state on a screen that
    // MEASURES as GAME_MODE_3_NORMAL, which nobody would have assumed).
    {
        static int sLog = -1;
        if (sLog < 0) {
            sLog = (getenv("BK_VR_LIVELOG") != NULL || getenv("BK_VR_EYEDUMP") != NULL) ? 1 : 0;
        }
        if (sLog == 1) {
            static s32 sPrevMode = -1, sPrevBs = -1;
            static int sPrevLive = -1, sPrevWater = -1;
            const s32 m = getGameMode(), b = bs_getState();
            const int live = BsStateIsLive() ? 1 : 0, water = player_inWater() ? 1 : 0;
            if (m != sPrevMode || b != sPrevBs || live != sPrevLive || water != sPrevWater) {
                printf("[VR] gameMode=%d bsState=0x%02X bsLive=%d inWater=%d\n", (int)m, (unsigned)b, live, water);
                fflush(stdout); // survives a killed process - an observability print that can vanish is no seam
                sPrevMode = m; sPrevBs = b; sPrevLive = live; sPrevWater = water;
            }
        }
    }
    // Hand the overlay the pad state FIRST, so a plain gamepad can open and drive it - the first
    // wild user played with an Xbox pad in the headset and pause + right trigger did nothing,
    // because every overlay read was motion-controller state. Fed before the zeroing below, or
    // the overlay would starve itself the moment it opens.
    port_vrNativeMenu_feedPad(pad->button, pad->stick_x, pad->stick_y);
    // While the native VR options overlay is up it owns EVERY input source: the whole pad zeroes
    // (SDL pads included), so neither the paused game nor its pause menu sees a single flick.
    if (port_vrNativeMenu_isOpen()) {
        pad->button = 0;
        pad->stick_x = 0;
        pad->stick_y = 0;
        pad->right_stick_x = 0;
        pad->right_stick_y = 0;
        return;
    }
    if (!vr_controllers_active() || CVarGetInteger("gVRMotionControls", 1) == 0) {
        return;
    }
    // While the port (ImGui) menu is open the sticks drive its pointer and nothing else - merging
    // them into the pad walked Banjo around behind the menu.
    if (VrImGuiMenuVisible()) {
        return;
    }

    const unsigned vb = vr_controller_buttons();
    const bool paused = (getGameMode() == GAME_MODE_4_PAUSED);
    u16 btn = 0;
    if (!paused) {
        if (vb & VR_BTN_RTRIGGER) { btn |= B_BUTTON; } // R2 attacks
        if (vb & VR_BTN_LTRIGGER) { btn |= Z_TRIG; }
    }
    if (vb & VR_BTN_A)        { btn |= A_BUTTON; }
    if (vb & VR_BTN_RGRIP)    { btn |= A_BUTTON; }
    if (vb & VR_BTN_B)        { btn |= B_BUTTON; }
    if (vb & VR_BTN_X)        { btn |= Z_TRIG; }
    if (vb & VR_BTN_LGRIP)    { btn |= R_TRIG; }
    if (vb & VR_BTN_MENU)     { btn |= START_BUTTON; }
    // Y = C-Left ONLY WHILE CROUCHED: the Talon Trot entry (Z + C-Left), otherwise unreachable on
    // motion controllers with the right stick owning the camera. Crouch-gated because a bare
    // C-press kicks Free Look back to the auto camera - the very reason Y was unbound before.
    // Freshness-gated like the swim remap above: bs_getState() is stale outside live gameplay
    // (quit while crouched and this would inject C-Left on the save select screen).
    if ((vb & VR_BTN_Y) && BsStateIsLive() && bs_getState() == BS_7_CROUCH) { btn |= L_CBUTTONS; }

    // Per axis the stronger source wins BY MAGNITUDE. The old form compared signed values, so an
    // idle VR stick (exactly 0) - or a whisper of negative drift - "won" against any POSITIVE
    // gamepad deflection and zeroed it: on an Xbox pad only LEFT and DOWN survived the merge,
    // which was the first wild bug report of the release. Magnitude is what "stronger" meant.
    auto mergeAxis = [](int8_t vr, int8_t& padAxis) {
        const int a = (vr < 0) ? -(int)vr : (int)vr;
        const int b = (padAxis < 0) ? -(int)padAxis : (int)padAxis;
        if (a > b) {
            padAxis = vr;
        }
    };

    // Right stick -> camera. The Modern control scheme (the default) reads the analog right stick
    // directly for its orbit camera, so feed the pad's right-stick fields (stronger source wins,
    // same rule as the left stick). Retro scheme has no analog camera - there the stick synthesizes
    // C-button flicks instead, with a dead zone so a resting thumb can't nudge the camera.
    float rs[2];
    vr_controller_stick(1, rs);
    {
        // Right-stick range law, second edition: libultraship fills these fields to +-85
        // (MAX_AXIS_RANGE) and controller_getRightStick now normalises by the same 85, so the VR
        // write matches - all three ends agree on one scale. (History: the merge once wrote 80
        // against a 127 reader, then 127 against an 85-filling gamepad; each mismatch surfaced
        // as a "look speed" bug that no speed constant could fix.)
        mergeAxis((int8_t)(rs[0] * 85.0f), pad->right_stick_x);
        mergeAxis((int8_t)(rs[1] * 85.0f), pad->right_stick_y);
    }
    if (CVarGetInteger(CVAR_SETTING("Controls.Scheme"), CONTROL_SCHEME_MODERN) != CONTROL_SCHEME_MODERN) {
        const float kCDeadZone = 0.5f;
        if (rs[0] < -kCDeadZone) { btn |= L_CBUTTONS; }
        if (rs[0] > kCDeadZone)  { btn |= R_CBUTTONS; }
        if (rs[1] > kCDeadZone)  { btn |= U_CBUTTONS; }
        if (rs[1] < -kCDeadZone) { btn |= D_CBUTTONS; }
    }

    pad->button |= btn;

    // Left stick -> analog stick. Same magnitude merge, so a gamepad or keyboard keeps working
    // alongside the controllers instead of fighting them for the same axis.
    float ls[2];
    vr_controller_stick(0, ls);
    mergeAxis((int8_t)(ls[0] * 80.0f), pad->stick_x);
    mergeAxis((int8_t)(ls[1] * 80.0f), pad->stick_y);
}

// Every input source merges first, THEN the swim swap - one application, whatever the pad is and
// whichever early-out the merge took (gamepad only, motion controllers, overlay open). A swap
// applied a second time is a swap undone, so there is exactly this one call site.
extern "C" void VrGame_MergePad(OSContPad* pad) {
    if (pad == NULL) {
        return;
    }
    MergePadSources(pad);
    SwimButtonSwap(pad);
}

// Per-tick VR<->game sync: while paused (or the VR overlay is up) the shared plane carries MENU
// content, so it switches to the SCREEN knobs - the HUD size/dist sliders stop moving the menus.
// FLIP CAM: how far the view is turned over RIGHT NOW, straight off the move's own animation clock
// (radians, positive = nose down). Not a timer of ours and not a canned spin - the view turns
// exactly as fast as Banjo turns, so it stays glued to the move at any frame rate, and a move cut
// short (landing early, taking a hit) simply stops driving it and the eye eases level.
//
//   FLIP JUMP (Z then A) - a full BACKWARD somersault, so a full turn backwards over the anim's
//   active range. It ends on 360, which is upright: the view arrives home by finishing the flip,
//   not by being snapped back.
//   BEAK BUSTER (A then Z) - a forward tuck into a beak-down plunge. The tuck is the first third of
//   the animation; past it the angle simply HOLDS nose-down, which is where Banjo's head actually
//   is for the whole fall, and the state ending on impact releases it.
static float VrFlipAngleRad(void) {
    if (vr_get_view_mode() != VR_VIEW_FIRST_PERSON || CVarGetInteger("gVRFpFlipCam", 1) == 0) {
        return 0.0f;
    }
    if (!BsStateIsLive() || getGameMode() != GAME_MODE_3_NORMAL) {
        return 0.0f;
    }
    const s32 st = bs_getState();
    if (st != BS_12_BFLIP && st != BS_F_BBUSTER) {
        return 0.0f;
    }
    void* anim = baanim_getAnimCtrlPtr();
    if (anim == NULL) {
        return 0.0f;
    }
    const f32 t = anctrl_getAnimTimer(anim);
    const float kDeg2Rad = 0.01745329252f;
    if (st == BS_12_BFLIP) {
        // bsbflip_init runs ASSET_4B_ANIM_BSBFLIP_ENTER over the sub-range 0 .. 0.7866.
        float p = t / 0.7866f;
        if (p < 0.0f) { p = 0.0f; }
        if (p > 1.0f) { p = 1.0f; }
        return -360.0f * p * kDeg2Rad; // backward
    }
    // bsbbuster_init runs ASSET_1D_ANIM_BSBBUSTER over the sub-range 0 .. 0.35 (the tuck).
    float p = t / 0.35f;
    if (p < 0.0f) { p = 0.0f; }
    if (p > 1.0f) { p = 1.0f; } // past the tuck: hold nose-down through the plunge
    return 90.0f * p * kDeg2Rad; // forward
}

extern "C" void VrGame_SyncFrame(void) {
    sVrTick++;              // freshness clock for the sky-pass Mtx registry
    VrGame_SampleMouse();   // one mouse sample per tick; look paths consume it once
    vr_set_hud_menu_mode(getGameMode() == GAME_MODE_4_PAUSED || port_vrNativeMenu_isOpen());
    vr_set_flip_angle(VrFlipAngleRad()); // flip cam target; vr.cpp eases it at the headset's rate

    // --- head translation scale, including the ANTI-CLIP wall clamp ---------------------------
    // Physical lean is what pushes the eye through walls: the game collides its CAMERA and knows
    // nothing about a player leaning half a metre sideways afterwards. Earlier versions probed
    // four fixed lateral directions and scaled lean by the nearest wall in ANY of them, eased both
    // ways - conservative in three directions that don't matter and rubbery in the one that does.
    // This casts ONE ray along the eye's ACTUAL displacement (vr_get_head_offset_m rotated into
    // the world by the camera's own pitch/yaw, through the game's own rotate helper - no
    // convention to get wrong) and CLAMPS the eye to the hit: the lean stops exactly at the wall,
    // which by construction is continuous as you lean in (no pop - the clamp point does not move).
    // Tightening is instant because the clamp only ever equals "park at the wall"; release eases
    // so stepping back out of a corner cannot fling the view.
    const int mode = vr_get_view_mode();
    const bool fp = (mode == VR_VIEW_FIRST_PERSON);
    float scale = CVarGetFloat("gVRHeadScale", 1.0f);
    if (fp && CVarGetInteger("gVRFpImmersive", 1) == 0) {
        scale = 0.0f; // immersive cam off: the eye stays pinned to the head bone
    }
    if (CVarGetInteger("gVRAntiClip", 1) != 0 && VrGame_StereoActive()) {
        // Diorama keeps the old nominal 100: at the tabletop scale a real conversion would clamp
        // every head motion against the miniature's walls, and orbiting the model IS the mode.
        const float unitsPerM = (mode == VR_VIEW_DIORAMA)      ? 100.0f
                              : (mode == VR_VIEW_FIRST_PERSON) ? CVarGetFloat("gVRFirstPersonScale", 100.0f)
                                                               : CVarGetFloat("gVRWorldScale", 100.0f);
        static float sGuard = 1.0f;
        float allowed = 1.0f;
        float off[3];
        vr_get_head_offset_m(off); // the APPLIED offset, metres - where the eye actually sits
        const float offLen = sqrtf(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
        if (offLen > 0.02f) { // under two centimetres there is nothing to clip
            f32 cam[3], rot[3];
            ncDynamicCamera_getPosition(cam);
            ncDynamicCamera_getRotation(rot);
            const float leanUnits = offLen * unitsPerM;
            const float radius = 0.10f * unitsPerM; // keep the eye a hand's width off the wall
            // World-space cast along the offset direction, length = lean + radius.
            const float k = (leanUnits + radius) / offLen;
            f32 dirW[3];
            func_80256E24(dirW, rot[0], rot[1], off[0] * k, off[1] * k, off[2] * k);
            f32 to[3] = { cam[0] + dirW[0], cam[1] + dirW[1], cam[2] + dirW[2] };
            f32 hit[3];
            if (func_80320B98(cam, to, hit, 0x9E0000) != NULL) {
                const float hx = hit[0] - cam[0], hy = hit[1] - cam[1], hz = hit[2] - cam[2];
                const float hitDist = sqrtf(hx * hx + hy * hy + hz * hz);
                allowed = (hitDist - radius) / (leanUnits > 1.0f ? leanUnits : 1.0f);
                if (allowed < 0.0f) {
                    allowed = 0.0f;
                }
                if (allowed > 1.0f) {
                    allowed = 1.0f;
                }
            }
        }
        if (allowed < 1.0f) {
            // The measured lean already includes the current guard, so the clamp COMPOSES: the
            // applied offset lands exactly at (hit - radius) and stays there while you push.
            sGuard *= allowed;
            if (sGuard < 0.05f) {
                sGuard = 0.05f; // never fully dead - a degenerate cast can't lock the head
            }
        } else {
            sGuard += (1.0f - sGuard) * 0.10f; // eased release, so backing out never pops
        }
        scale *= sGuard;
    }
    vr_set_head_scale(scale);

    // Camera distance applies LIVE. ncDynamicCamera_update does not run while the game is paused, so
    // without this the camera only jumped to its new distance after unpausing - the slider looked
    // dead in the very menu you set it from. Recomputing from the stored (unscaled) camera position
    // each tick is idempotent, so doing it here as well as in the camera handoff cannot compound.
    if (getGameMode() == GAME_MODE_4_PAUSED && VrGame_StereoActive()) {
        f32 pos[3];
        ncDynamicCamera_getPosition(pos);
        port_vrCamDist_apply(pos);
        viewport_setPosition_vec3f(pos);
    }
}

// Immersive First Person, the SM64 port's signature feel (gVRFpHeadMove, default on): stick-forward
// walks where your HEAD is looking, not where the stable view base points. The merged stick vector
// (motion controller AND gamepad alike) rotates by the head's yaw offset from the base. Runs after
// the merge, on the final stick, in port_shapeControllerInput.
extern "C" void VrGame_HeadMoveShape(OSContPad* pad) {
    if (pad == NULL || vr_get_view_mode() != VR_VIEW_FIRST_PERSON || !VrGame_StereoActive()) {
        return;
    }
    // IMMERSIVE CAM owns the feel as one switch: head-directed movement plus full physical lean.
    // Turning it off returns strict base-relative movement (the plain stick-drives-the-body model).
    if (CVarGetInteger("gVRFpImmersive", 1) == 0 || CVarGetInteger("gVRFpHeadMove", 1) == 0) {
        return;
    }
    const float a = vr_head_yaw_rad(); // + = head turned right of the view base
    if (a > -0.02f && a < 0.02f) {
        return;
    }
    const float c = cosf(a), sn = sinf(a);
    const float x = (float)pad->stick_x, y = (float)pad->stick_y;
    // Forward (0,+) maps to (sin a, cos a): walking follows the gaze. Flip sn if it tests mirrored.
    float rx = x * c + y * sn;
    float ry = -x * sn + y * c;
    if (rx > 127.0f) rx = 127.0f;
    if (rx < -128.0f) rx = -128.0f;
    if (ry > 127.0f) ry = 127.0f;
    if (ry < -128.0f) ry = -128.0f;
    pad->stick_x = (int8_t)rx;
    pad->stick_y = (int8_t)ry;
}

// Right-stick click cycles the view mode, and BOTH TRIGGERS HELD (~0.5 s) recenters. Neither is bound
// to the runtime-claimed menu button on purpose: on Quest-through-Virtual-Desktop the left menu button
// opens the streaming dash, which drops the app below FOCUSED - and controller state is only readable
// AT FOCUSED, so the press meant to trigger the action is the press that makes it unreadable.
// Edge-driven with a debounce so a held click acts once.
extern "C" void VrGame_PollVrShortcuts(void) {
    if (!vr_controllers_active() || CVarGetInteger("gVRMotionControls", 1) == 0) {
        return;
    }
    const unsigned vb = vr_controller_buttons();

    static int sStickDebounce = 0;
    if (sStickDebounce > 0) {
        sStickDebounce--;
    } else if (vb & VR_BTN_RSTICK) {
        VrGame_CycleViewMode();
        vr_controller_rumble(0.35f, 0.05f);
        sStickDebounce = 20;
    }

    // Recenter must be reachable in-headset by hand: a desktop hotkey never works because the game
    // window is not focused while a headset is on. Both triggers held is a combination the runtime has
    // no claim on, and the haptic tick confirms it fired.
    static int sBothTriggerFrames = 0;
    if ((vb & VR_BTN_LTRIGGER) && (vb & VR_BTN_RTRIGGER)) {
        sBothTriggerFrames++;
        if (sBothTriggerFrames == 30) { // ~0.5 s at 60 Hz
            vr_recenter();
            vr_controller_rumble(0.6f, 0.08f);
        }
    } else {
        sBothTriggerFrames = 0;
    }
}

#else // !ENABLE_VR - stubs so the flat build links without any VR code

extern "C" {
#include <libultraship/libultra/controller.h>
bool VrGame_StereoActive(void) {
    return false;
}
bool VrGame_StereoEligible(void) {
    return false;
}
void VrGame_CycleViewMode(void) {
}
void VrGame_MergePad(OSContPad* pad) {
    (void)pad;
}
void VrGame_PollVrShortcuts(void) {
}
void port_vrBsTicked(void) {
}
int port_vrFirstPerson_hidePlayer(void) {
    return 0;
}
void port_vrFirstPerson_override(float position[3], float rotation[3]) {
    (void)position;
    (void)rotation;
}
int port_vrPauseKeepsWorldLive(void) {
    return 0;
}
int port_vrStereoActive(void) {
    return 0;
}
float port_vrCamDistScale(void) {
    return 1.0f;
}
float port_vrDioramaLookScale(void) {
    return 1.0f;
}
void port_vrCamDist_apply(float position[3]) {
    (void)position;
}
int port_vrHudHidden(void) {
    return 0;
}
void VrGame_HeadMoveShape(OSContPad* pad) {
    (void)pad;
}
void VrGame_SyncFrame(void) {
}
void port_vrCullAdjust(float* pitchDeg, float* yawDeg, float* frustumX, float* frustumY, float* padWorldUnits) {
    (void)pitchDeg;
    (void)yawDeg;
    (void)frustumX;
    (void)frustumY;
    (void)padWorldUnits;
}
void port_vrMarkSkyPerspMtx(void* mtx) {
    (void)mtx;
}
bool vr_sky_persp_match(const void* mtxAddr) {
    (void)mtxAddr;
    return false;
}
void port_vrMouseDelta_peek(float* dx, float* dy) {
    *dx = *dy = 0.0f;
}
void port_vrMouseDelta_take(float* dx, float* dy) {
    *dx = *dy = 0.0f;
}
int port_vrImGuiMenuVisible(void) {
    return 0;
}
void port_vrFpFaceViewYaw(void) {
}
}

#endif
