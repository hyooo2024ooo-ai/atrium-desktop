#include "ui/stage_manager.h"

#include "clock.h"
#include "game_window.h"
#include "log.h"
#include "settings.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <thread>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

// DIP. Each set of windows stands in a box kBoxW x kBoxH, kEdge from the screen's edge, the boxes one
// above the other in the middle of what the bar leaves of the screen's height. The windows turn
// kTilt degrees about their upright axis, the side towards the screen's middle away from the eye
// (a viewer kDepth in front of the screen); a pointer over them turns them most of the way back.
// The eye is level with a point kKeystone of the way from a box to the strip's middle: the boxes
// above it lean down towards the middle of the screen, those below lean up, the more so the
// further they are.
constexpr float kEdge = 22, kBoxW = 172, kBoxH = 112, kSlotGap = 36, kVPad = 24, kStripW = kEdge + kBoxW + 44;
constexpr float kSlot = kBoxH + kSlotGap;
constexpr float kTilt = 22, kHoverTilt = 0.7f, kDepth = 340, kComeTilt = 34, kKeystone = 0.45f;
constexpr size_t kStack = 3;                                 // windows shown per set
constexpr float kStackDx = 9, kStackDy = 8, kStackZ = 26;  // each window behind the front one: inwards, up, away
constexpr float kShadowDrop = 6, kShadowAlpha = 0.5f;
constexpr float kIcon = 34;
constexpr float kPictureScale = 1.25f; // pictures are taken this much larger than the box (the pointer enlarges them)
constexpr float kPictureRadius = 8;
constexpr float kBarRoom = 90;         // the bar floats over the bottom of the work area
constexpr int kMaxEntries = 6;
constexpr UINT_PTR kWatchTimer = 0x5C01, kDownTimer = 0x5C02, kRaiseTimer = 0x5C03, kHideTimer = 0x5C04, kFlightTimer = 0x5C05;
constexpr UINT kWatchMs = 30, kRaiseMs = 220, kDownMs = 700;
constexpr UINT WM_STAGE_PICTURE = WM_APP + 0x5C; // a window's picture was taken
// The pointer at the screen's edge for kEdgeMs brings the strip out; away from it for kPeekLeaveMs
// sends it back. Whether a window covers the strip's place is looked at every kCoverMs.
constexpr ULONGLONG kEdgeMs = 40, kPeekLeaveMs = 350, kCoverMs = 150, kPendingMs = 1200, kSettleMs = 2000;
constexpr double kRevealInMs = 380, kRevealOutMs = 210, kRevealStagger = 32;
// A picture flies between a window's place and the strip in kFlightMs; flying back, the window comes
// up once it has landed, under it: the picture stays kFlightHoldMs (the window draws itself), then
// fades over kFlightFadeMs. The flight layer stays (letting clicks through) kFlightIdleMs after.
constexpr double kFlightMs = 460, kFlightHoldMs = 140, kFlightFadeMs = 160, kFlightStepMs = 34;
constexpr UINT kFlightIdleMs = 60000;
// A window's picture taken this recently (the pointer went to the strip) is used as it is: the window
// goes at once.
constexpr ULONGLONG kFreshMs = 4000;

std::wstring StatePath()
{
    std::wstring path = SettingsPath();
    return path.substr(0, path.find_last_of(L'\\')) + L"\\stage.json";
}

// Back without taking the foreground. Never a synchronous call into another app's window: one that
// hangs must not hang the bar.
void ShowQuietly(HWND h) { ShowWindowAsync(h, SW_SHOWNOACTIVATE); }

bool OnMonitor(HWND h, HMONITOR m) { return MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) == m; }

bool Has(const std::vector<std::wstring>& set, const std::wstring& key) { return std::find(set.begin(), set.end(), key) != set.end(); }

bool AppsDark()
{
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD,
                 nullptr, &light, &size);
    return light == 0;
}

// Turns what is drawn about the upright axis through (cx, cy) by `angle` degrees, `z` towards the eye,
// seen from `depth` in front of (ox, oy); px.
D2D1::Matrix4x4F Tilt(float cx, float cy, float ox, float oy, float angle, float depth, float z)
{
    using M = D2D1::Matrix4x4F;
    return M::Translation(-cx, -cy, 0) * M::RotationY(angle) * M::Translation(cx - ox, cy - oy, z) * M::PerspectiveProjection(depth) *
           M::Translation(ox, oy, 0);
}

// A window's own minimize and restore animations: off while Stage Manager flies its picture.
void Transitions(HWND h, bool on)
{
    BOOL off = on ? FALSE : TRUE;
    DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
}

float EaseInOut(float t) { return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.f) / 2; }

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

D2D1_POINT_2F Project(const D2D1_MATRIX_4X4_F& m, float x, float y)
{
    const float X = x * m._11 + y * m._21 + m._41, Y = x * m._12 + y * m._22 + m._42, W = x * m._14 + y * m._24 + m._44;
    return D2D1::Point2F(X / W, Y / W);
}

float EaseOut3(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

constexpr motion::Spring kGlide{0.42f, 0.86f};
constexpr motion::Spring kFadeSpring{0.30f, 1.f};

} // namespace

StageManager::StageManager(HINSTANCE instance, TaskModel& tasks)
    : tasks_(tasks), surface_(instance, L"Atrium Stage Manager", false), flight_(instance, L"Atrium Stage Flight", false, 0),
      snapshots_(surface_.Hwnd(), WM_STAGE_PICTURE)
{
    surface_.SetGlassless(true);
    flight_.SetGlassless(true);
    flight_.paint = [this](Canvas& c) { PaintFlights(c); };
    flight_.message = [this](UINT m, WPARAM w, LPARAM, LRESULT&) {
        if (m != WM_TIMER || w != kFlightTimer)
            return false;
        KillTimer(flight_.Hwnd(), kFlightTimer);
        if (flights_.empty())
            flight_.Hide(false); // idle for a while: its buffers go
        return true;
    };
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    if (FAILED(CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(desktops_.put()))))
        desktops_ = nullptr;
    Load(); // windows a previous run put away (it may not have ended well)
}

// ---- the stage ---------------------------------------------------------------------------------

void StageManager::SetEnabled(bool on)
{
    if (on == enabled_ && applied_)
        return;
    applied_ = true;
    enabled_ = on;
    if (on) {
        stage_.clear();
        pending_.clear();
        SetTimer(surface_.Hwnd(), kWatchTimer, kWatchMs, nullptr);
        Log(L"stage: on (%s, %s), %zu window(s) waiting from before", onRight_ ? L"right" : L"left", autoHide_ ? L"auto-hidden" : L"always shown",
            parked_.size());
        Update(tasks_.Snapshot(true));
    } else {
        KillTimer(surface_.Hwnd(), kWatchTimer);
        size_t n = parked_.size();
        HWND front = GetForegroundWindow();
        RestoreAll();
        // The windows coming back must not cover the one in front.
        if (n && front) {
            raise_ = front;
            SetTimer(surface_.Hwnd(), kRaiseTimer, kRaiseMs, nullptr);
        }
        Log(L"stage: off, %zu window(s) back", n);
    }
}

void StageManager::SetOnRight(bool onRight)
{
    if (onRight == onRight_)
        return;
    onRight_ = onRight;
    // It comes out again on the other side.
    shown_ = peek_ = false;
    surface_.Hide(false);
    Rebuild();
}

void StageManager::SetAutoHide(bool autoHide)
{
    if (autoHide == autoHide_)
        return;
    autoHide_ = autoHide;
    peek_ = false;
    Log(L"stage: %s", autoHide ? L"auto-hidden" : L"always shown");
    if (enabled_)
        Watch();
}

void StageManager::SetMonitor(HMONITOR monitor)
{
    if (monitor == monitor_)
        return;
    monitor_ = monitor;
    Rebuild();
}

void StageManager::Update(const std::vector<TaskItem>& items)
{
    items_ = items;
    coveredAt_ = 0; // windows moved: look again
    if (!enabled_)
        return;
    Prune();
    if (!pending_.empty() && GetTickCount64() > pendingUntil_)
        pending_.clear();
    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    auto active = std::find_if(items_.begin(), items_.end(), [](const TaskItem& t) { return t.active; });
    // A window in front, on this screen, of an app not on the stage: its set takes the stage. Not
    // while a set picked in the strip is on its way to the front.
    if (active != items_.end() && root && !IsIconic(root) && OnMonitor(root, monitor_)) {
        if (pending_.empty() || Has(pending_, active->key)) {
            pending_.clear();
            if (!Has(stage_, active->key))
                Stage(SetOf(active->key), false, root, false);
        }
    }
    Rebuild();
}

StageManager::Set StageManager::SetOf(const std::wstring& key) const
{
    for (const Set& set : recent_)
        if (Has(set, key))
            return set;
    return Set{key};
}

// `keys` take the stage (or join it: add). Their windows that were put away come back, `front` in
// front; then every other app's windows on this screen go to the strip: the apps that were on the
// stage as one set, at the top.
void StageManager::Stage(const Set& keys, bool add, HWND front, bool activate, const std::unordered_map<HWND, Pose>* fly)
{
    Unpark(keys, front, activate, fly);
    if (!add) {
        Set left; // the stage's apps that had windows up
        std::vector<Set> others;
        for (const TaskItem& t : items_) {
            if (Has(keys, t.key))
                continue;
            bool any = false;
            // Least recent first: the most recent window gets the latest number and shows in front.
            for (auto h = t.windows.rbegin(); h != t.windows.rend(); ++h)
                if (!parked_.contains(*h) && IsWindowVisible(*h) && !IsIconic(*h) && OnMonitor(*h, monitor_)) {
                    Park(*h, t.key);
                    any = true;
                }
            if (!any)
                continue;
            if (Has(stage_, t.key))
                left.push_back(t.key);
            else
                others.push_back(Set{t.key});
        }
        // The most recently used first: the windows nearest the top of the screen's stack.
        std::unordered_map<HWND, int> depth;
        int n = 0;
        for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
            depth[h] = n++;
        auto nearest = [&](const Set& set) {
            int best = INT_MAX;
            for (const auto& [h, p] : parked_)
                if (Has(set, p.key))
                    if (auto d = depth.find(h); d != depth.end())
                        best = std::min(best, d->second);
            return best;
        };
        std::stable_sort(others.begin(), others.end(), [&](const Set& a, const Set& b) { return nearest(a) < nearest(b); });
        if (!left.empty())
            others.insert(others.begin(), left);
        // An app is in one set only: the newest.
        for (const Set& set : others)
            for (const std::wstring& k : set)
                for (Set& old : recent_)
                    std::erase(old, k);
        std::erase_if(recent_, [](const Set& set) { return set.empty(); });
        recent_.insert(recent_.begin(), others.begin(), others.end());
        stage_.clear();
        if (!others.empty())
            Log(L"stage: %s on the stage, %zu set(s) to the strip", keys.empty() ? L"the desktop" : keys.front().c_str(), others.size());
    } else {
        Log(L"stage: %s joins the stage", keys.front().c_str());
    }
    for (const std::wstring& k : keys) {
        if (!Has(stage_, k))
            stage_.push_back(k);
        for (Set& old : recent_)
            std::erase(old, k);
    }
    std::erase_if(recent_, [](const Set& set) { return set.empty(); });
    Save();
}

// The window's picture is taken first (a minimized window has none); it goes down once it has, or
// after kDownMs whatever happened.
void StageManager::Park(HWND window, const std::wstring& key)
{
    Parked p;
    p.key = key;
    p.seq = ++seq_;
    p.since = GetTickCount64();
    p.down = false;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &p.frame, sizeof(p.frame))))
        GetWindowRect(window, &p.frame);
    parked_[window] = p;
    Transitions(window, false); // it goes at once: its picture flies instead
    if (auto fresh = pictures_.find(window);
        fresh != pictures_.end() && fresh->second.picture && fresh->second.picture->ok && GetTickCount64() - fresh->second.taken < kFreshMs) {
        PutDown(window, true);
        return;
    }
    const float s = PopupScale(WorkArea());
    snapshots_.Request(window, int(kBoxW * kPictureScale * s), int(kBoxH * kPictureScale * s), kPictureRadius * kPictureScale * s);
    SetTimer(surface_.Hwnd(), kDownTimer, kDownMs, nullptr);
}

// The flight layer over the whole screen: kept up between flights (its first frame is slow), and
// letting clicks through while nothing flies.
void StageManager::Launch()
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    KillTimer(flight_.Hwnd(), kFlightTimer);
    SetWindowRgn(flight_.Hwnd(), nullptr, FALSE);
    if (!flight_.Visible())
        flight_.Show(mi.rcMonitor, false, POINT{mi.rcMonitor.left, mi.rcMonitor.top}, false);
    else
        flight_.Move(mi.rcMonitor);
    flight_.Invalidate();
}

// The stage's windows are pictured while the pointer is on the strip: picked there, they go at once.
void StageManager::Foresee()
{
    const ULONGLONG now = GetTickCount64();
    const float s = PopupScale(WorkArea());
    for (const TaskItem& t : items_) {
        if (!Has(stage_, t.key))
            continue;
        for (HWND h : t.windows) {
            if (parked_.contains(h) || !IsWindowVisible(h) || IsIconic(h) || !OnMonitor(h, monitor_))
                continue;
            if (auto p = pictures_.find(h); p != pictures_.end() && now - p->second.taken < kFreshMs / 2)
                continue;
            pictures_[h].taken = now; // asked for: not again meanwhile
            snapshots_.Request(h, int(kBoxW * kPictureScale * s), int(kBoxH * kPictureScale * s), kPictureRadius * kPictureScale * s);
        }
    }
}

void StageManager::PutDown(HWND window, bool fly)
{
    auto it = parked_.find(window);
    if (it == parked_.end() || it->second.down)
        return;
    it->second.down = true;
    it->second.since = GetTickCount64();
    std::wstring key;
    auto picture = fly ? PictureOf(window, it->second.key, key) : nullptr;
    if (!picture || !picture->ok || key.rfind(L"stage|w|", 0) != 0) {
        Transitions(window, true); // no picture to fly: Windows' own animation
        ShowWindowAsync(window, SW_SHOWMINNOACTIVE);
        return;
    }
    ShowWindowAsync(window, SW_SHOWMINNOACTIVE);
    std::erase_if(flights_, [&](const Flight& f) { return f.window == window; });
    Flight f;
    f.window = window;
    f.picture = picture;
    f.key = key;
    f.toStrip = true;
    f.place = PlacePose(it->second.frame);
    flights_.push_back(std::move(f));
    Launch();
    surface_.Invalidate();
}

void StageManager::PictureTaken()
{
    for (WindowSnapshots::Result& r : snapshots_.Take()) {
        Log(L"stage: picture of %p %s", r.window, r.picture.ok ? (std::to_wstring(r.picture.width) + L"x" + std::to_wstring(r.picture.height)).c_str() : L"(none)");
        if (r.picture.ok) {
            Picture& p = pictures_[r.window];
            if (p.gen) {
                std::wstring old = L"stage|w|" + std::to_wstring(reinterpret_cast<uintptr_t>(r.window)) + L"|" + std::to_wstring(p.gen);
                surface_.ForgetImage(old);
                surface_.ForgetImage(old + L"|s");
            }
            p.picture = std::make_shared<const WindowPicture>(std::move(r.picture));
            p.gen = ++pictureGen_;
            p.taken = GetTickCount64();
            PutDown(r.window, true);
        } else {
            PutDown(r.window, false);
        }
    }
    surface_.Invalidate();
}

// The windows of `keys` that were put away come back, least recent first; `front` (by default the
// most recent of them) last, and in front, activated if asked (a pick in the strip).
void StageManager::Unpark(const Set& keys, HWND front, bool activate, const std::unordered_map<HWND, Pose>* fly)
{
    std::vector<std::pair<uint64_t, HWND>> mine;
    for (const auto& [h, p] : parked_)
        if (Has(keys, p.key))
            mine.emplace_back(p.seq, h);
    std::sort(mine.begin(), mine.end());
    if (!front && !mine.empty())
        front = mine.back().second;
    // Picked in the strip: the pictures fly back to the windows' places, the windows come up as they land.
    bool flying = false;
    for (const auto& [seq, h] : mine) {
        const Pose* from = nullptr;
        if (fly)
            if (auto it = fly->find(h); it != fly->end())
                from = &it->second;
        std::wstring key;
        auto picture = from ? PictureOf(h, parked_[h].key, key) : nullptr;
        std::erase_if(flights_, [&](const Flight& f) { return f.window == h; });
        if (!picture || !IsWindow(h) || key.rfind(L"stage|w|", 0) != 0)
            continue;
        Flight f;
        f.window = h;
        f.picture = picture;
        f.key = key;
        f.toStrip = false;
        f.from = *from;
        f.place = PlacePose(parked_[h].frame);
        f.activate = activate && h == front;
        flights_.push_back(std::move(f));
        flying = true;
    }
    if (flying)
        Launch();
    for (const auto& [seq, h] : mine) {
        parked_.erase(h);
        if (Flying(h))
            continue; // up when its picture lands
        Transitions(h, true);
        if (h != front && IsWindow(h))
            ShowQuietly(h);
    }
    if (!front)
        return;
    parked_.erase(front);
    if (Flying(front)) {
        // Up (and in front) when its picture lands.
    } else if (activate) {
        // Restoring waits for the app to answer (and for its animation): not on the UI thread,
        // whose strip is gliding meanwhile. The click on the strip lets this process hand on the
        // foreground from any of its threads.
        std::thread([front] { TaskModel::ActivateWindow(front); }).detach();
    } else if (IsIconic(front))
        ShowQuietly(front);
    if (!mine.empty()) {
        // The others come back asynchronously: `front` goes on top again once they have.
        raise_ = front;
        SetTimer(surface_.Hwnd(), kRaiseTimer, kRaiseMs, nullptr);
    }
}

void StageManager::Pick(const Set& keys, bool add)
{
    pending_ = keys;
    pendingUntil_ = GetTickCount64() + kPendingMs;
    hover_ = pressed_ = -1;
    // Where the card's pictures are now: they fly from there.
    std::unordered_map<HWND, Pose> from;
    for (const Entry& e : entries_)
        if (e.keys == keys)
            for (HWND h : e.windows) {
                Pose pose;
                if (StripPose(h, pose))
                    from[h] = pose;
            }
    pendingUntil_ += ULONGLONG(kFlightMs);
    Stage(keys, add, nullptr, true, &from);
    Rebuild();
}

void StageManager::RestoreAll()
{
    // Pictures on their way back: their windows come up now.
    for (Flight& f : flights_)
        if (!f.toStrip && !f.restored && IsWindow(f.window)) {
            Transitions(f.window, true);
            ShowQuietly(f.window);
        }
    flights_.clear();
    flight_.Hide(false);
    for (const auto& [h, p] : parked_)
        if (IsWindow(h))
            Transitions(h, true);
    std::vector<std::pair<uint64_t, HWND>> all;
    for (const auto& [h, p] : parked_)
        all.emplace_back(p.seq, h);
    std::sort(all.begin(), all.end());
    for (const auto& [seq, h] : all)
        if (IsWindow(h) && IsIconic(h))
            ShowQuietly(h);
    parked_.clear();
    recent_.clear();
    stage_.clear();
    pending_.clear();
    pictures_.clear();
    Save();
    Rebuild();
}

// Windows that went (closed, hidden to the tray) or came back some other way are no longer waiting.
void StageManager::Prune()
{
    const ULONGLONG now = GetTickCount64();
    std::erase_if(parked_, [&](const auto& kv) {
        HWND h = kv.first;
        if (!IsWindow(h) || !IsWindowVisible(h))
            return true;
        // Minimizing is asynchronous (and waits for the picture): a window just put away may not be down yet.
        if (kv.second.down && !IsIconic(h) && now - kv.second.since > kSettleMs) {
            Transitions(h, true); // came back some other way
            return true;
        }
        return false;
    });
    for (Set& set : recent_)
        std::erase_if(set, [&](const std::wstring& k) {
            return std::none_of(parked_.begin(), parked_.end(), [&](const auto& kv) { return kv.second.key == k; });
        });
    std::erase_if(recent_, [](const Set& set) { return set.empty(); });
    // Pictures of windows no longer waiting are old by the time they wait again.
    std::erase_if(pictures_, [&](const auto& kv) {
        if (parked_.contains(kv.first) || (IsWindow(kv.first) && now - kv.second.taken < kFreshMs))
            return false;
        std::wstring key = L"stage|w|" + std::to_wstring(reinterpret_cast<uintptr_t>(kv.first)) + L"|" + std::to_wstring(kv.second.gen);
        surface_.ForgetImage(key);
        surface_.ForgetImage(key + L"|s");
        return true;
    });
    Save();
}

void StageManager::Save()
{
    json::JsonArray list;
    std::vector<std::pair<uint64_t, HWND>> all;
    for (const auto& [h, p] : parked_)
        all.emplace_back(p.seq, h);
    std::sort(all.begin(), all.end());
    for (const auto& [seq, h] : all) {
        json::JsonObject o;
        o.SetNamedValue(L"hwnd", json::JsonValue::CreateNumberValue(double(reinterpret_cast<uintptr_t>(h))));
        o.SetNamedValue(L"key", json::JsonValue::CreateStringValue(parked_[h].key));
        list.Append(o);
    }
    json::JsonArray sets;
    for (const Set& set : recent_) {
        json::JsonArray keys;
        for (const std::wstring& k : set)
            keys.Append(json::JsonValue::CreateStringValue(k));
        sets.Append(keys);
    }
    json::JsonObject root;
    root.SetNamedValue(L"parked", list);
    root.SetNamedValue(L"sets", sets);
    std::wstring text(root.Stringify());
    if (text == saved_)
        return;
    saved_ = text;
    if (!WriteTextFile(StatePath(), text))
        Log(L"stage: could not write %s", StatePath().c_str());
}

void StageManager::Load()
{
    json::JsonObject root{nullptr};
    std::wstring text = ReadTextFile(StatePath());
    if (text.empty() || !json::JsonObject::TryParse(text, root))
        return;
    saved_ = text;
    if (root.HasKey(L"parked") && root.GetNamedValue(L"parked").ValueType() == json::JsonValueType::Array)
        for (const auto& v : root.GetNamedArray(L"parked")) {
            if (v.ValueType() != json::JsonValueType::Object)
                continue;
            auto o = v.GetObject();
            if (!o.HasKey(L"hwnd") || !o.HasKey(L"key") || o.GetNamedValue(L"hwnd").ValueType() != json::JsonValueType::Number ||
                o.GetNamedValue(L"key").ValueType() != json::JsonValueType::String)
                continue;
            HWND h = reinterpret_cast<HWND>(uintptr_t(o.GetNamedNumber(L"hwnd")));
            // A window handle is only reused once its window is gone; one still down is the same.
            if (!IsWindow(h) || !IsWindowVisible(h) || !IsIconic(h))
                continue;
            Parked p;
            p.key = std::wstring(o.GetNamedString(L"key"));
            p.seq = ++seq_;
            parked_[h] = p;
        }
    if (root.HasKey(L"sets") && root.GetNamedValue(L"sets").ValueType() == json::JsonValueType::Array)
        for (const auto& v : root.GetNamedArray(L"sets")) {
            if (v.ValueType() != json::JsonValueType::Array)
                continue;
            Set set;
            for (const auto& k : v.GetArray())
                if (k.ValueType() == json::JsonValueType::String && !Has(set, std::wstring(k.GetString())))
                    set.push_back(std::wstring(k.GetString()));
            if (!set.empty())
                recent_.push_back(std::move(set));
        }
    // Apps only known from their windows: each a set of its own.
    for (const auto& [h, p] : parked_)
        if (std::none_of(recent_.begin(), recent_.end(), [&](const Set& set) { return Has(set, p.key); }))
            recent_.push_back(Set{p.key});
    for (Set& set : recent_)
        std::erase_if(set, [&](const std::wstring& k) {
            return std::none_of(parked_.begin(), parked_.end(), [&](const auto& kv) { return kv.second.key == k; });
        });
    std::erase_if(recent_, [](const Set& set) { return set.empty(); });
}

// ---- the strip ---------------------------------------------------------------------------------

RECT StageManager::WorkArea() const
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcWork;
}

int StageManager::Capacity() const
{
    RECT wa = WorkArea();
    float avail = float(wa.bottom - wa.top) / PopupScale(wa) - 2 * kVPad - kBarRoom;
    return std::clamp(int((avail + kSlotGap) / kSlot), 1, kMaxEntries);
}

void StageManager::Rebuild()
{
    std::vector<Entry> next;
    if (enabled_) {
        const int cap = Capacity();
        for (const Set& set : recent_) {
            if (int(next.size()) >= cap)
                break;
            std::vector<std::pair<uint64_t, HWND>> mine;
            for (const auto& [h, p] : parked_) {
                if (!Has(set, p.key) || !IsWindow(h))
                    continue;
                BOOL here = TRUE; // windows put away on another virtual desktop wait there
                if (desktops_ && SUCCEEDED(desktops_->IsWindowOnCurrentVirtualDesktop(h, &here)) && !here)
                    continue;
                mine.emplace_back(p.seq, h);
            }
            if (mine.empty())
                continue;
            std::sort(mine.rbegin(), mine.rend()); // most recent first
            Entry e;
            e.keys = set;
            std::stable_sort(e.keys.begin(), e.keys.end(), [&](const std::wstring& a, const std::wstring& b) {
                auto newest = [&](const std::wstring& k) {
                    uint64_t n = 0;
                    for (const auto& [seq, h] : mine)
                        if (parked_[h].key == k)
                            n = std::max(n, seq);
                    return n;
                };
                return newest(a) > newest(b);
            });
            for (size_t i = 0; i < mine.size() && i < kStack; ++i)
                e.windows.push_back(mine[i].second);
            next.push_back(std::move(e));
        }
    }
    const bool visible = shown_ && surface_.Visible();
    // A set keeps its card (where it is, how far it is hovered).
    for (Entry& e : next) {
        auto old = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& o) { return o.keys == e.keys; });
        if (old != entries_.end()) {
            e.hover = old->hover;
            e.y = old->y;
            e.vy = old->vy;
            e.appear = old->appear;
            e.va = old->va;
            old->keys.clear(); // taken
        } else {
            e.appear = visible ? 0.f : 1.f; // a new card fades in; a strip coming out slides as a whole
        }
    }
    // Cards that went fade out where they were.
    for (Entry& old : entries_) {
        if (old.keys.empty() || !visible || old.appear < 0.02f)
            continue;
        old.targetAppear = 0;
        old.hover = motion::Follow{};
        leaving_.push_back(std::move(old));
    }
    if (!visible)
        leaving_.clear();
    const bool changed = next.size() != entries_.size() ||
                         !std::equal(next.begin(), next.end(), entries_.begin(), [](const Entry& a, const Entry& b) { return a.keys == b.keys; }) ||
                         !leaving_.empty();
    entries_ = std::move(next);
    if (hover_ >= int(entries_.size()))
        hover_ = -1;
    if (pressed_ >= int(entries_.size()))
        pressed_ = -1;
    Layout();
    if (entries_.empty() && leaving_.empty()) {
        Hide();
        return;
    }
    if (surface_.Visible()) {
        surface_.Move(rect_);
        surface_.Invalidate();
    }
    if (changed)
        Watch();
}

// The strip's window has room for Capacity() boxes, in the middle of what the bar leaves of the
// screen's height, from the screen's edge; the boxes stand in the middle of it, and glide when
// that changes.
void StageManager::Layout()
{
    RECT wa = WorkArea();
    const float s = PopupScale(wa);
    const int cap = Capacity();
    size_ = D2D1::SizeF(kStripW, float(cap) * kSlot - kSlotGap + 2 * kVPad);
    const size_t n = entries_.size();
    const float used = n ? float(n) * kSlot - kSlotGap : 0;
    const float top = (size_.height - used) / 2;
    const bool snap = !surface_.Visible();
    for (size_t i = 0; i < n; ++i) {
        Entry& e = entries_[i];
        e.targetY = top + float(i) * kSlot;
        if (snap || e.appear == 0) {
            e.y = e.targetY; // a new card fades in at its place
            e.vy = 0;
        }
    }
    const LONG w = LONG(std::lround(size_.width * s)), h = LONG(std::lround(size_.height * s));
    const LONG room = LONG(std::lround(kBarRoom * s));
    LONG x = onRight_ ? wa.right - w : wa.left;
    LONG y = wa.top + std::max<LONG>(0, (wa.bottom - wa.top - room - h) / 2);
    rect_ = RECT{x, y, x + w, y + h};
}

D2D1_RECT_F StageManager::Box(const Entry& e) const
{
    const float x = onRight_ ? size_.width - kEdge - kBoxW : kEdge;
    return D2D1::RectF(x, e.y, x + kBoxW, e.y + kBoxH);
}

float StageManager::Reveal(int index, double nowMs) const
{
    // Out: one card after the other, from the top. In: all together.
    const double delay = shown_ && revealFrom_ < 0.01f ? kRevealStagger * index : 0;
    const float t = float(std::clamp((nowMs - revealStart_ - delay) / (shown_ ? kRevealInMs : kRevealOutMs), 0.0, 1.0));
    const float target = shown_ ? 1.f : 0.f;
    return revealFrom_ + (target - revealFrom_) * (shown_ ? EaseOut3(t) : t * t);
}

bool StageManager::Animate(double nowMs)
{
    const float dt = lastStep_ > 0 ? float(std::clamp((nowMs - lastStep_) / 1000.0, 0.0, 0.05)) : 0.f;
    lastStep_ = nowMs;
    bool moving = false;
    auto step = [&](Entry& e) {
        kGlide.Step(e.y, e.vy, e.targetY, dt);
        kFadeSpring.Step(e.appear, e.va, e.targetAppear, dt);
        if (motion::Spring::AtRest(e.y, e.vy, e.targetY, 0.2f))
            e.y = e.targetY, e.vy = 0;
        else
            moving = true;
        if (motion::Spring::AtRest(e.appear, e.va, e.targetAppear, 0.004f))
            e.appear = e.targetAppear, e.va = 0;
        else
            moving = true;
        e.appear = std::clamp(e.appear, 0.f, 1.f);
    };
    for (Entry& e : entries_)
        step(e);
    for (Entry& e : leaving_)
        step(e);
    std::erase_if(leaving_, [](const Entry& e) { return e.appear <= 0.01f; });
    const double span = shown_ ? kRevealInMs + kRevealStagger * double(entries_.size()) : kRevealOutMs;
    if (nowMs - revealStart_ < span)
        moving = true;
    if (!moving)
        lastStep_ = 0;
    return moving;
}

// The boxes' span, with room for their shadows and a hovered box's growth: the rest of the column
// lets clicks through. While cards move the region only grows (where they go is in it already); it
// is fitted again when they rest.
void StageManager::UpdateRegion(bool moving)
{
    float top = size_.height, bottom = 0;
    for (const auto* list : {&entries_, &leaving_})
        for (const Entry& e : *list) {
            top = std::min({top, e.y, e.targetY});
            bottom = std::max({bottom, e.y + kBoxH, e.targetY + kBoxH});
        }
    RECT wr{};
    if (!GetWindowRect(surface_.Hwnd(), &wr))
        return;
    const float s = surface_.Scale();
    const LONG inset = rect_.top - wr.top;
    RECT r{0, std::max<LONG>(0, inset + LONG(std::floor((top - 24) * s))), wr.right - wr.left,
           std::min<LONG>(wr.bottom - wr.top, inset + LONG(std::ceil((bottom + 34) * s)))};
    if (bottom <= top)
        r = RECT{0, 0, 0, 0};
    if (moving && !IsRectEmpty(&region_))
        UnionRect(&r, &r, &region_);
    if (EqualRect(&r, &region_))
        return;
    region_ = r;
    HRGN rgn = CreateRectRgn(r.left, r.top, r.right, r.bottom);
    if (!SetWindowRgn(surface_.Hwnd(), rgn, FALSE))
        DeleteObject(rgn);
}

RECT StageManager::Zone() const
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    return onRight_ ? RECT{rect_.left, rect_.top, mi.rcMonitor.right, rect_.bottom} : RECT{mi.rcMonitor.left, rect_.top, rect_.right, rect_.bottom};
}

bool StageManager::Covered()
{
    const ULONGLONG now = GetTickCount64();
    if (coveredAt_ && now - coveredAt_ < kCoverMs)
        return covered_;
    coveredAt_ = now;
    covered_ = false;
    const RECT zone = Zone();
    for (const TaskItem& t : items_)
        for (HWND h : t.windows) {
            if (parked_.contains(h) || !IsWindowVisible(h) || IsIconic(h) || !OnMonitor(h, monitor_))
                continue;
            RECT r{}, hit{};
            if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) && !GetWindowRect(h, &r))
                continue;
            BOOL cloaked = FALSE;
            if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
                continue; // on another virtual desktop
            if (IntersectRect(&hit, &r, &zone))
                return covered_ = true;
        }
    return covered_;
}

// Every kWatchMs while on. Always shown, the strip is out unless a window covers its place; then,
// as auto-hidden, it comes out over the windows while the pointer rests on that edge of the screen,
// and goes once the pointer has left it.
void StageManager::Watch()
{
    // A click on the desktop clears the stage: its windows go to the strip together, as one set.
    if (enabled_) {
        HWND fg = GetForegroundWindow();
        wchar_t cls[16] = {};
        const bool desktop = fg && GetClassNameW(fg, cls, 16) && (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW"));
        if (desktop && !onDesktop_ && !stage_.empty()) {
            pending_.clear();
            Stage(Set{}, false, nullptr, false);
            Rebuild();
        }
        onDesktop_ = desktop;
    }
    if (!enabled_ || entries_.empty()) {
        peek_ = false;
        edgeSince_ = 0;
        if (leaving_.empty())
            Hide();
        return;
    }
    // Only a full-screen game owns the side edge. Full-screen browsers,
    // presentations and maximized applications retain the normal edge peek.
    HMONITOR monitor = monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (IsFullscreenGame(GetForegroundWindow(), monitor)) {
        peek_ = false;
        edgeSince_ = leftAt_ = 0;
        Hide();
        return;
    }
    const ULONGLONG now = GetTickCount64();
    POINT p{};
    GetCursorPos(&p);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    // Anywhere along the edge above the bar; not while dragging (a window to snap, a selection).
    const bool dragging = (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
    const bool atEdge = !dragging && (onRight_ ? p.x >= mi.rcMonitor.right - 2 : p.x <= mi.rcMonitor.left + 1) && p.y >= mi.rcMonitor.top &&
                        p.y < mi.rcWork.bottom;
    edgeSince_ = atEdge ? (edgeSince_ ? edgeSince_ : now) : 0;
    if (!autoHide_ && !Covered()) {
        peek_ = false;
        Show();
        return;
    }
    if (!peek_ && edgeSince_ && now - edgeSince_ >= kEdgeMs) {
        peek_ = true;
        leftAt_ = 0;
    }
    if (peek_) {
        const RECT zone = Zone();
        if (atEdge || inside_ || PtInRect(&zone, p))
            leftAt_ = 0;
        else if (!leftAt_)
            leftAt_ = now;
        else if (now - leftAt_ >= kPeekLeaveMs)
            peek_ = false;
    }
    if (peek_)
        Show();
    else
        Hide();
}

void StageManager::Show()
{
    if (shown_) {
        if (surface_.Visible())
            surface_.Move(rect_);
        return;
    }
    const double now = ClockMs();
    const bool visible = surface_.Visible();
    revealFrom_ = visible ? Reveal(0, now) : 0.f; // turning back on its way in
    revealStart_ = now;
    shown_ = true;
    if (!visible) {
        hover_ = pressed_ = -1;
        leaving_.clear();
        for (Entry& e : entries_) {
            e.y = e.targetY, e.vy = 0;
            e.appear = e.targetAppear = 1, e.va = 0;
            e.hover = motion::Follow{};
        }
        region_ = RECT{};
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        POINT anchor{onRight_ ? mi.rcMonitor.right : mi.rcMonitor.left, (rect_.top + rect_.bottom) / 2};
        surface_.Show(rect_, false, anchor, false); // the cards slide out themselves
    } else {
        surface_.Move(rect_);
        surface_.Invalidate();
    }
}

void StageManager::Hide()
{
    if (!shown_)
        return;
    const double now = ClockMs();
    revealFrom_ = Reveal(0, now);
    revealStart_ = now;
    shown_ = false;
    inside_ = false;
    hover_ = pressed_ = -1;
    surface_.Invalidate(); // the cards slide back into the edge; then the window goes
}

std::shared_ptr<const WindowPicture> StageManager::PictureOf(HWND window, const std::wstring& app, std::wstring& cacheKey)
{
    auto it = pictures_.find(window);
    if (it != pictures_.end() && it->second.picture && it->second.picture->ok) {
        cacheKey = L"stage|w|" + std::to_wstring(reinterpret_cast<uintptr_t>(window)) + L"|" + std::to_wstring(it->second.gen);
        return it->second.picture;
    }
    // No picture (put away before Atrium started, or one that cannot be printed): its app's icon.
    cacheKey = L"stage|p|" + app;
    auto& p = placeholders_[app];
    if (!p) {
        const float s = PopupScale(WorkArea());
        const int w = int(std::lround(kBoxW * s)), h = int(std::lround(kBoxW * s / 1.6f));
        // The largest icon the app has, up to 52 DIP.
        IconImage icon;
        for (int px : {int(std::lround(52 * s)), 64, 48, 32})
            if (!(icon = tasks_.Icon(app, px)).empty())
                break;
        p = std::make_shared<const WindowPicture>(PlaceholderPicture(icon, w, h, AppsDark()));
    }
    return p;
}

StageManager::Pose StageManager::PoseOf(const Entry& e, size_t j, const WindowPicture& p, float angle) const
{
    const float dir = onRight_ ? -1.f : 1.f;
    const D2D1_RECT_F box = Box(e);
    Pose pose;
    pose.cx = pose.ox = (box.left + box.right) / 2;
    pose.cy = (box.top + box.bottom) / 2;
    pose.oy = pose.cy + kKeystone * (size_.height / 2 - pose.cy);
    pose.angle = angle;
    pose.z = -kStackZ * float(j);
    const float f = std::min(kBoxW / float(p.width), kBoxH / float(p.height)) * std::pow(0.93f, float(j)); // DIP per picture px
    const float w = float(p.width) * f, h = float(p.height) * f;
    const float x = pose.cx - w / 2 + dir * kStackDx * float(j), y = pose.cy - h / 2 - kStackDy * float(j);
    pose.rect = D2D1::RectF(x, y, x + w, y + h);
    return pose;
}

D2D1_POINT_2F StageManager::StripOffset() const
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const float s = PopupScale(WorkArea());
    return D2D1::Point2F(float(rect_.left - mi.rcMonitor.left) / s, float(rect_.top - mi.rcMonitor.top) / s);
}

bool StageManager::StripPose(HWND window, Pose& pose)
{
    if (!shown_)
        return false;
    for (const Entry& e : entries_) {
        auto at = std::find(e.windows.begin(), e.windows.end(), window);
        if (at == e.windows.end())
            continue;
        auto parked = parked_.find(window);
        std::wstring key;
        auto p = PictureOf(window, parked != parked_.end() ? parked->second.key : e.keys.front(), key);
        if (!p || !p->ok)
            return false;
        Entry rest = e;
        rest.y = rest.targetY;
        pose = PoseOf(rest, size_t(at - e.windows.begin()), *p, (onRight_ ? -1.f : 1.f) * kTilt);
        const D2D1_POINT_2F o = StripOffset();
        pose.rect = D2D1::RectF(pose.rect.left + o.x, pose.rect.top + o.y, pose.rect.right + o.x, pose.rect.bottom + o.y);
        pose.cx += o.x, pose.ox += o.x, pose.cy += o.y, pose.oy += o.y;
        return true;
    }
    return false;
}

StageManager::Pose StageManager::PlacePose(const RECT& frame) const
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor_ ? monitor_ : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const float s = PopupScale(WorkArea());
    Pose pose;
    pose.rect = D2D1::RectF(float(frame.left - mi.rcMonitor.left) / s, float(frame.top - mi.rcMonitor.top) / s, float(frame.right - mi.rcMonitor.left) / s,
                            float(frame.bottom - mi.rcMonitor.top) / s);
    pose.cx = pose.ox = (pose.rect.left + pose.rect.right) / 2;
    pose.cy = pose.oy = (pose.rect.top + pose.rect.bottom) / 2;
    return pose;
}

bool StageManager::Flying(HWND window) const
{
    return std::any_of(flights_.begin(), flights_.end(), [&](const Flight& f) { return f.window == window; });
}

void StageManager::DrawPicture(Canvas& c, const WindowPicture& p, const std::wstring& key, const Pose& pose, float opacity)
{
    if (!p.ok || p.width <= 0 || p.height <= 0 || opacity < 0.004f)
        return;
    const float s = c.scale;
    const D2D1::Matrix4x4F m = Tilt(pose.cx * s, pose.cy * s, pose.ox * s, pose.oy * s, pose.angle, kDepth * s, pose.z * s);
    const D2D1_RECT_F& r = pose.rect;
    const float f = (r.right - r.left) / float(p.width), sp = float(kShadowPad) * f, pp = float(kPicturePad) * f;
    c.ImagePerspective(key + L"|s", p.shadow, D2D1::RectF(r.left - sp, r.top - sp, r.right + sp, r.bottom + sp),
                       m * D2D1::Matrix4x4F::Translation(0, kShadowDrop * s, 0), kShadowAlpha * opacity);
    c.ImagePerspective(key, p.image, D2D1::RectF(r.left - pp, r.top - pp, r.right + pp, r.bottom + pp), m, opacity);
}

void StageManager::PaintEntry(Canvas& c, Entry& e, int index, bool live)
{
    const float s = c.scale, dir = onRight_ ? -1.f : 1.f;
    const float hv = e.hover.Step(live && index == hover_, c.now);
    if (e.hover.moving)
        c.Continue();
    const float rv = Reveal(std::max(index, 0), c.now);
    const float opacity = e.appear * std::clamp(rv * 1.6f, 0.f, 1.f);
    // A card whose pictures are all on their way in (or out) shows once they are there.
    if (opacity < 0.004f || std::all_of(e.windows.begin(), e.windows.end(), [&](HWND h) { return Flying(h); }))
        return;
    const D2D1_RECT_F box = Box(e);
    const float cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
    const float k = (0.9f + 0.1f * e.appear) * (1 + 0.06f * hv) * (live && index == pressed_ ? 0.96f : 1.f);
    // Out of the screen's edge, turning to the eye as it comes.
    const float slide = onRight_ ? (size_.width - box.left + 24) * (1 - rv) : -(box.right + 24) * (1 - rv);
    const float angle = dir * (kTilt * (1 - kHoverTilt * hv) + kComeTilt * (1 - rv));
    c.BeginGroup(opacity, k, cx, cy, slide, 0);
    // Back to front: the windows behind the most recent one a step inwards, up and away.
    Pose front{};
    bool hasFront = false;
    for (size_t j = e.windows.size(); j-- > 0;) {
        HWND h = e.windows[j];
        if (Flying(h))
            continue;
        auto parked = parked_.find(h);
        std::wstring key;
        auto p = PictureOf(h, parked != parked_.end() ? parked->second.key : e.keys.front(), key);
        if (!p || !p->ok || p->width <= 0 || p->height <= 0)
            continue;
        const Pose pose = PoseOf(e, j, *p, angle);
        DrawPicture(c, *p, key, pose, 1);
        if (!hasFront || j == 0)
            front = pose, hasFront = true;
    }
    // The apps' icons in a column along the boxes' outer edge, level with the front window's lower
    // corner, overlapping it.
    if (hasFront) {
        const D2D1::Matrix4x4F m = Tilt(front.cx * s, front.cy * s, front.ox * s, front.oy * s, front.angle, kDepth * s, front.z * s);
        const float outer = onRight_ ? front.rect.right : front.rect.left;
        const float bottom = Project(m, outer * s, front.rect.bottom * s).y / s;
        const int iconPx = int(std::lround(kIcon * s));
        static const IconImage shadow = SoftShadow(96, 96);
        for (size_t j = std::min<size_t>(e.keys.size(), 3); j-- > 0;) {
            IconImage icon = tasks_.Icon(e.keys[j], iconPx);
            if (icon.empty())
                continue;
            const float ix = (onRight_ ? box.right : box.left) + dir * (10 + 24 * float(j)), iy = bottom - 6;
            const D2D1_RECT_F ir = D2D1::RectF(ix - kIcon / 2, iy - kIcon / 2, ix + kIcon / 2, iy + kIcon / 2);
            const float sp = kIcon * float(kShadowPad) / 96;
            c.Image(L"stage|shadow", shadow, D2D1::RectF(ir.left - sp, ir.top - sp + 2, ir.right + sp, ir.bottom + sp + 2), 0.45f);
            c.Image(L"stage|" + e.keys[j] + L"|" + std::to_wstring(iconPx), icon, ir);
        }
    }
    c.EndGroup();
}

// The pictures in flight, over everything: into the strip (the card shows them once they land) or
// back to their windows' places (the windows come up under them, then they fade).
void StageManager::PaintFlights(Canvas& c)
{
    const float dir = onRight_ ? -1.f : 1.f;
    for (auto it = flights_.begin(); it != flights_.end();) {
        Flight& f = *it;
        // Frame by frame, at most kFlightStepMs a frame: while windows go down or come up the screen
        // may not be composed for a moment; the picture waits rather than jumps.
        if (f.last > 0)
            f.elapsed += std::clamp(c.now - f.last, 0.0, kFlightStepMs);
        f.last = c.now;
        const float t = float(std::clamp(f.elapsed / kFlightMs, 0.0, 1.0)), e = EaseInOut(t);
        Pose a = f.toStrip ? f.place : f.from, b = f.toStrip ? Pose{} : f.place;
        float opacity = 1;
        if (f.toStrip && !StripPose(f.window, b)) {
            // The strip is not out: into the screen's edge.
            const D2D1_SIZE_F screen = D2D1::SizeF(float(flight_.Rect().right - flight_.Rect().left) / c.scale,
                                                   float(flight_.Rect().bottom - flight_.Rect().top) / c.scale);
            const float x = onRight_ ? screen.width + kBoxW / 2 : -kBoxW / 2, y = StripOffset().y + size_.height / 2;
            b.rect = D2D1::RectF(x - kBoxW / 2, y - kBoxH / 2, x + kBoxW / 2, y + kBoxH / 2);
            b.cx = b.ox = x;
            b.cy = b.oy = y;
            b.angle = dir * kTilt;
            opacity = 1 - e * e;
        }
        if (!f.toStrip && t >= 1 && !f.restored) {
            f.restored = true;
            HWND h = f.window;
            if (f.activate)
                std::thread([h] { TaskModel::ActivateWindow(h); }).detach();
            else
                ShowQuietly(h);
        }
        const double over = f.elapsed - kFlightMs - kFlightHoldMs;
        if (!f.toStrip && over > 0)
            opacity = 1 - float(std::clamp(over / kFlightFadeMs, 0.0, 1.0));
        Pose p;
        p.rect = D2D1::RectF(Lerp(a.rect.left, b.rect.left, e), Lerp(a.rect.top, b.rect.top, e), Lerp(a.rect.right, b.rect.right, e),
                             Lerp(a.rect.bottom, b.rect.bottom, e));
        p.cx = Lerp(a.cx, b.cx, e), p.cy = Lerp(a.cy, b.cy, e), p.ox = Lerp(a.ox, b.ox, e), p.oy = Lerp(a.oy, b.oy, e);
        p.angle = Lerp(a.angle, b.angle, e), p.z = Lerp(a.z, b.z, e);
        DrawPicture(c, *f.picture, f.key, p, opacity);
        const bool done = f.toStrip ? t >= 1 : over >= kFlightFadeMs;
        if (done) {
            if (!f.toStrip)
                Transitions(f.window, true); // up again: its own animations back
            it = flights_.erase(it);
            surface_.Invalidate(); // the card shows its picture now
        } else {
            ++it;
        }
    }
    if (!flights_.empty()) {
        c.Continue();
    } else {
        // Clicks go through until the next flight; the layer goes if none comes for a while.
        HRGN none = CreateRectRgn(0, 0, 0, 0);
        if (!SetWindowRgn(flight_.Hwnd(), none, FALSE))
            DeleteObject(none);
        SetTimer(flight_.Hwnd(), kFlightTimer, kFlightIdleMs, nullptr);
    }
}

void StageManager::Paint(Canvas& c)
{
    const bool moving = Animate(c.now);
    for (Entry& e : leaving_)
        PaintEntry(c, e, -1, false);
    for (size_t i = 0; i < entries_.size(); ++i)
        PaintEntry(c, entries_[i], int(i), true);
    UpdateRegion(moving);
    if (moving)
        c.Continue();
    else if (!shown_ || (entries_.empty() && leaving_.empty()))
        SetTimer(surface_.Hwnd(), kHideTimer, 1, nullptr); // back in the edge, or the last card gone: not from inside a paint
}

int StageManager::EntryAt(POINT pt) const
{
    float s = surface_.Scale(), x = float(pt.x) / s, y = float(pt.y) / s;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const D2D1_RECT_F r = Box(entries_[i]);
        if (x >= r.left - 14 && x < r.right + 10 && y >= r.top - 10 && y < r.bottom + 24)
            return int(i);
    }
    return -1;
}

bool StageManager::Message(UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    (void)result;
    POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
    case WM_STAGE_PICTURE:
        PictureTaken();
        return true;
    case WM_TIMER:
        if (w == kWatchTimer) {
            Watch();
            return true;
        }
        if (w == kDownTimer) {
            KillTimer(surface_.Hwnd(), kDownTimer);
            std::vector<HWND> late;
            for (const auto& [h, p] : parked_)
                if (!p.down && GetTickCount64() - p.since >= kDownMs - 20)
                    late.push_back(h);
            for (HWND h : late)
                PutDown(h, false); // no picture in time: it waits without one
            if (std::any_of(parked_.begin(), parked_.end(), [](const auto& kv) { return !kv.second.down; }))
                SetTimer(surface_.Hwnd(), kDownTimer, kDownMs / 4, nullptr);
            return true;
        }
        if (w == kHideTimer) {
            KillTimer(surface_.Hwnd(), kHideTimer);
            if (shown_ && entries_.empty() && leaving_.empty())
                Hide(); // slides away, then comes back here
            else if (!shown_)
                surface_.Hide(false);
            return true;
        }
        if (w == kRaiseTimer) {
            KillTimer(surface_.Hwnd(), kRaiseTimer);
            if (raise_ && IsWindow(raise_) && IsWindowVisible(raise_) && !IsIconic(raise_))
                SetWindowPos(raise_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            raise_ = nullptr;
            return true;
        }
        return false;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        if (!inside_)
            Foresee();
        inside_ = true;
        int i = shown_ ? EntryAt(pt) : -1;
        if (i != hover_) {
            hover_ = i;
            surface_.Invalidate();
        }
        return true;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        inside_ = false;
        hover_ = pressed_ = -1;
        surface_.Invalidate();
        return true;
    case WM_LBUTTONDOWN:
        Foresee();
        pressed_ = shown_ ? EntryAt(pt) : -1;
        surface_.Invalidate();
        return true;
    case WM_LBUTTONUP: {
        int i = EntryAt(pt);
        bool same = shown_ && i >= 0 && i == pressed_;
        pressed_ = -1;
        surface_.Invalidate();
        if (same) {
            Set keys = entries_[size_t(i)].keys;
            Log(L"stage: %s picked in the strip%s", keys.front().c_str(), (w & MK_SHIFT) ? L" (added)" : L"");
            Pick(keys, (w & MK_SHIFT) != 0);
        }
        return true;
    }
    }
    return false;
}

} // namespace atrium
