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
    vr_reset_defaults();
    CVarSetInteger("gVRViewMode", 1); // First Person is the shipped default
    CVarSetFloat("gVRWorldScale", 100.0f);
    CVarSetFloat("gVRStereo", 1.0f);
    CVarSetFloat("gVREyeHeight", -0.29f);
    CVarSetFloat("gVRHudScale", 0.35f);
    CVarSetFloat("gVRHudDist", 2.9f);
    CVarSetFloat("gVRMenuDist", 3.4f);
    CVarSetFloat("gVRMenuSize", 3.6f);
    CVarSetFloat("gVRThirdPersonDist", 0.8f);
    CVarSetFloat("gVRDioramaWorldScale", 6200.0f);
    CVarSetFloat("gVRDioramaDist", 0.0f);
    CVarSetFloat("gVRDioramaHeight", -0.06f);
    CVarSetInteger("gVRFpVerticalLook", 0);
    CVarSetInteger("gVRFpHeadMove", 1);
    CVarSetInteger("gVRFpImmersive", 1);
    CVarSetFloat("gVRHeadScale", 0.1f);
    CVarSetFloat("gVRFpLookSpeed", 160.0f);
    CVarSetInteger("gVRFpInvertX", 0);
    CVarSetInteger("gVRAntiClip", 1);
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

static const VrRow kRowsView[] = {
    { "VIEW MODE",   ROW_ENUM,  "gVRViewMode",        1.0f, 0.0f, 4.0f, 1.0f, NULL, kViewModeNames, 2, "HOW YOU SEE THE WORLD. FIRST PERSON IS INSIDE BANJO." },
    { "WORLD SCALE", ROW_FLOAT, "gVRWorldScale",      5.0f, 20.0f, 400.0f, 100.0f, NULL, NULL, -1, "GAME UNITS PER METRE. LOWER MAKES THE WORLD BIGGER." },
    { "STEREO",      ROW_FLOAT, "gVRStereo",          0.05f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "DEPTH STRENGTH. LOWER IF THE IMAGE IS HARD TO FUSE." },
    { "EYE HEIGHT",  ROW_FLOAT, "gVREyeHeight",       0.05f, -1.5f, 1.5f, -0.29f, NULL, NULL, -1, "RAISES OR LOWERS YOUR EYE IN THIRD PERSON." },
    { "CAM DIST",    ROW_FLOAT, "gVRThirdPersonDist", 0.05f, 0.3f, 3.0f, 0.8f, NULL, NULL, -1, "HOW FAR THE CAMERA SITS FROM BANJO. 1 IS STOCK." },
    { "HUD SIZE",    ROW_FLOAT, "gVRHudScale",        0.05f, 0.1f, 1.5f, 0.35f, NULL, NULL, -1, "HOW MUCH OF YOUR VIEW THE GAME HUD FILLS." },
    { "HUD DIST",    ROW_FLOAT, "gVRHudDist",         0.2f, 0.3f, 20.0f, 2.9f, NULL, NULL, -1, "METRES THE HUD FLOATS IN FRONT OF YOU." },
};

static const VrRow kRowsFp[] = {
    { "IMMERSIVE CAM", ROW_INT01, "gVRFpImmersive",          1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "YOUR HEAD IS BANJOS HEAD. LEAN AND WALK WHERE YOU LOOK." },
    { "HEAD MOVE",     ROW_INT01, "gVRFpHeadMove",           1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "STICK FORWARD WALKS WHERE YOU ARE LOOKING." },
    { "LEAN",          ROW_FLOAT, "gVRHeadScale",            0.05f, 0.0f, 1.5f, 0.1f, NULL, NULL, -1, "HOW MUCH YOUR REAL BODY MOVEMENT MOVES THE CAMERA." },
    { "LOOK SPEED",    ROW_FLOAT, "gVRFpLookSpeed",          10.0f, 40.0f, 320.0f, 160.0f, NULL, NULL, -1, "DEGREES PER SECOND THE STICK TURNS YOUR VIEW." },
    { "VERTICAL LOOK", ROW_INT01, "gVRFpVerticalLook",       1.0f, 0.0f, 1.0f, 0.0f, NULL, NULL, -1, "LETS THE STICK LOOK UP AND DOWN. OFF USES YOUR HEAD." },
    { "INVERT LOOK X", ROW_INT01, "gVRFpInvertX",            1.0f, 0.0f, 1.0f, 0.0f, NULL, NULL, -1, "REVERSES LEFT AND RIGHT STICK LOOK." },
    { "EYE FORWARD",   ROW_FLOAT, "gVRFirstPersonFwd",       0.15f, -3.0f, 5.0f, 0.0f, NULL, NULL, -1, "NUDGES THE EYE AHEAD OF OR BEHIND BANJOS HEAD." },
    { "EYE RAISE",     ROW_FLOAT, "gVRFirstPersonEyeHeight", 0.05f, -1.5f, 1.5f, 0.0f, NULL, NULL, -1, "RAISES OR LOWERS YOUR EYE IN FIRST PERSON." },
    { "FP SCALE",      ROW_FLOAT, "gVRFirstPersonScale",     1.0f, 20.0f, 400.0f, 100.0f, NULL, NULL, -1, "WORLD SIZE IN FIRST PERSON ONLY." },
};

static const VrRow kRowsWorld[] = {
    { "DIORAMA SCALE",  ROW_FLOAT, "gVRDioramaWorldScale", 100.0f, 200.0f, 12000.0f, 6200.0f, NULL, NULL, -1, "HOW SMALL THE TABLETOP LEVEL IS. HIGHER IS SMALLER." },
    { "DIORAMA DIST",   ROW_FLOAT, "gVRDioramaDist",       0.05f, -1.5f, 2.0f, 0.0f, NULL, NULL, -1, "METRES THE TABLETOP SITS IN FRONT OF YOU." },
    { "DIORAMA HEIGHT", ROW_FLOAT, "gVRDioramaHeight",     0.02f, -1.5f, 1.5f, -0.06f, NULL, NULL, -1, "RAISES OR LOWERS THE TABLETOP." },
    { "SCREEN DIST",    ROW_FLOAT, "gVRMenuDist",          0.2f, 0.5f, 12.0f, 3.4f, NULL, NULL, -1, "METRES THE FLAT SCREEN AND MENUS SIT AWAY." },
    { "SCREEN SIZE",    ROW_FLOAT, "gVRMenuSize",          0.2f, 1.0f, 16.0f, 3.6f, NULL, NULL, -1, "WIDTH OF THE FLAT SCREEN AND MENUS." },
    { "HIDE HUD",       ROW_INT01, "gVRHideHud",           1.0f, 0.0f, 1.0f, 0.0f, NULL, NULL, -1, "HIDES THE GAME HUD WHILE PLAYING. MENUS STAY." },
    { "POINTER SPEED",  ROW_FLOAT, "gVRPointerSpeed",      0.1f, 0.3f, 3.0f, 1.0f, NULL, NULL, -1, "HOW FAST THE STICK GLIDES THE PORT MENU POINTER." },
};

static const VrRow kRowsRender[] = {
    { "FOG",          ROW_ENUM,  "gVRFogMode",        1.0f, 0.0f, 2.0f, 1.0f, NULL, kFogModeNames, -1, "WORLD FOG IS FIXED FOR VR. STOCK IS THE OLD ONE." },
    { "FOG START",    ROW_FLOAT, "gVRFogNear",        5.0f, 1.0f, 400.0f, 60.0f, NULL, NULL, -1, "METRES WHERE FOG BEGINS." },
    { "FOG FULL",     ROW_FLOAT, "gVRFogFar",         10.0f, 2.0f, 1200.0f, 250.0f, NULL, NULL, -1, "METRES WHERE FOG IS FULLY THICK." },
    { "DRAW DIST",    ROW_FLOAT, "gVRDrawDistance",   0.1f, 1.0f, 4.0f, 1.0f, NULL, NULL, -1, "FAR CLIP MULTIPLIER. 1 ALREADY COVERS EVERY LEVEL." },
    { "CULLING OFF",  ROW_INT01, "gVRDisableCulling", 1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "DRAWS BOTH SIDES OF WALLS. FILLS VR SEE THROUGH GAPS." },
    { "ANTI CLIP",    ROW_INT01, "gVRAntiClip",       1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "STOPS LEANING PUSHING YOUR HEAD THROUGH WALLS." },
    { "CUTSCENES 3D", ROW_INT01, "gVRCutscenes",      1.0f, 0.0f, 1.0f, 0.0f, NULL, NULL, -1, "WATCH CUTSCENES IN STEREO INSTEAD OF ON THE SCREEN." },
    { "PASSTHROUGH",  ROW_INT01, "gVRPassthrough",    1.0f, 0.0f, 1.0f, 0.0f, NULL, NULL, -1, "SHOWS YOUR ROOM BEHIND A TABLETOP LEVEL." },
};

static const VrRow kRowsSystem[] = {
    { "MOTION CTRLS",  ROW_INT01,  "gVRMotionControls", 1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "USE THE VR CONTROLLERS. OFF IS GAMEPAD ONLY." },
    { "FREE LOOK CAM", ROW_INT01,  "gEnhancements.Camera.FreeLook.Enabled", 1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "RIGHT STICK AIMS THE CAMERA. NO AUTO RECENTRE." },
    { "MOUSE LOOK",    ROW_INT01,  "gVRMouseLook",      1.0f, 0.0f, 1.0f, 1.0f, NULL, NULL, -1, "THE DESKTOP MOUSE TURNS THE CAMERA WHEN CAPTURED." },
    { "MOUSE SPEED",   ROW_FLOAT,  "gVRMouseSens",      0.02f, 0.02f, 0.60f, 0.10f, NULL, NULL, -1, "DEGREES PER MOUSE COUNT. HIGHER TURNS FASTER." },
    { "RECENTER",      ROW_ACTION, NULL, 0, 0, 0, 0, RowRecenter, NULL, -1, "BRINGS THE WORLD SQUARE TO WHERE YOU ARE LOOKING." },
    { "VR DEFAULTS",   ROW_ACTION, NULL, 0, 0, 0, 0, RowDefaults, NULL, -1, "RESTORES EVERY VR SETTING ON EVERY PAGE." },
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

// ---- state -------------------------------------------------------------------

static bool sOpen = false;
static int sPage = 0;
static int sSel = 0;
static int sRepeat = 0;
static unsigned sPrevVb = 0;

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
            const int lo = (int)row->mn, hi = (int)row->mx;
            const int span = hi - lo + 1;
            int m = CVarGetInteger(row->cvar, (int)row->def);
            do {
                m = lo + ((m - lo + dir + span) % span);
            } while (m == row->skipValue);
            CVarSetInteger(row->cvar, m);
            if (row->names == kViewModeNames) {
                vr_set_view_mode(m);
            }
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
    // One press, one consumer (the round-3 law): while the port (ImGui) menu is up, the right
    // trigger is ITS pointer click - the same press must not also toggle this overlay underneath,
    // dimming the pause backdrop. Tracking the held state through the gate means the press that
    // closes the ImGui menu cannot edge-fire here on the way out either.
    if (port_vrImGuiMenuVisible()) {
        sOpen = false;
        sPrevVb = vb;
        return;
    }
    const unsigned pressed = vb & ~sPrevVb;
    sPrevVb = vb;

    float ls[2];
    vr_controller_stick(0, ls);

    if (!sOpen) {
        // Opens from PAUSE with the right trigger. The pad merge suppresses trigger-sourced game
        // buttons for the whole pause, so this press reaches ONLY here.
        if (pressed & VR_BTN_RTRIGGER) {
            sOpen = true;
            sPage = 0;
            sSel = 0;
            sRepeat = 0;
            vr_controller_rumble(0.3f, 0.04f);
        }
        return;
    }

    // Close: B or right trigger again.
    if (pressed & (VR_BTN_B | VR_BTN_RTRIGGER)) {
        sOpen = false;
        return;
    }

    // Grips flip pages - a whole-hand gesture for a whole-page move, and a control the runtime has
    // no claim on. Selection resets to the top of the new page.
    if (pressed & VR_BTN_RGRIP) {
        sPage = (sPage + 1) % kPageCount;
        sSel = 0;
        vr_controller_rumble(0.25f, 0.03f);
    } else if (pressed & VR_BTN_LGRIP) {
        sPage = (sPage + kPageCount - 1) % kPageCount;
        sSel = 0;
        vr_controller_rumble(0.25f, 0.03f);
    }

    const VrPage* page = &kPages[sPage];

    if (sRepeat > 0) {
        sRepeat--;
    }
    const bool up = ls[1] > 0.55f;
    const bool down = ls[1] < -0.55f;
    const bool left = ls[0] < -0.55f;
    const bool right = ls[0] > 0.55f;

    if ((up || down) && sRepeat == 0) {
        sSel = (sSel + (down ? 1 : page->rowCount - 1)) % page->rowCount;
        sRepeat = 8;
    } else if ((left || right) && sRepeat == 0) {
        AdjustRow(&page->rows[sSel], right ? 1 : -1);
        sRepeat = 6;
    } else if (!up && !down && !left && !right && sRepeat > 2) {
        sRepeat = 2; // quick re-arm on release
    }

    if (pressed & VR_BTN_A) {
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
    const int rowH = 13;
    int y = 55;

    const VrPage* page = &kPages[sPage];
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

    for (int i = 0; i < page->rowCount; i++) {
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
    print_dialog(x0, 199, (unsigned char*)"GRIPS TURN PAGE. B CLOSES.");
    text_setNormalTextColor(0xFF, 0xFF, 0xFF);
}

#else // no VR layer - stubs

extern "C" int port_vrNativeMenu_isOpen(void) {
    return 0;
}
extern "C" void port_vrNativeMenu_push(void* gfx) {
    (void)gfx;
}

#endif
