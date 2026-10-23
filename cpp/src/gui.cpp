// Native Windows UI. Conversion code is shared with the independently tested CLI.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "common.h"
#include "pipeline.h"
namespace fs = std::filesystem;
namespace {
constexpr wchar_t kApp[] = L"NyaaFlavor";
enum : int {
    IDC_LIST = 1001, IDC_ADD, IDC_REMOVE, IDC_CLEAR, IDC_GRID, IDC_GLISS, IDC_BORROW,
    IDC_DRUM_FALLBACK, IDC_OUTDIR, IDC_PICKOUT, IDC_OPENOUT, IDC_START, IDC_PBAR, IDC_LOG,
    IDC_CANCEL, IDC_STATUS, IDC_ADVANCED, IDC_COUNT,
    ID_TITLE = 2000, ID_GRID, ID_GLISS, ID_OUT, ID_SUB, ID_FILES, ID_OPTIONS, ID_LOG,
    ID_MS = 3000, ID_MS_PICK, ID_MIN_MS, ID_KEEP_MIDI, ID_REAL, ID_KEY, ID_GP,
};
constexpr UINT WM_LOG = WM_APP + 1, WM_PROGRESS = WM_APP + 2, WM_FINISH = WM_APP + 3, WM_AUTOSTART = WM_APP + 4;
HWND g_main{}, g_list{}, g_grid{}, g_gliss{}, g_borrow{}, g_drum_fallback{}, g_outdir{}, g_start{}, g_pbar{}, g_log{}, g_status{};
HFONT g_ui{}, g_title{}, g_logfont{};
UINT g_dpi = 96;
bool g_running = false, g_close_pending = false;
std::atomic<bool> g_cancel{false};
std::thread g_worker;
std::vector<std::wstring> g_files;
std::mutex g_mutex;
std::deque<std::string> g_lines;
nyaa::ConvertOptions g_settings;

int px(int n) { return MulDiv(n, static_cast<int>(g_dpi), 96); }
std::wstring text(HWND h) {
    std::wstring s(static_cast<size_t>(GetWindowTextLengthW(h)) + 1, L'\0');
    const int count = GetWindowTextW(h, s.data(), static_cast<int>(s.size()));
    s.resize(static_cast<size_t>(count)); return s;
}
std::wstring exe_dir() {
    std::vector<wchar_t> buffer(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return fs::path(std::wstring(buffer.data(), n)).parent_path().wstring();
}
bool equal_path(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
HWND control(HWND parent, const wchar_t* cls, const wchar_t* value, DWORD style, int id, HFONT font = nullptr) {
    HWND h = CreateWindowExW(0, cls, value, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g_ui), TRUE);
    return h;
}
void place(int id, int x, int y, int w, int h) { MoveWindow(GetDlgItem(g_main, id), px(x), px(y), px(w), px(h), TRUE); }
void log_line(const std::wstring& value) {
    int length = GetWindowTextLengthW(g_log);
    if (length > 1000000) {
        SendMessageW(g_log, EM_SETSEL, 0, 250000);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
        length = GetWindowTextLengthW(g_log);
    }
    SendMessageW(g_log, EM_SETSEL, length, length);
    const auto line = value + L"\r\n";
    SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}
void drain_log() {
    std::deque<std::string> lines;
    { std::lock_guard<std::mutex> lock(g_mutex); lines.swap(g_lines); }
    for (const auto& line : lines) log_line(nyaa::u8_to_w(line));
}
void count_files() {
    const auto value = std::to_wstring(g_files.size()) + L" 个文件 · 支持直接拖入窗口";
    SetDlgItemTextW(g_main, IDC_COUNT, value.c_str());
}
void add_files(const std::vector<std::wstring>& paths) {
    if (g_running) return;
    for (const auto& path : paths) {
        std::error_code ec;
        const fs::path p = fs::absolute(path, ec).lexically_normal();
        if (ec || !fs::is_regular_file(p, ec)) continue;
        const auto ext = nyaa::py_lower(nyaa::path_to_u8(p.extension()));
        if (ext != ".mid" && ext != ".midi" && ext != ".gp" && ext != ".gpx") { log_line(L"跳过不支持的文件：" + path); continue; }
        const auto full = p.wstring();
        if (std::any_of(g_files.begin(), g_files.end(), [&](const auto& old) { return equal_path(old, full); })) continue;
        g_files.push_back(full); SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(full.c_str()));
    }
    count_files();
}
void pick_files() {
    std::vector<wchar_t> buffer(65536);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_main; ofn.lpstrFile = buffer.data(); ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = L"选 MIDI 或 Guitar Pro 谱";
    ofn.lpstrFilter = L"谱或 MIDI\0*.mid;*.midi;*.gp;*.gpx\0MIDI\0*.mid;*.midi\0Guitar Pro\0*.gp;*.gpx\0所有文件\0*.*\0";
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    const wchar_t* cursor = buffer.data(); const std::wstring first = cursor; cursor += first.size() + 1;
    std::vector<std::wstring> picked;
    if (!*cursor) picked.push_back(first);
    else while (*cursor) { picked.push_back((fs::path(first) / cursor).wstring()); cursor += std::wcslen(cursor) + 1; }
    add_files(picked);
}
void remove_selected() {
    const int n = static_cast<int>(SendMessageW(g_list, LB_GETSELCOUNT, 0, 0));
    if (n <= 0) return;
    std::vector<int> indices(static_cast<size_t>(n));
    SendMessageW(g_list, LB_GETSELITEMS, n, reinterpret_cast<LPARAM>(indices.data()));
    for (auto it = indices.rbegin(); it != indices.rend(); ++it) { SendMessageW(g_list, LB_DELETESTRING, *it, 0); g_files.erase(g_files.begin() + *it); }
    count_files();
}
void pick_folder() {
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD options{}; dialog->GetOptions(&options); dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"输出到哪个文件夹");
    if (SUCCEEDED(dialog->Show(g_main))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR value = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value))) { SetWindowTextW(g_outdir, value); CoTaskMemFree(value); }
            item->Release();
        }
    }
    dialog->Release();
}
void open_folder() {
    const auto directory = text(g_outdir); std::error_code ec;
    if (directory.empty()) { MessageBoxW(g_main, L"请先选择输出文件夹。", kApp, MB_OK | MB_ICONINFORMATION); return; }
    fs::create_directories(directory, ec);
    if (ec) { MessageBoxW(g_main, L"无法创建输出文件夹，请检查路径和写入权限。", kApp, MB_OK | MB_ICONERROR); return; }
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(g_main, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) MessageBoxW(g_main, L"无法打开输出文件夹。", kApp, MB_OK | MB_ICONERROR);
}

INT_PTR CALLBACK advanced_proc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INITDIALOG) {
        SetWindowLongPtrW(dialog, DWLP_USER, lp); auto* opt = reinterpret_cast<nyaa::ConvertOptions*>(lp);
        SetWindowTextW(dialog, L"高级选项");
        auto add = [&](const wchar_t* cls, const wchar_t* value, DWORD style, int id, int x, int y, int w, int h) {
            const HWND child = control(dialog, cls, value, style, id); MoveWindow(child, px(x), px(y), px(w), px(h), TRUE); return child;
        };
        add(L"STATIC", L"MuseScore", 0, -1, 14, 18, 90, 22);
        add(L"EDIT", nyaa::u8_to_w(opt->musescore).c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, ID_MS, 110, 14, 310, 26);
        add(L"BUTTON", L"选择…", BS_PUSHBUTTON | WS_TABSTOP, ID_MS_PICK, 430, 14, 70, 26);
        add(L"STATIC", L"短音阈值(ms)", 0, -1, 14, 60, 90, 22);
        add(L"EDIT", std::to_wstring(static_cast<int>(opt->min_ms)).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, ID_MIN_MS, 110, 54, 80, 26);
        add(L"BUTTON", L"保留 Guitar Pro 导出的 MIDI", BS_AUTOCHECKBOX | WS_TABSTOP, ID_KEEP_MIDI, 110, 94, 350, 24);
        add(L"BUTTON", L"按实际时长换算（变速曲）", BS_AUTOCHECKBOX | WS_TABSTOP, ID_REAL, 110, 130, 380, 24);
        add(L"STATIC", L"调性（可留空）", 0, -1, 14, 174, 90, 22);
        add(L"EDIT", nyaa::u8_to_w(opt->key).c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, ID_KEY, 110, 168, 390, 26);
        add(L"BUTTON", L"使用 GP 谱声部信息（自动找同名谱）", BS_AUTOCHECKBOX | WS_TABSTOP, ID_GP, 110, 210, 390, 24);
        add(L"BUTTON", L"确定", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 340, 250, 74, 28);
        add(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 426, 250, 74, 28);
        CheckDlgButton(dialog, ID_KEEP_MIDI, opt->keep_midi); CheckDlgButton(dialog, ID_REAL, opt->time_mode == "real"); CheckDlgButton(dialog, ID_GP, opt->use_gp);
        RECT parent{}; GetWindowRect(g_main, &parent); const int w = px(530), h = px(330);
        SetWindowPos(dialog, nullptr, parent.left + (parent.right - parent.left - w) / 2, parent.top + (parent.bottom - parent.top - h) / 2, w, h, SWP_NOZORDER);
        return TRUE;
    }
    if (msg == WM_COMMAND) {
        const int id = LOWORD(wp);
        if (id == ID_MS_PICK) {
            wchar_t buffer[32768]{}; OPENFILENAMEW ofn{sizeof(ofn)};
            ofn.hwndOwner = dialog; ofn.lpstrFile = buffer; ofn.nMaxFile = 32768;
            ofn.lpstrFilter = L"MuseScore 程序\0*.exe\0"; ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) SetDlgItemTextW(dialog, ID_MS, buffer);
        } else if (id == IDOK) {
            BOOL valid = FALSE; const UINT threshold = GetDlgItemInt(dialog, ID_MIN_MS, &valid, FALSE);
            if (!valid || threshold > 1000) { MessageBoxW(dialog, L"短音阈值请输入 0..1000 毫秒。", kApp, MB_OK | MB_ICONINFORMATION); return TRUE; }
            auto* opt = reinterpret_cast<nyaa::ConvertOptions*>(GetWindowLongPtrW(dialog, DWLP_USER));
            opt->musescore = nyaa::w_to_u8(text(GetDlgItem(dialog, ID_MS))); opt->key = nyaa::w_to_u8(text(GetDlgItem(dialog, ID_KEY)));
            opt->min_ms = threshold; opt->keep_midi = IsDlgButtonChecked(dialog, ID_KEEP_MIDI) == BST_CHECKED;
            opt->time_mode = IsDlgButtonChecked(dialog, ID_REAL) == BST_CHECKED ? "real" : "grid";
            opt->use_gp = IsDlgButtonChecked(dialog, ID_GP) == BST_CHECKED; EndDialog(dialog, IDOK);
        } else if (id == IDCANCEL) EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (msg == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}
void advanced() {
    struct Template { DLGTEMPLATE dialog; WORD menu = 0, window_class = 0, title = 0; } form{};
    form.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME; form.dialog.cx = 350; form.dialog.cy = 210;
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &form.dialog, g_main, advanced_proc, reinterpret_cast<LPARAM>(&g_settings));
}

void set_busy(bool busy) {
    for (int id : {IDC_LIST, IDC_ADD, IDC_REMOVE, IDC_CLEAR, IDC_GRID, IDC_GLISS, IDC_BORROW, IDC_DRUM_FALLBACK, IDC_OUTDIR, IDC_PICKOUT, IDC_OPENOUT, IDC_START, IDC_ADVANCED}) EnableWindow(GetDlgItem(g_main, id), !busy);
    EnableWindow(GetDlgItem(g_main, IDC_CANCEL), busy);
}
void worker_main(HWND main, std::vector<std::wstring> files, std::wstring directory, nyaa::ConvertOptions options) {
    auto log = [main](const std::string& line) {
        { std::lock_guard<std::mutex> lock(g_mutex); g_lines.push_back(line); }
        PostMessageW(main, WM_LOG, 0, 0);
    };
    int success = 0, failed = 0; std::vector<std::wstring> used;
    for (size_t i = 0; i < files.size() && !g_cancel; ++i) {
        const fs::path source(files[i]); std::wstring stem = source.stem().wstring(), name = stem; int suffix = 2;
        while (std::any_of(used.begin(), used.end(), [&](const auto& old) { return equal_path(old, name); })) name = stem + L"_" + std::to_wstring(suffix++);
        used.push_back(name);
        log(""); log("正在转换 " + std::to_string(i + 1) + "/" + std::to_string(files.size()) + "：" + nyaa::path_to_u8(source.filename()));
        try {
            const auto destination = fs::path(directory) / (name + L"_B.nbs");
            const auto result = nyaa::convert_pipeline(nyaa::path_to_u8(source), nyaa::path_to_u8(destination), options, log);
            if (result.failed) { ++failed; log("✗ 失败：" + result.message); } else { ++success; log("✓ 已生成"); }
        } catch (const std::exception& e) { ++failed; log(std::string("✗ 失败：") + e.what()); }
        PostMessageW(main, WM_PROGRESS, static_cast<WPARAM>(i + 1), 0);
    }
    PostMessageW(main, WM_FINISH, static_cast<WPARAM>(success), static_cast<LPARAM>(failed));
}
void start_convert() {
    if (g_running) return;
    if (g_files.empty()) { MessageBoxW(g_main, L"先添加要转换的文件。", kApp, MB_ICONINFORMATION | MB_OK); return; }
    const auto directory = text(g_outdir); std::error_code ec;
    if (!directory.empty()) fs::create_directories(directory, ec);
    if (directory.empty() || ec || !fs::is_directory(directory, ec)) { MessageBoxW(g_main, L"输出文件夹不可用，请检查路径和写入权限。", kApp, MB_ICONERROR | MB_OK); return; }
    auto options = g_settings; const auto grid = SendMessageW(g_grid, CB_GETCURSEL, 0, 0);
    options.grid = grid == 0 ? 4 : grid == 2 ? 16 : 8;
    options.gliss = SendMessageW(g_gliss, CB_GETCURSEL, 0, 0) == 1 ? "keep" : "diatonic";
    options.borrow_table = IsDlgButtonChecked(g_main, IDC_BORROW) == BST_CHECKED;
    options.drum_fallback = IsDlgButtonChecked(g_main, IDC_DRUM_FALLBACK) == BST_CHECKED;
    if (g_worker.joinable()) g_worker.join();
    drain_log(); SetWindowTextW(g_log, L""); g_cancel = false; g_running = true; set_busy(true);
    SendMessageW(g_pbar, PBM_SETRANGE32, 0, g_files.size()); SendMessageW(g_pbar, PBM_SETPOS, 0, 0); SetWindowTextW(g_status, L"正在转换…");
    try { g_worker = std::thread(worker_main, g_main, g_files, directory, options); }
    catch (...) { g_running = false; set_busy(false); MessageBoxW(g_main, L"无法创建转换线程。", kApp, MB_ICONERROR | MB_OK); }
}

void layout() {
    RECT bounds{}; GetClientRect(g_main, &bounds);
    const int width = MulDiv(bounds.right, 96, g_dpi), height = MulDiv(bounds.bottom, 96, g_dpi);
    const int inner = width - 24, listHeight = std::max(60, (height - 400) * 2 / 5 + 68);
    place(ID_TITLE, 12, 8, 176, 32); place(ID_SUB, 195, 20, inner - 195, 22);
    place(ID_FILES, 12, 55, inner, 20); place(IDC_LIST, 12, 80, inner, listHeight);
    int y = 80 + listHeight + 8;
    place(IDC_ADD, 12, y, 96, 28); place(IDC_REMOVE, 116, y, 88, 28); place(IDC_CLEAR, 212, y, 68, 28); place(IDC_COUNT, 292, y + 5, inner - 280, 22);
    y += 42; place(ID_OPTIONS, 12, y, inner, 22); y += 28;
    place(ID_GRID, 12, y + 4, 65, 22); place(IDC_GRID, 80, y, 60, 200);
    place(ID_GLISS, 156, y + 4, 36, 22); place(IDC_GLISS, 196, y, 112, 200);
    place(IDC_BORROW, 326, y, 174, 26);
    place(IDC_DRUM_FALLBACK, 508, y, 260, 26);
    place(IDC_ADVANCED, inner - 76, y, 76, 28);
    y += 34;
    place(ID_OUT, 12, y + 4, 52, 22); place(IDC_OUTDIR, 70, y, inner - 222, 26);
    place(IDC_PICKOUT, width - 158, y, 74, 26); place(IDC_OPENOUT, width - 76, y, 64, 26);
    y += 40; place(IDC_START, 12, y, 125, 30); place(IDC_CANCEL, 145, y, 65, 30);
    place(IDC_PBAR, 222, y + 6, 200, 18); place(IDC_STATUS, 436, y + 5, inner - 424, 24);
    y += 42; place(ID_LOG, 12, y, inner, 22); y += 26;
    place(IDC_LOG, 12, y, inner, std::max(60, height - y - 12)); InvalidateRect(g_main, nullptr, TRUE);
}
void fonts() {
    if (g_ui) DeleteObject(g_ui); if (g_title) DeleteObject(g_title); if (g_logfont) DeleteObject(g_logfont);
    g_ui = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    g_title = CreateFontW(-px(24), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_logfont = CreateFontW(-px(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    for (HWND h = GetWindow(g_main, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT)) { const int id = GetDlgCtrlID(h); SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(id == ID_TITLE ? g_title : id == IDC_LOG ? g_logfont : g_ui), TRUE); }
}
void build_ui() {
    g_dpi = GetDpiForWindow(g_main); fonts();
    control(g_main, L"STATIC", kApp, 0, ID_TITLE, g_title); control(g_main, L"STATIC", L"把 MIDI / Guitar Pro 谱转成 Note Block Studio / Notebot .nbs", 0, ID_SUB);
    control(g_main, L"STATIC", L"① 要转换的文件", 0, ID_FILES);
    g_list = control(g_main, L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT, IDC_LIST);
    for (const auto& pair : std::vector<std::pair<int, const wchar_t*>>{{IDC_ADD,L"添加文件…"},{IDC_REMOVE,L"移除选中"},{IDC_CLEAR,L"清空"},{IDC_ADVANCED,L"高级…"},{IDC_PICKOUT,L"选择…"},{IDC_OPENOUT,L"打开"},{IDC_CANCEL,L"停止"}}) control(g_main, L"BUTTON", pair.second, BS_PUSHBUTTON | WS_TABSTOP, pair.first);
    control(g_main, L"STATIC", L"", 0, IDC_COUNT); control(g_main, L"STATIC", L"② 选项（默认就行）", 0, ID_OPTIONS); control(g_main, L"STATIC", L"每拍格数", 0, ID_GRID);
    g_grid = control(g_main, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_GRID);
    for (const wchar_t* value : {L"4",L"8",L"16"}) SendMessageW(g_grid, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    SendMessageW(g_grid, CB_SETCURSEL, 1, 0); control(g_main, L"STATIC", L"滑音", 0, ID_GLISS);
    g_gliss = control(g_main, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_GLISS);
    for (const wchar_t* value : {L"调内装饰音",L"原样保留"}) SendMessageW(g_gliss, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    SendMessageW(g_gliss, CB_SETCURSEL, 0, 0);
    g_borrow = control(g_main, L"BUTTON", L"自动换音色（推荐）", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_BORROW); CheckDlgButton(g_main, IDC_BORROW, BST_CHECKED);
    g_drum_fallback = control(g_main, L"BUTTON", L"识别沙锤、三角铁等打击乐", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_DRUM_FALLBACK);
    CheckDlgButton(g_main, IDC_DRUM_FALLBACK, BST_CHECKED);
    control(g_main, L"STATIC", L"输出到", 0, ID_OUT);
    g_outdir = control(g_main, L"EDIT", (fs::path(exe_dir()) / L"输出").c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, IDC_OUTDIR);
    g_start = control(g_main, L"BUTTON", L"③ 开始转换", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_START); g_pbar = control(g_main, PROGRESS_CLASSW, L"", 0, IDC_PBAR);
    g_status = control(g_main, L"STATIC", L"就绪", SS_LEFT, IDC_STATUS); control(g_main, L"STATIC", L"转换记录", 0, ID_LOG);
    g_log = control(g_main, L"EDIT", L"", WS_BORDER | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL, IDC_LOG, g_logfont);
    SendMessageW(g_log, EM_SETLIMITTEXT, 2000000, 0); set_busy(false); count_files(); layout();
}
LRESULT CALLBACK wndproc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
        case WM_CREATE: g_main = hwnd; build_ui(); DragAcceptFiles(hwnd, TRUE); return 0;
        case WM_SIZE: if (g_list) layout(); return 0;
        case WM_GETMINMAXINFO: { auto* info = reinterpret_cast<MINMAXINFO*>(lp); info->ptMinTrackSize = {px(900), px(580)}; return 0; }
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp); fonts(); const auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER | SWP_NOACTIVATE); layout(); return 0;
        }
        case WM_DROPFILES: {
            const HDROP drop = reinterpret_cast<HDROP>(wp); const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0); std::vector<std::wstring> paths;
            for (UINT i = 0; i < count; ++i) { const UINT n = DragQueryFileW(drop, i, nullptr, 0); std::wstring value(n + 1, L'\0'); DragQueryFileW(drop, i, value.data(), n + 1); value.resize(n); paths.push_back(value); }
            DragFinish(drop); add_files(paths); return 0;
        }
        case WM_LOG: drain_log(); return 0;
        case WM_PROGRESS: {
            SendMessageW(g_pbar, PBM_SETPOS, wp, 0); const auto value = L"正在转换 " + std::to_wstring(wp) + L"/" + std::to_wstring(g_files.size()); SetWindowTextW(g_status, value.c_str()); return 0;
        }
        case WM_FINISH: {
            drain_log(); if (g_worker.joinable()) g_worker.join(); g_running = false; set_busy(false);
            const auto summary = std::wstring(g_cancel ? L"已停止：成功 " : L"转换完成：成功 ") + std::to_wstring(wp) + L" 个，失败 " + std::to_wstring(lp) + L" 个";
            SetWindowTextW(g_status, summary.c_str()); log_line(summary); if (g_close_pending) DestroyWindow(hwnd); return 0;
        }
        case WM_AUTOSTART: start_convert(); return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wp); if (g_running && id != IDC_CANCEL) return 0;
            switch (id) {
                case IDC_ADD: pick_files(); break;
                case IDC_REMOVE: remove_selected(); break;
                case IDC_CLEAR: g_files.clear(); SendMessageW(g_list, LB_RESETCONTENT, 0, 0); count_files(); break;
                case IDC_PICKOUT: pick_folder(); break;
                case IDC_OPENOUT: open_folder(); break;
                case IDC_ADVANCED: advanced(); break;
                case IDC_START: start_convert(); break;
                case IDC_CANCEL: g_cancel = true; EnableWindow(GetDlgItem(hwnd, IDC_CANCEL), FALSE); SetWindowTextW(g_status, L"当前文件结束后停止…"); break;
            }
            return 0;
        }
        case WM_CLOSE:
            if (g_running) { g_close_pending = true; g_cancel = true; SetWindowTextW(g_status, L"当前文件结束后退出…"); return 0; }
            DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: return DefWindowProcW(hwnd, message, wp, lp);
    }
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g_dpi = GetDpiForSystem();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSEXW cls{sizeof(cls)}; cls.lpfnWndProc = wndproc; cls.hInstance = instance; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1); cls.lpszClassName = L"NyaaFlavorMain"; cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&cls)) return 1;
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"NyaaFlavor 0.1.0", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, px(920), px(720), nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    int count = 0;
    if (LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count)) {
        std::vector<std::wstring> paths; for (int i = 1; i < count; ++i) if (arguments[i][0] != L'-') paths.emplace_back(arguments[i]); add_files(paths); LocalFree(arguments);
    }
    ShowWindow(hwnd, show); UpdateWindow(hwnd); if (!g_files.empty()) PostMessageW(hwnd, WM_AUTOSTART, 0, 0);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) if (!IsDialogMessageW(hwnd, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (g_worker.joinable()) g_worker.join(); DeleteObject(g_ui); DeleteObject(g_title); DeleteObject(g_logfont); if (SUCCEEDED(com)) CoUninitialize(); return 0;
}
