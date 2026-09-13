/* Compiled only for the standalone UI test build. Captures this application's
 * own pages and checks its initial control state. Never launches the game. */
static void smoke_render(HWND window, HDC dc)
{
    SendMessageA(window, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT | PRF_ERASEBKGND);
    for (HWND child_window = GetWindow(window, GW_CHILD); child_window;
         child_window = GetWindow(child_window, GW_HWNDNEXT)) {
        if (!(GetWindowLongA(child_window, GWL_STYLE) & WS_VISIBLE)) continue;
        RECT r; GetWindowRect(child_window, &r);
        MapWindowPoints(NULL, window, (POINT *)&r, 2);
        int saved = SaveDC(dc);
        POINT origin; GetViewportOrgEx(dc, &origin);
        SetViewportOrgEx(dc, origin.x + r.left, origin.y + r.top, NULL);
        IntersectClipRect(dc, 0, 0, r.right - r.left, r.bottom - r.top);
        smoke_render(child_window, dc);
        RestoreDC(dc, saved);
    }
}
static char smoke_image[MAX_PATH], smoke_failures[256];
static void smoke_failure(int line, int *failures)
{
    ++*failures;
    size_t length = strlen(smoke_failures);
    snprintf(smoke_failures + length, sizeof(smoke_failures) - length, "%d ", line);
}
static int smoke_phase;
static void smoke_tick(void)
{
    static int step, polls, failures;
    if (step == PAGE_COUNT) {
        char path[MAX_PATH]; join(path, sizeof(path), exe_dir, "synthetic verify.iso");
        FILE *f = fopen(path, "wb");
        if (f) { fputs("Synthetic launcher verification fixture. No game data.", f); fclose(f); }
        set_text(IDC_ISO_EDIT, smoke_image[0] ? smoke_image : path);
        run_task(1); ++step; return;
    }
    if (step > PAGE_COUNT) {
        if (child && ++polls < 9000) return;
        if (smoke_image[0]) {
            if (!smoke_phase) {
                if (child || iso_verified != 2 || !IsWindowEnabled(ctl(IDC_EXTRACT)) || !banner) smoke_failure(__LINE__, &failures);
                else { smoke_phase = 1; polls = 0; run_task(3); return; }
            } else if (child || !data_valid() || IsWindowEnabled(ctl(IDC_PLAY)) ||
                       !strstr(labels[IDC_BUILD_STATUS - 100], "sources generated")) smoke_failure(__LINE__, &failures);
        } else if (child || iso_verified != -1 || !strstr(labels[IDC_ISO_HASH - 100], "SHA-1:") || banner) smoke_failure(__LINE__, &failures);
        char path[MAX_PATH]; join(path, sizeof(path), exe_dir, "smoke-result.txt");
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "failures=%d\nchecks=%s\nverification=%s\ndetail=%s\n", failures, smoke_failures,
                 labels[IDC_ISO_STATUS - 100], labels[IDC_ISO_HASH - 100]); fclose(f); }
        PostMessageA(main_wnd, WM_CLOSE, 0, 0); KillTimer(main_wnd, 99); return;
    }
    if (step == 0) {
        const char *args[] = {"launcher.exe", "path with spaces & symbols", "trailing\\", "quote\"inside", "", "caf\xc3\xa9"};
        char command[1024] = ""; wchar_t wide[1024]; int argc;
        for (unsigned i = 0; i < sizeof(args)/sizeof(args[0]); ++i)
            if (!append_arg(command, sizeof(command), args[i])) smoke_failure(__LINE__, &failures);
        MultiByteToWideChar(CP_UTF8, 0, command, -1, wide, 1024);
        wchar_t **argv = CommandLineToArgvW(wide, &argc);
        if (!argv || argc != 6) smoke_failure(__LINE__, &failures);
        else for (int i = 0; i < argc; ++i) {
            wchar_t expected[128]; MultiByteToWideChar(CP_UTF8, 0, args[i], -1, expected, 128);
            if (wcscmp(argv[i], expected)) smoke_failure(__LINE__, &failures);
        }
        if (argv) LocalFree(argv);
        if (IsWindowEnabled(ctl(IDC_EXTRACT))) smoke_failure(__LINE__, &failures);
        char nearby[MAX_PATH], source[MAX_PATH];
        find_source_dir(nearby, sizeof(nearby)); get_text(IDC_SRC_EDIT, source, sizeof(source));
        if (_stricmp(nearby, source)) smoke_failure(__LINE__, &failures);
        SetWindowPos(main_wnd, NULL, -30000, -30000, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        // A scroll must invalidate children, not just their painted parent.
        scroll_to(S(96));
        if (!GetUpdateRect(ctl(IDC_ISO_VERIFY), NULL, FALSE)) smoke_failure(__LINE__, &failures);
        for (int n = 0; n < 40; ++n) { scroll_to(S(48 * (n % 10))); UpdateWindow(content_wnd); }
        scroll_to(0);
        // A helper that exits with only a stale progress file is an error.
        WritePrivateProfileStringA("task", "state", "working", result_path);
        WritePrivateProfileStringA("task", "message", "Reading disc: 43%", result_path);
        // Real helpers write UTF-16; the test fixture does the same.
        FILE *snapshot = fopen(result_path, "wb");
        const wchar_t stale[] = L"\xfeff[task]\nstate=working\nmessage=Reading disc: 43%\n";
        if (snapshot) { fwrite(stale, sizeof(wchar_t), wcslen(stale), snapshot); fclose(snapshot); }
        child_kind = 1; child_exit_code = 1; read_task(1);
        if (iso_verified != -1 || !strstr(labels[IDC_ISO_STATUS - 100], "stopped before completing")) smoke_failure(__LINE__, &failures);
        iso_verified = 0; set_text(IDC_ISO_STATUS, "Choose an image to calculate its SHA-1.");
    }
    set_page(step);
    RedrawWindow(main_wnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    RECT rc; GetClientRect(main_wnd, &rc);
    HDC window = GetDC(main_wnd), memory = CreateCompatibleDC(window);
    BITMAPINFO bi = {0}; void *pixels;
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = rc.right;
    bi.bmiHeader.biHeight = -rc.bottom; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    HBITMAP bitmap = CreateDIBSection(window, &bi, DIB_RGB_COLORS, &pixels, NULL, 0);
    HBITMAP old = SelectObject(memory, bitmap);
    smoke_render(main_wnd, memory);
    BITMAPFILEHEADER header = {0}; header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + rc.right * rc.bottom * 4;
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s\\page-%d.bmp", exe_dir, step);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(&header, sizeof(header), 1, f); fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
        fwrite(pixels, rc.right * rc.bottom * 4, 1, f); fclose(f); }
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(main_wnd, window);
    if (step == 0) {
        snprintf(path, sizeof(path), "%s\\controls.txt", exe_dir); f = fopen(path, "w");
        if (f) {
            fprintf(f, "play=%d prepare=%d volume=%d artwork=%d\n", IsWindowEnabled(ctl(IDC_PLAY)),
                    IsWindowEnabled(ctl(IDC_EXTRACT)), volume_get(), banner != NULL); fclose(f);
        }
    }
    ++step;
}
