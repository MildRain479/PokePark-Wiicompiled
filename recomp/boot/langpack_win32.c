/* Making a language pack (langpack.h) on Windows: the disc image chosen
 * in the system's picker is extracted by decomp-toolkit (tools\dtk.exe, as
 * import_win32.c sets the game up) into a folder beside the pack's, read
 * beside the USA game's extracted files, and deleted once the pack is
 * made. All of it on a thread of its own, the game going on. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_settings.h"
#include "langpack.h"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

const char* hle_dvd_root(void); /* hle_dvd.c */

static char sImage[MAX_PATH];

static void remove_tree(const char* path) {
    char from[MAX_PATH + 2];
    SHFILEOPSTRUCTA op;
    memset(from, 0, sizeof(from));
    snprintf(from, MAX_PATH, "%s", path);
    memset(&op, 0, sizeof(op));
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NO_UI;
    SHFileOperationA(&op);
}

/* dtk's exit code extracting image into out, -1 if it couldn't start. */
static int extract(const char* image, const char* out) {
    char cmd[3 * MAX_PATH], dtk[MAX_PATH], *slash;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = (DWORD)-1;
    GetModuleFileNameA(NULL, dtk, sizeof(dtk));
    if ((slash = strrchr(dtk, '\\')) != NULL) *slash = '\0';
    strncat(dtk, "\\tools\\dtk.exe", sizeof(dtk) - strlen(dtk) - 1);
    snprintf(cmd, sizeof(cmd), "\"%s\" disc extract -q \"%s\" \"%s\"", dtk, image, out);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(dtk, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL,
                        &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

static void work(void* arg) {
    char tmp[1100], id[7] = "", path[1200], *slash;
    LangDisc usa, other;
    FILE* f;
    int lang, code;
    (void)arg;
    /* Beside the packs, on the same drive: languages\.disc */
    snprintf(tmp, sizeof(tmp), "%s", settings_language_dir(LANGUAGE_JP));
    for (slash = tmp; *slash; slash++)
        if (*slash == '/') *slash = '\\'; /* (the shell's delete takes only these) */
    if (!(slash = strrchr(tmp, '\\'))) {
        langpack_fail("The languages folder couldn't be found");
        return;
    }
    *slash = '\0';
    CreateDirectoryA(tmp, NULL); /* languages */
    snprintf(slash, sizeof(tmp) - (slash - tmp), "\\.disc");
    remove_tree(tmp); /* what an earlier try left */
    CreateDirectoryA(tmp, NULL);
    code = extract(sImage, tmp);
    snprintf(path, sizeof(path), "%s\\sys\\boot.bin", tmp);
    if (code == -1) {
        langpack_fail("Couldn't start tools\\dtk.exe, which reads disc images");
    } else if (code != 0 || !(f = fopen(path, "rb"))) {
        langpack_fail("That disc image couldn't be read. Is the file complete?");
    } else {
        fread(id, 1, 6, f);
        fclose(f);
        if ((lang = langpack_language_of(id)) < 0) {
            char m[160];
            snprintf(m, sizeof(m), "%s is not a Japanese disc (R8AJ01)", id);
            langpack_fail(m);
        } else if (!langdisc_folder(&other, tmp)) {
            langpack_fail("The disc image's files couldn't be read");
        } else {
            if (!hle_dvd_root()[0] || !langdisc_folder(&usa, hle_dvd_root())) {
                langpack_fail("The game's own files couldn't be read");
            } else {
                langpack_run(lang, &usa, &other);
                langdisc_free(&usa);
            }
            langdisc_free(&other);
        }
    }
    remove_tree(tmp);
}

void langpack_choose(void) {
    OPENFILENAMEA ofn;
    const char* test = getenv("RECOMP_LANGPACK_IMAGE"); /* tests: the image, not the picker */
    if (langpack_state(NULL, NULL) == LANGPACK_RUNNING) return;
    if (test && *test) {
        snprintf(sImage, sizeof(sImage), "%s", test);
        langpack_set_running("Reading the disc image");
        if (langpack_thread(work, NULL) != 0) langpack_fail("Couldn't start making the pack");
        return;
    }
    sImage[0] = '\0';
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = "Disc images (*.iso;*.rvz;*.wia;*.wbfs;*.ciso;*.gcz;*.nfs)\0*.iso;*.rvz;*.wia;*.wbfs;*.ciso;*.gcz;*.nfs\0"
                      "All files (*.*)\0*.*\0";
    ofn.lpstrFile = sImage;
    ofn.nMaxFile = sizeof(sImage);
    ofn.lpstrTitle = "Choose your copy of PokePark Wii in another language (Japanese)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) return;
    langpack_set_running("Reading the disc image");
    if (langpack_thread(work, NULL) != 0) langpack_fail("Couldn't start making the pack");
}
