/* ConkerRecomp launcher controls and page content. */
static void clear_logo(void)
{
    if (banner) { DeleteObject(banner); banner = NULL; }
    if (content_wnd) InvalidateRect(content_wnd, NULL, FALSE);
}
static void load_logo(void)
{
    char path[MAX_PATH]; wchar_t wide[MAX_PATH];
    clear_logo();
    get_text(IDC_ISO_EDIT, path, sizeof(path));
    if (path[0] && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, MAX_PATH))
        banner = conker_disc_title_art(wide, C_INPUT);
    InvalidateRect(content_wnd, NULL, FALSE);
}
static void refresh_state(void)
{
    if (loading_settings || !content_wnd) return;
    char source[MAX_PATH], exe[MAX_PATH], image[MAX_PATH], save[MAX_PATH], data[MAX_PATH];
    get_text(IDC_SRC_EDIT, source, sizeof(source)); get_text(IDC_ISO_EDIT, image, sizeof(image));
    get_text(IDC_DATA_EDIT, data, sizeof(data)); join(save, sizeof(save), data, "save");
    set_text(IDC_SAVES_EDIT, data[0] ? save : "Select your local game-data folder first.");
    EnableWindow(ctl(IDC_PLAY), !child && find_game_exe(exe, sizeof(exe)) && data_valid());
    EnableWindow(ctl(IDC_BUILD), !child && can_rebuild());
    EnableWindow(ctl(IDC_ISO_VERIFY), !child && source_valid(source) && file_exists(image));
    EnableWindow(ctl(IDC_EXTRACT), !child && iso_verified > 0 && source_valid(source) && file_exists(image));
    EnableWindow(ctl(IDC_SAVES_OPEN), data_valid());
    int inputs[] = {IDC_SRC_EDIT, IDC_SRC_BROWSE, IDC_DATA_EDIT, IDC_DATA_BROWSE,
                    IDC_ISO_EDIT, IDC_ISO_BROWSE, IDC_VOLUME, IDC_MUTE};
    for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) EnableWindow(ctl(inputs[i]), !child);
    if (!child && !labels[IDC_BUILD_STATUS - 100][0]) set_text(IDC_BUILD_STATUS, find_game_exe(exe, sizeof(exe)) ?
        "Local game build found. Play uses the selected prepared game files." :
        "Import your disc to generate C sources, then build the game on this PC.");
    InvalidateRect(main_wnd, NULL, FALSE);
}
static void build_ui(void)
{
    for (int i = 0; i < PAGE_COUNT; ++i)
        make_in(main_wnd, "BUTTON", nav_items[i].name, BS_OWNERDRAW | WS_TABSTOP,
                IDC_NAV_FIRST + i, -1, K_NAV, nav_items[i].icon);
    make_in(main_wnd, "BUTTON", "Play", BS_OWNERDRAW | WS_TABSTOP, IDC_PLAY, -1, K_PRIMARY, 0xE768);
    edit(IDC_ISO_EDIT, CARD_GAME); button("Browse...", IDC_ISO_BROWSE, CARD_GAME, K_SECONDARY, 0xF12B);
    button("Verify SHA-1", IDC_ISO_VERIFY, CARD_GAME, K_SECONDARY, 0);
    edit(IDC_DATA_EDIT, CARD_GAME); button("Browse...", IDC_DATA_BROWSE, CARD_GAME, K_SECONDARY, 0xF12B);
    button("Import & generate", IDC_EXTRACT, CARD_GAME, K_SECONDARY, 0);
    edit(IDC_SRC_EDIT, CARD_BUILD); button("Browse...", IDC_SRC_BROWSE, CARD_BUILD, K_SECONDARY, 0xF12B);
    button("Build local game", IDC_BUILD, CARD_BUILD, K_PRIMARY, 0);
    button("Task log", IDC_LOG_OPEN, CARD_BUILD, K_SECONDARY, 0);
    make_in(content_wnd, "ConkerSlider", "", WS_TABSTOP, IDC_VOLUME, CARD_AUDIO, K_NONE, 0);
    button("Mute", IDC_MUTE, CARD_AUDIO, K_TOGGLE, 0);
    button("Refresh controllers", IDC_ADAPTER, CARD_CONTROLS, K_SECONDARY, 0);
    edit(IDC_SAVES_EDIT, CARD_SAVES); SendMessageA(ctl(IDC_SAVES_EDIT), EM_SETREADONLY, TRUE, 0);
    button("Open save folder", IDC_SAVES_OPEN, CARD_SAVES, K_SECONDARY, 0xED25);
    button("Open local logs", IDC_EXTRACT_OPEN, CARD_ADVANCED, K_SECONDARY, 0xED25);
    button("Project repository", IDC_MODS_OPEN, CARD_ABOUT, K_SECONDARY, 0);
    int edits[] = {IDC_ISO_EDIT, IDC_DATA_EDIT, IDC_SRC_EDIT};
    for (unsigned i = 0; i < sizeof(edits)/sizeof(edits[0]); ++i)
        SendMessageA(ctl(edits[i]), EM_SETLIMITTEXT, MAX_PATH - 16, 0);
}
static void layout_card(int c, RECT rc)
{
    int x = rc.left + S(20), y = rc.top + S(20), right = rc.right - S(20), w = right - x, row = y + S(44);
    switch (c) {
    case CARD_GAME: {
        int tx = x + S(232), bw = S(110);
        place_input(IDC_ISO_EDIT, tx, y + S(64), right - bw - S(10) - tx, S(34));
        place(IDC_ISO_BROWSE, right - bw, y + S(64), bw, S(34));
        place(IDC_ISO_VERIFY, right - bw, y + S(108), bw, S(30));
        place_input(IDC_DATA_EDIT, x, y + S(216), w - S(120), S(34));
        place(IDC_DATA_BROWSE, right - bw, y + S(216), bw, S(34));
        place(IDC_EXTRACT, x, y + S(262), S(152), S(32)); break;
    }
    case CARD_BUILD:
        place_input(IDC_SRC_EDIT, x + S(100), row, w - S(220), S(34));
        place(IDC_SRC_BROWSE, right - S(110), row, S(110), S(34));
        place(IDC_BUILD, right - S(180), row + S(52), S(180), S(40));
        place(IDC_LOG_OPEN, right - S(180), row + S(102), S(180), S(30)); break;
    case CARD_AUDIO:
        place(IDC_VOLUME, x + S(72), row, w - S(126), S(32));
        place(IDC_MUTE, x, row + S(42), w, S(24)); break;
    case CARD_CONTROLS:
        place(IDC_ADAPTER, right - S(168), row, S(168), S(32)); break;
    case CARD_SAVES:
        place_input(IDC_SAVES_EDIT, x, row, w, S(32));
        place(IDC_SAVES_OPEN, x, row + S(44), S(164), S(32)); break;
    case CARD_ADVANCED:
        place(IDC_EXTRACT_OPEN, x, row + S(38), S(164), S(32)); break;
    case CARD_ABOUT:
        place(IDC_MODS_OPEN, x, row + S(96), S(172), S(32)); break;
    }
}
static void pill_state(COLORREF *color, char *out, int size)
{
    char exe[MAX_PATH];
    *color = C_ORANGE;
    if (child) {
        *color = C_ACCENT_HI;
        snprintf(out, size, "%s", child_kind == 0 ? "Game running" : child_kind == 1 ? "Verifying disc" : child_kind == 3 ? "Preparing locally" : "Building");
    } else if (find_game_exe(exe, sizeof(exe)) && data_valid()) {
        *color = C_GREEN; snprintf(out, size, "Local build ready");
    } else snprintf(out, size, "Setup needed");
}
static void paint_card_body(HDC dc, int c, RECT rc)
{
    int x = rc.left + S(20), y = rc.top + S(20), right = rc.right - S(20), w = right - x, row = y + S(44);
    if (card_info[c].icon) {
        glyph(dc, font_icon_lg, C_TEXT, x, y, S(28), S(28), card_info[c].icon);
        text(dc, font_head, C_TEXT, x + S(38), y, w - S(38), S(28), card_info[c].title, DT_SINGLELINE | DT_VCENTER);
    }
    switch (c) {
    case CARD_GAME: {
        RECT tile = {x, y, x + S(208), y + S(168)};
        int tx = x + S(232);
        round_rect(dc, tile, S(10), C_INPUT, C_INPUT_EDGE, 0);
        if (banner) {
            BITMAP bm; GetObject(banner, sizeof(bm), &bm);
            int bw = S(192), bh = MulDiv(bw, bm.bmHeight, bm.bmWidth);
            if (bh > S(148)) { bh = S(148); bw = MulDiv(bh, bm.bmWidth, bm.bmHeight); }
            HDC mem = CreateCompatibleDC(dc); HBITMAP old = SelectObject(mem, banner);
            SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, NULL);
            StretchBlt(dc, (tile.left + tile.right - bw)/2, (tile.top + tile.bottom - bh)/2,
                bw, bh, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(mem, old); DeleteDC(mem);
        } else {
            char selected[MAX_PATH]; get_text(IDC_ISO_EDIT, selected, sizeof(selected));
            text(dc, font_semi, C_TEXT_DIM, x, y, S(208), S(168),
                selected[0] ? "No title artwork" : "Select a disc image", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        text(dc, font_title, C_TEXT, tx, y, right - tx, S(32), "Conker: Live & Recomped", DT_SINGLELINE | DT_END_ELLIPSIS);
        text(dc, font_semi, C_TEXT_DIM, tx, y + S(38), right - tx, S(20), "Your game disc (ISO / XISO)", DT_SINGLELINE);
        text(dc, font_small, iso_verified > 0 ? C_GREEN : C_TEXT_DIM, tx, y + S(108), right - tx - S(125), S(48),
            labels[IDC_ISO_STATUS - 100], DT_WORDBREAK | DT_END_ELLIPSIS);
        text(dc, font_small, C_TEXT_DIM, tx, y + S(158), right - tx, S(34), labels[IDC_ISO_HASH - 100], DT_WORDBREAK);
        text(dc, font_semi, C_TEXT, x, y + S(190), w, S(20), "Local game data (folder containing default.xbe)", DT_SINGLELINE);
        text(dc, font_small, C_TEXT_DIM, x + S(168), y + S(258), w - S(168), S(42),
            "Verifies and extracts your disc, then generates C locally. Choose Build local game when complete.", DT_WORDBREAK);
        break;
    }
    case CARD_BUILD:
        text(dc, ui_font, C_TEXT, x, row, S(96), S(34), "Source folder", DT_SINGLELINE | DT_VCENTER);
        text(dc, font_small, C_TEXT_DIM, x, row + S(52), w - S(200), S(86), labels[IDC_BUILD_STATUS - 100], DT_WORDBREAK);
        break;
    case CARD_DISPLAY:
        text(dc, font_head, C_TEXT, x, row, w, S(30), "640 x 480  /  4:3", DT_SINGLELINE);
        text(dc, font_small, C_TEXT_DIM, x, row + S(42), w, S(60),
            "Original display format for comparison. Resolution options will follow after game validation. Direct3D 11 renderer.", DT_WORDBREAK);
        break;
    case CARD_AUDIO: {
        char b[32]; snprintf(b, sizeof(b), "%d%%", volume_get());
        text(dc, ui_font, C_TEXT, x, row, S(66), S(32), "Volume", DT_SINGLELINE | DT_VCENTER);
        text(dc, ui_font, C_TEXT, right - S(48), row, S(48), S(32), b, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        text(dc, font_small, C_TEXT_DIM, x, row + S(76), w, S(36), "Applied on Play. Game audio timing is unchanged.", DT_WORDBREAK);
        break;
    }
    case CARD_CONTROLS: {
        unsigned count = 0; XINPUT_STATE state;
        for (DWORD i = 0; i < 4; ++i) if (XInputGetState(i, &state) == ERROR_SUCCESS) ++count;
        char b[96]; snprintf(b, sizeof(b), "%u Xbox controller%s connected", count, count == 1 ? "" : "s");
        text(dc, font_semi, C_TEXT, x, row, w - S(180), S(32), b, DT_SINGLELINE | DT_VCENTER);
        text(dc, font_small, C_TEXT_DIM, x, row + S(48), w, S(64),
            "A: skip video / confirm. Left stick or D-pad: navigate the tavern.\nKeyboard: arrow keys navigate, Enter / Space confirm, Escape / Backspace go back. Focus the game window to play.", DT_WORDBREAK);
        break;
    }
    case CARD_ABOUT:
        text(dc, font_small, C_TEXT_DIM, x, row, w, S(88),
            "Conker: Live & Recomped\nLauncher by avak1an.\nGame files and generated code stay on your computer. The game is still in development; campaign and multiplayer need further work.", DT_WORDBREAK);
        break;
    case CARD_ADVANCED:
        text(dc, font_small, C_TEXT_DIM, x, row, w, S(36), "Settings and logs are stored in your local ConkerRecomp app-data folder.", DT_WORDBREAK);
        break;
    }
}
static void load_settings(void)
{
    char source[MAX_PATH], value[MAX_PATH], nearby[MAX_PATH], owner[MAX_PATH]; loading_settings = 1;
    find_source_dir(nearby, sizeof(nearby));
    ini_get("source", source, sizeof(source), "");
    ini_get("source_owner", owner, sizeof(owner), "");
    /* A different checkout must not silently inherit another checkout's tools.
     * Once saved here, an explicitly selected source remains respected. */
    int changed_checkout = nearby[0] && _stricmp(owner, nearby);
    if (changed_checkout || !source_valid(source)) snprintf(source, sizeof(source), "%s", nearby);
    set_text(IDC_SRC_EDIT, source);
    ini_get("data", value, sizeof(value), "");
    if (changed_checkout) value[0] = 0;
    set_text(IDC_DATA_EDIT, value);
    ini_get("iso", value, sizeof(value), ""); set_text(IDC_ISO_EDIT, value);
    int volume = ini_get_int("volume", 100); volume_set(volume < 0 ? 0 : volume > 100 ? 100 : volume);
    toggle_set(IDC_MUTE, ini_get_int("mute", 0));
    set_text(IDC_ISO_STATUS, value[0] ? "Click Verify SHA-1 to check this image." : "Choose an image to calculate its SHA-1.");
    set_status("Choose your disc, verify its identity, then import and generate locally.");
    load_logo(); loading_settings = 0; refresh_state();
}
static void on_command(int id, int code)
{
    char path[MAX_PATH];
    if (id < 100 || id >= IDC_LAST || loading_settings) return;
    if (id >= IDC_NAV_FIRST && id <= IDC_NAV_LAST) { set_page(id - IDC_NAV_FIRST); return; }
    if (!IsWindowEnabled(ctl(id))) return;
    if (code == EN_SETFOCUS || code == EN_KILLFOCUS) { InvalidateRect(content_wnd, NULL, FALSE); return; }
    if (id == IDC_MUTE) { toggle_set(id, !toggle_get(id)); return; }
    switch (id) {
    case IDC_ISO_BROWSE:
        get_text(IDC_ISO_EDIT, path, sizeof(path));
        if (browse_file("Choose your Conker disc image", "Xbox disc images (*.iso;*.xiso)\0*.iso;*.xiso\0All files\0*.*\0", path, sizeof(path))) set_text(IDC_ISO_EDIT, path);
        break;
    case IDC_SRC_BROWSE: case IDC_DATA_BROWSE: {
        int target = id == IDC_SRC_BROWSE ? IDC_SRC_EDIT : IDC_DATA_EDIT;
        get_text(target, path, sizeof(path));
        if (browse_folder(id == IDC_SRC_BROWSE ? "Choose the ConkerRecomp source folder" : "Choose local extracted Conker data", path, sizeof(path))) set_text(target, path);
        break;
    }
    case IDC_ISO_EDIT:
        if (code == EN_CHANGE) { clear_logo(); SetTimer(main_wnd, TIMER_LOGO, 250, NULL); iso_verified = 0; set_text(IDC_ISO_HASH, ""); set_text(IDC_ISO_STATUS, "Click Verify SHA-1 to check this image."); refresh_state(); }
        break;
    case IDC_SRC_EDIT: case IDC_DATA_EDIT:
        if (code == EN_CHANGE) {
            if (id == IDC_SRC_EDIT) {
                iso_verified = 0; set_text(IDC_ISO_HASH, "");
                set_text(IDC_ISO_STATUS, "Verify the image with this checkout's version list.");
                set_text(IDC_BUILD_STATUS, "");
            }
            refresh_state();
        }
        break;
    case IDC_PLAY: play(); break;
    case IDC_BUILD: run_task(2); break;
    case IDC_ISO_VERIFY: run_task(1); break;
    case IDC_EXTRACT: run_task(3); break;
    case IDC_ADAPTER: InvalidateRect(content_wnd, NULL, FALSE); break;
    case IDC_LOG_OPEN: ShellExecuteA(main_wnd, "open", log_path, NULL, NULL, SW_SHOWNORMAL); break;
    case IDC_EXTRACT_OPEN: ShellExecuteA(main_wnd, "open", local_dir, NULL, NULL, SW_SHOWNORMAL); break;
    case IDC_SAVES_OPEN:
        get_text(IDC_SAVES_EDIT, path, sizeof(path));
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) set_status("The game will create its save folder when needed.");
        else ShellExecuteA(main_wnd, "open", path, NULL, NULL, SW_SHOWNORMAL);
        break;
    case IDC_MODS_OPEN: ShellExecuteA(main_wnd, "open", "https://github.com/avak1an/ConkerRecomp", NULL, NULL, SW_SHOWNORMAL); break;
    }
}
