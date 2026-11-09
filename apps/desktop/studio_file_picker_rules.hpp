#pragma once

#include <initializer_list>
#include <string_view>

namespace carto::studio::file_picker_detail {

[[nodiscard]] inline wchar_t ascii_upper(wchar_t value) noexcept {
    return value >= L'a' && value <= L'z' ? static_cast<wchar_t>(value - L'a' + L'A') : value;
}

[[nodiscard]] inline bool ascii_equal(std::wstring_view first, std::wstring_view second) noexcept {
    if (first.size() != second.size()) return false;
    for (std::size_t index = 0; index < first.size(); ++index)
        if (ascii_upper(first[index]) != ascii_upper(second[index])) return false;
    return true;
}

[[nodiscard]] inline bool invalid_save_filename(std::wstring_view name) noexcept {
    if (name.empty() || name.back() == L'.' || name.back() == L' ') return true;
    for (const wchar_t ch : name) {
        if (ch < 32 || std::wstring_view(L"<>:\"/\\|?*").find(ch) != std::wstring_view::npos)
            return true;
    }
    auto base = name.substr(0, name.find(L'.'));
    while (!base.empty() && base.back() == L' ') base.remove_suffix(1);
    for (const auto reserved : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$", L"CLOCK$"})
        if (ascii_equal(base, reserved)) return true;
    if (base.size() == 4 && ((base[3] >= L'1' && base[3] <= L'9') ||
        base[3] == L'\u00b9' || base[3] == L'\u00b2' || base[3] == L'\u00b3') &&
        (ascii_equal(base.substr(0, 3), L"COM") || ascii_equal(base.substr(0, 3), L"LPT")))
        return true;
    return false;
}

} // namespace carto::studio::file_picker_detail
