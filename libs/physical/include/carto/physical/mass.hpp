#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/physical/construction.hpp>
#include <carto/physical/geometry_evidence.hpp>
#include <carto/physical/types.hpp>

#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

namespace carto::physical {

// Deterministic, CPU-derived mass facts for one physical twin. The receipt
// binds the exact digests of the geometry evidence and construction profile
// it was derived from; any later drift in those inputs is detectable stale
// data. Games observe these facts rather than inventing their own.
struct MassDerivationReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 1U * 1024U * 1024U;

    assets::Sha256Digest geometry_evidence_digest;
    assets::Sha256Digest construction_profile_digest;
    std::string algorithm_version = "cartographer-mass-derivation/0.1";
    // The distribution model records the explicit approximation used for the
    // inertia tensor; changing it changes the digest.
    std::string distribution_model = "uniform-scaled-inertia/1";
    core::Revision source_revision;
    double total_mass_kg = 0.0;
    // Enclosed authoring volume the construction is realized inside. Material
    // volume for shell/filled/layered constructions differs and is recorded in
    // the construction, not here.
    double total_volume_m3 = 0.0;
    core::Vec3d center_of_mass{};
    // Inertia about the center of mass under the recorded distribution model.
    detail::SymmetricTensor3 inertia_kg_m2{};

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<MassDerivationReceipt> deserialize(
        std::string_view text);
};

// Derives mass facts from validated evidence + construction. Fails closed
// when the construction references material intents that do not declare an
// authored density: Cartographer never substitutes downstream consumer catalog constants it
// does not own.
[[nodiscard]] core::Result<MassDerivationReceipt> derive_mass(
    const GeometryEvidence& geometry,
    const ConstructionProfile& construction);

namespace detail {
void encode_mass(std::ostream& output, const MassDerivationReceipt& receipt);
core::Result<void> decode_mass(std::istream& input, MassDerivationReceipt& receipt);
} // namespace detail

} // namespace carto::physical
