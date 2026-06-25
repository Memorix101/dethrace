// Dreamcast platform driver.
//
// Video goes straight to the PowerVR hardware: the game's 8 bit paletted
// framebuffer is expanded to an RGB565 texture and drawn as a full screen quad.
// SDL2 is used only for input (Dreamcast keyboard and controller), never for
// video, because the KOS SDL2 renderer path is not a good fit here.
//
// This mirrors the proven approach from the original Dreamcast port, adapted to
// the current harness platform API.

#include "harness.h"
#include "harness/config.h"
#include "harness/hooks.h"
#include "harness/trace.h"
#include "sdl2_scancode_map.h"

#include <SDL.h>
#include <kos.h>
#include <dc/vmu_fb.h>

#include <stdio.h>
#include <string.h>

// PowerVR textures must be powers of two. The framebuffer (up to 640x480) fits
// inside 1024x512.
#define TEX_WIDTH 1024
#define TEX_HEIGHT 512

#define DC_AXIS_DEADZONE 16000

extern void QuitGame(void);
extern br_pixelmap* gBack_screen;
// The 3D render viewport. It is a sub-rectangle of gBack_screen (the game frames
// it with the HUD), so its base_x/base_y give where the 3D belongs and we offset
// the hardware 3D and sky into that rectangle.
extern br_pixelmap* gRender_screen;
// Palette index of the solid sky colour, chosen by the game each frame (see
// ConditionallyFillWithSky). Painted behind the hardware 3D.
extern int gDC_sky_index;

static pvr_ptr_t pvram;
static uint16_t converted_palette[256];
// ARGB1555 version of the palette for the 2D/HUD overlay. Index 0 (the cleared /
// sky colour) is transparent, every other index opaque, so the overlay can be
// composited in front of the hardware 3D with the 3D showing through index 0.
static uint16_t converted_palette_argb1555[256];
static br_pixelmap* last_screen_src;
static int render_width, render_height;

static void (*gKeyHandler_func)(void);
// 32 bytes, 1 bit per key, matching the DOS executable behaviour.
static br_uint_32 key_state[8];

static Uint32 last_frame_time;

// FPS counter shown on the VMU LCD.
static maple_device_t* gVmu;
static vmufb_t gVmu_fb;
static int gFrame_count;
static Uint32 gFps_last_time;

// Hardware 3D path. BRender hands us already-transformed, screen-space triangles
// (see TriangleRender_PVR in the pentprim driver). We buffer them here and draw
// them on the PowerVR each frame instead of letting the CPU software-rasterize
// them, which is the whole point of the speed-up.
typedef struct {
    float x, y, z;
    float u, v;
    uint32_t argb;
} tDC3D_vertex;

#define DC3D_MAX_TRIS 24000
static tDC3D_vertex g3d_verts[DC3D_MAX_TRIS * 3];
static int16_t g3d_tritex[DC3D_MAX_TRIS]; // texture cache index per triangle, -1 = untextured
static uint8_t g3d_tricat[DC3D_MAX_TRIS];  // 0 = opaque (OP), 1 = punch-through (PT), 2 = blended (TR)
static int g3d_order[DC3D_MAX_TRIS];       // triangle indices sorted by (category, texture)
static int g3d_tri_count;
static int g3d_last_submitted; // diagnostic: triangles drawn last frame
static float g3d_dbg[6];       // diagnostic: first triangle's raw x,y,z per vertex

// Texture cache: BRender's 8bpp paletted textures are converted once to RGB565
// PowerVR textures, keyed by their source pixel pointer. Colours come from the
// game's current palette.
typedef struct {
    void* key;     // source 8bpp pixel pointer
    pvr_ptr_t tex; // PVR texture memory
    int pw, ph;    // power-of-two PVR dimensions
    float us, vs;  // UV scale = real_size / pow2_size
} tDC3D_texture;

#define DC3D_MAX_TEX 192
static tDC3D_texture g3d_tex[DC3D_MAX_TEX];
static int g3d_tex_count;

static int dc_pow2_ceil(int v) {
    int p = 8;
    while (p < v && p < 1024) {
        p <<= 1;
    }
    return p;
}

// Register (or look up) an 8bpp texture and return its cache index, or -1.
int DCPVR3D_RegisterTexture(void* pixels, int w, int h, int stride) {
    if (pixels == NULL || w <= 0 || h <= 0) {
        return -1;
    }
    for (int i = 0; i < g3d_tex_count; i++) {
        if (g3d_tex[i].key == pixels) {
            return i;
        }
    }
    if (g3d_tex_count >= DC3D_MAX_TEX) {
        return -1;
    }
    int pw = dc_pow2_ceil(w);
    int ph = dc_pow2_ceil(h);
    pvr_ptr_t tex = pvr_mem_malloc(pw * ph * 2);
    if (tex == NULL) {
        return -1;
    }
    // ARGB1555 so colour index 0 is transparent (alpha 0). Opaque-map surfaces
    // are drawn in the opaque list where alpha is ignored (index 0 reads as
    // black); punch-through surfaces are drawn in the PT list where alpha 0 is
    // keyed out, giving the transparent parts of fences, signs, etc.
    uint16_t* dst = (uint16_t*)tex;
    const uint8_t* src = (const uint8_t*)pixels;
    for (int y = 0; y < ph; y++) {
        const uint8_t* srow = src + (y < h ? y : h - 1) * stride;
        uint16_t* drow = dst + y * pw;
        for (int x = 0; x < pw; x++) {
            drow[x] = converted_palette_argb1555[srow[x < w ? x : w - 1]];
        }
    }
    int idx = g3d_tex_count++;
    g3d_tex[idx].key = pixels;
    g3d_tex[idx].tex = tex;
    g3d_tex[idx].pw = pw;
    g3d_tex[idx].ph = ph;
    g3d_tex[idx].us = (float)w / pw;
    g3d_tex[idx].vs = (float)h / ph;
    return idx;
}

void DCPVR3D_AddTriTex(
    float x0, float y0, float z0, float u0, float v0, uint32_t c0,
    float x1, float y1, float z1, float u1, float v1, uint32_t c1,
    float x2, float y2, float z2, float u2, float v2, uint32_t c2,
    int texid, int category) {
    if (g3d_tri_count >= DC3D_MAX_TRIS) {
        return;
    }
    if (g3d_tri_count == 0) {
        g3d_dbg[0] = x0; g3d_dbg[1] = y0; g3d_dbg[2] = z0;
        g3d_dbg[3] = x1; g3d_dbg[4] = y1; g3d_dbg[5] = z1;
    }
    tDC3D_vertex* v = &g3d_verts[g3d_tri_count * 3];
    v[0].x = x0; v[0].y = y0; v[0].z = z0; v[0].u = u0; v[0].v = v0; v[0].argb = c0;
    v[1].x = x1; v[1].y = y1; v[1].z = z1; v[1].u = u1; v[1].v = v1; v[1].argb = c1;
    v[2].x = x2; v[2].y = y2; v[2].z = z2; v[2].u = u2; v[2].v = v2; v[2].argb = c2;
    g3d_tritex[g3d_tri_count] = (int16_t)texid;
    g3d_tricat[g3d_tri_count] = (uint8_t)category;
    g3d_tri_count++;
}

void DCPVR3D_AddTri(
    float x0, float y0, float z0, uint32_t c0,
    float x1, float y1, float z1, uint32_t c1,
    float x2, float y2, float z2, uint32_t c2) {
    DCPVR3D_AddTriTex(x0, y0, z0, 0, 0, c0, x1, y1, z1, 0, 0, c1, x2, y2, z2, 0, 0, c2, -1, 0);
}

// Expand a game palette index to an opaque ARGB8888 colour (from the current
// RGB565 palette). Used for untextured flat/gouraud-shaded geometry, where the
// softrend vertex intensity is already the final shade-ramp palette index, not a
// 0..1 brightness - so the real surface colour comes straight from the palette.
uint32_t DCPVR3D_PaletteColor(int idx) {
    if (idx < 0) {
        idx = 0;
    } else if (idx > 255) {
        idx = 255;
    }
    uint16_t c = converted_palette[idx];
    uint32_t r = (c >> 11) & 0x1F;
    uint32_t g = (c >> 5) & 0x3F;
    uint32_t b = c & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// Output mapping from gBack_screen space to the 640x480 PowerVR output. The 3D
// screen coordinates are local to the render viewport, so we shift by the
// viewport offset (g3d_ox/g3d_oy) and then scale to the output (g3d_sx/g3d_sy).
static float g3d_sx, g3d_sy, g3d_ox, g3d_oy;

// Per-draw-order depth bias (polygon offset). The PowerVR resolves visibility
// per pixel by 1/w; coplanar surfaces (a shadow/skidmark/marking on the road)
// share the same 1/w, so during motion their order flips and they flicker. We
// nudge each triangle's depth nearer in proportion to its submission order, so a
// decal drawn after the road it sits on stays consistently in front. The factor
// is tiny, far below real depth differences, so genuine occlusion is unaffected.
#define DC3D_ZBIAS_PER_TRI 1.0e-6f

static void dc3d_emit_vertex(const tDC3D_vertex* t, float us, float vs, float zmul, int eol) {
    pvr_vertex_t v;
    v.flags = eol ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    v.x = (t->x + g3d_ox) * g3d_sx;
    v.y = (t->y + g3d_oy) * g3d_sy;
    v.z = t->z * zmul;
    v.u = t->u * us;
    v.v = t->v * vs;
    v.argb = t->argb;
    v.oargb = 0;
    pvr_prim(&v, sizeof(v));
}

// Sort the triangle indices by (category, texture) into g3d_order with a stable
// counting sort. This lets each list pass walk the triangles once, emitting a
// new polygon header only when the texture changes, instead of the old
// O(textures x triangles) scan (with ~166 textures that scan dominated dense
// scenes). The sort is stable, so within a texture the original submission order
// (and thus the draw-order depth bias) is preserved.
#define DC3D_SORT_KEYS (3 * (DC3D_MAX_TEX + 1))
static void dc3d_sort_order(void) {
    static int count[DC3D_SORT_KEYS + 1];
    memset(count, 0, sizeof(count));
    for (int i = 0; i < g3d_tri_count; i++) {
        int key = g3d_tricat[i] * (DC3D_MAX_TEX + 1) + (g3d_tritex[i] + 1);
        count[key + 1]++;
    }
    for (int k = 0; k < DC3D_SORT_KEYS; k++) {
        count[k + 1] += count[k];
    }
    for (int i = 0; i < g3d_tri_count; i++) {
        int key = g3d_tricat[i] * (DC3D_MAX_TEX + 1) + (g3d_tritex[i] + 1);
        g3d_order[count[key]++] = i;
    }
}

// Emit the buffered triangles of one category into the currently-open PVR list.
// `want_cat`: 0 opaque (OP list), 1 punch-through (PT list, colour index 0 keyed
// transparent), 2 blended (TR list, translucent over the road instead of
// z-fighting it). Textures are ARGB1555: the OP list ignores the alpha bit, the
// PT list keys out alpha 0. Walks the pre-sorted order once (O(triangles)).
static void dc3d_submit_list(int list, int want_cat) {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    int cur_tex = -2; // != any real texid (-1..) so the first triangle emits a header

    for (int oi = 0; oi < g3d_tri_count; oi++) {
        int i = g3d_order[oi];
        if (g3d_tricat[i] != want_cat) {
            continue;
        }
        int tex = g3d_tritex[i];
        if (tex != cur_tex) {
            cur_tex = tex;
            if (tex < 0) {
                pvr_poly_cxt_col(&cxt, list);
            } else {
                pvr_poly_cxt_txr(&cxt, list,
                    PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                    g3d_tex[tex].pw, g3d_tex[tex].ph, g3d_tex[tex].tex, PVR_FILTER_BILINEAR);
            }
            cxt.gen.culling = PVR_CULLING_NONE;
            pvr_poly_compile(&hdr, &cxt);
            pvr_prim(&hdr, sizeof(hdr));
        }
        float us = (tex < 0) ? 1.f : g3d_tex[tex].us;
        float vs = (tex < 0) ? 1.f : g3d_tex[tex].vs;
        float zmul = 1.0f + (float)i * DC3D_ZBIAS_PER_TRI;
        const tDC3D_vertex* t = &g3d_verts[i * 3];
        dc3d_emit_vertex(&t[0], us, vs, zmul, 0);
        dc3d_emit_vertex(&t[1], us, vs, zmul, 0);
        dc3d_emit_vertex(&t[2], us, vs, zmul, 1);
    }
}

// Opaque geometry into the open OP list. Sorts the frame's triangles first (the
// PT and TR passes reuse the same order).
static void dc3d_submit_opaque(void) {
    g3d_last_submitted = g3d_tri_count;
    if (g3d_tri_count == 0) {
        return;
    }
    dc3d_sort_order();
    dc3d_submit_list(PVR_LIST_OP_POLY, 0);
}

// Punch-through (index-0 transparent) geometry into the open PT list.
static void dc3d_submit_punchthrough(void) {
    if (g3d_tri_count == 0) {
        return;
    }
    dc3d_submit_list(PVR_LIST_PT_POLY, 1);
}

// Blended decals into the open TR list (call after the OP/PT geometry is done).
static void dc3d_submit_blended(void) {
    if (g3d_tri_count == 0) {
        return;
    }
    dc3d_submit_list(PVR_LIST_TR_POLY, 2);
}

static void set_key_from_scancode(SDL_Scancode scancode, int down) {
    int dethrace_scancode = sdl_scancode_map[scancode];
    if (dethrace_scancode == 0) {
        return;
    }
    if (down) {
        key_state[dethrace_scancode >> 5] |= (1 << (dethrace_scancode & 0x1F));
    } else {
        key_state[dethrace_scancode >> 5] &= ~(1 << (dethrace_scancode & 0x1F));
    }
    if (gKeyHandler_func != NULL) {
        gKeyHandler_func();
    }
}

// Carmageddon's default controls (DKEYMAP0.TXT) drive with the numeric keypad:
// accelerate = KP_8, brake/reverse = KP_2, steer = KP_4 / KP_6, handbrake =
// space. The menus, on the other hand, use the arrow keys and Return/Escape.
// So the d-pad drives the menus and the analog stick plus triggers drive the
// car.
static SDL_Scancode controller_button_to_scancode(Uint8 button) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A: return SDL_SCANCODE_RETURN;     // menu select
    case SDL_CONTROLLER_BUTTON_B: return SDL_SCANCODE_INSERT;     // reset car
    case SDL_CONTROLLER_BUTTON_X: return SDL_SCANCODE_SPACE;      // handbrake
    case SDL_CONTROLLER_BUTTON_Y: return SDL_SCANCODE_BACKSPACE;  // repair
    case SDL_CONTROLLER_BUTTON_START: return SDL_SCANCODE_ESCAPE; // pause / back
    case SDL_CONTROLLER_BUTTON_BACK: return SDL_SCANCODE_ESCAPE;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return SDL_SCANCODE_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return SDL_SCANCODE_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return SDL_SCANCODE_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return SDL_SCANCODE_RIGHT;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

// Map a controller axis to held keys with a deadzone, tracking the previous
// pressed state so we only emit transitions. The left stick steers (KP_4/KP_6)
// and also accelerates/brakes (KP_8/KP_2); the triggers accelerate and brake.
static void handle_controller_axis(Uint8 axis, int value) {
    static int neg_down[SDL_CONTROLLER_AXIS_MAX];
    static int pos_down[SDL_CONTROLLER_AXIS_MAX];
    SDL_Scancode neg = SDL_SCANCODE_UNKNOWN;
    SDL_Scancode pos = SDL_SCANCODE_UNKNOWN;

    if (axis >= SDL_CONTROLLER_AXIS_MAX) {
        return;
    }
    switch (axis) {
    case SDL_CONTROLLER_AXIS_LEFTX: neg = SDL_SCANCODE_KP_4; pos = SDL_SCANCODE_KP_6; break; // steer
    case SDL_CONTROLLER_AXIS_LEFTY: neg = SDL_SCANCODE_KP_8; pos = SDL_SCANCODE_KP_2; break; // accel / brake
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT: pos = SDL_SCANCODE_KP_2; break;  // brake / reverse
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: pos = SDL_SCANCODE_KP_8; break; // accelerate
    default: return;
    }
    int want_neg = (neg != SDL_SCANCODE_UNKNOWN) && (value < -DC_AXIS_DEADZONE);
    int want_pos = (pos != SDL_SCANCODE_UNKNOWN) && (value > DC_AXIS_DEADZONE);
    if (neg != SDL_SCANCODE_UNKNOWN && want_neg != neg_down[axis]) {
        set_key_from_scancode(neg, want_neg);
        neg_down[axis] = want_neg;
    }
    if (pos != SDL_SCANCODE_UNKNOWN && want_pos != pos_down[axis]) {
        set_key_from_scancode(pos, want_pos);
        pos_down[axis] = want_pos;
    }
}

static void DCPVR_ProcessWindowMessages(void) {
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            set_key_from_scancode(event.key.keysym.scancode, event.type == SDL_KEYDOWN);
            break;

        case SDL_CONTROLLERDEVICEADDED:
            SDL_GameControllerOpen(event.cdevice.which);
            break;

        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            SDL_Scancode sc = controller_button_to_scancode(event.cbutton.button);
            if (sc != SDL_SCANCODE_UNKNOWN) {
                set_key_from_scancode(sc, event.type == SDL_CONTROLLERBUTTONDOWN);
            }
            break;
        }

        case SDL_CONTROLLERAXISMOTION:
            handle_controller_axis(event.caxis.axis, event.caxis.value);
            break;

        case SDL_QUIT:
            QuitGame();
            break;
        }
    }
}

static void DCPVR_SetKeyHandler(void (*handler_func)(void)) {
    gKeyHandler_func = handler_func;
}

static void DCPVR_GetKeyboardState(br_uint_32* buffer) {
    memcpy(buffer, key_state, sizeof(key_state));
}

static int DCPVR_GetMouseButtons(int* pButton1, int* pButton2) {
    *pButton1 = 0;
    *pButton2 = 0;
    return 0;
}

static int DCPVR_GetMousePosition(int* pX, int* pY) {
    *pX = 0;
    *pY = 0;
    return 0;
}

static void DCPVR_CreateWindow(const char* title, int width, int height, tHarness_window_type window_type) {
    (void)title;
    (void)window_type;

    render_width = width;
    render_height = height;

    // Custom PVR init rather than pvr_init_defaults: we use three lists (opaque,
    // punch-through for index-0-transparent textures, translucent for blended
    // decals + the HUD overlay), and the urban scenes push ~1200 triangles. The
    // opb_overflow_count preallocates extra tile bins so dense tiles do not drop
    // geometry - that overflow is exactly the "pieces flicker in and out along
    // tile boundaries" artifact seen while moving. Bigger vertex buffer to match.
    pvr_init_params_t params = {
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16 },
        1024 * 1024, // vertex buffer
        0,           // dma disabled
        0,           // fsaa disabled
        0,           // translucent autosort enabled
        8,           // opb_overflow_count - extra OPBs to stop tile-overflow flicker
        0            // vertex buffer double-buffering enabled
    };
    pvr_init(&params);
    pvram = pvr_mem_malloc(TEX_WIDTH * TEX_HEIGHT * 2);

    // SDL is used purely for input; the PowerVR owns the display. Both the
    // Dreamcast keyboard (SDL key events) and the controller (SDL game
    // controller events) are handled in DCPVR_ProcessWindowMessages.
    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        LOG_WARN2("SDL input init failed: %s", SDL_GetError());
    }
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameControllerOpen(i);
        }
    }

    // First VMU with an LCD, for the FPS counter.
    gVmu = maple_enum_type(0, MAPLE_FUNC_LCD);
    gFps_last_time = SDL_GetTicks();
}

static void DCPVR_Swap(br_pixelmap* back_buffer) {
    DCPVR_ProcessWindowMessages();

    if (back_buffer != NULL && back_buffer->pixels != NULL) {
        const uint8_t* src = back_buffer->pixels;
        int w = back_buffer->width;
        int h = back_buffer->height;
        uint16_t* tex = (uint16_t*)pvram;
        for (int y = 0; y < h; y++) {
            uint16_t* dst = tex + (size_t)y * TEX_WIDTH;
            for (int x = 0; x < w; x++) {
                dst[x] = converted_palette_argb1555[src[x]];
            }
            src += w;
        }
        last_screen_src = back_buffer;
    }

    int sw = (last_screen_src != NULL) ? last_screen_src->width : render_width;
    int sh = (last_screen_src != NULL) ? last_screen_src->height : render_height;
    float u1 = (float)sw / TEX_WIDTH;
    float v1 = (float)sh / TEX_HEIGHT;

    // Map gBack_screen space to the 640x480 output, and find where the 3D render
    // viewport sits inside gBack_screen. The 3D screen coordinates are local to
    // that viewport, so the 3D (and its sky) must be shifted by the viewport
    // offset, otherwise they anchor top-left instead of in the framed centre.
    g3d_sx = (sw > 0) ? 640.0f / sw : 1.0f;
    g3d_sy = (sh > 0) ? 480.0f / sh : 1.0f;
    g3d_ox = (gRender_screen != NULL) ? (float)gRender_screen->base_x : 0.f;
    g3d_oy = (gRender_screen != NULL) ? (float)gRender_screen->base_y : 0.f;
    float vp_w = (gRender_screen != NULL) ? (float)gRender_screen->width : (float)sw;
    float vp_h = (gRender_screen != NULL) ? (float)gRender_screen->height : (float)sh;
    float vp_x0 = g3d_ox * g3d_sx;
    float vp_y0 = g3d_oy * g3d_sy;
    float vp_x1 = (g3d_ox + vp_w) * g3d_sx;
    float vp_y1 = (g3d_oy + vp_h) * g3d_sy;

    pvr_wait_ready();
    pvr_scene_begin();

    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t vert;

    // Opaque list: a solid sky background filling just the 3D viewport rectangle
    // (so the surrounding HUD frame stays black), then the hardware 3D world in
    // front of it. The 2D/HUD is composited in front below.
    pvr_list_begin(PVR_LIST_OP_POLY);

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));
    vert.argb = DCPVR3D_PaletteColor(gDC_sky_index);
    vert.oargb = 0;
    vert.flags = PVR_CMD_VERTEX;
    const float sky_z = 0.0001f; // far enough that all 3D draws in front
    vert.x = vp_x0; vert.y = vp_y0; vert.z = sky_z; vert.u = 0.f; vert.v = 0.f;
    pvr_prim(&vert, sizeof(vert));
    vert.x = vp_x1; vert.y = vp_y0; pvr_prim(&vert, sizeof(vert));
    vert.x = vp_x0; vert.y = vp_y1; pvr_prim(&vert, sizeof(vert));
    vert.x = vp_x1; vert.y = vp_y1; vert.flags = PVR_CMD_VERTEX_EOL;
    pvr_prim(&vert, sizeof(vert));

    dc3d_submit_opaque();
    pvr_list_finish();

    // The PowerVR requires lists be submitted in the order OP, OP_MOD, TR,
    // TR_MOD, PT, and renders them opaque -> punch-through -> translucent (so
    // translucent is always composited last, on top). The HUD must end up on
    // top of everything, so it goes in the translucent list, which means the
    // translucent list is submitted BEFORE the punch-through list below.

    // Translucent list: first the blended 3D decals (shadows, skidmarks) so they
    // blend over the road instead of z-fighting it, then the 2D/HUD overlay in
    // front. The overlay texture is ARGB1555 with index 0 (the cleared sky / 3D
    // region) transparent, so the hardware 3D shows through there while opaque HUD
    // pixels sit on top. Depth compare ALWAYS so the overlay is never occluded by
    // the 3D. This mirrors the original software build: HUD over the 3D.
    pvr_list_begin(PVR_LIST_TR_POLY);
    dc3d_submit_blended();
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
        PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
        TEX_WIDTH, TEX_HEIGHT, pvram, PVR_FILTER_NONE);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    vert.argb = PVR_PACK_COLOR(1.f, 1.f, 1.f, 1.f);
    vert.oargb = 0;
    vert.flags = PVR_CMD_VERTEX;
    const float hud_z = 1.0f;
    vert.x = 0.f;   vert.y = 0.f;   vert.z = hud_z; vert.u = 0.f; vert.v = 0.f;
    pvr_prim(&vert, sizeof(vert));
    vert.x = 640.f; vert.y = 0.f;   vert.u = u1;  vert.v = 0.f;
    pvr_prim(&vert, sizeof(vert));
    vert.x = 0.f;   vert.y = 480.f; vert.u = 0.f; vert.v = v1;
    pvr_prim(&vert, sizeof(vert));
    vert.x = 640.f; vert.y = 480.f; vert.u = u1;  vert.v = v1;
    vert.flags = PVR_CMD_VERTEX_EOL;
    pvr_prim(&vert, sizeof(vert));
    pvr_list_finish();

    // Punch-through list (submitted last per the required order): textured
    // geometry whose map keys colour index 0 as transparent (fences, signs,
    // foliage). It depth-tests like opaque but discards the keyed texels, so the
    // holes show the 3D behind instead of black fill. Rendered before the
    // translucent HUD, so the HUD still sits on top of it.
    pvr_list_begin(PVR_LIST_PT_POLY);
    dc3d_submit_punchthrough();
    pvr_list_finish();

    pvr_scene_finish();

    // All three 3D passes consumed the buffer; reset for next frame.
    g3d_tri_count = 0;

    if (harness_game_config.fps != 0) {
        Uint32 now = SDL_GetTicks();
        if (last_frame_time != 0) {
            unsigned int frame_time = now - last_frame_time;
            if (frame_time < 100) {
                int sleep_time = (1000 / (int)harness_game_config.fps) - frame_time;
                if (sleep_time > 5) {
                    thd_sleep(sleep_time);
                }
            }
        }
        last_frame_time = SDL_GetTicks();
    }

    // Update the VMU FPS counter roughly twice a second (the LCD is slow to
    // write over maple, so we must not do it every frame).
    gFrame_count++;
    if (gVmu != NULL) {
        Uint32 now = SDL_GetTicks();
        Uint32 elapsed = now - gFps_last_time;
        if (elapsed >= 500) {
            int fps = (int)((gFrame_count * 1000 + elapsed / 2) / elapsed);
            // Diagnostic: report fps and how many 3D triangles reached the PVR.
            printf("[dcpvr] fps=%d tris=%d tex=%d back=%dx%d vp=%d,%d+%dx%d\n",
                fps, g3d_last_submitted, g3d_tex_count,
                (last_screen_src != NULL) ? last_screen_src->width : -1,
                (last_screen_src != NULL) ? last_screen_src->height : -1,
                (gRender_screen != NULL) ? gRender_screen->base_x : -1,
                (gRender_screen != NULL) ? gRender_screen->base_y : -1,
                (gRender_screen != NULL) ? gRender_screen->width : -1,
                (gRender_screen != NULL) ? gRender_screen->height : -1);
            char buf[16];
            snprintf(buf, sizeof(buf), "FPS\n%d", fps);
            vmufb_clear(&gVmu_fb);
            vmufb_print_string_into(&gVmu_fb, NULL, 0, 0, 48, 32, 0, buf);
            vmufb_present(&gVmu_fb, gVmu);
            gFrame_count = 0;
            gFps_last_time = now;
        }
    }
}

static void DCPVR_PaletteChanged(br_colour entries[256]) {
    for (int i = 0; i < 256; i++) {
        br_uint_8 r = BR_RED(entries[i]);
        br_uint_8 g = BR_GRN(entries[i]);
        br_uint_8 b = BR_BLU(entries[i]);
        converted_palette[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        // ARGB1555: index 0 transparent, the rest opaque (alpha bit set).
        converted_palette_argb1555[i] = (i == 0)
            ? (uint16_t)0x0000
            : (uint16_t)(0x8000u | ((r & 0xF8) << 7) | ((g & 0xF8) << 2) | (b >> 3));
    }
    if (last_screen_src != NULL) {
        DCPVR_Swap(last_screen_src);
    }
}

static void DCPVR_Sleep(br_uint_32 ms) {
    thd_sleep(ms);
}

static br_uint_32 DCPVR_GetTicks(void) {
    return (br_uint_32)(timer_ms_gettime64() & 0xFFFFFFFF);
}

static int DCPVR_ShowCursor(int show) {
    (void)show;
    return 0;
}

static int DCPVR_SetWindowPos(void* hWnd, int x, int y, int nWidth, int nHeight) {
    (void)hWnd; (void)x; (void)y; (void)nWidth; (void)nHeight;
    return 0;
}

static void DCPVR_DestroyWindow(void) {
}

static int DCPVR_ShowErrorMessage(char* title, char* message) {
    fprintf(stderr, "%s: %s\n", title, message);
    return 0;
}

static void DCPVR_GetViewport(int* x, int* y, float* width_multiplier, float* height_multiplier) {
    *x = 0;
    *y = 0;
    *width_multiplier = 1.f;
    *height_multiplier = 1.f;
}

static int DCPVR_Platform_Init(tHarness_platform* platform) {
    platform->ProcessWindowMessages = DCPVR_ProcessWindowMessages;
    platform->Sleep = DCPVR_Sleep;
    platform->GetTicks = DCPVR_GetTicks;
    platform->ShowCursor = DCPVR_ShowCursor;
    platform->SetWindowPos = DCPVR_SetWindowPos;
    platform->DestroyWindow = DCPVR_DestroyWindow;
    platform->SetKeyHandler = DCPVR_SetKeyHandler;
    platform->GetKeyboardState = DCPVR_GetKeyboardState;
    platform->GetMousePosition = DCPVR_GetMousePosition;
    platform->GetMouseButtons = DCPVR_GetMouseButtons;
    platform->ShowErrorMessage = DCPVR_ShowErrorMessage;
    platform->CreateWindow_ = DCPVR_CreateWindow;
    platform->Swap = DCPVR_Swap;
    platform->PaletteChanged = DCPVR_PaletteChanged;
    platform->GetViewport = DCPVR_GetViewport;
    return 0;
}

const tPlatform_bootstrap DCPVR_bootstrap = {
    "dcpvr",
    "Dreamcast PowerVR video with SDL2 input",
    ePlatform_cap_software,
    DCPVR_Platform_Init,
};
