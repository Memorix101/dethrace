#include <SDL.h>

#include "harness.h"
#include "harness/config.h"
#include "harness/hooks.h"
#include "harness/trace.h"
#include "sdl2_scancode_to_dinput.h"
#include "sdl2_gamepad_to_dinput.h"

SDL_Window* window;
SDL_Renderer* renderer;
SDL_Texture* screen_texture;
uint32_t converted_palette[256];
br_pixelmap* last_screen_src;

SDL_GLContext* gl_context;

int render_width, render_height;

Uint32 last_frame_time;

uint8_t directinput_key_state[SDL_NUM_SCANCODES];

#ifdef __DREAMCAST__
#include <kos.h>
#include <stdatomic.h>
#include "../vmu_profiler.h"
#include <stdio.h>
#include <stdint.h>
#include <kos/init.h>
#include <arch/arch.h>
//#include "memf.h"


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

#endif

// Callbacks back into original game code
extern void QuitGame(void);
extern uint32_t gKeyboard_bits[8];

static int set_window_pos(void* hWnd, int x, int y, int nWidth, int nHeight) {
    // SDL_SetWindowPosition(hWnd, x, y);
    if (nWidth == 320 && nHeight == 200) {
        nWidth = 640;
        nHeight = 400;
    }
    SDL_SetWindowSize(hWnd, nWidth, nHeight);
    return 0;
}

static void destroy_window(void* hWnd) {
    // SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    window = NULL;
}

// Checks whether the `flag_check` is the only modifier applied.
// e.g. is_only_modifier(event.key.keysym.mod, KMOD_ALT) returns true when only the ALT key was pressed
static int is_only_key_modifier(int modifier_flags, int flag_check) {
    return (modifier_flags & flag_check) && (modifier_flags & (KMOD_CTRL | KMOD_SHIFT | KMOD_ALT | KMOD_GUI)) == (modifier_flags & flag_check);
}

#ifdef __DREAMCAST__
    maple_device_t *cont;
    cont_state_t *state;


void simulateKeyPress(SDL_Keycode key) {
    SDL_Event events[2];

    // Get a valid scancode from the key
    SDL_Scancode scancode = SDL_GetScancodeFromKey(key);
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        printf("Warning: SDL_GetScancodeFromKey(%d) returned unknown!\n", key);
        return; // Don't inject invalid events
    }

    // Simulate Key Down
    events[0].type = SDL_KEYDOWN;
    events[0].key.keysym.sym = key;
    events[0].key.keysym.scancode = scancode;
    events[0].key.state = SDL_PRESSED;

    // Simulate Key Up
    events[1].type = SDL_KEYUP;
    events[1].key.keysym.sym = key;
    events[1].key.keysym.scancode = scancode;
    events[1].key.state = SDL_RELEASED;

    // Push both events into the event queue
    SDL_PeepEvents(events, 2, SDL_ADDEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT);
}


void checkDreamcastController() {
    maple_device_t *cont;
    cont_state_t *state;

    cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (cont) {
        state = (cont_state_t *)maple_dev_status(cont);
        if (state) {
            if (state->buttons & CONT_A) {
                printf("Dreamcast: 'A' button pressed -> Simulating Enter Key (SDLK_RETURN)\n");
                simulateKeyPress(SDLK_RETURN);
            }
            if (state->buttons & CONT_B) {
                printf("Dreamcast: 'B' button pressed -> Simulating Escape Key (SDLK_ESCAPE)\n");
                simulateKeyPress(SDLK_ESCAPE);
            }
        }
    }
}
#endif

SDL_GameController *controller = NULL;
SDL_GameController *findController()
{
	for (int i = 0; i < SDL_NumJoysticks(); i++)
	{
		if (SDL_IsGameController(i))
		{
			return SDL_GameControllerOpen(i);
		}
	}

	return NULL;
}
    
static int get_and_handle_message(MSG_* msg) {
    SDL_Event event;
    int dinput_key;

    #ifdef __DREAMCAST__
    findController();
        //checkDreamcastController();
    #endif

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (event.key.windowID != SDL_GetWindowID(window)) {
                continue;
            }
            /*if (event.key.keysym.sym == SDLK_RETURN) {
                if (event.key.type == SDL_KEYDOWN) {
                    if ((event.key.keysym.mod & (KMOD_CTRL | KMOD_SHIFT | KMOD_ALT | KMOD_GUI))) {
                        // Ignore keydown of RETURN when used together with some modifier
                        return 0;
                    }
                } else if (event.key.type == SDL_KEYUP) {
                    if (is_only_key_modifier(event.key.keysym.mod, KMOD_ALT)) {
                        SDL_SetWindowFullscreen(window, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    }
                }
            }*/

            printf("Key pressed: %s\n", SDL_GetKeyName(event.key.keysym.sym));

            // Map incoming SDL scancode to DirectInput DIK_* key code.
            // https://github.com/DanielGibson/Snippets/blob/master/sdl2_scancode_to_dinput.h
            dinput_key = sdlScanCodeToDirectInputKeyNum[event.key.keysym.scancode];
            if (dinput_key == 0) {
                //LOG_WARN("unexpected scan code %s (%d)", SDL_GetScancodeName(event.key.keysym.scancode), event.key.keysym.scancode);
                LOG_WARN("Unexpected scan code: %d (Key: %s)\n",
           event.key.keysym.scancode,
           SDL_GetScancodeName(event.key.keysym.scancode));
                return 0;
            }
            // DInput expects high bit to be set if key is down
            // https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418261(v=vs.85)
            directinput_key_state[dinput_key] = (event.type == SDL_KEYDOWN ? 0x80 : 0);
            if (event.type == SDL_KEYDOWN) {
                gKeyboard_bits[dinput_key >> 5] |= (1 << (dinput_key & 0x1F));
            } else {
                gKeyboard_bits[dinput_key >> 5] &= ~(1 << (dinput_key & 0x1F));
            }
            break;

            case SDL_CONTROLLERDEVICEADDED:
				if (!controller)
				{   //SDL_Log("Added controller\n");
                    //SDL_GameControllerOpen(event.cdevice.which);
					SDL_GameControllerOpen(event.cdevice.which);
				}
				break;
			case SDL_CONTROLLERDEVICEREMOVED:
				if (controller && event.cdevice.which == SDL_JoystickInstanceID(
														 SDL_GameControllerGetJoystick(controller)))
				{
					SDL_GameControllerClose(controller);
					findController();
				}
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
                }
            }
            break;

        case SDL_QUIT:
            // msg->message = WM_QUIT;
            QuitGame();
            return 1;
        }
    }
    return 0;
}

static void get_keyboard_state(unsigned int count, uint8_t* buffer) {
    memcpy(buffer, directinput_key_state, count);
}

static int get_mouse_buttons(int* pButton1, int* pButton2) {
    if (SDL_GetMouseFocus() != window) {
        *pButton1 = 0;
        *pButton2 = 0;
        return 0;
    }
    int state = SDL_GetMouseState(NULL, NULL);
    *pButton1 = state & SDL_BUTTON_LMASK;
    *pButton2 = state & SDL_BUTTON_RMASK;
    return 0;
}

static int get_mouse_position(int* pX, int* pY) {
    float lX, lY;
    if (SDL_GetMouseFocus() != window) {
        return 0;
    }
    SDL_GetMouseState(pX, pY);
    if (renderer != NULL) {
        SDL_RenderWindowToLogical(renderer, *pX, *pY, &lX, &lY);
    } else {
        lX = *pX;
        lY = *pY;
    }

#if defined(DETHRACE_FIX_BUGS)
    // In hires mode (640x480), the menus are still rendered at (320x240),
    // so prescale the cursor coordinates accordingly.
    lX *= 320;
    lX /= render_width;
    lY *= 200;
    lY /= render_height;
#endif
    *pX = (int)lX;
    *pY = (int)lY;
    return 0;
}

static void limit_fps(void) {
    Uint32 now = SDL_GetTicks();
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
    last_frame_time = SDL_GetTicks();
}

int show_error_message(void* window, char* text, char* caption) {
    fprintf(stderr, "%s", text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, caption, text, window);
    return 0;
}

static void create_window(char* title, int width, int height, tHarness_window_type window_type) {
    //dbgio_dev_select("fb");

    render_width = width;
    render_height = height;

    if (SDL_Init(SDL_INIT_VIDEO| SDL_INIT_AUDIO | SDL_INIT_JOYSTICK| SDL_INIT_GAMECONTROLLER) != 0) {
        LOG_PANIC("SDL_INIT_VIDEO error: %s", SDL_GetError());
    }

    if (window_type == eWindow_type_opengl) {

        window = SDL_CreateWindow(title,
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            width == 320 ? 640 : width, height == 200 ? 480 : 480,
            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);

        if (window == NULL) {
            LOG_PANIC("Failed to create window: %s", SDL_GetError());
        }

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        gl_context = SDL_GL_CreateContext(window);

        if (gl_context == NULL) {
            LOG_WARN("Failed to create OpenGL core profile: %s. Trying OpenGLES...", SDL_GetError());
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
            gl_context = SDL_GL_CreateContext(window);
        }
        if (gl_context == NULL) {
            LOG_PANIC("Failed to create OpenGL context: %s", SDL_GetError());
        }
        SDL_GL_SetSwapInterval(1);

    } else {
        window = SDL_CreateWindow(title,
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            width == 320 ? 640 : width, height == 200 ? 480 : 480,
            SDL_WINDOW_RESIZABLE);
        if (window == NULL) {
            LOG_PANIC("Failed to create window: %s", SDL_GetError());
        }

        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
        if (renderer == NULL) {
            LOG_PANIC("Failed to create renderer: %s", SDL_GetError());
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_RenderSetLogicalSize(renderer, render_width, render_height);

        screen_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        if (screen_texture == NULL) {
            SDL_RendererInfo info;
            SDL_GetRendererInfo(renderer, &info);
            for (Uint32 i = 0; i < info.num_texture_formats; i++) {
                LOG_INFO("%s\n", SDL_GetPixelFormatName(info.texture_formats[i]));
            }
            LOG_PANIC("Failed to create screen_texture: %s", SDL_GetError());
        }
    }

    if (harness_game_config.start_full_screen) {
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }

    #ifdef __DREAMCAST__
        printf("Profiler init\n");
        vmu_profiler_start(0, setup_measures);
    #endif
}

static void swap(br_pixelmap* back_buffer) {
    uint8_t* src_pixels = back_buffer->pixels;
    uint32_t* dest_pixels;
    int dest_pitch;

    get_and_handle_message(NULL);

    if (gl_context != NULL) {
        SDL_GL_SwapWindow(window);
    } else {
        SDL_LockTexture(screen_texture, NULL, (void**)&dest_pixels, &dest_pitch);
        for (int i = 0; i < back_buffer->height * back_buffer->width; i++) {
            *dest_pixels = converted_palette[*src_pixels];
            dest_pixels++;
            src_pixels++;
        }
        SDL_UnlockTexture(screen_texture);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, screen_texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        last_screen_src = back_buffer;
    }

    #ifdef __DREAMCAST__
        // Update every frame
	    vmu_profiler_update();
    #endif

    if (harness_game_config.fps != 0) {
        limit_fps();
    }
}

static void palette_changed(br_colour entries[256]) {
    for (int i = 0; i < 256; i++) {
        converted_palette[i] = (0xff << 24 | BR_RED(entries[i]) << 16 | BR_GRN(entries[i]) << 8 | BR_BLU(entries[i]));
    }
    if (last_screen_src != NULL) {
        swap(last_screen_src);
    }
}

static void get_viewport(int* x, int* y, int* width, int* height) {
    int window_width, window_height;
    int vp_width, vp_height;
    SDL_GetWindowSize(window, &window_width, &window_height);

    const float target_aspect_ratio = 4.0f / 3.0f;
    const float aspect_ratio = (float)window_width / (float)window_height;

    vp_width = window_width;
    vp_height = window_height;
    if (aspect_ratio != target_aspect_ratio) {
        if (aspect_ratio > target_aspect_ratio) {
            vp_width = window_height * target_aspect_ratio + .5f;
        } else {
            vp_height = window_width / target_aspect_ratio + .5f;
        }
    }
    *x = (window_width - vp_width) / 2;
    *y = (window_height - vp_height) / 2;
    *width = vp_width;
    *height = vp_height;
}

void Harness_Platform_Init(tHarness_platform* platform) {
    platform->ProcessWindowMessages = get_and_handle_message;
    platform->Sleep = SDL_Delay;
    platform->GetTicks = SDL_GetTicks;
    platform->ShowCursor = SDL_ShowCursor;
    platform->SetWindowPos = set_window_pos;
    platform->DestroyWindow = destroy_window;
    platform->GetKeyboardState = get_keyboard_state;
    platform->GetMousePosition = get_mouse_position;
    platform->GetMouseButtons = get_mouse_buttons;
    platform->ShowErrorMessage = show_error_message;

    platform->CreateWindow_ = create_window;
    platform->Swap = swap;
    platform->PaletteChanged = palette_changed;
    platform->GL_GetProcAddress = SDL_GL_GetProcAddress;
    platform->GetViewport = get_viewport;
}
