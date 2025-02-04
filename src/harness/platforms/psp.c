#include <SDL2/SDL.h>

#include "harness.h"
#include "harness/config.h"
#include "harness/hooks.h"
#include "harness/trace.h"
#include "sdl2_scancode_to_dinput.h"
#include "sdl2_gamepad_to_dinput.h"

SDL_Window* window;
SDL_Renderer* renderer;
SDL_Texture* screen_texture;
uint32_t __attribute__((aligned(16))) converted_palette[256];
br_pixelmap* last_screen_src;
int render_width, render_height;

#include <memory.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <psppower.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <psprtc.h>


PSP_MODULE_INFO("DETHRACE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
//PSP_HEAP_THRESHOLD_SIZE_KB(1024);
PSP_MAIN_THREAD_STACK_SIZE_KB(2048);

Uint32 last_frame_time;
char list[0x20000] __attribute__((aligned(64)));
uint8_t directinput_key_state[SDL_NUM_SCANCODES];

static void* create_window_and_renderer(char* title, int x, int y, int width, int height) {
    render_width = 320;
    render_height = 200;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        LOG_PANIC("SDL_INIT_VIDEO error: %s", SDL_GetError());
    }

    if(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        LOG_WARN("SDL_INIT_GAMECONTROLLER error: %s", SDL_GetError());
    }

    window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        320, 200,
        SDL_WINDOW_RESIZABLE);

    if (window == NULL) {
        LOG_PANIC("Failed to create window: %s", SDL_GetError());
    }

    if (harness_game_config.start_full_screen) {
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (renderer == NULL) {
        LOG_PANIC("Failed to create renderer: %s", SDL_GetError());
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    //SDL_RenderSetLogicalSize(renderer, render_width, render_height);

    screen_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 200);
    if (screen_texture == NULL) {
        SDL_RendererInfo info;
        SDL_GetRendererInfo(renderer, &info);
        for (Uint32 i = 0; i < info.num_texture_formats; i++) {
            LOG_INFO("%s\n", SDL_GetPixelFormatName(info.texture_formats[i]));
        }
        LOG_PANIC("Failed to create screen_texture: %s", SDL_GetError());
    }

    // https://pspdev.github.io/basic_programs.html
#define BUFFER_WIDTH 512
#define BUFFER_HEIGHT 272
#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT BUFFER_HEIGHT
	// setup GU
	scePowerSetClockFrequency(333, 333, 166);
	pspDebugScreenInit();
	// setupCallbacks();

	// Setup GU
	void *fbp0 = guGetStaticVramBuffer(BUFFER_WIDTH, SCREEN_HEIGHT, GU_PSM_8888);
	void *fbp1 = guGetStaticVramBuffer(BUFFER_WIDTH, SCREEN_HEIGHT, GU_PSM_8888);

	sceGuInit();

	// Set up buffers
	sceGuStart(GU_DIRECT, list);
	sceGuDrawBuffer(GU_PSM_8888, fbp0, BUFFER_WIDTH);
	sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, fbp1, BUFFER_WIDTH);

	// We do not care about the depth buffer in this example
	sceGuDepthBuffer(fbp0, 0);	 // Set depth buffer to a length of 0
	sceGuDisable(GU_DEPTH_TEST); // Disable depth testing

	// Set up viewport
	sceGuOffset(2048 - (SCREEN_WIDTH / 2), 2048 - (SCREEN_HEIGHT / 2));
	sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

	// Start a new frame and enable the display
	sceGuFinish();
	sceGuDisplay(GU_TRUE);

    return window;
}

static int set_window_pos(void* hWnd, int x, int y, int nWidth, int nHeight) {
    // SDL_SetWindowPosition(hWnd, x, y);
    if (nWidth == 320 && nHeight == 200) {
        nWidth = 320;
        nHeight = 200;
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

#define JOYSTICK_SENSITIVITY 16000
int mouse_x_dir = 0;
int mouse_y_dir = 0;
int mouse_x = 0;
int mouse_y = 0;

static int get_and_handle_message(MSG_* msg) {
    SDL_Event event;
    int dinput_key;

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (event.key.windowID != SDL_GetWindowID(window)) {
                continue;
            }
            if (event.key.keysym.sym == SDLK_RETURN) {
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
            }

            // Map incoming SDL scancode to DirectInput DIK_* key code.
            // https://github.com/DanielGibson/Snippets/blob/master/sdl2_scancode_to_dinput.h
            dinput_key = sdlScanCodeToDirectInputKeyNum[event.key.keysym.scancode];
            if (dinput_key == 0) {
                LOG_WARN("unexpected scan code %s (%d)", SDL_GetScancodeName(event.key.keysym.scancode), event.key.keysym.scancode);
                return 0;
            }
            // DInput expects high bit to be set if key is down
            // https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418261(v=vs.85)
            directinput_key_state[dinput_key] = (event.type == SDL_KEYDOWN ? 0x80 : 0);
            break;

        case SDL_CONTROLLERDEVICEADDED:
            SDL_GameControllerOpen(event.cdevice.which);
            break;

        // TODO: case for removed device

        case SDL_CONTROLLERBUTTONDOWN:
            // code duplication bwoomp
            dinput_key = sdlGamepadToDirectInputKeyNum[event.cbutton.button];
            directinput_key_state[dinput_key] = 0x80;
            break;

        case SDL_CONTROLLERBUTTONUP:
            // code duplication bwoomp
            dinput_key = sdlGamepadToDirectInputKeyNum[event.cbutton.button];
            directinput_key_state[dinput_key] = 0x0;
            break;
        
        case SDL_JOYAXISMOTION:
            if (event.jaxis.axis == 0) {
                mouse_x_dir = event.jaxis.value / JOYSTICK_SENSITIVITY;
            }
            else if(event.jaxis.axis == 1) {
                mouse_y_dir = event.jaxis.value / JOYSTICK_SENSITIVITY;
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
            msg->message = WM_QUIT;
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

/*
static int get_mouse_position(int* pX, int* pY) {
    float lX, lY;
    if (SDL_GetMouseFocus() != window) {
        return 0;
    }
    SDL_GetMouseState(pX, pY);
    SDL_RenderWindowToLogical(renderer, *pX, *pY, &lX, &lY);

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
}*/

static int get_mouse_position(int *pX, int* pY)
{
    mouse_x += mouse_x_dir;
    mouse_y += mouse_y_dir;
    *pX = mouse_x;
    *pY = mouse_y;
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

static unsigned int __attribute__((aligned(16))) pixels[512 * 272];

static void present_screen(br_pixelmap* src) {
    // fastest way to convert 8 bit indexed to 32 bit
    uint8_t* src_pixels = src->pixels;
    // uint32_t* dest_pixels = 0;

    for (unsigned int x = 0; x < 512; x++) {
        for (unsigned int y = 0; y < 272; y++) {
            if (x < src->width && y < src->height) {
                int i = (y * 512) + x;
                uint8_t pixel = src_pixels[(y * src->width) + x];

                uint32_t c = converted_palette[pixel];
                pixels[i] = c;
            }
        }
    }

    sceKernelDcacheWritebackAll();

    // begin
sceGuStart(GU_DIRECT, list);

// clear
//sceGuClearColor(GU_RGBA(255, 255, 255, 255)); // white
//sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

// Set the buffer correctly
void* framebuffer = 0;
sceGuDrawBuffer(GU_PSM_8888, framebuffer, 512);

// Draw texture to framebuffer
sceGuCopyImage(GU_PSM_8888, 0, 0, 480, 272, 512, pixels, 0, 0, 512, (void*)(((unsigned int)framebuffer)+0x4000000));
sceGuTexSync();

// Finish rendering and wait for VBlank
sceGuFinish();
sceGuSync(0, 0);
sceDisplayWaitVblankStart();
sceDisplaySetFrameBuf(framebuffer, 512, GU_PSM_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);

// Swap buffers after waiting for VBlank
framebuffer = sceGuSwapBuffers();


    last_screen_src = src;

    SDL_Delay(16);

   /*if (harness_game_config.fps != 0) {
        limit_fps();
    }*/
}

static void set_palette(PALETTEENTRY_* pal) {
    for (int i = 0; i < 256; i++) {
        converted_palette[i] = (0xff << 24 | pal[i].peBlue << 16 | pal[i].peGreen << 8 | pal[i].peRed);
    }
    if (last_screen_src != NULL) {
        present_screen(last_screen_src);
    }
}

int show_error_message(void* window, char* text, char* caption) {
    fprintf(stderr, "%s", text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, caption, text, window);
    return 0;
}

void Harness_Platform_Init(tHarness_platform* platform) {
    platform->ProcessWindowMessages = get_and_handle_message;
    platform->Sleep = SDL_Delay;
    platform->GetTicks = SDL_GetTicks;
    platform->CreateWindowAndRenderer = create_window_and_renderer;
    platform->ShowCursor = SDL_ShowCursor;
    platform->SetWindowPos = set_window_pos;
    platform->DestroyWindow = destroy_window;
    platform->GetKeyboardState = get_keyboard_state;
    platform->GetMousePosition = get_mouse_position;
    platform->GetMouseButtons = get_mouse_buttons;
    platform->ShowErrorMessage = show_error_message;
    platform->Renderer_SetPalette = set_palette;
    platform->Renderer_Present = present_screen;
}
