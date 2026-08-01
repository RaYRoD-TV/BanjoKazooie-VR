#include "Engine.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <future>
#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif
#include "PR/libaudio.h"
#include <libultraship/libultraship.h>

#include <fast/Fast3dWindow.h>
#include <fast/interpreter.h>
#include "fast/resource/ResourceType.h"
#include <fast/resource/factory/DisplayListFactory.h>
#include <fast/resource/factory/TextureFactory.h>
#include <fast/resource/factory/MatrixFactory.h>
#include <fast/resource/factory/VertexFactory.h>
#include <libultraship/bridge/gfxbridge.h>
#include <libultraship/controller/controldeck/ControlDeck.h>
#include <libultraship/libultra/AudioDmaRegistry.h>
#include <SDL2/SDL.h>
#include <ship/controller/controldevice/controller/mapping/ControllerDefaultMappings.h>
#include <ship/resource/factory/BlobFactory.h>
#include <ship/resource/type/Blob.h>
#include <ship/utils/StringHelper.h>
#include <ship/window/gui/Fonts.h>
#include <ship/window/gui/resource/Font.h>

#include "Audio/GameAudio.h"
#include "build.h"
#include "Extractor/GameExtractor.h"
#include "ship/window/gui/FileBrowserWindow.h"
#include "Interpolation/FrameInterpolation.h"
#include "OS/OS.h"
#if defined(ENABLE_VR) && defined(_WIN32)
#include "vr/vr.h"
#include <imgui.h> // the in-headset menu machinery below feeds ImGui nav/pointer events directly
#include <backends/imgui_impl_sdl2.h> // gamepad-mode control (see the VR menu block)
// True when this frame should render in per-eye STEREO rather than on the flat head-locked panel.
// Defined in vr/VrGame.cpp next to the rest of the game-side VR glue.
extern "C" bool VrGame_StereoActive(void);
extern "C" bool VrGame_StereoEligible(void); // same gate minus the live-session check (headless eye dumps)
extern "C" void VrGame_CycleViewMode(void);

// --- ImGui menu in the headset -------------------------------------------------------------------
// The port menu is the whole VR tuning workflow, so it has to be USABLE from inside the headset:
// L3 toggles it (libultraship's Gui::StartFrame reads ImGuiKey_GamepadBack as the menu toggle), the
// RIGHT stick glides a virtual mouse pointer with the RIGHT trigger as left-click, the LEFT stick
// navigates widgets, and the whole menu renders into a stable offscreen texture that vr.cpp presents
// on the head-locked panel. Regular gamepads drive the same paths so the menu works identically with
// the motion controllers asleep in their dock.

// Feed ImGui's menu gamepad navigation directly from the SDL controllers, INDEPENDENT of OS window
// focus. In VR the headset compositor holds focus, so the desktop SDL window is "background" and
// ImGui's SDL2 backend auto-read returns 0 for every button - the menu renders but gets no nav input.
// SDL_GameControllerUpdate() force-refreshes pad state regardless of focus, then we push the exact
// ImGuiKey_Gamepad* events ImGui needs. Coexists with the auto-read (same keys agree).
static void VrFeedImGuiGamepadNav() {
    SDL_GameControllerUpdate();
    SDL_GameController* pads[8];
    int padCount = 0;
    for (int i = 0, n = SDL_NumJoysticks(); i < n && padCount < 8; i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameController* gc = SDL_GameControllerOpen(i); // ref-counted; same handles ControlDeck holds
            if (gc) {
                pads[padCount++] = gc;
            }
        }
    }
    if (padCount == 0) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    auto btn = [&](ImGuiKey key, SDL_GameControllerButton b) {
        bool down = false;
        for (int p = 0; p < padCount; p++) {
            down |= SDL_GameControllerGetButton(pads[p], b) != 0;
        }
        io.AddKeyEvent(key, down);
    };
    auto axis = [&](ImGuiKey key, SDL_GameControllerAxis a, int lo, int hi) {
        float best = 0.0f;
        for (int p = 0; p < padCount; p++) {
            float v = (float)(SDL_GameControllerGetAxis(pads[p], a) - lo) / (float)(hi - lo);
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            if (v > best) {
                best = v;
            }
        }
        io.AddKeyAnalogEvent(key, best > 0.1f, best);
    };
    const int dz = 8000;
    // Widget-focus nav on the hardware d-pad only - the left stick owns the pointer cursor.
    btn(ImGuiKey_GamepadDpadLeft, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    btn(ImGuiKey_GamepadDpadRight, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    btn(ImGuiKey_GamepadDpadUp, SDL_CONTROLLER_BUTTON_DPAD_UP);
    btn(ImGuiKey_GamepadDpadDown, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    btn(ImGuiKey_GamepadFaceDown, SDL_CONTROLLER_BUTTON_A);
    btn(ImGuiKey_GamepadFaceRight, SDL_CONTROLLER_BUTTON_B);
    btn(ImGuiKey_GamepadFaceLeft, SDL_CONTROLLER_BUTTON_X);
    btn(ImGuiKey_GamepadFaceUp, SDL_CONTROLLER_BUTTON_Y);
    btn(ImGuiKey_GamepadL1, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    btn(ImGuiKey_GamepadR1, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
}

// Stick-click switches CHATTER: one physical press can bounce through several down-edges in a few
// frames, and every edge is a full toggle - the menu strobes open/closed. Debounce: accept a rising
// edge, then ignore further rising edges for ~1/3 s. Falling edges always pass so fed key state
// stays consistent.
#define VR_STICK_CLICK_DEBOUNCE_TICKS 20

// Regular gamepads get the same two stick-click shortcuts as the motion controllers: left stick
// click toggles the port menu (ImGuiKey_GamepadBack - libultraship's menu toggle), right stick click
// cycles the view mode. Focus-independent and edge-driven.
static void VrSdlPadStickClicks(bool menuOpen) {
    static bool sPrevL = false, sPrevR = false;
    static int sLDebounce = 0, sRDebounce = 0;
    SDL_GameControllerUpdate();
    bool l = false, r = false;
    for (int i = 0, n = SDL_NumJoysticks(); i < n; i++) {
        if (SDL_IsGameController(i)) {
            if (SDL_GameController* gc = SDL_GameControllerOpen(i)) {
                l |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0;
                r |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK) != 0;
            }
        }
    }
    if (sLDebounce > 0) {
        sLDebounce--;
    }
    if (sRDebounce > 0) {
        sRDebounce--;
    }
    if (l != sPrevL) {
        if (l && sLDebounce == 0) {
            ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;
            ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadBack, true);
            sLDebounce = VR_STICK_CLICK_DEBOUNCE_TICKS;
        } else if (!l) {
            ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;
            ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadBack, false);
        }
    }
    if (r && !sPrevR && !menuOpen && sRDebounce == 0) {
        VrGame_CycleViewMode();
        sRDebounce = VR_STICK_CLICK_DEBOUNCE_TICKS;
    }
    sPrevL = l;
    sPrevR = r;
}

// Motion controllers drive the ImGui menu: L3 toggles it, and with it open the left stick navigates,
// A / right trigger activates, B backs out.
static void VrFeedImGuiFromVrControllers(bool menuOpen) {
    // MOTION CTRLS off: docked controllers must not toggle or drive the menu (stick drift and resting
    // triggers read as phantom input); the SDL pad + mouse paths cover everything.
    if (!vr_controllers_active() || !CVarGetInteger("gVRMotionControls", 1)) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    unsigned vb = vr_controller_buttons();
    {
        static bool sPrevBack = false;
        static int sBackDebounce = 0;
        const bool back = (vb & VR_BTN_LSTICK) != 0;
        if (sBackDebounce > 0) {
            sBackDebounce--;
        }
        if (back != sPrevBack) {
            if (back && sBackDebounce == 0) {
                io.AddKeyEvent(ImGuiKey_GamepadBack, true);
                sBackDebounce = VR_STICK_CLICK_DEBOUNCE_TICKS;
            } else if (!back) {
                io.AddKeyEvent(ImGuiKey_GamepadBack, false);
            }
            sPrevBack = back;
        }
    }
    if (!menuOpen) {
        return;
    }
    // A and B are deliberately NOT fed as nav-activate keys here. With nav switched off for the
    // pointer they cannot activate anything cleanly, and ImGui still reacts to a half-recognised
    // FaceDown on a focused slider - which is the "A acts weird on sliders" report. A is a POINTER
    // CLICK instead (below, alongside X and the right trigger), so every button that looks like
    // "press this" does the same, predictable thing.
    // The left stick deliberately feeds NO nav keys here: it drives the pointer cursor
    // (VrControllerImGuiMousePos), and a stick that both moves the cursor and shifts widget focus
    // fights itself. Widget-focus navigation stays on the hardware d-pad.
}

// Mouse position for ImGui INDEPENDENT of OS focus/hover, so the software cursor always renders on
// the VR menu panel: the SDL2 backend pushes a mouse-leave (-FLT_MAX) the moment the OS cursor drifts
// off the unfocused game window. Read the global mouse and clamp into the window rect instead.
static bool VrComputeImGuiMousePos(float* outX, float* outY) {
    SDL_Window* win = SDL_GL_GetCurrentWindow();
    if (win == nullptr) {
        return false;
    }
    int gx = 0, gy = 0, wx = 0, wy = 0, ww = 0, wh = 0;
    SDL_GetGlobalMouseState(&gx, &gy);
    SDL_GetWindowPosition(win, &wx, &wy);
    SDL_GetWindowSize(win, &ww, &wh);
    int cx = gx, cy = gy;
    if (cx < wx) {
        cx = wx;
    } else if (ww > 0 && cx > wx + ww - 1) {
        cx = wx + ww - 1;
    }
    if (cy < wy) {
        cy = wy;
    } else if (wh > 0 && cy > wy + wh - 1) {
        cy = wy + wh - 1;
    }
    const bool multiViewport = (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0;
    *outX = (float)(multiViewport ? cx : cx - wx);
    *outY = (float)(multiViewport ? cy : cy - wy);
    return true;
}

// Whether the pointer's button is held this frame (right trigger or X, pad or motion controller).
// Forced into io.MouseDown after NewFrame so a drag cannot be cancelled by the SDL backend's own
// mouse events while the game window sits unfocused behind the headset.
static bool sVrPointerHeld = false;

// VR controller as a MOUSE POINTER: the LEFT thumbstick glides a virtual cursor; the RIGHT TRIGGER
// or X left-clicks and HOLDS - what makes the menu usable in the headset (there is no OS mouse
// there). Regular gamepads drive the same cursor. The desktop mouse shares the cursor (latest mover
// wins) so an idle pad never locks the real mouse out.
static bool VrControllerImGuiMousePos(float* outX, float* outY) {
    static float cx = -1.0f, cy = -1.0f;
    SDL_Window* win = SDL_GL_GetCurrentWindow();
    if (win == nullptr) {
        return false;
    }
    int wx = 0, wy = 0, ww = 0, wh = 0;
    SDL_GetWindowPosition(win, &wx, &wy);
    SDL_GetWindowSize(win, &ww, &wh);
    if (ww <= 0 || wh <= 0) {
        return false;
    }
    if (cx < 0.0f) {
        cx = ww * 0.5f;
        cy = wh * 0.5f;
    }
    float rs[2] = { 0.0f, 0.0f };
    bool haveSource = false;
    const bool vrCtl = vr_controllers_active() && CVarGetInteger("gVRMotionControls", 1) != 0;
    if (vrCtl) {
        vr_controller_stick(0, rs); // LEFT stick drives the pointer (round-5 tester preference)
        haveSource = true;
    }
    bool padClick = false;
    SDL_GameControllerUpdate();
    for (int i = 0, n = SDL_NumJoysticks(); i < n; i++) {
        if (!SDL_IsGameController(i)) {
            continue;
        }
        SDL_GameController* gc = SDL_GameControllerOpen(i);
        if (gc == nullptr) {
            continue;
        }
        haveSource = true;
        float px = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
        float py = -(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f);
        if (fabsf(px) > fabsf(rs[0])) {
            rs[0] = px;
        }
        if (fabsf(py) > fabsf(rs[1])) {
            rs[1] = py;
        }
        padClick |= SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000;
        padClick |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X) != 0;
        padClick |= SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A) != 0;
    }
    if (!haveSource) {
        return false; // no pointer device - let the desktop mouse drive
    }
    bool mouseDown = false;
    {
        static bool sMouseSeen = false;
        static int sPrevGX = 0, sPrevGY = 0;
        int gx = 0, gy = 0;
        const Uint32 mb = SDL_GetGlobalMouseState(&gx, &gy);
        if (sMouseSeen && (gx != sPrevGX || gy != sPrevGY)) {
            cx = (float)(gx - wx);
            cy = (float)(gy - wy);
        }
        sPrevGX = gx;
        sPrevGY = gy;
        sMouseSeen = true;
        mouseDown = (mb & SDL_BUTTON_LMASK) != 0 && gx >= wx && gx < wx + ww && gy >= wy && gy < wy + wh;
    }
    // Pointer speed is TIME-BASED, in screen-widths per second. This matters more than the
    // constant did: this function is called once per interpolation SUB-FRAME, so a fixed
    // per-call step multiplied by the sub-frame count AND by the headset refresh - the reason
    // tuning the old constant barely moved the feel. Quadratic response keeps fine aim near
    // centre while full tilt still crosses the screen in about two and a half seconds.
    const float dt = ImGui::GetIO().DeltaTime > 0.0f ? ImGui::GetIO().DeltaTime : (1.0f / 60.0f);
    const float speed = (float)ww * 0.40f * CVarGetFloat("gVRPointerSpeed", 1.0f) * dt;
    const float kPtrDead = 0.20f;
    if (rs[0] > kPtrDead || rs[0] < -kPtrDead) {
        cx += rs[0] * fabsf(rs[0]) * speed;
    }
    if (rs[1] > kPtrDead || rs[1] < -kPtrDead) {
        cy -= rs[1] * fabsf(rs[1]) * speed;
    }
    if (cx < 0.0f) {
        cx = 0.0f;
    } else if (cx > ww - 1) {
        cx = (float)(ww - 1);
    }
    if (cy < 0.0f) {
        cy = 0.0f;
    } else if (cy > wh - 1) {
        cy = (float)(wh - 1);
    }
    // X or the right trigger HOLDS the mouse button down, so a slider can be grabbed and dragged
    // rather than only clicked. The held state is published for the draw side: forcing MouseDown
    // after NewFrame is what makes the hold survive the SDL backend's own (unfocused-window)
    // mouse events, which otherwise release the button mid-drag.
    {
        static bool sPrevClick = false;
        const unsigned vbNow = vr_controller_buttons();
        bool click = (vrCtl && ((vbNow & VR_BTN_RTRIGGER) != 0 || (vbNow & VR_BTN_X) != 0 ||
                                (vbNow & VR_BTN_A) != 0)) ||
                     padClick || mouseDown;
        sVrPointerHeld = click;
        if (click != sPrevClick) {
            ImGui::GetIO().AddMouseButtonEvent(0, click);
            sPrevClick = click;
        }
    }
    const bool multiViewport = (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0;
    *outX = multiViewport ? (cx + wx) : cx;
    *outY = multiViewport ? (cy + wy) : cy;
    return true;
}
#endif
#include "Network/Anchor/Anchor.h"
#include "port/Enhancements/Events/PortEnhancements.h"
#include "port/Patches/Patches.h"
#include "port/Save/SaveManager.h"
#include "port/UI/cvar_prefixes.h"
#include "ResourceHelpers.h"
#include "Localization/Language.h"
#include "Resource/Importers/AnimFactory.h"
#include "Resource/Importers/DemoInputFactory.h"
#include "Resource/Importers/DialogFactory.h"
#include "Resource/Importers/MapFactory.h"
#include "Resource/Importers/ModelFactory.h"
#include "Resource/Importers/SpriteFactory.h"
#include "src/port/Enhancements/Events/Hooks/Events.h"
#include "UI/LighthouseGui.hpp"
#include "UI/LighthouseModMenuWindow.h"

#ifdef __SWITCH__
#include <port/switch/SwitchImpl.h>
#endif

const float imguiScaleOptionToValue[4] = { 0.75f, 1.0f, 1.5f, 2.0f };
std::shared_ptr<Fast::Fast3dWindow> lhFast3dWindow;
const uint32_t defaultImGuiScale = 1;
int32_t previousImGuiScaleIndex = -1;
float previousImGuiScale = defaultImGuiScale;
bool portArchiveVersionMatch = false;
std::string assets_path;

namespace fs = std::filesystem;

extern "C" {

// Reset support
extern s32 D_80275610;

bool prevAltAssets = false;
// bool gEnableGammaBoost = true;

// Game mode helper
bool func_802E4A08(void);

// Soundfont ROM symbols - loaded from OTR in LoadSoundfonts()
u8* soundfont1ctl_ROM_START = NULL;
u8* soundfont1ctl_ROM_END = NULL;
u8* soundfont1tbl_ROM_START = NULL;
u8* soundfont2ctl_ROM_START = NULL;
u8* soundfont2ctl_ROM_END = NULL;
u8* soundfont2tbl_ROM_START = NULL;
}

std::vector<uint8_t*> MemoryPool;
GameEngine* GameEngine::Instance;

typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} OTRVersion;

// Read the port version from an OTR file
OTRVersion ReadPortVersionFromOTR(std::string otrPath) {
    OTRVersion version = {};

    // Use a temporary archive instance to load the otr and read the version file
    auto archive = std::make_shared<Ship::O2rArchive>(otrPath);
    if (archive->Open()) {
        auto t = archive->LoadFile("portVersion");
        if (t != nullptr && t->IsLoaded) {
            auto stream = std::make_shared<Ship::MemoryStream>(t->Buffer->data(), t->Buffer->size());
            auto reader = std::make_shared<Ship::BinaryReader>(stream);
            reader->SetEndianness(Ship::Endianness::Big);
            version.major = reader->ReadUInt16();
            version.minor = reader->ReadUInt16();
            version.patch = reader->ReadUInt16();
        } else {
            SPDLOG_WARN("Failed to read portVersion file from O2R: {}", otrPath);
        }
    } else {
        SPDLOG_WARN("Failed to open O2R for version reading: {}", otrPath);
    }

    return version;
}

// Reads the port version recorded in the named o2r. A missing file yields INT16_MAX in every
// field; an o2r that opens without a portVersion record yields zeros.
OTRVersion DetectOTRVersion(std::string fileName) {
    std::string otrPath = Ship::Context::LocateFileAcrossAppDirs(fileName);

    // Doesn't exist so nothing to do here
    if (!std::filesystem::exists(otrPath)) {
        SPDLOG_WARN("O2R file not found at path: {}", otrPath);
        return { INT16_MAX, INT16_MAX, INT16_MAX };
    }

    return ReadPortVersionFromOTR(otrPath);
}

bool VerifyArchiveVersion(OTRVersion version) {
    return version.major == gBuildVersionMajor && version.minor == gBuildVersionMinor;
}

GameEngine::GameEngine() {
    this->context = Ship::Context::CreateUninitializedInstance("Lighthouse", "bk", "lighthouse.cfg.json");

#ifdef __SWITCH__
    Ship::Switch::Init(Ship::PreInitPhase);
    Ship::Switch::Init(Ship::PostInitPhase);
#endif

    this->context->InitConfiguration();    // without this line InitConsoleVariables fails at Config::Reload()
    this->context->InitConsoleVariables(); // without this line the controldeck constructor failes in
    // ShipDeviceIndexMappingManager::UpdateControllerNamesFromConfig()

#if defined(ENABLE_VR) && defined(_WIN32)
    // VR: SDL_main already decided whether to enable VR (--vr / --novr / headset auto-detect) and
    // called vr_request_enable(). OpenXR binds to the WGL context, so force the OpenGL backend before
    // the window is created (InitWindow below reads Window.Backend.Id to choose the renderer) -
    // Lighthouse defaults to DX11, which has no XR path.
    // BK_VR_EYEDUMP=<n> is the headless verification seam: the per-eye render runs with synthetic
    // matrices (no headset, no OpenXR session) and dumps eye frames to BMPs - it needs the GL
    // backend for the same reason VR does.
    if (vr_is_requested() || getenv("BK_VR_EYEDUMP") != nullptr) {
        this->context->GetConfig()->SetInt("Window.Backend.Id", (int32_t)Fast::WindowBackend::FAST3D_SDL_OPENGL);
        this->context->GetConfig()->SetString("Window.Backend.Name", "OpenGL");
        SPDLOG_INFO("[VR] requested - forced OpenGL backend (OpenXR binds to WGL)");
        // Gamepad menu navigation - the mouse cursor isn't usable in the headset, so the menu must be
        // drivable with the controller.
        CVarSetInteger(CVAR_IMGUI_CONTROLLER_NAV, 1);
        // VR steals OS focus to the compositor, so the desktop SDL window runs in the background.
        // Without this, SDL stops updating gamepad state for an unfocused window and menu nav goes dead.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        CVarSave();
    }
#endif

    assets_path = Ship::Context::LocateFileAcrossAppDirs("lighthouse.o2r");
    portArchiveVersionMatch = std::filesystem::exists(assets_path); // TODO: port archive versioning

    auto controlDeck = std::make_shared<LUS::ControlDeck>();

    this->context->InitControlDeck(controlDeck);
    this->context->InitResourceManager({ assets_path }, {}, 3, true);
    this->context->InitConsole();

    // Register console commands for menu buttons
    Ship::Context::GetRawInstance()->GetConsole()->AddCommand(
        "reset", { [](std::shared_ptr<Ship::Console>, const std::vector<std::string>&, std::string*) -> bool {
                      gPortResetPending = 1; // lets audio spin-waits exit immediately
                      setBootMap(getDefaultBootMap());
                      D_80275610 = 3 + 1; // deferred: mainLoop picks this up next frame
                      CALL_EVENT(OnReset);
                      return 0;
                  },
                   "Reset to boot map." });
    Ship::Context::GetRawInstance()->GetConsole()->AddCommand(
        "quit", { [](std::shared_ptr<Ship::Console>, const std::vector<std::string>&, std::string*) -> bool {
                     Ship::Context::GetRawInstance()->GetWindow()->Close();
                     return 0;
                 },
                  "Quit the game." });

    lhFast3dWindow = std::make_shared<Fast::Fast3dWindow>(std::vector<std::shared_ptr<Ship::GuiWindow>>({}));
    this->context->InitWindow(lhFast3dWindow);

    LighthouseGui::SetupMenu();

    if (portArchiveVersionMatch) {
        fontMono = CreateFontWithSize(16.0f, "fonts/Inconsolata-Regular.ttf");
        fontMonoLarger = CreateFontWithSize(20.0f, "fonts/Inconsolata-Regular.ttf");
        fontMonoLargest = CreateFontWithSize(24.0f, "fonts/Inconsolata-Regular.ttf");
        fontStandard = CreateFontWithSize(16.0f, "fonts/Montserrat-Regular.ttf");
        fontStandardLarger = CreateFontWithSize(20.0f, "fonts/Montserrat-Regular.ttf");
        fontStandardLargest = CreateFontWithSize(24.0f, "fonts/Montserrat-Regular.ttf");
        ImGui::GetIO().FontDefault = fontStandardLarger;
    }

    previousImGuiScaleIndex = -1;
    previousImGuiScale = defaultImGuiScale;
    ScaleImGui();
}

typedef enum ExtractSteps {
    ES_PORT_ARCHIVE,
    ES_WINDOWS,
    ES_EXTRACT_ARGS,
    ES_EXTRACT,
    ES_VERIFY,
} ExtractSteps;

typedef enum PromptSteps {
    PS_FILE_CHECK,
    PS_LOCAL,
    PS_FIRST,
    PS_FIRST_WAIT, // waiting for the async file-pick result (resolves immediately on the native path)
    PS_WAIT,
    PS_NONE,
} PromptSteps;

typedef enum WindowsSteps {
    WS_TEMP,
    WS_PERMS,
    WS_ONEDRIVE,
    WS_DONE,
} WindowsSteps;

bool IsSubpath(const std::filesystem::path& path, const std::filesystem::path& base) {
    auto rel = std::filesystem::relative(path, base);
    return !rel.empty() && rel.native()[0] != '.';
}

bool PathTestCleanup() {
    try {
        if (std::filesystem::exists("./text.txt"))
            std::filesystem::remove("./text.txt");
        if (std::filesystem::exists("./test/"))
            std::filesystem::remove("./test/");
    } catch (std::filesystem::filesystem_error const&) { return false; }
    return true;
}

void CheckAndCreateModFolder() {
    try {
        std::string modsPath = Ship::Context::LocateFileAcrossAppDirs("mods", "bk");
        if (!std::filesystem::exists(modsPath)) {
            // Create mods folder relative to app dir
            modsPath = Ship::Context::GetPathRelativeToAppDirectory("mods", "bk");
            std::string filePath = modsPath + "/custom_mod_files_go_here.txt";
            if (std::filesystem::create_directories(modsPath)) {
                std::ofstream(filePath).close();
                std::filesystem::create_directories(modsPath + "/~romhacks"); // BK romhacks go here
                std::filesystem::create_directories(modsPath + "/~lang");     // Language packs go here
                std::filesystem::create_directories(modsPath + "/~shared");   // Mods usable by everything go here
            }
        }
    } catch (std::filesystem::filesystem_error const&) {
        // Couldn't make the folder, continue silently
        return;
    }
}

static const std::vector<std::string> sRomArchives = { "bk.o2r" };

static bool AnyRomArchiveExists() {
    for (const auto& archive : sRomArchives) {
        if (std::filesystem::exists(Ship::Context::LocateFileAcrossAppDirs(archive, "bk"))) {
            return true;
        }
    }
    return false;
}

// Register every resource factory the game's asset types need.
static void RegisterResourceFactories(const std::shared_ptr<Ship::ResourceLoader>& loader) {
    loader->RegisterResourceFactory(std::make_shared<Factories::ResourceFactoryBinarySpriteV0>(),
                                    RESOURCE_FORMAT_BINARY, "Sprite",
                                    static_cast<uint32_t>(Torch::ResourceType::BKSprite), 0);
    loader->RegisterResourceFactory(std::make_shared<Factories::ResourceFactoryBinaryModelV0>(), RESOURCE_FORMAT_BINARY,
                                    "Model", static_cast<uint32_t>(Torch::ResourceType::BKModel), 0);
    loader->RegisterResourceFactory(std::make_shared<Factories::ResourceFactoryBinaryBKAnimationV0>(),
                                    RESOURCE_FORMAT_BINARY, "BKAnimation",
                                    static_cast<uint32_t>(Torch::ResourceType::BKAnimation), 0);
    loader->RegisterResourceFactory(std::make_shared<Factories::ResourceFactoryBinaryBKDialogV0>(),
                                    RESOURCE_FORMAT_BINARY, "BKDialog",
                                    static_cast<uint32_t>(Torch::ResourceType::BKDialog), 0);
    loader->RegisterResourceFactory(std::make_shared<Factories::ResourceFactoryBinaryBKQuizQuestionV0>(),
                                    RESOURCE_FORMAT_BINARY, "BKQuizQuestion",
                                    static_cast<uint32_t>(Torch::ResourceType::BKQuizQuestion), 0);
    loader->RegisterResourceFactory(std::make_shared<Factories::ResourceFactoryBinaryBKGruntyQuestionV0>(),
                                    RESOURCE_FORMAT_BINARY, "BKGruntyQuestion",
                                    static_cast<uint32_t>(Torch::ResourceType::BKGruntyQuestion), 0);
    loader->RegisterResourceFactory(std::make_shared<Factories::ResourceFactoryBinaryBKDemoInputV0>(),
                                    RESOURCE_FORMAT_BINARY, "BKDemoInput",
                                    static_cast<uint32_t>(Torch::ResourceType::BKDemoInput), 0);
    loader->RegisterResourceFactory(std::make_shared<Factories::ResourceFactoryBinaryBKMapV0>(), RESOURCE_FORMAT_BINARY,
                                    "BKMap", static_cast<uint32_t>(Torch::ResourceType::BKMap), 0);
    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryTextureV0>(), RESOURCE_FORMAT_BINARY,
                                    "Texture", static_cast<uint32_t>(Fast::ResourceType::Texture), 0);
    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryTextureV1>(), RESOURCE_FORMAT_BINARY,
                                    "Texture", static_cast<uint32_t>(Fast::ResourceType::Texture), 1);

    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryVertexV0>(), RESOURCE_FORMAT_BINARY,
                                    "Vertex", static_cast<uint32_t>(Fast::ResourceType::Vertex), 0);
    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryXMLVertexV0>(), RESOURCE_FORMAT_XML, "Vertex",
                                    static_cast<uint32_t>(Fast::ResourceType::Vertex), 0);

    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryDisplayListV0>(),
                                    RESOURCE_FORMAT_BINARY, "DisplayList",
                                    static_cast<uint32_t>(Fast::ResourceType::DisplayList), 0);
    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryXMLDisplayListV0>(), RESOURCE_FORMAT_XML,
                                    "DisplayList", static_cast<uint32_t>(Fast::ResourceType::DisplayList), 0);

    loader->RegisterResourceFactory(std::make_shared<Fast::ResourceFactoryBinaryMatrixV0>(), RESOURCE_FORMAT_BINARY,
                                    "Matrix", static_cast<uint32_t>(Fast::ResourceType::Matrix), 0);

    loader->RegisterResourceFactory(std::make_shared<Ship::ResourceFactoryBinaryBlobV0>(), RESOURCE_FORMAT_BINARY,
                                    "Blob", static_cast<uint32_t>(Ship::ResourceType::Blob), 0);
}

// Loose mod directories (development convenience - a folder of unpacked assets
// used as an overlay). Not subject to the enable/disable CVar because they don't
// represent installable packages. Folders owned by the Mod Menu loader are skipped.
static void LoadLooseModDirectories(const std::string& patches_path) {
    if (patches_path.empty() || !std::filesystem::is_directory(patches_path)) {
        return;
    }
    for (const auto& p : std::filesystem::directory_iterator(patches_path)) {
        if (!p.is_directory()) {
            continue;
        }
        const std::string dirName = p.path().filename().generic_string();
        if (dirName == "~romhacks" || dirName == "~shared" || dirName == "~lang" || IsScopedModFolderName(dirName)) {
            continue;
        }
        SPDLOG_INFO("Found mod directory: {}", p.path().generic_string());
        Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->AddArchive(
            p.path().generic_string());
    }
}

// Load every .o2r language pack from mods/~lang into the ArchiveManager.
static void LoadLanguagePacks() {
    const std::string lang_path = Ship::Context::GetPathRelativeToAppDirectory("mods/~lang");
    if (lang_path.empty() || !std::filesystem::is_directory(lang_path)) {
        return;
    }
    for (const auto& p : std::filesystem::directory_iterator(lang_path)) {
        if (p.is_regular_file() && p.path().extension() == ".o2r") {
            SPDLOG_INFO("Loading language pack: {}", p.path().generic_string());
            Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->AddArchive(
                p.path().generic_string());
        }
    }
}

void GameEngine::FinishInit() {
    for (const auto& archive : sRomArchives) {
        std::string romPath = Ship::Context::LocateFileAcrossAppDirs(archive, "bk");
        if (std::filesystem::exists(romPath)) {
            context->GetResourceManager()->GetArchiveManager()->AddArchive(romPath);
        }
    }

    const std::string patches_path = Ship::Context::GetPathRelativeToAppDirectory("mods");
    if (!patches_path.empty() && !std::filesystem::exists(patches_path)) {
        std::filesystem::create_directories(patches_path);
    }

    // Load enabled mod o2rs into the ArchiveManager.
    UpdateModFiles(true);
    LoadLooseModDirectories(patches_path);
    LoadLanguagePacks();

#if (_DEBUG)
    auto defaultLogLevel = spdlog::level::debug;
#else
    auto defaultLogLevel = spdlog::level::info;
#endif
    auto logLevel =
        static_cast<spdlog::level::level_enum>(CVarGetInteger(CVAR_DEVELOPER_TOOLS("LogLevel"), defaultLogLevel));
    context->InitLogging(logLevel, logLevel);
    Ship::Context::GetRawInstance()->GetLogger()->set_pattern("[%H:%M:%S.%e] [%s:%#] [%l] %v");
    SPDLOG_INFO("Starting Lighthouse version {} (Branch: {} | Commit: {})", (char*)gBuildVersion, (char*)gGitBranch,
                (char*)gGitCommitHash);

    context->InitFileDropMgr();
    context->InitCrashHandler();
    context->InitEventSystem();

    this->context->InitAudio({ .SampleRate = 22000, .SampleLength = 736, .DesiredBuffered = 2208 });

    lhFast3dWindow->SetTargetFps(60);
    lhFast3dWindow->SetMaximumFrameLatency(1);
    lhFast3dWindow->SetRendererUCode(ucode_f3d);

    // Opt-in to memoization
    if (auto interpreter = lhFast3dWindow->GetInterpreterWeak().lock()) {
        interpreter->SetResolvedResourceCacheEnabled(true);
    }

#ifdef USE_NETWORKING
    SDLNet_Init();
#endif

    RegisterResourceFactories(context->GetResourceManager()->GetResourceLoader());
    prevAltAssets = CVarGetInteger(CVAR_SETTING("Mods.AlternateAssets"), 1);
    context->GetResourceManager()->SetAltAssetsEnabled(prevAltAssets);

    // Build the dialog-language list from the base region plus any loaded packs.
    Lighthouse::RescanLanguages();

    LighthouseGui::SetupGuiElements();
    // If UpdateModFiles(true) above quarantined conflicting romhack overlays,
    // surface that to the user now that the modal window is alive.
    MaybeShowModConflictPopup();
    // Likewise if it refused romhack overlays due to a non-v1.0 base.
    MaybeShowRomhackBaseMismatchPopup();
    Instance->AudioInit();
    // Instance->LoadDictionary();
    // Instance->LoadPlayerAnims();
#if defined(__SWITCH__) || defined(__WIIU__)
    CVarRegisterInteger(CVAR_IMGUI_CONTROLLER_NAV, 1); // always enable controller nav on switch/wii u
#endif
}

void GameEngine::RunExtract(int argc, char* argv[]) {
    bool extractDone = false;
    ExtractSteps extractStep = ES_PORT_ARCHIVE;
    WindowsSteps windowsStep = WS_TEMP;
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(context->GetWindow());
    auto gui = wnd->GetGui();
    bool menuWasVisible = false;
    if (gui->GetMenu()->IsVisible()) {
        menuWasVisible = true;
        gui->GetMenu()->Hide();
    }

    OTRVersion romArchiveVersion = { INT16_MAX, 0, 0 };
    for (const auto& archive : sRomArchives) {
        OTRVersion ver = DetectOTRVersion(archive);
        if (ver.major != INT16_MAX) {
            romArchiveVersion = ver;
            break;
        }
    }

    bool shouldRegen = !VerifyArchiveVersion(romArchiveVersion) && romArchiveVersion.major != INT16_MAX;

    std::filesystem::path ownPath;
    std::vector<std::string> args;
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            // argv[i], not argv[argc]: indexing by argc reads the NULL terminator and constructing a
            // std::string from it crashes the game on ANY command-line argument.
            if (argv[i] == nullptr) {
                continue;
            }
            // Option flags (--vr / --novr and friends) are not ROM paths; keep them out of the
            // extractor's candidate list.
            if (argv[i][0] == '-') {
                continue;
            }
            args.push_back(argv[i]);
        }
    }
    GameExtractor extract;
    PromptSteps promptStep = PS_FILE_CHECK;
    std::atomic<bool> extracting = false;
    bool extractStarted = false;
    std::atomic<size_t> extractCount{ 0 }, totalExtract{ 0 };

    // Async ROM selection: the result callback fires on this thread during the render step below, so
    // these plain locals are safe to capture by reference.
    bool romLoaded = false;
    bool romResultReady = false;

    std::string installPath = Ship::Context::GetAppBundlePath();
    std::string file;

#if defined(__SWITCH__)
    LighthouseGui::RegisterPopup("Outdated ROM Archives",
                                 "\x1b[2;2HYou've launched the Ship with an old ROM O2R file."
                                 "\x1b[4;2HPlease regenerate a new ROM O2R and relaunch."
                                 "\x1b[6;2HPress the Home button to exit...",
                                 "OK", "", [&]() { exit(1); });
#elif defined(__WIIU__)
    LighthouseGui::RegisterPopup("Outdated ROM Archives",
                                 "You've launched the Ship with an old a ROM O2R file.\n\n"
                                 "Please generate a ROM O2R and relaunch.\n\n"
                                 "Press and hold the Power button to shutdown...",
                                 "OK", "", [&]() { exit(1); });
    OSFatal();
#endif

    if (!std::filesystem::exists(Ship::Context::LocateFileAcrossAppDirs("/assets"))) {
        LighthouseGui::RegisterPopup(
            "Extractor assets not found",
            "No O2R files found. Missing 'assets/' folder needed to generate OTR file.\nPlease "
            "re-extract them from the download or.\n\nExiting...",
            "OK", "", [&]() {
                lhFast3dWindow = nullptr;
                context = nullptr;
                exit(1);
            });
    } else if (shouldRegen) {
        LighthouseGui::RegisterPopup("Outdated ROM Archives",
                                     "Your ROM archives were created with incompatible versions of Lighthouse.\n"
                                     "You will now be redirected to re-extract them.");
        for (const auto& archive : sRomArchives) {
            std::filesystem::remove(archive);
        }
    }

    std::shared_ptr<BS::thread_pool> threadPool = std::make_shared<BS::thread_pool>(1);
    while (!extractDone) {
        if (GameExtractor::sCustomCodePromptRequested.load()) {
            GameExtractor::sCustomCodePromptRequested = false;
            LighthouseGui::RegisterPopup(
                "Custom Code Romhack Detected",
                "This romhack ships custom code.\n"
                "Lighthouse cannot extract this code, so expected\n"
                "behavior will be missing or broken when playing.\n"
                "\n"
                "Continue extraction anyway?",
                "Continue", "Cancel",
                []() {
                    GameExtractor::sCustomCodePromptResult = 1;
                    GameExtractor::sCustomCodePromptActive = false;
                },
                []() {
                    GameExtractor::sCustomCodePromptResult = 0;
                    GameExtractor::sCustomCodePromptActive = false;
                });
        }
        if (LighthouseGui::PopupsQueued() > 0 || extracting || Ship::FileBrowserWindow::IsOpen()) {
            goto render;
        }

        if (extractStep == ES_EXTRACT && promptStep == PS_FIRST && extractStarted && !extracting) {
            extractStep = ES_VERIFY;
            extractStarted = false;
            extractCount = 0;
            totalExtract = 0;
        }
        switch (extractStep) {
            case ES_PORT_ARCHIVE: {
                if (portArchiveVersionMatch) {
#ifdef _WIN32
                    extractStep = ES_WINDOWS;
#elif (defined(__WIIU__) || defined(__SWITCH__))
                    extractStep = ES_VERIFY;
#else
                    extractStep = ES_EXTRACT;
#endif
                } else {
                    std::string msg;

#if defined(__SWITCH__)
                    msg = "\x1b[4;2HPlease re-extract it from the download.\n"
                          "\x1b[6;2HPress the Home button to exit...";
#elif defined(__WIIU__)
                    msg = "Please extract the lighthouse.o2r from the Lighthouse download\nto your "
                          "folder.\n\nPress "
                          "and hold the power\n"
                          "button to shutdown...";
#else
                    msg = "Please extract the lighthouse.o2r from the Lighthouse download to your "
                          "folder.\n\nExiting...";
#endif
                    std::string title =
                        !std::filesystem::exists(assets_path) ? "Missing lighthouse.o2r" : "lighthouse.o2r is outdated";
                    LighthouseGui::RegisterPopup(title, msg, "OK", "", [&]() { exit(1); });
                }
                continue;
            }
            case ES_WINDOWS: {
                switch (windowsStep) {
                    case WS_TEMP: {
#ifdef _WIN32
                        char* tempVar = getenv("TEMP");
                        std::filesystem::path tempPath;
                        try {
                            tempPath = std::filesystem::canonical(tempVar);
                        } catch (std::filesystem::filesystem_error const&) {
                            std::string userPath = getenv("USERPROFILE");
                            userPath.append("\\AppData\\Local\\Temp");
                            tempPath = std::filesystem::canonical(userPath);
                        }
                        wchar_t buffer[MAX_PATH];
                        GetModuleFileName(NULL, buffer, _countof(buffer));
                        ownPath = std::filesystem::canonical(buffer).parent_path();
                        if (IsSubpath(ownPath, tempPath)) {
                            LighthouseGui::RegisterPopup(
                                "Lighthouse Path Error",
                                "Lighthouse is running in a temp folder.\nExtract the .zip and run again.", "OK", "",
                                [&]() {
                                    threadPool = nullptr;
                                    lhFast3dWindow = nullptr;
                                    context = nullptr;
                                    exit(0);
                                });
                        } else {
                            windowsStep = WS_PERMS;
                        }
#endif
                        continue;
                    }
                    case WS_PERMS: {
                        FILE* tfile = fopen("./text.txt", "w");
                        std::filesystem::path tfolder = std::filesystem::path("./test/");
                        bool error = false;
                        try {
                            create_directories(tfolder);
                        } catch (std::filesystem::filesystem_error const&) { error = true; }
                        if (tfile == NULL || error) {
                            LighthouseGui::RegisterPopup(
                                "Lighthouse Permissions Error",
                                "Lighthouse does not have proper file permissions.\nPlease move it to a "
                                "folder that does and run again.",
                                "OK", "", [&]() {
                                    if (tfile != NULL) {
                                        fclose(tfile);
                                    }
                                    PathTestCleanup();
                                    threadPool = nullptr;
                                    lhFast3dWindow = nullptr;
                                    context = nullptr;
                                    exit(0);
                                });
                        } else {
                            fclose(tfile);
                            if (!PathTestCleanup()) {
                                LighthouseGui::RegisterPopup(
                                    "Lighthouse Permissions Error",
                                    "Lighthouse does not have proper file permissions.\nPlease move it to a "
                                    "folder that does and run again.",
                                    "OK", "", [&]() {
                                        threadPool = nullptr;
                                        lhFast3dWindow = nullptr;
                                        context = nullptr;
                                        exit(0);
                                    });
                            }
                            windowsStep = WS_ONEDRIVE;
                        }
                        continue;
                    }
                    case WS_ONEDRIVE: {
                        if (ownPath.string().find("OneDrive") != std::string::npos) {
                            LighthouseGui::RegisterPopup(
                                "Lighthouse Path Error",
                                "Lighthouse appears to be in a OneDrive folder, which will cause issues.\n"
                                "Please move it to a folder outside of OneDrive, like the root of a\n"
                                "drive (e.g. \"C:\\Games\\Lighthouse\").",
                                "OK", "", [&]() {
                                    threadPool = nullptr;
                                    lhFast3dWindow = nullptr;
                                    context = nullptr;
                                    exit(0);
                                });
                        } else {
                            windowsStep = WS_DONE;
                            if (args.size() > 0) {
                                extractStep = ES_EXTRACT_ARGS;
                            } else {
                                extractStep = ES_EXTRACT;
                            }
                        }
                        continue;
                    }
                    default:
                        continue;
                }
                break;
            }
            case ES_EXTRACT_ARGS: {
#if !defined(__SWITCH__) && !defined(__WIIU__)
                if (args.size() == 0) {
                    LighthouseGui::RegisterPopup(
                        "Run Lighthouse", "All files have been processed. Run Lighthouse?", "Yes", "No",
                        [&]() {
                            if (!AnyRomArchiveExists()) {
                                extractStep = ES_EXTRACT;
                                promptStep = PS_FILE_CHECK;
                            } else {
                                extractStep = ES_VERIFY;
                            }
                        },
                        [&]() {
                            threadPool = nullptr;
                            lhFast3dWindow = nullptr;
                            context = nullptr;
                            exit(0);
                        });
                    break;
                }
                file = args.at(0);
                args.erase(args.begin());
                extract = GameExtractor();
                if (extract.RunStandalone(file)) {
                    bool doExtract = true;
                    std::string archive = "bk.o2r";
                    if (std::filesystem::exists(Ship::Context::GetAppDirectoryPath("bk") + "/" + archive)) {
                        std::string msg = "Archive for current ROM, " + archive + ", already exists.\nExtract again?";
                        LighthouseGui::RegisterPopup("Confirm Re-extract", msg.c_str(), "Yes", "No", [&]() {
                            extracting = true;
                            (void)threadPool->submit_task([&]() -> void {
                                extract.GenerateOTR(extractCount, totalExtract, "bk");
                                extracting = false;
                            });
                        });
                    } else {
                        extracting = true;
                        (void)threadPool->submit_task([&]() -> void {
                            extract.GenerateOTR(extractCount, totalExtract, "bk");
                            extracting = false;
                        });
                    }
                } else {
                    bool open = true;
                    std::string msg = "File\n" + std::string(file) + "\nis not a ROM or does not match supported ROMs.";
                    LighthouseGui::RegisterPopup("Lighthouse ROM Error", msg.c_str());
                }
#else
                extractStep = ES_VERIFY;
#endif
                break;
            }
            case ES_EXTRACT: {
                switch (promptStep) {
                    case PS_FILE_CHECK: {
                        const bool romO2RExists = AnyRomArchiveExists();

                        if (!romO2RExists) {
                            LighthouseGui::RegisterPopup(
                                "No O2R Files", "No O2R files found. Generate one now?", "Yes", "No",
                                [&]() { promptStep = PS_LOCAL; },
                                [&]() {
                                    threadPool = nullptr;
                                    lhFast3dWindow = nullptr;
                                    context = nullptr;
                                    exit(0);
                                });
                        } else {
                            extractStep = ES_VERIFY;
                        }
                        continue;
                    }
                    case PS_LOCAL: {
                        extract = GameExtractor();
                        const std::string appDir = Ship::Context::GetAppDirectoryPath("bk");
                        std::error_code sameDirEc;
                        const bool sameDir = std::filesystem::equivalent(installPath, appDir, sameDirEc);
                        extract.SetSearchPath(installPath);
                        extract.GetRoms(args);
                        if (sameDirEc || !sameDir) {
                            extract.SetSearchPath(appDir);
                            extract.GetRoms(args);
                        }
                        std::sort(args.begin(), args.end());
                        args.erase(std::unique(args.begin(), args.end()), args.end());
                        if (!args.empty()) {
                            promptStep = PS_WAIT;
                            LighthouseGui::RegisterPopup(
                                "ROMs found", "ROMs found in application directory. Would you like to process them?",
                                "Yes", "No", [&]() { extractStep = ES_EXTRACT_ARGS; },
                                [&]() {
                                    args.clear();
                                    promptStep = PS_FIRST;
                                });
                        } else {
                            promptStep = PS_FIRST;
                        }
                        continue;
                    }
                    case PS_FIRST: {
                        if (args.empty()) {
                            // Skip the picker entirely if a baserom.us.z64 is sitting in the app
                            // directory: load it and go straight to extraction.
                            std::string baserom = Ship::Context::GetPathRelativeToAppDirectory("baserom.us.z64");
                            if (std::filesystem::exists(baserom) && extract.LoadRomFromPath(baserom)) {
                                extracting = true;
                                extractStarted = true;
                                file = extract.GetRomPath();
                                (void)threadPool->submit_task([&]() -> void {
                                    extract.GenerateOTR(extractCount, totalExtract, "bk");
                                    extracting = false;
                                });
                                continue; // stay in PS_FIRST; the completion check fires when done
                            }
                            // Otherwise open the picker (native dialog on desktop, ImGui browser on
                            // consoles/arm). For the browser, the IsOpen() gate above keeps the loop
                            // rendering until the user picks or cancels; the ROM loads before the callback.
                            romResultReady = false;
                            romLoaded = false;
                            extract.SelectGameFromUI([&](bool ok) {
                                romLoaded = ok;
                                romResultReady = true;
                            });
                            promptStep = PS_FIRST_WAIT;
                            continue;
                        }
                        extracting = true;
                        extractStarted = true;
                        file = extract.GetRomPath();
                        (void)threadPool->submit_task([&]() -> void {
                            extract.GenerateOTR(extractCount, totalExtract, "bk");
                            extracting = false;
                        });
                        continue;
                    }
                    case PS_FIRST_WAIT: {
                        if (!romResultReady) {
                            goto render; // browser still open; keep drawing it
                        }
                        romResultReady = false;
                        if (!romLoaded) {
                            promptStep = PS_FILE_CHECK; // cancelled or failed to load
                            continue;
                        }
                        extracting = true;
                        extractStarted = true;
                        file = extract.GetRomPath();
                        promptStep = PS_FIRST; // so the ES_EXTRACT/PS_FIRST completion check fires
                        (void)threadPool->submit_task([&]() -> void {
                            extract.GenerateOTR(extractCount, totalExtract, "bk");
                            extracting = false;
                        });
                        continue;
                    }
                    default:
                        break;
                }
                break;
            }
            case ES_VERIFY: {
                const bool romO2RExists = AnyRomArchiveExists();

                if (!romO2RExists) {
                    if (LighthouseGui::PopupsQueued() == 0) {
                        std::string errorMsg;
                        if (!GameExtractor::sLastError.empty()) {
                            // Insert line breaks for long error messages
                            std::string wrapped = GameExtractor::sLastError;
                            const size_t wrapCol = 80;
                            size_t pos = 0;
                            while (pos + wrapCol < wrapped.size()) {
                                size_t breakAt = wrapped.rfind(' ', pos + wrapCol);
                                if (breakAt == std::string::npos || breakAt <= pos) {
                                    breakAt = pos + wrapCol;
                                }
                                wrapped.insert(breakAt, "\n");
                                pos = breakAt + 1;
                            }
                            errorMsg = "ROM extraction failed:\n\n" + wrapped +
                                       "\n\nCheck logs/Lighthouse.log for full details.";
                        } else {
                            errorMsg = "No ROM O2R file detected.\nPlease generate a ROM O2R and relaunch.";
                        }
                        LighthouseGui::RegisterPopup("Extraction Error", errorMsg.c_str(), "OK", "", [&]() {
                            threadPool = nullptr;
                            lhFast3dWindow = nullptr;
                            context = nullptr;
                            exit(0);
                        });
                    }
                    // Don't set extractDone - keep the loop alive so the popup renders.
                    continue;
                }
                extractDone = true;
                continue;
            }
            default:
                break;
        }

    render:
        if (!WindowIsRunning()) {
            threadPool = nullptr;
            lhFast3dWindow = nullptr;
            context = nullptr;
            exit(0);
        }
        // Process window events for resize, mouse, keyboard events
        wnd->HandleEvents();
        UIWidgets::Colors themeColor =
            static_cast<UIWidgets::Colors>(CVarGetInteger(CVAR_SETTING("Menu.Theme"), UIWidgets::Colors::LightBlue));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, UIWidgets::ColorValues.at(themeColor));
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, UIWidgets::ColorValues.at(UIWidgets::Colors::DarkGray));

        // Skip dropped frames
        if (!wnd->IsFrameReady()) {
            continue;
        }
        gui->StartDraw();
        lhFast3dWindow->StartFrame();
        lhFast3dWindow->RunGuiOnly();
        const bool showExtractPopup = extracting && !GameExtractor::sCustomCodePromptActive.load();
        if (showExtractPopup && !ImGui::IsPopupOpen("ROM Extraction")) {
            ImGui::OpenPopup("ROM Extraction");
        }
        if (showExtractPopup) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 8.0f));
            auto color = UIWidgets::ColorValues.at(THEME_COLOR);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(color.x, color.y, color.z, 0.6f));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(color.x, color.y, color.z, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
            if (ImGui::BeginPopupModal("ROM Extraction", NULL,
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize |
                                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                           ImGuiWindowFlags_NoSavedSettings)) {
                int phase = GameExtractor::sPhase;
                float progress;
                if (phase == 3) {
                    progress = 100.0f;
                } else {
                    progress = (totalExtract > 0 ? (float)extractCount / (float)totalExtract : 0) * 100.0f;
                    if (progress > 100.0f)
                        progress = 100.0f;
                }

                // Status text
                auto filename = std::filesystem::path(file).filename().string();
                if (phase == 3) {
                    ImGui::Text("Done!");
                } else if (phase >= 1) {
                    ImGui::Text("Processing %s... (Step %d/2)", filename.c_str(), phase);
                    if (Companion::Instance != nullptr && !Companion::Instance->GetCurrentAssetName().empty()) {
                        auto assetName = Companion::Instance->GetCurrentAssetName();
                        float maxWidth = 600.0f - ImGui::GetStyle().WindowPadding.x * 2;
                        ImVec2 textSize = ImGui::CalcTextSize(assetName.c_str());
                        if (textSize.x > maxWidth) {
                            // Truncate with ellipsis
                            std::string ellipsis = "...";
                            float ellipsisWidth = ImGui::CalcTextSize(ellipsis.c_str()).x;
                            while (assetName.size() > 3 &&
                                   ImGui::CalcTextSize(assetName.c_str()).x > maxWidth - ellipsisWidth) {
                                assetName.pop_back();
                            }
                            assetName += ellipsis;
                        }
                        ImGui::Text("%s", assetName.c_str());
                    }
                } else {
                    ImGui::Text("Starting up...");
                }

                // Progress bar
                std::string overlay;
                if (totalExtract > 0 && extractCount > 0) {
                    overlay = fmt::format("{:.0f}%", progress);
                } else if (phase >= 1) {
                    overlay = "Reading ROM, please wait...";
                } else {
                    overlay = "Starting up...";
                }
                ImGui::ProgressBar(progress / 100.0f, ImVec2(600.0f, 50.0f), overlay.c_str());
                ImGui::EndPopup();
            }
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);
        }
        gui->EndDraw();
        lhFast3dWindow->EndFrame();
        ImGui::PopStyleColor(2);
    }
    threadPool = nullptr;

#ifdef __SWITCH__
    Ship::Switch::Init(Ship::PreInitPhase);
#elif defined(__WIIU__)
    Ship::WiiU::Init(appShortName);
#endif

#if not defined(__SWITCH__) && not defined(__WIIU__)
    CheckAndCreateModFolder();
#endif
    if (menuWasVisible) {
        gui->GetMenu()->Show();
    }
}

ImFont* GameEngine::CreateFontWithSize(float size, std::string fontPath) {
    auto mImGuiIo = &ImGui::GetIO();
    ImFont* font;
    if (fontPath == "") {
        ImFontConfig fontCfg = ImFontConfig();
        fontCfg.OversampleH = fontCfg.OversampleV = 1;
        fontCfg.PixelSnapH = true;
        fontCfg.SizePixels = size;
        font = mImGuiIo->Fonts->AddFontDefault(&fontCfg);
    } else {
        auto initData = std::make_shared<Ship::ResourceInitData>();
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;

        initData->Format = RESOURCE_FORMAT_BINARY;
        initData->Type = static_cast<uint32_t>(RESOURCE_TYPE_FONT);
        initData->ResourceVersion = 0;
        initData->Path = fontPath;
        std::shared_ptr<Ship::Font> fontData = std::static_pointer_cast<Ship::Font>(
            Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(fontPath, false, initData));
        font =
            mImGuiIo->Fonts->AddFontFromMemoryTTF(fontData->Data, static_cast<int>(fontData->DataSize), size, &config);
    }
    // FontAwesome fonts need to have their sizes reduced by 2.0f/3.0f in order to align correctly
    float iconFontSize = size * 2.0f / 3.0f;
    static const ImWchar sIconsRanges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
    ImFontConfig iconsConfig;
    iconsConfig.MergeMode = true;
    iconsConfig.PixelSnapH = true;
    iconsConfig.GlyphMinAdvanceX = iconFontSize;
    mImGuiIo->Fonts->AddFontFromMemoryCompressedBase85TTF(fontawesome_compressed_data_base85, iconFontSize,
                                                          &iconsConfig, sIconsRanges);

    return font;
}

void GameEngine::ScaleImGui() {
    int32_t imGuiScaleIndex = CVarGetInteger("gSettings.ImGuiScale", defaultImGuiScale);
    if (imGuiScaleIndex == previousImGuiScaleIndex) {
        return;
    }

    float scale = imguiScaleOptionToValue[imGuiScaleIndex];
    float newScale = scale / previousImGuiScale;
    ImGui::GetStyle().ScaleAllSizes(newScale);
    ImGui::GetIO().FontGlobalScale = scale;
    previousImGuiScale = scale;
    previousImGuiScaleIndex = imGuiScaleIndex;
}

void GameEngine::Create(int argc, char* argv[]) {
    const auto instance = Instance = new GameEngine();
    // instance->AudioInit();
    // DisplayListPatch::Run();
    // BK renders at 292x216, not the standard 320x240.
    GfxSetNativeDimensions(292, 216);
    instance->RunExtract(argc, argv);
    instance->FinishInit();
    PortEnhancements_Init();
    Anchor::Init();
    SaveManager_Init();
    ShipInit::InitAll();
    ShipInit::Init("BOOT");

    // Stop rumble on any exit path (including direct exit() calls)
    atexit([]() {
        if (Instance && Instance->context && Instance->context->GetControlDeck()) {
            for (int i = 0; i < 4; i++) {
                auto controller = Instance->context->GetControlDeck()->GetControllerByPort(i);
                if (controller) {
                    controller->GetRumble()->StopRumble();
                }
            }
        }
    });
}

extern void ResourceHelpers_ClearRefCache();
void ReleaseSoundfonts();

void GameEngine::Destroy() {
    // Stop rumble on all controllers before tearing down
    if (Instance->context && Instance->context->GetControlDeck()) {
        for (int i = 0; i < 4; i++) {
            auto controller = Instance->context->GetControlDeck()->GetControllerByPort(i);
            if (controller) {
                controller->GetRumble()->StopRumble();
            }
        }
    }

    LighthouseGui::Destroy();
    lhFast3dWindow = nullptr;

    // Flush all resource refs so destructors run while spdlog is still active.
    // sResourceRefCache holds shared_ptrs that outlive the LUS cache otherwise.
    ResourceHelpers_ClearRefCache();
    AudioDma_Clear();
    ReleaseSoundfonts();
    if (Instance->context && Instance->context->GetResourceManager()) {
        Instance->context->GetResourceManager()->UnloadResources("*");
    }
    Instance->context = nullptr;
    // PortEnhancements_Exit();
    for (auto ptr : MemoryPool) {
        free(ptr);
    }
    MemoryPool.clear();
#ifdef __SWITCH__
    Ship::Switch::Exit();
#endif
}

void GameEngine::StartFrame() const {
    using Ship::KbScancode;
    const int32_t dwScancode = this->context->GetWindow()->GetLastScancode();
    this->context->GetWindow()->SetLastScancode(-1);

    switch (dwScancode) {
        case KbScancode::LUS_KB_TAB: {
            // Toggle HD Assets
            CVarSetInteger(CVAR_SETTING("Mods.AlternateAssets"),
                           !CVarGetInteger(CVAR_SETTING("Mods.AlternateAssets"), 0));
            break;
        }
        case KbScancode::LUS_KB_F4: {
            // gNextGameState = GSTATE_BOOT;
            break;
        }
        default:
            break;
    }
}

void GameEngine::RenderGuiFrame() const {
    if (lhFast3dWindow == nullptr) {
        return;
    }
    // Pump window events so the modal stays interactive and the window can close.
    lhFast3dWindow->HandleEvents();
    if (!lhFast3dWindow->IsFrameReady()) {
        return;
    }
    auto gui = lhFast3dWindow->GetGui();
    gui->StartDraw();
    lhFast3dWindow->StartFrame();
    lhFast3dWindow->RunGuiOnly();
    gui->EndDraw();
    lhFast3dWindow->EndFrame();
}

bool GameEngine::sRelaunchRequested = false;

void GameEngine::RelaunchIfRequested(int argc, char* argv[]) {
    if (!sRelaunchRequested) {
        return;
    }
    // Called from SDL_main after Destroy()
#ifdef _WIN32
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(exePath, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            SPDLOG_ERROR("Relaunch failed: CreateProcess error {}", GetLastError());
        }
    }
#elif defined(__linux__) || defined(__APPLE__)
    execv(argv[0], argv);
    SPDLOG_ERROR("Relaunch failed: execv error {}", strerror(errno));
#endif
}

#define SAMPLES_PER_FRAME (560 * 2 * 2)

// 2 VIs per game frame (30fps)
#define gVIsPerFrame 2

extern "C" uint32_t GameEngine_GetSamplesPerFrame() {
    return SAMPLES_PER_FRAME;
}

// Attract-demo audio hold
static std::atomic<bool> sHoldAudio{ false };
static constexpr int kDemoAudioHoldFrames = 2; // drawn ticks to stay held after the load
static int sHoldFramesRemaining = 0;           // game thread only

extern "C" void port_beginDemoAudioHold(void) {
    if (kDemoAudioHoldFrames <= 0) {
        return;
    }
    sHoldFramesRemaining = kDemoAudioHoldFrames;
    sHoldAudio.store(true);
}

extern "C" void port_tickDemoAudioHold(void) {
    if (sHoldAudio.load() && --sHoldFramesRemaining <= 0) {
        sHoldAudio.store(false);
    }
}

extern "C" int port_audioHeld(void) {
    return sHoldAudio.load() ? 1 : 0;
}

static std::vector<std::shared_ptr<Ship::IResource>> sSoundfontResources;

void ReleaseSoundfonts() {
    sSoundfontResources.clear();
}

// Load soundfont BLOBs from OTR and set ROM symbol pointers
static void LoadSoundfonts() {
    auto rm = Ship::Context::GetRawInstance()->GetResourceManager();
    sSoundfontResources.clear();

    auto loadBlob = [&rm](const char* path, uint8_t*& start, uint8_t*& end) {
        auto res = rm->LoadResource(path);
        if (res) {
            start = (uint8_t*)res->GetRawPointer();
            end = start + res->GetPointerSize();
            AudioDma_Register(start, res->GetPointerSize());
            sSoundfontResources.push_back(res);
        } else {
            SPDLOG_ERROR("[Audio] Failed to load soundfont '{}'", path);
        }
    };

    loadBlob("soundfont/soundfont1ctl", soundfont1ctl_ROM_START, soundfont1ctl_ROM_END);
    loadBlob("soundfont/soundfont2ctl", soundfont2ctl_ROM_START, soundfont2ctl_ROM_END);

    // tbl assets don't need END - only START is referenced
    auto loadTbl = [&rm](const char* path, uint8_t*& start) {
        auto res = rm->LoadResource(path);
        if (res) {
            start = (uint8_t*)res->GetRawPointer();
            AudioDma_Register(start, res->GetPointerSize());
            sSoundfontResources.push_back(res);
        } else {
            SPDLOG_ERROR("[Audio] Failed to load soundfont '{}'", path);
        }
    };

    loadTbl("soundfont/soundfont1tbl", soundfont1tbl_ROM_START);
    loadTbl("soundfont/soundfont2tbl", soundfont2tbl_ROM_START);
}

void GameEngine::AudioInit() {
    LoadSoundfonts();
}

// Local timer helper for the per-sub-frame cost measurement.
namespace {
using Clock = std::chrono::steady_clock;
inline long long NsSince(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
}

// In-flight async builds of interpolated sub-frame replacement maps
std::vector<std::future<void>> sMapBuildFutures;

// Cost of the most recent sub-frame, and the wall time this pass may spend:
// subframes/paceFps is exactly the game time one task represents.
long long sLastSubFrameNs = 0;
long long sPassBudgetNs = 0;
} // namespace

void GameEngine::RunCommands(Gfx* Commands, const std::vector<std::unordered_map<Mtx*, MtxF>>& mtx_replacements,
                             size_t frameCount) {
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());

    if (wnd == nullptr) {
        return;
    }

    auto interpreter = wnd->GetInterpreterWeak().lock().get();

    // Process window events for resize, mouse, keyboard events
    wnd->HandleEvents();

    interpreter->mInterpolationIndex = 0;

    // Expand DrawAndRunGraphicsCommands so we can read the backbuffer between
    // Run() (frame rendered) and EndFrame() (buffer swap). On N64, CPU/RDP shared
    // physical memory so gFramebuffers always had valid pixel data after rendering.
    auto wndBase = Ship::Context::GetRawInstance()->GetWindow();
#if defined(ENABLE_VR) && defined(_WIN32)
    const bool vrActive = vr_is_requested() && vr_is_active();
    if (vr_is_requested() && !vrActive) {
        // Booting: advance the OpenXR session and close any frame it begins (safe no-op otherwise) so
        // the active loop starts clean next frame; then fall through to the flat render so the desktop
        // shows the game while VR spins up.
        vr_begin_frame();
        vr_submit();
    }
#endif
    const auto passT0 = Clock::now();
    for (size_t frameIdx = 0; frameIdx < frameCount; frameIdx++) {
        if (frameIdx >= 1 && frameIdx - 1 < sMapBuildFutures.size()) {
            sMapBuildFutures[frameIdx - 1].wait();
        }
        // Stop once another sub-frame no longer fits in what the tick's worth
        // of wall time has left.
        if (frameIdx > 0 && sLastSubFrameNs > 0 && (sPassBudgetNs - NsSince(passT0)) < sLastSubFrameNs) {
            break;
        }
        const auto& m = mtx_replacements[frameIdx];
        bool isFinalFrame = (frameIdx == frameCount - 1);
#if defined(ENABLE_VR) && defined(_WIN32)
        if (vrActive) {
            // Sample the full CPU cost of producing this sub-frame (the flat path's accounting, kept so
            // the sub-frame budget above still throttles correctly when stereo doubles the scene cost).
            auto runT0 = Clock::now();
            auto gui = wndBase->GetGui();
            wndBase->GetMouseStateManager()->StartFrame();
            // Keep every ImGui popup in the MAIN viewport: with multi-viewports a tall dropdown spills
            // past the window as its own OS window and never reaches the menu texture - in the headset
            // the menu "opens" invisibly.
            ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
            const bool menuOpen = gui->GetMenuOrMenubarVisible();
            float vrMouseX = 0.0f, vrMouseY = 0.0f;
            bool vrMouseValid = false;
            static bool sVrMenuNavArmed = false;
            // Every sub-frame (menu open or not) so L3 can TOGGLE the menu from the headset; same
            // shortcuts for regular gamepads, edge-driven.
            VrFeedImGuiFromVrControllers(menuOpen);
            VrSdlPadStickClicks(menuOpen);
            if (menuOpen) {
                // POINTER-ONLY interaction, and note what is NOT done here. Clearing
                // ImGuiConfigFlags_NavEnableGamepad is enough to stop ImGui's built-in ANALOG stick
                // navigation (the hypersensitivity, and the nav-active widget that swallowed stick
                // input instead of letting the mouse drag). Putting the SDL backend into Manual
                // gamepad mode ALSO looked like it would stop that - and it did, by breaking
                // everything else: with zero controllers registered the backend clears
                // ImGuiBackendFlags_HasGamepad, and ImGui's UpdateKeyboardInputs then force-zeroes
                // EVERY gamepad key each frame ("clear gamepad data if disabled"). That silently
                // wiped the ImGuiKey_GamepadBack we feed for the L3 toggle and every key we push.
                // So the backend keeps its default AutoAll mode; only nav is switched off.
                ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
                // The OS cursor isn't visible in the headset - draw ImGui's software cursor into the
                // menu texture, and hide the OS arrow so the desktop doesn't show two cursors.
                ImGui::GetIO().MouseDrawCursor = true;
                SDL_ShowCursor(SDL_DISABLE);
                // The port's mouse-camera support puts SDL in relative (captured) mode, which
                // freezes the global cursor position and starves the pointer path - "the game
                // steals the mouse". Force it off every menu frame; the game re-arms on close.
                SDL_SetRelativeMouseMode(SDL_FALSE);
                VrFeedImGuiGamepadNav();
                // Controller pointer first (motion or pad); desktop mouse as the fallback source.
                vrMouseValid = VrControllerImGuiMousePos(&vrMouseX, &vrMouseY);
                if (!vrMouseValid) {
                    vrMouseValid = VrComputeImGuiMousePos(&vrMouseX, &vrMouseY);
                }
                if (vrMouseValid) {
                    // Position AND button go into the queue together, before StartDraw, so NewFrame
                    // resolves a consistent (pos, delta, down) triple for the widgets it builds.
                    ImGui::GetIO().AddMousePosEvent(vrMouseX, vrMouseY);
                    ImGui::GetIO().AddMouseButtonEvent(0, sVrPointerHeld);
                    // ...and claim the OS cursor. While the game window holds focus the SDL backend
                    // queues the REAL mouse position after ours and simply wins, which left the
                    // cursor pinned to the physical mouse and the stick doing nothing. WantSetMousePos
                    // makes the backend warp the OS cursor onto our point before it reads it back,
                    // so its own feed now agrees with the stick instead of fighting it.
                    ImGui::GetIO().MousePos = ImVec2(vrMouseX, vrMouseY);
                    ImGui::GetIO().WantSetMousePos = true;
                }
                if (!sVrMenuNavArmed) {
                    sVrMenuNavArmed = true;
                }
            } else if (sVrMenuNavArmed) {
                ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; // desktop menu keeps nav
                ImGui::GetIO().MouseDrawCursor = false;
                SDL_ShowCursor(SDL_ENABLE);
                ImGui::GetIO().KeyRepeatDelay = 0.275f; // ImGui defaults, for the desktop
                ImGui::GetIO().KeyRepeatRate = 0.05f;
                sVrMenuNavArmed = false;
            }
            gui->StartDraw();
            // Nothing is forced after StartDraw. Gui::StartDraw calls DrawMenu, so every widget
            // samples the mouse INSIDE it - anything written afterwards lands a frame late and
            // poisons the delta ImGui derives next frame. The events queued before StartDraw are
            // the whole story.
            interpreter->StartFrame();
            // One OpenXR frame per sub-frame: each re-locates the head pose (vr_begin_frame ->
            // xrWaitFrame), paced by the headset, so the HMD runs at its native refresh with smooth
            // head tracking while game logic stays at its own rate.
            vr_begin_frame();
            if (VrGame_StereoActive()) {
                const int eyes = vr_eye_count();
                for (int e = 0; e < eyes; e++) {
                    interpreter->RunVrEye(Commands, m, vr_eye_viewproj(e), vr_sky_viewproj(e), vr_hud_viewproj(e),
                                          vr_full2d_viewproj(e), nullptr, vr_eye_width(e), vr_eye_height(e));
                    vr_submit_eye_texture(e, interpreter->GetVrFbTextureId(), vr_eye_width(e), vr_eye_height(e));
                }
            } else {
                // Title, file select, cutscenes and Theater mode: render the flat frame ONCE onto the
                // head-locked panel quad (no stereo substitution) - scripted cameras read as sickness
                // in stereo, and 2D screens have nothing to gain from it.
                vr_set_panel_mode(true);
                interpreter->RunVrPanel(Commands, m, vr_overlay_width(), vr_overlay_height());
                vr_submit_panel_texture(interpreter->GetVrFbTextureId(), vr_overlay_width(), vr_overlay_height());
            }
            if (menuOpen) {
                // Render the menu into its stable offscreen FBO and present THAT on the head-locked
                // panel - the menu floats over the live stereo world, fully drivable from the headset.
                uint32_t winW = 0, winH = 0;
                int32_t winX = 0, winY = 0;
                interpreter->GetDimensions(&winW, &winH, &winX, &winY);
                vr_menu_render_begin((int)winW, (int)winH); // bind + clear the private FBO
                gui->EndDraw();                             // render ImGui INTO that FBO
                vr_menu_apply_opacity();                    // gVRMenuOpacity -> see-through panel
                vr_menu_render_present((int)winW, (int)winH);
                vr_submit();
                vr_menu_mirror_desktop((int)winW, (int)winH); // desktop shows the same menu
            } else {
                vr_submit();
                gui->EndDraw();
                // Mirror the rendered VR frame onto the flat window as the LAST fb0 write before the
                // swap, so the desktop shows the game instead of flickering stale back-buffers.
                uint32_t mW = 0, mH = 0;
                int32_t mX = 0, mY = 0;
                interpreter->GetDimensions(&mW, &mH, &mX, &mY);
                const bool stereo = VrGame_StereoActive();
                const int sw = stereo ? vr_eye_width(0) : vr_overlay_width();
                const int sh = stereo ? vr_eye_height(0) : vr_overlay_height();
                vr_mirror_game_desktop(interpreter->GetVrFbTextureId(), sw, sh, (int)mW, (int)mH);
            }
            sLastSubFrameNs = NsSince(runT0);
            interpreter->EndFrame();
            CALL_EVENT(FrameDrawEnd);
            interpreter->mInterpolationIndex++;
            continue;
        }
#endif
        if (frameCount > 1 || wndBase->IsFrameReady()) {
            // Sample the full CPU cost of producing this sub-frame.
            auto runT0 = Clock::now();
            auto gui = wndBase->GetGui();
            wndBase->GetMouseStateManager()->StartFrame();
            gui->StartDraw();
            interpreter->StartFrame();
#if defined(ENABLE_VR) && defined(_WIN32)
            // Headless per-eye verification (BK_VR_EYEDUMP=<n>): render the SAME display list through
            // RunVrEye with synthetic matrices and dump every 30th eye frame to a BMP next to the exe,
            // up to n dumps. This is how the stereo path gets eyes on it on a machine with no headset:
            // the interpreter routing (projection substitution, HUD plane, sky) is identical to a live
            // session; only the head pose and fov are synthetic.
            {
                static int sDumpBudget = -2; // -2 = env unread, -1 = disabled
                if (sDumpBudget == -2) {
                    const char* e = getenv("BK_VR_EYEDUMP");
                    sDumpBudget = e ? atoi(e) : -1;
                    if (sDumpBudget > 0) {
                        setvbuf(stdout, NULL, _IONBF, 0); // harness runs get killed externally - lose no prints
                    }
                }
                if (sDumpBudget > 0 && VrGame_StereoEligible()) {
                    static int sFrameNo = 0;
                    // Skip the map-entry sequence (jiggy wipe + camera swoop, ~5 s): the dumps must
                    // show settled gameplay or they get compared against the wrong moment.
                    if (sFrameNo++ >= 300 && (sFrameNo % 30) == 0) {
                        float eyeVP[16], skyVP[16], hudVP[16], full2D[16];
                        const int dw = 1024, dh = 1024;
                        char path[64];
                        // Panel render first: identical off-screen machinery with NO stereo
                        // substitution. If this matches the flat window, RenderVrTarget is proven
                        // and any eye-image fault lives in the mVrEyeActive routing branches.
                        interpreter->RunVrPanel(Commands, m, dw, dh);
                        snprintf(path, sizeof(path), "vr_panel_dump_%02d.bmp", sDumpBudget);
                        vr_debug_dump_texture(interpreter->GetVrFbTextureId(), dw, dh, path);
                        // BOTH eyes, with the live separation math mirrored in the synth matrices.
                        // The L/R pair is what proves per-draw stereo ROUTING: the registered sky
                        // pass must land identical between the eyes while the world separates.
                        vr_debug_synth_matrices(0, eyeVP, skyVP, hudVP, full2D);
                        interpreter->RunVrEye(Commands, m, eyeVP, skyVP, hudVP, full2D, nullptr, dw, dh);
                        snprintf(path, sizeof(path), "vr_eye_dump_%02d.bmp", sDumpBudget);
                        vr_debug_dump_texture(interpreter->GetVrFbTextureId(), dw, dh, path);
                        vr_debug_synth_matrices(1, eyeVP, skyVP, hudVP, full2D);
                        interpreter->RunVrEye(Commands, m, eyeVP, skyVP, hudVP, full2D, nullptr, dw, dh);
                        snprintf(path, sizeof(path), "vr_eye_r_dump_%02d.bmp", sDumpBudget);
                        vr_debug_dump_texture(interpreter->GetVrFbTextureId(), dw, dh, path);
                        sDumpBudget--;
                    }
                }
            }
#endif
            interpreter->Run(Commands, m);
            if (OS_ViBlackActive()) {
                interpreter->mGfxFrameBuffer = 0;
                auto rapi = interpreter->GetCurrentRenderingAPI();
                rapi->StartDrawToFramebuffer(0, 1.0f);
                rapi->ClearFramebuffer(true, false);
            }
            gui->EndDraw();
            sLastSubFrameNs = NsSince(runT0);
            interpreter->EndFrame();
            CALL_EVENT(FrameDrawEnd);
        }
        interpreter->mInterpolationIndex++;
    }

    bool curAltAssets = CVarGetInteger(CVAR_SETTING("Mods.AlternateAssets"), 0);
    if (prevAltAssets != curAltAssets) {
        prevAltAssets = curAltAssets;
        Ship::Context::GetRawInstance()->GetResourceManager()->SetAltAssetsEnabled(curAltAssets);
        gfx_texture_cache_clear();
    }
}

bool GameEngine::IsInterpolationEnabled() {
    return (int)GetInterpolationFPS() > 60 / gVIsPerFrame;
}

// How many interpolated sub-frames to render this tick, plus the present-pacing
// fps that keeps wall-clock time aligned with the game's VI cadence. Pure policy
// derived from the interpolation target, adaptive cap, and demo/cutscene state.
namespace {
struct SubframePacing {
    int subframes; // renders to emit this tick (>= 1)
    int fps;       // target present fps for this tick
};

SubframePacing ComputeSubframePacing() {
    int target_fps = (int)GameEngine::Instance->GetInterpolationFPS();

    // Demo/replay modes render at the native rate
    const bool replayMode = func_802E4A08();
    if (!replayMode) {
        // Some music-synced cutscenes cap interpolation at native 30
        int fpsCap = port_getInterpolationFpsCap();
        if (fpsCap > 0 && target_fps > fpsCap) {
            target_fps = fpsCap;
        }
    }

    // Game-logic VI per tick: gVIsPerFrame (=2 -> 30 Hz) normally; demo
    // replay and cutscene stutter raise it for slow N64 frames.
    int viPerTick = port_getDemoViCount();
    if (viPerTick <= 0) {
        viPerTick = gVIsPerFrame + port_getCutsceneExtraVis();
    }
    if (viPerTick < gVIsPerFrame) {
        viPerTick = gVIsPerFrame;
    }

    int effective_logic_fps = 60 / viPerTick;
    if (effective_logic_fps < 1) {
        effective_logic_fps = 1;
    }

    // Subframes per tick: integer count. floor(target_fps / eff), min 1. This
    // guarantees an integer count even when target_fps isn't a multiple of
    // eff (the fractional-ratio jitter at target=30 / VI=3).
    int subframesPerTick = target_fps / effective_logic_fps;
    if (subframesPerTick < 1) {
        subframesPerTick = 1;
    }

    // Replay modes never interpolate: one render per tick, held to viPerTick/60 by the floor.
    if (replayMode) {
        subframesPerTick = 1;
    }

    // paceFps drives DXGI's per-present wait so that subframes * 1/paceFps =
    // viPerTick/60 wall (= game time per tick). When viPerTick == gVIsPerFrame
    // and target_fps is a multiple of eff, paceFps == target_fps and stays
    // constant. Otherwise it varies per tick to keep wall == game.
    int fps = subframesPerTick * effective_logic_fps;

    return { subframesPerTick, fps };
}
} // namespace

void GameEngine::ProcessGfxCommands(Gfx* commands) {
    auto wnd = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());

    if (wnd == nullptr) {
        return;
    }

    // if(gEnableGammaBoost) {
    //     wnd->EnableSRGBMode();
    // }
    wnd->SetRendererUCode(UcodeHandlers::ucode_f3dex);

    // Persistent across frames so each map's bucket array survives.
    // Interpolate clears entries but keeps the buckets, saving thousands
    // of node allocations per tick at high refresh rates.
    static std::vector<std::unordered_map<Mtx*, MtxF>> mtx_replacements;

    const SubframePacing pacing = ComputeSubframePacing();
    const int subframesPerTick = pacing.subframes;
    const int fps = pacing.fps;

    // Emit exactly subframesPerTick sub-frames with t values evenly spaced.
    // No accumulator carry: each tick is independent so VI changes don't
    // misalign leftover state.
    if ((int)mtx_replacements.size() < subframesPerTick) {
        mtx_replacements.resize(subframesPerTick);
    }
    size_t activeFrames = 0;
    sMapBuildFutures.clear();
    for (int i = 1; i <= subframesPerTick; i++) {
        if (i < subframesPerTick) {
            float t = (float)i / (float)subframesPerTick;
            if (i == 1) {
                FrameInterpolation_Interpolate(t, mtx_replacements[activeFrames]);
            } else {
                auto* map = &mtx_replacements[activeFrames];
                sMapBuildFutures.push_back(
                    std::async(std::launch::async, [t, map] { FrameInterpolation_Interpolate(t, *map); }));
            }
        } else {
            mtx_replacements[activeFrames].clear();
        }
        activeFrames++;
    }

    sPassBudgetNs = 1000000000LL * subframesPerTick / fps;

    if (wnd != nullptr) {
        wnd->SetTargetFps(fps);
        // Hardcoded: CVarGetInteger crashes due to heap corruption in debug builds.
        wnd->SetMaximumFrameLatency(2);
    }

    if (GfxDebuggerIsDebugging()) {
        if (mtx_replacements.empty()) {
            mtx_replacements.emplace_back();
        }
        mtx_replacements[0].clear();
        activeFrames = 1;
    }

    RunCommands(commands, mtx_replacements, activeFrames);

    // Drain any builds the render loop didn't consume (debugger path, early
    // return) before the next tick's StartRecord resets the trees they read.
    for (auto& f : sMapBuildFutures) {
        if (f.valid()) {
            f.wait();
        }
    }
    sMapBuildFutures.clear();
}

uint32_t GameEngine::GetInterpolationFPS() {
    if (CVarGetInteger(CVAR_SETTING("MatchRefreshRate"), 1)) { // default ON: pace interpolation at the display's rate
        return Ship::Context::GetRawInstance()->GetWindow()->GetCurrentRefreshRate();

    } else if (CVarGetInteger(CVAR_VSYNC_ENABLED, 1) ||
               !Ship::Context::GetRawInstance()->GetWindow()->CanDisableVerticalSync()) {
        return std::min<uint32_t>(Ship::Context::GetRawInstance()->GetWindow()->GetCurrentRefreshRate(),
                                  CVarGetInteger(CVAR_SETTING("InterpolationFPS"), 60));
    }

    return CVarGetInteger(CVAR_SETTING("InterpolationFPS"), 30);
}

uint32_t GameEngine::GetInterpolationFrameCount() {
    return static_cast<uint32_t>(ceil((float)GetInterpolationFPS() / (60.0f / gVIsPerFrame)));
}

extern "C" uint32_t GameEngine_GetInterpolationFrameCount() {
    return GameEngine::GetInterpolationFrameCount();
}

void GameEngine::ShowMessage(const char* title, const char* message, SDL_MessageBoxFlags type) {
#if defined(__SWITCH__)
    SPDLOG_ERROR(message);
#else
    SDL_ShowSimpleMessageBox(type, title, message, nullptr);
    SPDLOG_ERROR(message);
#endif
}

bool GameEngine::HasVersion(BKVersion ver) {
    auto versions = Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->GetGameVersions();
    return std::find(versions.begin(), versions.end(), ver) != versions.end();
}

extern "C" bool GameEngine_HasVersion(BKVersion ver) {
    return GameEngine::HasVersion(ver);
}

std::vector<BKVersion> GameEngine::GetAvailableVersions() {
    static constexpr BKVersion kKnown[] = { BK_VER_US_10, BK_VER_US_11, BK_VER_PAL, BK_VER_JP };
    auto loaded = Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->GetGameVersions();
    std::vector<BKVersion> present;
    for (BKVersion ver : kKnown) {
        if (std::find(loaded.begin(), loaded.end(), static_cast<uint32_t>(ver)) != loaded.end()) {
            present.push_back(ver);
        }
    }
    return present;
}

extern "C" uint32_t GameEngine_GetSampleRate() {
    auto player = Ship::Context::GetRawInstance()->GetAudio()->GetAudioPlayer();
    if (player == nullptr) {
        return 0;
    }

    if (!player->IsInitialized()) {
        return 0;
    }

    return player->GetSampleRate();
}

// End

Fast::Interpreter* GameEngine_GetInterpreter() {
    return std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow())
        ->GetInterpreterWeak()
        .lock()
        .get();
}

extern "C" float GameEngine_GetAspectRatio() {
    auto interpreter = GameEngine_GetInterpreter();
    return interpreter->mCurDimensions.aspect_ratio;
}

extern "C" uint32_t GameEngine_GetGameVersion() {
    return 0x00000001;
}

static const char* sOtrSignature = "__OTR__";

extern "C" uint8_t GameEngine_OTRSigCheck(const char* data) {
    if (data == nullptr) {
        return 0;
    }
    return strncmp(data, sOtrSignature, strlen(sOtrSignature)) == 0;
}

extern "C" void GameEngine_GetTextureInfo(const char* path, int32_t* width, int32_t* height, float* scale,
                                          bool* custom) {
    if (GameEngine_OTRSigCheck(path) != 1) {
        *custom = false;
        return;
    }
    std::shared_ptr<Fast::Texture> tex = std::static_pointer_cast<Fast::Texture>(
        Ship::Context::GetRawInstance()->GetResourceManager()->LoadResourceProcess(path));
    *width = tex->Width;
    *height = tex->Height;
    *scale = tex->VPixelScale;
    *custom = tex->Flags & (1 << 0);
}

// Gets the width of the main ImGui window
extern "C" uint32_t OTRGetCurrentWidth() {
    return GameEngine::Instance->context->GetWindow()->GetWidth();
}

// Gets the height of the main ImGui window
extern "C" uint32_t OTRGetCurrentHeight() {
    return GameEngine::Instance->context->GetWindow()->GetHeight();
}

extern "C" float OTRGetHUDAspectRatio() {
    if (CVarGetInteger("gHUDAspectRatio.Enabled", 0) == 0 || CVarGetInteger("gHUDAspectRatio.X", 0) == 0 ||
        CVarGetInteger("gHUDAspectRatio.Y", 0) == 0) {
        return GameEngine_GetAspectRatio();
    }
    return ((float)CVarGetInteger("gHUDAspectRatio.X", 1) / (float)CVarGetInteger("gHUDAspectRatio.Y", 1));
}

static float OTRWidescreenHalfHeight() {
    auto interpreter = GameEngine_GetInterpreter();
    return interpreter->mNativeDimensions.width * 3.0f / 4.0f / 2.0f;
}

extern "C" float OTRGetDimensionFromLeftEdge(float v) {
    auto interpreter = GameEngine_GetInterpreter();
    return (interpreter->mNativeDimensions.width / 2 -
            OTRWidescreenHalfHeight() * interpreter->mCurDimensions.aspect_ratio + (v));
}

extern "C" float OTRGetDimensionFromRightEdge(float v) {
    auto interpreter = GameEngine_GetInterpreter();
    return (interpreter->mNativeDimensions.width / 2 +
            OTRWidescreenHalfHeight() * interpreter->mCurDimensions.aspect_ratio -
            (interpreter->mNativeDimensions.width - v));
}

extern "C" float OTRGetDimensionFromLeftEdgeForcedAspect(float v, float aspectRatio) {
    auto interpreter = GameEngine_GetInterpreter();
    return (interpreter->mNativeDimensions.width / 2 -
            OTRWidescreenHalfHeight() * (aspectRatio > 0 ? aspectRatio : interpreter->mCurDimensions.aspect_ratio) +
            (v));
}

extern "C" float OTRGetDimensionFromRightEdgeForcedAspect(float v, float aspectRatio) {
    auto interpreter = GameEngine_GetInterpreter();
    return (interpreter->mNativeDimensions.width / 2 +
            OTRWidescreenHalfHeight() * (aspectRatio > 0 ? aspectRatio : interpreter->mCurDimensions.aspect_ratio) -
            (interpreter->mNativeDimensions.width - v));
}

extern "C" float OTRGetDimensionFromLeftEdgeOverride(float v) {
    return OTRGetDimensionFromLeftEdgeForcedAspect(v, OTRGetHUDAspectRatio());
}

extern "C" float OTRGetDimensionFromRightEdgeOverride(float v) {
    return OTRGetDimensionFromRightEdgeForcedAspect(v, OTRGetHUDAspectRatio());
}

// Gets the width of the current render target area
extern "C" uint32_t OTRGetGameRenderWidth() {
    auto interpreter = GameEngine_GetInterpreter();
    return interpreter->mCurDimensions.width;
}

// Gets the height of the current render target area
extern "C" uint32_t OTRGetGameRenderHeight() {
    auto interpreter = GameEngine_GetInterpreter();
    return interpreter->mCurDimensions.height;
}

extern "C" int16_t OTRGetRectDimensionFromLeftEdge(float v) {
    return ((int)floorf(OTRGetDimensionFromLeftEdge(v)));
}

extern "C" int16_t OTRGetRectDimensionFromRightEdge(float v) {
    return ((int)ceilf(OTRGetDimensionFromRightEdge(v)));
}

extern "C" int16_t OTRGetRectDimensionFromLeftEdgeForcedAspect(float v, float aspectRatio) {
    return ((int)floorf(OTRGetDimensionFromLeftEdgeForcedAspect(v, aspectRatio)));
}

extern "C" int16_t OTRGetRectDimensionFromRightEdgeForcedAspect(float v, float aspectRatio) {
    return ((int)ceilf(OTRGetDimensionFromRightEdgeForcedAspect(v, aspectRatio)));
}

extern "C" int16_t OTRGetRectDimensionFromLeftEdgeOverride(float v) {
    return OTRGetRectDimensionFromLeftEdgeForcedAspect(v, OTRGetHUDAspectRatio());
}

extern "C" int16_t OTRGetRectDimensionFromRightEdgeOverride(float v) {
    return OTRGetRectDimensionFromRightEdgeForcedAspect(v, OTRGetHUDAspectRatio());
}

extern "C" int32_t OTRConvertHUDXToScreenX(int32_t v) {
    auto interpreter = GameEngine_GetInterpreter();
    float gameAspectRatio = interpreter->mCurDimensions.aspect_ratio;
    int32_t gameHeight = interpreter->mCurDimensions.height;
    int32_t gameWidth = interpreter->mCurDimensions.width;
    float hudAspectRatio = (float)SCREEN_WIDTH / (float)SCREEN_HEIGHT;
    int32_t hudHeight = gameHeight;
    int32_t hudWidth = static_cast<int32_t>(hudHeight * hudAspectRatio);
    float hudScreenRatio = (hudWidth / (float)SCREEN_WIDTH);
    float hudCoord = v * hudScreenRatio;
    float gameOffset = static_cast<float>((gameWidth - hudWidth) / 2);
    float gameCoord = hudCoord + gameOffset;
    float gameScreenRatio = ((float)SCREEN_WIDTH / gameWidth);
    float screenScaledCoord = gameCoord * gameScreenRatio;
    int32_t screenScaledCoordInt = static_cast<int32_t>(screenScaledCoord);
    return screenScaledCoordInt;
}

extern "C" void* GameEngine_Malloc(size_t size) {
    MemoryPool.push_back((uint8_t*)malloc(size));
    return MemoryPool.back();
}

extern "C" void GameEngine_Free(void* ptr) {
    for (auto it = MemoryPool.begin(); it != MemoryPool.end(); ++it) {
        if (*it == ptr) {
            free(ptr);
            MemoryPool.erase(it);
            break;
        }
    }
}
