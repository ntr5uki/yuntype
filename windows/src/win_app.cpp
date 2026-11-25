#include "win_app.h"
#include "win_sender.h"
#include "win_utf8.h"

#include "../../core/pinyin_util.h"
#include "../../core/utf8_util.h"

#include <imm.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace yuntype::win {

namespace {

constexpr wchar_t kCtrlClassName[] = L"YunTypeWinControllerWnd";
constexpr UINT kTrayIconId = 1001;
constexpr UINT kTimerPollId = 2001;
constexpr UINT kTimerCompactionId = 2002;
constexpr UINT kTimerPersistId = 2003;
constexpr UINT kTimerPostCommitId = 2004;
constexpr UINT kPollIntervalMs = 40;
constexpr UINT kCompactionIntervalMs = 100;
constexpr UINT kPostCommitSyncMs = 300;
constexpr std::size_t kMaxAsciiTrackBytes = 512;
constexpr int kMaxLoadMoreRounds = 5;
constexpr std::uint64_t kShadowTimeoutUsec = 30ULL * 1000 * 1000;
constexpr std::uint64_t kWindowAsciiTtlUsec = 60ULL * 1000 * 1000;
constexpr std::size_t kMaxSavedWindowAscii = 16;

constexpr UINT WM_YT_TRAY = WM_APP + 1;
constexpr UINT WM_YT_TRIGGER = WM_APP + 2;
constexpr UINT WM_YT_ACTIVE_KEY = WM_APP + 3;
constexpr UINT WM_YT_PASSIVE_KEY = WM_APP + 4;
constexpr UINT WM_YT_FG_CHANGED = WM_APP + 5;
constexpr UINT WM_YT_RELOAD_CONFIG = WM_APP + 6;
constexpr UINT WM_YT_CLEAR_HISTORY = WM_APP + 7;
constexpr UINT WM_YT_MOUSE_CLICK = WM_APP + 8;
constexpr UINT WM_YT_CAPTURE_START = WM_APP + 9;

constexpr UINT IDM_TOGGLE_ENABLED = 3001;
constexpr UINT IDM_OPEN_SETTINGS = 3002;
constexpr UINT IDM_CLEAR_HISTORY = 3003;
constexpr UINT IDM_AUTO_START = 3004;
constexpr UINT IDM_EXIT = 3005;

bool isForegroundChineseImeActive() {
    HWND fg = ::GetForegroundWindow();
    if (!fg) {
        return false;
    }
    const DWORD tid = ::GetWindowThreadProcessId(fg, nullptr);
    GUITHREADINFO gti{};
    gti.cbSize = sizeof(gti);
    HWND focus = fg;
    if (::GetGUIThreadInfo(tid, &gti) && gti.hwndFocus) {
        focus = gti.hwndFocus;
    }
    HWND imeWnd = ::ImmGetDefaultIMEWnd(focus);
    if (!imeWnd) {
        return false;
    }
    constexpr WPARAM kImcGetOpenStatus = 0x0005;
    constexpr WPARAM kImcGetConversionMode = 0x0001;
    DWORD_PTR openStatus = 0;
    if (!::SendMessageTimeoutW(imeWnd, WM_IME_CONTROL, kImcGetOpenStatus, 0,
                               SMTO_ABORTIFHUNG, 15, &openStatus) ||
        openStatus == 0) {
        return false;
    }
    DWORD_PTR convMode = 0;
    if (!::SendMessageTimeoutW(imeWnd, WM_IME_CONTROL, kImcGetConversionMode, 0,
                               SMTO_ABORTIFHUNG, 15, &convMode)) {
        return false;
    }
    return (convMode & IME_CMODE_NATIVE) != 0;
}

bool isModifierVk(DWORD vk) {
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL;
}

unsigned int currentModifierMask() {
    unsigned int m = 0;
    if ((::GetAsyncKeyState(VK_MENU) & 0x8000) != 0) m |= 1U;
    if ((::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) m |= 2U;
    if ((::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) m |= 4U;
    if ((::GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
        (::GetAsyncKeyState(VK_RWIN) & 0x8000) != 0) m |= 8U;
    return m;
}

LPARAM packKeyFlags(bool shift, bool ctrl, bool alt, char asciiCh) {
    const unsigned int flags =
        (shift ? 1U : 0U) | (ctrl ? 2U : 0U) | (alt ? 4U : 0U) |
        (static_cast<unsigned int>(static_cast<unsigned char>(asciiCh)) << 8);
    return static_cast<LPARAM>(flags);
}

void unpackKeyFlags(LPARAM lp, bool &shift, bool &ctrl, bool &alt, char &asciiCh) {
    const unsigned int flags = static_cast<unsigned int>(lp);
    shift = (flags & 1U) != 0;
    ctrl = (flags & 2U) != 0;
    alt = (flags & 4U) != 0;
    asciiCh = static_cast<char>((flags >> 8) & 0xFFU);
}

HICON createProceduralTrayIcon() {
    constexpr int kSize = 32;
    HDC screenDc = ::GetDC(nullptr);
    HDC memDc = ::CreateCompatibleDC(screenDc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = kSize;
    bmi.bmiHeader.biHeight = -kSize;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP colorBmp = ::CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = ::SelectObject(memDc, colorBmp);

    // Draw rounded blue badge with crisp white "云"
    HBRUSH bgBr = ::CreateSolidBrush(RGB(37, 99, 235));
    HPEN bgPn = ::CreatePen(PS_SOLID, 1, RGB(29, 78, 216));
    HGDIOBJ oldBr = ::SelectObject(memDc, bgBr);
    HGDIOBJ oldPn = ::SelectObject(memDc, bgPn);
    ::RoundRect(memDc, 2, 2, kSize - 2, kSize - 2, 8, 8);
    ::SelectObject(memDc, oldPn);
    ::SelectObject(memDc, oldBr);
    ::DeleteObject(bgPn);
    ::DeleteObject(bgBr);

    LOGFONTW lf{};
    lf.lfHeight = -20;
    lf.lfWeight = FW_BOLD;
    lf.lfQuality = ANTIALIASED_QUALITY;
    ::wcsncpy(lf.lfFaceName, L"Microsoft YaHei UI", LF_FACESIZE - 1);
    HFONT font = ::CreateFontIndirectW(&lf);
    HGDIOBJ oldFont = ::SelectObject(memDc, font);
    ::SetBkMode(memDc, TRANSPARENT);
    ::SetTextColor(memDc, RGB(255, 255, 255));
    RECT rc{0, 0, kSize, kSize};
    ::DrawTextW(memDc, L"\u4E91", 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(memDc, oldFont);
    ::DeleteObject(font);

    // Ensure opaque alpha on painted pixels
    if (bits) {
        auto *px = reinterpret_cast<std::uint32_t *>(bits);
        for (int i = 0; i < kSize * kSize; ++i) {
            if ((px[i] & 0x00FFFFFFU) != 0) {
                px[i] |= 0xFF000000U;
            }
        }
    }

    HBITMAP maskBmp = ::CreateBitmap(kSize, kSize, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = colorBmp;
    ii.hbmMask = maskBmp;
    HICON icon = ::CreateIconIndirect(&ii);

    ::SelectObject(memDc, oldBmp);
    ::DeleteObject(maskBmp);
    ::DeleteObject(colorBmp);
    ::DeleteDC(memDc);
    ::ReleaseDC(nullptr, screenDc);
    return icon;
}

std::size_t fileSizeUtf8(const std::string &path) {
    if (path.empty()) {
        return 0;
    }
    const std::filesystem::path p(
        std::u8string(reinterpret_cast<const char8_t *>(path.data()), path.size()));
    std::error_code ec;
    const auto sz = std::filesystem::file_size(p, ec);
    return ec ? 0 : static_cast<std::size_t>(sz);
}

} // namespace

WinApp *WinApp::instance_ = nullptr;

WinApp::~WinApp() {
    settingsServer_.stop();
    if (fgHook_) {
        ::UnhookWinEvent(fgHook_);
        fgHook_ = nullptr;
    }
    if (captureHook_) {
        ::UnhookWinEvent(captureHook_);
        captureHook_ = nullptr;
    }
    if (kbHook_) {
        ::UnhookWindowsHookEx(kbHook_);
        kbHook_ = nullptr;
    }
    if (mouseHook_) {
        ::UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
    overlay_.destroy();
    removeTrayIcon();
    if (trayIcon_) {
        ::DestroyIcon(trayIcon_);
        trayIcon_ = nullptr;
    }
    if (ctrlHwnd_) {
        ::DestroyWindow(ctrlHwnd_);
        ctrlHwnd_ = nullptr;
    }
    flushHistoryToDisk();
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

int WinApp::run(HINSTANCE hInst) {
    hInst_ = hInst;
    instance_ = this;

    configDir_ = defaultConfigDir();
    dataDir_ = defaultDataDir();
    historyPath_ = dataDir_ + "\\history.json";

    const bool hadConfig = loadConfigFromDir(configDir_, config_);
    reloadConfigAndSync();
    if (config_.historyEnabled) {
        loadHistoryFromDisk();
    }

    if (!initControllerWindow(hInst)) {
        return 1;
    }
    overlay_.create(hInst, [this](int pageIndex) {
        if (!pendingCandidates_) {
            return;
        }
        const int pageSize = std::clamp(config_.candidatePageSize, 2, 10);
        const int curPage = pendingCandidates_->globalCursor / pageSize;
        const int globalIndex = curPage * pageSize + pageIndex;
        if (globalIndex >= 0 &&
            static_cast<std::size_t>(globalIndex) < pendingCandidates_->candidates.size()) {
            commitPendingCandidate(globalIndex);
        }
    });
    initTrayIcon();

    kbHook_ = ::SetWindowsHookExW(WH_KEYBOARD_LL, &WinApp::lowLevelKeyboardProc, hInst, 0);
    fgHook_ = ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                &WinApp::winEventProc, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    captureHook_ = ::SetWinEventHook(EVENT_SYSTEM_CAPTURESTART, EVENT_SYSTEM_CAPTURESTART, nullptr,
                                     &WinApp::winEventProc, 0, 0,
                                     WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    lastFgWnd_ = ::GetForegroundWindow();
    lastFgChangedUsec_ = monotonicNowUsec();

    if (config_.historyEnabled) {
        pollCompaction();
    }

    // First-run convenience: open settings if no model API key / config existed yet
    if (!hadConfig || !client_.config().isReady()) {
        openSettings();
    }

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    flushHistoryToDisk();
    return static_cast<int>(msg.wParam);
}

bool WinApp::initControllerWindow(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WinApp::ctrlWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kCtrlClassName;
    ::RegisterClassExW(&wc);

    ctrlHwnd_ = ::CreateWindowExW(0, kCtrlClassName, L"YunType", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, hInst, this);
    return ctrlHwnd_ != nullptr;
}

bool WinApp::initTrayIcon() {
    trayIcon_ = createProceduralTrayIcon();
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = ctrlHwnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_YT_TRAY;
    nid.hIcon = trayIcon_ ? trayIcon_ : ::LoadIconW(nullptr, IDI_APPLICATION);
    const std::wstring tip = L"\u4E91\u6253 YunType (" + utf8ToWide(config_.triggerKey) + L")";
    ::wcsncpy(nid.szTip, tip.c_str(), std::size(nid.szTip) - 1);
    return ::Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void WinApp::removeTrayIcon() {
    if (!ctrlHwnd_) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = ctrlHwnd_;
    nid.uID = kTrayIconId;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
}

void WinApp::showTrayMenu() {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
        return;
    }
    AppConfig snap;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        snap = config_;
    }
    const std::wstring toggleLabel =
        L"\u542F\u7528\u4E91\u6253 (" + utf8ToWide(snap.triggerKey) + L")";
    ::AppendMenuW(menu, MF_STRING | (snap.enabled ? MF_CHECKED : 0), IDM_TOGGLE_ENABLED,
                  toggleLabel.c_str());
    ::AppendMenuW(menu, MF_STRING, IDM_OPEN_SETTINGS, L"\u6253\u5F00\u8BBE\u7F6E...");
    ::AppendMenuW(menu, MF_STRING, IDM_CLEAR_HISTORY, L"\u6E05\u7A7A\u5386\u53F2\u8BB0\u5F55");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (snap.autoStart ? MF_CHECKED : 0), IDM_AUTO_START,
                  L"\u5F00\u673A\u81EA\u52A8\u542F\u52A8");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_EXIT, L"\u9000\u51FA");

    POINT pt{};
    ::GetCursorPos(&pt);
    ::SetForegroundWindow(ctrlHwnd_);
    const UINT cmd = static_cast<UINT>(
        ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                         pt.x, pt.y, 0, ctrlHwnd_, nullptr));
    ::DestroyMenu(menu);

    switch (cmd) {
    case IDM_TOGGLE_ENABLED: {
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            config_.enabled = !config_.enabled;
            saveConfigToDir(configDir_, config_);
        }
        reloadConfigAndSync();
        break;
    }
    case IDM_OPEN_SETTINGS:
        openSettings();
        break;
    case IDM_CLEAR_HISTORY:
        clearHistory();
        break;
    case IDM_AUTO_START: {
        bool nextAuto = false;
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            config_.autoStart = !config_.autoStart;
            nextAuto = config_.autoStart;
            saveConfigToDir(configDir_, config_);
        }
        syncAutoStartRegistry(nextAuto, currentExePathUtf8());
        break;
    }
    case IDM_EXIT:
        ::PostQuitMessage(0);
        break;
    default:
        break;
    }
}

void WinApp::openSettings() {
    SettingsServer::Callbacks cb;
    cb.getStateJson = [this]() -> std::string {
        std::lock_guard<std::mutex> lock(configMutex_);
        return serializeSettingsStateJson(config_, fileSizeUtf8(historyPath_));
    };
    cb.saveStateJson = [this](const std::string &body) -> std::pair<bool, std::string> {
        AppConfig next;
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            next = config_;
        }
        if (!applySettingsStateJson(body, next)) {
            return {false, "{\"error\":\"配置 JSON 格式无效\"}"};
        }
        if (const auto *m = next.findActiveModel();
            m && m->thinkingMode == "custom" && !m->extraRequestJson.empty()) {
            yuntype::LlmConfig probe = next.toLlmConfig();
            if (yuntype::extraRequestFieldsFragment(probe).empty()) {
                return {false, "{\"error\":\"自定义额外请求 JSON 格式无效，须为合法 JSON 对象\"}"};
            }
        }
        if (!saveConfigToDir(configDir_, next)) {
            return {false, "{\"error\":\"写入配置文件失败\"}"};
        }
        syncAutoStartRegistry(next.autoStart, currentExePathUtf8());
        std::string resp;
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            config_ = next;
            resp = serializeSettingsStateJson(config_, fileSizeUtf8(historyPath_));
        }
        if (ctrlHwnd_) {
            ::PostMessageW(ctrlHwnd_, WM_YT_RELOAD_CONFIG, 0, 0);
        }
        return {true, std::move(resp)};
    };
    cb.clearHistory = [this]() {
        if (ctrlHwnd_) {
            ::PostMessageW(ctrlHwnd_, WM_YT_CLEAR_HISTORY, 0, 0);
        }
    };
    settingsServer_.openSettingsUi(std::move(cb));
}

void WinApp::reloadConfigAndSync() {
    AppConfig snap;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        snap = config_;
    }
    triggerSpec_ = parseTriggerKey(snap.triggerKey);
    enabledAtomic_.store(snap.enabled);
    client_.configure(snap.toLlmConfig(configDir_ + "\\api.key"));
    if (ctrlHwnd_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = ctrlHwnd_;
        nid.uID = kTrayIconId;
        nid.uFlags = NIF_TIP;
        const std::wstring tip = L"\u4E91\u6253 YunType (" + utf8ToWide(snap.triggerKey) + L")";
        ::wcsncpy(nid.szTip, tip.c_str(), std::size(nid.szTip) - 1);
        ::Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    if (!snap.historyEnabled) {
        client_.cancelCompaction();
        historyDraft_.reset();
        shadowDraft_.reset();
        cancelPostCommitSync();
    }
    syncCompactionTimer();
    schedulePersistTimer();
}

void WinApp::loadHistoryFromDisk() {
    historyDirty_ = false;
    historyDirtySinceUsec_ = 0;
    if (history_.loadFromFile(historyPath_)) {
        notesSentThrough_ = 1;
        historyRevision_ = std::max<std::uint64_t>(historyRevision_, 2);
    }
}

void WinApp::flushHistoryToDisk() {
    if (!historyDirty_ || historyPath_.empty()) {
        schedulePersistTimer();
        return;
    }
    if (history_.saveToFile(historyPath_, notesSentThrough_)) {
        historyDirty_ = false;
        historyDirtySinceUsec_ = 0;
    }
    schedulePersistTimer();
}

void WinApp::markHistoryDirty() {
    if (!config_.historyEnabled || historyPath_.empty()) {
        return;
    }
    const std::uint64_t now = monotonicNowUsec();
    if (!historyDirty_) {
        historyDirty_ = true;
        historyDirtySinceUsec_ = now;
    }
    historyLastMutatedUsec_ = now;
    schedulePersistTimer();
}

void WinApp::onHistoryMutated() {
    markHistoryDirty();
    if (history_.compactionStale()) {
        client_.cancelCompaction();
        history_.deferCompaction();
    }
    pollCompaction();
}

void WinApp::clearHistory() {
    client_.cancelCompaction();
    historyDirty_ = false;
    historyDirtySinceUsec_ = 0;
    history_ = {};
    historyClearedAt_ = ++historyRevision_;
    notesSentThrough_ = 0;
    historyDraft_.reset();
    shadowDraft_.reset();
    cancelPostCommitSync();
    syncCompactionTimer();
    schedulePersistTimer();
    std::error_code ec;
    const std::filesystem::path p(
        std::u8string(reinterpret_cast<const char8_t *>(historyPath_.data()), historyPath_.size()));
    std::filesystem::remove(p, ec);
}

bool WinApp::isTriggerMatch(DWORD vk) const {
    if (vk != triggerSpec_.vk) {
        return false;
    }
    return currentModifierMask() == triggerSpec_.modifiers;
}

bool WinApp::hasActiveSession() const {
    return draftActive_ || pendingCandidates_.has_value() || !queue_.empty();
}

char WinApp::vkToPrintableAscii(DWORD vk, bool shift, bool caps) const {
    if (vk >= 'A' && vk <= 'Z') {
        const bool upper = (shift != caps);
        return static_cast<char>(upper ? vk : (vk - 'A' + 'a'));
    }
    if (vk >= '0' && vk <= '9') {
        static constexpr char kShiftedDigits[] = ")!@#$%^&*(";
        return shift ? kShiftedDigits[vk - '0'] : static_cast<char>(vk);
    }
    switch (vk) {
    case VK_SPACE: return ' ';
    case VK_OEM_MINUS: return shift ? '_' : '-';
    case VK_OEM_PLUS: return shift ? '+' : '=';
    case VK_OEM_1: return shift ? ':' : ';';
    case VK_OEM_2: return shift ? '?' : '/';
    case VK_OEM_3: return shift ? '~' : '`';
    case VK_OEM_4: return shift ? '{' : '[';
    case VK_OEM_5: return shift ? '|' : '\\';
    case VK_OEM_6: return shift ? '}' : ']';
    case VK_OEM_7: return shift ? '"' : '\'';
    case VK_OEM_COMMA: return shift ? '<' : ',';
    case VK_OEM_PERIOD: return shift ? '>' : '.';
    case VK_ADD: return '+';
    case VK_SUBTRACT: return '-';
    default: return 0;
    }
}

LRESULT CALLBACK WinApp::ctrlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WinApp *self = instance_;
    if (!self) {
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    switch (msg) {
    case WM_YT_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            self->showTrayMenu();
        } else if (lp == WM_LBUTTONDBLCLK) {
            self->openSettings();
        }
        return 0;
    case WM_YT_TRIGGER:
        self->onTrigger();
        return 0;
    case WM_YT_ACTIVE_KEY: {
        bool shift = false, ctrl = false, alt = false;
        char ascii = 0;
        unpackKeyFlags(lp, shift, ctrl, alt, ascii);
        self->onActiveKey(static_cast<DWORD>(wp), shift, ctrl, alt, ascii);
        return 0;
    }
    case WM_YT_PASSIVE_KEY: {
        bool shift = false, ctrl = false, alt = false;
        char ascii = 0;
        unpackKeyFlags(lp, shift, ctrl, alt, ascii);
        self->onPassiveKey(static_cast<DWORD>(wp), shift, ctrl, alt, ascii);
        return 0;
    }
    case WM_YT_FG_CHANGED:
        self->onForegroundChanged(reinterpret_cast<HWND>(wp));
        return 0;
    case WM_YT_CAPTURE_START:
        self->onCaptureStart(reinterpret_cast<HWND>(wp));
        return 0;
    case WM_YT_MOUSE_CLICK:
        self->onMouseClick(POINT{static_cast<LONG>(wp), static_cast<LONG>(lp)});
        return 0;
    case WM_YT_RELOAD_CONFIG:
        self->reloadConfigAndSync();
        return 0;
    case WM_YT_CLEAR_HISTORY:
        self->clearHistory();
        return 0;
    case WM_TIMER:
        switch (wp) {
        case kTimerPollId:
            self->pollQueue();
            break;
        case kTimerCompactionId:
            self->pollCompaction();
            break;
        case kTimerPersistId:
            self->onPersistTimer();
            break;
        case kTimerPostCommitId:
            self->onPostCommitSyncTimer();
            break;
        default:
            break;
        }
        return 0;
    case WM_DESTROY:
        ::KillTimer(hwnd, kTimerPollId);
        ::KillTimer(hwnd, kTimerCompactionId);
        ::KillTimer(hwnd, kTimerPersistId);
        ::KillTimer(hwnd, kTimerPostCommitId);
        self->pollTimerActive_ = false;
        self->compactionTimerActive_ = false;
        self->persistTimerActive_ = false;
        self->postCommitTimerActive_ = false;
        return 0;
    default:
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}

LRESULT CALLBACK WinApp::lowLevelKeyboardProc(int code, WPARAM wp, LPARAM lp) {
    WinApp *self = instance_;
    if (code != HC_ACTION || !self || !self->ctrlHwnd_) {
        return ::CallNextHookEx(nullptr, code, wp, lp);
    }
    const auto *kb = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lp);
    if ((kb->flags & LLKHF_INJECTED) != 0 || kb->dwExtraInfo == kYunTypeInjectedTag) {
        return ::CallNextHookEx(nullptr, code, wp, lp);
    }
    const DWORD vk = kb->vkCode;
    const bool isDown = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
    const bool isUp = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
    const std::uint64_t nowMs = ::GetTickCount64();

    // Swallow matching keyup of trigger hotkey and reset debounce window immediately
    if (isUp && vk == self->triggerSpec_.vk) {
        const std::uint64_t prev = self->lastTriggerTickMs_.load();
        if (prev != 0 && nowMs >= prev && nowMs - prev < 1500) {
            self->lastTriggerTickMs_.store(0);
            return 1;
        }
    }
    if (!isDown) {
        return ::CallNextHookEx(nullptr, code, wp, lp);
    }

    if (isModifierVk(vk)) {
        return ::CallNextHookEx(nullptr, code, wp, lp);
    }

    const bool shift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool caps = (::GetKeyState(VK_CAPITAL) & 0x0001) != 0;
    const char ascii = (ctrl || alt) ? 0 : self->vkToPrintableAscii(vk, shift, caps);

    if (self->enabledAtomic_.load() && self->isTriggerMatch(vk)) {
        const std::uint64_t prev = self->lastTriggerTickMs_.load();
        if (prev != 0 && nowMs >= prev && nowMs - prev < 180) {
            return 1; // Ignore key auto-repeat while trigger key is held
        }
        self->lastTriggerTickMs_.store(nowMs);
        if (alt) {
            suppressAltMenuActivation();
        }
        ::PostMessageW(self->ctrlHwnd_, WM_YT_TRIGGER, 0, 0);
        return 1; // Swallow trigger hotkey
    }

    if (self->activeSessionAtomic_.load()) {
        // Swallow active session keys so draft/candidate navigation never leaks into the app
        ::PostMessageW(self->ctrlHwnd_, WM_YT_ACTIVE_KEY, static_cast<WPARAM>(vk),
                       packKeyFlags(shift, ctrl, alt, ascii));
        return 1;
    }

    // Idle state: never block, observe asynchronously for pinyin buffer & shadow edits
    ::PostMessageW(self->ctrlHwnd_, WM_YT_PASSIVE_KEY, static_cast<WPARAM>(vk),
                   packKeyFlags(shift, ctrl, alt, ascii));
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK WinApp::lowLevelMouseProc(int code, WPARAM wp, LPARAM lp) {
    WinApp *self = instance_;
    if (code == HC_ACTION && self && wp == WM_LBUTTONDOWN) {
        const auto *ms = reinterpret_cast<const MSLLHOOKSTRUCT *>(lp);
        if (self->ctrlHwnd_) {
            ::PostMessageW(self->ctrlHwnd_, WM_YT_MOUSE_CLICK,
                           static_cast<WPARAM>(ms->pt.x), static_cast<LPARAM>(ms->pt.y));
        }
    }
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

void WinApp::syncMouseHook(bool active) {
    if (active) {
        if (!mouseHook_ && hInst_) {
            mouseHook_ = ::SetWindowsHookExW(WH_MOUSE_LL, &WinApp::lowLevelMouseProc, hInst_, 0);
        }
    } else if (mouseHook_) {
        ::UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
}

void CALLBACK WinApp::winEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (!instance_ || !instance_->ctrlHwnd_) {
        return;
    }
    if (event == EVENT_SYSTEM_FOREGROUND) {
        ::PostMessageW(instance_->ctrlHwnd_, WM_YT_FG_CHANGED, reinterpret_cast<WPARAM>(hwnd), 0);
    } else if (event == EVENT_SYSTEM_CAPTURESTART) {
        ::PostMessageW(instance_->ctrlHwnd_, WM_YT_CAPTURE_START, reinterpret_cast<WPARAM>(hwnd), 0);
    }
}

void WinApp::saveWindowAscii(HWND hwnd) {
    if (!hwnd || appAscii_.empty()) {
        return;
    }
    const std::uint64_t now = monotonicNowUsec();
    savedWindowAscii_[hwnd] = SavedWindowAscii{appAscii_, now};
    for (auto it = savedWindowAscii_.begin(); it != savedWindowAscii_.end();) {
        if (now >= it->second.savedUsec && (now - it->second.savedUsec) > kWindowAsciiTtlUsec) {
            it = savedWindowAscii_.erase(it);
        } else {
            ++it;
        }
    }
    while (savedWindowAscii_.size() > kMaxSavedWindowAscii) {
        auto oldest = savedWindowAscii_.begin();
        for (auto it = savedWindowAscii_.begin(); it != savedWindowAscii_.end(); ++it) {
            if (it->second.savedUsec < oldest->second.savedUsec) {
                oldest = it;
            }
        }
        savedWindowAscii_.erase(oldest);
    }
}

void WinApp::onForegroundChanged(HWND newFg) {
    if (newFg == overlay_.hwnd() || newFg == ctrlHwnd_) {
        return;
    }
    lastTriggerTickMs_.store(0);
    if (newFg != lastFgWnd_) {
        if (hasActiveSession()) {
            resetActiveSession();
        }
        cancelPostCommitSync();
        saveWindowAscii(lastFgWnd_);
        appAscii_.clear();
        selectionLikely_ = false;
        mouseCaptureSeen_ = false;
        lastCommittedText_.clear();
        historyDraft_.reset();
        shadowDraft_.reset();
        lastFgWnd_ = newFg;
        lastFgChangedUsec_ = monotonicNowUsec();
        if (newFg) {
            auto it = savedWindowAscii_.find(newFg);
            if (it != savedWindowAscii_.end()) {
                const std::uint64_t now = monotonicNowUsec();
                if (now >= it->second.savedUsec &&
                    (now - it->second.savedUsec) <= kWindowAsciiTtlUsec) {
                    appAscii_ = std::move(it->second.text);
                }
                savedWindowAscii_.erase(it);
            }
        }
    }
}

void WinApp::onCaptureStart(HWND hwnd) {
    if (!hwnd || hwnd == overlay_.hwnd() || hwnd == ctrlHwnd_) {
        return;
    }
    HWND root = ::GetAncestor(hwnd, GA_ROOT);
    if (!root) {
        root = hwnd;
    }
    if (root == lastFgWnd_) {
        mouseCaptureSeen_ = true;
        const std::uint64_t now = monotonicNowUsec();
        // Keep freshly restored per-window ASCII on immediate window-refocus clicks;
        // clear stale tracked ASCII for mid-session clicks/drags inside the active window.
        if (now >= lastFgChangedUsec_ && (now - lastFgChangedUsec_) >= 200000ULL) {
            appAscii_.clear();
            savedWindowAscii_.erase(lastFgWnd_);
        }
    }
}

void WinApp::onMouseClick(POINT pt) {
    if (!hasActiveSession()) {
        syncMouseHook(false);
        return;
    }
    if (overlay_.isVisible() && overlay_.hwnd()) {
        RECT ovRc{};
        if (::GetWindowRect(overlay_.hwnd(), &ovRc) && ::PtInRect(&ovRc, pt)) {
            return; // Click is inside the overlay (candidate selection)
        }
    }
    resetActiveSession();
}

void WinApp::onTrigger() {
    if (!config_.enabled) {
        return;
    }
    if (pendingCandidates_) {
        commitPendingCandidate(pendingCandidates_->globalCursor);
        return;
    }
    if (draftActive_) {
        if (!draft_.empty()) {
            std::string q = std::move(draft_);
            const std::size_t eraseChars = draftEraseChars_;
            std::string eraseSuffix = std::move(draftEraseSuffix_);
            const bool repSel = draftReplaceSelection_;
            draft_.clear();
            draftCursor_ = 0;
            draftEraseChars_ = 0;
            draftEraseSuffix_.clear();
            draftReplaceSelection_ = false;
            draftActive_ = false;
            submitSentence(std::move(q), eraseChars, std::move(eraseSuffix), repSel);
        } else {
            draftActive_ = false;
            draftEraseChars_ = 0;
            draftEraseSuffix_.clear();
            draftReplaceSelection_ = false;
            activeSessionAtomic_.store(hasActiveSession());
            refreshOverlay();
        }
        return;
    }

    HWND fg = ::GetForegroundWindow();
    if (fg && fg != lastFgWnd_) {
        saveWindowAscii(lastFgWnd_);
        lastFgWnd_ = fg;
        lastFgChangedUsec_ = monotonicNowUsec();
        if (appAscii_.empty()) {
            auto it = savedWindowAscii_.find(fg);
            if (it != savedWindowAscii_.end()) {
                const std::uint64_t now = monotonicNowUsec();
                if (now >= it->second.savedUsec &&
                    (now - it->second.savedUsec) <= kWindowAsciiTtlUsec) {
                    appAscii_ = std::move(it->second.text);
                }
                savedWindowAscii_.erase(it);
            }
        }
    }
    activeAnchor_ = caret_.queryCaret(fg, std::nullopt).rect;
    auto snap = caret_.readSurrounding(fg);

    // 1. Active selection via UIA (including Ctrl+A select-all): convert selected text directly
    //    and overwrite in-place with zero BackSpaces!
    if (snap && snap->end > snap->start && snap->end <= snap->text.size()) {
        std::string selected = snap->text.substr(snap->start, snap->end - snap->start);
        if (yuntype::hasAsciiAlpha(selected)) {
            selectionLikely_ = false;
            mouseCaptureSeen_ = false;
            appAscii_.clear();
            if (fg) {
                savedWindowAscii_.erase(fg);
            }
            submitSentence(std::move(selected), 0, {}, true);
            return;
        }
    }

    // 1b. Active selection in non-UIA apps without any idle mouse hook:
    //     - Scintilla (Notepad3 / Notepad++): exact native SCI_GETSELECTIONSTART/END check so
    //       mouse drags, double-clicks, and Ctrl+A all work while never copying an unselected line.
    //     - Explicit keyboard selections (Ctrl+A, Shift/Ctrl+Shift + navigation) in any non-UIA app.
    //     - WeChat / WPS (safe empty-Ctrl+C apps): probe silent Ctrl+C when mouse capture occurred
    //       or tracked ASCII is empty (mouse drag / double-click on earlier lines).
    const auto sciState = queryScintillaSelection(fg);
    const bool tryClipboardSelection =
        (sciState == ScintillaSelectionState::NonEmpty) ||
        (selectionLikely_ && sciState != ScintillaSelectionState::Empty) ||
        (sciState == ScintillaSelectionState::NotScintilla &&
         !isUnsafeEmptyCtrlCWindow(fg) &&
         (mouseCaptureSeen_ || appAscii_.empty()));
    selectionLikely_ = false;
    mouseCaptureSeen_ = false;
    if (tryClipboardSelection) {
        if (auto clipSel = readSelectionViaClipboard(fg)) {
            if (yuntype::hasAsciiAlpha(*clipSel)) {
                appAscii_.clear();
                if (fg) {
                    savedWindowAscii_.erase(fg);
                }
                submitSentence(std::move(*clipSel), 0, {}, true);
                return;
            }
        }
    }

    // 2. Recently typed ASCII pinyin suffix: trust appAscii_ directly (VS Code Monaco and other
    //    custom editors often return empty/stale UIA text, so never let UIA veto tracked ASCII!).
    //    Chinese IME input is already excluded at capture time in onPassiveKey().
    std::string suffix = yuntype::extractReplaceablePinyinSuffix(appAscii_);

    // 3. Focus-loss / click-reposition recovery: when appAscii_ is empty, inspect ASCII pinyin
    //    immediately before the caret via UIA (skipping freshly committed English).
    if (suffix.empty() && snap && snap->start == snap->end && snap->start > 0) {
        const std::string_view beforeCaret =
            std::string_view(snap->text).substr(0, snap->start);
        std::string recovered = yuntype::extractReplaceablePinyinSuffix(beforeCaret);
        if (!recovered.empty()) {
            const std::uint64_t now = monotonicNowUsec();
            const bool isFreshCommitSuffix =
                !lastCommittedText_.empty() &&
                lastCommittedText_.ends_with(recovered) &&
                now >= lastCommitUsec_ && (now - lastCommitUsec_) < 5000000ULL;
            if (!isFreshCommitSuffix) {
                suffix = std::move(recovered);
            }
        }
    }

    if (!suffix.empty()) {
        const std::size_t eraseCount = yuntype::utf8Length(suffix);
        std::string eraseSuffix = suffix;
        appAscii_.clear();
        submitSentence(std::move(suffix), eraseCount, std::move(eraseSuffix), false);
        return;
    }

    // 4. Otherwise enter clean draft mode anchored right next to the caret
    draftActive_ = true;
    draft_.clear();
    draftCursor_ = 0;
    draftEraseChars_ = 0;
    draftEraseSuffix_.clear();
    draftReplaceSelection_ = false;
    activeSessionAtomic_.store(true);
    refreshOverlay();
}

void WinApp::onActiveKey(DWORD vk, bool shift, bool ctrl, bool alt, char asciiCh) {
    (void)alt;
    if (pendingCandidates_) {
        handleCandidateKey(vk, shift, ctrl, asciiCh);
        return;
    }
    if (draftActive_) {
        handleDraftKey(vk, ctrl, asciiCh);
        return;
    }
    if (!queue_.empty()) {
        handleQueueKey(vk, asciiCh);
        return;
    }
    activeSessionAtomic_.store(false);
}

void WinApp::onPassiveKey(DWORD vk, bool shift, bool ctrl, bool alt, char asciiCh) {
    const std::uint64_t now = monotonicNowUsec();
    if (!lastFgWnd_) {
        lastFgWnd_ = ::GetForegroundWindow();
    }

    // Track explicit keyboard selection gestures (Ctrl+A, Shift+navigation, Ctrl+Shift+navigation)
    // so non-UIA apps (WeChat, Notepad3, WPS) can read the selection via silent Ctrl+C on trigger.
    if (ctrl && !alt && !shift && vk == 'A') {
        selectionLikely_ = true;
        appAscii_.clear();
        if (lastFgWnd_) {
            savedWindowAscii_.erase(lastFgWnd_);
        }
        return;
    }
    if (shift && !alt &&
        (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
         vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT)) {
        selectionLikely_ = true;
        appAscii_.clear();
        if (lastFgWnd_) {
            savedWindowAscii_.erase(lastFgWnd_);
        }
        return;
    }

    if (ctrl || alt) {
        if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU) {
            appAscii_.clear();
            if (!(ctrl && !alt && vk == 'C')) {
                selectionLikely_ = false;
                mouseCaptureSeen_ = false;
            }
        }
        return;
    }

    if (asciiCh >= 0x20 && asciiCh <= 0x7E) {
        selectionLikely_ = false;
        mouseCaptureSeen_ = false;
        if (isForegroundChineseImeActive()) {
            appAscii_.clear();
            return;
        }
        appAscii_.push_back(asciiCh);
        if (appAscii_.size() > kMaxAsciiTrackBytes) {
            appAscii_.erase(0, appAscii_.size() - kMaxAsciiTrackBytes);
        }
        if (config_.historyEnabled && shadowDraft_) {
            std::string s(1, asciiCh);
            if (history_.shadowInsertAscii(shadowDraft_, s, ++historyRevision_, now,
                                           kShadowTimeoutUsec)) {
                onHistoryMutated();
            }
        }
        return;
    }

    switch (vk) {
    case VK_BACK:
        selectionLikely_ = false;
        mouseCaptureSeen_ = false;
        if (!appAscii_.empty()) {
            appAscii_.pop_back();
        }
        if (config_.historyEnabled && shadowDraft_) {
            if (history_.shadowBackspace(shadowDraft_, ++historyRevision_, now,
                                         kShadowTimeoutUsec)) {
                onHistoryMutated();
            }
        }
        break;
    case VK_DELETE:
        selectionLikely_ = false;
        mouseCaptureSeen_ = false;
        appAscii_.clear();
        if (config_.historyEnabled && shadowDraft_) {
            if (history_.shadowDelete(shadowDraft_, ++historyRevision_, now, kShadowTimeoutUsec)) {
                onHistoryMutated();
            }
        }
        break;
    case VK_LEFT:
        selectionLikely_ = false;
        mouseCaptureSeen_ = false;
        appAscii_.clear();
        if (config_.historyEnabled && shadowDraft_) {
            history_.shadowMoveLeft(shadowDraft_, now, kShadowTimeoutUsec);
        }
        break;
    case VK_RIGHT:
        selectionLikely_ = false;
        mouseCaptureSeen_ = false;
        appAscii_.clear();
        if (config_.historyEnabled && shadowDraft_) {
            history_.shadowMoveRight(shadowDraft_, now, kShadowTimeoutUsec);
        }
        break;
    case VK_RETURN:
    case VK_UP:
    case VK_DOWN:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_TAB:
    case VK_ESCAPE:
        selectionLikely_ = false;
        mouseCaptureSeen_ = false;
        appAscii_.clear();
        shadowDraft_.reset();
        break;
    default:
        break;
    }
}

void WinApp::handleDraftKey(DWORD vk, bool ctrl, char asciiCh) {
    if (vk == VK_ESCAPE) {
        draftActive_ = false;
        draft_.clear();
        draftCursor_ = 0;
        draftEraseChars_ = 0;
        draftEraseSuffix_.clear();
        draftReplaceSelection_ = false;
        activeSessionAtomic_.store(hasActiveSession());
        refreshOverlay();
        return;
    }
    if (vk == VK_RETURN) {
        std::string q = std::move(draft_);
        const std::size_t eraseChars = draftEraseChars_;
        std::string eraseSuffix = std::move(draftEraseSuffix_);
        const bool repSel = draftReplaceSelection_;
        draft_.clear();
        draftCursor_ = 0;
        draftEraseChars_ = 0;
        draftEraseSuffix_.clear();
        draftReplaceSelection_ = false;
        draftActive_ = false;
        if (!q.empty()) {
            submitSentence(std::move(q), eraseChars, std::move(eraseSuffix), repSel);
        } else {
            activeSessionAtomic_.store(hasActiveSession());
            refreshOverlay();
        }
        return;
    }
    if (vk == VK_BACK) {
        if (draftCursor_ > 0) {
            draft_.erase(draftCursor_ - 1, 1);
            --draftCursor_;
        } else if (draft_.empty()) {
            draftActive_ = false;
            draftEraseChars_ = 0;
            draftEraseSuffix_.clear();
            draftReplaceSelection_ = false;
            activeSessionAtomic_.store(hasActiveSession());
        }
        refreshOverlay();
        return;
    }
    if (vk == VK_DELETE) {
        if (draftCursor_ < draft_.size()) {
            draft_.erase(draftCursor_, 1);
            refreshOverlay();
        }
        return;
    }
    if (vk == VK_LEFT) {
        if (draftCursor_ > 0) --draftCursor_;
        refreshOverlay();
        return;
    }
    if (vk == VK_RIGHT) {
        if (draftCursor_ < draft_.size()) ++draftCursor_;
        refreshOverlay();
        return;
    }
    if (vk == VK_HOME) {
        draftCursor_ = 0;
        refreshOverlay();
        return;
    }
    if (vk == VK_END) {
        draftCursor_ = draft_.size();
        refreshOverlay();
        return;
    }
    if (ctrl) {
        return;
    }
    if (asciiCh >= 0x20 && asciiCh <= 0x7E) {
        draft_.insert(draftCursor_, 1, asciiCh);
        ++draftCursor_;
        refreshOverlay();
    }
}

void WinApp::handleQueueKey(DWORD vk, char asciiCh) {
    if (vk == VK_ESCAPE) {
        resetActiveSession();
        return;
    }
    if (vk == VK_RETURN && !queue_.empty()) {
        std::string fallback = queue_.front().fallback;
        const std::size_t eraseChars = queue_.front().pendingEraseChars;
        std::string eraseSuffix = queue_.front().pendingEraseSuffix;
        const bool repSel = queue_.front().replaceSelection;
        client_.cancelSentence();
        queue_.pop_front();
        syncPollTimer();
        // If text is already in the app and equals fallback, just close without rewriting!
        if ((eraseChars > 0 && fallback == eraseSuffix) || repSel) {
            activeSessionAtomic_.store(hasActiveSession());
            refreshOverlay();
            return;
        }
        commitSentenceResult(std::move(fallback), {}, 0, {}, false, eraseChars, eraseSuffix, repSel);
        activeSessionAtomic_.store(hasActiveSession());
        refreshOverlay();
        return;
    }
    (void)asciiCh;
}

void WinApp::handleCandidateKey(DWORD vk, bool shift, bool ctrl, char asciiCh) {
    if (!pendingCandidates_) {
        return;
    }
    auto &pc = *pendingCandidates_;
    const int total = static_cast<int>(pc.candidates.size());
    const int pageSize = std::clamp(config_.candidatePageSize, 2, 10);
    const int curPage = pc.globalCursor / pageSize;
    const int totalPages = std::max(1, (total + pageSize - 1) / pageSize);

    if (vk == VK_ESCAPE) {
        resetActiveSession();
        return;
    }
    if (vk == VK_BACK) {
        if (pc.loadingMore) {
            client_.cancelSentence();
        }
        draft_ = pc.query;
        draftCursor_ = draft_.size();
        draftEraseChars_ = pc.pendingEraseChars;
        draftEraseSuffix_ = std::move(pc.pendingEraseSuffix);
        draftReplaceSelection_ = pc.replaceSelection;
        draftActive_ = true;
        pendingCandidates_.reset();
        syncPollTimer();
        activeSessionAtomic_.store(true);
        refreshOverlay();
        return;
    }
    if (vk == VK_RETURN || vk == VK_SPACE) {
        commitPendingCandidate(pc.globalCursor);
        return;
    }
    if (!ctrl && !shift && vk >= '0' && vk <= '9') {
        const int digitIdx = (vk == '0') ? 9 : static_cast<int>(vk - '1');
        if (digitIdx < pageSize) {
            const int target = curPage * pageSize + digitIdx;
            if (target < total) {
                commitPendingCandidate(target);
                return;
            }
        }
    }
    if (vk == VK_UP || vk == VK_LEFT) {
        pc.globalCursor = (pc.globalCursor - 1 + total) % total;
        refreshOverlay();
        return;
    }
    if (vk == VK_DOWN || vk == VK_RIGHT || vk == VK_TAB) {
        pc.globalCursor = (pc.globalCursor + 1) % total;
        refreshOverlay();
        return;
    }
    if (vk == VK_HOME) {
        pc.globalCursor = 0;
        refreshOverlay();
        return;
    }
    if (vk == VK_END) {
        pc.globalCursor = std::max(0, total - 1);
        refreshOverlay();
        return;
    }
    // Load more directly via '+' (Shift+=) or Numpad '+'
    if ((vk == VK_OEM_PLUS && shift) || vk == VK_ADD) {
        startLoadMoreCandidates();
        return;
    }
    // Previous page: '-', Numpad '-', PageUp
    if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT || vk == VK_PRIOR) {
        if (curPage > 0) {
            pc.globalCursor = (curPage - 1) * pageSize;
            refreshOverlay();
        }
        return;
    }
    // Next page or Load More on last page: '=', PageDown
    if ((vk == VK_OEM_PLUS && !shift) || vk == VK_NEXT) {
        if (curPage + 1 < totalPages) {
            pc.globalCursor = (curPage + 1) * pageSize;
            refreshOverlay();
        } else {
            startLoadMoreCandidates();
        }
        return;
    }
    // Printable ASCII continuation: commit highlighted candidate first, then pass char through!
    if (!ctrl && asciiCh >= 0x20 && asciiCh <= 0x7E) {
        commitPendingCandidate(pc.globalCursor);
        std::string passthrough(1, asciiCh);
        sendUnicodeText(passthrough);
        onPassiveKey(vk, shift, false, false, asciiCh);
    }
}

void WinApp::submitSentence(std::string query, std::size_t pendingEraseChars,
                            std::string pendingEraseSuffix, bool replaceSelection) {
    const bool needsLlm = yuntype::hasAsciiAlpha(query);
    queue_.push_back(SubmittedSentence{
        query,
        query,
        pendingEraseChars,
        std::move(pendingEraseSuffix),
        replaceSelection,
        false,
        needsLlm ? std::nullopt : std::optional<std::string>(query),
        std::nullopt,
    });
    activeSessionAtomic_.store(true);
    pollQueue();
    syncPollTimer();
    refreshOverlay();
}

yuntype::SentenceContext WinApp::buildHistoryContext(bool candidateMode) {
    if (!config_.historyEnabled) {
        return {};
    }
    HWND fg = lastFgWnd_ ? lastFgWnd_ : ::GetForegroundWindow();
    if (historyDraft_) {
        if (auto snap = caret_.readSurrounding(fg)) {
            if (history_.update(historyDraft_, snap, ++historyRevision_)) {
                onHistoryMutated();
            }
        }
    }
    const std::size_t budget = client_.historyBudget();
    const std::size_t corrBudget = std::max<std::size_t>(budget / 3, 256);
    if (candidateMode) {
        return yuntype::SentenceContext{
            {},
            {},
            {},
            history_.corrections(std::min<std::size_t>(corrBudget, 1024), 0),
            historyRevision_,
            false,
        };
    }
    std::string turns = history_.replayTurns(budget);
    const std::uint64_t after = turns.empty() ? 0 : notesSentThrough_;
    return yuntype::SentenceContext{
        std::move(turns),
        history_.newText(budget, after),
        history_.summary(),
        history_.corrections(corrBudget, after),
        historyRevision_,
        true,
    };
}

void WinApp::startLoadMoreCandidates() {
    if (!pendingCandidates_) {
        return;
    }
    auto &pc = *pendingCandidates_;
    if (pc.loadingMore) {
        return;
    }
    if (pc.loadMoreRounds >= kMaxLoadMoreRounds) {
        pc.statusNote = "·";
        refreshOverlay();
        return;
    }
    if (pc.sessionTurns.empty()) {
        pc.statusNote = "·";
        refreshOverlay();
        return;
    }
    pc.loadingMore = true;
    pc.statusNote.clear();
    client_.startMoreCandidates(pc.query, pc.sessionTurns, pc.sentRev);
    syncPollTimer();
    refreshOverlay();
}

void WinApp::commitPendingCandidate(int globalIndex) {
    if (!pendingCandidates_) {
        return;
    }
    auto pc = std::move(*pendingCandidates_);
    pendingCandidates_.reset();
    if (pc.loadingMore) {
        client_.cancelSentence();
    }
    syncPollTimer();
    activeSessionAtomic_.store(hasActiveSession());
    if (!hasActiveSession()) {
        overlay_.hide();
        syncMouseHook(false);
    }
    if (pc.candidates.empty()) {
        refreshOverlay();
        return;
    }
    const std::size_t idx =
        static_cast<std::size_t>(std::clamp(globalIndex, 0, static_cast<int>(pc.candidates.size()) - 1));
    const std::string chosen = pc.candidates[idx];
    const std::string firstCand = pc.candidates.front();
    commitSentenceResult(chosen, {}, pc.sentRev, firstCand, true, pc.pendingEraseChars,
                         pc.pendingEraseSuffix, pc.replaceSelection);
    refreshOverlay();
}

void WinApp::commitSentenceResult(std::string committed, std::string turnJson,
                                  std::uint64_t sentRev, std::string_view llmOriginal,
                                  bool candidateMode, std::size_t eraseChars,
                                  std::string_view eraseSuffix, bool replaceSelection) {
    appAscii_.clear();
    selectionLikely_ = false;
    mouseCaptureSeen_ = false;
    if (!hasActiveSession()) {
        overlay_.hide();
        syncMouseHook(false);
    }
    HWND fg = lastFgWnd_ ? lastFgWnd_ : ::GetForegroundWindow();
    if (fg) {
        savedWindowAscii_.erase(fg);
    }
    const bool allowHistory =
        config_.historyEnabled &&
        ((turnJson.empty() && !candidateMode) || sentRev >= historyClearedAt_);
    if (allowHistory) {
        const std::uint64_t now = monotonicNowUsec();
        if (!replaceSelection && eraseChars > 0 && !eraseSuffix.empty() && shadowDraft_) {
            if (history_.shadowEraseSuffixBeforeCursor(shadowDraft_, eraseSuffix,
                                                       ++historyRevision_, now,
                                                       kShadowTimeoutUsec)) {
                onHistoryMutated();
            }
        }
        auto snap = caret_.readSurrounding(fg);
        if (!replaceSelection && snap && eraseChars > 0 && !eraseSuffix.empty() &&
            snap->start == snap->end && snap->start >= eraseSuffix.size()) {
            const std::size_t suffixStart = snap->start - eraseSuffix.size();
            if (std::string_view(snap->text).substr(suffixStart, eraseSuffix.size()) ==
                eraseSuffix) {
                snap->text.erase(suffixStart, eraseSuffix.size());
                snap->start = suffixStart;
                snap->end = suffixStart;
            }
        }
        if (candidateMode) {
            if (history_.recordShortCommit(historyDraft_, shadowDraft_, snap, committed,
                                           ++historyRevision_, now, kShadowTimeoutUsec,
                                           llmOriginal)) {
                onHistoryMutated();
            }
        } else {
            if (!turnJson.empty() && sentRev >= historyCompactedAt_) {
                notesSentThrough_ = std::max(notesSentThrough_, sentRev);
            }
            history_.recordCommit(historyDraft_, shadowDraft_, snap, committed,
                                  turnJson.empty() ? nullptr : turnJson.c_str(),
                                  ++historyRevision_, now, kShadowTimeoutUsec, llmOriginal);
            onHistoryMutated();
        }
    }

    // Scheme 3A: zero-BackSpace replacement when selection is already active OR UIA/Edit selects
    // the exact pinyin suffix in one instant call; fall back to fast SendInput BackSpaces otherwise.
    if (replaceSelection) {
        sendUnicodeText(committed);
    } else if (eraseChars > 0) {
        if (!eraseSuffix.empty() && caret_.selectSuffixBeforeCaret(fg, eraseSuffix)) {
            sendUnicodeText(committed);
        } else {
            sendReplaceText(eraseChars, committed);
        }
    } else {
        sendUnicodeText(committed);
    }
    lastCommittedText_ = committed;
    lastCommitUsec_ = monotonicNowUsec();
    if (config_.historyEnabled && historyDraft_) {
        schedulePostCommitSync();
    }
}

void WinApp::syncPollTimer() {
    if (!ctrlHwnd_) {
        return;
    }
    const bool need =
        (!queue_.empty() && !queue_.front().error.has_value()) ||
        (pendingCandidates_ && pendingCandidates_->loadingMore);
    if (need && !pollTimerActive_) {
        ::SetTimer(ctrlHwnd_, kTimerPollId, kPollIntervalMs, nullptr);
        pollTimerActive_ = true;
    } else if (!need && pollTimerActive_) {
        ::KillTimer(ctrlHwnd_, kTimerPollId);
        pollTimerActive_ = false;
    }
}

void WinApp::syncCompactionTimer() {
    if (!ctrlHwnd_) {
        return;
    }
    const bool need = config_.historyEnabled && history_.compacting();
    if (need && !compactionTimerActive_) {
        ::SetTimer(ctrlHwnd_, kTimerCompactionId, kCompactionIntervalMs, nullptr);
        compactionTimerActive_ = true;
    } else if (!need && compactionTimerActive_) {
        ::KillTimer(ctrlHwnd_, kTimerCompactionId);
        compactionTimerActive_ = false;
    }
}

void WinApp::schedulePersistTimer() {
    if (!ctrlHwnd_) {
        return;
    }
    if (!historyDirty_ || !config_.historyEnabled || historyPath_.empty()) {
        if (persistTimerActive_) {
            ::KillTimer(ctrlHwnd_, kTimerPersistId);
            persistTimerActive_ = false;
        }
        return;
    }
    const std::uint64_t now = monotonicNowUsec();
    const std::uint64_t idleUsec =
        static_cast<std::uint64_t>(std::max(1, config_.persistIdleSeconds)) * 1000000ULL;
    const std::uint64_t maxUsec =
        static_cast<std::uint64_t>(std::max(1, config_.persistMaxSeconds)) * 1000000ULL;
    const std::uint64_t due =
        std::min(historyLastMutatedUsec_ + idleUsec, historyDirtySinceUsec_ + maxUsec);
    const std::uint64_t remUsec = (due > now) ? (due - now) : 0;
    const UINT delayMs = static_cast<UINT>(
        std::clamp<std::uint64_t>((remUsec + 999ULL) / 1000ULL, 50ULL, 86400000ULL));
    ::SetTimer(ctrlHwnd_, kTimerPersistId, delayMs, nullptr);
    persistTimerActive_ = true;
}

void WinApp::schedulePostCommitSync() {
    if (!ctrlHwnd_) {
        return;
    }
    ::SetTimer(ctrlHwnd_, kTimerPostCommitId, kPostCommitSyncMs, nullptr);
    postCommitTimerActive_ = true;
}

void WinApp::cancelPostCommitSync() {
    if (ctrlHwnd_ && postCommitTimerActive_) {
        ::KillTimer(ctrlHwnd_, kTimerPostCommitId);
    }
    postCommitTimerActive_ = false;
}

void WinApp::onPersistTimer() {
    if (ctrlHwnd_ && persistTimerActive_) {
        ::KillTimer(ctrlHwnd_, kTimerPersistId);
    }
    persistTimerActive_ = false;
    if (!historyDirty_ || !config_.historyEnabled) {
        return;
    }
    const std::uint64_t now = monotonicNowUsec();
    const std::uint64_t idleUsec =
        static_cast<std::uint64_t>(std::max(1, config_.persistIdleSeconds)) * 1000000ULL;
    const std::uint64_t maxUsec =
        static_cast<std::uint64_t>(std::max(1, config_.persistMaxSeconds)) * 1000000ULL;
    if ((now >= historyLastMutatedUsec_ && now - historyLastMutatedUsec_ >= idleUsec) ||
        (now >= historyDirtySinceUsec_ && now - historyDirtySinceUsec_ >= maxUsec)) {
        flushHistoryToDisk();
    } else {
        schedulePersistTimer();
    }
}

void WinApp::onPostCommitSyncTimer() {
    cancelPostCommitSync();
    if (!config_.historyEnabled || !historyDraft_ || lastCommitUsec_ == 0) {
        return;
    }
    HWND fg = ::GetForegroundWindow();
    if (fg == lastFgWnd_) {
        if (auto snap = caret_.readSurrounding(fg)) {
            if (history_.update(historyDraft_, snap, ++historyRevision_)) {
                onHistoryMutated();
            }
        }
    }
}

void WinApp::pollQueue() {
    // 1. Poll in-flight Load More candidates request
    if (pendingCandidates_ && pendingCandidates_->loadingMore) {
        auto snap = client_.pollSentence();
        auto &pc = *pendingCandidates_;
        if (snap.state == yuntype::JobState::Done) {
            pc.loadingMore = false;
            ++pc.loadMoreRounds;
            if (!snap.turnJson.empty()) {
                pc.sessionTurns += snap.turnJson;
                pc.sessionTurns.push_back('\n');
            }
            const std::size_t beforeCount = pc.candidates.size();
            for (const auto &c : snap.candidates) {
                if (!c.empty() &&
                    std::find(pc.candidates.begin(), pc.candidates.end(), c) == pc.candidates.end()) {
                    pc.candidates.push_back(c);
                }
            }
            if (pc.candidates.size() > beforeCount) {
                const int pageSize = std::clamp(config_.candidatePageSize, 2, 10);
                const int newPage = static_cast<int>(beforeCount) / pageSize;
                pc.globalCursor = newPage * pageSize;
                pc.statusNote.clear();
            } else {
                pc.statusNote = "·";
            }
            client_.cancelSentence();
            syncPollTimer();
            refreshOverlay();
        } else if (snap.state == yuntype::JobState::Error) {
            pc.loadingMore = false;
            pc.statusNote = "!";
            client_.cancelSentence();
            syncPollTimer();
            refreshOverlay();
        }
        return;
    }

    if (queue_.empty()) {
        syncPollTimer();
        return;
    }

    auto &front = queue_.front();
    if (!front.started) {
        if (front.ready) {
            std::string out = std::move(*front.ready);
            const std::size_t eraseChars = front.pendingEraseChars;
            std::string eraseSuffix = std::move(front.pendingEraseSuffix);
            const bool repSel = front.replaceSelection;
            queue_.pop_front();
            activeSessionAtomic_.store(hasActiveSession());
            commitSentenceResult(std::move(out), {}, 0, {}, false, eraseChars, eraseSuffix, repSel);
            syncPollTimer();
            refreshOverlay();
            return;
        }
        front.started = true;
        const bool candMode = yuntype::shouldRequestCandidates(client_.config(), front.query);
        const auto ctx = buildHistoryContext(candMode);
        client_.startSentence(front.query, front.fallback, ctx);
        syncPollTimer();
        refreshOverlay();
        return;
    }

    auto snap = client_.pollSentence();
    if (snap.state == yuntype::JobState::Running) {
        if (!snap.text.empty()) {
            refreshOverlay();
        }
        return;
    }
    if (snap.state == yuntype::JobState::Error) {
        front.error = snap.error.empty() ? "请求失败" : snap.error;
        client_.cancelSentence();
        syncPollTimer();
        refreshOverlay();
        return;
    }
    if (snap.state == yuntype::JobState::Done) {
        const bool candMode = snap.candidateMode;
        const std::uint64_t sentRev = snap.revision;
        std::string turnJson = std::move(snap.turnJson);
        std::vector<std::string> cands = std::move(snap.candidates);
        std::string finalText = std::move(snap.text);
        const std::string query = front.query;
        const std::string fallback = front.fallback;
        const std::size_t eraseChars = front.pendingEraseChars;
        std::string eraseSuffix = std::move(front.pendingEraseSuffix);
        const bool repSel = front.replaceSelection;
        queue_.pop_front();
        client_.cancelSentence();

        if (candMode && cands.size() > 1) {
            PendingCandidates pc;
            pc.query = query;
            pc.fallback = fallback;
            pc.pendingEraseChars = eraseChars;
            pc.pendingEraseSuffix = std::move(eraseSuffix);
            pc.replaceSelection = repSel;
            pc.candidates = std::move(cands);
            pc.sentRev = sentRev;
            pc.globalCursor = 0;
            if (!turnJson.empty()) {
                pc.sessionTurns = std::move(turnJson) + "\n";
            }
            pendingCandidates_ = std::move(pc);
            activeSessionAtomic_.store(true);
            syncPollTimer();
            refreshOverlay();
        } else {
            const std::string committed = !cands.empty() ? cands.front() : finalText;
            activeSessionAtomic_.store(hasActiveSession());
            commitSentenceResult(committed, candMode ? std::string{} : std::move(turnJson),
                                 sentRev, {}, candMode, eraseChars, eraseSuffix, repSel);
            syncPollTimer();
            refreshOverlay();
        }
    }
}

void WinApp::pollCompaction() {
    if (!config_.historyEnabled) {
        syncCompactionTimer();
        return;
    }
    if (history_.compactionStale()) {
        client_.cancelCompaction();
        history_.deferCompaction();
    }
    if (history_.compacting()) {
        auto [state, summary] = client_.pollCompaction();
        if (state == yuntype::JobState::Running) {
            syncCompactionTimer();
            return;
        }
        if (state == yuntype::JobState::Idle) {
            history_.deferCompaction();
        } else {
            historyCompactedAt_ = ++historyRevision_;
            history_.finishCompaction(state == yuntype::JobState::Done ? summary.c_str() : nullptr,
                                      historyCompactedAt_);
            markHistoryDirty();
            client_.cancelCompaction();
        }
    }
    const std::size_t budget = client_.historyBudget();
    const std::size_t trigger = budget - budget / 4;
    const std::size_t limit = static_cast<std::size_t>(std::max(1, config_.historyCount));
    if (history_.needsCompaction(limit, trigger)) {
        std::string batch = history_.beginCompaction(budget);
        client_.startCompaction(batch, history_.summary());
    }
    syncCompactionTimer();
}

void WinApp::refreshOverlay() {
    OverlayModel model;
    if (pendingCandidates_) {
        const auto &pc = *pendingCandidates_;
        const int pageSize = std::clamp(config_.candidatePageSize, 2, 10);
        const int total = static_cast<int>(pc.candidates.size());
        const int curPage = pc.globalCursor / pageSize;
        const int totalPages = std::max(1, (total + pageSize - 1) / pageSize);
        const int start = curPage * pageSize;
        const int end = std::min(total, start + pageSize);

        model.visible = true;
        model.preedit = pc.query;
        model.statusSuffix = pc.loadingMore ? "…" : pc.statusNote;
        for (int i = start; i < end; ++i) {
            model.pageCandidates.push_back(pc.candidates[static_cast<std::size_t>(i)]);
        }
        model.pageCursor = pc.globalCursor - start;
        model.currentPage = curPage;
        model.totalPages = totalPages;
        model.hasMoreAction = (pc.loadMoreRounds < kMaxLoadMoreRounds && !pc.sessionTurns.empty());
    } else if (draftActive_) {
        model.visible = true;
        model.preedit = draft_.empty() ? ("输入拼音，Enter/" + config_.triggerKey + " 转换") : draft_;
        model.preeditCursorByte = draft_.empty() ? 0 : draftCursor_;
        model.showDraftCaret = !draft_.empty();
    } else if (!queue_.empty()) {
        const auto &front = queue_.front();
        const auto snap = client_.pollSentence();
        model.visible = true;
        if (front.error) {
            model.preedit = front.query;
            model.statusSuffix = "!";
            model.auxText = *front.error + "（Enter 原样上屏 / Esc 取消）";
        } else if (!snap.candidateMode && !snap.text.empty()) {
            model.preedit = snap.text;
            model.statusSuffix = "…";
        } else {
            model.preedit = front.query;
            model.statusSuffix = "…";
        }
    } else {
        overlay_.hide();
        syncMouseHook(false);
        return;
    }

    overlay_.update(model, activeAnchor_);
    syncMouseHook(overlay_.isVisible());
}

void WinApp::resetActiveSession() {
    client_.cancelSentence();
    draftActive_ = false;
    draft_.clear();
    draftCursor_ = 0;
    draftEraseChars_ = 0;
    draftEraseSuffix_.clear();
    draftReplaceSelection_ = false;
    selectionLikely_ = false;
    mouseCaptureSeen_ = false;
    queue_.clear();
    pendingCandidates_.reset();
    activeSessionAtomic_.store(false);
    lastTriggerTickMs_.store(0);
    syncPollTimer();
    overlay_.hide();
    syncMouseHook(false);
}

} // namespace yuntype::win
