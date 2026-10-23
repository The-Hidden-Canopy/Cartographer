#include <carto/botanical/source.hpp>

#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

namespace carto::botanical {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}
core::Diagnostic malformed(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}
bool valid_identity(std::string_view value) noexcept {
    if (value.empty() || value.size() > Source::kMaxIdentityBytes ||
        value.front() == '.' || value.front() == '-' || value.front() == '_') return false;
    for (const char c : value) {
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!alpha && !(c >= '0' && c <= '9') && c != '.' && c != '_' && c != '-') return false;
    }
    return true;
}
bool read_unsigned(std::istream& input, std::uint64_t& value) {
    std::string token;
    if (!(input >> token) || token.empty()) return false;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
}
bool read_double(std::istream& input, double& value) {
    std::string token;
    if (!(input >> token) || token.empty()) return false;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value,
        std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size() &&
        std::isfinite(value);
}
bool in_range(double value, double minimum, double maximum) noexcept {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

} // namespace

core::Result<void> Recipe::validate() const {
    if (static_cast<unsigned>(habit) > static_cast<unsigned>(Habit::grass) ||
        static_cast<unsigned>(leaf_form) > static_cast<unsigned>(LeafForm::scale)) {
        return core::Result<void>::failure(invalid("unsupported botanical source discriminator"));
    }
    const double growth_length = growth_axis.length();
    const double reference_length = reference_axis.length();
    if (!growth_axis.finite() || !reference_axis.finite() ||
        !std::isfinite(growth_length) || !std::isfinite(reference_length) ||
        growth_length <= 1.0e-12 || reference_length <= 1.0e-12 ||
        std::abs(core::dot(growth_axis.normalized(), reference_axis.normalized())) >= 1.0 - 1.0e-8) {
        return core::Result<void>::failure(malformed(
            "botanical source axes must define a finite nonparallel frame"));
    }
    if (!in_range(height, 0.05, 10'000.0) || !in_range(crown_radius, 0.005, 10'000.0) ||
        !in_range(stem_radius, 0.0001, 1'000.0) || !in_range(crown_base, 0.0, 1.0) ||
        !in_range(crown_tip, 0.0, 1.0) || crown_base >= crown_tip ||
        !in_range(branch_pitch, -1.0, 4.0) || !in_range(branch_length, 0.001, 4.0) ||
        !in_range(branch_taper, 0.01, 1.0) || !in_range(leaf_length, 0.0001, 1'000.0) ||
        !in_range(leaf_width, 0.0001, 1'000.0)) {
        return core::Result<void>::failure(malformed(
            "botanical source dimensions exceed the authoring bounds"));
    }
    if (habit == Habit::grass) {
        if (stem_count == 0U || stem_count > 512U || branch_levels != 0U ||
            branches_per_level != 0U || secondary_branches != 0U ||
            leaves_per_branch == 0U || leaves_per_branch > 4U) {
            return core::Result<void>::failure(malformed(
                "grass source counts exceed the authoring bounds"));
        }
    } else if (stem_count == 0U || stem_count > 32U || branch_levels == 0U ||
        branch_levels > 24U || branches_per_level == 0U || branches_per_level > 32U ||
        secondary_branches > 8U || leaves_per_branch == 0U || leaves_per_branch > 16U) {
        return core::Result<void>::failure(malformed(
            "tree or shrub source counts exceed the authoring bounds"));
    }
    return core::Result<void>::success();
}

core::Result<void> Source::validate() const {
    if (!valid_identity(identity) || revision.value() == 0U) {
        return core::Result<void>::failure(invalid("botanical source identity or revision is invalid"));
    }
    return recipe.validate();
}

std::string Source::serialize() const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    out << "CARTOGRAPHER_PLANT_SOURCE " << kSchemaVersion << '\n'
        << "IDENTITY " << std::quoted(identity) << ' ' << revision.value() << '\n'
        << "RECIPE " << static_cast<unsigned>(recipe.habit) << ' '
        << static_cast<unsigned>(recipe.leaf_form) << ' ' << recipe.seed << '\n'
        << "FRAME " << recipe.growth_axis.x << ' ' << recipe.growth_axis.y << ' '
        << recipe.growth_axis.z << ' ' << recipe.reference_axis.x << ' '
        << recipe.reference_axis.y << ' ' << recipe.reference_axis.z << '\n'
        << "SIZE " << recipe.height << ' ' << recipe.crown_radius << ' '
        << recipe.stem_radius << ' ' << recipe.stem_count << '\n'
        << "BRANCHING " << recipe.branch_levels << ' ' << recipe.branches_per_level << ' '
        << recipe.secondary_branches << ' ' << recipe.crown_base << ' ' << recipe.crown_tip << ' '
        << recipe.branch_pitch << ' ' << recipe.branch_length << ' ' << recipe.branch_taper << '\n'
        << "FOLIAGE " << recipe.leaves_per_branch << ' ' << recipe.leaf_length << ' '
        << recipe.leaf_width << '\n';
    return out.str();
}

core::Result<Source> Source::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<Source>::failure(malformed("botanical source size is invalid"));
    }
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    std::string marker, field;
    std::uint64_t version = 0U, revision = 0U, habit_value = 0U, leaf_value = 0U;
    if (!(in >> marker) || marker != "CARTOGRAPHER_PLANT_SOURCE" ||
        !read_unsigned(in, version) || version != kSchemaVersion) {
        return core::Result<Source>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch, "botanical source schema is unsupported"));
    }
    Source source;
    if (!(in >> field) || field != "IDENTITY" || !(in >> std::quoted(source.identity)) ||
        !read_unsigned(in, revision) || !(in >> field) || field != "RECIPE" ||
        !read_unsigned(in, habit_value) || !read_unsigned(in, leaf_value) ||
        !read_unsigned(in, source.recipe.seed) || habit_value > 255U || leaf_value > 255U) {
        return core::Result<Source>::failure(malformed("botanical source header is malformed"));
    }
    source.revision = core::Revision{revision};
    source.recipe.habit = static_cast<Habit>(habit_value);
    source.recipe.leaf_form = static_cast<LeafForm>(leaf_value);
    std::uint64_t stem_count = 0U, levels = 0U, per_level = 0U, secondary = 0U, leaves = 0U;
    if (!(in >> field) || field != "FRAME" || !read_double(in, source.recipe.growth_axis.x) ||
        !read_double(in, source.recipe.growth_axis.y) || !read_double(in, source.recipe.growth_axis.z) ||
        !read_double(in, source.recipe.reference_axis.x) || !read_double(in, source.recipe.reference_axis.y) ||
        !read_double(in, source.recipe.reference_axis.z) || !(in >> field) || field != "SIZE" ||
        !read_double(in, source.recipe.height) || !read_double(in, source.recipe.crown_radius) ||
        !read_double(in, source.recipe.stem_radius) || !read_unsigned(in, stem_count) ||
        !(in >> field) || field != "BRANCHING" || !read_unsigned(in, levels) ||
        !read_unsigned(in, per_level) || !read_unsigned(in, secondary) ||
        !read_double(in, source.recipe.crown_base) || !read_double(in, source.recipe.crown_tip) ||
        !read_double(in, source.recipe.branch_pitch) || !read_double(in, source.recipe.branch_length) ||
        !read_double(in, source.recipe.branch_taper) || !(in >> field) || field != "FOLIAGE" ||
        !read_unsigned(in, leaves) || !read_double(in, source.recipe.leaf_length) ||
        !read_double(in, source.recipe.leaf_width) ||
        stem_count > std::numeric_limits<std::uint32_t>::max() ||
        levels > std::numeric_limits<std::uint32_t>::max() ||
        per_level > std::numeric_limits<std::uint32_t>::max() ||
        secondary > std::numeric_limits<std::uint32_t>::max() ||
        leaves > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<Source>::failure(malformed("botanical source fields are malformed"));
    }
    source.recipe.stem_count = static_cast<std::uint32_t>(stem_count);
    source.recipe.branch_levels = static_cast<std::uint32_t>(levels);
    source.recipe.branches_per_level = static_cast<std::uint32_t>(per_level);
    source.recipe.secondary_branches = static_cast<std::uint32_t>(secondary);
    source.recipe.leaves_per_branch = static_cast<std::uint32_t>(leaves);
    if (in >> field) {
        return core::Result<Source>::failure(malformed("botanical source contains trailing data"));
    }
    if (auto valid = source.validate(); !valid) return core::Result<Source>::failure(valid.error());
    return core::Result<Source>::success(std::move(source));
}

} // namespace carto::botanical
