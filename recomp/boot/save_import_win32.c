/* Bringing over a save (wii_save.h), on Windows: a file chosen in the
 * shell's picker, over the game's window, either a Wii's data.bin or any
 * of the files in Dolphin's save folder (then all of them are taken), or a
 * zip of them, and the game started again, as it reads its saves as it
 * starts.
 *
 * Environment:
 *   RECOMP_SAVE_IMPORT=<file>   take it without asking (for test runs)
 *
 * And taking it out (save_export): a zip of its files, where the player
 * says in the save dialog. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_device.h"
#include "host_settings.h"
#include "wii_save.h"

#define MAX_FILES 32
#define MAX_SIZE (16u << 20) /* a save's files are 3 MB at most */

/* The picker's own memory of the folder chosen last, kept for this. */
static const GUID kClient = {0x3c9d6e21, 0x8a4f, 0x4b17, {0xa2, 0x5e, 0x71, 0x0d, 0x94, 0xc3, 0x6b, 0x28}};

/* The file chosen, or 0 if the player backed out. */
static int pick(wchar_t* out, size_t n) {
    static const COMDLG_FILTERSPEC kTypes[] = {
        {L"Wii or Dolphin save", L"*.bin;*.zip;GWD;GD0;GD1;GD2;GD3;Pic0;Pic1;Pic2;Pic3"},
        {L"All files", L"*.*"},
    };
    IFileOpenDialog* dlg = NULL;
    IShellItem* item = NULL;
    wchar_t* path = NULL;
    FILEOPENDIALOGOPTIONS opts;
    int ok = 0;
    HRESULT init;
    const char* env = getenv("RECOMP_SAVE_IMPORT");
    if (env && *env) return MultiByteToWideChar(CP_UTF8, 0, env, -1, out, (int)n) > 0;
    win_set_fullscreen(0); /* (the picker would be hidden behind it) */
    init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog,
                                   (void**)&dlg))) {
        IFileOpenDialog_SetClientGuid(dlg, &kClient);
        if (SUCCEEDED(IFileOpenDialog_GetOptions(dlg, &opts)))
            IFileOpenDialog_SetOptions(dlg, opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        IFileOpenDialog_SetFileTypes(dlg, 2, kTypes);
        IFileOpenDialog_SetTitle(dlg, L"Import a Wii save: its data.bin, a zip of it, or any file in Dolphin's save folder");
        IFileOpenDialog_SetOkButtonLabel(dlg, L"Import");
        if (SUCCEEDED(IFileOpenDialog_Show(dlg, (HWND)win_handle())) &&
            SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item))) {
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
                wcsncpy(out, path, n - 1);
                out[n - 1] = 0;
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

/* The file at path, read, as name, into f; 0 if it couldn't be. */
static int load(const wchar_t* path, const wchar_t* name, WiiSaveFile* f) {
    FILE* in = _wfopen(path, L"rb");
    long size;
    uint8_t* data;
    if (!in) return 0;
    fseek(in, 0, SEEK_END);
    size = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (size < 0 || (unsigned long)size > MAX_SIZE || !(data = malloc(size ? (size_t)size : 1))) {
        fclose(in);
        return 0;
    }
    if (fread(data, 1, (size_t)size, in) != (size_t)size) {
        free(data);
        fclose(in);
        return 0;
    }
    fclose(in);
    WideCharToMultiByte(CP_UTF8, 0, name, -1, f->name, sizeof(f->name), NULL, NULL);
    f->data = data;
    f->size = (uint32_t)size;
    return 1;
}

/* Starts the game again, as it was started; returns only if it couldn't. */
static void restart(void) {
    wchar_t exe[MAX_PATH], *line = _wcsdup(GetCommandLineW());
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (line && GetModuleFileNameW(NULL, exe, MAX_PATH) &&
        CreateProcessW(exe, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        exit(0); /* as closing the window does */
    }
    free(line);
}

int save_import(void) {
    wchar_t path[MAX_PATH], dir[MAX_PATH], *slash;
    char name[64];
    WiiSaveFile files[MAX_FILES];
    int count = 0, r, i;
    if (!pick(path, MAX_PATH)) return -1;
    slash = wcsrchr(path, L'\\');
    if (wcsrchr(path, L'/') > slash) slash = wcsrchr(path, L'/');
    WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : path, -1, name, sizeof(name), NULL, NULL);
    if (slash && wiisave_own_name(name)) {
        /* One of the files in Dolphin's folder: all of them. */
        WIN32_FIND_DATAW fd;
        HANDLE h;
        wcsncpy(dir, path, slash - path);
        dir[slash - path] = 0;
        swprintf(path, MAX_PATH, L"%ls\\*", dir);
        if ((h = FindFirstFileW(path, &fd)) == INVALID_HANDLE_VALUE) return WIISAVE_UNREADABLE;
        r = WIISAVE_OK;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            swprintf(path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            if (count == MAX_FILES || !load(path, fd.cFileName, &files[count])) {
                r = WIISAVE_UNREADABLE;
                break;
            }
            count++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    } else {
        r = load(path, slash ? slash + 1 : path, &files[0]) ? (count = 1, WIISAVE_OK) : WIISAVE_UNREADABLE;
    }
    if (r == WIISAVE_OK) r = wiisave_import_files(hle_nand_root(), files, count);
    for (i = 0; i < count; i++) free((void*)files[i].data);
    fprintf(stderr, "save import: %s\n", wiisave_message(r));
    if (r == WIISAVE_OK) restart();
    return r;
}

/* Taking the save out (wii_save.h): where the zip goes, in the shell's
 * save dialog, or 0 if the player backed out. RECOMP_SAVE_EXPORT=<file>
 * names it without asking (for test runs). */
static int pick_out(wchar_t* out, size_t n) {
    static const COMDLG_FILTERSPEC kTypes[] = {{L"Zip of Dolphin's save files", L"*.zip"}};
    IFileSaveDialog* dlg = NULL;
    IShellItem* item = NULL;
    wchar_t* path = NULL;
    FILEOPENDIALOGOPTIONS opts;
    int ok = 0;
    HRESULT init;
    const char* env = getenv("RECOMP_SAVE_EXPORT");
    if (env && *env) return MultiByteToWideChar(CP_UTF8, 0, env, -1, out, (int)n) > 0;
    win_set_fullscreen(0);
    init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileSaveDialog,
                                   (void**)&dlg))) {
        IFileSaveDialog_SetClientGuid(dlg, &kClient);
        if (SUCCEEDED(IFileSaveDialog_GetOptions(dlg, &opts)))
            IFileSaveDialog_SetOptions(dlg, opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
        IFileSaveDialog_SetFileTypes(dlg, 1, kTypes);
        IFileSaveDialog_SetDefaultExtension(dlg, L"zip");
        IFileSaveDialog_SetFileName(dlg, L"PokePark Wii save.zip");
        IFileSaveDialog_SetTitle(dlg, L"Export your save: a zip of its files, as in Dolphin's save folder");
        IFileSaveDialog_SetOkButtonLabel(dlg, L"Export");
        if (SUCCEEDED(IFileSaveDialog_Show(dlg, (HWND)win_handle())) &&
            SUCCEEDED(IFileSaveDialog_GetResult(dlg, &item))) {
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
                wcsncpy(out, path, n - 1);
                out[n - 1] = 0;
                CoTaskMemFree(path);
                ok = 1;
            }
            IShellItem_Release(item);
        }
        IFileSaveDialog_Release(dlg);
    }
    if (SUCCEEDED(init)) CoUninitialize();
    if (g_settings.fullscreen) win_set_fullscreen(1);
    return ok;
}

int save_export(void) {
    wchar_t path[MAX_PATH];
    uint8_t* zip;
    size_t size;
    FILE* out;
    int r = wiisave_export_zip(hle_nand_root(), wiisave_dos_now(), &zip, &size);
    if (r != WIISAVE_OK) {
        fprintf(stderr, "save export: %s\n", wiisave_export_message(r));
        return r;
    }
    if (!pick_out(path, MAX_PATH)) {
        free(zip);
        return -1;
    }
    out = _wfopen(path, L"wb");
    r = out && fwrite(zip, 1, size, out) == size ? WIISAVE_OK : WIISAVE_UNWRITABLE;
    if (out && fclose(out) != 0) r = WIISAVE_UNWRITABLE;
    free(zip);
    fprintf(stderr, "save export: %s (%u bytes)\n", wiisave_export_message(r), (unsigned)size);
    return r;
}
