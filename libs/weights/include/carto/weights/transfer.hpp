#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/character/ids.hpp>
#include <carto/character/weights_record.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/skin.hpp>
#include <carto/weights/paint.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace carto::weights {

// Weight transfer across topology (SKN-004). A finished weight layer is
// carried to a new mesh topology through an explicit correspondence: each
// target vertex names the source vertices it gathers from, with barycentric
// coordinates, and influences are blended, optionally remapped onto a
// different joint set, capped, renormalized lock-aware, and re-admitted
// through character::validate_influences. The transfer is a pure operation:
// it returns a new binding plus a digest-bound receipt while the caller keeps
// the source, it never invents coverage (a target vertex whose samples gather
// no source influence refuses the whole transfer, mirroring
// generate_auto_weights' unreachable-region refusal), and every step is
// deterministic so the receipt digest is reproducible evidence.

// Explicit source->target vertex correspondence. Content-addressable so a
// receipt can bind the exact correspondence a transferred layer came from.
class WeightCorrespondence final {
public:
    static constexpr std::size_t kMaxTargetVertices =
        character::SkinWeightLayer::kMaxInfluencedVertices;
    static constexpr std::size_t kMaxSamplesPerVertex =
        kinematic::SkinBinding::kMaxInfluencesPerVertex;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U * 1024U;

    struct Sample {
        // Canonical source-mesh vertex index the sample gathers from.
        std::uint32_t source_vertex = 0U;
        // Barycentric coordinate: finite and strictly positive; the samples
        // of one target vertex sum to exactly one.
        double coordinate = 0.0;
    };

    // Stores the mapping in canonical form: samples pruned of non-positive
    // coordinates, unique by source vertex, sorted ascending, coordinates
    // renormalized to an exact sum of one with the residual placed on the
    // largest coordinate (lowest source vertex on ties).
    [[nodiscard]] core::Result<void> map(
        std::uint32_t target_vertex, std::vector<Sample> samples);
    [[nodiscard]] const std::map<std::uint32_t, std::vector<Sample>>& entries()
        const noexcept {
        return entries_;
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<WeightCorrespondence> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;

private:
    std::map<std::uint32_t, std::vector<Sample>> entries_;
};

// Joint remapping for cross-skeleton transfer: directed pairs
// (source joint -> target joint). The map must be injective; two source
// joints claiming one target joint would double-count it during the
// barycentric gather, so it is refused at pair time, not discovered later.
// Joints absent from the map map onto themselves, so an empty map is the
// identity and expresses the same-skeleton/new-mesh transfer mode.
class JointTransferMap final {
public:
    static constexpr std::size_t kMaxEntries = 4'096U;

    [[nodiscard]] core::Result<void> pair(
        kinematic::JointId source, kinematic::JointId target);
    [[nodiscard]] kinematic::JointId mapped(kinematic::JointId source) const noexcept;
    [[nodiscard]] const std::map<kinematic::JointId, kinematic::JointId>& entries()
        const noexcept {
        return map_;
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;

private:
    std::map<kinematic::JointId, kinematic::JointId> map_;
};

struct WeightTransferOptions {
    // Layer identity the produced SkinWeightLayer is pinned to.
    character::RigLayerId output_layer_id;
    // Content identity of the target mesh the new binding is authored
    // against; both are mandatory and nonzero.
    assets::Sha256Digest output_mesh_digest;
    std::uint64_t output_mesh_asset = 0U;
    // Digest of the KinematicGraph the transferred joints live on. Zero means
    // the transfer keeps the source binding's graph digest (identity joint
    // map, same skeleton); a nonzero value is required to be consistent with
    // a non-identity joint map.
    assets::Sha256Digest output_graph_digest{};
    // Final maximum active influences per transferred vertex (1..8); gathered
    // influences beyond the cap are dropped weakest-first (lowest joint id on
    // ties) before renormalization.
    std::uint32_t max_influences_per_vertex =
        kinematic::SkinBinding::kMaxInfluencesPerVertex;

    [[nodiscard]] core::Result<void> validate() const;
};

// Digest-bound provenance for one transfer run. Contains no wall time:
// identity and determinism come from the correspondence, joint map and the
// input/output binding digests.
struct WeightTransferReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U;

    assets::Sha256Digest correspondence_digest;
    // Zero digest means the identity joint map.
    assets::Sha256Digest joint_map_digest;
    // Zero digest means "no locks".
    assets::Sha256Digest locks_digest;
    assets::Sha256Digest input_binding_digest;
    assets::Sha256Digest output_binding_digest;
    std::uint64_t transferred_vertex_count = 0U;
    std::uint64_t total_influence_count = 0U;
    // Samples whose source vertex carried no influences (rigid): they
    // contribute nothing to the gather and are counted so the receipt stays
    // honest about what the correspondence actually reached.
    std::uint64_t rigid_sample_count = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<WeightTransferReceipt> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

struct WeightTransferOutput {
    kinematic::SkinBinding binding;
    character::SkinWeightLayer layer;
    WeightTransferReceipt receipt;
};

// Transfers `binding_source` onto the topology described by `correspondence`,
// producing a complete binding and layer pinned to the target identities in
// `options`, plus a receipt. `layer_source` must be the layer record of
// `binding_source` (its digest pins are checked and a mismatch refuses).
// `joint_map` remaps joints for cross-skeleton transfer; pass an empty map
// for the same-skeleton/new-mesh mode. `locks` may be null. Fails closed,
// without producing partial output, when: any record or option is invalid,
// the layer pins do not match the source binding, a target vertex gathers no
// source influence at all, the influence cap would drop a locked weight, or
// any produced vertex fails character::validate_influences.
[[nodiscard]] core::Result<WeightTransferOutput> transfer_weights(
    const kinematic::SkinBinding& binding_source,
    const character::SkinWeightLayer& layer_source,
    const WeightCorrespondence& correspondence,
    const JointTransferMap& joint_map,
    const WeightTransferOptions& options,
    const WeightLocks* locks = nullptr);

} // namespace carto::weights
