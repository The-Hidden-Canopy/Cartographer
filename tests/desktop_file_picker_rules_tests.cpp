#include "../apps/desktop/studio_file_picker_rules.hpp"

#include <array>
#include <iostream>
#include <string_view>

#ifdef _WIN32
#include "../apps/desktop/studio_file_picker_paths.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#endif

int main() {
    using carto::studio::file_picker_detail::invalid_save_filename;
    constexpr std::array<std::wstring_view, 13> invalid{
        L"", L"space ", L"trailing.", L"name?.carto", L"a/b", L"a\\b",
        L"NUL", L"con.txt", L"Lpt9.any", L"COM\u00b9.txt", L"con .txt",
        L"clock$", std::wstring_view(L"a\x01" L"b", 3)};
    constexpr std::array<std::wstring_view, 7> valid{
        L"project.carto", L".hidden", L"CONTRACT.carto", L"COM10", L"LPT0",
        L"room & detail.carto", L"\u6a39\u6728.carto"};
    for (const auto name : invalid) {
        if (!invalid_save_filename(name)) { std::cerr << "invalid name accepted\n"; return 1; }
    }
    for (const auto name : valid) {
        if (invalid_save_filename(name)) { std::cerr << "valid name rejected\n"; return 1; }
    }
#ifdef _WIN32
    namespace fs = std::filesystem;
    using carto::studio::file_picker_detail::plan_selection;
    using carto::studio::file_picker_detail::SelectionStatus;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / (L"carto-picker-" + std::to_wstring(stamp));
    if (!fs::create_directory(root)) { std::cerr << "fixture directory unavailable\n"; return 1; }
    const auto existing = root / L"existing.carto";
    { std::ofstream output(existing, std::ios::binary); output << "preserve"; }
    const auto folder = root / L"folder";
    fs::create_directory(folder);
    const auto extension_folder = root / L"folder-name.carto";
    fs::create_directory(extension_folder);
    const auto check = [&](bool condition) {
        if (!condition) std::cerr << "selection plan failed\n";
        return condition;
    };
    bool ok = true;
    using carto::studio::file_picker_detail::matches_filter;
    ok &= check(matches_filter(L"shape.OBJ", L"*.obj;*.ply;*.stl"));
    ok &= check(!matches_filter(L"image.png", L"*.obj;*.ply;*.stl"));
    ok &= check(matches_filter(L"README", L"*.*"));
    ok &= check(matches_filter(L"manifest.json", L"manifest.json"));
    const auto fresh = plan_selection(root, L"new scene", true, L"carto", true);
    ok &= check(fresh.status == SelectionStatus::selected && fresh.path == root / L"new scene.carto");
    ok &= check(plan_selection(root, L"existing", true, L"carto", true).status == SelectionStatus::confirm_replace);
    ok &= check(plan_selection(root, L"existing", true, L"carto", false).status == SelectionStatus::selected);
    ok &= check(plan_selection(root, L"existing.carto", false, L"carto", true).status == SelectionStatus::selected);
    ok &= check(plan_selection(root, L"missing.carto", false, L"carto", true).status == SelectionStatus::missing_file);
    ok &= check(plan_selection(root, L"missing/child", true, L"carto", true).status == SelectionStatus::missing_parent);
    ok &= check(plan_selection(root, L"folder", true, L"carto", true).status == SelectionStatus::enter_directory);
    ok &= check(plan_selection(root, L"folder-name", true, L"carto", true).status == SelectionStatus::invalid_name);
    ok &= check(plan_selection(root, L"con", true, L"carto", true).status == SelectionStatus::invalid_name);
    ok &= check(plan_selection(root, L"\\\\example\\share\\file", false, L"", true).status == SelectionStatus::use_system_browser);
    std::ifstream input(existing, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    ok &= check(contents == "preserve" && !fs::exists(root / L"new scene.carto"));
    input.close();
    fs::remove(existing);
    fs::remove(folder);
    fs::remove(extension_folder);
    fs::remove(root);
    if (!ok) return 1;
#endif
    return 0;
}
