#include <algorithm>
#include <cstring>
#include <vector>

#include <libultraship/bridge.h>
#include "port/UI/cvar_prefixes.h"
#include "port/Engine.h"
#include "port/Enhancements/Events/Hooks/Events.h"
#include "port/ShipInit.hpp"
#include "port/ShipUtils.h"
#include "port/Patches/GeoCull.h"
#include "port/vr/vr.h" // vr_is_active: the widescreen HUD anchor must stand down in stereo

#define CVAR_DRAW_DISTANCE CVAR_ENHANCEMENT("Graphics.DrawDistance")
#define CVAR_DISABLE_LOD CVAR_ENHANCEMENT("Graphics.DisableLOD")
#define CVAR_FP_LOBBY_DOOR_TILE CVAR_ENHANCEMENT("Fixes.FPLobbyDoorTile")

static const int kMaxDrawDistanceMul = 6;
static int sDrawDistanceCubeWidth(int mul) {
    return 4 * mul;
}

static int sDrawDistanceLevel = 1;
static int sDisableLOD = 0;

extern "C" {
#include <ultra64.h>
#include "enums.h"
#include "functions.h"
#include "libultraship/libultra/gbi.h"
#include "port/Romhack/RomhackConfig.h"

extern s32 gFramebufferWidth;
extern s32 gFramebufferHeight;

// Romhack model display lists can leave a palette (TLUT) mode enabled and leak it
// into the next model's draw.
void port_modelRenderResetTLUT(Gfx** gfx) {
    if (!port_isRomhack()) {
        return;
    }
    gDPSetTextureLUT((*gfx)++, G_TT_NONE);
}

// Widescreen HUD edge anchoring (centered-ortho HUD geometry).

static const float kHudCenterBand = 32.0f;

float port_hudOrthoShift(float refX) {
    // IN VR THERE IS NOTHING TO ANCHOR AGAINST, AND ASKING WAS THE FLICKER.
    //
    // This shift only ever existed to be cancelled again: it pushes a widget out toward a
    // widescreen edge, and the renderer's 4:3 squeeze (AdjXForAspectRatio) pulls it back to where
    // the artist put it. In stereo that squeeze does not run at all - it returns X untouched,
    // because the per-eye projection already carries the right aspect and squeezing again would
    // stretch the world. So in VR the push has nothing cancelling it and is simply wrong.
    //
    // Worse than wrong: UNSTABLE. The number it asked for is the renderer's CURRENT aspect ratio,
    // and the VR pass swaps that field to the eye buffer's aspect for the duration of each eye and
    // puts it back afterwards. The display list is built on the game thread while the render thread
    // is doing that swapping, so the answer depended on which of the two happened to be in the
    // field at the instant the widget was built. The two answers are 46 pixels apart out of a
    // 146 pixel half-width, and the widgets alternated between them at frame rate. That is the
    // life bar, the air meter and the jiggy and token counters flickering, and it is why they and
    // nothing else flicker: these are the only things in the game that call this function.
    //
    // For the record, the two fixes shipped before this one could never have worked. Both were
    // aimed at depth ordering, and these widgets clear G_ZBUFFER and draw with RM_XLU_SURF, which
    // carries neither Z_CMP nor Z_UPD - they do not depth-test at all. The comment in vr.cpp that
    // blames z-fighting for this flicker is a misattribution and has been corrected.
    if (vr_is_active()) {
        return 0.0f;
    }
    float halfW = (f32)gFramebufferWidth * 0.5f;
    float extraHalf = (f32)gFramebufferHeight * 0.5f * GameEngine_GetAspectRatio() - halfW;
    if (extraHalf < 0.0f) {
        extraHalf = 0.0f; // narrower than 4:3 (e.g. pillarboxed): never pull inward
    }
    if (refX < halfW - kHudCenterBand) {
        return -extraHalf; // left-anchored
    }
    if (refX > halfW + kHudCenterBand) {
        return extraHalf; // right-anchored
    }
    return 0.0f; // centered
}

int port_getDrawDistanceSetting(void) {
    return sDrawDistanceLevel;
}

int port_getDrawDistanceLevel(void) {
    int mul = port_getDrawDistanceSetting();
    if (IsDemoMode() && getGameMode() != GAME_MODE_4_PAUSED) {
        mul = 1;
    }
    return mul;
}

float port_drawDistanceMul(void) {
    int level = port_getDrawDistanceLevel();
    if (level <= 1) {
        return 1.0f;
    }
    return (float)level + 0.1f; // Nudge
}

void port_applyModelDrawDistanceCull(int* fadeFlag, float* cullMult, float* cullDist) {
    float mul = port_drawDistanceMul();
    *cullMult *= mul;
    *cullDist *= mul;
}

int port_spriteSizeCulled(float depth, float size, float baseThreshold, int disableFlag) {
    if (disableFlag) {
        return 0;
    }
    float scale = port_drawDistanceMul();
    return (3000.0f * scale < depth) && (((size / depth) * scale) < baseThreshold);
}

int port_shouldDisableLOD(void) {
    // Field-report A/B seam (BK_DIAG_SETLOD=<0|1>): force the answer for one run, bypassing the
    // boot-time CVar cache, so a far-LOD hypothesis can be tested without touching the config.
    static int sDiagForce = -2;
    if (sDiagForce == -2) {
        const char* e = getenv("BK_DIAG_SETLOD");
        sDiagForce = (e != NULL) ? atoi(e) : -1;
    }
    if (sDiagForce >= 0) {
        return sDiagForce;
    }
    return sDisableLOD;
}
}

// ============================================================================
// LEVEL OCCLUSION — extend draw distance to camera-area portal geometry
// ============================================================================
//
// Some distant level geometry is hidden by BK's camera-area portal culling rather than the
// distance/LOD culls the prop draw-distance enhancement covers. Disabling that culling
// wholesale floods the render buffer, so at the maxed draw-distance level we force just the
// specific chunks known to suffer from it. Currently the only one is the Mumbo's Mountain
// stonehenge: a single "outside areas {1,2}" CAMERA command in the opaque map model.

static void OnGeoCull_LevelOcclusion(IEvent* event) {
    auto* ev = reinterpret_cast<OnGeoCull*>(event);
    if (ev->type != OCCLUSION_CMD_CAMERA) {
        return;
    }
    if (gsworld_getMap() == MAP_2_MM_MUMBOS_MOUNTAIN && ev->offset == 0x2CD0 &&
        ev->modelBin == (const void*)mapModel_getModelBin(0)) {
        *ev->forceDraw = true;
    }
}

void RegisterLevelOcclusion_Init() {
    bool maxed = CVarGetInteger(CVAR_DRAW_DISTANCE, 1) >= kMaxDrawDistanceMul;
    GeoCull_SetConsumer(GEOCULL_CONSUMER_ENHANCEMENT, maxed);
    COND_HOOK(OnGeoCull, EVENT_PRIORITY_NORMAL, maxed, OnGeoCull_LevelOcclusion);
}

static RegisterShipInitFunc sInitLevelOcclusion(RegisterLevelOcclusion_Init, { CVAR_DRAW_DISTANCE });

static void RegisterDrawDistanceGraphics_Init() {
    COND_HOOK(DrawDistanceCubeWidth, EVENT_PRIORITY_NORMAL, CVarGetInteger(CVAR_DRAW_DISTANCE, 1) > 1,
              [](IEvent* event) {
                  int mul = port_getDrawDistanceLevel();
                  if (mul <= 1) {
                      return;
                  }
                  auto* ev = (DrawDistanceCubeWidth*)event;
                  int width = sDrawDistanceCubeWidth(mul);
                  if (width > ev->mapWidth) {
                      width = ev->mapWidth;
                  }
                  *ev->width = width;
              });
}

static RegisterShipInitFunc drawDistanceGraphicsInit(RegisterDrawDistanceGraphics_Init, { CVAR_DRAW_DISTANCE });

static void RefreshDrawDistanceCVars() {
    int mul = CVarGetInteger(CVAR_DRAW_DISTANCE, 1);
    if (mul < 1) {
        mul = 1;
    }
    if (mul > kMaxDrawDistanceMul) {
        mul = kMaxDrawDistanceMul;
    }
    sDrawDistanceLevel = mul;
    sDisableLOD = CVarGetInteger(CVAR_DISABLE_LOD, 0);
}

static RegisterShipInitFunc drawDistanceCVarCache(RefreshDrawDistanceCVars, { CVAR_DRAW_DISTANCE, CVAR_DISABLE_LOD });

// ============================================================================
// STALE TILE DESCRIPTOR — Freezeezy Peak lobby door trim
// ============================================================================

// F3DEX opcodes as used by BK64.
static const uint8_t kOpVtx = 0x04;
static const uint8_t kOpTri2 = 0xB1;
static const uint8_t kOpTri1 = 0xBF;
static const uint8_t kOpSetTileSize = 0xF2;
static const uint8_t kOpSetTile = 0xF5;
static const uint8_t kOpSetTImg = 0xFD;

static const uint32_t kTxClampBit = 0x2; // G_TX_CLAMP
static const uint8_t kRenderTile = 0;    // G_TX_RENDERTILE

// Torch tags a G_SETTIMG that references an extracted texture by putting 0xFF in
// the top byte of w1 and the texture index in the low 24 bits.
static const uint32_t kTexMarker = 0xFF;

static const char kFPLobbySuffix[] = "GL_FP_LOBBY_OPA";

static bool sPathEndsWith(const char* path, const char* suffix) {
    if (path == NULL) {
        return false;
    }
    size_t plen = strlen(path);
    size_t slen = strlen(suffix);
    return plen >= slen && strcmp(path + plen - slen, suffix) == 0;
}

static void OnModelDisplayListLoad_StaleTile(IEvent* event) {
    auto* ev = reinterpret_cast<OnModelDisplayListLoad*>(event);
    if (ev->dlWords == NULL || ev->texSizes == NULL || !sPathEndsWith(ev->path, kFPLobbySuffix)) {
        return;
    }

    // Walk the list tracking live render-tile state, flagging any G_SETTILE whose
    // clamp causes a larger texture to be drawn through a smaller tile.
    std::vector<uint32_t> toPatch;
    int curTex = -1;
    uint32_t liveTile = UINT32_MAX;
    uint32_t tileW = 0, tileH = 0;

    for (uint32_t i = 0; i + 1 < ev->dlWordCount; i += 2) {
        uint32_t w0 = ev->dlWords[i];
        uint32_t w1 = ev->dlWords[i + 1];
        uint8_t op = (uint8_t)(w0 >> 24);

        if (op == kOpSetTImg) {
            if (((w1 >> 24) & 0xFF) == kTexMarker) {
                curTex = (int)(w1 & 0x00FFFFFF);
            }
        } else if (op == kOpSetTile) {
            if (((w1 >> 24) & 0x7) == kRenderTile) {
                liveTile = i;
            }
        } else if (op == kOpSetTileSize) {
            if (((w1 >> 24) & 0x7) == kRenderTile) {
                uint32_t uls = (w0 >> 12) & 0xFFF;
                uint32_t ult = w0 & 0xFFF;
                uint32_t lrs = (w1 >> 12) & 0xFFF;
                uint32_t lrt = w1 & 0xFFF;
                tileW = ((lrs - uls) / 4) + 1;
                tileH = ((lrt - ult) / 4) + 1;
            }
        } else if (op == kOpVtx || op == kOpTri1 || op == kOpTri2) {
            if (curTex < 0 || liveTile == UINT32_MAX || tileW == 0 || tileH == 0) {
                continue;
            }
            if ((uint32_t)curTex >= ev->texCount) {
                continue;
            }
            uint32_t declW = ev->texSizes[curTex].width;
            uint32_t declH = ev->texSizes[curTex].height;
            if (declW <= tileW && declH <= tileH) {
                continue;
            }
            uint32_t tileWord = ev->dlWords[liveTile + 1];
            uint32_t cms = (tileWord >> 8) & 0x3;
            uint32_t cmt = (tileWord >> 18) & 0x3;
            if (((cms & kTxClampBit) == 0) && ((cmt & kTxClampBit) == 0)) {
                continue;
            }
            // Only safe to swap clamp for wrap when the mask makes the wrap period
            // exactly the texture size.
            uint32_t masks = (tileWord >> 4) & 0xF;
            uint32_t maskt = (tileWord >> 14) & 0xF;
            if (masks == 0 || maskt == 0 || (1u << masks) != declW || (1u << maskt) != declH) {
                continue;
            }
            if (std::find(toPatch.begin(), toPatch.end(), liveTile) == toPatch.end()) {
                toPatch.push_back(liveTile);
            }
        }
    }

    for (size_t n = 0; n < toPatch.size(); n++) {
        ev->dlWords[toPatch[n] + 1] &= ~((kTxClampBit << 18) | (kTxClampBit << 8));
    }
}

static void RegisterStaleTileFix_Init() {
    COND_HOOK(OnModelDisplayListLoad, EVENT_PRIORITY_NORMAL, CVarGetInteger(CVAR_FP_LOBBY_DOOR_TILE, 0),
              OnModelDisplayListLoad_StaleTile);
}

static RegisterShipInitFunc staleTileFixInit(RegisterStaleTileFix_Init, { CVAR_FP_LOBBY_DOOR_TILE });
