/* pc_main.c - PC entry point: SDL2/GL init, crash protection, boot sequence */
#ifndef _WIN32
#define _GNU_SOURCE  /* needed for dladdr */
#endif
#include "pc_platform.h"
#if defined(__linux__) && defined(__x86_64__)
#include <ucontext.h>  /* REG_RIP/REG_RSP for the fatal-fault report */
#endif
#include "pc_gx_internal.h"
#include "pc_texture_pack.h"
#include "pc_settings.h"
#include "pc_keybindings.h"
#include "pc_mouse.h"
#include "pc_assets.h"
#include "pc_disc.h"
#include "pc_typing.h"
#include "pc_pause_menu.h"
#include "pc_settings_menu.h"
#include "pc_discord.h"
#include "pc_profiler.h"
#include "pc_network_config.h"
#include "m_kankyo.h"
#ifdef NETCODE_ENABLED
#include "acnet/c_api.h"
extern int Net_ConfigureQuickstart(const char* name, int gender);
#endif

/* prefer discrete GPU on laptops */
#ifdef _WIN32
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

SDL_Window*   g_pc_window = NULL;
SDL_GLContext  g_pc_gl_context = NULL;
int           g_pc_running = 1;
int           g_pc_no_framelimit = 0;
int           g_pc_fast_forward = 0;
int           g_pc_verbose = 0;
int           g_pc_time_override = -1; /* -1=system clock, 0-23=override hour */
int           g_pc_min_override = -1; /* -1=system clock, 0-59=override minute */
int           g_pc_sec_override = -1; /* -1=system clock, 0-59=override second */
int           g_pc_date_month = -1; /* -1=system clock, 1-12=override month */
int           g_pc_date_day = -1; /* -1=system clock, 1-31=override day */
int           g_pc_date_year = -1; /* -1=system clock, else override year */
int           g_pc_weather_override = -1;
int           g_pc_weather_intensity_override = mEnv_WEATHER_INTENSITY_HEAVY;
int           g_pc_window_w = PC_SCREEN_WIDTH;
int           g_pc_window_h = PC_SCREEN_HEIGHT;
int           g_pc_widescreen_stretch = 0;
#ifdef NETCODE_ENABLED
static int      g_pc_online_enabled = 0;
static char     g_pc_online_host[256] = "127.0.0.1";
static int      g_pc_online_port = 24680;
static uint64_t g_pc_online_town = 1;
static uint64_t g_pc_online_account = 1;
static char     g_pc_online_invite_key[128] = "";
static const char* g_pc_online_quickstart_name = NULL;
static int      g_pc_online_quickstart_gender = 0;
#endif

/* exe image range — used by seg2k0 to distinguish pointers from segment addresses */
uintptr_t pc_image_base = 0;
uintptr_t pc_image_end  = 0;

static jmp_buf* pc_active_jmpbuf = NULL;
static volatile uintptr_t pc_last_crash_addr = 0;

static volatile uintptr_t pc_last_crash_data_addr = 0;

/* --- Fatal-fault reporting ---
 * The handlers below already ran for every fatal fault but only recorded an
 * address nothing ever printed, so a crash killed the process silently. The
 * report names the faulting instruction plus the return addresses stacked above
 * it, as offsets from the loaded image base. Map an offset back to a function
 * with the "nm address" column and an unstripped build:
 *   nm --numeric-sort AnimalCrossing.exe | less
 * Reporting never changes control flow — the jmp_buf path below is unaffected. */
#define PC_CRASH_MAX_FRAMES 48

static volatile int pc_crash_reported = 0;

static uintptr_t pc_crash_preferred_base(void) {
#ifdef _WIN32
    HMODULE exe = GetModuleHandle(NULL);
    if (exe != NULL) {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)exe;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((char*)exe + dos->e_lfanew);
        return (uintptr_t)nt->OptionalHeader.ImageBase;
    }
#endif
    return 0;
}

static void pc_crash_write(FILE* out, const char* label, unsigned long code, uintptr_t fault_pc,
                           uintptr_t data_addr, const uintptr_t* frames, int frame_count) {
    const uintptr_t base = pc_image_base;
    const uintptr_t end = pc_image_end;
    const uintptr_t preferred = pc_crash_preferred_base();
    int i;

    fprintf(out, "\n=== Animal Crossing fatal fault ===\n");
    fprintf(out, "exception  : %s (0x%08lx)\n", label, code);
    fprintf(out, "fault pc   : 0x%016llx\n", (unsigned long long)fault_pc);
    fprintf(out, "data addr  : 0x%016llx\n", (unsigned long long)data_addr);
    fprintf(out, "image base : 0x%016llx  end 0x%016llx  preferred 0x%016llx\n",
            (unsigned long long)base, (unsigned long long)end, (unsigned long long)preferred);
    if (base != 0 && fault_pc >= base && fault_pc < end) {
        fprintf(out, "fault rva  : 0x%llx  (nm address 0x%llx)\n",
                (unsigned long long)(fault_pc - base),
                (unsigned long long)(preferred + (fault_pc - base)));
    } else {
        fprintf(out, "fault rva  : outside the executable image (DLL or wild jump)\n");
    }
    fprintf(out, "return addresses inside the image, innermost first:\n");
    for (i = 0; i < frame_count; ++i) {
        if (base == 0 || frames[i] < base || frames[i] >= end) continue;
        fprintf(out, "  [%02d] rva 0x%-10llx nm address 0x%llx\n", i,
                (unsigned long long)(frames[i] - base),
                (unsigned long long)(preferred + (frames[i] - base)));
    }
    fprintf(out, "=== end fatal fault ===\n");
    fflush(out);
}

static void pc_crash_report(const char* label, unsigned long code, uintptr_t fault_pc,
                            uintptr_t data_addr, uintptr_t stack_pointer) {
    uintptr_t frames[PC_CRASH_MAX_FRAMES];
    int frame_count = 0;
    FILE* log;

    if (pc_crash_reported) return;
    pc_crash_reported = 1;

    /* Emit the essentials before touching anything that could fault again. */
    fprintf(stderr, "\n[FATAL] %s at pc 0x%016llx data 0x%016llx\n", label,
            (unsigned long long)fault_pc, (unsigned long long)data_addr);
    fflush(stderr);

    /* A raw scan of the stack for values pointing into the image. The decomp
     * translation units are built at -O0 so their frames are intact, and this
     * needs no unwind tables, which is what makes it survive a wild fault.
     * Only already-used stack (above the fault's own stack pointer) is read. */
    if (stack_pointer != 0 && pc_image_base != 0) {
        const uintptr_t* slot = (const uintptr_t*)(stack_pointer & ~(uintptr_t)(sizeof(uintptr_t) - 1));
        int scan_words = 2048;
        int scanned;
#ifdef _WIN32
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((LPCVOID)slot, &mbi, sizeof(mbi)) == sizeof(mbi)) {
                uintptr_t region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
                int available = (int)((region_end - (uintptr_t)slot) / sizeof(uintptr_t));
                if (available < scan_words) scan_words = available;
            } else {
                scan_words = 0;
            }
        }
#endif
        for (scanned = 0; scanned < scan_words && frame_count < PC_CRASH_MAX_FRAMES; ++scanned) {
            uintptr_t value = slot[scanned];
            if (value >= pc_image_base && value < pc_image_end) frames[frame_count++] = value;
        }
    }

    pc_crash_write(stderr, label, code, fault_pc, data_addr, frames, frame_count);
    log = fopen("crash.log", "w");
    if (log != NULL) {
        pc_crash_write(log, label, code, fault_pc, data_addr, frames, frame_count);
        fclose(log);
    }
}

#ifdef _WIN32
static const char* pc_crash_code_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        default: return "UNKNOWN";
    }
}
#endif

#ifdef _WIN32
/* longjmp from VEH is technically UB, but works on x86 MinGW (no SEH to corrupt).
 * GCC doesn't have __try/__except and checking every pointer in emu64 is impractical. */
static LONG WINAPI pc_veh_handler(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    int fatal = code == EXCEPTION_ACCESS_VIOLATION ||
                code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                code == EXCEPTION_PRIV_INSTRUCTION ||
                code == EXCEPTION_STACK_OVERFLOW ||
                code == EXCEPTION_IN_PAGE_ERROR;

    if (fatal) {
        pc_crash_report(pc_crash_code_name(code), (unsigned long)code,
                        (uintptr_t)ep->ExceptionRecord->ExceptionAddress,
                        code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR
                            ? (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1]
                            : 0,
                        (uintptr_t)ep->ContextRecord->Rsp);
    }

    if (pc_active_jmpbuf != NULL &&
        (code == EXCEPTION_ACCESS_VIOLATION ||
         code == EXCEPTION_ILLEGAL_INSTRUCTION ||
         code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
         code == EXCEPTION_PRIV_INSTRUCTION)) {
        pc_last_crash_addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        if (code == EXCEPTION_ACCESS_VIOLATION)
            pc_last_crash_data_addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        else
            pc_last_crash_data_addr = 0;
        jmp_buf* buf = pc_active_jmpbuf;
        pc_active_jmpbuf = NULL;
        longjmp(*buf, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
/* POSIX equivalent of VEH — longjmp from signal handler (POSIX-defined for program faults) */
static void pc_signal_handler(int sig, siginfo_t* info, void* ucontext) {
    uintptr_t fault_pc = 0;
    uintptr_t stack_pointer = 0;
#if defined(__linux__) && defined(__x86_64__)
    if (ucontext != NULL) {
        const ucontext_t* uc = (const ucontext_t*)ucontext;
        fault_pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
        stack_pointer = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
    }
#else
    (void)ucontext;
#endif
    pc_crash_report(sig == SIGSEGV ? "SIGSEGV" : (sig == SIGILL ? "SIGILL" : "SIGFPE"),
                    (unsigned long)sig, fault_pc, (uintptr_t)info->si_addr, stack_pointer);

    if (pc_active_jmpbuf != NULL) {
        pc_last_crash_addr = (uintptr_t)info->si_addr;
        pc_last_crash_data_addr = (sig == SIGSEGV) ?
            (uintptr_t)info->si_addr : 0;
        jmp_buf* buf = pc_active_jmpbuf;
        pc_active_jmpbuf = NULL;
        longjmp(*buf, 1);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

uintptr_t pc_crash_get_data_addr(void) {
    return pc_last_crash_data_addr;
}

void pc_crash_protection_init(void) {
    static int installed = 0;
    if (!installed) {
#ifdef _WIN32
        AddVectoredExceptionHandler(1, pc_veh_handler);
#else
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = pc_signal_handler;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
#endif
        installed = 1;
    }
}

void pc_crash_set_jmpbuf(jmp_buf* buf) {
    pc_active_jmpbuf = buf;
}

uintptr_t pc_crash_get_addr(void) {
    return pc_last_crash_addr;
}

void pc_platform_init(void) {
#ifdef _WIN32
    SetProcessDPIAware();
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        exit(1);
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
    /* macOS requires forward-compatible flag for Core Profile contexts */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
#ifdef PC_ENHANCEMENTS
    if (g_pc_settings.msaa > 0) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, g_pc_settings.msaa);
    }
#endif

    {
        Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
        int win_w = g_pc_settings.window_width;
        int win_h = g_pc_settings.window_height;
        if (g_pc_settings.fullscreen == 1) {
            flags |= SDL_WINDOW_FULLSCREEN;
        } else if (g_pc_settings.fullscreen == 2) {
            flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        }
        g_pc_window = SDL_CreateWindow(
            PC_WINDOW_TITLE,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            win_w, win_h, flags
        );
    }
    if (!g_pc_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        exit(1);
    }

    g_pc_gl_context = SDL_GL_CreateContext(g_pc_window);
    if (!g_pc_gl_context) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_pc_window);
        SDL_Quit();
        exit(1);
    }

    if (!gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress)) {
        fprintf(stderr, "gladLoadGL failed\n");
        SDL_GL_DeleteContext(g_pc_gl_context);
        SDL_DestroyWindow(g_pc_window);
        SDL_Quit();
        exit(1);
    }

    if (g_pc_verbose) {
        const char* vendor = (const char*)glGetString(GL_VENDOR);
        const char* renderer = (const char*)glGetString(GL_RENDERER);
        const char* version = (const char*)glGetString(GL_VERSION);
        const char* glsl = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
        printf("[GL] Vendor: %s\n", vendor ? vendor : "Unknown");
        printf("[GL] Renderer: %s\n", renderer ? renderer : "Unknown");
        printf("[GL] Version: %s\n", version ? version : "Unknown");
        printf("[GL] GLSL: %s\n", glsl ? glsl : "Unknown");
        const char* sdl_driver = SDL_GetCurrentVideoDriver();
        printf("[SDL] Video Driver: %s\n", sdl_driver ? sdl_driver : "Unknown");
    }

#ifndef _WIN32
    {
        const char* renderer = (const char*)glGetString(GL_RENDERER);
        if (renderer && (strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe"))) {
            const char* sdl_driver = SDL_GetCurrentVideoDriver();
            fprintf(stderr, "\n--- WARNING ---\n"
                            "Game is running on software renderer (llvmpipe/softpipe).\n"
                            "This usually means a hardware OpenGL driver was not loaded.\n");
            if (sdl_driver && strcmp(sdl_driver, "wayland") == 0) {
                fprintf(stderr, "On Wayland, verify EGL/Wayland support or try SDL_VIDEODRIVER=x11.\n");
            }
            fprintf(stderr, "----------------\n\n");
        }
    }
#endif

    SDL_GL_SetSwapInterval(g_pc_settings.vsync);

    pc_platform_update_window_size();

#ifdef PC_ENHANCEMENTS
    if (g_pc_settings.msaa > 0) {
        glEnable(GL_MULTISAMPLE);
    }
#endif

    pc_gx_init();
    pc_texture_pack_init();
#ifdef PC_ENHANCEMENTS
    if (g_pc_settings.preload_textures) {
        pc_texture_pack_preload_all();
    }
#endif
}

extern void PADCleanup(void);

static void pc_speedhack_toggle(void) {
    g_pc_fast_forward ^= 1;

    if (g_pc_verbose) {
        printf("[PC] fast-forward %s\n", g_pc_fast_forward ? "on (2x)" : "off");
    }
}

void pc_platform_shutdown(void) {
    pc_audio_shutdown();
    pc_audio_mq_shutdown();
    PADCleanup();
    pc_texture_pack_shutdown();
    pc_gx_shutdown();

    if (g_pc_gl_context) {
        SDL_GL_DeleteContext(g_pc_gl_context);
        g_pc_gl_context = NULL;
    }
    if (g_pc_window) {
        SDL_DestroyWindow(g_pc_window);
        g_pc_window = NULL;
    }
    SDL_Quit();
}

void pc_platform_update_window_size(void) {
    SDL_GL_GetDrawableSize(g_pc_window, &g_pc_window_w, &g_pc_window_h);
    if (g_pc_window_w <= 0) g_pc_window_w = PC_SCREEN_WIDTH;
    if (g_pc_window_h <= 0) g_pc_window_h = PC_SCREEN_HEIGHT;
}

void pc_platform_swap_buffers(void) {
    pc_gx_draw_pending();
    SDL_GL_SwapWindow(g_pc_window);
}

int pc_platform_poll_events(void) {
    SDL_Event event;

    pc_typing_update();
    pc_discord_update();

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                g_pc_running = 0;
                return 0;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    pc_platform_update_window_size();
                }
                break;
#ifdef MOUSE_INPUT
            case SDL_MOUSEWHEEL:
                g_mouse_wheel_delta += event.wheel.y;
                break;
#endif
            case SDL_KEYDOWN:
                /* Keybinding capture eats all input first (works from both
                 * the pause menu and the title Options menu). */
                if (pc_settings_menu_capture_active()) {
                    pc_settings_menu_handle_capture_event(&event);
                    break;
                }
                if (event.key.keysym.sym == SDLK_F3 && !event.key.repeat) {
                    pc_speedhack_toggle();
                    break;
                }
                if (event.key.keysym.sym == SDLK_F4 && !event.key.repeat) {
                    g_pc_fast_forward ^= 1;
                    printf("[PC] Fast forward %s (2x)\n", g_pc_fast_forward ? "ON" : "OFF");
                }
                if (event.key.keysym.sym == SDLK_ESCAPE && !event.key.repeat) {
                    if (g_pc_paused) {
                        pc_pause_menu_handle_event(&event);
                    } else {
                        pc_pause_menu_toggle();
                    }
                    break;
                }
                if (g_pc_paused) {
                    pc_pause_menu_handle_event(&event);
                    break; /* swallow all keys while paused */
                }
                pc_typing_handle_event(&event);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (pc_settings_menu_capture_active()) {
                    pc_settings_menu_handle_capture_event(&event);
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (pc_settings_menu_capture_active()) {
                    pc_settings_menu_handle_capture_event(&event);
                    break;
                }
                if (g_pc_paused) {
                    pc_pause_menu_handle_event(&event);
                    break;
                }
                /* Back/Select opens the pause menu (controller Esc). */
                if (event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                    pc_pause_menu_toggle();
                }
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (pc_settings_menu_capture_active()) {
                    pc_settings_menu_handle_capture_event(&event);
                    break;
                }
                if (g_pc_paused) {
                    pc_pause_menu_handle_event(&event);
                }
                break;
            case SDL_TEXTINPUT:
                if (g_pc_paused) break;
                pc_typing_handle_event(&event);
                break;
        }
    }

    pc_mouse_update();

    return 1;
}

/* game's main() renamed to ac_entry via -Dmain=ac_entry, boot.c's to boot_main */
extern void ac_entry(void);
extern int boot_main(int argc, const char** argv);

static int pc_parse_rain_intensity(const char* text) {
    if (strcmp(text, "light") == 0) {
        return mEnv_WEATHER_INTENSITY_LIGHT;
    }

    if (strcmp(text, "normal") == 0) {
        return mEnv_WEATHER_INTENSITY_NORMAL;
    }

    if (strcmp(text, "heavy") == 0) {
        return mEnv_WEATHER_INTENSITY_HEAVY;
    }

    return -1;
}

int main(int argc, char* argv[]) {
#ifdef NETCODE_ENABLED
    {
        pc_network_config_t network_config;
        const char* network_config_path = "network.ini";
        int explicit_network_config = 0;
        int network_config_found = 0;
        char network_error[256] = "";
        int arg_index;
        for (arg_index = 1; arg_index < argc; arg_index++) {
            if (strcmp(argv[arg_index], "--network-config") == 0 && arg_index + 1 < argc) {
                network_config_path = argv[++arg_index];
                explicit_network_config = 1;
            }
        }
        pc_network_config_defaults(&network_config);
        if (!pc_network_config_load(network_config_path, &network_config,
                                    &network_config_found, network_error, sizeof(network_error))) {
            fprintf(stderr, "Network configuration failed: %s\n", network_error);
            return 2;
        }
        if (!network_config_found) {
            if (explicit_network_config) {
                fprintf(stderr, "Network configuration does not exist: %s\n", network_config_path);
                return 2;
            }
            if (!pc_network_config_write_default(network_config_path, network_error, sizeof(network_error))) {
                fprintf(stderr, "Network configuration creation failed: %s\n", network_error);
                return 2;
            }
        }
        g_pc_online_enabled = network_config.enabled;
        memcpy(g_pc_online_host, network_config.host, sizeof(g_pc_online_host));
        g_pc_online_host[sizeof(g_pc_online_host) - 1] = '\0';
        g_pc_online_port = network_config.port;
        g_pc_online_town = network_config.town_id;
        g_pc_online_account = network_config.account_id;
        memcpy(g_pc_online_invite_key, network_config.invite_key, sizeof(g_pc_online_invite_key));
        g_pc_online_invite_key[sizeof(g_pc_online_invite_key) - 1] = '\0';
    }
#endif
#ifndef _WIN32
    /* prefer discrete GPU on Linux (NVIDIA PRIME and AMD) while respecting user overrides */
    setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 0);
    setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 0);
    setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 0);
    setenv("DRI_PRIME", "1", 0);

    const char* wayland_display = getenv("WAYLAND_DISPLAY");
    const char* x11_display = getenv("DISPLAY");

    const char* sdl_vid_drv = getenv("SDL_VIDEODRIVER");
    if (sdl_vid_drv != NULL && strcmp(sdl_vid_drv, "x11") == 0) {
        /* prefer GLX on X11 to prevent EGL fallback issues on some discrete drivers */
        setenv("SDL_VIDEO_GL_DRIVER", "libGL.so.1", 0);
    } else if (sdl_vid_drv == NULL && x11_display != NULL && wayland_display == NULL) {
        /* No driver set, and only X11 is available - safe to prefer GLX */
        setenv("SDL_VIDEO_GL_DRIVER", "libGL.so.1", 0);
    }
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: AnimalCrossing [options]\n");
            printf("  --verbose, -v       Enable diagnostic output\n");
            printf("  --no-framelimit     Alias for --framelimit 0 (uncapped)\n");
            printf("  --framelimit N      Set the target frame rate (default 60, 0 = uncapped)\n");
            printf("  --profile [N]       Print frame profiler summary every N frames (default 120)\n");
            printf("  --model-viewer [N]  Launch model viewer (optional start index)\n");
            printf("  --time H[:M[:S]]    Override in-game time (e.g. 5, 17:30, 5:55:00)\n");
            printf("  --date M/D[/Y]      Override in-game date (e.g. 7/4, 12/24/2026)\n");
            printf("  --rain [intensity]  Force rainy weather; intensity is light, normal, or heavy\n");
            printf("  --uber-shader       Disable shader specialization (single uber shader)\n");
#ifdef NETCODE_ENABLED
            printf("  --online HOST[:PORT] Connect to a dedicated town server\n");
            printf("  --town N             Dedicated-server town ID (default 1)\n");
            printf("  --account N          Local online account ID (default 1)\n");
            printf("  --invite-key KEY     Dedicated-town invitation key\n");
            printf("  --network-config F   Load connection settings from F (default network.ini)\n");
            printf("  --offline            Ignore an enabled network.ini for this launch\n");
            printf("  --quickstart NAME    Skip title/K.K. and prefill Rover's name prompt\n");
            printf("  --quickstart-gender G  Reserved launch default: male or female\n");
#endif
            printf("  --help, -h          Show this help message\n");
            return 0;
        } else if (strcmp(argv[i], "--framelimit") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                int v = atoi(argv[i + 1]);
                if (v > 0) {
                    g_pc_settings.max_fps = v;


                }
                i++;
            }
        } else if (strcmp(argv[i], "--no-framelimit") == 0) {
            g_pc_no_framelimit = 1;
        } else if (strcmp(argv[i], "--uber-shader") == 0) {
            extern int g_pc_uber_shader_only;
            g_pc_uber_shader_only = 1;
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            g_pc_verbose = 1;
        } else if (strcmp(argv[i], "--profile") == 0) {
            g_pc_profile_enabled = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                int interval = atoi(argv[i + 1]);
                if (interval > 0) g_pc_profile_interval = interval;
                i++;
            }
        } else if (strcmp(argv[i], "--model-viewer") == 0) {
            g_pc_model_viewer = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                g_pc_model_viewer_start = atoi(argv[i + 1]);
                i++;
            }
        } else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            int h = -1, m = -1, s = -1;
            sscanf(argv[i + 1], "%d:%d:%d", &h, &m, &s);
            if (h >= 0 && h <= 23) g_pc_time_override = h;
            if (m >= 0 && m <= 59) g_pc_min_override = m;
            if (s >= 0 && s <= 59) g_pc_sec_override = s;
            i++;
        } else if (strcmp(argv[i], "--date") == 0 && i + 1 < argc) {
            int mo = -1, d = -1, y = -1;
            sscanf(argv[i + 1], "%d/%d/%d", &mo, &d, &y);
            if (mo >= 1 && mo <= 12 && d >= 1 && d <= 31) {
                g_pc_date_month = mo;
                g_pc_date_day = d;
                if (y >= 2000) g_pc_date_year = y;
            }
            i++;
        } else if (strcmp(argv[i], "--rain") == 0) {
            g_pc_weather_override = mEnv_WEATHER_RAIN;
            g_pc_weather_intensity_override = mEnv_WEATHER_INTENSITY_HEAVY;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                int intensity = pc_parse_rain_intensity(argv[i + 1]);
                if (intensity >= 0) {
                    g_pc_weather_intensity_override = intensity;
                    i++;
                }
            }
#ifdef NETCODE_ENABLED
        } else if (strcmp(argv[i], "--online") == 0 && i + 1 < argc) {
            const char* endpoint = argv[++i];
            uint16_t port = (uint16_t)g_pc_online_port;
            char endpoint_error[128] = "";
            if (!pc_network_parse_endpoint(endpoint, g_pc_online_host, sizeof(g_pc_online_host),
                                           &port, endpoint_error, sizeof(endpoint_error))) {
                fprintf(stderr, "Invalid --online endpoint: %s\n", endpoint_error);
                return 2;
            }
            g_pc_online_port = port;
            g_pc_online_enabled = 1;
        } else if (strcmp(argv[i], "--offline") == 0) {
            g_pc_online_enabled = 0;
        } else if (strcmp(argv[i], "--network-config") == 0 && i + 1 < argc) {
            i++; /* Loaded before option processing so command-line values win. */
        } else if (strcmp(argv[i], "--town") == 0 && i + 1 < argc) {
            g_pc_online_town = (uint64_t)strtoull(argv[++i], NULL, 10);
            if (g_pc_online_town == 0) return 2;
        } else if (strcmp(argv[i], "--account") == 0 && i + 1 < argc) {
            g_pc_online_account = (uint64_t)strtoull(argv[++i], NULL, 10);
            if (g_pc_online_account == 0) return 2;
        } else if (strcmp(argv[i], "--invite-key") == 0 && i + 1 < argc) {
            size_t key_len = strlen(argv[++i]);
            if (key_len == 0 || key_len >= sizeof(g_pc_online_invite_key)) return 2;
            memcpy(g_pc_online_invite_key, argv[i], key_len + 1);
        } else if (strcmp(argv[i], "--quickstart") == 0 && i + 1 < argc) {
            g_pc_online_quickstart_name = argv[++i];
        } else if (strcmp(argv[i], "--quickstart-gender") == 0 && i + 1 < argc) {
            const char* gender = argv[++i];
            if (strcmp(gender, "male") == 0) g_pc_online_quickstart_gender = 0;
            else if (strcmp(gender, "female") == 0) g_pc_online_quickstart_gender = 1;
            else {
                fprintf(stderr, "Invalid quickstart gender: %s\n", gender);
                return 2;
            }
#endif
        }
    }

#ifdef NETCODE_ENABLED
    if (g_pc_online_quickstart_name != NULL &&
        (!g_pc_online_enabled ||
         !Net_ConfigureQuickstart(g_pc_online_quickstart_name, g_pc_online_quickstart_gender))) {
        fprintf(stderr, "Quickstart requires online mode and a 1-8 character alphanumeric name\n");
        return 2;
    }
#endif

    /* Redirect stdout/stderr to NUL unless verbose — unbuffered terminal writes
     * are extremely slow on Windows and tank FPS. */
    if (!g_pc_verbose && !g_pc_profile_enabled) {
#ifdef _WIN32
        freopen("NUL", "w", stdout);
        freopen("NUL", "w", stderr);
#else
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
#endif
    } else {
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }

    /* exe image range for seg2k0 — BSS can overlap N64 segment addresses */
#ifdef _WIN32
    {
        HMODULE exe = GetModuleHandle(NULL);
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)exe;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((char*)exe + dos->e_lfanew);
        pc_image_base = (uintptr_t)exe;
        pc_image_end = pc_image_base + nt->OptionalHeader.SizeOfImage;
    }
#elif defined(__APPLE__)
    {
        /* macOS: use dladdr — no ELF headers available */
        Dl_info dl;
        if (dladdr((void*)main, &dl) && dl.dli_fbase) {
            pc_image_base = (uintptr_t)dl.dli_fbase;
            /* Estimate image end — on 64-bit, seg2k0 uses threshold check
             * instead of image range, so this is defense-in-depth only. */
            pc_image_end = pc_image_base + 0x10000000;
        }
    }
#else
    {
        Dl_info dl;
        if (dladdr((void*)main, &dl) && dl.dli_fbase) {
            pc_image_base = (uintptr_t)dl.dli_fbase;
            /* 64-bit ELF */
            Elf64_Ehdr* ehdr = (Elf64_Ehdr*)dl.dli_fbase;
            Elf64_Phdr* phdr = (Elf64_Phdr*)((char*)dl.dli_fbase + ehdr->e_phoff);
            uintptr_t max_end = 0;
            for (int i = 0; i < ehdr->e_phnum; i++) {
                if (phdr[i].p_type == PT_LOAD) {
                    uintptr_t seg_end = phdr[i].p_vaddr + phdr[i].p_memsz;
                    if (seg_end > max_end) max_end = seg_end;
                }
            }
            /* ET_EXEC: p_vaddr is absolute. ET_DYN (PIE): relative to load address. */
            if (ehdr->e_type == ET_DYN) {
                pc_image_end = pc_image_base + max_end;
            } else {
                pc_image_end = max_end;
            }
        }
    }
#endif

    /* Installed only once the image range is known, so a fault report can turn
     * addresses into image offsets. Nothing called this before, which is why a
     * fatal fault closed the window without leaving anything behind. */
    pc_crash_protection_init();

    SDL_SetMainReady();
    pc_settings_load();
    pc_keybindings_load();
    pc_platform_init();
    pc_discord_init();
    pc_disc_init();
    if (!pc_assets_init()) {
        const char* msg =
            "No game data found.\n\n"
            "Animal Crossing needs the original GameCube ROM to run.\n"
            "Place a disc image (.iso, .gcm, or .ciso) to the \"rom\" subfolder.";
        fprintf(stderr, "[PC] %s\n", msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                 "Animal Crossing - Missing ROM", msg, g_pc_window);
        pc_discord_shutdown();
        pc_platform_shutdown();
        return 1;
    }

#ifdef NETCODE_ENABLED
    if (g_pc_verbose && g_pc_online_enabled) {
        printf("[NET] configured server=%s:%d town=%llu account=%llu source=network.ini/CLI\n",
               g_pc_online_host, g_pc_online_port,
               (unsigned long long)g_pc_online_town,
               (unsigned long long)g_pc_online_account);
    }
    if (g_pc_online_enabled &&
        !acnet_client_start(g_pc_online_host,
                            (uint16_t)g_pc_online_port,
                            g_pc_online_town,
                            g_pc_online_account,
                            0,
                            g_pc_online_invite_key)) {
        fprintf(stderr, "[NET] Unable to start online client: %s\n", acnet_client_last_error());
        pc_disc_shutdown();
        pc_discord_shutdown();
        pc_platform_shutdown();
        return 1;
    }
    if (g_pc_online_enabled) {
        const Uint32 connect_started = SDL_GetTicks();
        while (acnet_client_status() == ACNET_CONNECTING && SDL_GetTicks() - connect_started < 10000U) {
            if (!acnet_client_poll()) break;
            SDL_Delay(10);
        }
        if (acnet_client_status() != ACNET_CONNECTED) {
            fprintf(stderr, "[NET] Could not connect before game boot: %s\n", acnet_client_last_error());
            acnet_client_stop();
            pc_disc_shutdown();
            pc_discord_shutdown();
            pc_platform_shutdown();
            return 1;
        }
        if (g_pc_verbose) {
            uint8_t town_name[9] = {0};
            acnet_client_town_name(town_name, 8);
            printf("[NET] joined town='%.*s' resident_slot=%u seed=%u initialized=%d\n",
                   8, (const char*)town_name, (unsigned)acnet_client_resident_slot(),
                   (unsigned)acnet_client_town_seed(), acnet_client_town_initialized());
        }
    }
#endif

    ac_entry();                         /* sets HotStartEntry = &entry */
    boot_main(argc, (const char**)argv); /* full init → HotStartEntry → game loop */

#ifdef NETCODE_ENABLED
    acnet_client_stop();
#endif
    pc_disc_shutdown();
    pc_discord_shutdown();
    pc_platform_shutdown();
    return 0;
}
