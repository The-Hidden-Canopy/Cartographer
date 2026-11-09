#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "studio_file_picker.hpp"
#include "studio_file_picker_rules.hpp"
#include "studio_file_picker_paths.hpp"
#include "studio_native_chrome.hpp"

#include <dwmapi.h>
#include <shlwapi.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <string>
#include <system_error>
#include <vector>

namespace carto::studio {
namespace {

constexpr wchar_t kClassName[] = L"CartographerOpenFileBrowserV1";
thread_local std::filesystem::path last_selected_directory;
enum Control : int { address = 100, go, up, home, files, filename, filter,
                     system_browser, status, heading, location_label, filename_label, filter_label,
                     back, files_label };

struct Entry { std::filesystem::path path; bool directory = false; };
struct Filter { std::wstring label; std::wstring patterns; };

struct Browser {
    HWND window = nullptr;
    HWND owner = nullptr;
    std::filesystem::path directory;
    std::vector<std::filesystem::path> history;
    std::vector<Entry> entries;
    std::vector<Filter> filters;
    std::wstring prompt = L"OPEN A FILE  /  CARTOGRAPHER";
    std::wstring default_extension;
    bool save = false;
    bool confirm_overwrite = false;
    FilePickerResult result;
    HFONT font = nullptr;
    HBRUSH background = nullptr;
    HBRUSH field = nullptr;
    COLORREF bg = RGB(10, 16, 25), field_bg = RGB(19, 29, 41);
    COLORREF text = RGB(230, 236, 246), muted = RGB(155, 173, 190);
    COLORREF accent = RGB(10, 112, 150), border = RGB(47, 69, 87);
    COLORREF selection_text = RGB(255, 255, 255);
    bool system_high_contrast = false;
    bool controls_ready = true;
    bool done = false;
    UINT dpi = 96;

    int px(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }
    HWND item(int id) const { return GetDlgItem(window, id); }
    bool remote_location(const std::filesystem::path& path) const {
        return file_picker_detail::remote_location(path);
    }

    void set_status(const std::wstring& value) const { SetWindowTextW(item(status), value.c_str()); }
    std::wstring value(int id) const {
        HWND control = item(id);
        const int count = GetWindowTextLengthW(control);
        if (count < 0 || count > 32767) return {};
        std::wstring text_value(static_cast<std::size_t>(count) + 1U, L'\0');
        const int copied = GetWindowTextW(control, text_value.data(), count + 1);
        text_value.resize(static_cast<std::size_t>(std::max(0, copied)));
        return text_value;
    }
    void add_control(const wchar_t* cls, const wchar_t* label, DWORD style, int id,
                     DWORD extended = 0) {
        HWND child = CreateWindowExW(extended, cls, label,
            WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (child != nullptr) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        else controls_ready = false;
    }
    HFONT make_font() const {
        return CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    void layout(int width, int height) const {
        const int m = px(20), gap = px(8), h = px(32), side = px(64);
        const int inner = std::max(px(360), width - 2 * m);
        auto move = [this](int id, int x, int y, int w, int ht) {
            MoveWindow(item(id), x, y, std::max(1, w), std::max(1, ht), TRUE);
        };
        move(heading, m, m, inner, px(28));
        move(location_label, m, m + px(32), inner, px(20));
        const int address_y = m + px(54);
        move(back, m, address_y, side, h);
        move(up, m + side + gap, address_y, side, h);
        move(home, m + 2 * (side + gap), address_y, side, h);
        move(go, width - m - px(58), address_y, px(58), h);
        move(address, m + 3 * (side + gap), address_y,
             inner - 3 * (side + gap) - px(58) - gap, h);
        const int footer_y = height - m - h;
        const int filter_y = footer_y - h - gap;
        const int name_y = filter_y - h - gap;
        const int list_y = address_y + h + px(30);
        const int status_y = name_y - px(25);
        move(files_label, m, list_y - px(22), inner, px(18));
        move(files, m, list_y, inner, std::max(px(60), status_y - list_y - gap));
        move(status, m, status_y, inner, px(20));
        move(filename_label, m, name_y + px(7), px(70), px(20));
        move(filename, m + px(72), name_y, inner - px(72), h);
        move(filter_label, m, filter_y + px(7), px(70), px(20));
        move(filter, m + px(72), filter_y, inner - px(72), px(240));
        move(system_browser, m, footer_y, px(150), h);
        move(IDCANCEL, width - m - px(100), footer_y, px(100), h);
        move(IDOK, width - m - px(208), footer_y, px(100), h);
    }
    bool matches(const std::filesystem::path& path) const {
        if (filters.empty()) return true;
        int index = static_cast<int>(SendMessageW(item(filter), CB_GETCURSEL, 0, 0));
        if (index < 0 || static_cast<std::size_t>(index) >= filters.size()) index = 0;
        const auto name = path.filename().wstring();
        const auto& pattern = filters[static_cast<std::size_t>(index)].patterns;
        return file_picker_detail::matches_filter(name, pattern);
    }
    void populate() {
        entries.clear();
        SendMessageW(item(files), LB_RESETCONTENT, 0, 0);
        SetWindowTextW(item(address), directory.c_str());
        if (!save) SetWindowTextW(item(filename), L"");
        if (remote_location(directory)) {
            set_status(L"Use System browser for network locations."); return;
        }
        std::error_code ec;
        std::filesystem::directory_iterator it(directory, ec), end;
        if (ec) { set_status(L"Cannot open this location. Check the path or use System browser."); return; }
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            std::error_code type_error;
            const bool is_dir = it->is_directory(type_error);
            if (type_error || (!is_dir && !matches(it->path()))) continue;
            entries.push_back({it->path(), is_dir});
            if (entries.size() >= 10000U) break;
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            if (a.directory != b.directory) return a.directory > b.directory;
            return _wcsicmp(a.path.filename().c_str(), b.path.filename().c_str()) < 0;
        });
        for (const auto& entry : entries) {
            const auto label = std::wstring(entry.directory ? L"[Folder]  " : L"[File]       ") +
                entry.path.filename().wstring();
            SendMessageW(item(files), LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        if (ec) set_status(L"Some entries could not be read. Use System browser for more options.");
        else if (entries.size() == 10000U) set_status(L"Showing the first 10,000 entries. Use System browser for more options.");
        else set_status(std::to_wstring(entries.size()) + L" items  |  Double-click a folder to navigate");
    }
    void navigate(const std::filesystem::path& next, bool remember = true) {
        if (remote_location(next)) { set_status(L"Use System browser for network locations."); return; }
        std::error_code ec;
        if (!std::filesystem::is_directory(next, ec) || ec) {
            set_status(L"That folder is unavailable. Check the location and try again."); return;
        }
        const auto normalized = next.lexically_normal();
        if (remember && normalized != directory) history.push_back(directory);
        directory = normalized;
        populate();
    }
    void select_entry(bool activate) {
        const LRESULT selected = SendMessageW(item(files), LB_GETCURSEL, 0, 0);
        if (selected == LB_ERR || static_cast<std::size_t>(selected) >= entries.size()) return;
        const Entry entry = entries[static_cast<std::size_t>(selected)];
        if (entry.directory) {
            if (activate) navigate(entry.path);
            else if (!save) SetWindowTextW(item(filename), L"");
        } else {
            SetWindowTextW(item(filename), entry.path.filename().c_str());
            if (activate) accept();
        }
    }
    void accept() {
        const auto name = value(filename);
        if (name.empty()) { set_status(L"Choose a file or enter a file name."); return; }
        using file_picker_detail::SelectionStatus;
        const auto plan = file_picker_detail::plan_selection(directory,
            std::filesystem::path(name), save, default_extension, confirm_overwrite);
        switch (plan.status) {
        case SelectionStatus::enter_directory: navigate(plan.path); return;
        case SelectionStatus::invalid_name: set_status(L"Use a valid Windows file name or a full path."); return;
        case SelectionStatus::missing_file: set_status(L"That file is unavailable. Check its name or use System browser."); return;
        case SelectionStatus::missing_parent: set_status(L"The destination folder is unavailable. Check the location."); return;
        case SelectionStatus::unavailable: set_status(L"The destination cannot be inspected. Use System browser."); return;
        case SelectionStatus::use_system_browser: set_status(L"Use System browser for network locations."); return;
        case SelectionStatus::confirm_replace:
            if (MessageBoxW(window,
                L"A file with this name already exists. Replace it?",
                L"Confirm replacement", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
            break;
        case SelectionStatus::selected: break;
        }
        result = {FilePickerOutcome::selected, plan.path};
        done = true;
        DestroyWindow(window);
    }
};

LRESULT CALLBACK browser_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    Browser* b = reinterpret_cast<Browser*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        b = static_cast<Browser*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(b));
        b->window = window;
    }
    if (b == nullptr) return DefWindowProcW(window, message, wp, lp);
    try {
    switch (message) {
    case WM_CREATE: {
        apply_native_caption(window, b->system_high_contrast);
        const auto user32 = GetModuleHandleW(L"user32.dll");
        const auto dpi_for_window = user32 == nullptr ? nullptr :
            reinterpret_cast<UINT (WINAPI*)(HWND)>(GetProcAddress(user32, "GetDpiForWindow"));
        b->dpi = dpi_for_window == nullptr ? 96U : dpi_for_window(window);
        if (b->dpi == 0) b->dpi = 96;
        b->font = b->make_font();
        if (b->font == nullptr) return -1;
        b->add_control(L"STATIC", b->prompt.c_str(), 0, heading);
        b->add_control(L"STATIC", L"Location", 0, location_label);
        const DWORD button = WS_TABSTOP | BS_OWNERDRAW;
        b->add_control(L"BUTTON", L"Back", button, back);
        b->add_control(L"BUTTON", L"Up", button, up);
        b->add_control(L"BUTTON", L"Home", button, home);
        b->add_control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, address, WS_EX_CLIENTEDGE);
        b->add_control(L"BUTTON", L"Go", button, go);
        b->add_control(L"STATIC", L"Files and folders", 0, files_label);
        b->add_control(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, files, WS_EX_CLIENTEDGE);
        b->add_control(L"STATIC", L"", 0, status);
        b->add_control(L"STATIC", L"File name", 0, filename_label);
        b->add_control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, filename, WS_EX_CLIENTEDGE);
        b->add_control(L"STATIC", L"File type", 0, filter_label);
        b->add_control(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, filter);
        b->add_control(L"BUTTON", L"System browser...", button, system_browser);
        b->add_control(L"BUTTON", b->save ? L"Save" : L"Open", button | BS_DEFPUSHBUTTON, IDOK);
        b->add_control(L"BUTTON", L"Cancel", button, IDCANCEL);
        if (!b->controls_ready) return -1;
        for (const auto& f : b->filters) SendMessageW(b->item(filter), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(f.label.c_str()));
        SendMessageW(b->item(filter), CB_SETCURSEL, 0, 0);
        b->populate();
        SetFocus(b->item(files));
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = {b->px(620), b->px(480)};
        return 0;
    }
    case WM_SIZE: b->layout(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_DPICHANGED: {
        b->dpi = HIWORD(wp);
        const HFONT next_font = b->make_font();
        if (next_font != nullptr) {
            const HFONT old_font = b->font;
            b->font = next_font;
            for (HWND child = GetWindow(window, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT))
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(b->font), TRUE);
            if (old_font != nullptr) DeleteObject(old_font);
        }
        SendMessageW(b->item(files), LB_SETITEMHEIGHT, 0, b->px(30));
        const auto* bounds = reinterpret_cast<RECT*>(lp);
        SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                     bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_MEASUREITEM: {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        item->itemHeight = static_cast<UINT>(b->px(30)); return TRUE;
    }
    case WM_DRAWITEM: {
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (draw->CtlID == files && draw->itemID == static_cast<UINT>(-1)) return TRUE;
        const bool is_list = draw->CtlID == files;
        const bool selected = (draw->itemState & ODS_SELECTED) != 0;
        const COLORREF fill = selected || draw->CtlID == IDOK ? b->accent : (is_list ? b->field_bg : b->border);
        HBRUSH brush = CreateSolidBrush(fill);
        FillRect(draw->hDC, &draw->rcItem, brush); DeleteObject(brush);
        SetBkMode(draw->hDC, TRANSPARENT);
        const COLORREF foreground = selected || draw->CtlID == IDOK
            ? b->selection_text
            : b->system_high_contrast && !is_list ? GetSysColor(COLOR_BTNTEXT) : b->text;
        SetTextColor(draw->hDC, foreground);
        std::wstring label;
        if (is_list) {
            const LRESULT len = SendMessageW(draw->hwndItem, LB_GETTEXTLEN, draw->itemID, 0);
            if (len >= 0 && len < 32768) { label.resize(static_cast<std::size_t>(len) + 1U); SendMessageW(draw->hwndItem, LB_GETTEXT, draw->itemID, reinterpret_cast<LPARAM>(label.data())); label.resize(static_cast<std::size_t>(len)); }
        } else {
            const int len = GetWindowTextLengthW(draw->hwndItem);
            label.resize(static_cast<std::size_t>(std::max(0, len)) + 1U);
            GetWindowTextW(draw->hwndItem, label.data(), len + 1);
            label.resize(static_cast<std::size_t>(std::max(0, len)));
        }
        RECT text_rect = draw->rcItem;
        if (is_list) text_rect.left += b->px(10);
        DrawTextW(draw->hDC, label.c_str(), -1, &text_rect,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX |
            (is_list ? DT_LEFT : DT_CENTER));
        if (draw->itemState & ODS_FOCUS) DrawFocusRect(draw->hDC, &draw->rcItem);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wp);
        const bool is_field = message != WM_CTLCOLORSTATIC;
        SetTextColor(dc, b->text); SetBkColor(dc, is_field ? b->field_bg : b->bg);
        return reinterpret_cast<LRESULT>(is_field ? b->field : b->background);
    }
    case WM_ERASEBKGND: {
        RECT bounds{}; GetClientRect(window, &bounds);
        FillRect(reinterpret_cast<HDC>(wp), &bounds, b->background); return 1;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: b->accept(); return 0;
        case IDCANCEL: b->done = true; DestroyWindow(window); return 0;
        case go: { const std::filesystem::path entered(b->value(address)); b->navigate(entered.is_absolute() ? entered : b->directory / entered); return 0; }
        case back: if (!b->history.empty()) { const auto previous = b->history.back(); b->history.pop_back(); b->navigate(previous, false); } return 0;
        case up: if (b->directory.has_parent_path()) b->navigate(b->directory.parent_path()); return 0;
        case home: { std::array<wchar_t, 32768> home_path{}; const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home_path.data(), static_cast<DWORD>(home_path.size())); if (n > 0 && n < home_path.size()) b->navigate(home_path.data()); return 0; }
        case files: if (HIWORD(wp) == LBN_SELCHANGE) b->select_entry(false); else if (HIWORD(wp) == LBN_DBLCLK) b->select_entry(true); return 0;
        case filter: if (HIWORD(wp) == CBN_SELCHANGE) b->populate(); return 0;
        case system_browser: b->result.outcome = FilePickerOutcome::system_browser; b->done = true; DestroyWindow(window); return 0;
        }
        break;
    case WM_CLOSE: b->done = true; DestroyWindow(window); return 0;
    }
    } catch (...) {
        if (message == WM_CREATE) return -1;
        SetWindowTextW(b->item(status), L"Unable to read this location. Use System browser for other locations.");
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

} // namespace

static FilePickerResult choose_studio_file_impl(HWND owner, const OPENFILENAMEW& options, FilePickerTheme theme, bool save) {
    Browser b;
    b.owner = owner;
    b.save = save;
    b.confirm_overwrite = (options.Flags & OFN_OVERWRITEPROMPT) != 0;
    if (options.lpstrDefExt != nullptr) b.default_extension = options.lpstrDefExt;
    if (save) b.prompt = L"SAVE A FILE  /  CARTOGRAPHER";
    if (options.lpstrTitle != nullptr && *options.lpstrTitle != L'\0') b.prompt = options.lpstrTitle;
    if (theme == FilePickerTheme::light) {
        b.bg = RGB(232, 237, 242); b.field_bg = RGB(250, 251, 253);
        b.text = RGB(22, 32, 43); b.muted = RGB(70, 85, 100); b.border = RGB(187, 204, 216);
        b.accent = RGB(0, 104, 145);
    } else if (theme == FilePickerTheme::high_contrast) {
        b.bg = RGB(0,0,0); b.field_bg = RGB(0,0,0); b.text = RGB(255,255,255);
        b.border = RGB(70,70,70); b.accent = RGB(0,80,120);
    }
    if (system_high_contrast_enabled()) {
        b.system_high_contrast = true;
        b.bg = GetSysColor(COLOR_WINDOW); b.field_bg = GetSysColor(COLOR_WINDOW);
        b.text = GetSysColor(COLOR_WINDOWTEXT); b.border = GetSysColor(COLOR_BTNFACE);
        b.accent = GetSysColor(COLOR_HIGHLIGHT); b.selection_text = GetSysColor(COLOR_HIGHLIGHTTEXT);
    }
    b.background = CreateSolidBrush(b.bg); b.field = CreateSolidBrush(b.field_bg);
    if (b.background == nullptr || b.field == nullptr) {
        if (b.background != nullptr) DeleteObject(b.background);
        if (b.field != nullptr) DeleteObject(b.field);
        return {FilePickerOutcome::system_browser, {}};
    }
    if (options.lpstrFilter != nullptr) {
        const wchar_t* p = options.lpstrFilter;
        while (*p != L'\0') {
            std::wstring label(p); p += label.size() + 1U;
            if (*p == L'\0') break;
            std::wstring patterns(p); p += patterns.size() + 1U;
            b.filters.push_back({std::move(label), std::move(patterns)});
        }
    }
    std::error_code ec;
    if (options.lpstrInitialDir != nullptr) {
        b.directory = options.lpstrInitialDir;
    } else if (!last_selected_directory.empty() &&
               std::filesystem::is_directory(last_selected_directory, ec) && !ec) {
        b.directory = last_selected_directory;
    } else {
        ec.clear();
        b.directory = std::filesystem::current_path(ec);
    }
    if (ec) { if (b.background) DeleteObject(b.background); if (b.field) DeleteObject(b.field); return {FilePickerOutcome::system_browser, {}}; }
    WNDCLASSW cls{};
    cls.lpfnWndProc = browser_proc; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kClassName; cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    if (RegisterClassW(&cls) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DeleteObject(b.background); DeleteObject(b.field); return {FilePickerOutcome::system_browser, {}};
    }
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kClassName,
        save ? L"Save file | Cartographer" : L"Open file | Cartographer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 920, 680, owner, nullptr, cls.hInstance, &b);
    if (window == nullptr) { if (b.font != nullptr) DeleteObject(b.font); DeleteObject(b.background); DeleteObject(b.field); return {FilePickerOutcome::system_browser, {}}; }
    if (owner != nullptr) EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOWNORMAL); UpdateWindow(window);
    MSG message{};
    while (!b.done) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) { if (result == 0) PostQuitMessage(static_cast<int>(message.wParam)); break; }
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
            const HWND focused = GetFocus();
            if (focused == b.item(address)) { SendMessageW(window, WM_COMMAND, MAKEWPARAM(go, BN_CLICKED), 0); continue; }
            if (focused == b.item(files)) { SendMessageW(window, WM_COMMAND, MAKEWPARAM(files, LBN_DBLCLK), 0); continue; }
            if (focused == b.item(filename)) { SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0); continue; }
            if (focused == b.item(IDOK) || focused == b.item(IDCANCEL) ||
                focused == b.item(back) || focused == b.item(up) || focused == b.item(home) ||
                focused == b.item(go) || focused == b.item(system_browser)) {
                SendMessageW(focused, BM_CLICK, 0, 0); continue;
            }
        }
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (IsWindow(window)) DestroyWindow(window);
    if (owner != nullptr && IsWindow(owner)) { EnableWindow(owner, TRUE); SetActiveWindow(owner); }
    if (b.font != nullptr) DeleteObject(b.font);
    DeleteObject(b.background); DeleteObject(b.field);
    if (b.result.outcome == FilePickerOutcome::selected && b.result.path.has_value())
        last_selected_directory = b.result.path->parent_path();
    return b.result;
}

FilePickerResult choose_studio_file(HWND owner, const OPENFILENAMEW& options, FilePickerTheme theme, bool save) {
    try {
        return choose_studio_file_impl(owner, options, theme, save);
    } catch (const std::exception&) {
        return {FilePickerOutcome::system_browser, {}};
    }
}

} // namespace carto::studio
