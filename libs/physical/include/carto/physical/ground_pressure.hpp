#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/physical/mass.hpp>
#include <carto/physical/types.hpp>

#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

namespace carto::physical {

// Standard gravity used to convert derived mass into ground pressure. Fixed
// constant: pressure evidence is deterministic across machines.
inline constexpr double kStandardGravityMetersPerSecond2 = 9.80665;

// Ground pressure evidence for Blood & Soil-style domain consumers: a support
// contact patch extracted from authored geometry, loaded with the derived mass
// of the twin. The distribution model is explicit and digest-visible.
struct GroundPressureEvidence {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 1U * 1024U * 1024U;

    // Exact mass receipt this evidence binds; recompute to detect drift.
    assets::Sha256Digest mass_receipt_digest;
    core::Revision source_revision;
    // Authored support footprint: the rectangle of ground the asset rests on.
    core::Bounds3d contact_patch{};
    double contact_patch_area = 0.0;
    double supported_mass_kg = 0.0;
    double weight_newtons = 0.0;
    // Uniform distribution over the patch under the recorded model.
    double mean_pressure_pa = 0.0;
    // Where the weight acts: center of mass projected onto the patch plane.
    core::Vec3d center_of_pressure{};
    std::string pressure_model = "uniform-contact-patch/1";
    std::string algorithm_version = "cartographer-ground-pressure/0.1";

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<GroundPressureEvidence> deserialize(
        std::string_view text);
};

// Derives ground pressure evidence from a validated mass receipt and an
// authored contact patch. Fails closed when the patch is degenerate or the
// center of mass falls outside the patch footprint.
[[nodiscard]] core::Result<GroundPressureEvidence> derive_ground_pressure(
    const MassDerivationReceipt& mass,
    core::Bounds3d contact_patch);

namespace detail {
void encode_ground_pressure(
    std::ostream& output, const GroundPressureEvidence& evidence);
core::Result<void> decode_ground_pressure(
    std::istream& input, GroundPressureEvidence& evidence);
} // namespace detail

} // namespace carto::physical
