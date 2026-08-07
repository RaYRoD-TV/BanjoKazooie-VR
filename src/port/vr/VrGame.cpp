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
#include "port/Controller/ModernCamera.h"   // port_cameraInvertXSign / YSign - the port's look invert

// Decomp headers, each with its own extern "C" guard, so the types below keep C linkage. model.h
// brings the model bin and its BONE TABLE, which the animated head offset walks; bonetransform.h
// brings the pose buffer it samples into.
#include "model.h"
#include "core2/bonetransform.h"

extern "C" {
#include "enums.h"      // game_mode_e, GameMap
#include <libultraship/libultra/controller.h>

s32 getGameMode(void);
s32 bs_getState(void); // the player state machine - the crouch gate for the Y = C-Left trot entry
enum map_e gsworld_getMap(void);
// The game's own map -> level table (core2/gc/section.c). Cutscene maps carry LEVEL_D_CUTSCENE
// there, and this accessor consults the romhack scene remap before the table, so it stays right
// for a hack that reassigns a map.
enum level_e map_getLevel(enum map_e map);
s32 gsworld_getEnableDraw(void);
// Field-report repro (BK_DIAG_WARP / BK_DIAG_SPAWN): the dev menu's own warp entry point, the
// player's ground position, and the game's deferred spawn queue - the same calls game code makes.
void func_8031D04C(enum map_e map, s32 exit_id);
void player_getPosition(f32 dst[3]);
void __spawnQueue_add_4(void* fn, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);
void* spawnQueue_actor_f32(s32 actor_id, uintptr_t x, uintptr_t y, uintptr_t z);
void gctransition_8030BEA4(s32 idx); // start a transition by table index (1 = falling jiggies in)
void func_8028E9C4(s32 mode, f32 out[3]); // mode 5 = player EYE position (foot pos + per-transform head height)
void player_getRotation(f32 dst[3]);
bool player_inWater(void); // swimming or diving - the swim-follow gate
int bsbswim_inSet(int state); // nonzero for the UNDERWATER (dive) state family - surface paddling excluded
bool func_802A73BC(void); // at/near the water SURFACE - the swim state machine's own boundary predicate
s32 balookat_getState(void); // nonzero while the game aims Banjo at something (drone/lookat shots)
s32  func_80294524(void); // nonzero while the game HOLDS you at the surface - the state where A jumps out
int bsbfly_inSet(int state); // nonzero in the FLIGHT state family - flying steers like swimming does
f32 floor_getCurrentFloorYPosition(void); // the physics floor under the player - the FP eye's hard deck
// The fall cam and the landing haptic ask the game for their inputs rather than inventing them:
// how far THIS fall has already run, and how hard the body is coming down.
f32 bafalldamage_get_distance_fallen(void);
f32 baphysics_get_vertical_velocity(void);
// The player's animation clock, for the flip cam and the animated head. AnimCtrl is opaque here -
// only the normalized 0..1 position and the asset id matter, and C linkage cares about the name,
// not the pointer's type.
void* baanim_getAnimCtrlPtr(void);
f32   anctrl_getAnimTimer(void* animCtrl);
enum asset_e anctrl_getIndex(void* animCtrl);

// Reading Banjo's POSE without drawing him. animationFile_getBoneTransformList is pure with respect
// to rendering - it reads the animation asset and writes a bone list, and touches no render state -
// which is what makes an animated first-person eye possible at all while the bear's draw is skipped.
struct animation_file_s;
typedef struct animation_file_s AnimationFile;
AnimationFile* animBinCache_get(enum asset_e asset_id); // NULL for sentinel ids and anything unloaded
void animationFile_getBoneTransformList(AnimationFile* anim_file, f32 progress, BoneTransformList* out);
void boneTransformList_reset(BoneTransformList* self);
// The 3-in-1 bone read animMtxList_setBoned itself uses: rotation quaternion, scale, translation.
void func_8033A5B8(BoneTransformList* self, s32 bone_id, f32 quat[4], f32 scale[3], f32 translation[3]);
void func_80345274(f32 quat[4], f32 out[3][3]); // quaternion -> 3x3, the same conversion the draw does
bool vec4f_isZero(f32 quat[4]);                 // true for the IDENTITY quaternion, despite the name
BKModelBin* baModel_getModelBin(void);
// The three angles and the scale the model is DRAWN with. All four have to come from the live
// accessors, not from the baModelYaw / baModelPitch / baModelRoll / baModelScale globals: those are
// written inside baModel_draw, and first person skips that draw, so they hold whatever the bear was
// doing the last time he was visible.
f32 baModel_computeModelYaw(void);
f32 baModel_getScale(void);
f32 pitch_get(void);
f32 roll_get(void);
// Which creature the player currently IS. Every transformation is a different skeleton, so this is
// what says whether Banjo's bones mean anything this tick.
enum transformation_e bsStoredState_getTransformation(void);

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
s32  ncDynamicCamera_getState(void); // which camera is driving - logged so scripted shots are identifiable
void viewport_setPosition_vec3f(f32 pos[3]);
// The game's own collision raycast. Using it (rather than a second collision system) keeps the VR
// anti-clip agreeing with the walls the game itself believes in. The real return is a collision
// triangle whose type this file has no need of - only "did it hit" matters here.
void* func_80320B98(f32 from[3], f32 to[3], f32 hitOut[3], u32 mask);
// The game's own rotate-by-pitch-then-yaw helper - the same one the frustum planes and the free
// look orbit are built with, so directions derived here can never disagree with the game's.
void func_80256E24(f32 dst[3], f32 pitch, f32 yaw, f32 x, f32 y, f32 z);
// The FULL three-angle version: dst = base + src rotated by roll, then pitch, then yaw. That order
// is not a detail - it is exactly the order func_80252AF0 pushes roll, pitch and yaw onto the
// matrix stack for a model, so a point rotated with this lands where the drawn model puts it.
// rotation is the same {pitch, yaw, roll} triple modelRender_draw is handed.
void func_80256F44(f32 base[3], f32 rotation[3], f32 src[3], f32 dst[3]);
}

#include <math.h>
#include <stdio.h>
#include <string.h>

// ---- what the game is doing right now ---------------------------------------

// The cutscene maps: scripted camera sweeps (the intro, Grunty's reveals, the endings). Those camera
// moves are authored for a flat screen and read as motion sickness in stereo, so they play on the flat
// panel instead - the same call the flying ports make for their level intros.
//
// WHICH maps those are is asked of the game rather than kept as a list here, because a list here can
// only ever describe the stock game. The map -> level table in core2/gc/section.c marks every cutscene
// map LEVEL_D_CUTSCENE, the game itself decides "this is a cutscene" from exactly that (gsworld.c's
// per-tick update and exit.c both do), and the port already reads it the same way for the cutscene
// aspect lock (Patches/CameraPatches.cpp). Going through map_getLevel picks up the romhack scene
// remap too (section.c consults port_getRomhackSceneRemap first), which is the part that matters: a
// romhack is free to hand one of the intro maps to a real level, and Torch emits a SCENE_REMAP entry
// for every map whose level differs from vanilla. With a hardcoded list that level played FLAT on the
// head-locked panel for its whole duration - "VR does not work with romhacks", exactly. The reverse
// now works as well: a hack's own new cutscene, on whatever map id, stays flat without anyone here
// hearing about it.
//
// For the stock game the two agree map for map, so this is not a behaviour change: the 23 maps the
// old switch listed are precisely the 23 the table marks LEVEL_D_CUTSCENE.
static bool VrGame_IsCutsceneMap(int map) {
    // The file select is the exception, and it is not a cutscene - it is the save screen, which the
    // table files under Spiral Mountain because that is the room it is set in. It is a menu either
    // way, so it belongs on the panel, and it stays an id because the boot path names it as one
    // (core1/init.c) no matter what a romhack does with its level.
    if (map == MAP_91_FILE_SELECT) {
        return true;
    }
    // map_getLevel dereferences the table entry, so an id that is not in the table would fault. It
    // cannot be one here: the game runs the same lookup on every map load (world_reset.c) and on
    // every world update tick (gsworld.c), and the caller has already required that the world is
    // drawing - so any map that reaches this line has been through map_getLevel many times over.
    return map_getLevel((enum map_e)map) == LEVEL_D_CUTSCENE;
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

// Is VR First Person the mode right now? Deliberately NOT port_vrFirstPerson_hidePlayer: that one
// carries the drive-tick freshness below, which goes stale the moment the game frames its own shot,
// and a suppression that lapses mid-dialogue would let the cartridge's own look mode back in at the
// worst possible moment. This asks the plain question and gets a stable answer.
extern "C" int port_vrFirstPersonMode(void) {
    return VrFp_Active() ? 1 : 0;
}

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

// Shortest signed distance between two headings, in (-180, 180]. sYaw is a free-running number -
// spin left for long enough and it is at -2000 - so the usual "+540 then fmod" trick is not safe
// on any difference that involves it: fmodf keeps the sign of its argument, and one negative
// result there is a view that jumps most of a turn. Every angle difference in this file goes
// through here so there is only one place for that to be got right.
static float VrWrapDeg(float deg) {
    deg = fmodf(deg + 180.0f, 360.0f);
    if (deg < 0.0f) {
        deg += 360.0f;
    }
    return deg - 180.0f;
}

// Standing on the ground with only the walk cycle moving his head. In these states the ONLY thing
// displacing the head vertically is the stride, so View Bob can take the vertical away without
// costing anything else. Everywhere else - crouching, attacking, landing, swimming, flying - the
// vertical IS the move, and must survive whatever View Bob is set to. Idles are included because
// Banjo breathes and shifts on the spot, which bobs the eye just as a walk does.
static bool VrFp_IsStrideState(s32 st) {
    return st == BS_1_IDLE || st == BS_2_WALK_SLOW || st == BS_3_WALK || st == BS_4_WALK_FAST ||
           st == BS_C_SKID || st == BS_1F_WALK_CREEP || st == BS_15_BTROT_IDLE ||
           st == BS_16_BTROT_WALK || st == BS_1B_WONDERWING_IDLE || st == BS_1C_WONDERWING_WALK ||
           st == BS_26_LONGLEG_IDLE || st == BS_27_LONGLEG_WALK || st == BS_3A_CARRY_IDLE ||
           st == BS_3B_CARRY_WALK;
}

// Swimming ON TOP of the water, where the waterline runs through Banjo and therefore through the
// eye. The dive states are deliberately NOT here: under the surface there is no line to stay above
// and the full stroke is what makes it read as swimming.
static bool VrFp_IsSurfaceSwim(s32 st) {
    return st == BS_2D_SWIM_IDLE || st == BS_2E_SWIM;
}

// TOUCHING DOWN, whatever carried you there. Every landing in the game funnels into one state: the
// jump, the fall, the fly-down, the barge, the rebound and the beak buster's recovery all set
// BS_20_LANDING the moment player_isStable goes true, and that state starts no animation of its
// own - it plays the TAIL of whatever was already running, which in every one of those is a crouch
// and a stand.
//
// The somersault is the one that does not go there. bsbflip_update never leaves for BS_20_LANDING;
// on touchdown it REPLAYS its own enter animation from 0.8566 as the landing (_bsbflip_802A2DC0)
// and the state stays BS_12_BFLIP for the ~0.32 s that runs. The somersault is bounded by its own
// sub-range end of 0.7866 and the landing replay starts past it - the same clock test the flip's
// view tilt already makes, and the one that killed the extra flip, so the file keeps one
// definition of "the flip has landed" instead of two that can drift apart.
// NOT covered: the ASSET_4C hold loop. If the flip's own rotation turns out to swing the eye
// through the whole move, this predicate is the wrong window and the cap belongs upstream.
static bool VrFp_IsLandingState(s32 st) {
    if (st == BS_20_LANDING || st == BS_4C_LANDING_IN_WATER) {
        return true;
    }
    if (st == BS_12_BFLIP) {
        void* anim = baanim_getAnimCtrlPtr();
        return anim != NULL && anctrl_getIndex(anim) == ASSET_4B_ANIM_BSBFLIP_ENTER &&
               anctrl_getAnimTimer(anim) > 0.7866f;
    }
    return false;
}

// ---- what just happened to the body -------------------------------------------
//
// Taking a hit and dying, in every skin Banjo can be wearing. Each transformation runs its own ow
// and die state, and a table is the honest way to say that - the game has no "is hurt" predicate
// to borrow. Three things ask these now (the swim follow, the view tilt and the haptics), so they
// sit above all of them.
static bool VrBs_IsHurt(s32 st) {
    return st == BS_E_OW || st == BS_3E_ANT_OW || st == BS_4D_PUMPKIN_OW || st == BS_63_CROC_OW ||
           st == BS_6C_WALRUS_OW || st == BS_7B_BTROT_OW || st == BS_7F_DIVE_OW ||
           st == BS_89_BEE_OW || st == BS_91_FLY_OW;
}

static bool VrBs_IsDeath(s32 st) {
    return st == BS_41_DIE || st == BS_43_ANT_DIE || st == BS_4E_PUMPKIN_DIE || st == BS_54_SWIM_DIE ||
           st == BS_64_CROC_DIE || st == BS_6D_WALRUS_DIE || st == BS_8A_BEE_DIE;
}

// The moves that turn the BODY OVER on purpose, and so drive the view themselves off their own
// animation clock rather than being an attitude the fall and the vertical clamp get to argue with.
// One list, because the tilt code asks the same question twice - which envelope is running, and
// which arm computes the angle - and the forward roll was missing from BOTH, so it turned the view
// by nothing at all. Two enumerations of one idea is how that happens.
static bool VrBs_IsBodyTurn(s32 st) {
    return st == BS_12_BFLIP || st == BS_F_BBUSTER || st == BS_31_ROLL;
}

// The body-turn set the tilt actually follows this tick. The forward roll carries its own player
// toggle (field request): a full forward somersault is the strongest thing the immersive cam ever
// does, and some players want the move without the view turning over. OFF is exactly the
// pre-roll-cam behavior for BS_31_ROLL alone; the flip jump and beak buster keep their arms. Both
// places that ask the body-turn question go through here, so the envelope family and the angle
// arm can never disagree about the roll.
static bool VrBs_IsBodyTurnLive(s32 st) {
    if (st == BS_31_ROLL && CVarGetInteger("gVRFpRollCam", 1) == 0) {
        return false;
    }
    return VrBs_IsBodyTurn(st);
}

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
    // THE 180 IS THE WHOLE POINT and it went missing once, so it is spelled out here. The model's
    // camera convention has the body facing the OPPOSITE way to the view angle, so turning a view
    // yaw into a body yaw means adding half a turn. Wrapped signed first so a free-running sYaw of
    // several thousand degrees still lands correctly, then lifted back into the game's 0..360.
    //
    // A tidy-up once rewrote this as `VrWrapDeg(viewYaw - 180) + 180`, which LOOKS like the same
    // expression and is in fact the identity: it returns viewYaw itself. Banjo then faced exactly
    // backwards everywhere this is called, which is every aim state - so in first person he swam
    // away from the stick and punched over his shoulder, both reported from the field. Two symptoms,
    // one sign. If this is ever touched again, check it against a table of angles and not by eye.
    float bodyYaw = VrWrapDeg(viewYaw + 180.0f);
    if (bodyYaw < 0.0f) {
        bodyYaw += 360.0f;
    }
    yaw_set(bodyYaw);
    yaw_setIdeal(bodyYaw);
}

// ---- the animated head ------------------------------------------------------
//
// IMMERSIVE CAM ANIMATING. Without this the eye is func_8028E9C4 mode 5 - the player's foot
// position plus a FIXED per-transform head height - and no animation can move a constant, so
// crouching, lunging, being slammed into the ground and the whole walk cycle all leave the view
// hanging at one height like a camera on a pole.
//
// Why it cannot simply read the drawn skeleton: the bone matrices and the model's ref points are
// PRODUCED by baModel_draw, and first person skips that draw to hide the bear. What survives the
// skipped draw is the animation CLOCK - baAnim_update ticks the player's AnimCtrl from the player
// update, not from the render - and the pose is a pure function of that clock. So the pose is
// sampled here and the head chain walked by hand, with the same arithmetic animMtxList_setBoned
// performs on the matrix stack (anim_defrag.c).
//
// THE ANCHOR TRICK, which is what makes it safe to leave on: the point carried down the chain is
// NOT the head pivot - a pivot does not move when the head rotates about it - but the point the
// game already calls the eye, model space (0, eyeHeight, 0). eyeHeight is handed in by the caller
// as the height mode 5 just added, so the per-transform table is read once, by the game, and never
// copied here. In a bind pose every local transform is the identity, the point comes back
// unchanged, and the offset is EXACTLY zero - see the increment form in the walk, written so that
// an identity hop cancels bit for bit rather than merely nearly.

// Banjo's head. Verified twice over: the Big Head / Small Head cheat scales exactly this bone
// (ba_anim.c), and the model's own bone table puts it at (0, 91.0, 1.84) with the beak bone just
// above it at height 100 - the same 100 the fixed eye constant adds.
static const s32 kHeadBoneId = 0x12;

// The pose buffer. Static rather than boneTransformList_new: this is read from the camera path
// every tick, and a block on the game heap can be moved by a defrag, which turns a cached pointer
// into a landmine. Same 0x6D bone capacity boneTransformList_new hands out, so the bounds this
// enforces are the game's own.
static BoneTransform sHeadPoseBones[0x6D];
static BoneTransformList sHeadPose = { sHeadPoseBones,
                                       (s32)(sizeof(sHeadPoseBones) / sizeof(sHeadPoseBones[0])) };

// Where the eye ends up once the head chain is applied, in BODY-LOCAL space, relative to where it
// sits in the bind pose. False means there is nothing to read - the player is not ordinary Banjo,
// or there is no animation, an asset that is a sentinel or simply not resident, a model with no
// skeleton, or a skeleton with no head bone - and the caller then adds nothing at all.
static bool VrFp_HeadBoneOffsetLocal(f32 eyeHeight, f32 outLocal[3]) {
    // ORDINARY BANJO ONLY, and this gate is the feature's safety catch. Every transformation is a
    // different creature on a skeleton of its own, and 0x12 is a low bone id that the termite, the
    // pumpkin, the walrus, the croc and the bee very likely all carry as something that is not a
    // head - so merely finding a bone 0x12 proves nothing. Driving the eye off another rig's bone
    // would put the view somewhere meaningless. For those forms the fixed per-transform head height
    // is already the right answer, which is exactly what func_8028E9C4 mode 5 hands the caller.
    if (bsStoredState_getTransformation() != TRANSFORM_1_BANJO) {
        return false;
    }
    void* animCtrl = baanim_getAnimCtrlPtr();
    if (animCtrl == NULL) {
        return false;
    }
    AnimationFile* animFile = animBinCache_get(anctrl_getIndex(animCtrl));
    if (animFile == NULL) {
        return false;
    }
    BKModelBin* bin = baModel_getModelBin();
    if (bin == NULL) {
        return false;
    }
    BKAnimationList* skel = modelbin_getAnimationList(bin);
    if (skel == NULL || skel->count <= 0) {
        return false;
    }
    // The draw scales the WHOLE model by this (func_80252AF0 multiplies every model-space point by
    // it before the rotations), so the bone chain below and the world-space eye height handed in
    // are not in the same units until it is applied - once on the way in, once on the way back out.
    // It is 1.0 for ordinary Banjo today, and the transform gate above keeps the pumpkin's 0.3 out
    // of here entirely, but a head offset that quietly disagreed with the drawn body would be a
    // nasty thing to leave lying around. A model scaled to nothing has no head worth reading.
    const f32 modelScale = baModel_getScale();
    if (modelScale <= 0.0f) {
        return false;
    }

    // Find the head's ENTRY in this skeleton. The table is ordered by entry, not by bone id, and
    // Banjo has more than one model bin (the bear alone, and the bear carrying the bird), so there
    // is no promise the head sits at the same entry in the one currently loaded.
    s32 head = -1;
    for (s32 i = 0; i < skel->count; i++) {
        if (skel->animations[i].bone_id == kHeadBoneId) {
            head = i;
            break;
        }
    }
    if (head < 0) {
        return false;
    }

    // Sample the pose. RESET first: getBoneTransformList only writes the bones the asset actually
    // animates, so everything it leaves alone has to already hold the identity a bind pose means.
    // The (index, timer) pair is exactly the pair anim_update feeds it (anim_buffer.c), and the
    // timer is already absolute across the whole asset - anctrl_update folds the sub-range in, so
    // applying the sub-range a second time here would sample the wrong frame. Mid-blend the game
    // interpolates this pose against the outgoing one and only the incoming half is read here; a
    // few ticks of a slightly early pose disappears under the easing at the call site.
    boneTransformList_reset(&sHeadPose);
    animationFile_getBoneTransformList(animFile, anctrl_getAnimTimer(animCtrl), &sHeadPose);

    // The anchor, in MODEL space - hence the divide, which undoes the scale the draw would apply.
    const f32 start[3] = { 0.0f, eyeHeight / modelScale, 0.0f };
    f32 p[3] = { start[0], start[1], start[2] };

    // Child to root, the same arithmetic animMtxList_setBoned performs one bone at a time:
    // p' = ((p - t) * S) * R + t + u*d. Written as an INCREMENT on p so an identity hop cancels
    // exactly: v - v is zero in floating point, while (p - t) + t is only nearly p.
    s32 at = head;
    for (s32 hop = 0; at >= 0 && at < skel->count && hop < skel->count; hop++) {
        const BKAnimation* bone = &skel->animations[at];
        // The pose list is indexed by BONE id, and it is a fixed 0x6D like the game's own. A
        // skeleton carrying a higher id would read past the end of the buffer, so it stands down
        // instead - the eye keeps the fixed height, which is exactly the old behaviour.
        if (bone->bone_id < 0 || bone->bone_id >= sHeadPose.count) {
            return false;
        }
        f32 quat[4], scale[3], delta[3];
        func_8033A5B8(&sHeadPose, bone->bone_id, quat, scale, delta);

        const f32 v[3] = { p[0] - bone->translation[0], p[1] - bone->translation[1],
                           p[2] - bone->translation[2] };
        f32 w[3] = { v[0] * scale[0], v[1] * scale[1], v[2] * scale[2] };
        if (!vec4f_isZero(quat)) { // "isZero" reads IDENTITY here - the same test the draw makes
            f32 R[3][3];
            func_80345274(quat, R);
            const f32 s0 = w[0], s1 = w[1], s2 = w[2];
            w[0] = s0 * R[0][0] + s1 * R[1][0] + s2 * R[2][0];
            w[1] = s0 * R[0][1] + s1 * R[1][1] + s2 * R[2][1];
            w[2] = s0 * R[0][2] + s1 * R[1][2] + s2 * R[2][2];
        }
        // Translation channels are scaled by the skeleton's own factor (10.0 on both Banjo models)
        // before they mean anything, exactly as the draw scales them.
        p[0] += (w[0] - v[0]) + skel->unk0 * delta[0];
        p[1] += (w[1] - v[1]) + skel->unk0 * delta[1];
        p[2] += (w[2] - v[2]) + skel->unk0 * delta[2];

        at = bone->mtx_id; // the parent's entry in the same table; -1 is the root and ends the walk
    }

    // Back out of model space, so the caller gets a distance in the same units the world is in and
    // the clamp downstream measures a real one.
    outLocal[0] = (p[0] - start[0]) * modelScale;
    outLocal[1] = (p[1] - start[1]) * modelScale;
    outLocal[2] = (p[2] - start[2]) * modelScale;
    return true;
}

// The APPLIED head offset, held in BODY-LOCAL space and eased there.
//
// Local, not world, and this is the whole design. bs_setState calls port_vrFpFaceViewYaw as an aim
// state begins (barge, claw, peck, both eggs), which snaps the body yaw to face the view - up to
// 180 degrees in ONE tick. An offset eased in WORLD space cannot tell that turn from a pose change:
// the old world vector is still pointing where the body used to face, so it takes several ticks to
// swing round and the eye slides roughly a third of a metre sideways while it does. Held in the
// body's own frame the turn costs nothing at all - the offset rides round rigidly with him, which
// is what a head bolted to a body actually does - and the filter only ever sees genuine pose change.
static f32 sHeadOffLocal[3] = { 0.0f, 0.0f, 0.0f };

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

    // IMMERSIVE CAM ANIMATING: move the eye to where his HEAD actually is this tick, so a crouch
    // ducks the view, an attack lunges and dips it, and the stride carries a real bob - all of it
    // out of the animation the body is already playing, none of it invented here.
    {
        f32 target[3] = { 0.0f, 0.0f, 0.0f };
        // Liveness before any game global is believed (the round-31 law): the animation controller
        // and the model bin are reset-only globals like every other, so a screen with no player
        // behind it must not be able to pose an eye.
        if (CVarGetInteger("gVRFpImmersive", 1) != 0 && BsStateIsLive()) {
            f32 foot[3];
            playerPosition_get(foot);
            // eye[1] - foot[1] IS the game's per-transform head height, since mode 5 is exactly
            // "foot position plus that height". Passing it through means the transformation table
            // has one home and this file never holds a second copy of it. The helper stands down
            // for every transformation other than ordinary Banjo and returns false, which leaves
            // that fixed height alone - the right answer for a termite or a pumpkin.
            if (!VrFp_HeadBoneOffsetLocal(eye[1] - foot[1], target)) {
                target[0] = target[1] = target[2] = 0.0f;
            }
            // VIEW BOB OWNS THE WALKING BOB, whichever code produces it. A player turned View Bob
            // off, still bobbed while walking, and could only stop it by turning off Immersive
            // Camera outright - losing the crouch, the attacks and the lean along with it. He was
            // right and the switch was lying: it gated an older synthetic bob further down this
            // function, while Banjo's REAL walk animation was bobbing his head through the bone
            // chain above and answering to nothing.
            //
            // Only the vertical, and only while walking. Body-local Y is the stride's up and down
            // (X is the pitch axis, Z is forward); zeroing it in these states leaves the sway and
            // leaves every other state untouched, so a crouch still ducks, an attack still lunges
            // and a landing still dips. Those are what people mean by the immersive camera, and
            // none of them is what anybody calls view bob.
            //
            // Ordered before the clamp below on purpose: that clamp scales the whole vector by its
            // own length, so a Y removed afterwards would still have decided how far X and Z moved.
            if (CVarGetInteger("gVRFpViewBob", 0) == 0 && VrFp_IsStrideState(bs_getState())) {
                target[1] = 0.0f;
            }
            // AT THE SURFACE, YOUR HEAD STAYS ABOVE IT. This one is not a preference and is not
            // gated on View Bob. Banjo's surface swim animation rocks his whole body through the
            // waterline - fine to watch from behind him, and from inside his head it ducked the
            // eye under the water on every stroke and simply held it there while swimming forward.
            // The reporter's words were that you never feel like you are swimming on the surface,
            // because the view is always beneath it. Losing the vertical of the stroke costs the
            // motion almost nothing: the roll and the surge forward are what read as swimming, and
            // both are still here.
            if (VrFp_IsSurfaceSwim(bs_getState())) {
                target[1] = 0.0f;
            }
            // A LANDING IS NOT A CROUCH, and until now the head walk could not tell them apart.
            // Landing animations duck Banjo's head hard on impact, and the somersault's is the
            // worst: its replay opens at 0.8566 with the body already deep in the crouch, so the
            // target arrives as a STEP rather than a ramp (the game cross-fades the drawn bear
            // into that pose over 0.2 s, but the sampler above reads the incoming pose at full
            // weight from the first tick). The ease then delivers three quarters of it in four
            // ticks: down hard, and back up as he stands. "The camera jolts down into the ground
            // and back up" on landing the flip - reported in those words.
            //
            // ONE rule, not two mechanisms: the dip is floored, tighter while landing. 30 during a
            // landing sits comfortably under the 47-unit crouch drop this feature exists for, so a
            // landing reads as a smaller duck than a deliberate crouch, which is exactly what it
            // is. 50 everywhere else bounds anything the sampler meets that is deeper than the
            // deliberate crouch - the tucked somersault, the slam - while leaving the crouch
            // itself whole. Vertical only, downward only, and BEFORE the magnitude clamp below for
            // the same reason the View Bob rule is: that clamp scales the whole vector, so a Y
            // trimmed after it would still have decided how far X and Z moved. The forward lunge
            // is what makes an impact read as an impact, and it survives whole - trimming by
            // length would take the lunge away along with the drop.
            {
                const f32 kDipCap = VrFp_IsLandingState(bs_getState()) ? 30.0f : 50.0f;
                if (target[1] < -kDipCap) {
                    target[1] = -kDipCap;
                }
            }
            // CLAMP THE MAGNITUDE, direction kept. A full crouch measures about 57 units (47 down,
            // 32 forward) and has to survive whole - it IS the feature. The beak buster swings the
            // head 88 units, most of a metre at life size, and applied raw that is a plunge the
            // player never made with their own neck. Scaling the whole vector rather than trimming
            // an axis keeps a clamped move dipping and lunging the way the body does, just less far.
            const f32 kMaxOffset = 60.0f;
            const float len =
                sqrtf(target[0] * target[0] + target[1] * target[1] + target[2] * target[2]);
            if (len > kMaxOffset) {
                const float k = kMaxOffset / len;
                target[0] *= k;
                target[1] *= k;
                target[2] *= k;
            }
        }
        // Eased ONCE PER GAME TICK, which is enough here and is not the flip cam's situation. The
        // flip cam rotates the eye's own view matrix, built fresh in vr.cpp every headset frame
        // out of the live pose - nothing on the game side can ever smooth that, so it has to ease
        // where the frames are. This offset instead moves the GAME's camera position, and the
        // camera position is subtracted into every object's modelview (func_80252AF0), which
        // mlMtxApply records for frame interpolation - the same machinery that already carries
        // ordinary walking smoothly from a 30 Hz tick to a 144 Hz headset. Easing it a second time
        // per frame would only add lag to a value that is already interpolated.
        //
        // No reset when the override stops driving (a scripted shot, a loading zone, pause). The
        // target is recomputed from the CURRENT pose every tick, so a resumed eye is only ever
        // wrong if the pose changed while the game held the camera - and that resumption is a hard
        // cut anyway. Pause is the case that would have been hurt by a reset: the pose cannot
        // change there, the held value is still exactly right, and starting again from zero would
        // have lifted the view out of a crouch on every unpause.
        const float kEase = 0.30f;
        for (int i = 0; i < 3; i++) {
            sHeadOffLocal[i] += (target[i] - sHeadOffLocal[i]) * kEase;
        }
        // Only NOW does it become a world offset, through ALL THREE of the angles the model itself
        // is drawn with and in the draw's own order. func_80252AF0 pushes roll, then pitch, then
        // yaw, and func_80256F44 is the vector form of exactly that sequence, so the eye can never
        // disagree with where the bear is drawn.
        //
        // Pitch and roll are not decoration. Diving swings them across most of a circle (bSwim.c
        // drives roll and pitch through 275..360 and 0..85) and flight does the same, and a
        // 60-unit offset turned by yaw alone would sit the better part of a metre from the real
        // head at full pitch. All three come from the live accessors, never from baModelPitch /
        // baModelRoll: those are written inside the draw that first person skips, so they hold the
        // angles the bear had the last time he was visible.
        f32 modelRot[3] = { pitch_get(), baModel_computeModelYaw(), roll_get() };
        f32 base[3] = { 0.0f, 0.0f, 0.0f };
        f32 world[3];
        func_80256F44(base, modelRot, sHeadOffLocal, world);
        position[0] += world[0];
        position[1] += world[1];
        position[2] += world[2];
    }

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

    // FLOOR CLAMP, after every Y modifier INCLUDING the animated head above - which is the order
    // that matters: a crouch has to be allowed to lower the eye, and then the lowered eye still has
    // to sit above the ground. The game legitimately drives the player POSITION under the floor in
    // some states, whatever the head is doing: the beak buster slam buries the body on impact by
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

    // A NEW ROOM MEANS A NEW FACING, and the view base has to follow it. The base is deliberately
    // captured once and held - that is what makes it world-stable - but a door, a warp pad or a
    // level entry re-faces BANJO while first person never disengages, so the captured yaw was still
    // pointing wherever you happened to look in the previous room. Every exit came out facing the
    // wrong way, reported as exactly that, in all areas. The map changing is the game's own
    // statement that you have been re-placed, so the base re-captures from his fresh facing - the
    // same thing entering first person does - rather than trusting a yaw from another room.
    {
        static s32 sBaseMap = -1;
        const s32 mapNow = (s32)gsworld_getMap();
        if (mapNow != sBaseMap) {
            sBaseMap = mapNow;
            sBaseValid = false;
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
    // steering, the view IS his heading. That stays inside the comfort law: the view only ever
    // moves while the player is commanding the turn themselves, dead-stops with the stick, and
    // never follows on land - the world-stable base is untouched everywhere else.
    //
    // Second edition, and the shape is the sm64 port's rather than ours. That camera does not
    // transfer a capped DELTA from body to view; it re-derives the view from the body every tick
    // (yaw = faceAngle + 180) and lets the game's own turn integrator be the whole comfort
    // budget. Three things follow from that, and all three were problems here:
    //   - A cap cannot bite, because there is nothing to clip. The follow only runs while the
    //     stick is held, so every degree a cap ever clipped was a degree the view never got back:
    //     a hard 180 spin ended with the eye pointing somewhere he was not. That was the whole
    //     "the view lags behind" complaint, and no cap value fixes it - only not having one does.
    //   - It self-corrects. A tick that goes wrong for any reason is gone by the next one,
    //     because the answer is recomputed rather than accumulated.
    //   - The easing is already done for us. bSwim walks his yaw IDEAL by at most 2.4 to 4.3
    //     degrees a tick and yaw_update then chases that ideal at a bounded 90 to 250 deg/s, so
    //     what reaches the view is a rate-limited S-curve the moment the stick moves and a decay
    //     to a stop the moment it is released. Re-smoothing it here would only add lag.
    // The one thing sm64 does not have to solve, because its look is applied before the fix-up
    // and simply overwritten: the player's own look must survive a steering tick. So what is
    // re-derived is body heading PLUS however far the player has looked off it, measured fresh
    // each tick. Look and steer at once and both apply.
    //
    // And the coupling runs BOTH ways underwater, which is the part that makes it comfortable
    // rather than merely responsive. Stick held, the view follows the body; stick released, the
    // BODY follows the view. There is then no moment where the two disagree, so nothing has to
    // reconverge when steering stops - and swimming where you look costs no view motion at all,
    // because the body is what moves.
    {
        static float sPrevPlayerYaw = 0.0f;
        static bool sPrevYawValid = false;
        f32 pr[3];
        player_getRotation(pr);
        const float dyaw = VrWrapDeg(pr[1] - sPrevPlayerYaw);
        // UNDERWATER only. The dive-family state check alone was NOT enough: the surface strokes
        // run THROUGH genuine dive states (bsbdiveb/bsswim_divea eject to SWIM_IDLE only via the
        // near-surface check, so every B stroke at the surface is a few ticks of BS_2C_DIVE_B),
        // which engaged the follow in bursts while steering - "the left stick auto-turns at the
        // surface", reported twice. The state machine's OWN boundary predicate (func_802A73BC,
        // the same call it uses to leave the dive states near the surface) is the missing term:
        // follow only when genuinely submerged AWAY from the surface. Liveness-stamped like every
        // bs-state consumer (round 31).
        // FLYING wants the same idea but NOT the same numbers, and reusing swimming's was why the
        // first attempt did nothing. Flight yaw runs bounded at up to 500 deg/s (bFly.c, R held),
        // which is well past the 15 degree window swimming used to carry, so that window threw away
        // the whole banking turn - the follow never engaged at all during the exact manoeuvre it
        // exists for. And BS_23_FLY_ENTER is NOT in the flight family, so the follow was dead for
        // the whole entry.
        const s32 bsNow = bs_getState();
        const bool swimFollow = player_inWater() && bsbswim_inSet(bsNow) && !func_802A73BC();
        const bool flyFollow = bsbfly_inSet(bsNow) || bsNow == BS_23_FLY_ENTER;
        // BEING HIT IS NOT STEERING, on either side of the coupling. The ow and die states re-face
        // Banjo instantly, away from whatever hit him (bsbswim_ow_init and the flight knockback
        // both do yaw_setIdeal then yaw_applyIdeal in a single tick), and BS_7F_DIVE_OW and
        // BS_54_SWIM_DIE are both members of bsbswim_inSet, so without this the whole block is
        // live through a hit. The window below vetoes a BIG re-face, but a hit taken from roughly
        // behind produces a small one that slips under it and gets handed to the view verbatim -
        // the eye turns up to 30 degrees on the hit frame with no input at all. Standing the
        // follow down entirely for the duration is the honest answer: nobody is steering while
        // they are being knocked about, so there is nothing to follow.
        const bool struck = VrBs_IsHurt(bsNow) || VrBs_IsDeath(bsNow);
        if (sPrevYawValid && BsStateIsLive() && (swimFollow || flyFollow) && !struck &&
            CVarGetInteger("gVRFpSwimFollow", 1) != 0) {
            f32 move[2];
            controller_getJoystick(0, move);
            // Both moves yaw on the stick's X axis and nothing else: underwater X IS the steer
            // (bSwim.c walks the yaw ideal by bastick_getX every tick, and Y is pitch), and flight
            // banks on X. So X alone answers "is the player turning right now".
            const float steer = fabsf(move[0]);
            // How far off centre counts as steering. Flight keeps 0.35, since a bank is a firm
            // push. Swimming drops to a whisker off centre, because the game gives the swim turn no
            // deadzone of its own: the turn rate is straight-line proportional to X, so even half a
            // stick still swings Banjo at ~45 deg/s while the old 0.35 gate left the view parked
            // through the whole gentle course correction ("sometimes Banjo turns and it doesn't
            // follow"). The pad has already zeroed everything inside the hardware deadzone before
            // we see it, so any X still standing IS a commanded turn; 0.05 sits a couple of raw
            // counts above that edge so stick jitter on its own can never start the view moving.
            const float gate = flyFollow ? 0.35f : 0.05f;
            // Window = "is this a steer or a snap", and it is a VETO rather than a clamp: the only
            // job left for it is to spot a jump the turn integrator cannot have produced. Underwater
            // the fastest that integrator runs is the dive entry's 500 deg/s, which over the longest
            // tick the port allows (1/20 s on a slow frame) is 25 degrees, so 30 admits every real
            // turn. Past that it did not come from steering at all: taking a hit underwater re-faces
            // Banjo instantly away from what hit him (yaw_applyIdeal in the ow state, up to 180
            // degrees in one tick), and a load zone can hand him any angle. A vetoed tick leaves
            // the view exactly where it was and lets the snap become part of how far you are
            // looking off his nose, which is what world-stable MEANS through a teleport: your head
            // did not move, so your view does not. Steering after it still tracks him one to one,
            // from wherever the two now stand. A clamp would instead have dragged the eye part way
            // into a turn Banjo never made.
            const float window = flyFollow ? 45.0f : 30.0f;
            if (steer > gate) {
                if (dyaw > -window && dyaw < window) {
                    // How far the player has looked off his nose, measured against where he was
                    // pointing when this was last answered. Everything the look block added since
                    // then is in here, so a stick-and-steer at the same time keeps both.
                    const float look = VrWrapDeg(sYaw - (sPrevPlayerYaw + 180.0f));
                    // The view IS his heading plus that look. Not a step toward it - the answer
                    // itself, recomputed, so there is nothing to lose and nothing to catch up on.
                    sYaw = pr[1] + 180.0f + look;
                }
            } else if (swimFollow) {
                // Stick centred and genuinely submerged: the body takes the view's heading instead,
                // so drifting and looking around leaves the two agreeing and the next stroke goes
                // where you are looking. Costs the player no view motion whatsoever - it is the
                // body that turns - and it is the same call the aim states already make, so the
                // yaw convention lives in exactly one place. Not for FLIGHT: the flight model owns
                // its own yaw hard (its own bounded velocity and its own targets) and writing over
                // that would be two hands on the stick. Not while struck either, which the gate on
                // the whole block above now covers for both directions of the coupling.
                port_vrFpFaceViewYaw();
            }
        }
        // ON THE SURFACE the same coupling runs in ONE direction only: body follows view, never
        // view follows body. Surface swimming steers like walking - the stick picks a direction
        // RELATIVE TO THE CAMERA and Banjo turns himself to face it (swim.c runs the stick-zone
        // system, not the dive's turn-rate-on-X) - so the view is the reference frame there, and a
        // view that followed the body would be chasing its own tail. That is why the follow above
        // gates on genuinely-submerged, and why shallow surface-only water (Gobi's pools, reported)
        // had no coupling at all: look somewhere, stroke, and Banjo set off wherever he last
        // pointed. Now a centred stick at the surface turns HIM to face where you are looking, so
        // the next stroke goes there. Zero view motion - the comfort law is untouched.
        //
        // Idle only (BS_2E means the stick is in a zone, and then the stick system owns his yaw),
        // fully centred stick on BOTH axes (X alone is the dive's steer test; at the surface a
        // diagonal push means a direction, and facing the view would fight it), never while the
        // game aims him at something (balookat), and never while struck.
        if (sPrevYawValid && BsStateIsLive() && bs_getState() == BS_2D_SWIM_IDLE &&
            player_inWater() && !struck && balookat_getState() == 0 &&
            CVarGetInteger("gVRFpSwimFollow", 1) != 0) {
            f32 mv[2];
            controller_getJoystick(0, mv);
            if (fabsf(mv[0]) < 0.05f && fabsf(mv[1]) < 0.05f) {
                port_vrFpFaceViewYaw();
            }
        }
        // Re-read AFTER the body may have been turned above, or the next tick would measure our own
        // write as a turn Banjo made and hand the view a step it never earned.
        player_getRotation(pr);
        sPrevPlayerYaw = pr[1];
        sPrevYawValid = true;
    }

    f32 rs[2];
    controller_getRightStick(0, rs);
    const float dt = time_getDelta();
    const float kDead = 0.15f;
    const float lookSpeed = CVarGetFloat("gVRFpLookSpeed", 220.0f);
    // INVERT LOOK. Both axes read the port's own camera invert (Enhancements -> Camera), the same
    // pair the game's own C-up first person already obeys: a player who inverts Y means every first
    // person the port has, not just the one the cartridge shipped with. Vertical had NO invert here
    // at all, which is why ticking it did nothing in the headset.
    // It arrives as a plain -1 multiplier laid ON TOP of the signs below. The signs themselves were
    // settled by testing in the headset (see the note on the yaw line) and inverting must never be
    // done by rewriting one of them - the shipped default and the invert are separate facts, and
    // folding them together is how a build ships inverted for everybody.
    const float invertX = port_cameraInvertXSign();
    const float invertY = port_cameraInvertYSign();

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

    // Stick up looks UP by default here, which is the OPPOSITE arithmetic sign to the third-person
    // orbit (where stick up lifts the camera and tilts the view down). Both are the right default
    // for their own camera, so this axis inverts by multiplying its own sign - never by copying the
    // orbit's expression across.
    if (CVarGetInteger("gVRFpVerticalLook", 1) != 0) {
        sPitch += curve(rs[1], kDead) * lookSpeed * 0.625f * invertY * dt;
    } else {
        sPitch = 0.0f;
    }

    // Mouse look, raw: counts to degrees the tick they happen, through the same invert flags as the
    // stick. The stick keeps its comfort easing above; the mouse gets NONE - 1:1 position response
    // is the whole sm64coopdx / SRB2 feel, and any filter here reads as lag under the hand.
    {
        float mdx, mdy;
        port_vrMouseDelta_take(&mdx, &mdy);
        if (mdx != 0.0f || mdy != 0.0f) {
            const float mouseSens = CVarGetFloat("gVRMouseSens", 0.10f);
            sYaw -= mdx * mouseSens * invertX; // mouse right looks right (same minus law as the stick)
            if (CVarGetInteger("gVRFpVerticalLook", 1) != 0) {
                sPitch += -mdy * mouseSens * invertY; // mouse up looks up
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
//   right grip        Z - CROUCH (squeeze to duck). Third Z source with the left trigger and X,
//                     and the only one that survives pause. Jump is the A face button.
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
    // NEVER at or near the surface, because up there A is the JUMP OUT OF WATER (swim.c: the surface
    // states take bakey_pressed(BUTTON_A) straight to BS_5_JUMP). The dive-family check above is not
    // enough on its own: a surface stroke passes THROUGH a real dive state for a few ticks, so an A
    // press timed inside one of those ticks got swapped to B and came out as a swim stroke instead
    // of a jump - "the jump is too weak" and, in a pool walled on all sides, no way out at all.
    // func_80294524 is the game's own "held at the surface" flag (climbsurface.c sets it only in
    // that case) and func_802A73BC is its near-surface band, so the whole exit zone keeps stock
    // buttons and the swap only ever applies where you are genuinely swimming under.
    if (func_80294524() || func_802A73BC()) {
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
            static s32 sPrevMode = -1, sPrevBs = -1, sPrevCam = -1;
            static int sPrevLive = -1, sPrevWater = -1, sPrevFraming = -1, sPrevSurf = -1;
            const s32 m = getGameMode(), b = bs_getState(), cam = ncDynamicCamera_getState();
            const int live = BsStateIsLive() ? 1 : 0, water = player_inWater() ? 1 : 0;
            const int framing = port_vrFirstPerson_hidePlayer();
            // surf packs the two water-boundary predicates: bit 0 = near the surface band
            // (func_802A73BC, the follow's stand-down), bit 1 = held at the surface where A jumps
            // (func_80294524). "The swim follow does not work in THIS pool" is answered by whether
            // a swim there ever shows surf=0 - shallow water never does, and the follow is
            // designed to stand down at the surface because surface steering is camera-relative.
            const int surf = (func_802A73BC() ? 1 : 0) | (func_80294524() ? 2 : 0);
            if (m != sPrevMode || b != sPrevBs || live != sPrevLive || water != sPrevWater || cam != sPrevCam ||
                framing != sPrevFraming || surf != sPrevSurf) {
                // cam = which camera owns the shot, fpFraming = whether our First Person offsets apply.
                // A scripted shot shows as a cam change with fpFraming dropping to 0.
                printf("[VR] gameMode=%d bsState=0x%02X bsLive=%d inWater=%d surf=%d cam=%d fpFraming=%d\n",
                       (int)m, (unsigned)b, live, water, surf, (int)cam, framing);
                fflush(stdout); // survives a killed process - an observability print that can vanish is no seam
                sPrevMode = m; sPrevBs = b; sPrevLive = live; sPrevWater = water; sPrevCam = cam;
                sPrevFraming = framing; sPrevSurf = surf;
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
    // Right grip CROUCHES. Squeezing the hand you already hold the world with to duck reads as the
    // body doing it, which is the whole point of first person here. It is the third Z source
    // alongside the left trigger and X, and the only one that also works while paused. The cost,
    // stated plainly: jump was doubled on this grip and is now the A face button alone on the
    // controllers that have one. Vive wands and WMR have no A face button, so a player on those
    // profiles jumps with the gamepad or rebinds - flagged rather than silently traded away.
    if (vb & VR_BTN_RGRIP)    { btn |= Z_TRIG; }
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

// ---- haptics ------------------------------------------------------------------
//
// What the world does to you, felt in the hands. Deliberately a SHORT list: a controller that
// buzzes at everything stops meaning anything, so this fires only where the body takes or delivers
// a real impact, and each event gets its own weight and length so they are told apart by feel
// rather than by being told apart at all.
//
//   land            a tap, scaled by how hard you came down - a hop is barely there
//   hard landing    fall damage. The heaviest thing here, and it should be
//   hurt            a solid knock, medium length
//   death           long and heavy, once
//   attack lands    a short bright tick, so a connected swipe reads differently from a whiff
//
// Not gated on First Person or on the immersive cam: the hands are the hands in every view mode,
// including the tabletop diorama and the flat theater screen. Gated only on VR being live and on
// the player's own HAPTICS switch (gVRHaptics, on by default).
static void VrHaptic(float strength, float seconds) {
    if (!vr_is_active() || CVarGetInteger("gVRHaptics", 1) == 0) {
        return;
    }
    vr_controller_rumble(strength, seconds);
}

// An attack of Banjo's just landed on something. Called from the collision resolve, which is the
// only place that knows a hitbox MATCHED rather than merely being live - see hitboxdata.c.
//
// Rate limited, because the resolve runs on OVERLAP and not on impact: one buster can match half a
// dozen actors in a tick, and rolling into something that does not die keeps matching every tick
// after. A blow is one tick in the hand however many things it caught, so a few ticks of quiet
// after each one is what makes it read as a hit rather than as a texture.
extern "C" void port_vrHapticAttackHit(void) {
    // Same gate every other bs consumer in this file carries, and it is not decoration here: the
    // ATTRACT DEMO replays a recorded pad into real gameplay behind the title screen, hitboxes and
    // all, in a mode that is by definition not GAME_MODE_3_NORMAL. Without this the controllers
    // tick in your hands while demo-Banjo rolls into a Gruntling and nobody is playing.
    if (!BsStateIsLive() || getGameMode() != GAME_MODE_3_NORMAL) {
        return;
    }
    static int sLastTick = -100;
    if (sVrTick - sLastTick < 6) {
        return;
    }
    sLastTick = sVrTick;
    VrHaptic(0.40f, 0.045f);
}

// Everything else is a state edge or a physics edge, so it is read here rather than wired into the
// game: bs_getState() changes exactly once per event, and the player machine's own liveness stamp
// keeps a stale state on a menu screen from firing anything.
static void VrGame_PollHaptics(void) {
    static int sPrevKind = -1;
    static bool sPrevStable = true;
    static float sAirVy = 0.0f;

    if (!vr_is_active() || !BsStateIsLive() || getGameMode() != GAME_MODE_3_NORMAL) {
        sPrevKind = -1;
        sPrevStable = true;
        sAirVy = 0.0f;
        return;
    }

    const s32 st = bs_getState();
    const bool hurts = VrBs_IsDeath(st) || st == BS_72_SPLAT || VrBs_IsHurt(st);
    // ONE EVENT, ONE BUZZ, however many states the game spends it in. Keyed on which KIND of thing
    // happened rather than on the state id, because a death over water runs BS_41_DIE and then
    // hands off to BS_54_SWIM_DIE partway through and both are deaths - on the raw id that was two
    // heavy buzzes for one death, a couple of seconds apart, against a spec line that says once.
    const int kind = VrBs_IsDeath(st)      ? 1
                     : (st == BS_72_SPLAT) ? 2
                     : VrBs_IsHurt(st)     ? 3
                                           : 0;
    if (sPrevKind >= 0 && kind != sPrevKind) {
        if (kind == 1) {
            VrHaptic(0.85f, 0.35f);
        } else if (kind == 2) {
            VrHaptic(0.95f, 0.20f); // fall damage: the one that is supposed to hurt
        } else if (kind == 3) {
            VrHaptic(0.60f, 0.11f);
        }
    }
    sPrevKind = kind;

    // LANDING. The last vertical speed while airborne is what the impact was worth - by the time
    // the feet are down the physics has already zeroed it, so it has to be caught on the way in.
    // A landing that HURT reaches the ground and enters BS_72_SPLAT on the same tick, and a later
    // call replaces an earlier one outright - so the light tap has to stand aside there, or the
    // heaviest event in the game would be overwritten by the smallest one a few lines on.
    //
    // WATER IS NOT AIR. player_isStable() is false the whole time you are swimming, so treating it
    // as "airborne" armed the tap with the swim descent's own speed (bSwim.c sinks at up to 400)
    // and then fired it the moment you climbed out - a phantom landing on every water exit. In
    // water there is nothing to land from, so it counts as already down: no speed is gathered and
    // no edge is left behind. Jumping out is unaffected, because the arc after you leave the water
    // is genuinely airborne and gets measured normally.
    const bool stable = player_isStable() || player_inWater();
    if (!stable) {
        sAirVy = baphysics_get_vertical_velocity();
    } else if (!sPrevStable && !hurts) {
        // Nothing for stepping off a kerb; a full jump is a firm tap and terminal velocity is the
        // most a plain landing ever gets. The hurting landings are BS_72_SPLAT above and they run
        // on their own weight, so this stays deliberately light.
        const float speed = (sAirVy < 0.0f) ? -sAirVy : 0.0f;
        if (speed > 300.0f) {
            float k = (speed - 300.0f) / 1700.0f;
            if (k > 1.0f) {
                k = 1.0f;
            }
            VrHaptic(0.15f + 0.35f * k, 0.05f);
        }
        sAirVy = 0.0f;
    }
    sPrevStable = stable;
}

// ---- the first person view tilt ---------------------------------------------
//
// ONE angle covers the flip cam, the beak buster, taking a hit, dying, and the long fall out of a
// level (radians, positive = nose down). It is recomputed FROM SCRATCH every tick out of what the
// body is doing right now: an animation's own progress wherever there is one to read, and a
// clamped elapsed-time envelope only where the animation loops and therefore cannot say. Nothing
// here integrates a velocity, so nothing can drift or overshoot - and every envelope is written to
// be back at zero (or at a whole revolution, which IS upright) by the time the state that owns it
// is allowed to end, so a move that finishes needs no "completing" afterwards.
//
// Peak angles come from the sm64 port's own table, converted out of its s16 units (0x8000 = 180
// degrees): hurt 0x2000, death 0x2800, fall 0x3000. Those are numbers a lot of people have already
// spent a lot of hours behind, which is worth more here than anything freshly guessed.
static const float kTiltHurtDeg  = -45.0f;  // a hit throws the head BACK, so the view pitches up
static const float kTiltSplatDeg = 33.75f;  // fall damage lands beak first, so this one goes down
static const float kTiltDeathDeg = -56.25f; // flat on your back looking at the sky, and it holds
static const float kTiltFallDeg  = -67.5f;  // the long fall: your eyes go to what you just left

static float VrFlipAngleRad(void) {
    const float kDeg2Rad = 0.01745329252f;
    const float kPi = 3.14159265359f;

    // Elapsed-time envelopes and the fall ramp. Both are reset whenever the tilt stands down, so a
    // screen with no player behind it can never resume one mid-swing.
    static float sEnvT = 0.0f;     // seconds into the current hurt / splat / death envelope
    static float sFallRamp = 0.0f; // 0..1, how far the long-fall look-up has come in
    static int sPrevFam = 0;       // which envelope shape was running last tick (see below)

    // IMMERSIVE CAM is the enabler. "Your head is Banjo's head" and "your view turns when his
    // body turns" are one idea, so they are one switch - a separate FLIP CAM row was a second
    // knob for the same promise, and the one people actually found was this one.
    if (vr_get_view_mode() != VR_VIEW_FIRST_PERSON || CVarGetInteger("gVRFpImmersive", 1) == 0 ||
        !BsStateIsLive() || getGameMode() != GAME_MODE_3_NORMAL) {
        sEnvT = 0.0f;
        sFallRamp = 0.0f;
        sPrevFam = 0;
        return 0.0f;
    }

    const s32 st = bs_getState();
    // The envelope restarts when the EVENT changes, not when the state id does. Those are not the
    // same thing: dying over water runs BS_41_DIE and then hands off to BS_54_SWIM_DIE partway
    // through (die.c, and bsbswim_die_init even tests for that exact predecessor), and both are
    // deaths. Keying the reset on the raw id restarted the death envelope at the hand-off, which
    // collapsed a tilt the comment below promises will HOLD and swung the view about 45 degrees
    // while the player was dead and holding no stick. So the reset asks which of these shapes is
    // running, and one death is one envelope however many states the game spends it in.
    const int fam = VrBs_IsDeath(st)          ? 1
                    : (st == BS_72_SPLAT)     ? 2
                    : VrBs_IsHurt(st)         ? 3
                    : VrBs_IsBodyTurnLive(st) ? 4
                                              : 0;
    if (fam != sPrevFam) {
        sEnvT = 0.0f; // a new event starts its envelope at the beginning, never part way through
        sPrevFam = fam;
    }
    // CLAMPED delta. A hitch, a loading pause or a spell in a menu must not let an envelope jump
    // most of its length in one tick - that is the one way a time-driven shape can still surprise.
    float dt = time_getDelta();
    if (dt < 0.0f) {
        dt = 0.0f;
    }
    if (dt > 0.1f) {
        dt = 0.1f;
    }
    sEnvT += dt;

    // --- what the body itself is doing -------------------------------------------------------
    // Two different exemptions, and they are NOT the same set, which is worth saying plainly
    // because treating them as one put the world upside down.
    //
    //   ownsAngle - a deliberate move is driving the view, so the long-fall look-up must not blend
    //               itself in on top. True for the somersault AND the beak buster.
    //   goesOverTop - this move is SUPPOSED to pass vertical, so the never-past-vertical clamp has
    //               to stand aside or it would stop the move completing. True for the somersault
    //               ONLY. The beak buster tops out at 90 degrees nose-down and never goes over, so
    //               exempting it bought nothing and cost everything: its angle PARKS at exactly 90
    //               for the whole plunge (bsbbuster_init runs the tuck over 0 .. 0.35 and the clock
    //               stops there), so a player already looking down 75 - which is what you do when
    //               you are aiming a beak buster at something - was held at 165 degrees, inverted,
    //               for as long as the plunge lasted. Clamped it is still 85 degrees nose-down and
    //               still unmistakably a beak buster.
    //
    // Everything else here is an ATTITUDE - which way the body is lying or being thrown - and gets
    // both: blended into by the fall, and bounded against wherever the player is already looking.
    float deg = 0.0f;
    bool ownsAngle = false;
    bool goesOverTop = false;
    if (VrBs_IsDeath(st)) {
        // Smoothstep in and HOLD. You are on your back and you are not getting up, so the view
        // has no business easing itself level while the death plays out.
        float p = sEnvT / 0.55f;
        if (p > 1.0f) {
            p = 1.0f;
        }
        deg = kTiltDeathDeg * (p * p * (3.0f - 2.0f * p));
    } else if (st == BS_72_SPLAT) {
        // Fall damage: bssplat_init plays its impact animation for 1.1 s, so the half-sine is
        // sized to it - the view is driven into the dirt and pushed back up as he gets up.
        float p = sEnvT / 1.1f;
        if (p > 1.0f) {
            p = 1.0f;
        }
        deg = kTiltSplatDeg * sinf(kPi * p);
    } else if (VrBs_IsHurt(st)) {
        // A SHORT flinch, and short on purpose. NO ow state ends on its animation - every one of
        // them exits on the physics, when player_isStable() goes true at the end of the knockback
        // arc (ow.c, and the same for the ant, bee, croc and the rest, which all run different
        // animation lengths anyway). So sizing this to the animation, as a first pass did at
        // 0.91 s, made it "level again when he lands" only on perfectly flat ground: knocked back
        // onto a step 100 units up, the arc ends at 0.31 s and the tilt was still at 38 degrees
        // when the state let go, and the release then unwound all of it in about six frames. That
        // snap is the exact thing this feature exists to avoid.
        //
        // 0.30 s is under the shortest arc worth worrying about, so the view is already home
        // whenever and wherever he lands, and nothing is left for the release to unwind. It also
        // reads better: being hit is a jolt, not a swoon.
        float p = sEnvT / 0.30f;
        if (p > 1.0f) {
            p = 1.0f;
        }
        deg = kTiltHurtDeg * sinf(kPi * p);
    } else if (VrBs_IsBodyTurnLive(st)) {
        ownsAngle = true;
        goesOverTop = (st == BS_12_BFLIP || st == BS_31_ROLL);
        void* anim = baanim_getAnimCtrlPtr();
        if (anim != NULL) {
            const f32 t = anctrl_getAnimTimer(anim);
            const enum asset_e asset = anctrl_getIndex(anim);
            if (st == BS_12_BFLIP) {
                // FLIP JUMP (Z then A) - a full BACKWARD somersault over the ENTER animation's
                // active range, which bsbflip_init sets to 0 .. 0.7866.
                //
                // THE LANDING BUG, and why it was landing and not the flip: the state does not
                // end when the somersault ends. bsbflip_update swaps the animation to
                // ASSET_4C_ANIM_BSBFLIP_HOLD (or, if A is released, to the EXIT animation) the
                // moment the ENTER animation stops, and ONLY THEN does it start watching for
                // player_isStable - so every landing happens while a different animation is
                // playing. The old code asked bs_getState() alone and then divided whatever
                // animation timer it found by the ENTER range, so for the last half second of
                // every flip it was reading the HOLD loop's clock: 0.13 s per lap, which came out
                // as the target sweeping a full turn roughly eight times a second. Landing then
                // released a target caught anywhere in that churn, and the release rounds to the
                // NEAREST whole revolution - so up to another half turn got rolled on after
                // touchdown. That is the extra flip.
                //
                // The fix is the sm64 camera's rule: the angle is a function of the move's own
                // animation progress, and when the move is over the angle is ALREADY home. Once
                // the somersault animation is no longer the one playing, the turn is complete -
                // and a completed revolution IS upright, so this returns level and the release
                // folds the finished turn away with no travel at all. (Ruled out on the way: the
                // animated head offset only moves the eye's POSITION, it cannot rotate the view;
                // and the ease running on past the state change is only a problem when it has
                // somewhere left to go, which after this it does not.)
                //
                // AND THE ASSET ID ALONE DOES NOT SAY IT. On touchdown the state REPLAYS the
                // somersault asset as its landing animation: _bsbflip_802A2DC0 (bFlip.c) sets
                // index 0x4B again with start 0.8566, and the state stays BS_12_BFLIP for the
                // ~0.32 s that runs. An asset-only test passes that, and 0.8566/0.7866 clamps to
                // a flat -360 for its whole length - a fresh full revolution, at landing, which
                // is the exact bug. What separates them is the CLOCK: the somersault is bounded
                // by its own sub-range end (0.7866), and the landing replay begins past it.
                if (asset != ASSET_4B_ANIM_BSBFLIP_ENTER || t > 0.7866f) {
                    deg = 0.0f;
                } else {
                    float p = t / 0.7866f;
                    if (p < 0.0f) {
                        p = 0.0f;
                    }
                    if (p > 1.0f) {
                        p = 1.0f;
                    }
                    deg = -360.0f * p; // backward
                }
            } else if (st == BS_31_ROLL) {
                // FORWARD ROLL (B at a run) - one full FORWARD somersault, nose down. Read off the
                // shipped animation rather than guessed from the state's name: the torso bone in
                // ASSET_4F_BSTWIRL sweeps its PITCH curve through +360 degrees over the move (0,
                // 61, 107, 134, 169, 247, 290, then 377 at frame 60 settling to exactly 360), in
                // the same sign the flip jump's own asset uses for its -360 backward turn and the
                // same sign the prone poses use - the belly slide and the underwater swim both park
                // that curve near +90, nose down.
                //
                // AND IT DOES NOT SPIN. The asset carries no yaw curve on the torso at all, and
                // bstwirl_init pins his facing and simply drives him forward. So this pitches and
                // only pitches. An uncommanded yaw spin is the one thing a VR camera must never do
                // to somebody, and the name "twirl" was the only thing here that ever suggested it.
                //
                // The clock ends at 0.8011 rather than 1.0 because that is the game's own end of
                // the move: bstwirl_update watches for exactly that mark, then stretches the
                // duration to 2.5 s, drops the hitbox and spends the rest standing him back up. The
                // revolution is complete there, so past it the target is 0 and the release rule
                // folds the finished turn away with no travel, same as the flip jump. Jumping out
                // of a roll with A releases mid-turn and that rule finishes it the way the body was
                // already going.
                const float kTurnEnd = 0.8011f;
                if (asset != ASSET_4F_ANIM_BSTWIRL || t > kTurnEnd) {
                    deg = 0.0f;
                } else {
                    float p = t / kTurnEnd;
                    if (p < 0.0f) {
                        p = 0.0f;
                    }
                    if (p > 1.0f) {
                        p = 1.0f;
                    }
                    deg = 360.0f * p; // forward
                }
            } else if (asset == ASSET_1D_ANIM_BSBBUSTER) {
                // BEAK BUSTER (A then Z) - a forward tuck into a beak-down plunge, then back up.
                // bsbbuster_init runs the tuck over sub-range 0 .. 0.35 and the clock PARKS there
                // for the whole plunge, which is why holding nose-down past the tuck is right.
                // After the slam, bsbbuster_update extends the same animation to 0.7299 to stand
                // him back up - so the head comes up on exactly the animation that lifts it, and
                // the angle is 0 by the time the state can hand off to BS_20_LANDING.
                const float kTuckEnd = 0.35f;
                const float kRecoverEnd = 0.7299f;
                if (t <= kTuckEnd) {
                    float p = t / kTuckEnd;
                    if (p < 0.0f) {
                        p = 0.0f;
                    }
                    deg = 90.0f * p; // forward
                } else {
                    float p = (t - kTuckEnd) / (kRecoverEnd - kTuckEnd);
                    if (p > 1.0f) {
                        p = 1.0f;
                    }
                    deg = 90.0f * (1.0f - p);
                }
            }
        }
    }

    // --- THE LONG FALL, laid over whatever carried you off the edge --------------------------
    // A really long fall turns the eyes up toward what you just left.
    // Time-driven and not animation-driven on purpose: bsjump_tumble_init plays its flail on a
    // LOOP, so animation progress there says nothing about how long you have been falling.
    // Latched by its own value, so it eases in over 1.5 s and back out over 0.75 s and cannot
    // flicker as the fall passes between BS_2F_FALL and the tumble.
    //
    // The distance is read off the game's own fall-damage ruler rather than picked: that table
    // (falldamage.c) costs you one health past 1000 units and one more per thousand after it, so
    // 4000 is the drop that takes four - half of a full honeycomb bar, and unmistakably a plunge
    // rather than a step off a ledge. The reference gates this on sm64's death plane, which Banjo
    // has no equivalent of; "a fall this game considers serious" is the closest true statement.
    // Under it nothing happens at all, and the 1.5 s ramp on top means a short damaging drop is
    // over before the look-up has come far in.
    //
    // EVERY skin falls, not just the bear. The hurt and death tables above enumerate all nine ow
    // states and all seven die states because each transformation runs its own; falling is exactly
    // the same, and listing only Banjo's meant a 4000-unit drop as the pumpkin in Mad Monster or
    // the croc in Bubblegloop did nothing at all while the identical drop as Banjo swung the full
    // 67.5. None of the transformations ever enter BS_3D_FALL_TUMBLING, so there was no chance of
    // it happening by accident.
    {
        const bool grounded = player_isStable() || player_inWater();
        const bool inFallState = st == BS_3D_FALL_TUMBLING || st == BS_2F_FALL ||
                                 st == BS_38_ANT_FALL || st == BS_4B_PUMPKIN_FALL ||
                                 st == BS_61_CROC_FALL || st == BS_6A_WALRUS_FALL ||
                                 st == BS_71_BTROT_FALL || st == BS_88_BEE_FALL;
        const bool falling = !grounded && inFallState &&
                             bafalldamage_get_distance_fallen() > 4000.0f;
        sFallRamp += (falling ? 1.0f : -2.0f) * dt / 1.5f;
        if (sFallRamp < 0.0f) {
            sFallRamp = 0.0f;
        }
        if (sFallRamp > 1.0f) {
            sFallRamp = 1.0f;
        }
    }
    if (sFallRamp > 0.001f && !ownsAngle) {
        // BLENDED, not added: the fall OVERRIDES whatever action carried you over the edge, and
        // hands back to it the same way. Smoothstep on the ramp so neither end of it is a corner.
        const float t = sFallRamp * sFallRamp * (3.0f - 2.0f * sFallRamp);
        deg = deg * (1.0f - t) + kTiltFallDeg * t;
    }

    // NEVER PAST VERTICAL. This is a pitch laid on top of the one the player is already holding,
    // and nothing downstream adds the two up: the tilt is an eye-space rotation applied after the
    // game camera (mat_flip_apply), so it composes freely with it. sPitch is the stick-and-mouse
    // pitch in the game's convention, where POSITIVE is up (a camera pitched down reads as ~340,
    // which is what port_vrCullAdjust normalizes), and this angle's positive is nose DOWN - so the
    // two oppose and the composed look-up is sPitch - deg. Left alone, a player looking up at 75
    // while a fall asks for 67.5 more is taken to 142 and the world rolls over on him. That is the
    // single worst thing a VR camera can do to someone and it costs one clamp to make impossible.
    // Bounded to 85, just short of straight up, where the horizon still says which way is up.
    // Spins are exempt, as above - a somersault that cannot pass vertical is not a somersault.
    // The player's own NECK is not in this sum and cannot be: tilting your head back is a real
    // motion your inner ear agrees with, and clamping it is what actually makes people ill.
    if (!goesOverTop) {
        const float kMax = 85.0f;
        if (deg < sPitch - kMax) {
            deg = sPitch - kMax;
        }
        if (deg > sPitch + kMax) {
            deg = sPitch + kMax;
        }
    }
    return deg * kDeg2Rad;
}

// Per-tick VR<->game sync: while paused (or the VR overlay is up) the shared plane carries MENU
// content, so it switches to the SCREEN knobs - the HUD size/dist sliders stop moving the menus.
// ---- field-report repro (diagnostic only, env-gated, zero cost when unset) -------------------
// Headless reproduction of wild reports without a save that stands at the scene:
//   BK_DIAG_WARP=<map>,<exit>   fires the dev menu's warp once gameplay has settled
//   BK_DIAG_SPAWN=<id>[,<id>..] spawns a ring of four of each actor around the player, so one
//                               capture shows the model's front AND back
// Both print what they did, so a run's log proves the scene was actually reached.
static void VrDiag_Tick(void) {
    static int sState = -1; // -1 unparsed, 0 off, 1 settle-then-warp, 2 settle-then-spawn, 3 done
    static int sWarpMap = -1;
    static int sWarpExit = 0;
    static int sSpawn[8];
    static int sSpawnCount = 0;
    static int sTicks = 0;
    if (sState < 0) {
        sState = 0;
        const char* w = getenv("BK_DIAG_WARP");
        const char* s = getenv("BK_DIAG_SPAWN");
        if (w != NULL && sscanf(w, "%i,%i", &sWarpMap, &sWarpExit) >= 1) {
            sState = 1;
        }
        if (s != NULL) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s", s);
            for (char* t = strtok(buf, ","); t != NULL && sSpawnCount < 8; t = strtok(NULL, ",")) {
                int v = 0;
                if (sscanf(t, "%i", &v) == 1) {
                    sSpawn[sSpawnCount++] = v;
                }
            }
            if (sState == 0 && sSpawnCount > 0) {
                sState = 2; // no warp requested: settle in the boot map, then spawn
            }
        }
        if (sState == 0 && getenv("BK_DIAG_TRANSITION") != NULL) {
            sState = 2; // transition-only run: settle, then fire it
        }
        if (sState != 0) {
            setvbuf(stdout, NULL, _IONBF, 0);
            printf("[DIAG] armed: state=%d warp=0x%X,%d spawnCount=%d\n", sState, sWarpMap, sWarpExit, sSpawnCount);
        }
        // A/B seam for the far-LOD investigation: force the existing Disable LOD enhancement on
        // for this run only (the harness restores the config file around the run).
        const char* lod = getenv("BK_DIAG_SETLOD");
        if (lod != NULL) {
            CVarSetInteger(CVAR_ENHANCEMENT("Graphics.DisableLOD"), atoi(lod));
            printf("[DIAG] DisableLOD forced to %d\n", atoi(lod));
        }
    }
    if (sState == 0) {
        return;
    }
    static int sBeat = 0;
    ++sBeat;
    if ((sState < 3 && (sBeat <= 400 || (sBeat % 30) == 0)) || (sBeat % 300) == 0) {
        printf("[DIAG] t=%d state=%d mode=%d draw=%d map=0x%X ticks=%d\n", sBeat, sState, (int)getGameMode(),
               (int)gsworld_getEnableDraw(), (int)gsworld_getMap(), sTicks);
    }
    if (sState >= 3) {
        return;
    }
    if (getGameMode() != GAME_MODE_3_NORMAL || !gsworld_getEnableDraw()) {
        sTicks = 0; // count settled gameplay ticks only
        return;
    }
    sTicks++;
    if (sState == 1) {
        if (sTicks >= 120) { // four seconds into the boot map: past the entry wipe and swoop
            printf("[DIAG] warp to map 0x%X exit %d\n", sWarpMap, sWarpExit);
            func_8031D04C((enum map_e)sWarpMap, sWarpExit);
            sState = 2;
            sTicks = 0;
        }
        return;
    }
    if (sTicks >= 150) { // settled in the target map (the warp drops EnableDraw during the load)
        // BK_DIAG_TRANSITION=<idx>: fire the game's own indexed transition over live gameplay.
        // Index 1 is the falling-jiggy screen-capture transition, which otherwise only plays on
        // world entry from the lair - unreachable headless without a save standing at a door.
        const char* tr = getenv("BK_DIAG_TRANSITION");
        if (tr != NULL) {
            int idx = 0;
            if (sscanf(tr, "%i", &idx) == 1) {
                printf("[DIAG] transition index %d\n", idx);
                gctransition_8030BEA4(idx);
            }
        }
        f32 pos[3];
        player_getPosition(pos);
        for (int i = 0; i < sSpawnCount; i++) {
            // A distance-graded line per actor id, so ONE capture answers whether a rendering
            // fault tracks distance (LOD/draw-distance selectors) - near and far in the same frame.
            static const f32 kDist[4] = { 150.0f, 400.0f, 900.0f, 1800.0f };
            for (int k = 0; k < 4; k++) {
                f32 p[3] = { pos[0] + kDist[k], pos[1] + 40.0f, pos[2] + 220.0f * (f32)i };
                u32 xb, yb, zb;
                memcpy(&xb, &p[0], 4);
                memcpy(&yb, &p[1], 4);
                memcpy(&zb, &p[2], 4);
                __spawnQueue_add_4((void*)spawnQueue_actor_f32, (uintptr_t)(u32)sSpawn[i], (uintptr_t)xb,
                                   (uintptr_t)yb, (uintptr_t)zb);
            }
            printf("[DIAG] spawn ring actor 0x%X around (%.0f, %.0f, %.0f)\n", sSpawn[i], pos[0], pos[1], pos[2]);
        }
        sState = 3;
    }
}

extern "C" void VrGame_SyncFrame(void) {
    sVrTick++;              // freshness clock for the sky-pass Mtx registry
    VrGame_SampleMouse();   // one mouse sample per tick; look paths consume it once
    VrGame_PollHaptics();   // landing, damage and death, felt in the hands
    VrDiag_Tick();          // field-report repro: env-gated warp/spawn, no-op when unset
    // DIORAMA anchoring: hand vr.cpp how far the camera currently is from what it is looking at,
    // so the tabletop can be anchored on BANJO instead of on the camera that orbits him.
    {
        f32 cam[3], focus[3];
        ncDynamicCamera_getPosition(cam);
        func_802C0490(focus);
        const float dx = cam[0] - focus[0], dy = cam[1] - focus[1], dz = cam[2] - focus[2];
        vr_set_focus_distance(sqrtf(dx * dx + dy * dy + dz * dz));
    }
    vr_set_hud_menu_mode(getGameMode() == GAME_MODE_4_PAUSED || port_vrNativeMenu_isOpen());
    vr_set_flip_angle(VrFlipAngleRad()); // view tilt target; vr.cpp eases it at the headset's rate
    // Whether First Person is really driving the camera. The same drive-tick freshness that decides
    // if the bear stays hidden decides whether our framing offsets apply: when the game takes the
    // camera for a conversation or a switch reveal, the offsets fade and the authored shot stands.
    vr_set_fp_framing(port_vrFirstPerson_hidePlayer());

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
void port_vrHapticAttackHit(void) {
}
int port_vrFirstPersonMode(void) {
    return 0; // flat build: the cartridge's own C-Up first person is untouched
}
}

#endif
