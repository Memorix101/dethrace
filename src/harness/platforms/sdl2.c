#include <SDL.h>

#include "harness.h"
#include "memf.h"
#include "harness/config.h"
#include "harness/hooks.h"
#include "harness/trace.h"
<<<<<<< HEAD
#include "sdl2_scancode_map.h"
#include "sdl2_syms.h"

SDL_COMPILE_TIME_ASSERT(sdl2_platform_requires_SDL2, SDL_MAJOR_VERSION == 2);
=======
#include "sdl2_scancode_to_dinput.h"
#include "sdl2_gamepad_to_dinput.h"
SDL_Window* window;
SDL_Renderer* renderer;
SDL_Texture* screen_texture;
uint32_t converted_palette[256];
br_pixelmap* last_screen_src;
int render_width, render_height;
>>>>>>> origin/pvr

static SDL_Window* window;
static SDL_Renderer* renderer;
static SDL_Texture* screen_texture;
static br_uint_32 converted_palette[256];
static br_pixelmap* last_screen_src;

<<<<<<< HEAD
static SDL_GLContext* gl_context;

static int render_width, render_height;

static Uint32 last_frame_time;

static void (*gKeyHandler_func)(void);
=======
uint8_t directinput_key_state[SDL_NUM_SCANCODES];
#include <kos.h>
#include <stdatomic.h>
#include "../vmu_profiler.h"
#include <stdio.h>
#include <stdint.h>
#include <kos/init.h>
#include <arch/arch.h>



//extern size_t xform_verts;
void update_transformed_verts(vmu_profiler_measurement_t *m)
{
	//m->ustorage = (size_t)xform_verts;
}

void fps_callback(vmu_profiler_measurement_t *m) {
    pvr_stats_t stats;
    pvr_get_stats(&stats);
    m->fstorage = stats.frame_rate;  
}

void mem_callback(vmu_profiler_measurement_t *m) {
    void* base = (void*)(uintptr_t)page_phys_base; // Cast required
    void* top = (void*)(uintptr_t)_arch_mem_top;   // Cast required
    void* current = sbrk(0);      // Current break (end of allocated heap)

    uint32_t total_memory = (uintptr_t)top - (uintptr_t)base;
    uint32_t used_memory = (uintptr_t)current - (uintptr_t)base;
    uint32_t free_memory = total_memory - used_memory;

    // Convert to megabytes
    float total_memory_mb = total_memory / (1024.0f * 1024.0f);
    float used_memory_mb = used_memory / (1024.0f * 1024.0f);
    float free_memory_mb = free_memory / (1024.0f * 1024.0f);

    m->fstorage = used_memory_mb;
}

#include <kos.h>
#include <stdio.h>

void cpu_usage_callback(vmu_profiler_measurement_t *m) {
   static uint64_t last_active_time = 0;
    static uint64_t last_real_time = 0;

    // Get current active CPU time in nanoseconds
    uint64_t current_active_time = perf_cntr_timer_ns();

    // Get current real-world time in milliseconds
    uint64_t current_real_time = timer_ms_gettime64();

    if (last_real_time == 0) {
        // Initialize the last times during the first call
        last_active_time = current_active_time;
        last_real_time = current_real_time;
        return;
    }

    // Calculate elapsed times
    uint64_t active_time_elapsed = current_active_time - last_active_time;
    uint64_t real_time_elapsed = (current_real_time - last_real_time) * 1000000; // Convert ms to ns

    // Calculate CPU usage as a percentage
    float cpu_usage = ((float)active_time_elapsed / (float)real_time_elapsed) * 100.0f;

    // Display the CPU usage
    //printf("CPU Usage: %.2f%%\n", cpu_usage);

    // Update the last times
    last_active_time = current_active_time;
    last_real_time = current_real_time;

    // Store the result in the profiler
    m->fstorage = cpu_usage;
}


void setup_measures(struct vmu_profiler *p) {
    vmu_profiler_measurement_t *fps_msr = init_measurement("FPS", use_float, fps_callback);
	vmu_profiler_measurement_t *cpu_msr = init_measurement("SH4", use_float, cpu_usage_callback);
	vmu_profiler_measurement_t *mem_msr = init_measurement("MEM", use_float, mem_callback);
    vmu_profiler_add_measure(p, fps_msr);
    vmu_profiler_add_measure(p, cpu_msr);
	vmu_profiler_add_measure(p, mem_msr);
}

static void* create_window_and_renderer(char* title, int x, int y, int width, int height) {
    // gdb_init();
    //dbgio_dev_select("fb");
    //SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    render_width = width;
    render_height = height;
    //SDL_SetHint(SDL_HINT_DC_VIDEO_MODE, "SDL_DC_TEXTURED_VIDEO");
    //SDL_SetHint(SDL_HINT_DC_VIDEO_MODE, "SDL_DC_DIRECT_VIDEO"); 
    SDL_SetHint(SDL_HINT_VIDEO_DOUBLE_BUFFER, "0");
    if (SDL_Init(SDL_INIT_VIDEO| SDL_INIT_AUDIO | SDL_INIT_JOYSTICK| SDL_INIT_GAMECONTROLLER) != 0) {
        LOG_PANIC("SDL_INIT_VIDEO error: %s", SDL_GetError());
    }

    // if(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
    //     LOG_WARN("SDL_INIT_GAMECONTROLLER error: %s", SDL_GetError());
    // }

    window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_FULLSCREEN_DESKTOP);
>>>>>>> origin/pvr

// 32 bytes, 1 bit per key. Matches dos executable behavior
static br_uint_32 key_state[8];

<<<<<<< HEAD
static struct {
    int x, y;
    float scale_x, scale_y;
} viewport;

// Callbacks back into original game code
extern void QuitGame(void);
extern br_pixelmap* gBack_screen;

static void set_key_from_scancode(SDL_Scancode scancode, int down);
#ifdef __DREAMCAST__
static SDL_Joystick* gController;
static void SDL2_Harness_HandleControllerEvent(const SDL_Event* event);
#endif

#ifdef DETHRACE_SDL_DYNAMIC
#ifdef _WIN32
static const char* const possible_locations[] = {
    "SDL2.dll",
};
#elif defined(__APPLE__)
#define SHARED_OBJECT_NAME "libSDL2"
#define SDL2_LIBNAME "libSDL2.dylib"
#define SDL2_FRAMEWORK "SDL2.framework/Versions/A/SDL2"
static const char* const possible_locations[] = {
    "@loader_path/" SDL2_LIBNAME,                     /* MyApp.app/Contents/MacOS/libSDL2_dylib */
    "@loader_path/../Frameworks/" SDL2_FRAMEWORK,     /* MyApp.app/Contents/Frameworks/SDL2_framework */
    "@executable_path/" SDL2_LIBNAME,                 /* MyApp.app/Contents/MacOS/libSDL2_dylib */
    "@executable_path/../Frameworks/" SDL2_FRAMEWORK, /* MyApp.app/Contents/Frameworks/SDL2_framework */
    NULL,                                             /* /Users/username/Library/Frameworks/SDL2_framework */
    "/Library/Frameworks" SDL2_FRAMEWORK,             /* /Library/Frameworks/SDL2_framework */
    SDL2_LIBNAME                                      /* oh well, anywhere the system can see the .dylib (/usr/local/lib or whatever) */
};
#else
#include "elfdlopennote.h"
#ifdef ELF_NOTE_DLOPEN
ELF_NOTE_DLOPEN(
    "SDL2",
    "Platform-specific operations such as creating windows and handling events",
    ELF_NOTE_DLOPEN_PRIORITY_SUGGESTED,
    "libSDL2-2.0.so.0",
    "libSDL2-2.0.so");
#endif
static const char* const possible_locations[] = {
    "libSDL2-2.0.so.0",
    "libSDL2-2.0.so",
};
#endif

static void* sdl2_so;
#endif

#define SDL_NAME "SDL2"
#define OBJECT_NAME sdl2_so
#define SYMBOL_PREFIX SDL2_
#define FOREACH_SDLX_SYM FOREACH_SDL2_SYM

#include "sdl_dyn_common.h"

static void calculate_viewport(int window_width, int window_height) {
    int vp_width, vp_height;
    float target_aspect_ratio;
    float aspect_ratio;

    aspect_ratio = (float)window_width / window_height;
    target_aspect_ratio = (float)gBack_screen->width / gBack_screen->height;

    vp_width = window_width;
    vp_height = window_height;
    if (aspect_ratio != target_aspect_ratio) {
        if (aspect_ratio > target_aspect_ratio) {
            vp_width = window_height * target_aspect_ratio + .5f;
        } else {
            vp_height = window_width / target_aspect_ratio + .5f;
=======
    if (harness_game_config.start_full_screen) {
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    
    //SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED); //SDL_RENDERER_PRESENTVSYNC
    if (renderer == NULL) {
        LOG_PANIC("Failed to create renderer: %s", SDL_GetError());
    }
    //printf("HERE\n");
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    //printf("HERE2\n");
    SDL_RenderSetLogicalSize(renderer, render_width, render_height);
    printf("Video res: width %d. height %d\n ", width, height);
    screen_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height); // 320x200
    //printf("HERE4\n");
    if (screen_texture == NULL) {
        SDL_RendererInfo info;
        SDL_GetRendererInfo(renderer, &info);
        for (Uint32 i = 0; i < info.num_texture_formats; i++) {
            LOG_INFO("%s\n", SDL_GetPixelFormatName(info.texture_formats[i]));
>>>>>>> origin/pvr
        }
    }
<<<<<<< HEAD
    viewport.x = (window_width - vp_width) / 2;
    viewport.y = (window_height - vp_height) / 2;
    viewport.scale_x = (float)vp_width / gBack_screen->width;
    viewport.scale_y = (float)vp_height / gBack_screen->height;
}

static int SDL2_Harness_SetWindowPos(void* hWnd, int x, int y, int nWidth, int nHeight) {
=======

    printf("Profiler init\n");
    vmu_profiler_start(0, setup_measures);

    return window;
}

static int set_window_pos(void* hWnd, int x, int y, int nWidth, int nHeight) {
// #ifndef __DREAMCAST__    
>>>>>>> origin/pvr
    // SDL_SetWindowPosition(hWnd, x, y);
    if (nWidth == 320 && nHeight == 200) {
        nWidth = 640;
        nHeight = 400;
    }
<<<<<<< HEAD
    SDL2_SetWindowSize(hWnd, nWidth, nHeight);
=======
    SDL_SetWindowSize(hWnd, nWidth, nHeight);
// #endif    
>>>>>>> origin/pvr
    return 0;
}

static void SDL2_Harness_DestroyWindow(void) {
    // SDL2_GL_DeleteContext(context);
    if (window != NULL) {
        SDL2_DestroyWindow(window);
    }
    SDL2_Quit();
    window = NULL;
}

// Checks whether the `flag_check` is the only modifier applied.
// e.g. is_only_modifier(event.key.keysym.mod, KMOD_ALT) returns true when only the ALT key was pressed
static int is_only_key_modifier(int modifier_flags, int flag_check) {
    return (modifier_flags & flag_check) && (modifier_flags & (KMOD_CTRL | KMOD_SHIFT | KMOD_ALT | KMOD_GUI)) == (modifier_flags & flag_check);
}

static void SDL2_Harness_ProcessWindowMessages(void) {
    SDL_Event event;

<<<<<<< HEAD
    while (SDL2_PollEvent(&event)) {
        switch (event.type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (event.key.windowID != SDL2_GetWindowID(window)) {
                continue;
            }
            if (event.key.keysym.sym == SDLK_RETURN) {
                if (event.key.type == SDL_KEYDOWN) {
                    if ((event.key.keysym.mod & (KMOD_CTRL | KMOD_SHIFT | KMOD_ALT | KMOD_GUI))) {
                        // Ignore keydown of RETURN when used together with some modifier
                        return;
                    }
                } else if (event.key.type == SDL_KEYUP) {
                    if (is_only_key_modifier(event.key.keysym.mod, KMOD_ALT)) {
                        SDL2_SetWindowFullscreen(window, (SDL2_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
=======
     while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                dinput_key = sdlScanCodeToDirectInputKeyNum[event.key.keysym.scancode];
                if (dinput_key != 0) {
                    directinput_key_state[dinput_key] = (event.type == SDL_KEYDOWN ? 0x80 : 0);
                }
                break;

            case SDL_CONTROLLERDEVICEADDED:
                SDL_GameControllerOpen(event.cdevice.which);
                break;

            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
                dinput_key = sdlGamepadToDirectInputKeyNum.buttonMapping[event.cbutton.button];
                if (dinput_key != 0) {
                    directinput_key_state[dinput_key] = (event.type == SDL_CONTROLLERBUTTONDOWN ? 0x80 : 0);
                }
                break;

            case SDL_CONTROLLERAXISMOTION:
                if (event.caxis.value > 16000) {  // Axis positive
                    dinput_key = sdlGamepadToDirectInputKeyNum.axisPositive[event.caxis.axis];
                    if (dinput_key != 0) {
                        directinput_key_state[dinput_key] = 0x80;
                    }
                } else if (event.caxis.value < -16000) {  // Axis negative
                    dinput_key = sdlGamepadToDirectInputKeyNum.axisNegative[event.caxis.axis];
                    if (dinput_key != 0) {
                        directinput_key_state[dinput_key] = 0x80;
                    }
                } else {  // Reset when neutral
                    directinput_key_state[sdlGamepadToDirectInputKeyNum.axisPositive[event.caxis.axis]] = 0x00;
                    directinput_key_state[sdlGamepadToDirectInputKeyNum.axisNegative[event.caxis.axis]] = 0x00;
                }
                break;

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                    if (SDL_GetWindowID(window) == event.window.windowID) {
                        msg->message = WM_QUIT;
                        return 1;
>>>>>>> origin/pvr
                    }
                }
                break;

<<<<<<< HEAD
            // Map incoming SDL scancode to PC scan code as used by game code
            if (sdl_scancode_map[event.key.keysym.scancode] == 0) {
                LOG_WARN3("unexpected scan code %s (%d)", SDL2_GetScancodeName(event.key.keysym.scancode), event.key.keysym.scancode);
                return;
            }
            set_key_from_scancode(event.key.keysym.scancode, event.type == SDL_KEYDOWN);
            break;

#ifdef __DREAMCAST__
        case SDL_JOYHATMOTION:
        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP:
        case SDL_JOYAXISMOTION:
            SDL2_Harness_HandleControllerEvent(&event);
            break;
#endif

        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                calculate_viewport(event.window.data1, event.window.data2);
            }
            break;

        case SDL_QUIT:
            QuitGame();
=======
            case SDL_QUIT:
                msg->message = WM_QUIT;
                return 1;
>>>>>>> origin/pvr
        }
    }
}

// Apply a key state change expressed as an SDL scancode, translating it to the
// PC scancode the game expects. Shared by the keyboard handler and (on the
// Dreamcast) the controller-to-keyboard mapping.
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

#ifdef __DREAMCAST__
// Map the Dreamcast controller to the keyboard the game expects. The d-pad and
// the analog stick both drive the arrow keys, so they work for menu navigation
// and for steering and acceleration in game. Button indices follow the
// KallistiOS SDL2 joystick driver; adjust the cases below if a pad reports
// differently.
#define DC_AXIS_DEADZONE 16000

static void set_hat_direction(int active, int was_active, SDL_Scancode scancode) {
    if (active != was_active) {
        set_key_from_scancode(scancode, active);
    }
}

static void SDL2_Harness_HandleControllerEvent(const SDL_Event* event) {
    static int prev_hat = SDL_HAT_CENTERED;
    static int axis_neg[8];
    static int axis_pos[8];

    switch (event->type) {
    case SDL_JOYHATMOTION: {
        int hat = event->jhat.value;
        set_hat_direction(hat & SDL_HAT_UP, prev_hat & SDL_HAT_UP, SDL_SCANCODE_UP);
        set_hat_direction(hat & SDL_HAT_DOWN, prev_hat & SDL_HAT_DOWN, SDL_SCANCODE_DOWN);
        set_hat_direction(hat & SDL_HAT_LEFT, prev_hat & SDL_HAT_LEFT, SDL_SCANCODE_LEFT);
        set_hat_direction(hat & SDL_HAT_RIGHT, prev_hat & SDL_HAT_RIGHT, SDL_SCANCODE_RIGHT);
        prev_hat = hat;
        break;
    }
    case SDL_JOYBUTTONDOWN:
    case SDL_JOYBUTTONUP: {
        int down = (event->type == SDL_JOYBUTTONDOWN);
        SDL_Scancode sc;
        switch (event->jbutton.button) {
        case 0: sc = SDL_SCANCODE_RETURN; break; // A: select
        case 1: sc = SDL_SCANCODE_ESCAPE; break; // B: back
        case 2: sc = SDL_SCANCODE_SPACE; break;  // X: handbrake
        case 3: sc = SDL_SCANCODE_TAB; break;    // Y
        default: sc = SDL_SCANCODE_RETURN; break; // Start and others: select
        }
        set_key_from_scancode(sc, down);
        break;
    }
    case SDL_JOYAXISMOTION: {
        int axis = event->jaxis.axis;
        if (axis < 0 || axis >= 8) {
            break;
        }
        SDL_Scancode neg, pos;
        if (axis == 0) {
            neg = SDL_SCANCODE_LEFT;
            pos = SDL_SCANCODE_RIGHT;
        } else if (axis == 1) {
            neg = SDL_SCANCODE_UP;
            pos = SDL_SCANCODE_DOWN;
        } else {
            break;
        }
        int want_neg = event->jaxis.value < -DC_AXIS_DEADZONE;
        int want_pos = event->jaxis.value > DC_AXIS_DEADZONE;
        if (want_neg != axis_neg[axis]) {
            set_key_from_scancode(neg, want_neg);
            axis_neg[axis] = want_neg;
        }
        if (want_pos != axis_pos[axis]) {
            set_key_from_scancode(pos, want_pos);
            axis_pos[axis] = want_pos;
        }
        break;
    }
    }
}
#endif

static void SDL2_Harness_SetKeyHandler(void (*handler_func)(void)) {
    gKeyHandler_func = handler_func;
}

static void SDL2_Harness_GetKeyboardState(br_uint_32* buffer) {
    memcpy(buffer, key_state, sizeof(key_state));
}

static int SDL2_Harness_GetMouseButtons(int* pButton1, int* pButton2) {
    if (SDL2_GetMouseFocus() != window) {
        *pButton1 = 0;
        *pButton2 = 0;
        return 0;
    }
    int state = SDL2_GetMouseState(NULL, NULL);
    *pButton1 = state & SDL_BUTTON_LMASK;
    *pButton2 = state & SDL_BUTTON_RMASK;
    return 0;
}

static int SDL2_Harness_GetMousePosition(int* pX, int* pY) {
    int window_width, window_height;
    float lX, lY;

    if (SDL2_GetMouseFocus() != window) {
        return 0;
    }
    SDL2_GetWindowSize(window, &window_width, &window_height);

    SDL2_GetMouseState(pX, pY);
    if (renderer != NULL) {
        // software renderer
        SDL2_RenderWindowToLogical(renderer, *pX, *pY, &lX, &lY);
    } else {
        // hardware renderer
        // handle case where window is stretched larger than the pixel size
        lX = *pX * (640.0f / window_width);
        lY = *pY * (480.0f / window_height);
    }
    *pX = (int)lX;
    *pY = (int)lY;
    return 0;
}

static void limit_fps(void) {
    Uint32 now = SDL2_GetTicks();
    if (last_frame_time != 0) {
        unsigned int frame_time = now - last_frame_time;
        last_frame_time = now;
        if (frame_time < 100) {
            int sleep_time = (1000 / harness_game_config.fps) - frame_time;
            if (sleep_time > 5) {
                gHarness_platform.Sleep(sleep_time);
            }
        }
    }
    last_frame_time = SDL2_GetTicks();
}

static int SDL2_Harness_ShowErrorMessage(char* title, char* message) {
    fprintf(stderr, "%s", message);
    SDL2_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, window);
    return 0;
}

static void SDL2_Harness_CreateWindow(const char* title, int width, int height, tHarness_window_type window_type) {
    int window_width, window_height;
    Uint32 extra_window_flags;

    render_width = width;
    render_height = height;

    window_width = width;
    window_height = height;

    // special case lores and make a bigger window
    if (width == 320 && height == 200) {
        window_width = 640;
        window_height = 480;
    }
<<<<<<< HEAD
=======
    SDL_UnlockTexture(screen_texture);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, screen_texture, NULL, NULL);
    //SDL_RenderCopyEx(renderer, screen_texture, NULL, NULL, 0, NULL, SDL_FLIP_VERTICAL | SDL_FLIP_HORIZONTAL);
    SDL_RenderPresent(renderer);
>>>>>>> origin/pvr

    if (SDL2_Init(SDL_INIT_VIDEO) != 0) {
        LOG_PANIC2("SDL_INIT_VIDEO error: %s", SDL2_GetError());
    }

#ifdef __DREAMCAST__
    // The Dreamcast has no keyboard by default, so open the controller and feed
    // its input through the keyboard mapping above.
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0 && SDL_NumJoysticks() > 0) {
        gController = SDL_JoystickOpen(0);
        SDL_JoystickEventState(SDL_ENABLE);
    }
#endif

    extra_window_flags = SDL_WINDOW_RESIZABLE;
    if (harness_game_config.start_full_screen) {
        extra_window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    if (window_type == eWindow_type_opengl) {

        window = SDL2_CreateWindow(title,
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            window_width, window_height,
            extra_window_flags | SDL_WINDOW_OPENGL);

        if (window == NULL) {
            LOG_PANIC2("Failed to create window: %s", SDL2_GetError());
        }

        SDL2_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL2_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL2_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        gl_context = SDL2_GL_CreateContext(window);

        if (gl_context == NULL) {
            LOG_WARN2("Failed to create OpenGL core profile: %s. Trying OpenGLES...", SDL2_GetError());
            SDL2_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
            SDL2_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL2_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
            gl_context = SDL2_GL_CreateContext(window);
        }
        if (gl_context == NULL) {
            LOG_PANIC2("Failed to create OpenGL context: %s", SDL2_GetError());
        }
        SDL2_GL_SetSwapInterval(1);

    } else {
        window = SDL2_CreateWindow(title,
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            window_width, window_height,
            extra_window_flags);
        if (window == NULL) {
            LOG_PANIC2("Failed to create window: %s", SDL2_GetError());
        }

        renderer = SDL2_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
        if (renderer == NULL) {
            LOG_PANIC2("Failed to create renderer: %s", SDL2_GetError());
        }
        SDL2_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL2_RenderSetLogicalSize(renderer, render_width, render_height);

        screen_texture = SDL2_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        if (screen_texture == NULL) {
            SDL_RendererInfo info;
            SDL2_GetRendererInfo(renderer, &info);
            for (Uint32 i = 0; i < info.num_texture_formats; i++) {
                LOG_INFO2("%s\n", SDL2_GetPixelFormatName(info.texture_formats[i]));
            }
            LOG_PANIC2("Failed to create screen_texture: %s", SDL2_GetError());
        }
    }

    SDL2_ShowCursor(SDL_DISABLE);

    viewport.x = 0;
    viewport.y = 0;
    viewport.scale_x = 1;
    viewport.scale_y = 1;
}

static void SDL2_Harness_Swap(br_pixelmap* back_buffer) {
    int i;
    int dest_pitch;
    uint8_t* src_pixels;
    br_uint_32* dest_pixels;

    SDL2_Harness_ProcessWindowMessages();

    if (gl_context != NULL) {
        SDL2_GL_SwapWindow(window);
    } else {
        src_pixels = back_buffer->pixels;

        SDL2_LockTexture(screen_texture, NULL, (void**)&dest_pixels, &dest_pitch);
        for (i = 0; i < back_buffer->height * back_buffer->width; i++) {
            *dest_pixels = converted_palette[*src_pixels];
            dest_pixels++;
            src_pixels++;
        }
        SDL2_UnlockTexture(screen_texture);
        SDL2_RenderClear(renderer);
        SDL2_RenderCopy(renderer, screen_texture, NULL, NULL);
        SDL2_RenderPresent(renderer);
        last_screen_src = back_buffer;
    }

    // Update every frame
	vmu_profiler_update();

    if (harness_game_config.fps != 0) {
        limit_fps();
    }
}

<<<<<<< HEAD
static void SDL2_Harness_PaletteChanged(br_colour entries[256]) {
    int i;
    for (i = 0; i < 256; i++) {
        converted_palette[i] = (0xffu << 24 | BR_RED(entries[i]) << 16 | BR_GRN(entries[i]) << 8 | BR_BLU(entries[i]));
=======
static void set_palette(PALETTEENTRY_* pal) {
    for (int i = 0; i < 256; i++) {
        converted_palette[i] = (0xff << 24 | pal[i].peBlue << 16 | pal[i].peGreen << 8 | pal[i].peRed);
>>>>>>> origin/pvr
    }
    if (last_screen_src != NULL) {
        SDL2_Harness_Swap(last_screen_src);
    }
}

static void SDL2_Harness_GetViewport(int* x, int* y, float* width_multipler, float* height_multiplier) {
    *x = viewport.x;
    *y = viewport.y;
    *width_multipler = viewport.scale_x;
    *height_multiplier = viewport.scale_y;
}

static int SDL2_Harness_Platform_Init(tHarness_platform* platform) {
    if (SDL2_LoadSymbols() != 0) {
        return 1;
    }
    platform->ProcessWindowMessages = SDL2_Harness_ProcessWindowMessages;
    platform->Sleep = SDL2_Delay;
    platform->GetTicks = SDL2_GetTicks;
    platform->ShowCursor = SDL2_ShowCursor;
    platform->SetWindowPos = SDL2_Harness_SetWindowPos;
    platform->DestroyWindow = SDL2_Harness_DestroyWindow;
    platform->SetKeyHandler = SDL2_Harness_SetKeyHandler;
    platform->GetKeyboardState = SDL2_Harness_GetKeyboardState;
    platform->GetMousePosition = SDL2_Harness_GetMousePosition;
    platform->GetMouseButtons = SDL2_Harness_GetMouseButtons;
    platform->ShowErrorMessage = SDL2_Harness_ShowErrorMessage;

    platform->CreateWindow_ = SDL2_Harness_CreateWindow;
    platform->Swap = SDL2_Harness_Swap;
    platform->PaletteChanged = SDL2_Harness_PaletteChanged;
    platform->GL_GetProcAddress = SDL2_GL_GetProcAddress;
    platform->GetViewport = SDL2_Harness_GetViewport;
    return 0;
};

const tPlatform_bootstrap SDL2_bootstrap = {
    "sdl2",
    "SDL2 video backend (libsdl.org)",
    ePlatform_cap_software | ePlatform_cap_opengl,
    SDL2_Harness_Platform_Init,
};
