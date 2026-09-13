/* ConkerRecomp native Windows launcher.
 * Copyright (C) 2026 avak1an. See the project LICENSE.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include "title_art.h"
#include <xinput.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_TITLE "Conker: Live & Recomped - Launcher"
#define PC_PORT_NAME "ConkerRecomp"
#define PC_PORT_VERSION "Development"
#define TIMER_PROCESS 1
#define TIMER_LOGO 2
enum {
    IDC_ISO_EDIT = 100, IDC_ISO_BROWSE, IDC_ISO_STATUS, IDC_ISO_VERIFY, IDC_ISO_HASH,
    IDC_SCALE_1, IDC_SCALE_2, IDC_SCALE_3, IDC_SCALE_4, IDC_FULLSCREEN,
    IDC_NO_CONSOLE, IDC_VOLUME, IDC_MUTE, IDC_KEYMAP_EDIT, IDC_KEYMAP_BROWSE,
    IDC_KEYMAP_CREATE, IDC_ADAPTER, IDC_ADAPTER_STATUS, IDC_SAVES_EDIT,
    IDC_SAVES_BROWSE, IDC_SAVES_OPEN, IDC_MODS_LIST, IDC_MODS_UP, IDC_MODS_DOWN,
    IDC_MODS_REFRESH, IDC_MODS_OPEN, IDC_EXTRACT, IDC_EXTRACT_OPEN, IDC_EXTRA_EDIT,
    IDC_SRC_EDIT, IDC_SRC_BROWSE, IDC_BUILD, IDC_BUILD_STATUS, IDC_PLAY, IDC_STATUS,
    IDC_DATA_EDIT, IDC_DATA_BROWSE, IDC_LOG_OPEN,
    IDC_NAV_FIRST, IDC_NAV_LAST = IDC_NAV_FIRST + 7, IDC_LAST
};
static HINSTANCE app;
static HWND main_wnd, content_wnd;
static HFONT ui_font, bold_font;
static char exe_dir[MAX_PATH], exe_path[MAX_PATH], ini_path[MAX_PATH];
static char local_dir[MAX_PATH], result_path[MAX_PATH], log_path[MAX_PATH];
static HANDLE child;
static HANDLE instance_mutex;
static int child_kind; /* 0 game, 1 verification, 2 local rebuild, 3 import/generate */
static int iso_verified, loading_settings; /* verification: 0 unknown, 1 release, 2 development, -1 rejected */
static DWORD child_exit_code;
static char labels[IDC_LAST - 100][512];
static void refresh_state(void);
static void load_logo(void);
static void set_status(const char *message);
static void save_settings(void);
static int toggle_get(int id);
static int volume_get(void);

static HWND ctl(int id)
{
    HWND c = content_wnd ? GetDlgItem(content_wnd, id) : NULL;
    return c ? c : GetDlgItem(main_wnd, id);
}
static int is_label(int id)
{
    return id == IDC_ISO_STATUS || id == IDC_ISO_HASH || id == IDC_BUILD_STATUS ||
        id == IDC_ADAPTER_STATUS || id == IDC_STATUS;
}
static void set_text(int id, const char *value)
{
    if (is_label(id)) {
        snprintf(labels[id - 100], sizeof(labels[0]), "%s", value);
        if (content_wnd) InvalidateRect(content_wnd, NULL, FALSE);
        if (main_wnd) InvalidateRect(main_wnd, NULL, FALSE);
    } else SetWindowTextA(ctl(id), value);
}
static void get_text(int id, char *out, int size) { GetWindowTextA(ctl(id), out, size); }
static int file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static int join(char *out, size_t size, const char *dir, const char *leaf)
{
    int n = snprintf(out, size, "%s\\%s", dir, leaf);
    return n >= 0 && (size_t)n < size;
}
static void ini_get(const char *key, char *out, int size, const char *def)
{ GetPrivateProfileStringA("conker", key, def, out, size, ini_path); }
static void ini_set(const char *key, const char *value)
{ WritePrivateProfileStringA("conker", key, value, ini_path); }
static int ini_get_int(const char *key, int def)
{ return GetPrivateProfileIntA("conker", key, def, ini_path); }
static void ini_set_int(const char *key, int value)
{ char b[32]; snprintf(b, sizeof(b), "%d", value); ini_set(key, b); }
static int init_local_paths(void)
{
    char base[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, base))) return 0;
#ifdef CONKER_LAUNCHER_TEST
    if (!join(local_dir, sizeof(local_dir), exe_dir, "test-profile")) return 0;
#else
    if (!join(local_dir, sizeof(local_dir), base, "ConkerRecomp")) return 0;
#endif
    if (!CreateDirectoryA(local_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    char task_name[64], log_name[64];
    snprintf(task_name, sizeof(task_name), "launcher-task-%lu.ini", GetCurrentProcessId());
    snprintf(log_name, sizeof(log_name), "launcher-task-%lu.log", GetCurrentProcessId());
    return join(ini_path, sizeof(ini_path), local_dir, "launcher.ini") &&
        join(result_path, sizeof(result_path), local_dir, task_name) &&
        join(log_path, sizeof(log_path), local_dir, log_name);
}
static int source_valid(const char *source)
{
    char p[MAX_PATH];
    return source[0] && join(p, sizeof(p), source, "tools\\launcher\\backend.py") && file_exists(p);
}
static void find_source_dir(char *out, int size)
{
    char current[MAX_PATH];
    snprintf(current, sizeof(current), "%s", exe_dir);
    for (int i = 0; i < 5; ++i) {
        if (source_valid(current)) { snprintf(out, size, "%s", current); return; }
        char *slash = strrchr(current, '\\');
        if (!slash || slash <= current + 2) break;
        *slash = 0;
    }
    out[0] = 0;
}
static int read_snapshot(const char *path, wchar_t *out, DWORD capacity);
static void task_value(const wchar_t *snapshot, const wchar_t *key, char *out, int size);
static int find_game_exe(char *out, int size)
{
    char source[MAX_PATH], receipt[MAX_PATH], data[MAX_PATH], built_data[MAX_PATH];
    wchar_t snapshot[4096];
    get_text(IDC_SRC_EDIT, source, sizeof(source));
    get_text(IDC_DATA_EDIT, data, sizeof(data));
    if (!source_valid(source) || !join(receipt, sizeof(receipt), source, "build\\runtime-ready.ini") ||
        !read_snapshot(receipt, snapshot, sizeof(snapshot)/sizeof(snapshot[0]))) return 0;
    task_value(snapshot, L"data_dir", built_data, sizeof(built_data));
    if (!data[0] || _stricmp(data, built_data)) return 0;
    return join(out, size, source, "build\\Debug\\conker_recomp.exe") && file_exists(out);
}
static int can_rebuild(void)
{
    char source[MAX_PATH], data[MAX_PATH], path[MAX_PATH];
    get_text(IDC_SRC_EDIT, source, sizeof(source));
    get_text(IDC_DATA_EDIT, data, sizeof(data));
    const char *required[] = {"..\\preparation.json", "..\\generated\\recomp\\recomp_dispatch.c"};
    if (!source_valid(source) || !data[0]) return 0;
    for (unsigned i = 0; i < sizeof(required)/sizeof(required[0]); ++i)
        if (!join(path, sizeof(path), data, required[i]) || !file_exists(path)) return 0;
    return 1;
}
static int data_valid(void)
{
    char dir[MAX_PATH], p[MAX_PATH]; get_text(IDC_DATA_EDIT, dir, sizeof(dir));
    return dir[0] && join(p, sizeof(p), dir, "default.xbe") && file_exists(p);
}
static int python_path(char *out, int size)
{
    DWORD n = SearchPathA(NULL, "python.exe", NULL, size, out, NULL);
    if (!n || n >= (DWORD)size || strstr(out, "WindowsApps")) {
        n = SearchPathA(NULL, "py.exe", NULL, size, out, NULL);
    }
    return n && n < (DWORD)size;
}
/* Windows CRT argument quoting, including trailing backslashes. No shell. */
static int append_arg(char *out, size_t size, const char *arg)
{
    size_t n = strlen(out), len = strlen(arg), slashes = 0;
    if (n + len * 2 + 4 >= size) return 0;
    if (n) out[n++] = ' ';
    out[n++] = '"';
    for (const char *p = arg; ; ++p) {
        if (*p == '\\') { ++slashes; continue; }
        size_t copies = (*p == '"' || !*p) ? slashes * 2 : slashes;
        while (copies--) out[n++] = '\\';
        slashes = 0;
        if (!*p) break;
        if (*p == '"') out[n++] = '\\';
        out[n++] = *p;
    }
    out[n++] = '"'; out[n] = 0; return 1;
}
static int run_child(const char *program, char *cmd, const char *cwd, int kind)
{
    STARTUPINFOA si = {0}; PROCESS_INFORMATION pi = {0};
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    char output[MAX_PATH];
    if (child) return 0;
    if (kind == 0) join(output, sizeof(output), local_dir, "game.log");
    else snprintf(output, sizeof(output), "%s", log_path);
    HANDLE log = CreateFileA(output, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        set_status("Cannot open the local task log."); return 0;
    }
    si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = log; si.hStdInput = input;
    BOOL ok = CreateProcessA(program, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi);
    DWORD error = GetLastError(); CloseHandle(log); CloseHandle(input);
    if (!ok) {
        char msg[128]; snprintf(msg, sizeof(msg), "Could not start the program (Windows error %lu).", error);
        set_status(msg); return 0;
    }
    CloseHandle(pi.hThread); child = pi.hProcess; child_kind = kind;
    SetTimer(main_wnd, TIMER_PROCESS, 250, NULL); refresh_state(); return 1;
}
static void run_task(int kind)
{
    char source[MAX_PATH], image[MAX_PATH], script[MAX_PATH], python[MAX_PATH], cmd[4096] = "";
    get_text(IDC_SRC_EDIT, source, sizeof(source)); get_text(IDC_ISO_EDIT, image, sizeof(image));
    if (!source_valid(source) || !python_path(python, sizeof(python))) {
        set_status("Choose the ConkerRecomp source folder and install Python 3.10 or newer."); return;
    }
    if (kind != 2 && !file_exists(image)) { set_status("Choose your Conker ISO or XISO first."); return; }
    join(script, sizeof(script), source, "tools\\launcher\\backend.py");
    int ok = append_arg(cmd, sizeof(cmd), python);
    const char *name = strrchr(python, '\\'); name = name ? name + 1 : python;
    if (!_stricmp(name, "py.exe")) ok &= append_arg(cmd, sizeof(cmd), "-3");
    ok &= append_arg(cmd, sizeof(cmd), script);
    ok &= append_arg(cmd, sizeof(cmd), kind == 1 ? "verify" : kind == 3 ? "prepare" : "build");
    ok &= append_arg(cmd, sizeof(cmd), "--root"); ok &= append_arg(cmd, sizeof(cmd), source);
    ok &= append_arg(cmd, sizeof(cmd), "--result"); ok &= append_arg(cmd, sizeof(cmd), result_path);
    if (kind != 2) { ok &= append_arg(cmd, sizeof(cmd), "--image"); ok &= append_arg(cmd, sizeof(cmd), image); }
    else {
        char data[MAX_PATH]; get_text(IDC_DATA_EDIT, data, sizeof(data));
        ok &= append_arg(cmd, sizeof(cmd), "--data-dir"); ok &= append_arg(cmd, sizeof(cmd), data);
    }
    if (!ok) { set_status("A selected path is too long."); return; }
    save_settings(); DeleteFileA(result_path); child_exit_code = STILL_ACTIVE;
    if (kind == 1) { iso_verified = 0; set_text(IDC_ISO_HASH, "Reading the entire image..."); }
    if (run_child(python, cmd, source, kind))
        set_status(kind == 1 ? "Verifying the disc SHA-1 and SHA-256..." :
            kind == 3 ? "Importing and generating locally. Progress is in the task log." :
            "Building locally. Compiler output is in the task log.");
}
static void play(void)
{
    char game[MAX_PATH], data[MAX_PATH], source[MAX_PATH], cmd[2048] = "";
    char volume[32]; char *previous = NULL; DWORD old_count;
    if (!find_game_exe(game, sizeof(game)) || !data_valid()) {
        set_status("Select local game data and build conker_recomp.exe first."); return;
    }
    save_settings(); get_text(IDC_DATA_EDIT, data, sizeof(data)); get_text(IDC_SRC_EDIT, source, sizeof(source));
    if (!append_arg(cmd, sizeof(cmd), game) || !append_arg(cmd, sizeof(cmd), "--data-dir") ||
        !append_arg(cmd, sizeof(cmd), data)) { set_status("A selected path is too long."); return; }
    old_count = GetEnvironmentVariableA("CONKER_VOLUME", NULL, 0);
    if (old_count) {
        previous = malloc(old_count);
        if (!previous) { set_status("Unable to allocate launch settings."); return; }
        GetEnvironmentVariableA("CONKER_VOLUME", previous, old_count);
    }
    snprintf(volume, sizeof(volume), "%d", toggle_get(IDC_MUTE) ? 0 : volume_get());
    SetEnvironmentVariableA("CONKER_VOLUME", volume);
    int started = run_child(game, cmd, source, 0);
    SetEnvironmentVariableA("CONKER_VOLUME", previous);
    free(previous);
    if (started) set_status("Game running at 640 x 480 (4:3). Focus the game for controller input.");
}
/* Read one complete snapshot without the profile API's cache or a handle
 * that blocks atomic replacement. A result is bounded UTF-16LE with a BOM. */
static int read_snapshot(const char *path, wchar_t *out, DWORD capacity)
{
    HANDLE file = CreateFileA(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    out[0] = 0;
    if (file == INVALID_HANDLE_VALUE) return 0;
    DWORD size = GetFileSize(file, NULL), count = 0;
    int ok = size >= 2 && !(size & 1) && size < capacity * sizeof(wchar_t) &&
        ReadFile(file, out, size, &count, NULL) && count == size;
    CloseHandle(file);
    if (!ok || out[0] != 0xfeff) { out[0] = 0; return 0; }
    out[size / sizeof(wchar_t)] = 0;
    return 1;
}
static void task_value(const wchar_t *snapshot, const wchar_t *key, char *out, int size)
{
    size_t length = wcslen(key);
    out[0] = 0;
    if (*snapshot == 0xfeff) ++snapshot;
    for (const wchar_t *line = snapshot; *line; ) {
        const wchar_t *end = wcschr(line, L'\n');
        if (!end) end = line + wcslen(line);
        if ((size_t)(end - line) > length && !wcsncmp(line, key, length) && line[length] == L'=') {
            const wchar_t *value = line + length + 1;
            if (end > value && end[-1] == L'\r') --end;
            int n = WideCharToMultiByte(CP_UTF8, 0, value, (int)(end - value), out, size - 1, NULL, NULL);
            out[n] = 0; return;
        }
        line = *end ? end + 1 : end;
    }
}
static void read_task(int finished)
{
    wchar_t snapshot[4096];
    char msg[512], detail[512], state[64], data[MAX_PATH];
    if (!read_snapshot(result_path, snapshot, sizeof(snapshot)/sizeof(snapshot[0])) && !finished) return;
    task_value(snapshot, L"message", msg, sizeof(msg));
    task_value(snapshot, L"detail", detail, sizeof(detail));
    task_value(snapshot, L"state", state, sizeof(state));
    if (finished && (!state[0] || !strcmp(state, "working") || !msg[0] ||
        (child_exit_code != 0 && (!strcmp(state, "matched") || !strcmp(state, "development") ||
                                !strcmp(state, "prepared") || !strcmp(state, "complete"))))) {
        snprintf(msg, sizeof(msg), "The helper stopped before completing (exit %lu). Open Task log for details.", child_exit_code);
        snprintf(state, sizeof(state), "error"); detail[0] = 0;
    }
    if (child_kind == 1) {
        if (msg[0]) set_text(IDC_ISO_STATUS, msg);
        if (detail[0]) set_text(IDC_ISO_HASH, detail);
        if (finished) iso_verified = !strcmp(state, "matched") ? 1 : !strcmp(state, "development") ? 2 : -1;
    } else if (msg[0]) {
        set_text(IDC_BUILD_STATUS, msg);
        if (child_kind == 3) {
            set_text(IDC_ISO_STATUS, msg);
            if (finished && child_exit_code == 0 && !strcmp(state, "prepared")) {
                task_value(snapshot, L"data_dir", data, sizeof(data));
                if (data[0]) { set_text(IDC_DATA_EDIT, data); save_settings(); }
            }
        }
    }
    if (msg[0]) set_status(msg);
}
static void child_finished(void)
{
    DWORD code = 0; GetExitCodeProcess(child, &code); child_exit_code = code; CloseHandle(child); child = NULL;
    KillTimer(main_wnd, TIMER_PROCESS);
    if (child_kind) read_task(1);
    else {
        char msg[128]; snprintf(msg, sizeof(msg), "Game closed (exit code %lu).", code); set_status(msg);
    }
    refresh_state();
}
static void set_status(const char *message) { set_text(IDC_STATUS, message); }
static void save_settings(void)
{
    char value[MAX_PATH];
    find_source_dir(value, sizeof(value)); ini_set("source_owner", value);
    get_text(IDC_SRC_EDIT, value, sizeof(value)); ini_set("source", value);
    get_text(IDC_DATA_EDIT, value, sizeof(value)); ini_set("data", value);
    get_text(IDC_ISO_EDIT, value, sizeof(value)); ini_set("iso", value);
    ini_set_int("volume", volume_get()); ini_set_int("mute", toggle_get(IDC_MUTE));
}

static int browse_file(const char* title, const char* filter, char* path, int size)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = main_wnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = (DWORD) size;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetOpenFileNameA(&ofn) != 0;
}

static int CALLBACK browse_folder_init(HWND h, UINT msg, LPARAM lp, LPARAM data)
{
    if (msg == BFFM_INITIALIZED && data != 0) {
        SendMessageA(h, BFFM_SETSELECTIONA, TRUE, data);
    }
    return 0;
}

static int browse_folder(const char* title, char* path, int size)
{
    BROWSEINFOA bi;
    LPITEMIDLIST pidl;
    char start[MAX_PATH];
    strncpy(start, path, sizeof(start) - 1);
    start[sizeof(start) - 1] = '\0';
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = main_wnd;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = browse_folder_init;
    bi.lParam = (LPARAM) (start[0] ? start : NULL);
    pidl = SHBrowseForFolderA(&bi);
    if (pidl == NULL) {
        return 0;
    }
    if (!SHGetPathFromIDListA(pidl, path)) {
        CoTaskMemFree(pidl);
        return 0;
    }
    CoTaskMemFree(pidl);
    (void) size;
    return 1;
}

#define C_BG RGB(0x0F, 0x11, 0x16)
#define C_SIDEBAR RGB(0x14, 0x17, 0x1E)
#define C_CARD RGB(0x1A, 0x1E, 0x26)
#define C_CARD_EDGE RGB(0x28, 0x2D, 0x37)
#define C_INPUT RGB(0x11, 0x13, 0x19)
#define C_INPUT_EDGE RGB(0x2C, 0x31, 0x3C)
#define C_TEXT RGB(0xE8, 0xEA, 0xEE)
#define C_TEXT_DIM RGB(0x9A, 0xA1, 0xAD)
#define C_ACCENT RGB(0x25, 0x74, 0xF0)
#define C_ACCENT_HI RGB(0x3D, 0x86, 0xF7)
#define C_ACCENT_LO RGB(0x1B, 0x5C, 0xC4)
#define C_GREEN RGB(0x2E, 0xC2, 0x6E)
#define C_ORANGE RGB(0xF0, 0xA0, 0x30)
#define C_RED RGB(0xE5, 0x4D, 0x4D)
#define C_BTN RGB(0x24, 0x29, 0x33)
#define C_BTN_HI RGB(0x30, 0x36, 0x42)
#define C_TRACK RGB(0x2C, 0x31, 0x3C)
#define C_WHITE RGB(0xFF, 0xFF, 0xFF)

enum { K_NONE, K_PRIMARY, K_SECONDARY, K_TOGGLE, K_SEGMENT, K_NAV };

enum { CARD_GAME, CARD_BUILD, CARD_DISPLAY, CARD_AUDIO, CARD_CONTROLS, CARD_SAVES, CARD_ABOUT, CARD_ADVANCED, CARD_COUNT };
enum { PAGE_SETUP, PAGE_BUILD, PAGE_DISPLAY, PAGE_AUDIO, PAGE_CONTROLS, PAGE_SAVES, PAGE_ABOUT, PAGE_ADVANCED, PAGE_COUNT };

#define SLM_SETPOS (WM_USER + 1)
#define SLM_GETPOS (WM_USER + 2)

static const struct {
    const char* name;
    wchar_t icon;
} nav_items[PAGE_COUNT] = {
    { "Setup", 0xE713 },    { "Build", 0xE90F }, { "Display", 0xE7F4 }, { "Audio", 0xE767 },
    { "Controls", 0xE7FC }, { "Saves", 0xE74E }, { "About", 0xE946 },    { "Files", 0xE9E9 },
};

static const unsigned page_cards[PAGE_COUNT] = {
    0xFF,
    (1u << CARD_GAME) | (1u << CARD_BUILD),
    1u << CARD_DISPLAY,
    1u << CARD_AUDIO,
    1u << CARD_CONTROLS,
    1u << CARD_SAVES,
    1u << CARD_ABOUT,
    1u << CARD_ADVANCED,
};

/* title, icon and height (unscaled) of each card */
static const struct {
    const char* title;
    wchar_t icon;
    int h;
} card_info[CARD_COUNT] = {
    { "Conker: Live & Recomped", 0, 330 }, { "Build", 0xE90F, 216 },   { "Display", 0xE7F4, 184 },
    { "Audio", 0xE767, 184 },              { "Controls", 0xE7FC, 186 }, { "Saves", 0xE74E, 166 },
    { "About", 0xE946, 210 },               { "Files", 0xE9E9, 160 },
};

/* the card grid of the Setup page: one or two cards per row */
static const int card_rows[][2] = {
    { CARD_GAME, -1 },         { CARD_BUILD, -1 },       { CARD_DISPLAY, CARD_AUDIO },
    { CARD_CONTROLS, -1 },     { CARD_ABOUT, CARD_SAVES }, { CARD_ADVANCED, -1 },
};

struct ctlinfo {
    int kind;     /* K_* for owner-drawn buttons */
    int card;     /* the card it belongs to, -1 for the sidebar and footer */
    int state;    /* toggle / segment / sidebar selection */
    wchar_t icon; /* Segoe MDL2 glyph drawn before the text, 0 for none */
    int framed;   /* an input frame is painted behind it */
    RECT frame;
};

#define NCTL (IDC_LAST - 100)
#define INFO(id) (info[(id) - 100])
static struct ctlinfo info[NCTL];

static int dpi = 96;
static int page = PAGE_SETUP;
static int scroll_y, content_total;
static int in_layout;
static HFONT font_semi, font_head, font_title, font_small, font_icon, font_icon_lg, font_play;
static HBRUSH br_input;
static HBITMAP banner;


static HFONT font_icon_sm;

static RECT card_rc[CARD_COUNT];
static int have_icon_font;
static int S(int v) { return MulDiv(v, dpi, 96); }
static int toggle_get(int id)
{
    return INFO(id).state;
}

static void toggle_set(int id, int on)
{
    INFO(id).state = on ? 1 : 0;
    InvalidateRect(ctl(id), NULL, FALSE);
}

static int volume_get(void)
{
    return (int) SendMessageA(ctl(IDC_VOLUME), SLM_GETPOS, 0, 0);
}

static void volume_set(int v)
{
    SendMessageA(ctl(IDC_VOLUME), SLM_SETPOS, (WPARAM) v, 0);
}

static void fill(HDC dc, RECT r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void round_rect(HDC dc, RECT r, int radius, COLORREF fill_c, COLORREF edge_c, int dashed)
{
    HPEN pen = CreatePen(dashed ? PS_DASH : PS_SOLID, 1, edge_c);
    HBRUSH br = CreateSolidBrush(fill_c);
    HPEN op = (HPEN) SelectObject(dc, pen);
    HBRUSH ob = (HBRUSH) SelectObject(dc, br);
    SetBkMode(dc, TRANSPARENT);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(br);
}

static void dot(HDC dc, int cx, int cy, int radius, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    HPEN op = (HPEN) SelectObject(dc, GetStockObject(NULL_PEN));
    HBRUSH ob = (HBRUSH) SelectObject(dc, br);
    Ellipse(dc, cx - radius, cy - radius, cx + radius + 1, cy + radius + 1);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(br);
}

static void text(HDC dc, HFONT f, COLORREF c, int x, int y, int w, int h, const char* s, UINT flags)
{
    RECT r;
    HFONT of = (HFONT) SelectObject(dc, f);
    r.left = x;
    r.top = y;
    r.right = x + w;
    r.bottom = y + h;
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextA(dc, s, -1, &r, flags | DT_NOPREFIX);
    SelectObject(dc, of);
}

static int text_width(HDC dc, HFONT f, const char* s)
{
    SIZE sz;
    HFONT of = (HFONT) SelectObject(dc, f);
    GetTextExtentPoint32A(dc, s, (int) strlen(s), &sz);
    SelectObject(dc, of);
    return sz.cx;
}

static void glyph(HDC dc, HFONT f, COLORREF c, int x, int y, int w, int h, wchar_t g)
{
    RECT r;
    wchar_t s[2];
    HFONT of;
    if (!have_icon_font || g == 0) {
        return;
    }
    s[0] = g;
    s[1] = 0;
    of = (HFONT) SelectObject(dc, f);
    r.left = x;
    r.top = y;
    r.right = x + w;
    r.bottom = y + h;
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, 1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, of);
}

/* --- owner-drawn buttons ---------------------------------------------------------- */

static void draw_button(DRAWITEMSTRUCT* di)
{
    int id = (int) di->CtlID;
    struct ctlinfo* ci = &INFO(id);
    RECT r = di->rcItem;
    HDC dc = di->hDC;
    char label[256];
    int pressed = (di->itemState & ODS_SELECTED) != 0;
    int disabled = (di->itemState & ODS_DISABLED) != 0;
    int focus = (di->itemState & ODS_FOCUS) != 0 && !(di->itemState & ODS_NOFOCUSRECT);
    COLORREF bg = ci->kind == K_NAV ? C_SIDEBAR : id == IDC_PLAY ? C_BG : C_CARD;
    COLORREF fg = disabled ? C_TEXT_DIM : C_TEXT;

    GetWindowTextA(di->hwndItem, label, sizeof(label));
    fill(dc, r, bg);
    switch (ci->kind) {
    case K_PRIMARY: {
        COLORREF c = disabled ? C_BTN : pressed ? C_ACCENT_LO : C_ACCENT;
        round_rect(dc, r, S(8), c, c, 0);
        if (ci->icon != 0 && have_icon_font) {
            int tw = text_width(dc, font_play, label) + S(30);
            int x = (r.left + r.right - tw) / 2;
            glyph(dc, font_icon, disabled ? C_TEXT_DIM : C_WHITE, x, r.top, S(24), r.bottom - r.top, ci->icon);
            text(dc, font_play, disabled ? C_TEXT_DIM : C_WHITE, x + S(30), r.top, tw, r.bottom - r.top, label,
                 DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        } else {
            text(dc, font_semi, disabled ? C_TEXT_DIM : C_WHITE, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 label, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }
        break;
    }
    case K_SECONDARY:
        round_rect(dc, r, S(8), pressed ? C_BTN_HI : C_BTN, C_INPUT_EDGE, 0);
        if (ci->icon != 0 && have_icon_font && text_width(dc, ui_font, label) + S(42) <= r.right - r.left) {
            int tw = text_width(dc, ui_font, label) + S(26);
            int x = (r.left + r.right - tw) / 2;
            glyph(dc, font_icon, fg, x, r.top, S(20), r.bottom - r.top, ci->icon);
            text(dc, ui_font, fg, x + S(26), r.top, tw, r.bottom - r.top, label, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        } else {
            text(dc, ui_font, fg, r.left + S(4), r.top, r.right - r.left - S(8), r.bottom - r.top, label,
                 DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
        }
        break;
    case K_TOGGLE: {
        int cy = (r.top + r.bottom) / 2;
        RECT pill;
        pill.left = r.left;
        pill.right = r.left + S(40);
        pill.top = cy - S(10);
        pill.bottom = cy + S(10);
        round_rect(dc, pill, S(10), ci->state ? (disabled ? C_ACCENT_LO : C_ACCENT) : C_TRACK,
                   ci->state ? (disabled ? C_ACCENT_LO : C_ACCENT) : C_INPUT_EDGE, 0);
        dot(dc, ci->state ? pill.right - S(10) : pill.left + S(10), cy, S(7), disabled ? C_TEXT_DIM : C_WHITE);
        text(dc, ui_font, fg, r.left + S(52), r.top, r.right - r.left - S(52), r.bottom - r.top, label,
             DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        if (focus) {
            RECT fr = pill;
            InflateRect(&fr, S(3), S(3));
            round_rect(dc, fr, S(12), C_CARD, C_ACCENT_HI, 0);
            round_rect(dc, pill, S(10), ci->state ? C_ACCENT : C_TRACK, ci->state ? C_ACCENT : C_INPUT_EDGE, 0);
            dot(dc, ci->state ? pill.right - S(10) : pill.left + S(10), cy, S(7), C_WHITE);
        }
        return;
    }
    case K_SEGMENT:
        round_rect(dc, r, S(6), ci->state ? C_ACCENT : pressed ? C_BTN_HI : C_BTN,
                   ci->state ? C_ACCENT : C_INPUT_EDGE, 0);
        text(dc, font_semi, ci->state ? C_WHITE : fg, r.left, r.top, r.right - r.left, r.bottom - r.top, label,
             DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        break;
    case K_NAV:
        if (ci->state) {
            round_rect(dc, r, S(8), C_ACCENT, C_ACCENT, 0);
        } else if (pressed) {
            round_rect(dc, r, S(8), C_BTN, C_BTN, 0);
        }
        glyph(dc, font_icon, ci->state ? C_WHITE : C_TEXT_DIM, r.left + S(12), r.top, S(24), r.bottom - r.top,
              ci->icon);
        text(dc, ci->state ? font_semi : ui_font, ci->state ? C_WHITE : C_TEXT_DIM, r.left + S(46), r.top,
             r.right - r.left - S(50), r.bottom - r.top, label, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        break;
    default:
        break;
    }
    if (focus && ci->kind != K_NAV) {
        HPEN pen = CreatePen(PS_SOLID, 1, C_ACCENT_HI);
        HPEN op = (HPEN) SelectObject(dc, pen);
        HBRUSH ob = (HBRUSH) SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, S(14), S(14));
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(pen);
    }
}

/* --- volume slider ---------------------------------------------------------------- */

static void slider_notify(HWND h)
{
    SendMessageA(GetParent(h), WM_HSCROLL, 0, (LPARAM) h);
}

static void slider_from_x(HWND h, int x)
{
    RECT rc;
    int r, pos;
    GetClientRect(h, &rc);
    r = S(9);
    pos = MulDiv(x - r, 100, rc.right - 2 * r);
    pos = pos < 0 ? 0 : pos > 100 ? 100 : pos;
    SetWindowLongPtrA(h, GWLP_USERDATA, pos);
    InvalidateRect(h, NULL, FALSE);
    slider_notify(h);
}

static LRESULT CALLBACK slider_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    int pos = (int) GetWindowLongPtrA(h, GWLP_USERDATA);
    switch (m) {
    case SLM_SETPOS:
        pos = (int) wp;
        SetWindowLongPtrA(h, GWLP_USERDATA, pos < 0 ? 0 : pos > 100 ? 100 : pos);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case SLM_GETPOS:
        return pos;
    case WM_ERASEBKGND:
        return 1;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_PRINTCLIENT:
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = m == WM_PRINTCLIENT ? (HDC)wp : BeginPaint(h, &ps);
        RECT rc, track;
        int r = S(9), cy, kx;
        GetClientRect(h, &rc);
        fill(dc, rc, C_CARD);
        cy = (rc.top + rc.bottom) / 2;
        kx = r + MulDiv(rc.right - 2 * r, pos, 100);
        track.left = r;
        track.right = rc.right - r;
        track.top = cy - S(3);
        track.bottom = cy + S(3);
        round_rect(dc, track, S(3), C_TRACK, C_TRACK, 0);
        track.right = kx;
        if (track.right > track.left) {
            round_rect(dc, track, S(3), C_ACCENT, C_ACCENT, 0);
        }
        dot(dc, kx, cy, r, GetFocus() == h ? C_ACCENT_HI : C_ACCENT);
        dot(dc, kx, cy, r - S(2), C_WHITE);
        if (m != WM_PRINTCLIENT) EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(h);
        SetCapture(h);
        slider_from_x(h, (short) LOWORD(lp));
        return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == h) {
            slider_from_x(h, (short) LOWORD(lp));
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == h) {
            ReleaseCapture();
        }
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_KEYDOWN: {
        int np = pos;
        if (wp == VK_LEFT || wp == VK_DOWN) {
            np = pos - 5;
        } else if (wp == VK_RIGHT || wp == VK_UP) {
            np = pos + 5;
        } else if (wp == VK_HOME) {
            np = 0;
        } else if (wp == VK_END) {
            np = 100;
        } else {
            break;
        }
        SetWindowLongPtrA(h, GWLP_USERDATA, np < 0 ? 0 : np > 100 ? 100 : np);
        InvalidateRect(h, NULL, FALSE);
        slider_notify(h);
        return 0;
    }
    }
    return DefWindowProcA(h, m, wp, lp);
}

/* --- controls -------------------------------------------------------------------- */

static HWND make_in(HWND parent, const char* cls, const char* label, DWORD style, int id, int card, int kind,
                    wchar_t icon)
{
    HWND c = CreateWindowExA(0, cls, label, WS_CHILD | style, 0, 0, 10, 10, parent, (HMENU) (INT_PTR) id, app, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM) ui_font, TRUE);
    INFO(id).kind = kind;
    INFO(id).card = card;
    INFO(id).icon = icon;
    return c;
}

static void button(const char* label, int id, int card, int kind, wchar_t icon)
{
    make_in(content_wnd, "BUTTON", label, BS_OWNERDRAW | WS_TABSTOP, id, card, kind, icon);
}

static void edit(int id, int card)
{
    make_in(content_wnd, "EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, id, card, K_NONE, 0);
}

static void create_fonts(void)
{
    HFONT* fonts[] = { &ui_font, &bold_font, &font_semi, &font_head, &font_title, &font_small,
                       &font_icon, &font_icon_lg, &font_play };
    int i;
    for (i = 0; i < (int) (sizeof(fonts) / sizeof(fonts[0])); i++) {
        if (*fonts[i] != NULL) {
            DeleteObject(*fonts[i]);
            *fonts[i] = NULL;
        }
    }
    ui_font = CreateFontA(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    bold_font = CreateFontA(-S(13), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    font_semi = CreateFontA(-S(13), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                            "Segoe UI");
    font_head = CreateFontA(-S(16), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    font_title = CreateFontA(-S(22), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                             "Segoe UI");
    font_small = CreateFontA(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                             "Segoe UI");
    font_play = CreateFontA(-S(16), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                            "Segoe UI");
    font_icon = CreateFontA(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                            "Segoe MDL2 Assets");
    font_icon_lg = CreateFontA(-S(19), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                               "Segoe MDL2 Assets");
    if (font_icon_sm != NULL) {
        DeleteObject(font_icon_sm);
    }
    font_icon_sm = CreateFontA(-S(10), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                               "Segoe MDL2 Assets");
    {
        /* the icon font ships with Windows 10 and 11; without it the
         * glyphs are skipped rather than drawn as boxes */
        HDC dc = GetDC(NULL);
        char face[LF_FACESIZE];
        HFONT of = (HFONT) SelectObject(dc, font_icon);
        GetTextFaceA(dc, sizeof(face), face);
        have_icon_font = _stricmp(face, "Segoe MDL2 Assets") == 0;
        SelectObject(dc, of);
        ReleaseDC(NULL, dc);
    }
}

static BOOL CALLBACK apply_font(HWND h, LPARAM lp)
{
    SendMessageA(h, WM_SETFONT, (WPARAM) lp, TRUE);
    return TRUE;
}


static void layout_card(int c, RECT rc);
static void set_page(int p);
static void place(int id, int x, int y, int w, int h)
{
    SetWindowPos(ctl(id), NULL, x, y, w, h,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
}

/* an edit box (or the mod list) inside a painted frame */
static void place_input(int id, int x, int y, int w, int h)
{
    struct ctlinfo* ci = &INFO(id);
    ci->framed = 1;
    ci->frame.left = x;
    ci->frame.top = y;
    ci->frame.right = x + w;
    ci->frame.bottom = y + h;
    {
        int eh = S(18);
        place(id, x + S(12), y + (h - eh) / 2, w - S(24), eh);
    }
}

static void layout_content(void)
{
    RECT rc;
    int cw, ch, pad = S(24), gap = S(16), y, r, i, id, pass;
    unsigned mask = page_cards[page];
    SCROLLINFO si;
    if (content_wnd == NULL || in_layout) {
        return;
    }
    in_layout = 1;
    for (pass = 0; pass < 2; pass++) {
        GetClientRect(content_wnd, &rc);
        cw = rc.right;
        ch = rc.bottom;
        for (id = 100; id < IDC_LAST; id++) {
            INFO(id).framed = 0;
        }
        for (i = 0; i < CARD_COUNT; i++) {
            SetRectEmpty(&card_rc[i]);
        }
        y = pad - scroll_y;
        for (r = 0; r < (int) (sizeof(card_rows) / sizeof(card_rows[0])); r++) {
            int cards[2], n = 0, rowh = 0, k, cx = pad, each;
            for (k = 0; k < 2; k++) {
                int c = card_rows[r][k];
                if (c >= 0 && (mask & (1u << c))) {
                    cards[n++] = c;
                    if (S(card_info[c].h) > rowh) {
                        rowh = S(card_info[c].h);
                    }
                }
            }
            if (n == 0) {
                continue;
            }
            each = (cw - 2 * pad - (n - 1) * gap) / n;
            for (k = 0; k < n; k++) {
                RECT cr;
                cr.left = cx;
                cr.top = y;
                cr.right = cx + each;
                cr.bottom = y + rowh;
                card_rc[cards[k]] = cr;
                layout_card(cards[k], cr);
                cx += each + gap;
            }
            y += rowh + gap;
        }
        content_total = y + scroll_y - gap + pad;
        /* controls of the cards that are not on this page */
        for (id = 100; id < IDC_LAST; id++) {
            int c = INFO(id).card;
            if (c >= 0 && !(mask & (1u << c))) {
                ShowWindow(ctl(id), SW_HIDE);
            }
        }
        {
            int max = content_total - ch;
            int ns = max < 0 ? 0 : scroll_y > max ? max : scroll_y;
            if (ns == scroll_y) {
                break;
            }
            scroll_y = ns;
        }
    }
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = content_total - 1;
    si.nPage = (UINT) ch;
    si.nPos = scroll_y;
    SetScrollInfo(content_wnd, SB_VERT, &si, TRUE);
    in_layout = 0;
    /* Moving siblings can copy pixels from their old overlapping positions.
     * Repaint every child at its final position, including newly exposed areas. */
    RedrawWindow(content_wnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static void layout_main(void)
{
    RECT rc;
    int W, H, side = S(200), header = S(64), footer = S(76), i;
    if (main_wnd == NULL) {
        return;
    }
    GetClientRect(main_wnd, &rc);
    W = rc.right;
    H = rc.bottom;
    for (i = 0; i < PAGE_COUNT; i++) {
        place(IDC_NAV_FIRST + i, S(12), S(76) + i * S(48), side - S(24), S(42));
    }
    place(IDC_PLAY, side + S(24), H - footer + S(14), S(150), S(48));
    if (content_wnd != NULL) {
        SetWindowPos(content_wnd, NULL, side, header, W - side, H - header - footer,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        layout_content();
    }
    InvalidateRect(main_wnd, NULL, FALSE);
}

static void set_page(int p)
{
    int i;
    page = p;
    scroll_y = 0;
    for (i = 0; i < PAGE_COUNT; i++) {
        INFO(IDC_NAV_FIRST + i).state = (i == p);
        InvalidateRect(ctl(IDC_NAV_FIRST + i), NULL, FALSE);
    }
    layout_content();
}

static void scroll_to(int y)
{
    int max = content_total - (int) (S(1)) * 0;
    RECT rc;
    GetClientRect(content_wnd, &rc);
    max = content_total - rc.bottom;
    if (max < 0) {
        max = 0;
    }
    y = y < 0 ? 0 : y > max ? max : y;
    if (y != scroll_y) {
        scroll_y = y;
        layout_content();
    }
}

/* Conker page layout, state and actions. */

#include "pages.inc"
static void paint_content(HWND h, HDC target)
{
    PAINTSTRUCT ps;
    HDC wdc = target ? target : BeginPaint(h, &ps);
    RECT rc;
    HDC dc;
    HBITMAP bmp, ob;
    int c, id;
    HWND focus = GetFocus();
    GetClientRect(h, &rc);
    dc = CreateCompatibleDC(wdc);
    bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    ob = (HBITMAP) SelectObject(dc, bmp);
    fill(dc, rc, C_BG);
    for (c = 0; c < CARD_COUNT; c++) {
        if (IsRectEmpty(&card_rc[c])) {
            continue;
        }
        round_rect(dc, card_rc[c], S(12), C_CARD, C_CARD_EDGE, 0);
        paint_card_body(dc, c, card_rc[c]);
    }
    for (id = 100; id < IDC_LAST; id++) {
        if (INFO(id).framed) {
            round_rect(dc, INFO(id).frame, S(8), C_INPUT, focus == ctl(id) ? C_ACCENT : C_INPUT_EDGE, 0);
        }
    }
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
    if (!target) EndPaint(h, &ps);
}

static void paint_main(HWND h, HDC target)
{
    PAINTSTRUCT ps;
    HDC wdc = target ? target : BeginPaint(h, &ps);
    RECT rc, r;
    HDC dc;
    HBITMAP bmp, ob;
    int side = S(200), header = S(64), footer = S(76), tw;
    COLORREF pill_c;
    char pill[64];
    GetClientRect(h, &rc);
    dc = CreateCompatibleDC(wdc);
    bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    ob = (HBITMAP) SelectObject(dc, bmp);
    fill(dc, rc, C_BG);
    r = rc;
    r.right = side;
    fill(dc, r, C_SIDEBAR);
    /* sidebar: a small mark above the pages */
    {
        RECT m;
        m.left = S(20);
        m.top = S(22);
        m.right = S(20) + S(32);
        m.bottom = S(22) + S(32);
        round_rect(dc, m, S(8), C_ACCENT, C_ACCENT, 0);
        text(dc, font_head, C_WHITE, m.left, m.top, S(32), S(32), "C", DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        text(dc, font_semi, C_TEXT, m.right + S(10), m.top, side - m.right - S(12), S(32), "ConkerRecomp",
             DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    }
    /* header */
    tw = text_width(dc, font_title, PC_PORT_NAME);
    text(dc, font_title, C_TEXT, side + S(24), 0, tw + S(4), header, PC_PORT_NAME, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    text(dc, ui_font, C_TEXT_DIM, side + S(24) + tw + S(10), S(2), S(200), header, PC_PORT_VERSION,
         DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    pill_state(&pill_c, pill, sizeof(pill));
    tw = text_width(dc, font_semi, pill) + S(50);
    r.left = rc.right - S(24) - tw;
    r.right = rc.right - S(24);
    r.top = S(14);
    r.bottom = S(50);
    round_rect(dc, r, S(10), C_CARD, C_CARD_EDGE, 0);
    dot(dc, r.left + S(20), (r.top + r.bottom) / 2, S(5), pill_c);
    text(dc, font_semi, C_TEXT, r.left + S(34), r.top, tw - S(40), r.bottom - r.top, pill,
         DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    /* footer: the status line right of Play */
    text(dc, ui_font, C_TEXT_DIM, side + S(190), rc.bottom - footer + S(14), rc.right - S(24) - side - S(190), S(48),
         labels[IDC_STATUS - 100], DT_RIGHT | DT_WORDBREAK | DT_END_ELLIPSIS);
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
    if (!target) EndPaint(h, &ps);
}



#ifdef CONKER_LAUNCHER_TEST
#include "smoke.inc"
#endif
static LRESULT CALLBACK content_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        on_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_DRAWITEM:
        draw_button((DRAWITEMSTRUCT*) lp);
        return TRUE;
    case WM_HSCROLL:
        if ((HWND) lp == ctl(IDC_VOLUME)) {
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC) wp, C_TEXT);
        SetBkColor((HDC) wp, C_INPUT);
        return (LRESULT) br_input;
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:
        paint_content(h, (HDC)wp);
        return 0;
    case WM_PAINT:
        paint_content(h, NULL);
        return 0;
    case WM_SIZE:
        layout_content();
        return 0;
    case WM_VSCROLL: {
        RECT rc;
        int line = S(40);
        GetClientRect(h, &rc);
        switch (LOWORD(wp)) {
        case SB_LINEUP:
            scroll_to(scroll_y - line);
            break;
        case SB_LINEDOWN:
            scroll_to(scroll_y + line);
            break;
        case SB_PAGEUP:
            scroll_to(scroll_y - rc.bottom);
            break;
        case SB_PAGEDOWN:
            scroll_to(scroll_y + rc.bottom);
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si;
            memset(&si, 0, sizeof(si));
            si.cbSize = sizeof(si);
            si.fMask = SIF_TRACKPOS;
            GetScrollInfo(h, SB_VERT, &si);
            scroll_to(si.nTrackPos);
            break;
        }
        case SB_TOP:
            scroll_to(0);
            break;
        case SB_BOTTOM:
            scroll_to(content_total);
            break;
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        scroll_to(scroll_y - GET_WHEEL_DELTA_WPARAM(wp) * S(48) / WHEEL_DELTA);
        return 0;
    case WM_SETFOCUS:
        /* clicks on the background take the focus away from an edit */
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        on_command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_DRAWITEM:
        draw_button((DRAWITEMSTRUCT*) lp);
        return TRUE;
    case WM_TIMER:
        if (wp == TIMER_LOGO) { KillTimer(h, TIMER_LOGO); load_logo(); return 0; }
#ifdef CONKER_LAUNCHER_TEST
        if (wp == 99) { smoke_tick(); return 0; }
#endif
        if (wp == TIMER_PROCESS && child) {
            if (WaitForSingleObject(child, 0) == WAIT_OBJECT_0) child_finished();
            else if (child_kind) read_task(0);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:
        paint_main(h, (HDC)wp);
        return 0;
    case WM_PAINT:
        paint_main(h, NULL);
        return 0;
    case WM_SIZE:
        layout_main();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*) lp;
        mm->ptMinTrackSize.x = S(900);
        mm->ptMinTrackSize.y = S(600);
        return 0;
    }
    case WM_DPICHANGED: {
        RECT* r = (RECT*) lp;
        dpi = HIWORD(wp);
        create_fonts();
        EnumChildWindows(content_wnd, apply_font, (LPARAM) ui_font);
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout_main();
        return 0;
    }
    case WM_CLOSE:
        save_settings();
        if (banner) { DeleteObject(banner); banner = NULL; }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSA wc;
    INITCOMMONCONTROLSEX icc;
    RECT r;
    MSG msg;
    char* slash;
    HMODULE user32 = GetModuleHandleA("user32.dll");
    (void) prev;
    (void) cmdline;

    app = inst;
#ifndef CONKER_LAUNCHER_TEST
    instance_mutex = CreateMutexA(NULL, FALSE, "Local\\ConkerRecompLauncher");
    if (!instance_mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowA("ConkerLauncher", NULL);
        if (existing) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); }
        CloseHandle(instance_mutex); return 0;
    }
#endif
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
    slash = strrchr(exe_dir, '\\');
    if (slash != NULL) {
        *slash = '\0';
    }
    if (!init_local_paths()) return 1;

    {
        /* per-monitor DPI on Windows 10 1703+, system DPI before that */
        typedef BOOL(WINAPI * set_ctx_t)(HANDLE);
        typedef BOOL(WINAPI * set_aware_t)(void);
        set_ctx_t set_ctx = (set_ctx_t) GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        set_aware_t set_aware = (set_aware_t) GetProcAddress(user32, "SetProcessDPIAware");
        if (set_ctx != NULL) {
            set_ctx((HANDLE) (INT_PTR) -4); /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        } else if (set_aware != NULL) {
            set_aware();
        }
    }

    CoInitialize(NULL);
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = "ConkerLauncher";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);
    wc.lpfnWndProc = content_proc;
    wc.lpszClassName = "ConkerContent";
    wc.style = 0;
    RegisterClassA(&wc);
    wc.lpfnWndProc = slider_proc;
    wc.lpszClassName = "ConkerSlider";
    RegisterClassA(&wc);

    main_wnd = CreateWindowExA(0, "ConkerLauncher", APP_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                               CW_USEDEFAULT, 100, 100, NULL, NULL, inst, NULL);
    {
        typedef UINT(WINAPI * get_dpi_t)(HWND);
        get_dpi_t get_dpi = (get_dpi_t) GetProcAddress(user32, "GetDpiForWindow");
        if (get_dpi != NULL) {
            dpi = (int) get_dpi(main_wnd);
        } else {
            HDC dc = GetDC(NULL);
            dpi = GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(NULL, dc);
        }
        if (dpi < 96) {
            dpi = 96;
        }
    }
    {
        /* dark title bar (Windows 10 1809+; the attribute id changed in 20H1) */
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        if (dwm != NULL) {
            typedef HRESULT(WINAPI * set_attr_t)(HWND, DWORD, LPCVOID, DWORD);
            set_attr_t set_attr = (set_attr_t) GetProcAddress(dwm, "DwmSetWindowAttribute");
            BOOL on = TRUE;
            if (set_attr != NULL && set_attr(main_wnd, 20, &on, sizeof(on)) != S_OK) {
                set_attr(main_wnd, 19, &on, sizeof(on));
            }
        }
    }
    create_fonts();
    br_input = CreateSolidBrush(C_INPUT);
    content_wnd = CreateWindowExA(WS_EX_CONTROLPARENT, "ConkerContent", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                                  0, 0, 10, 10, main_wnd, NULL, inst, NULL);
    build_ui();
    load_settings();
    set_page(PAGE_SETUP);

    r.left = 0;
    r.top = 0;
    r.right = S(1100);
    r.bottom = S(900);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    {
        RECT work;
        int w = r.right - r.left, hgt = r.bottom - r.top;
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
        if (w > work.right - work.left) {
            w = work.right - work.left;
        }
        if (hgt > work.bottom - work.top) {
            hgt = work.bottom - work.top;
        }
        SetWindowPos(main_wnd, NULL, work.left + (work.right - work.left - w) / 2,
                     work.top + (work.bottom - work.top - hgt) / 2, w, hgt, SWP_NOZORDER);
    }
    layout_main();
    ShowWindow(main_wnd, show);
#ifdef CONKER_LAUNCHER_TEST
    if (strstr(cmdline, "--smoke-test") || strstr(cmdline, "--setup-test")) {
        if (strstr(cmdline, "--setup-test")) {
            int argc; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            if (argv && argc == 3)
                WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, smoke_image, sizeof(smoke_image), NULL, NULL);
            if (argv) LocalFree(argv);
        }
        SetTimer(main_wnd, 99, 200, NULL);
    }
#endif

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(main_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    if (child != NULL) {
        CloseHandle(child);
    }
    if (instance_mutex) CloseHandle(instance_mutex);
    CoUninitialize();
    return 0;
}
