/* Where the photos copied to the SD card go (host_sdcard.h): a folder
 * chosen in the shell's folder picker, over the game's window, which
 * remembers the one chosen last (starting in Pictures).
 *
 * Environment:
 *   RECOMP_SD_DIR=<dir>   save there without asking (for test runs) */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_device.h"
#include "host_sdcard.h"
#include "host_settings.h"

static wchar_t sFolder[MAX_PATH];

/* The picker's own memory of the folder chosen last, kept for this game. */
static const GUID kClient = {0x6b1f3a52, 0x4c2e, 0x4d8a, {0x9b, 0x61, 0x2f, 0x0c, 0x5e, 0x7a, 0x13, 0xd4}};

static int choose(void) {
    const char* env = getenv("RECOMP_SD_DIR");
    IFileOpenDialog* dlg = NULL;
    IShellItem *item = NULL, *pictures = NULL;
    wchar_t* path = NULL;
    FILEOPENDIALOGOPTIONS opts;
    int ok = 0;
    HRESULT init;
    if (env && *env) {
        MultiByteToWideChar(CP_UTF8, 0, env, -1, sFolder, MAX_PATH);
        CreateDirectoryW(sFolder, NULL);
        return 1;
    }
    win_set_fullscreen(0); /* (the picker would be hidden behind it) */
    init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog,
                                   (void**)&dlg))) {
        IFileOpenDialog_SetClientGuid(dlg, &kClient);
        if (SUCCEEDED(IFileOpenDialog_GetOptions(dlg, &opts)))
            IFileOpenDialog_SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        IFileOpenDialog_SetTitle(dlg, L"Save photos to");
        IFileOpenDialog_SetOkButtonLabel(dlg, L"Save here");
        if (SUCCEEDED(SHGetKnownFolderItem(&FOLDERID_Pictures, KF_FLAG_DEFAULT, NULL, &IID_IShellItem,
                                           (void**)&pictures))) {
            IFileOpenDialog_SetDefaultFolder(dlg, pictures);
            IShellItem_Release(pictures);
        }
        if (SUCCEEDED(IFileOpenDialog_Show(dlg, (HWND)win_handle())) &&
            SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item))) {
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
                wcsncpy(sFolder, path, MAX_PATH - 1);
                sFolder[MAX_PATH - 1] = 0;
                CoTaskMemFree(path);
                ok = 1;
            }
            IShellItem_Release(item);
        }
        IFileOpenDialog_Release(dlg);
    }
    if (SUCCEEDED(init)) CoUninitialize();
    if (g_settings.fullscreen) win_set_fullscreen(1);
    return ok;
}

/* name in the folder, or, if that's taken, with its number counted on
 * (R8A_0001.JPG, R8A_0002.JPG, ...), or else (2), (3)... added. */
static int save(const char* name, const uint8_t* data, uint32_t size) {
    wchar_t path[MAX_PATH + 64], wname[64];
    char stem[64], ext[16] = "";
    const char* dot = strrchr(name, '.');
    int n, digits = 0, num = 0, tries;
    HANDLE f;
    DWORD wrote = 0;
    snprintf(stem, sizeof(stem), "%.*s", dot ? (int)(dot - name) : (int)strlen(name), name);
    if (dot) snprintf(ext, sizeof(ext), "%s", dot);
    for (n = (int)strlen(stem); n > 0 && stem[n - 1] >= '0' && stem[n - 1] <= '9'; n--) digits++;
    if (digits) num = atoi(stem + n);
    for (tries = 0; tries < 10000; tries++) {
        char file[96];
        if (!tries) snprintf(file, sizeof(file), "%s%s", stem, ext);
        else if (digits) snprintf(file, sizeof(file), "%.*s%0*d%s", n, stem, digits, num + tries, ext);
        else snprintf(file, sizeof(file), "%s (%d)%s", stem, tries + 1, ext);
        MultiByteToWideChar(CP_UTF8, 0, file, -1, wname, 64);
        _snwprintf(path, MAX_PATH + 64, L"%s\\%s", sFolder, wname);
        f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) return 0;
    }
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(f, data, size, &wrote, NULL) || wrote != size) {
        CloseHandle(f);
        DeleteFileW(path);
        return 0;
    }
    CloseHandle(f);
    fprintf(stderr, "sd: saved %ls\n", path);
    return 1;
}

void sdcard_init(void) {
    g_host_sd_choose = choose;
    g_host_sd_save = save;
}
