// The native VR options overlay: every VR tunable the port has, drawn in Banjo-Kazooie's OWN dialog
// font, organized into the SAME pages as the Enhancements -> VR screen so the two menus are one
// mental model. Right trigger from PAUSE opens it; LEFT/RIGHT GRIP flip pages; left stick navigates;
// left/right adjusts; A activates; B or right trigger closes.
//
// Rendering: light full-view dim, then a solid near-black plate, then native-scale glyphs pushed
// into the game's deferred print buffer (they flush over the plate at end of frame). Strings live in
// static buffers - the print buffer stores pointers until the flush.
//
// Input ownership: while open, VrGame_MergePad zeroes the whole pad, so this file is the ONLY
// consumer of the sticks and buttons it reads. The open flag self-heals if the game leaves pause.

#include "vr.h"

#if defined(ENABLE_VR) && defined(_WIN32)

#include <stdio.h>
#include <string.h>
#include <libultraship/bridge/consolevariablebridge.h>

extern "C" {
#include "enums.h" // GAME_MODE_4_PAUSED
#include <libultraship/libultra/controller.h> // N64 pad button masks - the gamepad drive path
void print_dialog(int x, int y, unsigned char* string);
void print_bold_overlapping(int x, int y, float scale, unsigned char* string);
void text_setNormalTextColor(int r, int g, int b);
int getGameMode(void);
void gcbound_reset(void);
void gcbound_alpha(int alpha);
void gcbound_color(int r, int g, int b);
void gcbound_draw(void* gfx);   // Gfx** - full-view tint plate, widescreen-safe
void port_vrBoundRect(void* gfx, int x, int y, int w, int h, int r, int g, int b, int a); // translucent plate
}

// ---- rows -------------------------------------------------------------------

enum VrRowKind { ROW_ENUM, ROW_FLOAT, ROW_INT01, ROW_ACTION };

struct VrRow {
    const char* label;
    VrRowKind kind;
    const char* cvar;
    float step, mn, mx, def;
    void (*action)(void);
    const char* const* names; // ROW_ENUM: value names, indexed from mn
    int skipValue;            // ROW_ENUM: value to skip while cycling (-1 = none)
    const char* help;         // one-line explanation, shown for the selected row
};

static void RowRecenter(void) {
    vr_recenter();
    vr_controller_rumble(0.5f, 0.06f);
}
static void RowDefaults(void) {
    // Restores every NUMBER on every page - and deliberately NOT the view mode: a reset should
    // fix values, not teleport the player into a different way of seeing the world.
    vr_reset_defaults();
    CVarSetFloat("gVRWorldScale", 100.0f);
    CVarSetFloat("gVRStereo", 1.0f);
    CVarSetFloat("gVREyeHeight", -1.09f);
    CVarSetFloat("gVRHudScale", 0.35f);
    CVarSetFloat("gVRHudDist", 2.9f);
    CVarSetFloat("gVRMenuDist", 3.4f);
    CVarSetFloat("gVRMenuSize", 3.6f);
    CVarSetFloat("gVRThirdPersonDist", 0.7f);
    CVarSetFloat("gVRFirstPersonScale", 100.0f);
    CVarSetFloat("gVRFirstPersonFwd", 0.0f);
    CVarSetFloat("gVRFirstPersonEyeHeight", 0.0f);
    CVarSetFloat("gVRDioramaWorldScale", 8000.0f);
    CVarSetFloat("gVRDioramaDist", 0.05f);
    CVarSetFloat("gVRDioramaHeight", -0.4f);
    CVarSetFloat("gVRMenuOpacity", 0.85f);
    CVarSetFloat("gVRImGuiOpacity", 0.95f);
    CVarSetInteger("gVRFpViewBob", 0);
    CVarSetInteger("gVRFpVerticalLook", 1);
    CVarSetInteger("gVRSwimADash", 1);
    CVarSetInteger("gVRFpHeadMove", 1);
    CVarSetInteger("gVRFpImmersive", 1);
    CVarSetInteger("gVRFpSwimFollow", 1);
    CVarSetFloat("gVRHeadScale", 0.1f);
    CVarSetFloat("gVRFpLookSpeed", 160.0f);
    // The invert rows on the SYSTEM page drive the port's own camera invert, so the reset that
    // covers that page has to clear those keys - a row a page's own defaults button cannot undo
    // reads as a defaults button that does not work.
    CVarSetInteger("gEnhancements.Camera.InvertX", 0);
    CVarSetInteger("gEnhancements.Camera.InvertY", 0);
    // Free Look carries its OWN invert pair, and the orbit now honours either one. Those two live
    // on the desktop page with no row in here, so a player who set them months ago on a monitor
    // would tick INVERT LOOK Y in the headset, see nothing change, untick it, still see nothing,
    // and have no way to reach the switch actually holding it down. Clear them with the rest.
    CVarSetInteger("gEnhancements.Camera.FreeLook.InvertX", 0);
    CVarSetInteger("gEnhancements.Camera.FreeLook.InvertY", 0);
    CVarSetInteger("gVRAntiClip", 1);
    CVarSetInteger("gVRMouseLook", 1);
    CVarSetFloat("gVRMouseSens", 0.10f);
    CVarSetInteger("gVRHideHud", 0);
    CVarSetFloat("gVRPointerSpeed", 1.0f);
    CVarSetInteger("gVRFogMode", 1);
    CVarSetFloat("gVRFogNear", 60.0f);
    CVarSetFloat("gVRFogFar", 250.0f);
    CVarSetFloat("gVRDrawDistance", 4.0f);
    CVarSetInteger("gMSAAValue", 4);
    CVarSetInteger("gVRDisableCulling", 1);
    CVarSetInteger("gVRCutscenes", 0);
    CVarSetInteger("gVRPassthrough", 0);
    CVarSetInteger("gVRMotionControls", 1);
    CVarSetInteger("gVRHaptics", 1);
}
static void RowResume(void); // needs the state block below

static const char* kViewModeNames[5] = { "THIRD PERSON", "FIRST PERSON", "-", "DIORAMA", "THEATER" };
static const char* kFogModeNames[3] = { "OFF", "WORLD", "STOCK" };

// Pages mirror Enhancements -> VR one for one, so a knob found in one menu lives in the same group
// in the other.
struct VrPage {
    const char* title;
    const VrRow* rows;
    int rowCount;
};

// The VIEW page shows the ACTIVE mode's own knobs. The per-mode split (each mode keeps its own
// scale and framing CVars so tuning one never disturbs another) is invisible from inside a
// headset: a first-person player who twists WORLD SCALE sees nothing happen and reads it as
// broken - the first wild user did exactly that. Three variants of the first page, picked live.
static const VrRow kRowsView[] = {
    { "VIEW MODE",   ROW_ENUM,  "gVRViewMode",        1.0f, 0.0f, 4.0f, 1.0f, NULL, kViewModeNames, 2, "HOW YOU SEE THE WORLD. FIRST PERSON IS INSIDE BANJO." },
    { "SWIM FOLLOW", ROW_INT01, "gVRFpSwimFollow",    1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "THE VIEW TURNS WITH BANJO WHILE YOU SWIM OR FLY." },
    { "SWIM A DASH", ROW_INT01, "gVRSwimADash",       1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "UNDERWATER A IS THE FAST DASH AND B THE SLOW PADDLE." },
    { "WORLD SCALE", ROW_FLOAT, "gVRWorldScale",      5.0f, 20.0f, 400.0f, 100.0f, NULL, NULL, -1, "GAME UNITS PER METRE. LOWER MAKES THE WORLD BIGGER." },
    { "STEREO",      ROW_FLOAT, "gVRStereo",          0.05f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "DEPTH STRENGTH. LOWER IF THE IMAGE IS HARD TO FUSE." },
    { "EYE HEIGHT",  ROW_FLOAT, "gVREyeHeight",       0.05f, -1.5f, 1.5f, -1.09f, NULL, NULL, -1, "RAISES OR LOWERS YOUR EYE IN THIRD PERSON." },
    { "CAM DIST",    ROW_FLOAT, "gVRThirdPersonDist", 0.05f, 0.3f, 3.0f, 0.7f, NULL, NULL, -1, "HOW FAR THE CAMERA SITS FROM BANJO. 1 IS STOCK." },
    { "HUD SIZE",    ROW_FLOAT, "gVRHudScale",        0.05f, 0.1f, 1.5f, 0.35f, NULL, NULL, -1, "HOW MUCH OF YOUR VIEW THE GAME HUD FILLS." },
    { "HUD DIST",    ROW_FLOAT, "gVRHudDist",         0.2f, 0.3f, 20.0f, 2.9f, NULL, NULL, -1, "METRES THE HUD FLOATS IN FRONT OF YOU." },
    { "VR DEFAULTS", ROW_ACTION, NULL, 0, 0, 0, 0, RowDefaults, NULL, -1, "RESTORES EVERY VR SETTING AND THE INVERT LOOK KEYS." },
};

static const VrRow kRowsViewFp[] = {
    { "VIEW MODE",   ROW_ENUM,  "gVRViewMode",             1.0f, 0.0f, 4.0f, 1.0f, NULL, kViewModeNames, 2, "HOW YOU SEE THE WORLD. FIRST PERSON IS INSIDE BANJO." },
    { "SWIM FOLLOW", ROW_INT01, "gVRFpSwimFollow",         1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "THE VIEW TURNS WITH BANJO WHILE YOU SWIM OR FLY." },
    { "FP SCALE",    ROW_FLOAT, "gVRFirstPersonScale",     1.0f, 20.0f, 400.0f, 100.0f, NULL, NULL, -1, "WORLD SIZE IN FIRST PERSON ONLY." },
    { "STEREO",      ROW_FLOAT, "gVRStereo",               0.05f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "DEPTH STRENGTH. LOWER IF THE IMAGE IS HARD TO FUSE." },
    { "EYE RAISE",   ROW_FLOAT, "gVRFirstPersonEyeHeight", 0.05f, -1.5f, 1.5f, 0.0f, NULL, NULL, -1, "RAISES OR LOWERS YOUR EYE IN FIRST PERSON." },
    { "EYE FORWARD", ROW_FLOAT, "gVRFirstPersonFwd",       0.15f, -3.0f, 5.0f, 0.0f, NULL, NULL, -1, "NUDGES THE EYE AHEAD OF OR BEHIND BANJOS HEAD." },
    { "HUD SIZE",    ROW_FLOAT, "gVRHudScale",             0.05f, 0.1f, 1.5f, 0.35f, NULL, NULL, -1, "HOW MUCH OF YOUR VIEW THE GAME HUD FILLS." },
    { "HUD DIST",    ROW_FLOAT, "gVRHudDist",              0.2f, 0.3f, 20.0f, 2.9f, NULL, NULL, -1, "METRES THE HUD FLOATS IN FRONT OF YOU." },
    { "VR DEFAULTS", ROW_ACTION, NULL, 0, 0, 0, 0, RowDefaults, NULL, -1, "RESTORES EVERY VR SETTING AND THE INVERT LOOK KEYS." },
};

static const VrRow kRowsViewDiorama[] = {
    { "VIEW MODE",      ROW_ENUM,  "gVRViewMode",          1.0f, 0.0f, 4.0f, 1.0f, NULL, kViewModeNames, 2, "HOW YOU SEE THE WORLD. FIRST PERSON IS INSIDE BANJO." },
    { "DIORAMA SCALE",  ROW_FLOAT, "gVRDioramaWorldScale", 100.0f, 200.0f, 12000.0f, 8000.0f, NULL, NULL, -1, "HOW SMALL THE TABLETOP LEVEL IS. HIGHER IS SMALLER." },
    { "STEREO",         ROW_FLOAT, "gVRStereo",            0.05f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "DEPTH STRENGTH. LOWER IF THE IMAGE IS HARD TO FUSE." },
    { "DIORAMA DIST",   ROW_FLOAT, "gVRDioramaDist",       0.05f, -1.5f, 2.0f, 0.05f, NULL, NULL, -1, "METRES THE TABLETOP SITS IN FRONT OF YOU." },
    { "DIORAMA HEIGHT", ROW_FLOAT, "gVRDioramaHeight",     0.02f, -1.5f, 1.5f, -0.4f, NULL, NULL, -1, "RAISES OR LOWERS THE TABLETOP." },
    { "HUD SIZE",       ROW_FLOAT, "gVRHudScale",          0.05f, 0.1f, 1.5f, 0.35f, NULL, NULL, -1, "HOW MUCH OF YOUR VIEW THE GAME HUD FILLS." },
    { "HUD DIST",       ROW_FLOAT, "gVRHudDist",           0.2f, 0.3f, 20.0f, 2.9f, NULL, NULL, -1, "METRES THE HUD FLOATS IN FRONT OF YOU." },
    { "VR DEFAULTS",    ROW_ACTION, NULL, 0, 0, 0, 0, RowDefaults, NULL, -1, "RESTORES EVERY VR SETTING AND THE INVERT LOOK KEYS." },
};

static const VrRow kRowsFp[] = {
    { "IMMERSIVE CAM", ROW_INT01, "gVRFpImmersive",          1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "YOUR HEAD IS BANJOS HEAD. LEAN, WALK AND DUCK AS HE DOES." },
    { "HEAD MOVE",     ROW_INT01, "gVRFpHeadMove",           1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "STICK FORWARD WALKS WHERE YOU ARE LOOKING." },
    { "LEAN",          ROW_FLOAT, "gVRHeadScale",            0.05f, 0.0f, 1.5f, 0.1f, NULL, NULL, -1, "HOW MUCH YOUR REAL BODY MOVEMENT MOVES THE CAMERA." },
    { "LOOK SPEED",    ROW_FLOAT, "gVRFpLookSpeed",          10.0f, 40.0f, 320.0f, 160.0f, NULL, NULL, -1, "DEGREES PER SECOND THE STICK TURNS YOUR VIEW." },
    { "VERTICAL LOOK", ROW_INT01, "gVRFpVerticalLook",       1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "LETS THE STICK LOOK UP AND DOWN. OFF USES YOUR HEAD." },
    { "VIEW BOB",      ROW_INT01, "gVRFpViewBob",            1.0f, 0.0f, 1.0f, 0.0f, NULL, NULL, -1, "SMALL WALK BOB AND LANDING DIP IN FIRST PERSON." },
    { "EYE FORWARD",   ROW_FLOAT, "gVRFirstPersonFwd",       0.15f, -3.0f, 5.0f, 0.0f, NULL, NULL, -1, "NUDGES THE EYE AHEAD OF OR BEHIND BANJOS HEAD." },
    { "EYE RAISE",     ROW_FLOAT, "gVRFirstPersonEyeHeight", 0.05f, -1.5f, 1.5f, 0.0f, NULL, NULL, -1, "RAISES OR LOWERS YOUR EYE IN FIRST PERSON." },
    { "FP SCALE",      ROW_FLOAT, "gVRFirstPersonScale",     1.0f, 20.0f, 400.0f, 100.0f, NULL, NULL, -1, "WORLD SIZE IN FIRST PERSON ONLY." },
};

static const VrRow kRowsWorld[] = {
    { "DIORAMA SCALE",  ROW_FLOAT, "gVRDioramaWorldScale", 100.0f, 200.0f, 12000.0f, 8000.0f, NULL, NULL, -1, "HOW SMALL THE TABLETOP LEVEL IS. HIGHER IS SMALLER." },
    { "DIORAMA DIST",   ROW_FLOAT, "gVRDioramaDist",       0.05f, -1.5f, 2.0f, 0.05f, NULL, NULL, -1, "METRES THE TABLETOP SITS IN FRONT OF YOU." },
    { "DIORAMA HEIGHT", ROW_FLOAT, "gVRDioramaHeight",     0.02f, -1.5f, 1.5f, -0.4f, NULL, NULL, -1, "RAISES OR LOWERS THE TABLETOP." },
    { "SCREEN DIST",    ROW_FLOAT, "gVRMenuDist",          0.2f, 0.5f, 12.0f, 3.4f, NULL, NULL, -1, "METRES THE FLAT SCREEN AND MENUS SIT AWAY." },
    { "SCREEN SIZE",    ROW_FLOAT, "gVRMenuSize",          0.2f, 1.0f, 16.0f, 3.6f, NULL, NULL, -1, "WIDTH OF THE FLAT SCREEN AND MENUS." },
    { "HIDE HUD",       ROW_INT01, "gVRHideHud",           1.0f, 0.0f, 1.0f, 0.0f, NULL, NULL, -1, "HIDES THE GAME HUD WHILE PLAYING. MENUS STAY." },
    { "POINTER SPEED",  ROW_FLOAT, "gVRPointerSpeed",      0.1f, 0.3f, 3.0f, 1.0f, NULL, NULL, -1, "HOW FAST THE STICK GLIDES THE PORT MENU POINTER." },
    // The two opacity rows moved here off SYSTEM. This page is the one named for the screen and
    // the menus, which is where a player looks for how solid they are - and it freed the row on
    // SYSTEM that HAPTICS needed, since SYSTEM was already at the ten rows the panel can hold.
    { "MENU OPACITY",   ROW_FLOAT, "gVRMenuOpacity",       0.05f, 0.3f, 1.0f, 0.85f, NULL, NULL, -1, "HOW SOLID MENUS LOOK. LOWER SHOWS THE GAME THROUGH." },
    { "SETTINGS MENU",  ROW_FLOAT, "gVRImGuiOpacity",      0.05f, 0.3f, 1.0f, 0.95f, NULL, NULL, -1, "HOW SOLID THE PORT SETTINGS MENU LOOKS IN VR." },
};

static const VrRow kRowsRender[] = {
    { "FOG",          ROW_ENUM,  "gVRFogMode",        1.0f, 0.0f, 2.0f, 1.0f, NULL, kFogModeNames, -1, "WORLD FOG IS FIXED FOR VR. STOCK IS THE OLD ONE." },
    { "FOG START",    ROW_FLOAT, "gVRFogNear",        5.0f, 1.0f, 400.0f, 60.0f, NULL, NULL, -1, "METRES WHERE FOG BEGINS." },
    { "FOG FULL",     ROW_FLOAT, "gVRFogFar",         10.0f, 2.0f, 1200.0f, 250.0f, NULL, NULL, -1, "METRES WHERE FOG IS FULLY THICK." },
    { "DRAW DIST",    ROW_FLOAT, "gVRDrawDistance",   0.1f, 1.0f, 4.0f, 4.0f, NULL, NULL, -1, "FAR CLIP MULTIPLIER. 1 ALREADY COVERS EVERY LEVEL." },
    { "CULLING OFF",  ROW_INT01, "gVRDisableCulling", 1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "DRAWS BOTH SIDES OF WALLS. FILLS VR SEE THROUGH GAPS." },
    { "ANTI CLIP",    ROW_INT01, "gVRAntiClip",       1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "STOPS LEANING PUSHING YOUR HEAD THROUGH WALLS." },
    { "CUTSCENES 3D", ROW_INT01, "gVRCutscenes",      1.0f, 0.0f, 1.0f, 0.0f, NULL, NULL, -1, "WATCH CUTSCENES IN STEREO INSTEAD OF ON THE SCREEN." },
    { "PASSTHROUGH",  ROW_INT01, "gVRPassthrough",    1.0f, 0.0f, 1.0f, 0.0f, NULL, NULL, -1, "SHOWS YOUR ROOM BEHIND A TABLETOP LEVEL." },
};

// INVERT LOOK lives here and not on the FIRST PERSON page because it is no longer a first-person
// setting: one pair of switches now inverts every camera the port drives, so a third-person player
// must not have to open a page named after a mode they are not in to find it. VR DEFAULTS came off
// this page to make room - it is the same action already sitting on page one, which is where the
// menu opens. The two opacity rows went to WORLD SCREEN for the same reason, and they read better
// there anyway; what is left is exactly the input-and-device page, which is where HAPTICS belongs.
static const VrRow kRowsSystem[] = {
    { "MOTION CTRLS",  ROW_INT01,  "gVRMotionControls", 1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "USE THE VR CONTROLLERS. OFF IS GAMEPAD ONLY." },
    { "HAPTICS",       ROW_INT01,  "gVRHaptics",        1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "THE CONTROLLERS BUZZ WHEN YOU LAND OR TAKE A HIT." },
    { "FREE LOOK CAM", ROW_INT01,  "gEnhancements.Camera.FreeLook.Enabled", 1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "RIGHT STICK AIMS THE CAMERA. NO AUTO RECENTRE." },
    { "MOUSE LOOK",    ROW_INT01,  "gVRMouseLook",      1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "THE DESKTOP MOUSE TURNS THE CAMERA WHEN CAPTURED." },
    { "MOUSE SPEED",   ROW_FLOAT,  "gVRMouseSens",      0.02f, 0.02f, 0.60f, 0.10f, NULL, NULL, -1, "DEGREES PER MOUSE COUNT. HIGHER TURNS FASTER." },
    { "INVERT LOOK X", ROW_INT01,  "gEnhancements.Camera.InvertX", 1.0f, 0.0f, 1.0f, 0.0f, NULL, NULL, -1, "REVERSES LEFT AND RIGHT LOOK IN EVERY CAMERA." },
    { "INVERT LOOK Y", ROW_INT01,  "gEnhancements.Camera.InvertY", 1.0f, 0.0f, 1.0f, 0.0f, NULL, NULL, -1, "REVERSES UP AND DOWN LOOK IN EVERY CAMERA." },
    { "RECENTER",      ROW_ACTION, NULL, 0, 0, 0, 0, RowRecenter, NULL, -1, "BRINGS THE WORLD SQUARE TO WHERE YOU ARE LOOKING." },
    { "RESUME",        ROW_ACTION, NULL, 0, 0, 0, 0, RowResume, NULL, -1, "CLOSES THIS MENU AND RETURNS TO THE PAUSE SCREEN." },
};

static const VrPage kPages[] = {
    { "VIEW",         kRowsView,   (int)(sizeof(kRowsView) / sizeof(VrRow)) },
    { "FIRST PERSON", kRowsFp,     (int)(sizeof(kRowsFp) / sizeof(VrRow)) },
    { "WORLD SCREEN", kRowsWorld,  (int)(sizeof(kRowsWorld) / sizeof(VrRow)) },
    { "RENDERING",    kRowsRender, (int)(sizeof(kRowsRender) / sizeof(VrRow)) },
    { "SYSTEM",       kRowsSystem, (int)(sizeof(kRowsSystem) / sizeof(VrRow)) },
};
static const int kPageCount = (int)(sizeof(kPages) / sizeof(VrPage));

// The VIEW page swaps to the active mode's row set, so its knobs always do something for the
// world the player is currently standing in. Input and draw must resolve through the same call.
static VrPage ResolvePage(int idx) {
    VrPage p = kPages[idx];
    if (idx == 0) {
        const int m = vr_get_view_mode();
        if (m == VR_VIEW_FIRST_PERSON) {
            p.rows = kRowsViewFp;
            p.rowCount = (int)(sizeof(kRowsViewFp) / sizeof(VrRow));
        } else if (m == VR_VIEW_DIORAMA) {
            p.rows = kRowsViewDiorama;
            p.rowCount = (int)(sizeof(kRowsViewDiorama) / sizeof(VrRow));
        }
    }
    return p;
}

// ---- state -------------------------------------------------------------------

static bool sOpen = false;
static int sPage = 0;
static int sSel = 0;
static int sRepeat = 0;
static unsigned sPrevVb = 0;
static unsigned sPrevPadBtn = 0;

// The merged N64 pad, handed over by VrGame_MergePad BEFORE the overlay zeroes it. This is what
// makes the overlay drivable from a plain gamepad: the first wild user played with an Xbox pad in
// the headset, pressed pause + right trigger, and nothing happened - every read below was
// motion-controller state. Now both sources drive the same rows: pause + R opens, stick
// navigates, left/right adjusts, A activates, B (or R again) closes, Z flips pages.
static unsigned sPadBtn = 0;
static int sPadStickX = 0;
static int sPadStickY = 0;

extern "C" void port_vrNativeMenu_feedPad(unsigned short button, signed char stickX, signed char stickY) {
    sPadBtn = button;
    sPadStickX = stickX;
    sPadStickY = stickY;
}

static void RowResume(void) {
    sOpen = false;
}

extern "C" int port_vrNativeMenu_isOpen(void) {
    // Self-heal: open implies paused - a stale open flag once ate all input for a whole session.
    if (sOpen && getGameMode() != GAME_MODE_4_PAUSED) {
        sOpen = false;
    }
    return sOpen ? 1 : 0;
}

// ---- input -------------------------------------------------------------------

static void AdjustRow(const VrRow* row, int dir) {
    switch (row->kind) {
        case ROW_ENUM: {
            if (row->names == kViewModeNames) {
                // FIRST -> THIRD -> DIORAMA -> THEATER, the order players expect, instead of the
                // enum's raw numbering (the values are pinned by the shared eye-matrix code).
                static const int kOrder[4] = { VR_VIEW_FIRST_PERSON, VR_VIEW_THIRD_PERSON, VR_VIEW_DIORAMA,
                                               VR_VIEW_THEATER };
                const int cur = CVarGetInteger(row->cvar, (int)row->def);
                int idx = 0;
                for (int k = 0; k < 4; k++) {
                    if (kOrder[k] == cur) {
                        idx = k;
                    }
                }
                idx = (idx + (dir > 0 ? 1 : 3)) % 4;
                CVarSetInteger(row->cvar, kOrder[idx]);
                vr_set_view_mode(kOrder[idx]);
                break;
            }
            const int lo = (int)row->mn, hi = (int)row->mx;
            const int span = hi - lo + 1;
            int m = CVarGetInteger(row->cvar, (int)row->def);
            do {
                m = lo + ((m - lo + dir + span) % span);
            } while (m == row->skipValue);
            CVarSetInteger(row->cvar, m);
            break;
        }
        case ROW_INT01:
            CVarSetInteger(row->cvar, CVarGetInteger(row->cvar, (int)row->def) ? 0 : 1);
            break;
        case ROW_FLOAT: {
            float v = CVarGetFloat(row->cvar, row->def) + row->step * (float)dir;
            if (v < row->mn) {
                v = row->mn;
            }
            if (v > row->mx) {
                v = row->mx;
            }
            CVarSetFloat(row->cvar, v);
            break;
        }
        default:
            break;
    }
}

extern "C" int port_vrImGuiMenuVisible(void); // VrGame.cpp

static void MenuInput(void) {
    const unsigned vb = vr_controller_buttons();
    const unsigned pb = sPadBtn;
    // One press, one consumer (the round-3 law): while the port (ImGui) menu is up, the right
    // trigger is ITS pointer click - the same press must not also toggle this overlay underneath,
    // dimming the pause backdrop. Tracking the held state through the gate means the press that
    // closes the ImGui menu cannot edge-fire here on the way out either.
    if (port_vrImGuiMenuVisible()) {
        sOpen = false;
        sPrevVb = vb;
        sPrevPadBtn = pb;
        return;
    }
    const unsigned pressed = vb & ~sPrevVb;
    const unsigned padPressed = pb & ~sPrevPadBtn;
    sPrevVb = vb;
    sPrevPadBtn = pb;

    float ls[2];
    vr_controller_stick(0, ls);

    if (!sOpen) {
        // Opens from PAUSE with the right trigger - motion controller RT, or the pad's R button
        // for gamepad players (R does nothing in the game's own pause menu, so the press has
        // exactly one consumer). The VR merge suppresses trigger-sourced game buttons for the
        // whole pause on the controller side.
        if ((pressed & VR_BTN_RTRIGGER) || (padPressed & R_TRIG)) {
            sOpen = true;
            sPage = 0;
            sSel = 0;
            sRepeat = 0;
            vr_controller_rumble(0.3f, 0.04f);
        }
        return;
    }

    // Close: B or right trigger / pad R again.
    if ((pressed & (VR_BTN_B | VR_BTN_RTRIGGER)) || (padPressed & (B_BUTTON | R_TRIG))) {
        sOpen = false;
        return;
    }

    // Grips flip pages - a whole-hand gesture for a whole-page move, and a control the runtime has
    // no claim on. On a gamepad, Z cycles forward and L backward. Selection resets to the top.
    if ((pressed & VR_BTN_RGRIP) || (padPressed & Z_TRIG)) {
        sPage = (sPage + 1) % kPageCount;
        sSel = 0;
        vr_controller_rumble(0.25f, 0.03f);
    } else if ((pressed & VR_BTN_LGRIP) || (padPressed & L_TRIG)) {
        sPage = (sPage + kPageCount - 1) % kPageCount;
        sSel = 0;
        vr_controller_rumble(0.25f, 0.03f);
    }

    const VrPage resolved = ResolvePage(sPage);
    const VrPage* page = &resolved;
    // Switching view mode on the first row can shrink the row set out from under the selection.
    if (sSel >= page->rowCount) {
        sSel = page->rowCount - 1;
    }

    if (sRepeat > 0) {
        sRepeat--;
    }
    // Whichever stick is deflected drives - the VR thumbstick is -1..1, the pad stick is the
    // N64's -80..80, so 44 is the same 0.55 threshold in pad units.
    const bool up = ls[1] > 0.55f || sPadStickY > 44;
    const bool down = ls[1] < -0.55f || sPadStickY < -44;
    const bool left = ls[0] < -0.55f || sPadStickX < -44;
    const bool right = ls[0] > 0.55f || sPadStickX > 44;

    if ((up || down) && sRepeat == 0) {
        sSel = (sSel + (down ? 1 : page->rowCount - 1)) % page->rowCount;
        sRepeat = 8;
    } else if ((left || right) && sRepeat == 0) {
        AdjustRow(&page->rows[sSel], right ? 1 : -1);
        sRepeat = 6;
    } else if (!up && !down && !left && !right && sRepeat > 2) {
        sRepeat = 2; // quick re-arm on release
    }

    if ((pressed & VR_BTN_A) || (padPressed & A_BUTTON)) {
        const VrRow* row = &page->rows[sSel];
        if (row->kind == ROW_ACTION) {
            row->action();
        } else {
            AdjustRow(row, 1);
        }
    }
}

// ---- draw (game-tick side) ----------------------------------------------------
// gfx: the pause menu's display-list cursor (Gfx**), used for the dim + plate. The text rides the
// deferred print buffer and lands on top at the frame's flush.

extern "C" void port_vrNativeMenu_push(void* gfx) {
    if (!vr_is_active()) {
        sOpen = false;
        return;
    }
    MenuInput();
    if (!sOpen) {
        return;
    }

    // Light full-view dim so the world recedes, then a SOLID near-black plate under the text block
    // so every glyph reads regardless of the world behind (the MK64 pattern).
    // Light full-view dim so the world recedes, then a TRANSLUCENT panel plate under the rows: the
    // world still reads through it (a solid slab felt like a wall) while every glyph keeps a
    // consistent dark backing. The panel is inset from the 292x216 native frame, and the value
    // column is placed so the longest label clears it - nothing can bleed off the plate.
    gcbound_reset();
    gcbound_color(0, 0, 0);
    gcbound_alpha(90);
    gcbound_draw(gfx);
    port_vrBoundRect(gfx, 22, 6, 250, 202, 6, 8, 22, 205);

    static char sValBufs[12][20]; // static: the print buffer stores POINTERS until the frame flush
    static char sPageHdr[24];
    const int xCur = 31;  // selection cursor block
    const int x0 = 40;    // labels
    const int xVal = 168; // values - clears "DIORAMA HEIGHT", the longest label
    // 12, not 13: rows start at 55 and the help text sits at 170, so 13 px only ever fitted NINE
    // rows before the last one collided with the help line. One pixel buys a tenth row on every
    // page, which is what a new toggle costs. (The selection bar is still 13 tall and only one is
    // ever drawn, so the tighter pitch changes nothing about how it reads.)
    const int rowH = 12;
    int y = 55;

    const VrPage resolved = ResolvePage(sPage);
    const VrPage* page = &resolved;
    if (sSel >= page->rowCount) {
        sSel = page->rowCount - 1;
    }
    text_setNormalTextColor(0xFF, 0xFF, 0xFF);
    // The bold font hangs UPWARD from the y it is given, so y=8 put most of the glyphs above the
    // panel's top edge - it read as "the title does not fit" however narrow it was made. Dropping it
    // to a baseline of 26 seats the whole word inside the plate; the rows below move down to match.
    print_bold_overlapping(30, 26, -0.78f, (unsigned char*)"VR OPTIONS");
    // The dialog font carries letters, digits and a period - "/" and ">" come out as garbage
    // glyphs, so the counter spells OF and the cursor is drawn geometry instead of a character.
    snprintf(sPageHdr, sizeof(sPageHdr), "PAGE %d OF %d", sPage + 1, kPageCount);
    text_setNormalTextColor(0xB4, 0xB4, 0xB4);
    print_dialog(x0, 40, (unsigned char*)page->title);
    print_dialog(xVal, 40, (unsigned char*)sPageHdr);

    // Bounded by the value-buffer count, not just the row count: the print buffer holds POINTERS
    // into sValBufs until the frame flush, so an eleventh row on some future page would write past
    // the array instead of merely looking wrong.
    const int drawRows = page->rowCount < (int)(sizeof(sValBufs) / sizeof(sValBufs[0]))
                             ? page->rowCount
                             : (int)(sizeof(sValBufs) / sizeof(sValBufs[0]));
    for (int i = 0; i < drawRows; i++) {
        const VrRow* row = &page->rows[i];
        char* val = sValBufs[i];
        val[0] = 0;
        switch (row->kind) {
            case ROW_ENUM: {
                int m = CVarGetInteger(row->cvar, (int)row->def) - (int)row->mn;
                const int span = (int)row->mx - (int)row->mn;
                if (m < 0 || m > span) {
                    m = 0;
                }
                snprintf(val, 20, "%s", row->names[m]);
                break;
            }
            case ROW_INT01:
                snprintf(val, 20, "%s", CVarGetInteger(row->cvar, (int)row->def) ? "ON" : "OFF");
                break;
            case ROW_FLOAT:
                snprintf(val, 20, "%.2f", CVarGetFloat(row->cvar, row->def));
                break;
            default:
                break;
        }
        if (i == sSel) {
            // Cursor: a bar of drawn geometry (never a missing glyph) plus the pause menu's own
            // white-versus-grey row ramp. Vertically it spans the glyph band: the dialog font
            // draws UPWARD from its baseline (the discovery that clipped the bold title), so the
            // band is y-10..y+3, and a bar reads as correct even a pixel out where a block never
            // did. Horizontally it HUGS the row instead of spanning the panel: from just left of
            // the label to just past the last character actually printed (value if there is one,
            // label otherwise). The dialog glyphs run about 8 px, and rounding up means the bar
            // can only ever be a touch generous, never short of the text.
            // 9 px per glyph plus real padding both sides: the dialog glyphs run up to 8 px, so
            // rounding up per character plus the margins keeps the whole string comfortably
            // INSIDE the bar (the tester saw text poking out at 8/char with a 4 px pad).
            int barRight = (val[0] != 0) ? (xVal + (int)strlen(val) * 9) : (x0 + (int)strlen(row->label) * 9);
            if (barRight > 260) {
                barRight = 260;
            }
            port_vrBoundRect(gfx, x0 - 7, y - 10, barRight - (x0 - 7) + 8, 13, 74, 96, 168, 150);
            text_setNormalTextColor(0xFF, 0xFF, 0xFF);
        } else {
            text_setNormalTextColor(0x96, 0x96, 0x96);
        }
        print_dialog(x0, y, (unsigned char*)row->label);
        if (val[0] != 0) {
            print_dialog(xVal, y, (unsigned char*)val);
        }
        y += rowH;
    }

    // A one-line explanation of whatever is selected - the headset equivalent of a tooltip, always
    // visible so nothing in here is a guess.
    if (page->rows[sSel].help != NULL) {
        // Word-wrap into the panel. The old single line ran clean off the right edge; the font is
        // fixed width here, so a character budget is an exact fit rather than a guess.
        static char sHelpA[40], sHelpB[40];
        const int kHelpCols = 30;
        const char* h = page->rows[sSel].help;
        int cut = 0, lastSpace = -1;
        while (h[cut] != 0 && cut < kHelpCols) {
            if (h[cut] == ' ') {
                lastSpace = cut;
            }
            cut++;
        }
        if (h[cut] != 0 && lastSpace > 0) {
            cut = lastSpace; // break on a word boundary, never mid-word
        }
        int n = cut < 39 ? cut : 39;
        memcpy(sHelpA, h, n);
        sHelpA[n] = 0;
        const char* rest = (h[cut] == ' ') ? h + cut + 1 : h + cut;
        int m = 0;
        while (rest[m] != 0 && m < 39) {
            m++;
        }
        memcpy(sHelpB, rest, m);
        sHelpB[m] = 0;

        text_setNormalTextColor(0xFF, 0xE0, 0x78);
        print_dialog(x0, 172, (unsigned char*)sHelpA);
        if (sHelpB[0] != 0) {
            print_dialog(x0, 183, (unsigned char*)sHelpB);
        }
    }

    // Footer: the page control stays on screen, so the other pages are never a secret.
    text_setNormalTextColor(0xB4, 0xB4, 0xB4);
    print_dialog(x0, 199, (unsigned char*)"GRIPS OR Z TURN PAGE. B CLOSES.");
    text_setNormalTextColor(0xFF, 0xFF, 0xFF);
}

#else // no VR layer - stubs

extern "C" int port_vrNativeMenu_isOpen(void) {
    return 0;
}
extern "C" void port_vrNativeMenu_push(void* gfx) {
    (void)gfx;
}
extern "C" void port_vrNativeMenu_feedPad(unsigned short button, signed char stickX, signed char stickY) {
    (void)button;
    (void)stickX;
    (void)stickY;
}

#endif
