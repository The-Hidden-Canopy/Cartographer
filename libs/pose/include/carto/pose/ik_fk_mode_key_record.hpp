#pragma once

#include <carto/pose/ik_fk_mode_key_intent.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace carto::pose {

// Portable authored-intent data only. The matched pose is deliberately not
// serialized. Context, source/pose/document digests, clock correspondence and
// channel-control-axis crosswalk remain caller assertions, not authenticated
// facts, current project state, observed motion, or execution evidence.
struct IkFkModeKeyRecord {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes =
        2U * motion::KeyedCurve::kMaxSerializedBytes +
        2U * motion::CurveEditReceipt::kMaxSerializedBytes + 16U * 1024U;

    ModeKeyIntentContext context;
    ModeIntentCrosswalk crosswalk;
    character::MotionDocumentId document;
    temporal::ClipId clip;
    assets::Sha256Digest clip_digest;
    IkFkMode source_mode = IkFkMode::fk;
    IkFkMode target_mode = IkFkMode::fk;
    double mode_before = 0.0;
    double mode_at_key = 0.0;
    motion::KeyedCurve proposed_curve;
    motion::CurveEditReceipt edit;

    // Verifies canonical insertion relationship by deleting precisely the new
    // tick, checking its source digest and re-inserting it. It does not prove
    // that this reconstructed source is the current authoritative curve.
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<IkFkModeKeyRecord> deserialize(std::string_view);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Checks internal consistency only; the public proposal value is forgeable.
// A future owner must independently revalidate live references and CAS before
// admitting this record to a project or considering its caller assertions.
[[nodiscard]] core::Result<IkFkModeKeyRecord> make_ik_fk_mode_key_record(
    const IkFkModeKeyIntent& intent);

} // namespace carto::pose
