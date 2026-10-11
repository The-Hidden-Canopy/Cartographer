#include <carto/physical/condition.hpp>

#include <iomanip>
#include <sstream>
#include <utility>

namespace carto::physical {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_PHYSICAL_SURFACE_CONDITION_STATE";

} // namespace

core::Result<void> SurfaceConditionState::validate() const {
    if (!domain || observed_revision.exhausted() ||
        !detail::safe_text(game_namespace, kMaxGameNamespaceBytes) ||
        game_record == 0U) {
        return core::Result<void>::failure(detail::invalid(
            "surface condition state identity, revision, owner, or record is invalid"));
    }
    if (!detail::finite_unit(wear) || !detail::finite_unit(moisture) ||
        !detail::finite_nonnegative(damage_depth) ||
        !detail::finite_nonnegative(accumulated_exposure)) {
        return core::Result<void>::failure(detail::validation(
            "surface condition state wear, damage, moisture, or exposure is invalid"));
    }
    return core::Result<void>::success();
}

std::string SurfaceConditionState::serialize() const {
    std::ostringstream output;
    detail::encode_condition(output, *this);
    return output.str();
}

core::Result<SurfaceConditionState> SurfaceConditionState::deserialize(
    std::string_view text) {
    if (auto result = detail::check_text_size(text, kMaxSerializedBytes, "surface condition state");
        !result) {
        return core::Result<SurfaceConditionState>::failure(result.error());
    }
    std::istringstream input{std::string(text)};
    SurfaceConditionState state;
    if (auto result = detail::decode_condition(input, state); !result) {
        return core::Result<SurfaceConditionState>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<SurfaceConditionState>::failure(detail::parse_error(
            "surface condition state contains trailing unbound data"));
    }
    if (auto result = state.validate(); !result) {
        return core::Result<SurfaceConditionState>::failure(result.error());
    }
    return core::Result<SurfaceConditionState>::success(std::move(state));
}

namespace detail {

void encode_condition(std::ostream& output, const SurfaceConditionState& state) {
    output << kMagic << ' ' << SurfaceConditionState::kSchemaVersion << '\n';
    output << "DOMAIN " << state.domain.value << ' ';
    write_revision(output, state.observed_revision) << '\n';
    output << "GAME " << std::quoted(state.game_namespace) << ' ' << state.game_record
           << '\n';
    output << std::setprecision(17);
    output << "CONDITION " << state.wear << ' ' << state.damage_depth << ' '
           << state.moisture << ' ' << state.accumulated_exposure << '\n';
    output << "END\n";
}

core::Result<void> decode_condition(std::istream& input, SurfaceConditionState& state) {
    if (auto result = read_frame(input, kMagic, SurfaceConditionState::kSchemaVersion);
        !result) {
        return result;
    }
    std::uint64_t domain = 0U;
    if (!require_token(input, "DOMAIN") || !read_integer(input, domain) ||
        !read_revision(input, state.observed_revision)) {
        return core::Result<void>::failure(parse_error(
            "surface condition state identity record is malformed"));
    }
    state.domain = surface::SurfaceDomainId{domain};
    if (!require_token(input, "GAME") ||
        !(input >> std::quoted(state.game_namespace)) ||
        !read_integer(input, state.game_record)) {
        return core::Result<void>::failure(parse_error(
            "surface condition state game record is malformed"));
    }
    if (!require_token(input, "CONDITION") || !read_double(input, state.wear) ||
        !read_double(input, state.damage_depth) || !read_double(input, state.moisture) ||
        !read_double(input, state.accumulated_exposure)) {
        return core::Result<void>::failure(parse_error(
            "surface condition state values are malformed"));
    }
    return read_end(input, kMagic);
}

} // namespace detail

} // namespace carto::physical
