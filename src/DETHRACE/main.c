
#include <stdio.h>
#include <stdlib.h>
#ifdef __DREAMCAST__
#include <kos.h>
#include "brender.h"
void * BR_RESIDENT_ENTRY HostImageLoad(char *name)
{
	return NULL;
}

void BR_RESIDENT_ENTRY HostImageUnload(void *image)
{
}

void * BR_RESIDENT_ENTRY HostImageLookupName(void *img, char *name, br_uint_32 hint)
{
	return NULL;
}

void * BR_RESIDENT_ENTRY HostImageLookupOrdinal(void *img, br_uint_32 ordinal)
{
	return NULL;
}
#endif

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

#include "brender.h"

extern int Harness_Init(int* argc, char* argv[]);
extern int Harness_Quit(void);
extern int original_main(int pArgc, char* pArgv[]);

void BR_CALLBACK _BrBeginHook(void) {
    struct br_device* BR_EXPORT BrDrv1SoftPrimBegin(char* arguments);
    struct br_device* BR_EXPORT BrDrv1SoftRendBegin(char* arguments);
    struct br_device* BR_EXPORT BrDrv1VirtualFramebufferBegin(char* arguments);
    struct br_device* BR_EXPORT BrDrv1GLBegin(char* arguments);

<<<<<<< HEAD
#if _MSC_VER != 1020
    BrDevAddStatic(NULL, BrDrv1SoftPrimBegin, NULL);
    BrDevAddStatic(NULL, BrDrv1SoftRendBegin, NULL);
    BrDevAddStatic(NULL, BrDrv1VirtualFramebufferBegin, NULL);
    BrDevAddStatic(NULL, BrDrv1GLBegin, NULL);
#endif
=======
    BrDevAddStatic(NULL, (br_device_begin_fn *)BrDrv1SoftPrimBegin, NULL);
    BrDevAddStatic(NULL, (br_device_begin_fn *)BrDrv1SoftRendBegin, NULL);
    // BrDevAddStatic(NULL, BrDrv1SDL2Begin, NULL);
>>>>>>> origin/pvr
}

void BR_CALLBACK _BrEndHook(void) {
}

int main(int argc, char* argv[]) {
    int result;

#ifdef __DREAMCAST__
    // Keep stdout/stderr unbuffered so the last log line before any crash is
    // not lost. Helps diagnosing bring-up issues over the dc-load console.
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#endif

#ifdef _WIN32
#if _MSC_VER != 1020
    /* Attach to the console that started us if any */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        /* We attached successfully, lets redirect IO to the consoles handles if not already redirected */
        if (_fileno(stdout) == -2 || _get_osfhandle(_fileno(stdout)) == -2) {
            freopen("CONOUT$", "w", stdout);
        }

        if (_fileno(stderr) == -2 || _get_osfhandle(_fileno(stderr)) == -2) {
            freopen("CONOUT$", "w", stderr);
        }

        if (_fileno(stdin) == -2 || _get_osfhandle(_fileno(stdin)) == -2) {
            freopen("CONIN$", "r", stdin);
        }
    }
#endif
<<<<<<< HEAD
#endif

    result = Harness_Init(&argc, argv);
    if (result != 0) {
        return result;
    }
=======
// #ifdef __DREAMCAST__
//     fs_chdir("/cd/dethrace");
// #endif    
    Harness_Init(&argc, argv);
>>>>>>> origin/pvr

    result = original_main(argc, argv);

    Harness_Quit();

    return result;
}
