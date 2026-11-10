#pragma once

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace atrium {

namespace game_window {

inline std::wstring Lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); });
    return value;
}

inline std::wstring ImagePath(HWND window)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    std::wstring path(32768, L'\0');
    DWORD length = DWORD(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &length))
        length = 0;
    CloseHandle(process);
    path.resize(length);
    return path;
}

inline std::vector<std::wstring> ConfiguredGames()
{
    wchar_t module[32768]{};
    DWORD length = GetModuleFileNameW(nullptr, module, DWORD(_countof(module)));
    std::wstring path(module, length);
    const size_t slash = path.find_last_of(L"\\/");
    path.resize(slash == std::wstring::npos ? 0 : slash + 1);
    path += L"games.txt";

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return {};
    }
    std::string bytes(size_t(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr))
        read = 0;
    CloseHandle(file);
    bytes.resize(read);
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
        bytes.erase(0, 3);
    int chars = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    std::wstring text(chars, L'\0');
    if (chars)
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), text.data(), chars);

    std::vector<std::wstring> games;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t end = text.find_first_of(L"\r\n", at);
        std::wstring line = text.substr(at, end == std::wstring::npos ? text.size() - at : end - at);
        const size_t comment = line.find(L'#');
        if (comment != std::wstring::npos)
            line.resize(comment);
        const size_t first = line.find_first_not_of(L" \t");
        const size_t last = line.find_last_not_of(L" \t");
        if (first != std::wstring::npos)
            games.push_back(Lower(line.substr(first, last - first + 1)));
        if (end == std::wstring::npos)
            break;
        at = text.find_first_not_of(L"\r\n", end);
        if (at == std::wstring::npos)
            break;
    }
    return games;
}

inline bool WindowsRegisteredGame(const std::wstring& image)
{
    HKEY children = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"System\\GameConfigStore\\Children", 0, KEY_READ, &children) != ERROR_SUCCESS)
        return false;
    bool match = false;
    for (DWORD index = 0; !match; ++index) {
        wchar_t name[256]{};
        DWORD nameLength = DWORD(_countof(name));
        if (RegEnumKeyExW(children, index, name, &nameLength, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        HKEY child = nullptr;
        if (RegOpenKeyExW(children, name, 0, KEY_READ, &child) != ERROR_SUCCESS)
            continue;
        DWORD bytes = 0;
        if (RegGetValueW(child, nullptr, L"MatchedExeFullPath", RRF_RT_REG_SZ, nullptr, nullptr, &bytes) == ERROR_SUCCESS && bytes > sizeof(wchar_t)) {
            std::wstring registered(bytes / sizeof(wchar_t), L'\0');
            if (RegGetValueW(child, nullptr, L"MatchedExeFullPath", RRF_RT_REG_SZ, nullptr, registered.data(), &bytes) == ERROR_SUCCESS) {
                while (!registered.empty() && registered.back() == L'\0')
                    registered.pop_back();
                match = _wcsicmp(registered.c_str(), image.c_str()) == 0;
            }
        }
        RegCloseKey(child);
    }
    RegCloseKey(children);
    return match;
}

inline bool IsGameProcess(HWND window)
{
    const std::wstring image = ImagePath(window);
    if (image.empty())
        return false;
    const std::wstring key = Lower(image);
    static std::unordered_map<std::wstring, bool> cache;
    if (const auto found = cache.find(key); found != cache.end())
        return found->second;
    const size_t slash = key.find_last_of(L"\\/");
    const std::wstring file = slash == std::wstring::npos ? key : key.substr(slash + 1);
    static const std::vector<std::wstring> configured = ConfiguredGames();
    const bool listed = std::any_of(configured.begin(), configured.end(), [&](const std::wstring& entry) {
        return entry.find_first_of(L"\\/") == std::wstring::npos ? entry == file : entry == key;
    });
    return cache.emplace(key, listed || WindowsRegisteredGame(image)).first->second;
}

} // namespace game_window

// A strict full-screen test followed by an explicit/Windows game identity test.
// Maximized work-area windows and full-screen browsers therefore remain normal
// edge-reveal clients.
inline bool IsFullscreenGame(HWND window, HMONITOR monitor)
{
    if (!window || !monitor)
        return false;
    window = GetAncestor(window, GA_ROOT);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid || pid == GetCurrentProcessId() || IsIconic(window) || !IsWindowVisible(window) ||
        MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) != monitor)
        return false;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return false;
    MONITORINFO info{sizeof(info)};
    RECT bounds{};
    if (!GetMonitorInfoW(monitor, &info))
        return false;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) && !GetWindowRect(window, &bounds))
        return false;
    constexpr LONG tolerance = 2;
    const bool fills = bounds.left <= info.rcMonitor.left + tolerance && bounds.top <= info.rcMonitor.top + tolerance &&
                       bounds.right >= info.rcMonitor.right - tolerance && bounds.bottom >= info.rcMonitor.bottom - tolerance;
    return fills && game_window::IsGameProcess(window);
}

} // namespace atrium
