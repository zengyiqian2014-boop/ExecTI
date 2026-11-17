#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace command_history {

inline constexpr size_t kMaxHistory = 20;
inline constexpr const wchar_t* kPresets[] = {
    L"cmd.exe", L"powershell.exe", L"regedit.exe", L"taskmgr.exe",
    L"explorer.exe", L"notepad.exe", L"control.exe",
    L"mmc.exe services.msc", L"mmc.exe compmgmt.msc", L"mmc.exe eventvwr.msc"
};

inline bool SameCommand(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}

inline void AppendUnique(std::vector<std::wstring>& items, const std::wstring& entry) {
    if (!entry.empty() && std::none_of(items.begin(), items.end(),
            [&](const std::wstring& item) { return SameCommand(item, entry); }))
        items.push_back(entry);
}

// Only commands the user actually ran belong in persisted history.
inline std::vector<std::wstring> BuildHistory(
        const std::vector<std::wstring>& history, const std::wstring& newest = L"") {
    std::vector<std::wstring> out;
    AppendUnique(out, newest);
    for (const auto& entry : history) {
        if (out.size() == kMaxHistory) break;
        AppendUnique(out, entry);
    }
    return out;
}

// Presets are always available, including on a new account, without seeding MRU.
inline std::vector<std::wstring> BuildSuggestions(const std::vector<std::wstring>& history) {
    auto out = BuildHistory(history);
    for (const auto* preset : kPresets) AppendUnique(out, preset);
    return out;
}

} // namespace command_history
