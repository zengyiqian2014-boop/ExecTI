#include "command_history.h"

#include <cassert>
#include <iostream>

using command_history::BuildHistory;
using command_history::BuildSuggestions;

int main() {
    const std::vector<std::wstring> defaults = {
        L"cmd.exe", L"powershell.exe", L"regedit.exe", L"taskmgr.exe",
        L"explorer.exe", L"notepad.exe", L"control.exe",
        L"mmc.exe services.msc", L"mmc.exe compmgmt.msc", L"mmc.exe eventvwr.msc"
    };
    assert(BuildSuggestions({}) == defaults);
    assert(BuildHistory({}).empty());

    const std::vector<std::wstring> history = {
        L"C:\\Tools\\custom.exe --test", L"REGEDIT.EXE", L"regedit.exe", L"", L"cmd.exe"
    };
    const auto original = history;
    const auto suggestions = BuildSuggestions(history);
    assert(history == original); // Displaying defaults must not mutate saved history.
    assert(suggestions.size() == defaults.size() + 1);
    assert(suggestions[0] == history[0]);
    assert(suggestions[1] == L"REGEDIT.EXE");
    assert(suggestions[2] == L"cmd.exe");
    assert(suggestions[3] == L"powershell.exe");
    assert(suggestions.back() == L"mmc.exe eventvwr.msc");

    const auto updated = BuildHistory(history, L"RegEdit.exe");
    assert((updated == std::vector<std::wstring>{
        L"RegEdit.exe", L"C:\\Tools\\custom.exe --test", L"cmd.exe"}));
    assert(BuildSuggestions(updated).size() == defaults.size() + 1);
    assert(BuildHistory({}, L"my-tool.exe").size() == 1); // Never persist unused defaults.

    std::vector<std::wstring> full;
    for (int i = 0; i < 25; ++i) full.push_back(L"tool" + std::to_wstring(i) + L".exe");
    const auto capped = BuildHistory(full, L"latest.exe");
    assert(capped.size() == 20);
    assert(capped.front() == L"latest.exe");
    assert(capped.back() == L"tool18.exe");
    const auto fullSuggestions = BuildSuggestions(full);
    assert(fullSuggestions.size() == 30); // The history cap must not hide presets.
    assert(fullSuggestions[19] == L"tool19.exe");
    assert(fullSuggestions[20] == L"cmd.exe");

    const std::wstring longCommand = L"custom.exe " + std::wstring(4096, L'x');
    assert(BuildSuggestions({longCommand}).front() == longCommand);
    std::cout << "command history tests passed\n";
}
