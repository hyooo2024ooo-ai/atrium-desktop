// The floating glass bar (coexists with the system taskbar: it sits above the
// work area and never hides or replaces Explorer's taskbar).
//
// Two top-level windows (D08): a click-through layered visual window that
// DirectComposition renders into, and an invisible input window directly below
// it whose rounded region receives the clicks. Both are excluded from capture,
// so the monitor capture shows what lies underneath (D01).
//
// Stacking (D25): both windows are topmost and are raised again when another
// window covers them (foreground changes, a move ends, a 1 s check); they stay
// below a revealed system taskbar. While a maximized or full-screen app is in
// front the bar slides down off the screen and comes up again while the
// pointer is at the screen's bottom edge below it (auto-hide; setting autoHide
// can keep it up always, or hide it whatever is in front).
// The render thread also draws the glass layer of Atrium's popups.
#pragma once

#include "render/bar_painter.h"
#include "render/glass_panel.h"
#include "settings.h"
#include "shell/task_model.h"

#include <windows.h>
#include <dwrite_3.h>
#include <ole2.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace atrium {

class Surface;

class Bar : public GlassHost {
public:
    // debugCapturable: development only. Leaves the bar visible to screen capture
    // and freezes the backdrop after the first frame so the glass never samples
    // itself; used to take screenshots of the real surface.
    Bar(HINSTANCE instance, bool debugCapturable);
    ~Bar();
    Bar(const Bar&) = delete;
    Bar& operator=(const Bar&) = delete;

    // UI thread only.
    void Apply(const Settings& s);
    void SetItems(std::vector<TaskItem> items);
    void Relayout();
    void SetVisible(bool visible);
    bool Visible() const { return visible_; }
    // Unread notifications, shown as a badge on the bell.
    void SetUnread(int count);
    // Network, battery and volume for the status icon.
    void SetStatus(const SystemStatus& status);
    // The app at `index` (0-based, in bar order) as a click on it would (Win + digit).
    void ActivateIndex(int index);
    // Screen rectangle of an entry ("#start", "#notify", an app key); empty if not shown.
    RECT EntryRect(const std::wstring& key) const;
    // Screen rectangle of the glass shape.
    RECT BarRect() const;
    // The window that had the foreground before a click on the logo activated the bar.
    HWND PreviousForeground() const { return previousForeground_; }
    // Glass layers of popup windows (GlassHost).
    void SetPanel(const GlassPanel& panel) override;
    // Stops the render thread, pumping this (UI) thread's messages meanwhile, before the popups and
    // widgets it draws for go. False if it did not stop within `timeoutMs` (a stuck driver call).
    bool StopRendering(DWORD timeoutMs);

    std::function<IconImage(const std::wstring& key, int px)> iconProvider;
    std::function<void(const std::wstring& key)> onActivate;  // left click on an app
    std::function<void(const std::wstring& key)> onLaunch;    // middle click on an app
    std::function<void(const std::wstring& key, POINT)> onItemMenu; // right click on an app
    std::function<void(POINT)> onContextMenu;                 // right click elsewhere on the bar
    std::function<void()> onClockClick;
    // Bar entries; `anchor` is the entry's rectangle on screen (popups open above it).
    std::function<void(RECT anchor)> onStartClick;  // the Atrium logo
    std::function<void(RECT anchor)> onNotifyClick; // the notification bell
    std::function<void(RECT anchor)> onTrayClick;   // the tray entry (Atrium's tray panel)
    std::function<void(RECT anchor)> onStatusClick; // the status icon (Wi-Fi, battery: the control centre)
    std::function<void(int notches)> onStatusWheel; // the wheel over the status icon (volume)
    // The overflow entry: apps that did not fit, in bar order.
    std::function<void(const std::vector<std::wstring>& keys, RECT anchor)> onOverflow;
    // Drag and drop of an app to visible position `toIndex` (0-based, in the
    // list without the dragged item). The bar already shows the new order.
    std::function<void(const std::wstring& key, int toIndex)> onReorder;
    // Files dragged from Explorer: hovering an app brings it to the front,
    // dropping opens the files with it.
    std::function<void(const std::wstring& key)> onFileDragHover;
    std::function<bool(const std::wstring& key)> canOpenFiles;
    std::function<void(const std::wstring& key, const std::vector<std::wstring>& files)> onDropFiles;
    std::function<void()> onCloseRequest; // WM_CLOSE on a bar window (taskkill, logoff)
    // True while a popup opened from the bar is open: an auto-hidden bar stays up.
    std::function<bool()> keepShown;
    // The pointer rests on an app with windows: preview them above `anchor` (screen px: the icon's
    // centre, the top of the magnified icons); onPreviewLeave when it moves off, onPreviewClose on a press.
    std::function<void(const std::wstring& key, const std::vector<HWND>& windows, POINT anchor)> onPreview;
    std::function<void()> onPreviewLeave;
    std::function<void()> onPreviewClose;
    std::function<bool()> previewOpen; // a preview is up: moving to another app swaps it at once

private:
    struct Snapshot {
        Settings settings;
        BarScene scene;
        RECT window{};     // visual window, screen px
        RECT monitor{};    // monitor rect, screen px
        HMONITOR hmonitor = nullptr;
        float scale = 1;
        bool visible = true;
        unsigned generation = 0;
        std::vector<GlassPanel> panels;
    };
    struct Hit {
        std::wstring key; // app key, L"#clock", or empty for the bar itself
        RECT rect{};
    };
    struct CachedIcon {
        uint64_t version = 0;
        int px = 0;
        uint64_t id = 0;
        std::shared_ptr<const IconImage> image;
    };

    static LRESULT CALLBACK VisualProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK InputProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnInput(HWND h, UINT m, WPARAM w, LPARAM l);
    void Bounce(const std::wstring& key);
    std::wstring HitTest(POINT client) const;
    // Copies scene state to the render thread; `bar` false: only the popups'
    // glass changed (the bar is not drawn again for it).
    void Publish(bool bar = true);
    void UpdateTooltips();   // refreshes the text of a visible tooltip
    void HideTooltip();
    void ShowTooltip();      // above the hovered item, after a short delay
    std::wstring TooltipText(const std::wstring& key) const;
    void BeginDrag(POINT client);
    void UpdateDrag(POINT client);
    void EndDrag(bool commit);
    friend struct BarDropTarget;
    std::wstring FileDragOver(POINT screen); // returns the app under the point
    void FileDragLeave();
    void RenderThread();
    bool RenderSession(); // returns false to stop, true to rebuild (device lost, monitor changed)
    void SetRegion(bool extended); // input region: the shape, plus the room above it while magnified
    static void CALLBACK WinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD thread, DWORD time);
    void GuardStacking(const wchar_t* why);
    bool ForegroundCoversScreen() const;
    void UpdateCover();    // the foreground app is maximized or full screen on the bar's monitor
    void UpdateAutoHide(); // tucks the bar away or brings it up
    void WatchPointer();   // while covered: the screen's bottom edge brings the bar up, leaving it lets it go
    void Raise();

    bool debugCapturable_ = false;
    void ShowOwnWindowsToCapture(bool visible); // render thread; waits for the UI thread
    HWND visual_ = nullptr;
    HWND input_ = nullptr;
    std::unique_ptr<Surface> tooltip_;
    std::wstring tooltipText_;
    D2D1_SIZE_F tooltipSize_{};
    float tooltipTailX_ = 0;
    Settings settings_;
    std::vector<TaskItem> items_;
    std::map<std::wstring, CachedIcon> icons_;
    uint64_t nextIconId_ = 1;
    BarScene scene_;
    std::vector<Hit> hits_;
    bool previewShown_ = false; // the hovered app's windows are previewed instead of its tooltip
    bool visible_ = true;
    bool tracking_ = false;
    std::wstring pressed_;
    bool pressedLeft_ = false;
    POINT pressPt_{};
    // Drag reorder (client px). The dragged item follows the pointer; the
    // others make room at `dragInsert_`.
    bool dragging_ = false;
    std::wstring dragKey_;
    int dragFrom_ = -1, dragInsert_ = -1;
    int dragX_ = 0, dragY_ = 0, grabDx_ = 0;
    // App strip metrics of the last layout (client px).
    int appsLeft_ = 0, pitch_ = 1, plate_ = 0, gap_ = 0, shown_ = 0;
    std::vector<std::wstring> hidden_; // apps behind the overflow entry
    int unread_ = 0;
    SystemStatus status_;
    bool belowTray_ = false;   // yielded to the system taskbar the user is using
    bool trayHeldUp_ = false;  // the system taskbar is up without the user (logged once)
    HWND previousForeground_ = nullptr;
    RECT applied_{};      // last window rect given to SetWindowPos
    int appliedRgn_[6]{}; // bar width, height, radius, insets and extension of the last region
    // Room around the shape inside the windows (px): sides (shadow plus the
    // magnified bar's stretch), top (shadow plus magnified icons), bottom.
    int insetX_ = 0, insetTop_ = 0, inset_ = 0;
    int rise_ = 0;        // how far magnified or hopping icons reach above the shape
    int radius_ = 0;
    bool regionExtended_ = false;
    std::map<std::wstring, double> bounces_; // key -> start (ClockMs)
    std::map<HWND, GlassPanel> panels_;
    // Stacking guard.
    HWINEVENTHOOK hooks_[3]{};
    // Auto-hide: `covered_` by a maximized or full-screen app, `revealed_` by
    // the pointer; `tucked_` slides the bar off the screen, after which its
    // windows are hidden (`slidOut_`). In fullscreen mode, only a recognized
    // game that fills the monitor suppresses edge reveal.
    bool covered_ = false, suppressReveal_ = false, revealed_ = false, tucked_ = false, slidOut_ = false;
    ULONGLONG leftAt_ = 0; // when the pointer left a revealed bar
    std::map<HWND, std::pair<int, ULONGLONG>> raises_; // windows we were raised over: count, first time
    HWND lastCover_ = nullptr;
    bool appliedVisible_ = false;
    std::wstring fileHoverKey_;
    winrt::com_ptr<IDropTarget> dropTarget_;
    bool oleReady_ = false;
    RECT window_{};
    RECT monitor_{};
    HMONITOR hmonitor_ = nullptr;
    float scale_ = 1;
    winrt::com_ptr<IDWriteFactory3> dwrite_; // UI-thread text measurement

    std::mutex lock_;
    Snapshot snapshot_;
    HANDLE wake_ = nullptr;  // snapshot changed
    HANDLE quit_ = nullptr;
    HANDLE frame_ = nullptr; // a capture frame arrived
    HANDLE captureAck_ = nullptr; // the UI thread changed the windows' capture exclusion
    std::atomic<bool> captureVisible_{false}; // Atrium's windows show in screenshots (render thread)
    bool duplicationFailed_ = false;            // Desktop Duplication cannot run here: Windows.Graphics.Capture
    std::thread render_;
};

} // namespace atrium
