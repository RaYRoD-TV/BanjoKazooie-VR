// Enhancements -> VR: every VR tunable as a LIVE slider or toggle. vr.cpp re-reads these CVars each
// frame, so a change applies on the next rendered frame with the headset on - which is the only way to
// tune framing, since none of it can be judged from the desktop.
//
// The whole page is compiled out without ENABLE_VR.

#include "LighthouseMenu.h"

#if defined(ENABLE_VR) && defined(_WIN32)
#include "port/vr/vr.h"
#endif

namespace LighthouseGui {

using namespace UIWidgets;

void LighthouseMenu::AddMenuVR() {
#if defined(ENABLE_VR) && defined(_WIN32)
    WidgetPath path = { "Enhancements", "VR", SECTION_COLUMN_1 };
    AddSidebarEntry("Enhancements", path.sidebarName, 2);
    path.column = SECTION_COLUMN_1;

    AddWidget(path, "View", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "View Mode", WIDGET_CVAR_COMBOBOX)
        .CVar("gVRViewMode")
        .RaceDisable(false)
        .Options(ComboboxOptions()
                     .Tooltip("Third Person: the game's chase camera in life-size stereo.\n"
                              "First Person: the eye pushed forward to Banjo's head.\n"
                              "Diorama: the level shrunk to a tabletop you lean around.\n"
                              "Theater: the flat frame on a virtual screen (most comfortable).\n\n"
                              "Right stick click cycles these in-game.")
                     .ComboMap({
                         { VR_VIEW_FIRST_PERSON, "First Person" },
                         { VR_VIEW_THIRD_PERSON, "Third Person" },
                         { VR_VIEW_DIORAMA, "Diorama" },
                         { VR_VIEW_THEATER, "Theater" },
                     }));

    AddWidget(path, "World Scale", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRWorldScale")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(20.0f).Max(400.0f).Step(1.0f).DefaultValue(100.0f).Tooltip(
            "Game units per metre - how big the world feels in Third Person. LOWER makes the world "
            "bigger. Tune this first: get Banjo reading bear-sized and everything else follows."));

    AddWidget(path, "Stereo Depth", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRStereo")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(0.0f).Max(1.0f).Step(0.05f).DefaultValue(1.0f).Tooltip(
            "Eye separation strength. 0 is flat (mono), 1 is full IPD. Lower if the image is hard to "
            "fuse; this only changes separation, never the size of the world."));

    AddWidget(path, "Eye Height", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVREyeHeight")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(-1.5f).Max(1.5f).Step(0.01f).DefaultValue(-1.09f).Tooltip(
            "Metres to raise or lower the eye in Third Person."));

    AddWidget(path, "Camera Distance", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRThirdPersonDist")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(0.3f).Max(3.0f).Step(0.05f).DefaultValue(0.7f).Tooltip(
            "How far the chase camera sits from Banjo, as a multiple of the game's own distance. "
            "1.0 is stock, 0.5 is right behind his shoulder, 2.0 is a wide view. Scales the "
            "distance the game itself asks for, so wall collision keeps working normally."));

    AddWidget(path, "First Person", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "First Person Forward", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRFirstPersonFwd")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(-3.0f).Max(5.0f).Step(0.15f).DefaultValue(0.0f).Tooltip(
            "Fine-tune nudge forward (+) or back (-) from Banjo's eyes in First Person. The camera "
            "already sits at his head; leave at 0 unless the framing feels off."));

    AddWidget(path, "First Person Scale", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRFirstPersonScale")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(20.0f).Max(400.0f).Step(1.0f).DefaultValue(100.0f).Tooltip(
            "First Person has its own world scale, so tuning it never changes Third Person."));

    AddWidget(path, "Vertical Look (Right Stick)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVRFpVerticalLook")
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Whether the right stick may pitch the First Person view up/down. Off = vertical stays "
            "on your head only; the stick still turns left/right either way."));

    AddWidget(path, "Flip Cam", WIDGET_CVAR_CHECKBOX)
        .CVar("gVRFpFlipCam")
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "The view somersaults with Banjo in First Person: a full turn on the flip jump (Z then A) "
            "and a tuck into the beak buster (A then Z), driven by the move's own animation so it "
            "matches exactly. Turn off to keep the view level through every move."));

    AddWidget(path, "Underwater A Dashes", WIDGET_CVAR_CHECKBOX)
        .CVar("gVRSwimADash")
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Swaps the two underwater strokes so A is the fast beak dash and B the slow controlled "
            "paddle. Off restores the stock layout (A paddles, B dashes). Diving from the surface "
            "is always A either way."));

    AddWidget(path, "Immersive Camera", WIDGET_CVAR_CHECKBOX)
        .CVar("gVRFpImmersive")
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "The full first-person feel in one switch: your head IS Banjo's head. Lean to peek "
            "around corners, crouch to duck, and walk where you look. Off falls back to plain "
            "base-relative movement with no physical lean."));

    AddWidget(path, "Lean Amount", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRHeadScale")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(0.0f).Max(1.5f).Step(0.05f).DefaultValue(0.1f).Tooltip(
            "How much your real body movement moves the camera. 1.0 is life-size (lean a foot, the "
            "view moves a foot); 0 pins the view to Banjo's head bone."));

    AddWidget(path, "Look Speed", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRFpLookSpeed")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(40.0f).Max(320.0f).Step(10.0f).DefaultValue(160.0f).Tooltip(
            "Degrees per second the right stick turns the First Person view at full deflection."));

    AddWidget(path, "Invert Look (X Axis)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVRFpInvertX")
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Reverses left/right stick look in First Person."));

    AddWidget(path, "Swim Follow", WIDGET_CVAR_CHECKBOX)
        .CVar("gVRFpSwimFollow")
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "In water, the First Person view turns with Banjo while you steer with the left "
            "stick - so the thumb holding the swim buttons never has to leave them. Only ever "
            "moves while you are commanding the turn; on land the view base stays put."));

    AddWidget(path, "Head-Directed Movement", WIDGET_CVAR_CHECKBOX)
        .CVar("gVRFpHeadMove")
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Stick-forward walks where your HEAD is looking, not where the view base points - the "
            "immersive First Person feel from the Mario 64 port. Turn off for strict base-relative "
            "movement."));

    AddWidget(path, "First Person Eye Height", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRFirstPersonEyeHeight")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(-1.5f).Max(1.5f).Step(0.05f).DefaultValue(0.0f).Tooltip(
            "Metres to raise or lower the eye in First Person."));

    AddWidget(path, "Diorama", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Diorama Scale", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRDioramaWorldScale")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(200.0f).Max(12000.0f).Step(50.0f).DefaultValue(8000.0f).Tooltip(
            "Game units per metre for the tabletop. HIGHER makes the miniature smaller."));

    AddWidget(path, "Diorama Distance", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRDioramaDist")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(-1.5f).Max(2.0f).Step(0.05f).DefaultValue(0.05f).Tooltip(
            "Metres the tabletop sits in front of you."));

    AddWidget(path, "Diorama Height", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRDioramaHeight")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(-1.5f).Max(1.5f).Step(0.02f).DefaultValue(-0.4f).Tooltip(
            "Metres the tabletop sits above (+) or below (-) eye level."));

    path.column = SECTION_COLUMN_2;

    AddWidget(path, "HUD & Menus", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "HUD Size", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRHudScale")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(0.1f).Max(1.5f).Step(0.05f).DefaultValue(0.35f).Tooltip(
            "How much of your view the in-game HUD fills. Smaller is tighter and more central."));

    AddWidget(path, "HUD Distance", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRHudDist")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(0.3f).Max(20.0f).Step(0.1f).DefaultValue(2.9f).Tooltip(
            "Metres the HUD plane sits in front of you. Pushing it out genuinely makes it recede - it "
            "is a real object at that distance, not a decal on your face."));

    AddWidget(path, "Screen Distance", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRMenuDist")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(0.5f).Max(12.0f).Step(0.1f).DefaultValue(3.4f).Tooltip(
            "Metres the flat screen sits in front of you (Theater, the file select, and cutscenes)."));

    AddWidget(path, "Screen Size", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRMenuSize")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(1.0f).Max(16.0f).Step(0.1f).DefaultValue(3.6f).Tooltip(
            "Width in metres of that flat screen."));

    AddWidget(path, "Rendering", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Fog", WIDGET_CVAR_COMBOBOX)
        .CVar("gVRFogMode")
        .RaceDisable(false)
        .Options(ComboboxOptions()
                     .Tooltip("The N64 computes fog from PROJECTED depth, which the per-eye projection "
                              "breaks - stock fog saturates and drowns the view in stereo. World "
                              "Distance recomputes it from true view distance instead.")
                     .ComboMap({
                         { 0, "Off" },
                         { 1, "World Distance" },
                         { 2, "Stock (broken in stereo)" },
                     }));

    AddWidget(path, "Fog Start", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRFogNear")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(1.0f).Max(400.0f).Step(1.0f).DefaultValue(60.0f).Tooltip(
            "Metres (at life size) where fog begins."));

    AddWidget(path, "Fog Full", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRFogFar")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(2.0f).Max(1200.0f).Step(1.0f).DefaultValue(250.0f).Tooltip(
            "Metres (at life size) where fog is fully opaque."));

    AddWidget(path, "Draw Distance", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRDrawDistance")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(1.0f).Max(4.0f).Step(0.1f).DefaultValue(4.0f).Tooltip(
            "Multiplies the far clip plane. The base is already 2 km - ten times the biggest world - "
            "so nothing in the game ever clips even at 1.0; maxed by default because the headroom "
            "is free."));

    AddWidget(path, "Anti-Aliasing (MSAA)", WIDGET_CVAR_COMBOBOX)
        .CVar("gMSAAValue")
        .RaceDisable(false)
        .Options(ComboboxOptions()
                     .Tooltip("Multisampling for the headset eyes and the flat window alike. The eye "
                              "render used to ship without it, which is why edges crawled in VR while "
                              "the desktop looked fine.")
                     .ComboMap({
                         { 1, "Off" },
                         { 2, "2x" },
                         { 4, "4x" },
                         { 8, "8x" },
                     }));

    AddWidget(path, "Menu Pointer Speed", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRPointerSpeed")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(0.3f).Max(3.0f).Step(0.1f).DefaultValue(1.0f).Tooltip(
            "How fast the right stick glides the menu pointer while this menu is open in the "
            "headset."));

    AddWidget(path, "Anti-Clip (Third Person / Diorama)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVRAntiClip")
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Stops your head passing through walls by easing off physical lean exactly when the "
            "game's own camera collision pulls in close to Banjo. Full lean in the open, tightened "
            "in corridors, and it recovers smoothly - no popping, no second collision system."));

    AddWidget(path, "Disable Backface Culling", WIDGET_CVAR_CHECKBOX)
        .CVar("gVRDisableCulling")
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Draws both sides of every surface during the eye passes, filling the see-through holes a "
            "wide VR view exposes. Only affects the headset render - the flat game keeps stock "
            "culling. Turn off if some geometry draws in the wrong order."));

    AddWidget(path, "Behaviour", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Hide HUD", WIDGET_CVAR_CHECKBOX)
        .CVar("gVRHideHud")
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Drops the honeycombs, counters and every other HUD element from the headset view for a "
            "clean look at the world. Dialogue boxes ride the same layer and hide too."));

    AddWidget(path, "Motion Controllers", WIDGET_CVAR_CHECKBOX)
        .CVar("gVRMotionControls")
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "Merge the VR controllers into the pad. Turn off for pure gamepad play."));

    AddWidget(path, "Mouse Look", WIDGET_CVAR_CHECKBOX)
        .CVar("gVRMouseLook")
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(true).Tooltip(
            "The desktop mouse turns the camera while it is captured: Free Look orbit in Third "
            "Person, the view base in First Person. Raw 1:1 response, the sm64coopdx / SRB2 feel."));

    AddWidget(path, "Mouse Sensitivity", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRMouseSens")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(0.02f).Max(0.60f).Step(0.02f).DefaultValue(0.10f).Tooltip(
            "Degrees of turn per mouse count. Higher is faster."));

    AddWidget(path, "Menu Opacity", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRMenuOpacity")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(0.3f).Max(1.0f).Step(0.05f).DefaultValue(0.85f).Tooltip(
            "How solid the menus and pause panel look in the headset. Lower shows the live game "
            "through them."));

    AddWidget(path, "Settings Menu Opacity", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gVRImGuiOpacity")
        .RaceDisable(false)
        .Options(FloatSliderOptions().Min(0.3f).Max(1.0f).Step(0.05f).DefaultValue(0.95f).Tooltip(
            "How solid THIS settings menu looks in the headset. Separate from the game menus so "
            "you can keep the game readable behind it while tuning."));

    AddWidget(path, "First Person View Bob", WIDGET_CVAR_CHECKBOX)
        .CVar("gVRFpViewBob")
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "A small stride bob while walking and a dip on landing, driven by Banjo's real motion. "
            "Centimetres, not camera waggle - off by default; turn on if you like the extra life."));

    AddWidget(path, "Stereo Cutscenes", WIDGET_CVAR_CHECKBOX)
        .CVar("gVRCutscenes")
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Watch cutscenes in full stereo instead of on the flat screen. Off by default: their "
            "scripted camera sweeps are authored for a flat screen and can read as motion sickness."));

    AddWidget(path, "Mixed Reality (Passthrough)", WIDGET_CVAR_CHECKBOX)
        .CVar("gVRPassthrough")
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Composite your real room behind the game. Locks to Diorama - the level becomes a "
            "miniature sitting in your room. Needs runtime passthrough support."));

    AddWidget(path, "Recenter View", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([](WidgetInfo& info) { vr_recenter(); })
        .Options(ButtonOptions().Size(Sizes::Inline).Tooltip(
            "Bring the world square to your gaze. Also available in-headset by holding BOTH TRIGGERS "
            "for about half a second (a haptic tick confirms it)."));

    AddWidget(path, "Reset VR Defaults", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([](WidgetInfo& info) { vr_reset_defaults(); })
        .Options(ButtonOptions().Size(Sizes::Inline).Tooltip("Restore the shipped VR framing values."));
#endif
}

} // namespace LighthouseGui
