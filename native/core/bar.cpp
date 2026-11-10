#include "bar.h"

#include "game_window.h"
#include "render/continuous_corners.h"

#include "clock.h"
#include "log.h"
#include "render/capture.h"
#include "render/desktop_backdrop.h"
#include "render/glass_renderer.h"
#include "render/ui_font.h"
#include "ui/surface.h"

#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <vector>

namespace atrium {

namespace {

constexpr UINT WM_BAR_CAPTURE = WM_APP + 0x70; // wParam 1: Atrium may show in screenshots (render thread -> UI thread)

// DCompositionWaitForCompositorClock (Windows 11): waits for the next composed frame or a handle;
// WAIT_OBJECT_0 + count is the clock. Looked up at run time (absent before build 22000).
using WaitClockFn = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);
const WaitClockFn g_waitClock = reinterpret_cast<WaitClockFn>(GetProcAddress(LoadLibraryW(L"dcomp.dll"), "DCompositionWaitForCompositorClock"));

constexpr wchar_t kVisualClass[] = L"AtriumBarVisual";
constexpr wchar_t kInputClass[] = L"AtriumBarInput";
constexpr wchar_t kClockKey[] = L"#clock";
constexpr wchar_t kStartKey[] = L"#start";
constexpr wchar_t kNotifyKey[] = L"#notify";
constexpr wchar_t kTrayKey[] = L"#tray";
constexpr wchar_t kStatusKey[] = L"#status";
constexpr wchar_t kMoreKey[] = L"#more";
// Bar entries and the clock start with '#'; app keys are AUMIDs or paths.
bool IsEntry(const std::wstring& key) { return !key.empty() && key[0] == L'#'; }
constexpr double kInsetDip = 14; // room for the drop shadow around the shape
constexpr DWORD kStaleMs = 2000;  // present at least this often (echo filter cap)
constexpr UINT_PTR kTooltipTimer = 1;
constexpr UINT kTooltipDelayMs = 450;
constexpr float kTooltipFont = 12.5f, kTooltipPadX = 12, kTooltipPadY = 6, kTooltipMinH = 28;
constexpr float kTooltipTailH = 5, kTooltipTailHalfW = 12, kTooltipTailInset = 8;
constexpr UINT kPreviewSwapMs = 60;
constexpr UINT_PTR kFileHoverTimer = 2;
constexpr UINT kFileHoverDelayMs = 600;
constexpr UINT_PTR kGuardTimer = 3;
constexpr UINT kGuardMs = 1000;
constexpr UINT kGuardFastMs = 150; // while below a system taskbar the user is using
constexpr UINT_PTR kSlideTimer = 4;   // the slide out is over: hide the windows
constexpr UINT_PTR kPointerTimer = 5; // while covered: watch the pointer
constexpr UINT kPointerMs = 50;
constexpr DWORD kLeaveMs = 500;       // a revealed bar goes down this long after the pointer left it

Bar* g_guard = nullptr; // the bar whose stacking the WinEvent hooks look after

// Windows the bar never climbs over: menus, tooltips, the shell's own
// surfaces, input method windows.
bool IgnoredCover(HWND w)
{
    static const wchar_t* const kClasses[] = {L"#32768", L"tooltips_class32", L"Shell_SecondaryTrayWnd", L"NotifyIconOverflowWindow",
                                              L"TopLevelWindowForOverflowXamlIsland", L"Xaml_WindowedPopupClass", L"XamlExplorerHostIslandWindow",
                                              L"Windows.UI.Core.CoreWindow", L"SysShadow", L"IME", L"MSCTFIME UI", L"ForegroundStaging",
                                              L"MultitaskingViewFrame", L"Shell_InputSwitchTopLevelWindow", L"Progman", L"WorkerW"};
    wchar_t cls[96] = {};
    GetClassNameW(w, cls, 96);
    for (const wchar_t* c : kClasses)
        if (wcscmp(cls, c) == 0)
            return true;
    LONG ex = GetWindowLongW(w, GWL_EXSTYLE);
    if ((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED))
        return true; // click-through overlays
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return cloaked != FALSE;
}

int Px(double dip, float scale) { return int(std::lround(dip * scale)); }

// Ink over the glass (mean sRGB luminance of what it shows, 0..1): black and
// white text read equally well near 0.46, so dark ink from 0.50 up and light
// ink again below 0.42. Light grey windows (0.5..0.65) used to keep white text.
constexpr float kDarkInkAbove = 0.50f, kLightInkBelow = 0.42f;

GlassParams ParamsFor(const Settings& s, float scale)
{
    GlassParams p{};
    p.radius = float(s.cornerRadius * scale);
    p.edge = float(s.edge * scale);
    p.refract = float(s.refraction * scale);
    p.dispersion = float(s.dispersion);
    // Blur strength is mostly the mip level; mixing a sharp and a blurred sample
    // only at the low end avoids double images at medium settings.
    p.blurMix = float(std::min(1.0, s.blur * 4.0));
    p.blurLod = float(1.0 + 4.5 * s.blur);
    p.tintMix = float(1.0 - s.transparency);
    p.tintR = float((s.tint >> 16) & 0xFF) / 255.f;
    p.tintG = float((s.tint >> 8) & 0xFF) / 255.f;
    p.tintB = float(s.tint & 0xFF) / 255.f;
    p.tintA = 1;
    p.highlight = float(s.highlight);
    p.shadow = float(s.shadow);
    p.lightX = -0.6f;
    p.lightY = -0.8f;
    p.inset = float(kInsetDip * scale);
    return p;
}

// The popups' glass: exactly the bar's material (D26), one set of parameters
// per piece of glass in the window.
void PanelParams(const Settings& s, float scale, const GlassPanel& p, std::vector<GlassParams>& out)
{
    out.clear();
    GlassParams g = ParamsFor(s, scale);
    g.inset = p.inset;
    if (p.shapes.empty()) {
        g.shapeL = g.shapeT = p.inset;
        g.shapeR = float(p.window.right - p.window.left) - p.inset;
        g.shapeB = float(p.window.bottom - p.window.top) - p.inset;
        g.radius = p.radius;
        out.push_back(g);
        return;
    }
    for (const GlassShape& sh : p.shapes) {
        g.shapeL = sh.left;
        g.shapeT = sh.top;
        g.shapeR = sh.right;
        g.shapeB = sh.bottom;
        g.radius = std::min(sh.radius, std::min(sh.right - sh.left, sh.bottom - sh.top) / 2);
        g.fade = sh.fade;
        g.partCount = float(std::min<size_t>(sh.parts.size(), kMaxGlassParts));
        g.melt = sh.melt;
        g.partMix = sh.partMix;
        g.tailX = sh.tailX;
        g.tailHalfWidth = sh.tailHalfWidth;
        g.tailHeight = sh.tailHeight;
        g.tailInset = sh.tailInset;
        for (size_t i = 0; i < sh.parts.size() && i < kMaxGlassParts; ++i) {
            const GlassShape::Part& pt = sh.parts[i];
            g.parts[i][0] = pt.left;
            g.parts[i][1] = pt.top;
            g.parts[i][2] = pt.right;
            g.parts[i][3] = pt.bottom;
            for (int k = 0; k < 4; ++k)
                g.partRadii[i][k] = pt.radii[k];
        }
        out.push_back(g);
    }
}

// Luminance of what the glass shows (backdrop mixed with the tint), the
// bar's rule for its ink.
float GlassLuma(const Settings& s, float backdrop)
{
    float tm = float(1.0 - s.transparency);
    float tintLuma = (0.2126f * float((s.tint >> 16) & 0xFF) + 0.7152f * float((s.tint >> 8) & 0xFF) + 0.0722f * float(s.tint & 0xFF)) / 255.f;
    return backdrop * (1 - tm) + tintLuma * tm;
}

void ClockStrings(std::wstring& time, std::wstring& date)
{
    wchar_t t[64] = {}, d[64] = {};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, t, 64);
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, d, 64, nullptr);
    time = t;
    date = d;
}


// Widest rendering of a clock string: every digit replaced by the widest digit.
std::wstring WidestDigits(std::wstring s)
{
    for (auto& c : s)
        if (iswdigit(c))
            c = L'8';
    return s;
}

void RegisterClasses(HINSTANCE instance)
{
    static bool done = false;
    if (done)
        return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = DefWindowProcW; // replaced per window once `this` is attached
    wc.lpszClassName = kVisualClass;
    RegisterClassExW(&wc);
    wc.lpszClassName = kInputClass;
    RegisterClassExW(&wc);
    done = true;
}

} // namespace

// OLE drop target on the input window: files dragged over an app bring it to
// the front after a moment; dropping opens them with that app.
struct BarDropTarget : winrt::implements<BarDropTarget, IDropTarget> {
    Bar* bar;
    bool files = false;
    explicit BarDropTarget(Bar* b) : bar(b) {}

    DWORD Effect(POINTL pt)
    {
        std::wstring key = bar->FileDragOver(POINT{pt.x, pt.y});
        return files && !key.empty() && bar->canOpenFiles && bar->canOpenFiles(key) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    }
    HRESULT __stdcall DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override
    {
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        files = data && data->QueryGetData(&fe) == S_OK;
        *effect = Effect(pt);
        return S_OK;
    }
    HRESULT __stdcall DragOver(DWORD, POINTL pt, DWORD* effect) override
    {
        *effect = Effect(pt);
        return S_OK;
    }
    HRESULT __stdcall DragLeave() override
    {
        bar->FileDragLeave();
        return S_OK;
    }
    HRESULT __stdcall Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override
    {
        *effect = Effect(pt);
        std::wstring key = bar->fileHoverKey_;
        bar->FileDragLeave();
        if (*effect == DROPEFFECT_NONE || !data)
            return S_OK;
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm{};
        if (FAILED(data->GetData(&fe, &sm)))
            return S_OK;
        std::vector<std::wstring> paths;
        if (auto drop = static_cast<HDROP>(GlobalLock(sm.hGlobal))) {
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count && i < 64; ++i) {
                std::wstring p(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
                p.resize(DragQueryFileW(drop, i, p.data(), UINT(p.size())));
                paths.push_back(std::move(p));
            }
            GlobalUnlock(sm.hGlobal);
        }
        ReleaseStgMedium(&sm);
        if (!paths.empty() && bar->onDropFiles)
            bar->onDropFiles(key, paths);
        return S_OK;
    }
};

Bar::Bar(HINSTANCE instance, bool debugCapturable) : debugCapturable_(debugCapturable)
{
    RegisterClasses(instance);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    quit_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    frame_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3), reinterpret_cast<IUnknown**>(dwrite_.put()));

    // Visual: never hit-tested (layered + transparent), no redirection bitmap.
    visual_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                              kVisualClass, L"Atrium Bar", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    SetLayeredWindowAttributes(visual_, 0, 255, LWA_ALPHA);
    // Input: draws nothing, so it is invisible; its window region is the shape.
    input_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kInputClass,
                             L"Atrium Bar Input", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    SetWindowLongPtrW(input_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(input_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Bar::InputProc));
    SetWindowLongPtrW(visual_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(visual_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Bar::VisualProc));
    // Out of every capture until the render thread has a backdrop that
    // leaves them out by itself (Desktop Duplication).
    captureAck_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    captureVisible_ = debugCapturable_;
    if (debugCapturable_)
        Log(L"bar: --debug-capturable, Atrium shows in screenshots (with Windows.Graphics.Capture, the backdrop freezes after the first frame)");
    SetCaptureExclusion(visual_, captureVisible_);
    SetCaptureExclusion(input_, captureVisible_);

    // A compact Dock-style label, using the same material and adaptive ink as
    // every other surface. It never takes focus or receives pointer input.
    tooltip_ = std::make_unique<Surface>(instance, L"Atrium Hover Label", false);
    tooltip_->message = [](UINT m, WPARAM, LPARAM, LRESULT& result) {
        if (m != WM_NCHITTEST)
            return false;
        result = HTTRANSPARENT;
        return true;
    };
    tooltip_->paint = [this](Canvas& c) {
        auto bounds = D2D1::RectF(0, 0, tooltipSize_.width, tooltipSize_.height);
        if (c.panel)
            c.Callout(bounds, std::min(16.f, tooltipSize_.height / 2), tooltipTailX_, kTooltipTailHalfW, kTooltipTailH, kTooltipTailInset, c.theme.panel);
        c.Text(tooltipText_, D2D1::RectF(kTooltipPadX, kTooltipPadY, bounds.right - kTooltipPadX, bounds.bottom - kTooltipPadY),
               kTooltipFont, c.theme.text, DWRITE_FONT_WEIGHT_MEDIUM, 1);
    };

    // Drag and drop needs OLE on this (STA) thread; balanced in the destructor.
    oleReady_ = SUCCEEDED(OleInitialize(nullptr));
    if (oleReady_) {
        dropTarget_ = winrt::make<BarDropTarget>(this);
        if (FAILED(RegisterDragDrop(input_, dropTarget_.get()))) {
            Log(L"bar: RegisterDragDrop failed; dropping files on apps is unavailable");
            dropTarget_ = nullptr;
        }
    }

    // Stacking guard: raised again when covered, hidden behind full-screen apps.
    g_guard = this;
    hooks_[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &Bar::WinEvent, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    hooks_[1] = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr, &Bar::WinEvent, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // Maximizing, restoring and entering full screen move the window without a move-size loop.
    hooks_[2] = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, &Bar::WinEvent, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(input_, kGuardTimer, kGuardMs, nullptr);

    render_ = std::thread([this] { RenderThread(); });
}

Bar::~Bar()
{
    for (HWINEVENTHOOK h : hooks_)
        if (h)
            UnhookWinEvent(h);
    if (g_guard == this)
        g_guard = nullptr;
    SetEvent(quit_);
    if (render_.joinable())
        render_.join();
    if (dropTarget_)
        RevokeDragDrop(input_);
    dropTarget_ = nullptr;
    tooltip_.reset();
    DestroyWindow(input_);
    DestroyWindow(visual_);
    CloseHandle(wake_);
    CloseHandle(quit_);
    CloseHandle(frame_);
    CloseHandle(captureAck_);
    if (oleReady_)
        OleUninitialize();
}

bool Bar::StopRendering(DWORD timeoutMs)
{
    SetEvent(quit_);
    if (!render_.joinable())
        return true;
    // The render thread may wait for this thread (ShowOwnWindowsToCapture, DirectComposition):
    // keep its messages flowing while waiting, never a bare join.
    HANDLE thread = render_.native_handle();
    const ULONGLONG until = GetTickCount64() + timeoutMs;
    while (MsgWaitForMultipleObjects(1, &thread, FALSE, 50, QS_ALLINPUT) != WAIT_OBJECT_0) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (GetTickCount64() > until) {
            Log(L"core: the render thread did not stop in %lu ms", timeoutMs);
            return false;
        }
    }
    render_.join();
    return true;
}

LRESULT CALLBACK Bar::VisualProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<Bar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_DPICHANGED && self) {
        self->Relayout();
        return 0;
    }
    if (m == WM_BAR_CAPTURE && self) {
        // From the render thread: whether Atrium's windows may show in screenshots.
        bool visible = w != 0;
        SetCaptureExclusion(self->visual_, visible);
        SetCaptureExclusion(self->input_, visible);
        Surface::SetCaptureVisible(visible);
        SetEvent(self->captureAck_);
        return 0;
    }
    if (m == WM_CLOSE && self) {
        if (self->onCloseRequest)
            self->onCloseRequest(); // the app decides; never destroy just this window
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK Bar::InputProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<Bar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->OnInput(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

std::wstring Bar::HitTest(POINT pt) const
{
    for (const auto& hit : hits_)
        if (PtInRect(&hit.rect, pt))
            return hit.key;
    return {};
}

LRESULT Bar::OnInput(HWND h, UINT m, WPARAM w, LPARAM l)
{
    POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
    case WM_MOUSEACTIVATE: {
        // Never take focus from the user's app, except for the logo and the
        // bell: the start menu and the notification list need the keyboard,
        // and only the foreground process may hand the foreground to its own window.
        POINT cur;
        GetCursorPos(&cur);
        ScreenToClient(h, &cur);
        if ((HitTest(cur) == kStartKey || HitTest(cur) == kNotifyKey) && HIWORD(l) == WM_LBUTTONDOWN) {
            previousForeground_ = GetForegroundWindow();
            if (previousForeground_ == input_ || previousForeground_ == visual_)
                previousForeground_ = nullptr;
            return MA_ACTIVATE;
        }
        return MA_NOACTIVATE;
    }
    case WM_CLOSE:
        if (onCloseRequest)
            onCloseRequest();
        return 0;
    case WM_MOUSEMOVE: {
        if (dragging_) {
            UpdateDrag(pt);
            return 0;
        }
        // A press on an app that moves past the system drag threshold starts a drag.
        if (pressedLeft_ && !pressed_.empty() && !IsEntry(pressed_) && (w & MK_LBUTTON) &&
            (std::abs(pt.x - pressPt_.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(pt.y - pressPt_.y) > GetSystemMetrics(SM_CYDRAG))) {
            BeginDrag(pt);
            return 0;
        }
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
            SetRegion(true); // magnified icons reach above the shape
        }
        std::wstring hover = HitTest(pt);
        bool magnify = scene_.magnify > 1.001f;
        if (hover != scene_.hoverKey || (magnify && scene_.pointerX != float(pt.x))) {
            if (hover != scene_.hoverKey) {
                scene_.hoverKey = hover;
                // With a preview up, the next app's windows follow the pointer without the tooltip's delay.
                bool quick = previewShown_ || (previewOpen && previewOpen());
                HideTooltip();
                if (!hover.empty())
                    SetTimer(h, kTooltipTimer, quick ? kPreviewSwapMs : kTooltipDelayMs, nullptr);
            }
            scene_.pointerX = float(pt.x);
            Publish();
        }
        return 0;
    }
    case WM_TIMER:
        if (w == kTooltipTimer) {
            KillTimer(h, kTooltipTimer);
            if (pressed_.empty() && !dragging_)
                ShowTooltip();
            return 0;
        }
        if (w == kFileHoverTimer) {
            KillTimer(h, kFileHoverTimer);
            if (!fileHoverKey_.empty() && !IsEntry(fileHoverKey_) && onFileDragHover)
                onFileDragHover(fileHoverKey_);
            return 0;
        }
        if (w == kGuardTimer) {
            GuardStacking(nullptr);
            return 0;
        }
        if (w == kSlideTimer) {
            KillTimer(h, kSlideTimer);
            if (tucked_ && !slidOut_) {
                slidOut_ = true;
                Relayout(); // hides the windows; the render thread rests
            }
            return 0;
        }
        if (w == kPointerTimer) {
            WatchPointer();
            return 0;
        }
        break;
    case WM_MOUSELEAVE:
        tracking_ = false;
        HideTooltip();
        if (!dragging_) {
            SetRegion(false);
            scene_.hoverKey.clear();
            scene_.pressedKey.clear();
            scene_.pointerX = -1;
            pressed_.clear();
            Publish();
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
        HideTooltip();
        if (onPreviewClose)
            onPreviewClose(); // a press on the bar: the preview gives way
        pressed_ = HitTest(pt);
        pressedLeft_ = m == WM_LBUTTONDOWN;
        pressPt_ = pt;
        scene_.pressedKey = pressed_;
        if (m == WM_LBUTTONDOWN) { // the glass gives way under the press
            scene_.rippleX = float(pt.x);
            scene_.rippleY = float(pt.y);
            scene_.rippleMs = ClockMs();
        }
        SetCapture(h);
        Publish();
        return 0;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP: {
        if (dragging_) {
            // Dropped well above or below the bar: cancel, like the system taskbar.
            int mid = int((scene_.bar.top + scene_.bar.bottom) / 2);
            EndDrag(std::abs(pt.y - mid) <= int(scene_.bar.bottom - scene_.bar.top));
            ReleaseCapture();
            POINT cur;
            GetCursorPos(&cur);
            if (WindowFromPoint(cur) != input_) { // released off the bar: no hover is left
                tracking_ = false;
                SetRegion(false);
                scene_.hoverKey.clear();
                scene_.pointerX = -1;
                Publish();
            }
            return 0;
        }
        ReleaseCapture();
        std::wstring key = HitTest(pt);
        bool same = !pressed_.empty() && key == pressed_;
        pressed_.clear();
        pressedLeft_ = false;
        scene_.pressedKey.clear();
        Publish();
        if (!same)
            return 0;
        // Shift+click starts a new instance, as on the Windows taskbar.
        bool launch = m == WM_MBUTTONUP || (w & MK_SHIFT);
        if (IsEntry(key)) {
            if (m != WM_LBUTTONUP)
                return 0;
            RECT anchor = EntryRect(key);
            if (key == kClockKey && onClockClick)
                onClockClick();
            else if (key == kStartKey && onStartClick)
                onStartClick(anchor);
            else if (key == kNotifyKey && onNotifyClick)
                onNotifyClick(anchor);
            else if (key == kTrayKey && onTrayClick)
                onTrayClick(anchor);
            else if (key == kStatusKey && onStatusClick)
                onStatusClick(anchor);
            else if (key == kMoreKey && onOverflow)
                onOverflow(hidden_, anchor);
        } else if (launch ? bool(onLaunch) : bool(onActivate)) {
            // Starting an app (or another instance) hops its icon, as in the macOS Dock.
            auto t = std::find_if(items_.begin(), items_.end(), [&](const TaskItem& i) { return i.key == key; });
            if (t != items_.end() && (launch || t->windows.empty()))
                Bounce(key);
            (launch ? onLaunch : onActivate)(key);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (dragging_ && reinterpret_cast<HWND>(l) != h) {
            EndDrag(false); // capture taken away (e.g. a system menu opened)
            SetRegion(false);
        }
        return 0;
    case WM_MOUSEWHEEL: { // over the status icon: the volume
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &p);
        int notches = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
        if (notches && HitTest(p) == kStatusKey && onStatusWheel)
            onStatusWheel(notches);
        return 0;
    }
    case WM_RBUTTONUP: {
        if (dragging_)
            return 0;
        std::wstring key = HitTest(pt);
        POINT screen = pt;
        ClientToScreen(h, &screen);
        // Menus open above the bar (and above icons the pointer magnified), without the tooltip.
        HideTooltip();
        KillTimer(h, kTooltipTimer);
        RECT wr;
        GetWindowRect(h, &wr);
        screen.y = wr.top;
        if (!key.empty() && !IsEntry(key) && onItemMenu)
            onItemMenu(key, screen);
        else if (onContextMenu)
            onContextMenu(screen);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

void Bar::BeginDrag(POINT pt)
{
    int from = -1;
    for (int i = 0; i < int(scene_.items.size()); ++i)
        if (scene_.items[size_t(i)].key == pressed_)
            from = i;
    if (from < 0)
        return;
    const auto& plate = scene_.items[size_t(from)].plate;
    dragging_ = true;
    dragKey_ = pressed_;
    dragFrom_ = dragInsert_ = from;
    grabDx_ = pressPt_.x - int((plate.left + plate.right) / 2);
    dragX_ = pt.x;
    dragY_ = pt.y;
    scene_.pressedKey.clear();
    HideTooltip();
    Relayout();
}

void Bar::UpdateDrag(POINT pt)
{
    dragX_ = pt.x;
    dragY_ = pt.y;
    // Insertion point: the slot whose centre is nearest to the dragged plate's.
    float centre = float(pt.x - grabDx_);
    int slot = int(std::lround((centre - float(appsLeft_) - float(plate_) / 2) / float(pitch_)));
    dragInsert_ = std::clamp(slot, 0, std::max(shown_ - 1, 0));
    Relayout();
}

void Bar::EndDrag(bool commit)
{
    if (!dragging_)
        return;
    dragging_ = false;
    std::wstring key = dragKey_;
    dragKey_.clear();
    pressed_.clear();
    pressedLeft_ = false;
    scene_.pressedKey.clear();
    int from = -1;
    for (int i = 0; i < shown_ && i < int(items_.size()); ++i)
        if (items_[size_t(i)].key == key)
            from = i;
    bool moved = commit && from >= 0 && dragInsert_ >= 0 && dragInsert_ != from && dragInsert_ < int(items_.size());
    if (moved) {
        // Show the new order right away; the model confirms it a moment later
        // with the same order, which then needs no relayout.
        TaskItem t = std::move(items_[size_t(from)]);
        items_.erase(items_.begin() + from);
        items_.insert(items_.begin() + dragInsert_, std::move(t));
    }
    scene_.hoverKey = moved ? key : scene_.hoverKey;
    Relayout(); // the dropped icon slides from the pointer into its slot
    if (moved && onReorder)
        onReorder(key, dragInsert_);
}

std::wstring Bar::FileDragOver(POINT screen)
{
    POINT pt{screen.x - window_.left, screen.y - window_.top};
    std::wstring key = HitTest(pt);
    if (IsEntry(key))
        key.clear();
    if (key != fileHoverKey_) {
        fileHoverKey_ = key;
        scene_.hoverKey = key;
        KillTimer(input_, kFileHoverTimer);
        if (!key.empty())
            SetTimer(input_, kFileHoverTimer, kFileHoverDelayMs, nullptr);
        Publish();
    }
    return key;
}

void Bar::FileDragLeave()
{
    KillTimer(input_, kFileHoverTimer);
    fileHoverKey_.clear();
    scene_.hoverKey.clear();
    Publish();
}

void Bar::Apply(const Settings& s)
{
    settings_ = s;
    Relayout();
    UpdateCover(); // the hide mode may have changed
}

void Bar::SetItems(std::vector<TaskItem> items)
{
    // Title changes (a browser switching tabs) only affect the tooltips; skip
    // the relayout and the redraw unless something visible changed.
    auto visible = [](const std::vector<TaskItem>& v) {
        std::wstring sig;
        for (const auto& t : v)
            sig += t.key + L"|" + std::to_wstring(t.windows.size() > 0) + std::to_wstring(t.active) + std::to_wstring(t.attention) + L"|" +
                   std::to_wstring(t.iconVersion) + L"\n";
        return sig;
    };
    bool same = visible(items) == visible(items_);
    items_ = std::move(items);
    if (same)
        UpdateTooltips();
    else
        Relayout();
}

void Bar::SetVisible(bool visible)
{
    visible_ = visible;
    Relayout();
}

void Bar::Relayout()
{
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const float s = float(dpiX) / 96.f;
    const RECT& wa = mi.rcWork;
    const Settings& st = settings_;

    // Metrics (px). Every icon sits centred on a square plate (press and drag
    // feedback); the plate has the same gap `v` to the top and bottom edge,
    // and its corner radius is the bar's minus that gap, so plate and bar
    // corners are concentric.
    const int inset = Px(kInsetDip, s);
    const int barH = Px(st.height, s);
    const int icon = std::clamp(Px(st.iconSize, s), Px(12, s), barH - Px(16, s));
    int plate = std::max(icon + Px(4, s), std::min(barH - Px(12, s), int(std::lround(icon * 1.55))));
    int gap = std::max(Px(4, s), int(std::lround(icon * 0.18)));
    const int sepGap = Px(8, s);
    const float mag = float(std::clamp(st.magnify, 1.0, 2.0));

    // Clock block sized for its widest possible text.
    float timePx = float(barH) * 0.25f, datePx = float(barH) * 0.19f;
    int clockW = 0;
    if (st.showClock) {
        std::wstring time, date;
        ClockStrings(time, date);
        float w = std::max(MeasureUiText(dwrite_.get(), WidestDigits(time), timePx, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                           MeasureUiText(dwrite_.get(), WidestDigits(date), datePx, DWRITE_FONT_WEIGHT_NORMAL));
        clockW = int(std::ceil(w)) + Px(22, s);
    }

    // Left to right: [logo] [apps ... (overflow)] | [tray] [status] [clock] [bell].
    // The logo, the overflow entry, the tray and the bell use the same plates
    // as the apps; so does the status ring (battery or volume, Wi-Fi).
    // The status entry as three icons (statusStyle) is wider than a plate by about an icon.
    const int statusExtra = st.statusStyle == L"icons" ? int(std::lround(icon * 1.3f)) : 0;
    const int rightCount = int(st.showTray) + int(st.showStatus) + int(st.showNotifications) + int(clockW != 0);
    const int groupGap = rightCount ? sepGap : 0;
    const int total = int(items_.size());
    int n = total;
    bool more = false;
    auto padFor = [&](int p) { return (barH - p) / 2 + Px(4, s); }; // ends: plate gap plus a little air
    auto contentWidth = [&](int count, int p, int g, bool overflow) {
        int slots = count + (overflow ? 1 : 0);
        int w = 2 * padFor(p) + p + 2 * g;               // logo and the air after it
        w += slots ? slots * p + (slots - 1) * g : 0;    // apps
        if (rightCount)
            w += 2 * sepGap + p * (int(st.showTray) + int(st.showStatus) + int(st.showNotifications)) + (st.showStatus ? statusExtra : 0) + clockW +
                 (rightCount - 1) * g;
        return std::max(w, barH * 2);
    };
    // The magnified bar stretches by up to (mag - 1) * range on top of its width.
    auto stretchFor = [&](int p, int g) { return mag > 1.001f ? int(std::ceil((mag - 1) * float(p + g) * 2.3f)) : 0; };
    const int screenW = int(wa.right - wa.left) - 2 * inset;
    const int maxW = std::max(barH * 2, std::min(Px(st.width, s), screenW - stretchFor(plate, gap)));
    // Too many apps: tighten the gaps, then the plates; what still does not fit
    // goes behind the overflow entry (never silently dropped).
    while (n > 0 && contentWidth(n, plate, gap, false) > maxW && gap > Px(2, s))
        --gap;
    while (n > 0 && contentWidth(n, plate, gap, false) > maxW && plate > icon + Px(4, s))
        --plate;
    if (contentWidth(n, plate, gap, false) > maxW) {
        more = true;
        while (n > 0 && contentWidth(n, plate, gap, true) > maxW)
            --n;
    }
    hidden_.clear();
    for (int i = n; i < total; ++i)
        hidden_.push_back(items_[size_t(i)].key);
    // Never narrower than what is always there (logo, overflow, tray, status, clock, bell).
    const int barW = std::max(st.autoWidth ? std::min(contentWidth(n, plate, gap, more), maxW) : maxW, std::min(contentWidth(0, plate, gap, more), screenW));
    const int pad = padFor(plate);
    const int statusW = plate + statusExtra;
    const int v = (barH - plate) / 2;
    const int pitch = plate + gap;
    const int radius = std::min(Px(st.cornerRadius, s), std::min(barW, barH) / 2);

    // Room in the windows around the shape: the shadow everywhere; at the
    // sides half of the magnified stretch (the bar grows on both sides of the
    // pointer); above, the magnified and hopping icons.
    const float range = float(pitch) * 2.3f;
    const int stretch = stretchFor(plate, gap);
    // The window keeps the width of the widest bar the settings allow, and
    // the bar sits centred in it: when apps come and go only the shape
    // changes, so its width and the icons can glide instead of the window
    // jumping to a new size.
    const int roomW = std::max(barW, maxW);
    const int side = inset + (stretch + 1) / 2 + (stretch ? Px(2, s) : 0);
    const int insetX = side + (roomW - barW) / 2;
    const int lift = std::max(int(std::ceil(float(icon) * (mag - 1))), int(std::ceil(float(icon) * 0.55f)));
    const int rise = std::max(0, lift - (barH - icon) / 2 + Px(6, s));
    const int insetTop = std::max(inset, rise);

    const int winW = roomW + 2 * side, winH = barH + insetTop + inset;
    const int x = wa.left + (wa.right - wa.left - winW) / 2;
    const int y = wa.bottom - Px(st.margin, s) - barH - insetTop;

    // Display order: during a drag the dragged app leaves its slot and the
    // others close up around the insertion point.
    std::vector<int> order(size_t(std::max(n, 0)));
    for (int i = 0; i < n; ++i)
        order[size_t(i)] = i;
    int dragIndex = -1;
    if (dragging_) {
        for (int i = 0; i < n; ++i)
            if (items_[size_t(i)].key == dragKey_)
                dragIndex = i;
        if (dragIndex < 0) {
            dragging_ = false; // the app went away mid-drag
            dragKey_.clear();
        } else {
            dragInsert_ = std::clamp(dragInsert_, 0, n - 1);
            order.erase(order.begin() + dragIndex);
            order.insert(order.begin() + dragInsert_, dragIndex);
        }
    }

    // Launch feedback is one 480 ms hop.
    const double now = ClockMs();
    for (auto it = bounces_.begin(); it != bounces_.end();)
        it = now - it->second > 480 ? bounces_.erase(it) : std::next(it);

    // Scene in window coordinates.
    BarScene scene;
    scene.scale = s;
    scene.bar = D2D1::RectF(float(insetX), float(insetTop), float(insetX + barW), float(insetTop + barH));
    scene.windowW = float(winW);
    scene.windowH = float(winH);
    scene.tucked = tucked_;
    scene.plateRadius = float(std::clamp(radius - v, Px(3, s), plate / 2));
    scene.hoverKey = dragging_ ? std::wstring() : scene_.hoverKey;
    scene.pressedKey = scene_.pressedKey;
    scene.dragKey = dragging_ ? dragKey_ : std::wstring();
    scene.magnify = mag;
    scene.magnifyRange = range;
    // Keep the pointer and the ripple; only the geometry changes. The pointer
    // moves with the window if it was rebuilt at another x.
    scene.pointerX = scene_.pointerX < 0 ? -1 : scene_.pointerX + float(window_.left - x);
    scene.rippleX = scene_.rippleX + float(window_.left - x);
    scene.rippleY = scene_.rippleY + float(window_.top - y);
    scene.rippleMs = scene_.rippleMs;
    hits_.clear();
    // Hit cells reach up to the window's top: magnified icons rise above the
    // shape, and the input region then covers that room too.
    const int hitTop = 0, hitBottom = insetTop + barH;
    const float plateTop = float(insetTop + v);
    const float iconTop = float(insetTop) + float(barH - icon) / 2;
    auto entry = [&](const wchar_t* key, int left, const wchar_t* glyph) {
        SceneItem it;
        it.key = key;
        it.plate = D2D1::RectF(float(left), plateTop, float(left + plate), plateTop + float(plate));
        float il = float(left) + float(plate - icon) / 2;
        it.icon = D2D1::RectF(il, iconTop, il + float(icon), iconTop + float(icon));
        it.glyph = glyph;
        return it;
    };
    // Disabled entries consume neither space nor hit targets.
    const int startLeft = insetX + pad;
    int cursor = insetX + barW - pad;
    auto rightSlot = [&](int width, bool enabled) {
        if (!enabled)
            return cursor;
        cursor -= width;
        const int left = cursor;
        cursor -= gap;
        return left;
    };
    const int bellLeft = rightSlot(plate, st.showNotifications);
    const int clockLeft = rightSlot(clockW, clockW != 0), clockRight = clockLeft + clockW;
    const int statusLeft = rightSlot(statusW, st.showStatus), statusRight = statusLeft + statusW;
    const int trayLeft = rightSlot(plate, st.showTray);
    const int rightLeft = rightCount ? cursor + gap : insetX + barW - pad;
    const int appsMin = startLeft + plate + 2 * gap;
    const int slots = n + (more ? 1 : 0);
    const int appsW = slots ? slots * plate + (slots - 1) * gap : 0;
    // Apps are centred in the space between the logo and the separator (matters when autoWidth is off).
    const int appsLeft = appsMin + std::max(0, (rightLeft - 2 * groupGap - appsMin - appsW) / 2);
    scene.extras.push_back(entry(kStartKey, startLeft, L""));
    hits_.push_back({kStartKey, RECT{insetX, hitTop, appsMin - gap / 2, hitBottom}});
    // Icons are rendered for their magnified size, so they stay sharp when they grow.
    const int iconPx = int(std::lround(float(icon) * mag));
    std::map<std::wstring, CachedIcon> keep;
    for (int k = 0; k < n; ++k) {
        const int i = order[size_t(k)];
        const TaskItem& t = items_[size_t(i)];
        SceneItem it;
        it.key = t.key;
        float left = float(appsLeft + k * pitch);
        if (i == dragIndex) // follows the pointer, kept within the app strip
            left = std::clamp(float(dragX_ - grabDx_) - float(plate) / 2, float(appsLeft - gap), float(appsLeft + n * pitch - gap - plate + gap));
        it.plate = D2D1::RectF(left, plateTop, left + float(plate), plateTop + float(plate));
        float iconLeft = left + float(plate - icon) / 2;
        it.icon = D2D1::RectF(iconLeft, iconTop, iconLeft + float(icon), iconTop + float(icon));
        it.running = !t.windows.empty();
        it.active = t.active;
        it.attention = t.attention;
        if (auto b = bounces_.find(t.key); b != bounces_.end()) {
            it.bounceMs = b->second;
        }
        // Icons are cached per key until the model reports a new version.
        CachedIcon c;
        if (auto f = icons_.find(t.key); f != icons_.end() && f->second.version == t.iconVersion && f->second.px == iconPx && f->second.image)
            c = f->second;
        else if (iconProvider) {
            IconImage img = iconProvider(t.key, iconPx);
            if (!img.empty())
                c = {t.iconVersion, iconPx, nextIconId_++, std::make_shared<const IconImage>(std::move(img))};
        }
        if (c.image) {
            it.image = c.image;
            it.imageId = c.id;
            keep[t.key] = c;
        }
        scene.items.push_back(std::move(it));
        // Hit cells split the gaps; the last one reaches the separator.
        int hl = k == 0 ? appsMin - gap / 2 : appsLeft + k * pitch - gap / 2;
        int hr = k == slots - 1 ? rightLeft - groupGap : appsLeft + k * pitch + plate + (gap - gap / 2);
        hits_.push_back({t.key, RECT{hl, hitTop, hr, hitBottom}});
    }
    if (more) {
        int left = appsLeft + n * pitch;
        scene.extras.push_back(entry(kMoreKey, left, L"\uE712"));
        hits_.push_back({kMoreKey, RECT{n ? left - gap / 2 : appsMin - gap / 2, hitTop, rightLeft - groupGap, hitBottom}});
    }
    icons_ = std::move(keep);
    appsLeft_ = appsLeft;
    pitch_ = pitch;
    plate_ = plate;
    gap_ = gap;
    shown_ = n;
    scene.separatorX = rightCount ? float(rightLeft - sepGap) : -1;
    if (st.showTray) {
        scene.extras.push_back(entry(kTrayKey, trayLeft, L"\uE70E"));
        hits_.push_back({kTrayKey, RECT{trayLeft - gap / 2, hitTop, trayLeft + plate + gap / 2, hitBottom}});
    }
    if (st.showStatus) {
        SceneItem status = entry(kStatusKey, statusLeft, L"");
        status.plate = D2D1::RectF(float(statusLeft), plateTop, float(statusRight), plateTop + float(plate));
        status.icon = D2D1::RectF(float(statusLeft), iconTop, float(statusRight), iconTop + float(icon));
        scene.extras.push_back(std::move(status));
        hits_.push_back({kStatusKey, RECT{statusLeft - gap / 2, hitTop, statusRight + gap / 2, hitBottom}});
    }
    scene.status = status_;
    scene.statusIcons = st.statusStyle == L"icons";
    if (clockW) {
        scene.clock = D2D1::RectF(float(clockLeft), plateTop, float(clockRight), plateTop + float(plate));
        scene.timePx = timePx;
        scene.datePx = datePx;
        hits_.push_back({kClockKey, RECT{clockLeft - gap / 2, hitTop, clockRight + gap / 2, hitBottom}});
    }
    if (st.showNotifications) {
        SceneItem bell = entry(kNotifyKey, bellLeft, L"\uEA8F");
        bell.badge = unread_;
        scene.extras.push_back(std::move(bell));
        hits_.push_back({kNotifyKey, RECT{bellLeft - gap / 2, hitTop, insetX + barW, hitBottom}});
    }
    if (!scene.hoverKey.empty() && std::none_of(hits_.begin(), hits_.end(), [&](const Hit& h) { return h.key == scene.hoverKey; }))
        scene.hoverKey.clear(); // the hovered app went away
    scene_ = std::move(scene);

    // The window only changes when the bar's size does (not on every drag move).
    const bool shown = visible_ && !slidOut_;
    RECT win{x, y, x + winW, y + winH};
    if (!dragging_ || !EqualRect(&win, &applied_) || shown != appliedVisible_) {
        SetWindowPos(visual_, HWND_TOPMOST, x, y, winW, winH, SWP_NOACTIVATE | (shown ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        // Input window right below the visual one, region = the rounded shape.
        SetWindowPos(input_, visual_, x, y, winW, winH, SWP_NOACTIVATE | (shown ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        applied_ = win;
        appliedVisible_ = shown;
    }
    window_ = win;
    monitor_ = mi.rcMonitor;
    hmonitor_ = mon;
    scale_ = s;
    inset_ = inset;
    insetX_ = insetX;
    insetTop_ = insetTop;
    rise_ = rise;
    radius_ = radius;
    SetRegion(regionExtended_);
    UpdateTooltips();
    Publish();
}

void Bar::SetRegion(bool extended)
{
    regionExtended_ = extended;
    const int barW = int(scene_.bar.right - scene_.bar.left), barH = int(scene_.bar.bottom - scene_.bar.top);
    const int ext = extended ? rise_ : 0;
    if (appliedRgn_[0] == barW && appliedRgn_[1] == barH && appliedRgn_[2] == radius_ && appliedRgn_[3] == insetX_ && appliedRgn_[4] == insetTop_ &&
        appliedRgn_[5] == ext)
        return;
    HRGN rgn = ContinuousRegion(RECT{insetX_, insetTop_, insetX_ + barW + 1, insetTop_ + barH + 1}, radius_);
    if (ext > 0) {
        // While the pointer is on the bar, the room above it belongs to the
        // magnified icons (clicks there go to the icon below the pointer).
        HRGN top = CreateRectRgn(insetX_ + radius_ / 2, insetTop_ - ext, insetX_ + barW - radius_ / 2, insetTop_ + radius_);
        CombineRgn(rgn, rgn, top, RGN_OR);
        DeleteObject(top);
    }
    SetWindowRgn(input_, rgn, FALSE);
    appliedRgn_[0] = barW;
    appliedRgn_[1] = barH;
    appliedRgn_[2] = radius_;
    appliedRgn_[3] = insetX_;
    appliedRgn_[4] = insetTop_;
    appliedRgn_[5] = ext;
}

void Bar::ActivateIndex(int index)
{
    if (index < 0 || index >= int(items_.size()) || !onActivate)
        return;
    const TaskItem& t = items_[size_t(index)];
    Log(L"bar: app %d by the keyboard (%s)", index + 1, t.name.c_str());
    if (t.windows.empty())
        Bounce(t.key);
    onActivate(std::wstring(t.key)); // the copy: activating may change the items
}

void Bar::Bounce(const std::wstring& key)
{
    Log(L"bar: bounce %s", key.c_str());
    double now = ClockMs();
    bounces_[key] = now;
    for (auto& it : scene_.items)
        if (it.key == key) {
            it.bounceMs = now;
        }
    Publish();
}

void Bar::SetPanel(const GlassPanel& panel)
{
    if (panel.visible)
        panels_[panel.hwnd] = panel;
    else
        panels_.erase(panel.hwnd);
    Publish(false);
}

void CALLBACK Bar::WinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD)
{
    if (!g_guard || idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
        return;
    if (event == EVENT_OBJECT_LOCATIONCHANGE) { // only the foreground app's own window matters
        if (hwnd == GetForegroundWindow())
            g_guard->UpdateCover();
        return;
    }
    g_guard->GuardStacking(event == EVENT_SYSTEM_FOREGROUND ? L"foreground" : L"move");
}

// The app in front fills the bar's monitor: maximized there (the system
// taskbar auto-hides, so maximized windows reach the bottom too) or covering
// the whole monitor (full screen). Never the desktop or Atrium itself.
bool Bar::ForegroundCoversScreen() const
{
    HWND fg = GetForegroundWindow();
    if (!fg)
        return false;
    fg = GetAncestor(fg, GA_ROOT);
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId() || IsIconic(fg) || !IsWindowVisible(fg))
        return false;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    for (const wchar_t* c : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"})
        if (wcscmp(cls, c) == 0)
            return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked || MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) != hmonitor_)
        return false;
    if (IsZoomed(fg))
        return true;
    RECT r{};
    GetWindowRect(fg, &r);
    return r.left <= monitor_.left && r.top <= monitor_.top && r.right >= monitor_.right && r.bottom >= monitor_.bottom;
}

void Bar::UpdateCover()
{
    // The hide mode (autoHide): never, always, or while such an app is in front.
    const std::wstring& mode = settings_.autoHide;
    const bool foregroundCovers = ForegroundCoversScreen();
    const bool covered = mode == L"always" || (mode != L"never" && foregroundCovers);
    // "always" remains a conventional auto-hidden bar. In "fullscreen", only
    // a recognized game that fills the monitor owns the bottom edge; browsers
    // and presentations retain normal pointer reveal.
    const bool suppressReveal = mode == L"fullscreen" && IsFullscreenGame(GetForegroundWindow(), hmonitor_);
    if (covered == covered_ && suppressReveal == suppressReveal_)
        return;
    covered_ = covered;
    suppressReveal_ = suppressReveal;
    // Maximizing with the pointer on the bar keeps it up until the pointer leaves.
    POINT p{};
    GetCursorPos(&p);
    RECT bar = BarRect(), zone{bar.left, window_.top, bar.right, monitor_.bottom};
    revealed_ = covered && !suppressReveal_ && !slidOut_ && PtInRect(&zone, p);
    leftAt_ = 0;
    wchar_t cls[64] = {};
    GetClassNameW(GetForegroundWindow(), cls, 64);
    if (mode == L"fullscreen")
        Log(L"bar: %s%s (%s)", covered ? L"a maximized or full-screen app is in front: auto-hide" : L"no maximized or full-screen app in front",
            suppressReveal ? L", full-screen game: edge reveal suppressed" : L"", cls);
    else
        Log(L"bar: auto-hide %s (%s)", covered ? L"always" : L"never", mode.c_str());
    if (covered && !suppressReveal_)
        SetTimer(input_, kPointerTimer, kPointerMs, nullptr);
    else
        KillTimer(input_, kPointerTimer);
    UpdateAutoHide();
}

void Bar::UpdateAutoHide()
{
    bool tuck = covered_ && !revealed_ && visible_;
    if (tuck == tucked_)
        return;
    tucked_ = tuck;
    scene_.tucked = tuck;
    if (tuck) {
        HideTooltip();
        SetRegion(false);
        // The painter's slide out takes kSlideOutMs; then the windows go.
        SetTimer(input_, kSlideTimer, UINT(BarPainter::kSlideOutMs) + 60, nullptr);
        Publish();
        return;
    }
    KillTimer(input_, kSlideTimer);
    if (slidOut_) {
        slidOut_ = false;
        Relayout(); // shows the windows; the painter slides the bar up
    } else {
        Publish();
    }
    Raise();
}

void Bar::WatchPointer()
{
    if (!covered_ || suppressReveal_) {
        KillTimer(input_, kPointerTimer);
        if (revealed_) {
            revealed_ = false;
            leftAt_ = 0;
            UpdateAutoHide();
        }
        return;
    }
    POINT p{};
    GetCursorPos(&p);
    RECT bar = BarRect();
    if (!revealed_) {
        // The bottom edge of the screen, below the bar.
        if (p.y >= monitor_.bottom - 2 && p.x >= bar.left && p.x < bar.right) {
            revealed_ = true;
            leftAt_ = 0;
            UpdateAutoHide();
        }
        return;
    }
    RECT zone{bar.left, window_.top, bar.right, monitor_.bottom};
    if (PtInRect(&zone, p) || dragging_ || (keepShown && keepShown())) {
        leftAt_ = 0;
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (!leftAt_)
        leftAt_ = now;
    else if (now - leftAt_ >= kLeaveMs) {
        revealed_ = false;
        leftAt_ = 0;
        UpdateAutoHide();
    }
}

void Bar::Raise()
{
    SetWindowPos(visual_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SetWindowPos(input_, visual_, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// Keeps the bar on top: hidden behind a full-screen app, below a revealed
// system taskbar that overlaps it while the user is using that taskbar (the
// pointer on it outside the bar, or the keyboard in it), and otherwise raised again when
// another topmost window covers it. A taskbar held up only by an app's
// flashing button does not push the bar down (the bar shows the app's
// attention itself). A window that keeps pushing itself over the bar
// wins after a few rounds, so two always-on-top windows never fight.
void Bar::GuardStacking(const wchar_t* why)
{
    UpdateCover();
    if (!visible_ || tucked_ || dragging_)
        return;
    RECT bar = BarRect(), r{}, overlap{}, trayRect{};
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    bool trayOver = tray && IsWindowVisible(tray) && GetWindowRect(tray, &trayRect) && IntersectRect(&overlap, &trayRect, &bar) &&
                    overlap.bottom - overlap.top > 2;
    bool usingTray = false;
    POINT cur{};
    wchar_t fgCls[64] = {};
    if (trayOver) {
        GetCursorPos(&cur);
        HWND fg = GetForegroundWindow();
        GetClassNameW(fg, fgCls, 64);
        // The pointer counts on the system taskbar outside the bar (the screen's
        // bottom edge reveals it): over the bar it is meant for the bar.
        RECT ours{bar.left, window_.top, bar.right, monitor_.bottom}; // down to the edge that brings an auto-hidden bar up
        usingTray = (PtInRect(&trayRect, cur) && !PtInRect(&ours, cur)) || fg == tray || wcscmp(fgCls, L"TopLevelWindowForOverflowXamlIsland") == 0 ||
                    wcscmp(fgCls, L"Shell_TrayWnd") == 0;
    }
    bool trayAbove = false;
    HWND cover = nullptr;
    for (HWND w = GetTopWindow(nullptr); w && w != visual_; w = GetWindow(w, GW_HWNDNEXT)) {
        if (w == tray) {
            trayAbove = true;
            continue;
        }
        if (w == input_ || !IsWindowVisible(w))
            continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId())
            continue;
        if (!(GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
            cover = w; // the bar has lost its topmost place
            break;
        }
        if (GetWindowRect(w, &r) && IntersectRect(&overlap, &r, &bar) && !IgnoredCover(w)) {
            cover = w;
            break;
        }
    }
    if (trayOver && usingTray) {
        // The revealed taskbar stays in front; it slides away again by itself.
        if (!trayAbove) {
            SetWindowPos(visual_, tray, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            SetWindowPos(input_, visual_, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            if (!belowTray_)
                Log(L"bar: below the revealed system taskbar (pointer %ld,%ld, foreground %s)", cur.x, cur.y, fgCls);
        }
        if (!belowTray_) // the system taskbar gets the pointer now: watch for it coming back to the bar
            SetTimer(input_, kGuardTimer, kGuardFastMs, nullptr);
        belowTray_ = true;
        return;
    }
    if (trayOver && trayAbove) {
        // Up without the user (an app's button flashing): the bar stays in front.
        Raise();
        if (belowTray_)
            SetTimer(input_, kGuardTimer, kGuardMs, nullptr);
        if (belowTray_ || !trayHeldUp_)
            Log(L"bar: over the system taskbar, which an app holds up");
        belowTray_ = false;
        trayHeldUp_ = true;
        return;
    }
    if (belowTray_) {
        Log(L"bar: the system taskbar went down again");
        SetTimer(input_, kGuardTimer, kGuardMs, nullptr);
    }
    belowTray_ = false;
    trayHeldUp_ = trayOver;
    if (!cover)
        return;
    ULONGLONG now = GetTickCount64();
    for (auto it = raises_.begin(); it != raises_.end();)
        it = now - it->second.second > 10000 || !IsWindow(it->first) ? raises_.erase(it) : std::next(it);
    auto& count = raises_.try_emplace(cover, 0, now).first->second;
    if (++count.first > 4) {
        if (count.first == 5) {
            wchar_t cls[64] = {};
            GetClassNameW(cover, cls, 64);
            Log(L"bar: %s keeps covering the bar; leaving it in front", cls);
        }
        return;
    }
    Raise();
    if (cover != lastCover_) {
        wchar_t cls[64] = {};
        GetClassNameW(cover, cls, 64);
        Log(L"bar: raised over %s (%s)", cls, why ? why : L"check");
        lastCover_ = cover;
    }
}

void Bar::SetStatus(const SystemStatus& status)
{
    if (status == status_)
        return;
    status_ = status;
    scene_.status = status;
    Publish();
    UpdateTooltips();
}

void Bar::SetUnread(int count)
{
    if (count == unread_)
        return;
    unread_ = count;
    Relayout();
}

RECT Bar::EntryRect(const std::wstring& key) const
{
    auto rect = [&](const SceneItem& it) {
        return RECT{window_.left + LONG(it.plate.left), window_.top + LONG(it.plate.top), window_.left + LONG(it.plate.right),
                    window_.top + LONG(it.plate.bottom)};
    };
    for (const auto& it : scene_.extras)
        if (it.key == key)
            return rect(it);
    for (const auto& it : scene_.items)
        if (it.key == key)
            return rect(it);
    if (key == kClockKey)
        return RECT{window_.left + LONG(scene_.clock.left), window_.top + LONG(scene_.clock.top), window_.left + LONG(scene_.clock.right),
                    window_.top + LONG(scene_.clock.bottom)};
    // Toasts still have an anchor when their taskbar entry is hidden.
    const RECT bar = BarRect();
    return RECT{bar.right - (bar.bottom - bar.top), bar.top, bar.right, bar.bottom};
}

RECT Bar::BarRect() const
{
    return RECT{window_.left + LONG(scene_.bar.left), window_.top + LONG(scene_.bar.top), window_.left + LONG(scene_.bar.right),
                window_.top + LONG(scene_.bar.bottom)};
}

std::wstring Bar::TooltipText(const std::wstring& key) const
{
    if (key == kStartKey)
        return L"\u5F00\u59CB"; // 开始
    if (key == kNotifyKey)
        return unread_ ? std::to_wstring(unread_) + L" \u6761\u672A\u8BFB\u901A\u77E5" : L"\u901A\u77E5"; // N 条未读通知 / 通知
    if (key == kTrayKey)
        return L"\u6258\u76D8"; // 托盘
    if (key == kStatusKey) {
        const SystemStatus& st = status_;
        std::wstring net = !st.connected ? L"\u672A\u8FDE\u63A5\u7F51\u7EDC" // 未连接网络
                                         : (st.wifi ? L"Wi-Fi " : L"") + st.network + (st.wifi ? L" \u00B7 \u4FE1\u53F7 " + std::to_wstring(st.signal) + L"%" : L"");
        std::wstring more;
        if (st.battery)
            more = L"\n\u7535\u6C60 " + std::to_wstring(st.batteryPercent) + L"%" + (st.charging ? L" \u00B7 \u5DF2\u63A5\u901A\u7535\u6E90" : L""); // 电池 N% · 已接通电源
        if (st.audio)
            more += L"\n\u97F3\u91CF " + (st.muted ? std::wstring(L"\u9759\u97F3") : std::to_wstring(st.volume) + L"%"); // 音量 N% / 静音
        return net + more;
    }
    if (key == kMoreKey)
        return L"\u53E6\u5916 " + std::to_wstring(hidden_.size()) + L" \u4E2A\u5E94\u7528"; // 另外 N 个应用
    if (key == kClockKey) {
        wchar_t buf[128] = {};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, nullptr, nullptr, buf, 128, nullptr);
        return buf;
    }
    for (const auto& t : items_)
        if (t.key == key)
            return t.windows.size() > 1 ? t.name + L" \u00B7 " + std::to_wstring(t.windows.size()) + L" \u4E2A\u7A97\u53E3" : t.name;
    return {};
}

void Bar::ShowTooltip()
{
    const std::wstring& key = scene_.hoverKey;
    auto hit = std::find_if(hits_.begin(), hits_.end(), [&](const Hit& h) { return h.key == key; });
    // An app with windows: its windows, previewed, instead of the tooltip (as the Windows taskbar).
    if (onPreview && hit != hits_.end() && !IsEntry(key)) {
        auto item = std::find_if(items_.begin(), items_.end(), [&](const TaskItem& t) { return t.key == key; });
        if (item != items_.end() && !item->windows.empty()) {
            tooltip_->Hide(false);
            int rise = scene_.magnify > 1.001f ? rise_ : 0;
            POINT anchor{window_.left + (hit->rect.left + hit->rect.right) / 2, window_.top + int(scene_.bar.top) - rise};
            previewShown_ = true;
            onPreview(key, item->windows, anchor);
            return;
        }
    }
    if (previewShown_) { // its last window went: back to the tooltip
        previewShown_ = false;
        if (onPreviewClose)
            onPreviewClose();
    }
    std::wstring text = TooltipText(key);
    if (hit == hits_.end() || text.empty()) {
        tooltip_->Hide(false);
        return;
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hmonitor_, &mi);
    int margin = Px(8, scale_);
    const float maxTextW = std::min(320.f, float(mi.rcWork.right - mi.rcWork.left - 2 * margin) / scale_ - 2 * kTooltipPadX);
    winrt::com_ptr<IDWriteTextFormat> format;
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (FAILED(UiTextFormat(dwrite_.get(), kTooltipFont, DWRITE_FONT_WEIGHT_MEDIUM, format.put())))
        return;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (FAILED(UiTextLayout(dwrite_.get(), text, format.get(), maxTextW, 1000, layout.put())))
        return;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    tooltipText_ = std::move(text);
    tooltipSize_ = D2D1::SizeF(std::min(maxTextW, std::ceil(metrics.widthIncludingTrailingWhitespace)) + 2 * kTooltipPadX,
                              std::max(kTooltipMinH, std::ceil(metrics.height) + 2 * kTooltipPadY));
    int w = Px(tooltipSize_.width, scale_), h = Px(tooltipSize_.height, scale_);
    int cx = window_.left + (hit->rect.left + hit->rect.right) / 2;
    // Above the magnified icons when they grow.
    int rise = scene_.magnify > 1.001f ? rise_ : 0;
    int top = std::max(int(mi.rcWork.top) + margin, int(window_.top) + int(scene_.bar.top) - rise - h - margin);
    int left = std::clamp(cx - w / 2, int(mi.rcWork.left) + margin, int(mi.rcWork.right) - margin - w);
    RECT rect{left, top, left + w, top + h + Px(kTooltipTailH, scale_)};
    tooltipTailX_ = std::clamp(float(cx - left) / scale_, 0.f, tooltipSize_.width);
    tooltip_->SetRadius(std::min(16.f, tooltipSize_.height / 2));
    ShapeDip label{D2D1::RectF(0, 0, tooltipSize_.width, tooltipSize_.height), std::min(16.f, tooltipSize_.height / 2)};
    label.tailX = tooltipTailX_;
    label.tailHalfWidth = kTooltipTailHalfW;
    label.tailHeight = kTooltipTailH;
    label.tailInset = kTooltipTailInset;
    tooltip_->SetShapes({label});
    if (tooltip_->Visible()) {
        tooltip_->Move(rect);
        tooltip_->Invalidate();
    } else {
        tooltip_->Show(rect, false, POINT{cx, rect.bottom});
    }
}

void Bar::HideTooltip()
{
    KillTimer(input_, kTooltipTimer);
    if (previewShown_) {
        previewShown_ = false;
        if (onPreviewLeave)
            onPreviewLeave();
    }
    tooltip_->Hide();
}

void Bar::UpdateTooltips()
{
    if (tooltip_->Visible() || previewShown_)
        ShowTooltip(); // e.g. the window count changed while hovering
}

void Bar::Publish(bool bar)
{
    {
        std::lock_guard g(lock_);
        snapshot_.settings = settings_;
        snapshot_.scene = scene_;
        snapshot_.window = window_;
        snapshot_.monitor = monitor_;
        snapshot_.hmonitor = hmonitor_;
        snapshot_.scale = scale_;
        snapshot_.visible = visible_ && !slidOut_;
        snapshot_.panels.clear();
        for (const auto& [hwnd, p] : panels_)
            snapshot_.panels.push_back(p);
        if (bar)
            ++snapshot_.generation;
    }
    SetEvent(wake_);
}

void Bar::ShowOwnWindowsToCapture(bool visible)
{
    if (debugCapturable_)
        visible = true;
    if (visible == captureVisible_)
        return;
    // The windows belong to the UI thread; it answers once they are changed.
    ResetEvent(captureAck_);
    PostMessageW(visual_, WM_BAR_CAPTURE, visible ? 1 : 0, 0);
    if (WaitForSingleObject(captureAck_, 1000) != WAIT_OBJECT_0)
        Log(L"render: the UI thread did not confirm the capture exclusion in time");
    captureVisible_ = visible;
    Log(L"render: Atrium %s screenshots", visible ? L"shows in" : L"is kept out of");
}

void Bar::RenderThread()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    while (WaitForSingleObject(quit_, 0) == WAIT_TIMEOUT) {
        bool rebuild = false;
        try {
            rebuild = RenderSession();
        } catch (const winrt::hresult_error& e) {
            Log(L"render: 0x%08X %s; rebuilding in 1 s", static_cast<unsigned>(e.code()), e.message().c_str());
            rebuild = true;
        }
        if (!rebuild || WaitForSingleObject(quit_, 1000) != WAIT_TIMEOUT)
            break;
    }
    winrt::uninit_apartment();
}

bool Bar::RenderSession()
{
    // Wait for the first layout.
    HANDLE first[] = {quit_, wake_};
    Snapshot snap;
    for (;;) {
        {
            std::lock_guard g(lock_);
            snap = snapshot_;
        }
        if (snap.hmonitor)
            break;
        if (WaitForMultipleObjects(2, first, FALSE, INFINITE) == WAIT_OBJECT_0)
            return false;
    }

    Gpu gpu = Gpu::ForMonitor(snap.hmonitor);
    // The backdrop: Desktop Duplication, which leaves Atrium's windows out by
    // itself (so screenshots may show them); else Windows.Graphics.Capture
    // with Atrium kept out of every capture.
    std::unique_ptr<BackdropSource> capture;
    if (!duplicationFailed_) {
        HRESULT why = S_OK;
        bool elsewhere = false;
        for (int tries = 0;; ++tries) {
            capture = DuplicationCapture::Create(gpu, snap.hmonitor, frame_, why, elsewhere);
            if (capture || !DuplicationCapture::Temporary(why))
                break;
            if (tries == 0)
                Log(L"render: desktop duplication unavailable for now (0x%08X), trying again every second", static_cast<unsigned>(why));
            if (WaitForSingleObject(quit_, 1000) != WAIT_TIMEOUT)
                return false;
        }
        if (!capture && elsewhere)
            PreferPowerSavingGpu();
        if (!capture) {
            duplicationFailed_ = true;
            Log(L"render: desktop duplication unavailable (0x%08X); Windows.Graphics.Capture, Atrium kept out of screenshots",
                static_cast<unsigned>(why));
        }
    }
    if (!capture) {
        ShowOwnWindowsToCapture(false); // before the first frame, or the glass would sample itself
        capture = std::make_unique<MonitorCapture>(gpu, snap.hmonitor, frame_);
    } else {
        ShowOwnWindowsToCapture(true);
    }
    // --debug-capturable with Windows.Graphics.Capture: a frozen backdrop (later frames contain the glass).
    const bool freeze = debugCapturable_ && !capture->KeepsOwnWindowsOut();
    DesktopBackdrop desktop(gpu, frame_); // the widgets' backdrop (D38)
    GlassRenderer renderer(gpu, visual_, snap.window.right - snap.window.left, snap.window.bottom - snap.window.top);
    BarPainter painter(renderer.D2D(), renderer.DWrite());

    unsigned drawnGeneration = 0;
    std::wstring drawnText;
    bool darkClock = false, darkApps = false, drawnDark = false, drawnDarkIcons = false, animating = false;
    InkFade clockInk, appsInk; // the inks fade to a new colour, not switch
    Readback dotBackdrop; // newest completed luminance grid, reused while the desktop is still
    ULONGLONG lastPresent = 0;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    // Per-minute counters: captured frames, presents, frames skipped as echoes.
    unsigned frames = 0, presents = 0, skipped = 0;
    std::vector<double> workMs;    // capture copy + comparison (+ draw and present), per wake-up
    std::vector<double> presentMs; // the same, only for wake-ups that presented
    std::vector<double> panelsMs;  // of which the popups' and widgets' glass, when there was any
    winrt::com_ptr<ID3D11Texture2D> frozen; // --debug-capturable only
    ULONGLONG statsStart = GetTickCount64();
    Log(L"render: started on %s, window %ldx%ld", gpu.adapterName.c_str(), snap.window.right - snap.window.left, snap.window.bottom - snap.window.top);
    // Glass layers of the popups, created while they are shown.
    struct PanelState {
        explicit PanelState(HWND owner) : hwnd(owner) {}
        ~PanelState() { ForgetGlass(hwnd); }
        HWND hwnd; // frame/ink records leave with the layer, including device resets
        std::unique_ptr<GlassRenderer> renderer;
        unsigned pop = 0;
        bool ready = false;   // presented once (the popup was told)
        unsigned presentedGen = 0;
        int ink = -1;         // ink last posted: 1 dark, 0 light, -1 none yet
        RECT window{};        // where it was last drawn: a moved panel is drawn again at once
        unsigned desktopVersion = 0; // a widget: the desktop picture its backdrop came from
        // Presents of an animation, logged when it ends (smoothness checks).
        double burstStart = 0, burstLast = 0, burstGap = 0;
        int burstFrames = 0;
        // Why it was drawn again, during the burst: moved or resized, a new desktop
        // picture, what is under it changed (readback), its parameters changed.
        int burstMoved = 0, burstDesktop = 0, burstReadback = 0, burstParams = 0;
        bool owed = false;    // a frame to draw once its last one is on screen (Ready), or at the next tick
        uint64_t drawnTick = 0; // the compositor frame it was last drawn in
        bool dark = false;
        InkGrid grid;         // ink region by region (published for the popup)
        std::vector<GlassParams> params;
    };
    std::map<HWND, PanelState> panels;
    double slowLogged = -1e9;
    uint64_t tick = 1; // compositor frames (every wake-up when not paced by the clock)
    // A panel that moves (a widget dragged or gliding) needs the backdrop at
    // its new place even when the screen has not changed and no frame comes:
    // for a second after a move the newest frame is kept.
    winrt::com_ptr<ID3D11Texture2D> lastFrame;
    double panelMovedMs = -1e9;
    // A popup that opens needs it too (Atrium's own windows bring no frame):
    // otherwise it is kept at most every 100 ms.
    double keptMs = -1e9;
    auto keepFrame = [&](ID3D11Texture2D* frame) {
        if (!frame)
            return;
        double now = ClockMs();
        if (now - panelMovedMs > 1000 && now - keptMs < 100)
            return;
        keptMs = now;
        D3D11_TEXTURE2D_DESC d{};
        frame->GetDesc(&d);
        D3D11_TEXTURE2D_DESC have{};
        if (lastFrame)
            lastFrame->GetDesc(&have);
        if (!lastFrame || have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) {
            lastFrame = nullptr;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = 0;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.CPUAccessFlags = 0;
            if (FAILED(gpu.device->CreateTexture2D(&d, nullptr, lastFrame.put())))
                return;
        }
        gpu.context->CopyResource(lastFrame.get(), frame);
    };
    std::vector<GlassParams> gp;
    // Draws the panels; true while one of them animates.
    auto renderPanels = [&](const Snapshot& snap, ID3D11Texture2D* source, bool& lost) {
        for (auto it = panels.begin(); it != panels.end();) {
            bool listed = std::any_of(snap.panels.begin(), snap.panels.end(), [&](const GlassPanel& p) { return p.hwnd == it->first; });
            it = listed ? std::next(it) : panels.erase(it);
        }
        bool moving = false;
        double now = ClockMs();
        // Widgets sample the desktop, never the windows above them; only a
        // new picture of it (or a move) draws their glass again.
        bool widgets = std::any_of(snap.panels.begin(), snap.panels.end(), [](const GlassPanel& p) { return p.desktop && p.visible; });
        desktop.Update(widgets ? snap.settings.widgetBackdrop : std::wstring(L"off"));
        for (auto& [hwnd, ps] : panels)
            if (ps.burstFrames && now - ps.burstLast > 250) {
                if (ps.burstFrames >= 4) {
                    wchar_t title[64] = {};
                    InternalGetWindowText(hwnd, title, 64); // no message to the UI thread (it may be waiting for this glass)
                    double span = ps.burstLast - ps.burstStart;
                    Log(L"render: %s glass animated %d presents in %.0f ms (%.0f fps), longest gap %.1f ms (moved %d, desktop %d, under %d, "
                        L"params %d)",
                        title, ps.burstFrames, span, span > 0 ? 1000.0 * (ps.burstFrames - 1) / span : 0.0, ps.burstGap, ps.burstMoved,
                        ps.burstDesktop, ps.burstReadback, ps.burstParams);
                }
                ps.burstFrames = 0;
                ps.burstMoved = ps.burstDesktop = ps.burstReadback = ps.burstParams = 0;
            }
        for (const GlassPanel& p : snap.panels) {
            int w = p.window.right - p.window.left, h = p.window.bottom - p.window.top;
            if (w <= 0 || h <= 0 || !IsWindow(p.hwnd))
                continue;
            PanelState& ps = panels.try_emplace(p.hwnd, p.hwnd).first->second;
            const int bufferW = std::max(w, p.bufferW), bufferH = std::max(h, p.bufferH);
            const double t0 = ClockMs();
            double created = 0;
            if (!ps.renderer) {
                created = 1;
                try {
                    ps.renderer = std::make_unique<GlassRenderer>(gpu, p.hwnd, bufferW, bufferH, false, false, p.desktop);
                } catch (const winrt::hresult_error& e) {
                    Log(L"render: popup glass unavailable: 0x%08X", static_cast<unsigned>(e.code()));
                    continue;
                }
            }
            if (created)
                created = ClockMs() - t0;
            GlassRenderer& r = *ps.renderer;
            const bool moved = !EqualRect(&ps.window, &p.window);
            bool changed = r.Width() != bufferW || r.Height() != bufferH || moved;
            bool newDesktop = false, under = false;
            ps.window = p.window;
            if (moved)
                panelMovedMs = now;
            r.Resize(bufferW, bufferH);
            if (p.desktop) {
                ID3D11Texture2D* pic = desktop.Texture();
                // Again once the glass is on screen if its ink was never measured: the
                // first copy (before any present) is not compared, and a still desktop
                // would leave a widget with the default ink over any wallpaper.
                // A new picture of the desktop only counts where it changed under this glass (the
                // backdrop it copies: the window, the refraction margin, rounded out to 64 px).
                RECT around = p.window;
                InflateRect(&around, 128, 128);
                bool fresh = ps.desktopVersion != desktop.Version();
                if (fresh && r.HasBackdrop() && ps.grid.w != 0 && !changed && !desktop.ChangedSince(ps.desktopVersion, around)) {
                    ps.desktopVersion = desktop.Version();
                    fresh = false;
                }
                // Consume the latest desktop frame on the same render loop as the bar.
                if (pic && (changed || fresh || !r.HasBackdrop() || ps.grid.w == 0)) {
                    r.UpdateBackdrop(pic, p.window.left - desktop.Origin().x, p.window.top - desktop.Origin().y);
                    newDesktop = fresh;
                    ps.desktopVersion = desktop.Version();
                }
            } else if (ID3D11Texture2D* below = source ? source : (moved || !r.HasBackdrop()) ? lastFrame.get() : nullptr) {
                r.UpdateBackdrop(below, p.window.left - snap.monitor.left, p.window.top - snap.monitor.top);
            }
            PanelParams(snap.settings, snap.scale, p, gp);
            if (auto rb = r.Poll()) {
                changed |= rb->changed;
                under = rb->changed;
                // The ink region by region (cells of about 20 DIP), from what
                // the glass shows there, each cell with the bar's hysteresis;
                // the popup's own ink is what most of it needs.
                const float padX = float(rb->origin.x), padY = float(rb->origin.y);
                const float cell = std::max(8.f, 20.f * snap.scale);
                InkGrid grid;
                grid.w = std::max(1, int(std::ceil(float(w) / cell)));
                grid.h = std::max(1, int(std::ceil(float(h) / cell)));
                grid.cellX = float(w) / float(grid.w);
                grid.cellY = float(h) / float(grid.h);
                grid.dark.resize(size_t(grid.w) * size_t(grid.h));
                const std::wstring& tc = snap.settings.textColor;
                bool sameSize = ps.grid.w == grid.w && ps.grid.h == grid.h;
                int darkCells = 0;
                for (int y = 0; y < grid.h; ++y)
                    for (int x = 0; x < grid.w; ++x) {
                        size_t i = size_t(y) * size_t(grid.w) + size_t(x);
                        bool was = sameSize ? ps.grid.dark[i] != 0 : ps.dark;
                        bool d = was;
                        if (tc == L"dark" || tc == L"light") {
                            d = tc == L"dark";
                        } else {
                            float x0 = float(x) * grid.cellX, y0 = float(y) * grid.cellY;
                            float lum = GlassLuma(snap.settings, rb->RegionLuma(x0 + padX, y0 + padY, x0 + grid.cellX + padX, y0 + grid.cellY + padY));
                            if (!was && lum > kDarkInkAbove)
                                d = true;
                            else if (was && lum < kLightInkBelow)
                                d = false;
                        }
                        grid.dark[i] = d ? 1 : 0;
                        darkCells += d ? 1 : 0;
                    }
                ps.dark = 2 * darkCells >= int(grid.dark.size());
                if (!(grid == ps.grid)) {
                    ps.grid = grid;
                    PublishInk(p.hwnd, grid);
                    ps.ink = ps.dark ? 1 : 0;
                    PostMessageW(p.hwnd, WM_GLASS_INK, WPARAM(ps.ink), 0);
                }
            }
            if (p.pop != ps.pop) {
                r.Pop(p.popOpen, p.popX, p.popY);
                ps.pop = p.pop;
            }
            if (p.rippleMs > 0 && now - p.rippleMs < 720) {
                float u = float((now - p.rippleMs) / 720);
                for (GlassParams& g : gp) {
                    g.rippleX = p.rippleX;
                    g.rippleY = p.rippleY;
                    g.rippleR = 150 * snap.scale * (1 - (1 - u) * (1 - u) * (1 - u));
                    g.rippleA = 0.8f * (1 - u) * (1 - u);
                }
                moving = true;
            }
            if (!r.HasBackdrop())
                continue; // waits for the first capture frame under it
            // During a geometry animation the content supplies each frame.
            // A background-only present just before that update would consume
            // this composition's slot and push the matching geometry a frame
            // late. Keep the latest backdrop and draw it with the new geometry.
            if (p.animating && r.HasPresented() && p.gen == ps.presentedGen)
                continue;
            const double t1 = ClockMs();
            bool same = gp.size() == ps.params.size() && memcmp(gp.data(), ps.params.data(), gp.size() * sizeof(GlassParams)) == 0;
            if (!changed && !ps.owed && r.HasPresented() && same) {
                ps.presentedGen = p.gen;
                GlassPresented(p.hwnd, p.gen);
                continue;
            }
            // Once per composed frame (a new picture under it or new shapes may come in between),
            // and only when its last frame is on screen; else at the next tick.
            if ((ps.drawnTick == tick && !moved) || !r.Ready()) {
                ps.owed = true;
                continue;
            }
            ps.owed = false;
            ps.drawnTick = tick;
            if (p.desktop)
                r.SetOffset(p.offset);
            if (!r.Present(gp, nullptr)) {
                lost = true;
                return false;
            }
            ps.presentedGen = p.gen;
            GlassPresented(p.hwnd, p.gen);
            // A panel that holds the render thread up: what took the time (rate-limited).
            if (double t2 = ClockMs(); t2 - t0 > 16 && t2 - slowLogged > 2000) {
                slowLogged = t2;
                wchar_t title[64] = {};
                InternalGetWindowText(p.hwnd, title, 64);
                Log(L"render: slow glass for %s: %.1f ms (renderer created in %.1f ms, backdrop %.1f ms, present %.1f ms)", title, t2 - t0, created,
                    t1 - t0 - created, t2 - t1);
            }
            ps.burstMoved += moved ? 1 : 0;
            ps.burstDesktop += newDesktop ? 1 : 0;
            ps.burstReadback += under ? 1 : 0;
            ps.burstParams += same ? 0 : 1;
            ps.params = gp;
            if (!ps.burstFrames || now - ps.burstLast > 250) {
                ps.burstStart = now;
                ps.burstGap = 0;
                ps.burstFrames = 0;
            } else {
                ps.burstGap = std::max(ps.burstGap, now - ps.burstLast);
            }
            ps.burstLast = now;
            ++ps.burstFrames;
            if (!ps.ready) {
                ps.ready = true;
                PostMessageW(p.hwnd, WM_GLASS_READY, 0, 0);
                wchar_t title[64] = {};
                InternalGetWindowText(p.hwnd, title, 64);
                Log(L"render: popup glass ready for %s (%dx%d px, %zu shapes, radius %.0f px, tint %.2f, ink %s)", title, w, h, gp.size(),
                    gp.empty() ? 0.f : gp[0].radius, gp.empty() ? 0.f : gp[0].tintMix, ps.dark ? L"dark" : L"light");
            }
        }
        return moving;
    };

    HANDLE handles[] = {quit_, wake_, frame_};
    bool panelsMoving = false, barOwed = false;
    for (;;) {
        // Animations, and glass owed a frame (not Ready last time), run on the compositor's
        // clock (D45): one wake-up per composed frame, at the display's rate. A timeout would
        // be rounded up to 15.6 ms, and the popups' latency objects (no vsync) would spin.
        // Pending readbacks are polled soon.
        bool owed = barOwed || std::any_of(panels.begin(), panels.end(), [](const auto& e) { return e.second.owed; });
        bool paced = animating || panelsMoving || owed;
        bool readback = renderer.ReadbackPending() || std::any_of(panels.begin(), panels.end(), [](const auto& e) {
            return e.second.renderer && e.second.renderer->ReadbackPending();
        });
        DWORD w = WAIT_FAILED;
        if (paced && g_waitClock) {
            w = g_waitClock(3, handles, 100);
            if (w == WAIT_OBJECT_0 + 3 || w == WAIT_TIMEOUT) {
                ++tick;
            } else if (w > WAIT_OBJECT_0 + 3) {
                w = WaitForMultipleObjects(3, handles, FALSE, 6); // no clock (the display is off)
                ++tick;
            }
        } else {
            w = WaitForMultipleObjects(3, handles, FALSE, paced ? 6 : readback ? 3 : 1000);
            ++tick;
        }
        if (w == WAIT_OBJECT_0)
            return false;
        if (GetTickCount64() - statsStart >= 60000) {
            std::sort(workMs.begin(), workMs.end());
            auto pct = [&](double q) { return workMs.empty() ? 0.0 : workMs[std::min(workMs.size() - 1, size_t(q * double(workMs.size())))]; };
            std::sort(presentMs.begin(), presentMs.end());
            double presentP95 = presentMs.empty() ? 0.0 : presentMs[std::min(presentMs.size() - 1, size_t(0.95 * double(presentMs.size())))];
            std::sort(panelsMs.begin(), panelsMs.end());
            double panelsP95 = panelsMs.empty() ? 0.0 : panelsMs[std::min(panelsMs.size() - 1, size_t(0.95 * double(panelsMs.size())))];
            PROCESS_MEMORY_COUNTERS_EX mem{sizeof(mem)};
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem));
            Log(L"render: last minute %u frames, %u presents, %u echoes skipped; work p50 %.2f ms, p95 %.2f ms, max %.2f ms; "
                L"presenting p95 %.2f ms; popups and widgets p95 %.2f ms, max %.2f ms; private %zu MB",
                frames, presents, skipped, pct(0.5), pct(0.95), workMs.empty() ? 0.0 : workMs.back(), presentP95, panelsP95,
                panelsMs.empty() ? 0.0 : panelsMs.back(), size_t(mem.PrivateUsage >> 20));
            frames = presents = skipped = 0;
            workMs.clear();
            presentMs.clear();
            panelsMs.clear();
            statsStart = GetTickCount64();
        }
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        auto elapsedMs = [&] {
            LARGE_INTEGER t1;
            QueryPerformanceCounter(&t1);
            return double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
        };
        {
            std::lock_guard g(lock_);
            snap = snapshot_;
        }
        ID3D11Texture2D* frame = capture->Acquire();
        if (capture->Lost())
            return true; // a new source (display mode change, secure desktop)
        if (snap.hmonitor != MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY))
            return true; // primary monitor changed: new device and capture
        if (!snap.visible) {
            // Retire the bar's reads even while hidden. Otherwise its last pending
            // slot keeps the idle loop waking every 3 ms until the bar is revealed.
            renderer.Poll();
            drawnGeneration = 0; // its ink and glass are measured again when revealed
            bool lost = false;
            winrt::com_ptr<ID3D11Texture2D> hidden;
            hidden.copy_from(frame);
            if (freeze && hidden) { // the popups show in screenshots: they must not sample themselves
                if (!frozen) {
                    D3D11_TEXTURE2D_DESC d{};
                    hidden->GetDesc(&d);
                    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    d.MiscFlags = 0;
                    winrt::check_hresult(gpu.device->CreateTexture2D(&d, nullptr, frozen.put()));
                    gpu.context->CopyResource(frozen.get(), hidden.get());
                }
                hidden = frozen;
            }
            if (!freeze)
                keepFrame(frame);
            double p0 = elapsedMs();
            panelsMoving = renderPanels(snap, hidden.get(), lost);
            if (!snap.panels.empty())
                panelsMs.push_back(elapsedMs() - p0);
            hidden = nullptr;
            // Submit the copies from the duplicated frame before handing it back: AcquireNextFrame
            // waits for the GPU to be done with the last frame while it holds the shared device's
            // lock, so unsubmitted copies would wait for a Flush that can no longer get the lock.
            gpu.context->Flush();
            capture->Release();
            if (lost)
                return true;
            animating = barOwed = false; // drawn afresh when the bar shows again
            continue;
        }
        renderer.Resize(snap.window.right - snap.window.left, snap.window.bottom - snap.window.top);

        // Backdrop source: the newest capture frame, or in --debug-capturable
        // mode a private copy of the first one (later frames contain the glass).
        winrt::com_ptr<ID3D11Texture2D> source;
        if (frame) {
            source.copy_from(frame);
            ++frames;
            if (freeze) {
                if (!frozen) {
                    D3D11_TEXTURE2D_DESC d{};
                    source->GetDesc(&d);
                    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    d.MiscFlags = 0;
                    winrt::check_hresult(gpu.device->CreateTexture2D(&d, nullptr, frozen.put()));
                    gpu.context->CopyResource(frozen.get(), source.get());
                }
                source = frozen;
            }
        } else if (frozen && snap.generation != drawnGeneration) {
            source = frozen; // size-dependent textures were recreated
        }
        bool hadBackdrop = source != nullptr;
        if (source)
            renderer.UpdateBackdrop(source.get(), snap.window.left - snap.monitor.left, snap.window.top - snap.monitor.top);
        {
            bool lost = false;
            if (!freeze)
                keepFrame(frame);
            double p0 = elapsedMs();
            panelsMoving = renderPanels(snap, source.get(), lost);
            if (!snap.panels.empty())
                panelsMs.push_back(elapsedMs() - p0);
            if (lost) {
                Log(L"render: device lost (popup glass)");
                return true;
            }
        }
        source = nullptr;
        gpu.context->Flush(); // the panels' copies and comparisons start now, in one submission
        capture->Release();   // after the Flush (see above)

        bool changed = !renderer.HasPresented();
        if (auto rb = renderer.Poll()) {
            changed |= rb->changed;
            // Ink per region, from what the glass shows there: the backdrop
            // luminance mixed with the tint. Hysteresis avoids flicker.
            float tm = float(1.0 - snap.settings.transparency);
            uint32_t tint = snap.settings.tint;
            float tintLuma = (0.2126f * float((tint >> 16) & 0xFF) + 0.7152f * float((tint >> 8) & 0xFF) + 0.0722f * float(tint & 0xFF)) / 255.f;
            auto decide = [&](bool& dark, D2D1_RECT_F r) {
                if (r.right <= r.left)
                    return;
                const float pad = float(GlassRenderer::kPad);
                float lum = rb->RegionLuma(r.left + pad, r.top + pad, r.right + pad, r.bottom + pad) * (1 - tm) + tintLuma * tm;
                if (!dark && lum > kDarkInkAbove)
                    dark = true;
                else if (dark && lum < kLightInkBelow)
                    dark = false;
            };
            decide(darkClock, snap.scene.clock);
            if (!snap.scene.items.empty())
                decide(darkApps, D2D1::RectF(snap.scene.items.front().icon.left, snap.scene.bar.top, snap.scene.items.back().icon.right,
                                             snap.scene.bar.bottom));
            dotBackdrop = std::move(*rb);
        }
        auto ink = [&](bool autoDark) { return snap.settings.textColor == L"dark" || (snap.settings.textColor == L"auto" && autoDark); };
        bool dark = ink(darkClock), darkIcons = ink(darkApps);

        std::wstring time, date;
        if (snap.settings.showClock)
            ClockStrings(time, date);
        std::wstring textKey = time + L"|" + date;
        const double nowMs = ClockMs();
        animating = painter.Animate(snap.scene, nowMs);
        animating |= painter.UpdateDotInk(snap.scene, dotBackdrop, snap.settings, nowMs);
        clockInk.Target(dark ? 1.f : 0.f, nowMs);
        appsInk.Target(darkIcons ? 1.f : 0.f, nowMs);
        animating |= clockInk.Moving(nowMs) || appsInk.Moving(nowMs);
        const Ink inks{clockInk.Value(nowMs), appsInk.Value(nowMs)};
        ULONGLONG now = GetTickCount64();
        bool force = snap.generation != drawnGeneration || textKey != drawnText || dark != drawnDark || darkIcons != drawnDarkIcons ||
                     animating || barOwed || now - lastPresent > kStaleMs;
        if (!changed && !force) {
            ++skipped; // echo of our own present, or nothing new under the glass
            if (hadBackdrop)
                workMs.push_back(elapsedMs());
            continue;
        }
        if (!renderer.Ready()) {
            barOwed = true; // the last frame is not on screen yet: drawn when it is
            continue;
        }
        barOwed = false;
        GlassParams params = ParamsFor(snap.settings, snap.scale);
        const BarFrame& bf = painter.Frame();
        params.radius = std::min(params.radius, (bf.shape.bottom - bf.shape.top) / 2);
        params.shapeL = bf.shape.left;
        params.shapeT = bf.shape.top;
        params.shapeR = bf.shape.right;
        params.shapeB = bf.shape.bottom;
        params.rippleX = bf.rippleX;
        params.rippleY = bf.rippleY;
        params.rippleR = bf.rippleR;
        params.rippleA = bf.rippleA;
        bool ok = renderer.Present(params,
                                   [&](ID2D1DeviceContext2* d2d) {
                                       painter.Draw(d2d, snap.scene, inks, time, date,
                                                    std::to_wstring(snap.generation) + textKey + (dark ? L"d" : L"l") + (darkIcons ? L"d" : L"l"),
                                                    animating);
                                   });
        if (!ok) {
            Log(L"render: device lost");
            return true;
        }
        ++presents;
        workMs.push_back(elapsedMs());
        presentMs.push_back(workMs.back());
        drawnGeneration = snap.generation;
        drawnText = textKey;
        drawnDark = dark;
        drawnDarkIcons = darkIcons;
        lastPresent = now;
    }
}

} // namespace atrium
