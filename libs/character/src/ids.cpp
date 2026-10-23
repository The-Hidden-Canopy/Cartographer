#include <carto/character/ids.hpp>

#include <algorithm>
#include <utility>

namespace carto::character {
namespace {

constexpr std::size_t kMaxReasonBytes = TombstoneSet::kMaxReasonBytes;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_reason(std::string_view value) {
    if (value.empty() || value.size() > kMaxReasonBytes) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU;
    });
}

} // namespace

std::string_view id_kind_name(IdKind kind) noexcept {
    switch (kind) {
    case IdKind::character: return "character";
    case IdKind::control: return "control";
    case IdKind::morph_target: return "morph-target";
    case IdKind::corrective_driver: return "corrective-driver";
    case IdKind::rig_layer: return "rig-layer";
    case IdKind::rig: return "rig";
    case IdKind::constraint: return "constraint";
    case IdKind::motion_document: return "motion-document";
    case IdKind::garment_binding: return "garment-binding";
    case IdKind::groom_binding: return "groom-binding";
    case IdKind::intake_source: return "intake-source";
    }
    return "unknown";
}

core::Result<void> Tombstone::validate() const {
    if (kind > IdKind::intake_source || identity == 0U || !safe_reason(reason) ||
        retired_at.value() == 0U) {
        return core::Result<void>::failure(invalid(
            "character tombstone kind, identity, reason, or revision is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> TombstoneSet::insert(Tombstone tombstone) {
    if (auto result = tombstone.validate(); !result) return result;
    if (tombstones_.size() >= kMaxTombstones) {
        return core::Result<void>::failure(validation(
            "character tombstone set has reached its bounded capacity"));
    }
    const auto ordering = [](const Tombstone& left, const Tombstone& right) {
        return std::tie(left.kind, left.identity) <
            std::tie(right.kind, right.identity);
    };
    const auto position = std::lower_bound(
        tombstones_.begin(), tombstones_.end(), tombstone, ordering);
    if (position != tombstones_.end() &&
        position->kind == tombstone.kind && position->identity == tombstone.identity) {
        return core::Result<void>::failure(validation(
            "character identity already carries a tombstone"));
    }
    tombstones_.insert(position, std::move(tombstone));
    return core::Result<void>::success();
}

bool TombstoneSet::contains(IdKind kind, std::uint64_t identity) const noexcept {
    const auto found = std::lower_bound(
        tombstones_.begin(), tombstones_.end(), identity,
        [kind](const Tombstone& tombstone, std::uint64_t value) {
            return std::tie(tombstone.kind, tombstone.identity) <
                std::tie(kind, value);
        });
    return found != tombstones_.end() && found->kind == kind &&
        found->identity == identity;
}

core::Result<void> TombstoneSet::validate() const {
    if (tombstones_.size() > kMaxTombstones) {
        return core::Result<void>::failure(validation(
            "character tombstone set exceeds its bounded capacity"));
    }
    for (std::size_t index = 0U; index < tombstones_.size(); ++index) {
        if (auto result = tombstones_[index].validate(); !result) return result;
        if (index > 0U) {
            const auto& previous = tombstones_[index - 1U];
            const auto& current = tombstones_[index];
            if (std::tie(previous.kind, previous.identity) >=
                std::tie(current.kind, current.identity)) {
                return core::Result<void>::failure(validation(
                    "character tombstones must be sorted and unique by identity"));
            }
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::character
