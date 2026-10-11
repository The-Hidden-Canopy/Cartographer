#pragma once

#include <carto/temporal/channel.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace carto::temporal {

// Explicit marker-id -> game-binding-name table. Dispatch is never implicit:
// an unbound marker refuses with an explicit not-found diagnostic. Binding
// names are game-owned domain strings; Cartographer stores and validates
// them but does not interpret them.
class EventBindingTable final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 1U * 1024U * 1024U;
    static constexpr std::size_t kMaxBindings = 4'096U;
    static constexpr std::size_t kMaxBindingNameBytes = 256U;

    [[nodiscard]] core::Result<void> bind(EventId marker, std::string game_binding);
    [[nodiscard]] bool contains(EventId marker) const noexcept;
    [[nodiscard]] core::Result<std::string_view> resolve(EventId marker) const;
    [[nodiscard]] const std::map<EventId, std::string>& bindings() const noexcept {
        return bindings_;
    }

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<EventBindingTable> deserialize(
        std::string_view text);

private:
    std::map<EventId, std::string> bindings_;
};

// Every event marker declared by the channel must carry an explicit binding.
// The first unbound marker fails closed with a diagnostic naming it; no
// implicit or fallback dispatch path exists.
[[nodiscard]] core::Result<void> verify_channel_events(
    const TemporalChannel& channel, const EventBindingTable& table);

} // namespace carto::temporal
