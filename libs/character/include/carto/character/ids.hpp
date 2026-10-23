#pragma once

#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::character {

// Strong nonzero identity in the libs/world and libs/kinematic idiom. Values
// are monotonically allocated within a project namespace; zero is the invalid
// sentinel and is never assigned. Runtime engine handles are never identities.
template <typename Tag>
struct Id {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const Id&) const noexcept = default;
};

struct CharacterTag;
struct ControlTag;
struct MorphTargetTag;
struct CorrectiveDriverTag;
struct RigLayerTag;
struct RigTag;
struct ConstraintTag;
struct MotionDocumentTag;
struct GarmentBindingTag;
struct GroomBindingTag;
struct IntakeSourceTag;

using CharacterId = Id<CharacterTag>;
using ControlId = Id<ControlTag>;
using MorphTargetId = Id<MorphTargetTag>;
using CorrectiveDriverId = Id<CorrectiveDriverTag>;
using RigLayerId = Id<RigLayerTag>;
using RigId = Id<RigTag>;
using ConstraintId = Id<ConstraintTag>;
using MotionDocumentId = Id<MotionDocumentTag>;
using GarmentBindingId = Id<GarmentBindingTag>;
using GroomBindingId = Id<GroomBindingTag>;
using IntakeSourceId = Id<IntakeSourceTag>;

// Stable classification of every character identity space. Tombstones and
// type-erased references key on this enumeration, not on C++ type identity.
enum class IdKind : std::uint8_t {
    character,
    control,
    morph_target,
    corrective_driver,
    rig_layer,
    rig,
    constraint,
    motion_document,
    garment_binding,
    groom_binding,
    intake_source,
};

[[nodiscard]] std::string_view id_kind_name(IdKind kind) noexcept;

template <typename Tag>
struct id_kind_of;

template <> struct id_kind_of<CharacterTag> { static constexpr IdKind value = IdKind::character; };
template <> struct id_kind_of<ControlTag> { static constexpr IdKind value = IdKind::control; };
template <> struct id_kind_of<MorphTargetTag> { static constexpr IdKind value = IdKind::morph_target; };
template <> struct id_kind_of<CorrectiveDriverTag> { static constexpr IdKind value = IdKind::corrective_driver; };
template <> struct id_kind_of<RigLayerTag> { static constexpr IdKind value = IdKind::rig_layer; };
template <> struct id_kind_of<RigTag> { static constexpr IdKind value = IdKind::rig; };
template <> struct id_kind_of<ConstraintTag> { static constexpr IdKind value = IdKind::constraint; };
template <> struct id_kind_of<MotionDocumentTag> { static constexpr IdKind value = IdKind::motion_document; };
template <> struct id_kind_of<GarmentBindingTag> { static constexpr IdKind value = IdKind::garment_binding; };
template <> struct id_kind_of<GroomBindingTag> { static constexpr IdKind value = IdKind::groom_binding; };
template <> struct id_kind_of<IntakeSourceTag> { static constexpr IdKind value = IdKind::intake_source; };

// Retained proof that an identity was allocated and then retired. Deleted IDs
// are never reused: the tombstone pins the retirement reason and the project
// revision at which the deletion committed.
struct Tombstone {
    IdKind kind = IdKind::character;
    std::uint64_t identity = 0U;
    std::string reason;
    core::Revision retired_at;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const Tombstone&) const noexcept = default;
};

// Bounded, canonically sorted set of retired identities. Lookup is by
// (kind, identity) so a numeric collision across identity spaces cannot
// resurrect a deleted ID.
class TombstoneSet final {
public:
    static constexpr std::size_t kMaxTombstones = 65'536U;
    static constexpr std::size_t kMaxReasonBytes = 256U;

    [[nodiscard]] core::Result<void> insert(Tombstone tombstone);

    [[nodiscard]] bool contains(IdKind kind, std::uint64_t identity) const noexcept;
    template <typename Tag>
    [[nodiscard]] bool contains(Id<Tag> id) const noexcept {
        return contains(id_kind_of<Tag>::value, id.value);
    }

    [[nodiscard]] const std::vector<Tombstone>& tombstones() const noexcept {
        return tombstones_;
    }
    [[nodiscard]] bool empty() const noexcept { return tombstones_.empty(); }
    [[nodiscard]] core::Result<void> validate() const;

private:
    std::vector<Tombstone> tombstones_;
};

// Monotonic per-namespace allocation with retirement tracking. Allocation
// fails closed at exhaustion instead of wrapping, and retiring an unknown or
// already-retired identity is a validation error, never a silent overwrite.
template <typename Tag>
class IdSequence final {
public:
    static constexpr std::uint64_t kMaxIdentity = 0xFFFFFFFFFFFFULL;

    [[nodiscard]] core::Result<Id<Tag>> allocate() {
        if (next_.value > kMaxIdentity) {
            return core::Result<Id<Tag>>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state, "character identity space is exhausted"));
        }
        const Id<Tag> issued = next_;
        next_ = Id<Tag>{next_.value + 1U};
        return core::Result<Id<Tag>>::success(issued);
    }

    [[nodiscard]] core::Result<void> retire(
        Id<Tag> id,
        std::string reason,
        core::Revision revision) {
        if (!id) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "cannot retire the invalid character identity"));
        }
        if (id.value >= next_.value) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "cannot retire an identity this sequence never allocated"));
        }
        return tombstones_.insert(Tombstone{
            id_kind_of<Tag>::value, id.value, std::move(reason), revision});
    }

    [[nodiscard]] bool retired(Id<Tag> id) const noexcept {
        return tombstones_.contains(id_kind_of<Tag>::value, id.value);
    }
    [[nodiscard]] const TombstoneSet& tombstones() const noexcept { return tombstones_; }

private:
    Id<Tag> next_{1U};
    TombstoneSet tombstones_;
};

// Renders the stable string identity used across interchange boundaries and
// inside prepared manifests ("character:7").
template <typename Tag>
[[nodiscard]] std::string id_string(Id<Tag> id) {
    return std::string(id_kind_name(id_kind_of<Tag>::value)) + ":" +
        std::to_string(id.value);
}

} // namespace carto::character
