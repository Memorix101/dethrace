// Dreamcast (KallistiOS) implementation of the harness OS layer.
//
// The filesystem root on Dreamcast is /cd (the ISO9660 image, in Flycast or on
// real hardware). Game assets live at /cd/DATA and the config at
// /cd/dethrace.ini, so OS_GetPrefPath resolves there and the harness chdir's
// into it, after which the game's relative "DATA/..." paths resolve correctly.

#include "harness/config.h"
#include "harness/os.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define DC_FS_ROOT "/cd/"

static DIR* directory_iterator;
static char name_buf[1024];

// KallistiOS newlib does not provide access(), which the harness uses to probe
// for asset files. Implement it via stat(); we only need existence checks.
int access(const char* pathname, int mode) {
    struct stat st;
    (void)mode;
    return stat(pathname, &st) == 0 ? 0 : -1;
}

// KOS has a built-in exception handler, so there is nothing to install here.
void OS_InstallSignalHandler(char* program_name) {
    (void)program_name;
}

void OS_RemoveSignalHandler(void) {
}

char* OS_GetFirstFileInDirectory(char* path) {
    directory_iterator = opendir(path);
    if (directory_iterator == NULL) {
        return NULL;
    }
    return OS_GetNextFileInDirectory();
}

char* OS_GetNextFileInDirectory(void) {
    struct dirent* entry;

    if (directory_iterator == NULL) {
        return NULL;
    }
    while ((entry = readdir(directory_iterator)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        return entry->d_name;
    }
    closedir(directory_iterator);
    directory_iterator = NULL;
    return NULL;
}

// Split a path into its directory part. Writes into the supplied buffer and
// returns it. Mirrors the subset of dirname() the harness relies on.
static char* dc_dirname(char* path) {
    char* slash = strrchr(path, '/');
    if (slash == NULL) {
        path[0] = '.';
        path[1] = '\0';
        return path;
    }
    if (slash == path) {
        path[1] = '\0';
    } else {
        *slash = '\0';
    }
    return path;
}

static char* dc_basename(char* path) {
    char* slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

// fopen with a case-insensitive directory fallback. The ISO9660 image may store
// names in a different case than the game requests (DOS assets are upper case
// but the game sometimes asks in mixed case).
FILE* OS_fopen(const char* pathname, const char* mode) {
    FILE* f = fopen(pathname, mode);
    if (f != NULL) {
        return f;
    }

    char dir_buf[512];
    char base_buf[512];
    strcpy(dir_buf, pathname);
    strcpy(base_buf, pathname);
    char* dir_name = dc_dirname(dir_buf);
    char* base_name = dc_basename(base_buf);

    DIR* dir = opendir(dir_name);
    if (dir == NULL) {
        return NULL;
    }
    for (struct dirent* entry = readdir(dir); entry != NULL; entry = readdir(dir)) {
        if (strcasecmp(base_name, entry->d_name) == 0) {
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", dir_name, entry->d_name);
            f = fopen(full, mode);
            break;
        }
    }
    closedir(dir);
    return f;
}

// No interactive console on the Dreamcast.
size_t OS_ConsoleReadPassword(char* pBuffer, size_t pBufferLen) {
    if (pBufferLen > 0) {
        pBuffer[0] = '\0';
    }
    return 0;
}

char* OS_Dirname(const char* path) {
    // argv[0] is NULL when booted from disc, so guard against it.
    if (path == NULL) {
        strcpy(name_buf, ".");
        return name_buf;
    }
    strcpy(name_buf, path);
    return dc_dirname(name_buf);
}

char* OS_Basename(const char* path) {
    if (path == NULL) {
        strcpy(name_buf, ".");
        return name_buf;
    }
    strcpy(name_buf, path);
    return dc_basename(name_buf);
}

char* OS_GetWorkingDirectory(char* argv0) {
    return OS_Dirname(argv0);
}

// Resolve to the CD root, where dethrace.ini and DATA live.
int OS_GetPrefPath(char* dest, char* app) {
    (void)app;
    strcpy(dest, DC_FS_ROOT);
    return 0;
}

// Networking is disabled on the Dreamcast build. These remain as stubs so the
// harness links cleanly.
int OS_GetAdapterAddress(char* name, void* pSockaddr_in) {
    (void)name;
    (void)pSockaddr_in;
    return 0;
}

int OS_InitSockets(void) {
    return 0;
}

int OS_GetLastSocketError(void) {
    return 0;
}

void OS_CleanupSockets(void) {
}

int OS_SetSocketNonBlocking(int socket) {
    (void)socket;
    return 0;
}

int OS_CloseSocket(int socket) {
    (void)socket;
    return 0;
}
