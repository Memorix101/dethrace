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

// Feature isolation switches for the persistent level-geometry flicker. Every
// system that touches per-triangle depth/texture state is suspect and every
// individual test so far (z-bias magnitude and sign, mipmapping, eviction
// timing, fog) changed nothing, so instead of guessing one at a time: all off
// here as a baseline, then flip one at a time in later builds until either the
// flicker reappears (pins the cause) or everything is back on with no
// flicker returning (means it was never in this set).
#define DC_FEAT_MIPMAP 0
#define DC_FEAT_ZBIAS 0
// Re-enabled: this was switched off as part of the old flicker-isolation
// baseline and never turned back on after the flicker was fixed elsewhere.
// The fog parameters (gDC_fog_*) have been computed by DepthEffect all along;
// this just lets the per-polygon fog_type actually use them. Restores the
// level fog the software renderer shows, and masks track pop-in at the yon.
#define DC_FEAT_FOG 1
#define DC_FEAT_EVICT 1
#define DC_FEAT_BILINEAR 0
#define DC_FEAT_STALECHECK 0

extern void QuitGame(void);
extern br_pixelmap* gBack_screen;
// The 3D render viewport. It is a sub-rectangle of gBack_screen (the game frames
// it with the HUD), so its base_x/base_y give where the 3D belongs and we offset
// the hardware 3D and sky into that rectangle.
extern br_pixelmap* gRender_screen;
// Palette index of the solid sky colour, chosen by the game each frame (see
// ConditionallyFillWithSky). Painted behind the hardware 3D.
extern int gDC_sky_index;
// Hardware fog parameters, recomputed each frame in DepthEffect() (depth.c)
// from the same range/colour the original software DoFog() uses. gDC_fog_min/
// max are eye-space distances in world units, matching what
// pvr_fog_table_linear() expects.
extern int gDC_fog_enabled;
extern float gDC_fog_min;
extern float gDC_fog_max;
extern uint32_t gDC_fog_colour;

static pvr_ptr_t pvram;
static uint16_t converted_palette[256];
// ARGB1555 version of the palette for the 2D/HUD overlay. Index 0 (the cleared /
// sky colour) is transparent, every other index opaque, so the overlay can be
// composited in front of the hardware 3D with the 3D showing through index 0.
static uint16_t converted_palette_argb1555[256];

// When non-NULL, DCPVR3D_RegisterTexture bakes source texels through this
// 256-entry ARGB1555 lookup instead of the raw palette. Used for blended effect
// sprites (smoke/dust): their texels are source indices into the game's
// blend/shade table (out = blend[dest*256 + src]), NOT displayable colours, so
// the raw index renders the wrong hue (a smoke texel 0xf1 is dark blue in the
// palette). dc_triangle_fill fills this with palette[blend[D*256 + src]] against
// a representative background D so the baked texture shows the tinted colour.
// Set only around the register call for such a sprite, then cleared.
const uint16_t* g_dc_blend_lut;
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

// Peak observed per frame is ~1825 triangles; 24000 was ~13x overkill and its
// static buffers (g3d_verts alone is DC3D_MAX_TRIS*3*24 bytes) ate ~1.85 MB of
// the Dreamcast's 16 MB, which was pushing level loads into out-of-memory
// aborts. 8192 keeps a ~4.5x margin (and overflow just drops the excess
// triangles for one frame, never crashes) while giving back ~1.2 MB of RAM.
#define DC3D_MAX_TRIS 8192
static tDC3D_vertex g3d_verts[DC3D_MAX_TRIS * 3];
static int16_t g3d_tritex[DC3D_MAX_TRIS]; // texture cache index per triangle, -1 = untextured
static uint8_t g3d_tricat[DC3D_MAX_TRIS];  // 0 = opaque (OP), 1 = punch-through (PT), 2 = blended (TR)
static int g3d_order[DC3D_MAX_TRIS];       // triangle indices sorted by (category, texture)
static int g3d_tri_count;
static int g3d_last_submitted; // diagnostic: triangles drawn last frame
static float g3d_dbg[6];       // diagnostic: first triangle's raw x,y,z per vertex

// Temporary diagnostic counters (incremented from v1model.c's dc_triangle_fill)
// to tell apart "genuinely untextured" triangles from "textured but PVR
// registration failed" triangles, since both currently render as a flat colour.
int g3d_diag_notex;
int g3d_diag_texfail;
extern int g3d_diag_nullps;
extern int g3d_diag_nullbuf;
extern int g3d_diag_wrongtype;
extern int g3d_diag_lasttype;
// diagnostic: triangles dropped whole by dc_triangle_fill's near-zero-W guard
// (no fallback draw - a dropped triangle exposes the black PVR background)
int g3d_diag_degenw;
// diagnostic: smallest surviving (non-dropped) vertex W seen since the last
// report, to check whether BRender's clipper is letting through W values
// small enough to blow up 1/w precision without tripping the degenerate guard.
float g3d_diag_minw = 1.0e30f;
// diagnostic: range of comp_f[C_I] actually reaching dc_face_colour for
// TEXTURED triangles, to check whether it's really a clean 0..1 brightness
// (as dc_face_colour assumes) for every material/lighting mode in the game, or
// whether some materials drive it through a different numeric convention that
// the *255 clamp-to-0..255 formula mishandles (silently going pure black or
// pure white instead of the intended shade).
float g3d_diag_minI = 1.0e30f;
float g3d_diag_maxI = -1.0e30f;
// diagnostic: does comp_scales/offsets[C_I] vary per material (consistent with
// material->index_base/index_range) or stay constant across the whole frame?
float g3d_diag_iscale_min = 1.0e30f;
float g3d_diag_iscale_max = -1.0e30f;
float g3d_diag_ioffset_min = 1.0e30f;
float g3d_diag_ioffset_max = -1.0e30f;
// diagnostic: how often DepthEffectSky actually calls DoHorizon (vs skips it)
int g3d_diag_dohorizon_run;
int g3d_diag_dohorizon_skip;

// Texture cache: BRender's 8bpp paletted textures are converted once to RGB565
// PowerVR textures, keyed by their source pixel pointer. Colours come from the
// game's current palette.
typedef struct {
    void* key;     // source 8bpp pixel pointer
    pvr_ptr_t tex; // PVR texture memory
    int pw, ph;    // power-of-two PVR dimensions
    float us, vs;  // UV scale = real_size / pow2_size
    int mipmapped; // 1 = square twiddled mipmap chain, 0 = plain non-twiddled
    int last_used; // g3d_frame_num this slot was last hit or (re)registered
    int srcw, srch;   // source width/height at registration, to catch reuse
    uint32_t checksum; // sparse hash of source pixels, to catch reuse
} tDC3D_texture;

// diagnostic/correctness: cache hits where the source pointer matches but the
// image doesn't (BRender freed and reused this address for different texture
// data - track art streams in/out per section, so this happens far more for
// level textures than for the one car texture set that lives the whole race).
// A stale hit would silently serve the wrong PVR texture for that surface.
int g3d_diag_stale_tex;

// Cheap sparse hash of 8bpp source pixels: enough samples to almost certainly
// catch a genuinely different image, without the cost of hashing the whole
// buffer on every cache lookup.
static uint32_t dc3d_tex_checksum(const uint8_t* pixels, int w, int h, int stride) {
    uint32_t hv = 2166136261u;
    for (int s = 0; s < 32; s++) {
        int x = (s * 37) % w;
        int y = (s * 23) % h;
        hv = (hv ^ pixels[y * stride + x]) * 16777619u;
    }
    return hv;
}

// Raised from 192: the cache was pinned at 192/192 with evictions climbing
// steadily during play (repeated texture re-conversion + re-upload) while VRAM
// still had room. More slots let the working set stay resident (observed peak
// ~197). Kept at 256 rather than higher because each slot also costs main RAM
// (this struct array + the sort key array), and the game runs right at the edge
// of the Dreamcast's 16 MB in low-memory mode - 320 tipped model prep into an
// out-of-memory abort at level load. 256 clears the working set with margin
// while staying within budget. VRAM-full is still handled (dc3d_alloc_evicting
// evicts on pvr_mem_malloc failure).
#define DC3D_MAX_TEX 256
static tDC3D_texture g3d_tex[DC3D_MAX_TEX];
static int g3d_tex_count;
// Incremented once per frame in DCPVR_Swap. Drives LRU eviction below.
static int g3d_frame_num;

// diagnostic: how many textures dc3d_evict_lru has actually freed, ever.
int g3d_diag_evictions;

// Free the least-recently-used texture that wasn't touched this frame OR the
// previous one, and isn't already free. The 1-frame margin (not just "this
// frame") matters because eviction runs during the CPU-side BRender walk for
// frame N, while frame N-1's submission is still rendering asynchronously on
// the PVR (DCPVR_Swap's pvr_wait_ready for frame N hasn't even been reached
// yet at this point) - evicting a texture frame N-1 is still using would
// overwrite VRAM out from under that in-flight render, corrupting whatever is
// on screen for one frame. Skipping anything used in the last 2 frames keeps
// eviction at least a full frame behind the hardware.
// Returns its slot index ready for reuse, or -1 if nothing is evictable.
static int dc3d_evict_lru(void) {
    int victim = -1;
    int oldest = 0;
    for (int i = 0; i < g3d_tex_count; i++) {
        if (g3d_tex[i].key == NULL || g3d_tex[i].last_used >= g3d_frame_num - 1) {
            continue;
        }
        if (victim < 0 || g3d_tex[i].last_used < oldest) {
            victim = i;
            oldest = g3d_tex[i].last_used;
        }
    }
    if (victim < 0) {
        return -1;
    }
    pvr_mem_free(g3d_tex[victim].tex);
    g3d_tex[victim].key = NULL;
    g3d_tex[victim].tex = NULL;
    g3d_diag_evictions++;
    return victim;
}

// pvr_mem_malloc, evicting LRU textures (not used this frame) and retrying on
// failure, so a fragmented/full VRAM heap degrades by dropping old textures
// instead of leaving newly-needed ones permanently unregistered.
static pvr_ptr_t dc3d_alloc_evicting(uint32_t bytes) {
    pvr_ptr_t p = pvr_mem_malloc(bytes);
    if (!DC_FEAT_EVICT) {
        return p;
    }
    int guard = DC3D_MAX_TEX;
    while (p == NULL && guard-- > 0) {
        if (dc3d_evict_lru() < 0) {
            break;
        }
        p = pvr_mem_malloc(bytes);
    }
    return p;
}

// Only square textures up to this size get mipmaps (to bound VRAM use). Bilinear
// mipmapping kills the texture shimmer/crawl on minified surfaces while moving.
#define DC3D_MIP_MAX 256
static uint16_t g3d_mip_a[DC3D_MIP_MAX * DC3D_MIP_MAX];
static uint16_t g3d_mip_b[DC3D_MIP_MAX * DC3D_MIP_MAX];

static int dc_pow2_ceil(int v) {
    int p = 8;
    while (p < v && p < 1024) {
        p <<= 1;
    }
    return p;
}

// Byte offset of the mipmap level of side `size` within a twiddled 16bpp mipmap
// texture (PowerVR layout, smallest level first after a 6-byte pad). From GLdc.
static uint32_t dc_mip_offset(int size) {
    switch (size) {
    case 1024: return 0xAAAB0;
    case 512:  return 0x2AAB0;
    case 256:  return 0x0AAB0;
    case 128:  return 0x02AB0;
    case 64:   return 0x00AB0;
    case 32:   return 0x002B0;
    case 16:   return 0x000B0;
    case 8:    return 0x00030;
    case 4:    return 0x00010;
    case 2:    return 0x00008;
    case 1:    return 0x00006;
    default:   return 0;
    }
}

// Box-downsample an ARGB1555 image by 2x into dst. Alpha stays set if any of the
// four source texels is opaque, so punch-through edges do not erode.
static void dc_mip_downsample(const uint16_t* src, uint16_t* dst, int s) {
    int d = s / 2;
    for (int y = 0; y < d; y++) {
        const uint16_t* r0 = src + (y * 2) * s;
        const uint16_t* r1 = r0 + s;
        uint16_t* o = dst + y * d;
        for (int x = 0; x < d; x++) {
            uint16_t p0 = r0[x * 2], p1 = r0[x * 2 + 1], p2 = r1[x * 2], p3 = r1[x * 2 + 1];
            uint32_t a = ((p0 | p1 | p2 | p3) & 0x8000);
            uint32_t r = (((p0 >> 10) & 0x1F) + ((p1 >> 10) & 0x1F) + ((p2 >> 10) & 0x1F) + ((p3 >> 10) & 0x1F)) >> 2;
            uint32_t g = (((p0 >> 5) & 0x1F) + ((p1 >> 5) & 0x1F) + ((p2 >> 5) & 0x1F) + ((p3 >> 5) & 0x1F)) >> 2;
            uint32_t b = ((p0 & 0x1F) + (p1 & 0x1F) + (p2 & 0x1F) + (p3 & 0x1F)) >> 2;
            o[x] = (uint16_t)(a | (r << 10) | (g << 5) | b);
        }
    }
}

// Register (or look up) an 8bpp texture and return its cache index, or -1.
int DCPVR3D_RegisterTexture(void* pixels, int w, int h, int stride) {
    if (pixels == NULL || w <= 0 || h <= 0) {
        return -1;
    }
    // Fast path: a material's triangles are submitted back to back, so the vast
    // majority of calls ask for the exact same texture as the previous call.
    // Remember the last hit and check it first, skipping the linear scan over
    // up to 192 cache slots that would otherwise run for every textured
    // triangle. Re-validated against the slot's current key so an eviction that
    // reused the slot can't return a stale index.
    static void* s_last_key = NULL;
    static int s_last_slot = -1;
    if (pixels == s_last_key && s_last_slot >= 0 &&
        s_last_slot < g3d_tex_count && g3d_tex[s_last_slot].key == pixels &&
        (!DC_FEAT_STALECHECK || (g3d_tex[s_last_slot].srcw == w && g3d_tex[s_last_slot].srch == h))) {
        g3d_tex[s_last_slot].last_used = g3d_frame_num;
        return s_last_slot;
    }
    uint32_t csum = DC_FEAT_STALECHECK ? dc3d_tex_checksum((const uint8_t*)pixels, w, h, stride) : 0;
    int stale_slot = -1;
    for (int i = 0; i < g3d_tex_count; i++) {
        if (g3d_tex[i].key == pixels) {
            if (!DC_FEAT_STALECHECK || (g3d_tex[i].srcw == w && g3d_tex[i].srch == h && g3d_tex[i].checksum == csum)) {
                g3d_tex[i].last_used = g3d_frame_num;
                s_last_key = pixels;
                s_last_slot = i;
                return i;
            }
            // Same pointer, different image: re-register into this same slot
            // rather than treating it as a fresh texture, so it isn't also
            // mistaken for a duplicate of whatever else now holds this pointer.
            g3d_diag_stale_tex++;
            if (g3d_tex[i].tex != NULL) {
                pvr_mem_free(g3d_tex[i].tex);
            }
            g3d_tex[i].key = NULL;
            g3d_tex[i].tex = NULL;
            stale_slot = i;
            break;
        }
    }
    // Prefer an already-freed slot (from a past eviction, or just invalidated
    // above) over growing the array; only evict when the array is full and has
    // no free slot either.
    int slot = stale_slot;
    for (int i = 0; slot < 0 && i < g3d_tex_count; i++) {
        if (g3d_tex[i].key == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (g3d_tex_count < DC3D_MAX_TEX) {
            slot = g3d_tex_count++;
        } else {
            slot = DC_FEAT_EVICT ? dc3d_evict_lru() : -1;
            if (slot < 0) {
                return -1;
            }
        }
    }
    int pw = dc_pow2_ceil(w);
    int ph = dc_pow2_ceil(h);
    const uint8_t* src = (const uint8_t*)pixels;
    // Blended effect sprites bake through the blend LUT (see g_dc_blend_lut);
    // everything else uses the raw palette.
    const uint16_t* bake = g_dc_blend_lut ? g_dc_blend_lut : converted_palette_argb1555;

    // Square textures up to DC3D_MIP_MAX get a twiddled mipmap chain (bilinear
    // mipmapping removes the texture shimmer/crawl while moving). The base level
    // is the clamped source in a pw x pw square; UV scale keeps the real w x h in
    // the top-left. Non-square (or oversized) textures keep the plain
    // non-twiddled path below.
    if (DC_FEAT_MIPMAP && pw == ph && pw <= DC3D_MIP_MAX) {
        int s = pw;
        pvr_ptr_t tex = dc3d_alloc_evicting((uint32_t)(s * s * 2) + dc_mip_offset(s));
        if (tex != NULL) {
            // Build the base level (s x s, ARGB1555, edge-clamped) in g3d_mip_a.
            for (int y = 0; y < s; y++) {
                const uint8_t* srow = src + (y < h ? y : h - 1) * stride;
                uint16_t* drow = g3d_mip_a + y * s;
                for (int x = 0; x < s; x++) {
                    drow[x] = bake[srow[x < w ? x : w - 1]];
                }
            }
            // Twiddle-load each level (largest first), downsampling as we go.
            uint16_t* cur = g3d_mip_a;
            uint16_t* nxt = g3d_mip_b;
            for (int d = s; ; d /= 2) {
                pvr_txr_load_ex(cur, (pvr_ptr_t)((uint8_t*)tex + dc_mip_offset(d)),
                    (uint32_t)d, (uint32_t)d, PVR_TXRLOAD_16BPP);
                if (d == 1) {
                    break;
                }
                dc_mip_downsample(cur, nxt, d);
                uint16_t* tmp = cur; cur = nxt; nxt = tmp;
            }
            g3d_tex[slot].key = pixels;
            g3d_tex[slot].tex = tex;
            g3d_tex[slot].pw = s;
            g3d_tex[slot].ph = s;
            g3d_tex[slot].us = (float)w / s;
            g3d_tex[slot].vs = (float)h / s;
            g3d_tex[slot].mipmapped = 1;
            g3d_tex[slot].last_used = g3d_frame_num;
            g3d_tex[slot].srcw = w;
            g3d_tex[slot].srch = h;
            g3d_tex[slot].checksum = csum;
            s_last_key = pixels;
            s_last_slot = slot;
            return slot;
        }
        // fall through to the plain path if the mipmap allocation failed
    }

    pvr_ptr_t tex = dc3d_alloc_evicting(pw * ph * 2);
    if (tex == NULL) {
        return -1;
    }
    // ARGB1555 so colour index 0 is transparent (alpha 0). Opaque-map surfaces
    // are drawn in the opaque list where alpha is ignored (index 0 reads as
    // black); punch-through surfaces are drawn in the PT list where alpha 0 is
    // keyed out, giving the transparent parts of fences, signs, etc.
    uint16_t* dst = (uint16_t*)tex;
    for (int y = 0; y < ph; y++) {
        const uint8_t* srow = src + (y < h ? y : h - 1) * stride;
        uint16_t* drow = dst + y * pw;
        for (int x = 0; x < pw; x++) {
            drow[x] = bake[srow[x < w ? x : w - 1]];
        }
    }
    g3d_tex[slot].key = pixels;
    g3d_tex[slot].tex = tex;
    g3d_tex[slot].pw = pw;
    g3d_tex[slot].ph = ph;
    g3d_tex[slot].us = (float)w / pw;
    g3d_tex[slot].vs = (float)h / ph;
    g3d_tex[slot].mipmapped = 0;
    g3d_tex[slot].last_used = g3d_frame_num;
    g3d_tex[slot].srcw = w;
    g3d_tex[slot].srch = h;
    g3d_tex[slot].checksum = csum;
    s_last_key = pixels;
    s_last_slot = slot;
    return slot;
}

static uint16_t g_blend_lut_buf[256];
static const void* g_blend_lut_table; // cache key so the LUT rebuilds only when the (table,row) changes
static int g_blend_lut_row = -1;

// Arm g_dc_blend_lut for the next DCPVR3D_RegisterTexture call so an effect
// sprite's texels bake through one row of a 256-wide game table
// (out = table[row*stride + src]) - turning the source indices into the colours
// the software renderer would produce, instead of showing the raw (wrong-hue)
// index. Used for the flames: softrend maps a textured sprite's texel through
// index_shade at the vertex-intensity row (shade[intensity*256 + texel], see
// fti8pizp.c), which DC otherwise skips. Cached by (table pointer, row).
void DCPVR3D_ArmRemapLut(const void* table, int stride, int row) {
    if (table == NULL) {
        g_dc_blend_lut = NULL;
        return;
    }
    if (g_blend_lut_table != table || g_blend_lut_row != row) {
        const uint8_t* base = (const uint8_t*)table + (size_t)row * stride;
        for (int s = 0; s < 256; s++) {
            g_blend_lut_buf[s] = (s == 0) ? (uint16_t)0 : converted_palette_argb1555[base[s]];
        }
        g_blend_lut_table = table;
        g_blend_lut_row = row;
    }
    g_dc_blend_lut = g_blend_lut_buf;
}

void DCPVR3D_DisarmBlendLut(void) {
    g_dc_blend_lut = NULL;
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

// Fast path for the per-triangle bridge (dc_triangle_fill): reserve one
// triangle's three vertices and let the caller write them in place. Avoids
// AddTriTex's 20 scalar stack arguments plus a second copy per triangle -
// dc_triangle_fill runs for every visible triangle, so this is hot. Returns
// NULL when the frame buffer of triangles is full (caller just drops the tri,
// same policy as AddTriTex). The returned pointer is 3 consecutive
// tDC3D_vertex (layout shared with v1model.c - see the mirror declaration
// there).
void* DCPVR3D_AllocTri(int texid, int category) {
    if (g3d_tri_count >= DC3D_MAX_TRIS) {
        return NULL;
    }
    g3d_tritex[g3d_tri_count] = (int16_t)texid;
    g3d_tricat[g3d_tri_count] = (uint8_t)category;
    return &g3d_verts[3 * g3d_tri_count++];
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
// decal drawn after the road it sits on stays consistently in front. `i` is the
// global submission index across the whole frame (up to ~1800). This was
// previously dropped to 1e-8 on suspicion it was amplifying too much on close
// geometry and causing jagged black spikes near the car - that turned out to be
// an unrelated bug (the car's drop shadow, fixed separately in
// dc_triangle_fill/v1model.c).
//
// Neither 1e-6 nor 1e-8 nor a multiplicative 1e-4 stopped level-only flicker
// (cars never show it). PowerVR depth is 1/w, which loses absolute precision at
// distance - the same real-world gap between two surfaces shrinks roughly with
// distance^2 once converted to 1/w, so two architectural pieces authored flush
// against each other (an archway frame against its tunnel wall, trim against a
// pillar) can be numerically tied at typical level-geometry range, flipping
// order as the camera rotates and recomputes 1/w. The car never shows this
// because it's always close, where 1/w still has ample precision. A
// *multiplicative* bias (the old `z * (1 + i*eps)`) scales down exactly where
// this hurts most - tiny depth values get a tinier absolute nudge - so it can
// never reliably beat the precision loss at range. Made it additive instead: a
// fixed absolute amount per submission-order step, independent of how small the
// depth value already is.
#define DC3D_ZBIAS_PER_TRI 1.0e-5f

static void dc3d_emit_vertex(const tDC3D_vertex* t, float us, float vs, float zadd, int eol) {
    pvr_vertex_t v;
    v.flags = eol ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    v.x = (t->x + g3d_ox) * g3d_sx;
    v.y = (t->y + g3d_oy) * g3d_sy;
    v.z = t->z + zadd;
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
            } else if (g3d_tex[tex].mipmapped) {
                // Twiddled square texture with a mipmap chain: bilinear mipmapping.
                pvr_poly_cxt_txr(&cxt, list, PVR_TXRFMT_ARGB1555,
                    g3d_tex[tex].pw, g3d_tex[tex].ph, g3d_tex[tex].tex,
                    DC_FEAT_BILINEAR ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
                cxt.txr.mipmap = PVR_MIPMAP_ENABLE;
            } else {
                pvr_poly_cxt_txr(&cxt, list,
                    PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                    g3d_tex[tex].pw, g3d_tex[tex].ph, g3d_tex[tex].tex,
                    DC_FEAT_BILINEAR ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
            }
            cxt.gen.culling = PVR_CULLING_NONE;
            cxt.gen.fog_type = (DC_FEAT_FOG && gDC_fog_enabled) ? PVR_FOG_TABLE : PVR_FOG_DISABLE;
            pvr_poly_compile(&hdr, &cxt);
            pvr_prim(&hdr, sizeof(hdr));
        }
        float us = (tex < 0) ? 1.f : g3d_tex[tex].us;
        float vs = (tex < 0) ? 1.f : g3d_tex[tex].vs;
        float zadd = DC_FEAT_ZBIAS ? (float)i * DC3D_ZBIAS_PER_TRI : 0.0f;
        const tDC3D_vertex* t = &g3d_verts[i * 3];
        dc3d_emit_vertex(&t[0], us, vs, zadd, 0);
        dc3d_emit_vertex(&t[1], us, vs, zadd, 0);
        dc3d_emit_vertex(&t[2], us, vs, zadd, 1);
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
    // decals + the HUD overlay), and the urban scenes push ~1200-1800 triangles.
    // Bin size 16 + overflow_count 8 still let dense tiles overflow (textures
    // popping in/out along tile boundaries while moving). OPB memory cost is
    // (sum of active bin sizes) * tile_count * (1 + overflow_count), allocated
    // twice (double-buffered) - PVR_BINSIZE_32 doubles the per-tile headroom, so
    // overflow_count is kept low (it's a multiplier on the now-bigger base size)
    // to land at roughly the same total OPB footprint as the old 16/8 config
    // instead of starving the PVR texture heap (which caused an out-of-memory
    // crash when both were maxed out at 32/16).
    pvr_init_params_t params = {
        { PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32 },
        1024 * 1024, // vertex buffer
        0,           // dma disabled
        0,           // fsaa disabled
        0,           // translucent autosort enabled
        4,           // opb_overflow_count - extra OPBs to stop tile-overflow flicker
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

// Temporary walk attribution split (us, accumulated, printed per second from
// the fps line). The earlier profiling lumped everything between two Swaps into
// one "walk" number and attributed it to BRender - but that span also contains
// the game logic (physics/AI/sound) and the software effects. Before touching
// BRender's core, split it:
//   g_dc_t_scene - the whole BrZbSceneRenderBegin..End block in RenderAFrame
//                  (BRender geometry + our triangle bridge)
//   g_dc_t_fx    - software effects inside that block (DepthEffect*, splashes,
//                  smoke, sparks, proximity rays - CPU pixel work)
//   walk - scene = game logic and everything else per frame.
uint64_t g_dc_t_scene, g_dc_t_fx, g_dc_t_fx2;
// Sub-split of g_dc_t_scene (also filled from RenderAFrame): the shadow pass
// (per-car ray/face maths + its own scene render), the non-track actors
// (cars/peds), and the track walk. scene minus these three = lollipops,
// depth-effects wrapper and BrZbSceneRenderEnd.
uint64_t g_dc_t_shad, g_dc_t_ntrack, g_dc_t_track;
// Actors under gNon_track_actor this frame (set from RenderAFrame).
int g_dc_actor_count;
static uint64_t g_t_walk;
static uint64_t g_last_swap_end;

static void DCPVR_Swap(br_pixelmap* back_buffer) {
    {
        uint64_t now = timer_us_gettime64();
        if (g_last_swap_end != 0) {
            g_t_walk += now - g_last_swap_end;
        }
    }
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

    // Hardware table fog must be configured outside scene begin/finish. Only
    // touch it when active: the per-polygon fog_type set in dc3d_submit_list
    // already stays PVR_FOG_DISABLE otherwise, so a stale table is harmless.
    if (gDC_fog_enabled && gDC_fog_min < gDC_fog_max) {
        float fr = ((gDC_fog_colour >> 16) & 0xFF) / 255.f;
        float fg = ((gDC_fog_colour >> 8) & 0xFF) / 255.f;
        float fb = (gDC_fog_colour & 0xFF) / 255.f;
        pvr_fog_table_color(1.0f, fr, fg, fb);
        pvr_fog_table_linear(gDC_fog_min, gDC_fog_max);
    }

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
    // Far larger than any 3D 1/w (near foreground geometry can exceed 1.0), so
    // the translucent autosort always places the HUD overlay last = on top. At
    // 1.0 the HUD lost to near geometry in the lower screen and the 3D bled
    // through the gauges/text there.
    const float hud_z = 1000.0f;
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
    g3d_frame_num++;

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
    {
        Uint32 now = SDL_GetTicks();
        Uint32 elapsed = now - gFps_last_time;
        if (elapsed >= 500) {
            int fps = (int)((gFrame_count * 1000 + elapsed / 2) / elapsed);
            // Perf diagnostic: fps + how many 3D triangles reached the PVR + how
            // full the texture cache is. Re-enabled while optimising the path.
            // xf_fast/xf_scalar: per-vertex transforms through the sh4zam (FTRV)
            // path vs the remaining scalar paths, summed over this interval -
            // shows whether extending sh4zam to the scalar functions is worth
            // it. Divide by frame count for per-frame vertex volume.
            int _fc = gFrame_count ? gFrame_count : 1;
            // walk = everything between two Swaps; scene = the BrZbSceneRender
            // block (BRender); fx = software effects inside it; logic = the rest.
            // fx = sky-dome add + smoke/splash/sparks; fxsmk = just the latter.
            printf("[dcpvr] fps=%d tris=%d act=%d | scene=%luus (shad=%lu ntrk=%lu trk=%lu fx=%lu fxsmk=%lu) logic=%luus\n",
                fps, g3d_last_submitted, g_dc_actor_count,
                (unsigned long)(g_dc_t_scene / _fc),
                (unsigned long)(g_dc_t_shad / _fc),
                (unsigned long)(g_dc_t_ntrack / _fc),
                (unsigned long)(g_dc_t_track / _fc),
                (unsigned long)(g_dc_t_fx / _fc),
                (unsigned long)(g_dc_t_fx2 / _fc),
                (unsigned long)((g_t_walk > g_dc_t_scene ? g_t_walk - g_dc_t_scene : 0) / _fc));
            g_t_walk = 0;
            g_dc_t_scene = 0;
            g_dc_t_fx = 0;
            g_dc_t_fx2 = 0;
            g_dc_t_shad = 0;
            g_dc_t_ntrack = 0;
            g_dc_t_track = 0;
            g3d_diag_notex = 0;
            g3d_diag_texfail = 0;
            g3d_diag_nullps = 0;
            g3d_diag_nullbuf = 0;
            g3d_diag_wrongtype = 0;
            g3d_diag_dohorizon_run = 0;
            g3d_diag_dohorizon_skip = 0;
            g3d_diag_degenw = 0;
            g3d_diag_minw = 1.0e30f;
            g3d_diag_minI = 1.0e30f;
            g3d_diag_maxI = -1.0e30f;
            g3d_diag_iscale_min = 1.0e30f;
            g3d_diag_iscale_max = -1.0e30f;
            g3d_diag_ioffset_min = 1.0e30f;
            g3d_diag_ioffset_max = -1.0e30f;
            if (gVmu != NULL) {
                char buf[16];
                snprintf(buf, sizeof(buf), "FPS\n%d", fps);
                vmufb_clear(&gVmu_fb);
                vmufb_print_string_into(&gVmu_fb, NULL, 0, 0, 48, 32, 0, buf);
                vmufb_present(&gVmu_fb, gVmu);
            }
            gFrame_count = 0;
            gFps_last_time = now;
        }
    }

    // End-of-Swap mark so the next frame's walk excludes this Swap itself
    // (and the fps-limiter sleep above).
    g_last_swap_end = timer_us_gettime64();
}

// Free all cached PowerVR textures and reset the cache. Called when the palette
// changes wholesale (a level reload), because the cached textures were converted
// with the previous palette and would otherwise show stale/garish colours.
static void dc3d_clear_texture_cache(void) {
    for (int i = 0; i < g3d_tex_count; i++) {
        if (g3d_tex[i].tex != NULL) {
            pvr_mem_free(g3d_tex[i].tex);
        }
    }
    g3d_tex_count = 0;
}

static void DCPVR_PaletteChanged(br_colour entries[256]) {
    int big_change = 0;
    for (int i = 0; i < 256; i++) {
        br_uint_8 r = BR_RED(entries[i]);
        br_uint_8 g = BR_GRN(entries[i]);
        br_uint_8 b = BR_BLU(entries[i]);
        uint16_t nc = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        // Count entries that changed a lot vs the previous palette: a gradual
        // fade nudges every entry a little, a level reload swaps most outright.
        uint16_t oc = converted_palette[i];
        int dr = ((nc >> 11) & 0x1F) - ((oc >> 11) & 0x1F);
        int dg = ((nc >> 5) & 0x3F) - ((oc >> 5) & 0x3F);
        int db = (nc & 0x1F) - (oc & 0x1F);
        if (dr < 0) dr = -dr;
        if (dg < 0) dg = -dg;
        if (db < 0) db = -db;
        if (dr + dg + db > 16) {
            big_change++;
        }
        converted_palette[i] = nc;
        // ARGB1555: index 0 transparent, the rest opaque (alpha bit set).
        converted_palette_argb1555[i] = (i == 0)
            ? (uint16_t)0x0000
            : (uint16_t)(0x8000u | ((r & 0xF8) << 7) | ((g & 0xF8) << 2) | (b >> 3));
    }
    if (big_change > 96) {
        dc3d_clear_texture_cache();
    }
    // The sprite bake LUT is built from converted_palette_argb1555, so force it
    // to rebuild after any palette change.
    g_blend_lut_table = NULL;
    g_blend_lut_row = -1;
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
