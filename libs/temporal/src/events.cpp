#include <carto/temporal/events.hpp>

#include "text_format.hpp"

#include <iomanip>
#include <sstream>
#include <utility>

namespace carto::temporal {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_EVENT_BINDINGS";

} // namespace

core::Result<void> EventBindingTable::bind(
    EventId marker, std::string game_binding) {
    if (!marker) {
        return core::Result<void>::failure(
            detail::invalid("event binding marker identity is invalid"));
    }
    if (!detail::safe_text(game_binding, kMaxBindingNameBytes)) {
        return core::Result<void>::failure(
            detail::invalid("event binding name is invalid"));
    }
    if (bindings_.size() >= kMaxBindings) {
        return core::Result<void>::failure(detail::validation(
            "event binding table has reached its bounded capacity"));
    }
    if (!bindings_.emplace(marker, std::move(game_binding)).second) {
        return core::Result<void>::failure(detail::invalid(
            "event marker identity is duplicated in the binding table"));
    }
    return core::Result<void>::success();
}

bool EventBindingTable::contains(EventId marker) const noexcept {
    return bindings_.find(marker) != bindings_.end();
}

core::Result<std::string_view> EventBindingTable::resolve(EventId marker) const {
    const auto found = bindings_.find(marker);
    if (found == bindings_.end()) {
        return core::Result<std::string_view>::failure(
            core::Diagnostic(
                core::ErrorCode::not_found,
                "temporal event marker has no explicit game binding; "
                "implicit dispatch is forbidden")
                .with_context("marker_id=" + std::to_string(marker.value)));
    }
    return core::Result<std::string_view>::success(found->second);
}

core::Result<void> EventBindingTable::validate() const {
    if (bindings_.size() > kMaxBindings) {
        return core::Result<void>::failure(detail::validation(
            "event binding table exceeds its bounded capacity"));
    }
    for (const auto& [marker, name] : bindings_) {
        if (!marker || !detail::safe_text(name, kMaxBindingNameBytes)) {
            return core::Result<void>::failure(detail::invalid(
                "event binding table contains an invalid marker or name"));
        }
    }
    return core::Result<void>::success();
}

std::string EventBindingTable::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << "BINDINGS " << bindings_.size() << '\n';
    for (const auto& [marker, name] : bindings_) {
        output << "BINDING " << marker.value << ' ' << std::quoted(name) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> EventBindingTable::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<EventBindingTable> EventBindingTable::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<EventBindingTable>::failure(detail::parse_error(
            "event binding table text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kMagic) {
        return core::Result<EventBindingTable>::failure(detail::parse_error(
            "event binding table framing is invalid"));
    }
    if (!detail::read_integer(input, version)) {
        return core::Result<EventBindingTable>::failure(detail::parse_error(
            "event binding table schema version is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<EventBindingTable>::failure(detail::version_error(
            "event binding table schema version is not supported"));
    }

    EventBindingTable table;
    std::size_t binding_count = 0U;
    if (!detail::require_token(input, "BINDINGS") ||
        !detail::read_count(input, kMaxBindings, binding_count)) {
        return core::Result<EventBindingTable>::failure(detail::parse_error(
            "event binding table header is malformed"));
    }
    for (std::size_t index = 0U; index < binding_count; ++index) {
        EventId marker;
        std::string name;
        if (!detail::require_token(input, "BINDING") ||
            !detail::read_integer(input, marker.value) ||
            !(input >> std::quoted(name))) {
            return core::Result<EventBindingTable>::failure(detail::parse_error(
                "event binding record is malformed"));
        }
        if (auto result = table.bind(marker, std::move(name)); !result) {
            return core::Result<EventBindingTable>::failure(result.error());
        }
    }
    if (!detail::require_token(input, "END")) {
        return core::Result<EventBindingTable>::failure(detail::parse_error(
            "event binding table is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<EventBindingTable>::failure(detail::parse_error(
            "event binding table contains trailing unbound data"));
    }
    if (auto result = table.validate(); !result) {
        return core::Result<EventBindingTable>::failure(result.error());
    }
    return core::Result<EventBindingTable>::success(std::move(table));
}

core::Result<void> verify_channel_events(
    const TemporalChannel& channel, const EventBindingTable& table) {
    if (auto result = channel.validate(); !result) return result;
    if (auto result = table.validate(); !result) return result;
    for (const auto& marker : channel.event_markers) {
        const auto resolved = table.resolve(marker.id);
        if (!resolved) {
            return core::Result<void>::failure(
                resolved.error()
                    .with_context("channel=" + channel.channel_name)
                    .with_context("marker=" + marker.name));
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::temporal
