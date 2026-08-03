// Control Schemes
//
// Provides three selectable gamepad presets and the per-frame input shaping two
// of them depend on:
//
//   Retro  - Stock N64 layout
//   Modern - Xbox Live Arcade layout
//   Pocket - Super Pocket layout

#include <memory>
#include <SDL2/SDL.h>

#include <ship/Context.h>
#include <ship/controller/controldeck/ControlDeck.h>
#include <ship/controller/controldevice/controller/Controller.h>
#include <ship/controller/controldevice/controller/mapping/ControllerAxisDirectionMapping.h>
#include <ship/controller/controldevice/controller/mapping/sdl/SDLButtonToButtonMapping.h>
#include <ship/controller/controldevice/controller/mapping/sdl/SDLAxisDirectionToButtonMapping.h>
#include <ship/controller/controldevice/controller/mapping/sdl/SDLButtonToAxisDirectionMapping.h>
#include <ship/controller/physicaldevice/ConnectedPhysicalDeviceManager.h>
#include <ship/controller/physicaldevice/PhysicalDeviceType.h>
#include <libultraship/libultra/controller.h>
#include <libultraship/bridge.h>

// VR motion-controller merge (stubbed out in a flat build - see src/port/vr/VrGame.cpp).
extern "C" void VrGame_MergePad(OSContPad* pad);
extern "C" void VrGame_PollVrShortcuts(void);
extern "C" void VrGame_HeadMoveShape(OSContPad* pad); // FP head-directed movement (after all merges)
extern "C" void VrGame_SyncFrame(void);               // per-tick VR<->game state sync (menu plane etc.)

#include "port/UI/cvar_prefixes.h"
#include "port/Enhancements/Camera/FreeLookCamera.h"
#include "port/ShipUtils.h"
#include "ControlSchemes.h"

extern "C" {
#include "enums.h"
int bs_getState(void);
int bsbtrot_inSet(int state); // nonzero if `state` is one of the Talon Trot states
s32 getGameMode(void);
}

using namespace Ship;

namespace {

std::shared_ptr<Controller> GetPort0Controller() {
    auto ctx = Ship::Context::GetRawInstance();
    if (ctx == nullptr) {
        return nullptr;
    }
    auto controlDeck = ctx->GetControlDeck();
    if (controlDeck == nullptr) {
        return nullptr;
    }
    return controlDeck->GetControllerByPort(0);
}

// Replace a single N64 button's SDL mappings (keyboard mappings are untouched).
void ClearButtonSDL(const std::shared_ptr<Controller>& controller, CONTROLLERBUTTONS_T bitmask) {
    auto button = controller->GetButton(bitmask);
    if (button != nullptr) {
        button->ClearAllButtonMappingsForDeviceType(Ship::SDLGamepad);
    }
}

void BindButton(const std::shared_ptr<Controller>& controller, CONTROLLERBUTTONS_T bitmask, int32_t sdlButton) {
    auto button = controller->GetButton(bitmask);
    if (button == nullptr) {
        return;
    }
    auto mapping = std::make_shared<SDLButtonToButtonMapping>(0, bitmask, sdlButton);
    button->AddButtonMapping(mapping);
    mapping->SaveToConfig();
    button->SaveButtonMappingIdsToConfig();
}

void BindAxisToButton(const std::shared_ptr<Controller>& controller, CONTROLLERBUTTONS_T bitmask, int32_t sdlAxis,
                      int32_t axisDirection) {
    auto button = controller->GetButton(bitmask);
    if (button == nullptr) {
        return;
    }
    auto mapping = std::make_shared<SDLAxisDirectionToButtonMapping>(0, bitmask, sdlAxis, axisDirection);
    button->AddButtonMapping(mapping);
    mapping->SaveToConfig();
    button->SaveButtonMappingIdsToConfig();
}

void BindButtonToLeftStick(const std::shared_ptr<Controller>& controller, Direction direction, int32_t sdlButton) {
    auto stick = controller->GetLeftStick();
    if (stick == nullptr) {
        return;
    }
    auto mapping = std::make_shared<SDLButtonToAxisDirectionMapping>(0, LEFT_STICK, direction, sdlButton);
    stick->AddAxisDirectionMapping(direction, mapping);
    mapping->SaveToConfig();
    stick->SaveAxisDirectionMappingIdsToConfig();
}

// True if the given C-button is currently driven by a stick axis (the right
// stick), as opposed to a face button. Drives which directions the shaping pass
// regenerates from the raw right stick.
bool CButtonIsAxis(const std::shared_ptr<Controller>& controller, CONTROLLERBUTTONS_T bitmask) {
    auto button = controller->GetButton(bitmask);
    if (button == nullptr) {
        return false;
    }
    for (auto& [id, mapping] : button->GetAllButtonMappings()) {
        if (dynamic_cast<SDLAxisDirectionToButtonMapping*>(mapping.get()) != nullptr) {
            return true;
        }
    }
    return false;
}

} // namespace

void ControlSchemes_Apply(int scheme) {
    auto controller = GetPort0Controller();
    if (controller == nullptr) {
        return;
    }

    // Every scheme starts from the stock defaults (the Retro layout), then
    // repurposes only the buttons it changes. This keeps the left-stick and
    // start/D-pad mappings correct without rebuilding them by hand.
    controller->ClearAllMappingsForDeviceType(Ship::SDLGamepad);
    controller->AddDefaultMappings(Ship::SDLGamepad);

    switch (scheme) {
        case CONTROL_SCHEME_MODERN: {
            // Yaw-only free look + Wonderwing when crouched
            ClearButtonSDL(controller, BTN_CLEFT);
            ClearButtonSDL(controller, BTN_CRIGHT);
            ClearButtonSDL(controller, BTN_CDOWN);
            ClearButtonSDL(controller, BTN_CUP);
            BindButton(controller, BTN_CUP, SDL_CONTROLLER_BUTTON_Y);
            BindButton(controller, BTN_CDOWN, SDL_CONTROLLER_BUTTON_B);
            BindAxisToButton(controller, BTN_CLEFT, SDL_CONTROLLER_AXIS_RIGHTX, -1);
            BindAxisToButton(controller, BTN_CRIGHT, SDL_CONTROLLER_AXIS_RIGHTX, 1);
            // The D-pad IS the C-pad: Banjo never uses the N64 D-pad, and real button C presses
            // are the only reliable route to Z + C-Left (Talon Trot) with the right stick owning
            // the camera. These pass the shaping pass untouched - only AXIS-sourced C bits are
            // stripped there.
            BindButton(controller, BTN_CUP, SDL_CONTROLLER_BUTTON_DPAD_UP);
            BindButton(controller, BTN_CDOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            BindButton(controller, BTN_CLEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
            BindButton(controller, BTN_CRIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

            // Recenter camera
            ClearButtonSDL(controller, BTN_R);
            BindButton(controller, BTN_R, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);

            // Crouch / Talon Trot
            ClearButtonSDL(controller, BTN_Z);
            BindAxisToButton(controller, BTN_Z, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1);
            BindAxisToButton(controller, BTN_Z, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1);

            // Attacks
            ClearButtonSDL(controller, BTN_B);
            BindButton(controller, BTN_B, SDL_CONTROLLER_BUTTON_X);

            // L remains L to preserve skip dialog combo
            ClearButtonSDL(controller, BTN_L);
            BindButton(controller, BTN_L, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
            break;
        }
        case CONTROL_SCHEME_POCKET: {
            // Movement: D-pad
            ClearButtonSDL(controller, BTN_DUP);
            ClearButtonSDL(controller, BTN_DDOWN);
            ClearButtonSDL(controller, BTN_DLEFT);
            ClearButtonSDL(controller, BTN_DRIGHT);
            controller->GetLeftStick()->ClearAllMappingsForDeviceType(Ship::SDLGamepad);
            BindButtonToLeftStick(controller, UP, SDL_CONTROLLER_BUTTON_DPAD_UP);
            BindButtonToLeftStick(controller, DOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            BindButtonToLeftStick(controller, LEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
            BindButtonToLeftStick(controller, RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

            // First person
            ClearButtonSDL(controller, BTN_CUP);
            BindButton(controller, BTN_CUP, SDL_CONTROLLER_BUTTON_Y);

            // Camera left/right
            ClearButtonSDL(controller, BTN_CLEFT);
            BindButton(controller, BTN_CLEFT, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
            ClearButtonSDL(controller, BTN_CRIGHT);
            BindButton(controller, BTN_CRIGHT, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);

            // Zoom out / poop egg
            // Context-aware, imgui menu takes up BACK/SEL
            // so we can't be faithful to Pocket here
            ClearButtonSDL(controller, BTN_CDOWN);
            BindButton(controller, BTN_CDOWN, SDL_CONTROLLER_BUTTON_B);

            // Recenter camera
            ClearButtonSDL(controller, BTN_R);

            // Crouch / Talon Trot
            ClearButtonSDL(controller, BTN_Z);
            BindAxisToButton(controller, BTN_Z, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1);

            // Attacks
            ClearButtonSDL(controller, BTN_B);
            BindButton(controller, BTN_B, SDL_CONTROLLER_BUTTON_X);
            break;
        }
        case CONTROL_SCHEME_RETRO:
        default:
            break;
    }

    auto ctx = Ship::Context::GetRawInstance();
    if (ctx != nullptr && ctx->GetConsoleVariables() != nullptr) {
        ctx->GetConsoleVariables()->Save();
    }
}

extern "C" void port_shapeControllerInput(void* contPad) {
    auto* pad = static_cast<OSContPad*>(contPad);
    if (pad == nullptr) {
        return;
    }
    // VR motion controllers merge into pad 0 HERE - ahead of the port's edge detection in
    // pfsManager_update, so menus see fresh-press edges - and before the demo-mode bail, so the
    // controllers can still drive the attract-mode screens. The merge is additive per axis (stronger
    // source wins), so a gamepad or keyboard keeps working alongside.
    VrGame_MergePad(pad);
    VrGame_PollVrShortcuts();
    VrGame_SyncFrame();
    // Demo modes feed their own recorded pad; don't reshape live input over it.
    if (IsDemoMode()) {
        return;
    }
    auto controller = GetPort0Controller();
    if (controller == nullptr) {
        return;
    }

    const bool modern = CVarGetInteger(CVAR_SETTING("Controls.Scheme"), CONTROL_SCHEME_MODERN) == CONTROL_SCHEME_MODERN;
    const bool crouched = (bs_getState() == BS_7_CROUCH);
    const bool eggPooping = (bs_getState() == BS_A_EGG_ASS);

    int32_t rx = pad->right_stick_x;
    int32_t ry = pad->right_stick_y;
    int32_t arx = (rx < 0) ? -rx : rx;
    int32_t ary = (ry < 0) ? -ry : ry;

    if (modern) {
        if (!crouched && !eggPooping) {
            // Face-B doubles as C-Down for the crouched egg poop; standing, that face press must
            // not zoom the camera. But a C-Down from any OTHER source (the D-pad default, a custom
            // bind) is a deliberate C press and passes - source-checked against the raw pad so a
            // remap can never be silently eaten. (The wild report "can't map C-buttons" was these
            // strips running source-blind.)
            bool faceBHeld = false;
            bool dpadDownHeld = false;
            auto ctxB = Ship::Context::GetRawInstance();
            auto deckB = ctxB != nullptr ? ctxB->GetControlDeck() : nullptr;
            auto devMgrB = deckB != nullptr ? deckB->GetConnectedPhysicalDeviceManager() : nullptr;
            if (devMgrB != nullptr) {
                for (auto& [instanceId, gamepad] : devMgrB->GetConnectedSDLGamepadsForPort(0)) {
                    if (gamepad == nullptr) {
                        continue;
                    }
                    if (SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_B)) {
                        faceBHeld = true;
                    }
                    if (SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) {
                        dpadDownHeld = true;
                    }
                }
            }
            if (faceBHeld && !dpadDownHeld) {
                pad->button &= ~BTN_CDOWN;
            }
        }

        // Bindings applied under Retro can leave the right stick mapped to C-buttons as AXES; under
        // Modern those must never reach the game as C presses (a stick-up C-Up plays the camera
        // sound, yanks Free Look back to the auto camera, and triggers the game's own look mode).
        // The wonderwing gesture below still reads the pre-strip stick direction.
        const uint16_t stickCPre = pad->button & (BTN_CLEFT | BTN_CRIGHT);
        {
            uint16_t axisMaskM = 0;
            if (CButtonIsAxis(controller, BTN_CRIGHT)) axisMaskM |= BTN_CRIGHT;
            if (CButtonIsAxis(controller, BTN_CLEFT))  axisMaskM |= BTN_CLEFT;
            if (CButtonIsAxis(controller, BTN_CDOWN))  axisMaskM |= BTN_CDOWN;
            if (CButtonIsAxis(controller, BTN_CUP))    axisMaskM |= BTN_CUP;
            // NO MAGNITUDE TEST. There used to be one here (`> 24*24`), and it was a second
            // threshold competing with the one that SETS the bit - which is the whole bug.
            // libultraship asserts an axis-sourced C press at 25% of raw stick travel, but the pad
            // fields read above have been through a 20% deadzone and a rescale since, so 24 on the
            // pad needs about 43% of raw travel. Everything between 25% and 43% therefore set
            // C-Up and did not get stripped: a leak band a fifth of the stick wide, sitting exactly
            // where a gentle VR look-up lives.
            //
            // Two field reports came out of that one band, and they are the same bug seen from both
            // sides. Push the stick up hard and you clear 43%, the strip fires, and C-Up never
            // reaches the game - so the egg does not come out. Let the stick RELAX back through the
            // band and C-Up re-asserts against a previous tick of zero, which is a fresh press edge,
            // and the game drops into its own first person and locks Banjo until you press A or B.
            // A thumb returns to centre far more slowly than it leaves it, which is why the trap
            // fires "sometimes" rather than always.
            //
            // The mask above already answers the only question worth asking - is this C bit bound to
            // a stick axis - by reading the bindings themselves. Re-deriving that from the merged pad
            // was never necessary and could only ever disagree.
            pad->button &= ~axisMaskM;
        }

        const s32 mode = getGameMode();
        const bool picturePuzzle = (mode == GAME_MODE_8_BOTTLES_BONUS || mode == GAME_MODE_A_SNS_PICTURE);
        if (!picturePuzzle) {
            const uint16_t stickC = stickCPre;
            uint16_t allow = 0;

            // One action per push, so holding the stick cannot repeat Wonderwing.
            static bool sStickArmed = true;
            if (stickC == 0) {
                sStickArmed = true;
            } else if (crouched && (stickC & BTN_CRIGHT) && arx >= ary && sStickArmed) {
                allow = BTN_CRIGHT;
                sStickArmed = false;
            }
            // Strip only the AXIS-sourced C-left/right (the free-look stick posing as C presses).
            // Button-sourced presses - the D-pad row, custom binds - are deliberate and pass, which
            // is what makes Z + C-Left (Talon Trot) reachable at all under Modern.
            uint16_t axisCLR = 0;
            if (CButtonIsAxis(controller, BTN_CLEFT)) {
                axisCLR |= BTN_CLEFT;
            }
            if (CButtonIsAxis(controller, BTN_CRIGHT)) {
                axisCLR |= BTN_CRIGHT;
            }
            pad->button = (pad->button & ~axisCLR) | allow;
        }
    } else {
        uint16_t axisMask = 0;
        if (CButtonIsAxis(controller, BTN_CRIGHT)) {
            axisMask |= BTN_CRIGHT;
        }
        if (CButtonIsAxis(controller, BTN_CLEFT)) {
            axisMask |= BTN_CLEFT;
        }
        if (CButtonIsAxis(controller, BTN_CDOWN)) {
            axisMask |= BTN_CDOWN;
        }
        if (CButtonIsAxis(controller, BTN_CUP)) {
            axisMask |= BTN_CUP;
        }

        static uint16_t sLatchDir = 0;
        static int32_t sLatchTTL = 0;

        if (port_freeLook_isEnabled()) {
            uint16_t stickBits = 0;
            if (arx * arx + ary * ary > 24 * 24) {
                if (arx >= ary) {
                    stickBits = (rx > 0) ? BTN_CRIGHT : BTN_CLEFT;
                } else {
                    stickBits = (ry > 0) ? BTN_CUP : BTN_CDOWN;
                }
            }
            pad->button &= ~(stickBits & axisMask);
            sLatchDir = 0;
            sLatchTTL = 0;
        } else if (axisMask != 0) {
            pad->button &= ~axisMask;

            if (arx * arx + ary * ary > 24 * 24) {
                uint16_t dir;
                if (arx >= ary) {
                    dir = (rx > 0) ? BTN_CRIGHT : BTN_CLEFT;
                } else {
                    dir = (ry > 0) ? BTN_CUP : BTN_CDOWN;
                }

                if (dir & axisMask) {
                    if (dir != sLatchDir) {
                        if (sLatchTTL <= 0) {
                            sLatchDir = dir;
                            sLatchTTL = 6;
                        }
                    } else {
                        sLatchTTL = 6;
                    }
                    if (sLatchDir & axisMask) {
                        pad->button |= sLatchDir;
                    }
                } else {
                    if (sLatchTTL > 0) {
                        sLatchTTL--;
                    } else {
                        sLatchDir = 0;
                    }
                }
            } else {
                if (sLatchTTL > 0) {
                    sLatchTTL--;
                } else {
                    sLatchDir = 0;
                }
            }
        }
    }

    // Modern Talon Trot
    static bool sPrevLeftTrigger = false;
    static bool sPrevRightTrigger = false;

    if (modern) {
        const Sint16 kTriggerThreshold = 8000;
        bool leftTrigger = false;
        bool rightTrigger = false;

        auto ctx = Ship::Context::GetRawInstance();
        auto controlDeck = ctx != nullptr ? ctx->GetControlDeck() : nullptr;
        auto deviceManager = controlDeck != nullptr ? controlDeck->GetConnectedPhysicalDeviceManager() : nullptr;
        if (deviceManager != nullptr) {
            for (auto& [instanceId, gamepad] : deviceManager->GetConnectedSDLGamepadsForPort(0)) {
                if (gamepad == nullptr) {
                    continue;
                }
                if (SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kTriggerThreshold) {
                    leftTrigger = true;
                }
                if (SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kTriggerThreshold) {
                    rightTrigger = true;
                }
            }
        }

        if (crouched) {
            bool leftEdge = leftTrigger && !sPrevLeftTrigger;
            bool rightEdge = rightTrigger && !sPrevRightTrigger;
            if ((rightEdge && sPrevLeftTrigger) || (leftEdge && sPrevRightTrigger)) {
                pad->button |= BTN_CLEFT;
            }
        }

        sPrevLeftTrigger = leftTrigger;
        sPrevRightTrigger = rightTrigger;
    } else {
        sPrevLeftTrigger = false;
        sPrevRightTrigger = false;
    }

    // Toggle Trot
    static bool sTrotLatched = false;
    static bool sTrotToggledOff = false;
    static bool sPrevZ = false;
    if (CVarGetInteger(CVAR_SETTING("Controls.TalonTrotToggle"), 0) != 0 && bsbtrot_inSet(bs_getState())) {
        bool zNow = (pad->button & BTN_Z) != 0;
        if (!sTrotToggledOff) {
            if (!sTrotLatched) {
                if (!zNow) {
                    sTrotLatched = true; // crouch released after entering trot -> latch it on
                }
            } else if (zNow && !sPrevZ) {
                sTrotLatched = false; // fresh Z tap while latched -> toggle off
                sTrotToggledOff = true;
            }
            if (sTrotLatched) {
                pad->button |= BTN_Z; // keep Z held so the trot doesn't exit
            }
        }
        sPrevZ = zNow;
    } else {
        // Not trotting (or feature off): reset for the next trot.
        sTrotLatched = false;
        sTrotToggledOff = false;
        sPrevZ = (pad->button & BTN_Z) != 0;
    }

    // Pocket: Context-aware B
    // While crouched, poop an egg
    // While standing, quick tap cycles zooms and hold recenters the camera
    static int32_t sBHeldFrames = 0;
    static bool sBHoldFired = false;
    static bool sBWasHeld = false;
    const int32_t kBHoldFrames = 12; // ~0.2s before a standing press counts as a hold
    bool bHeld = (pad->button & BTN_CDOWN) != 0;
    if (CVarGetInteger(CVAR_SETTING("Controls.Scheme"), CONTROL_SCHEME_MODERN) == CONTROL_SCHEME_POCKET && !crouched &&
        !eggPooping) {
        pad->button &= ~BTN_CDOWN;
        if (bHeld) {
            if (sBHeldFrames < kBHoldFrames) {
                sBHeldFrames++;
            }
            if (sBHeldFrames >= kBHoldFrames) {
                pad->button |= BTN_R;
                sBHoldFired = true;
            }
        } else {
            if (sBWasHeld && !sBHoldFired && sBHeldFrames > 0) {
                pad->button |= BTN_CDOWN;
            }
            sBHeldFrames = 0;
            sBHoldFired = false;
        }
    } else {
        sBHeldFrames = 0;
        sBHoldFired = false;
    }
    sBWasHeld = bHeld;

    // Pocket: Talon Trot (LT + RT) and tip-toe (RT)
    //  LT crouches. While crouched, an RT press injects C-Left to start the Talon Trot.
    //  RT on its own (no crouch) lightly touches the analog stick for a tip-toe.
    static bool sTipToe = false;
    static bool sPrevR2 = false;
    if (CVarGetInteger(CVAR_SETTING("Controls.Scheme"), CONTROL_SCHEME_MODERN) == CONTROL_SCHEME_POCKET) {
        bool r2 = false;
        bool l2 = false;
        auto ctx = Ship::Context::GetRawInstance();
        auto controlDeck = ctx != nullptr ? ctx->GetControlDeck() : nullptr;
        auto deviceManager = controlDeck != nullptr ? controlDeck->GetConnectedPhysicalDeviceManager() : nullptr;
        if (deviceManager != nullptr) {
            for (auto& [instanceId, gamepad] : deviceManager->GetConnectedSDLGamepadsForPort(0)) {
                if (gamepad == nullptr) {
                    continue;
                }
                if (SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000) {
                    r2 = true;
                }
                if (SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000) {
                    l2 = true;
                }
            }
        }

        if (l2) {
            // Crouching: RT is the Talon Trot trigger, not tip-toe.
            if (crouched && r2 && !sPrevR2) {
                pad->button |= BTN_CLEFT; // C-Left while crouched starts the Talon Trot
            }
            sTipToe = false;
        } else if (CVarGetInteger(CVAR_SETTING("Controls.PocketTipToeHold"), 0) != 0) {
            sTipToe = r2; // hold mode
        } else if (r2 && !sPrevR2) {
            sTipToe = !sTipToe; // tap toggles
        }
        sPrevR2 = r2;

        if (sTipToe) {
            const float kTipToeScale = 0.25f;
            pad->stick_x = static_cast<int8_t>(pad->stick_x * kTipToeScale);
            pad->stick_y = static_cast<int8_t>(pad->stick_y * kTipToeScale);
        }
    } else {
        sTipToe = false;
        sPrevR2 = false;
    }

    // [port] VR First Person head-directed movement rotates the FINAL stick (all sources merged).
    VrGame_HeadMoveShape(pad);

}
