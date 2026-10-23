#include <carto/weights/transfer.hpp>

#include "weights_internal.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace carto::weights {
namespace {

constexpr std::string_view kCorrespondenceMagic =
    "CARTOGRAPHER_WEIGHTS_CORRESPONDENCE";
constexpr std::string_view kJointTransferMagic =
    "CARTOGRAPHER_WEIGHTS_JOINT_TRANSFER";
constexpr std::string_view kReceiptMagic = "CARTOGRAPHER_WEIGHTS_TRANSFER_RECEIPT";

} // namespace

// ---------------------------------------------------------------------------
// WeightCorrespondence

core::Result<void> WeightCorrespondence::map(
    std::uint32_t target_vertex,
    std::vector<Sample> samples) {
    if (target_vertex > kinematic::SkinBinding::kMaxVertexIndex) {
        return core::Result<void>::failure(detail::invalid(
            "correspondence target vertex exceeds the skin vertex index bound"));
    }
    if (samples.empty()) {
        return core::Result<void>::failure(detail::invalid(
            "a correspondence mapping requires at least one sample"));
    }
    if (samples.size() > kMaxSamplesPerVertex) {
        return core::Result<void>::failure(detail::invalid(
            "correspondence samples per target vertex exceed the bound"));
    }
    if (entries_.size() >= kMaxTargetVertices &&
        entries_.find(target_vertex) == entries_.end()) {
        return core::Result<void>::failure(detail::invalid(
            "correspondence exceeds its target vertex bound"));
    }
    double sum = 0.0;
    for (const Sample& sample : samples) {
        if (!std::isfinite(sample.coordinate) || sample.coordinate <= 0.0) {
            return core::Result<void>::failure(detail::invalid(
                "correspondence coordinates must be positive and finite"));
        }
        sum += sample.coordinate;
    }
    std::sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) {
        return a.source_vertex < b.source_vertex;
    });
    if (std::adjacent_find(samples.begin(), samples.end(),
            [](const Sample& a, const Sample& b) {
                return a.source_vertex == b.source_vertex;
            }) != samples.end()) {
        return core::Result<void>::failure(detail::invalid(
            "correspondence samples of one target vertex must be unique"));
    }
    if (!std::isfinite(sum) ||
        std::abs(sum - 1.0) > kinematic::SkinBinding::kWeightSumTolerance) {
        return core::Result<void>::failure(detail::invalid(
            "correspondence samples of one target vertex must sum to one"));
    }
    // Canonical normalization: residual rounding lands on the largest
    // coordinate (lowest source vertex on ties), so the stored form is
    // deterministic and re-serialization is stable.
    auto largest = std::max_element(samples.begin(), samples.end(),
        [](const Sample& a, const Sample& b) {
            if (a.coordinate != b.coordinate) return a.coordinate < b.coordinate;
            return b.source_vertex < a.source_vertex;
        });
    largest->coordinate += 1.0 - sum;
    entries_[target_vertex] = std::move(samples);
    return core::Result<void>::success();
}

core::Result<void> WeightCorrespondence::validate() const {
    if (entries_.size() > kMaxTargetVertices) {
        return core::Result<void>::failure(detail::invalid(
            "correspondence exceeds its target vertex bound"));
    }
    for (const auto& [target_vertex, samples] : entries_) {
        if (target_vertex > kinematic::SkinBinding::kMaxVertexIndex) {
            return core::Result<void>::failure(detail::invalid(
                "correspondence target vertex exceeds the skin vertex index bound"));
        }
        if (samples.empty() || samples.size() > kMaxSamplesPerVertex) {
            return core::Result<void>::failure(detail::validation(
                "correspondence mappings require 1..8 samples"));
        }
        if (!std::is_sorted(samples.begin(), samples.end(),
                [](const Sample& a, const Sample& b) {
                    return a.source_vertex < b.source_vertex;
                }) ||
            std::adjacent_find(samples.begin(), samples.end(),
                [](const Sample& a, const Sample& b) {
                    return a.source_vertex == b.source_vertex;
                }) != samples.end()) {
            return core::Result<void>::failure(detail::validation(
                "correspondence samples must be sorted and unique per target vertex"));
        }
        double sum = 0.0;
        for (const Sample& sample : samples) {
            if (!std::isfinite(sample.coordinate) || sample.coordinate <= 0.0) {
                return core::Result<void>::failure(detail::validation(
                    "correspondence coordinates must be positive and finite"));
            }
            sum += sample.coordinate;
        }
        if (std::abs(sum - 1.0) > kinematic::SkinBinding::kWeightSumTolerance) {
            return core::Result<void>::failure(detail::validation(
                "correspondence samples of one target vertex must sum to one"));
        }
    }
    return core::Result<void>::success();
}

std::string WeightCorrespondence::serialize() const {
    std::ostringstream output;
    output << kCorrespondenceMagic << " 1\n";
    output << std::setprecision(17);
    output << "VERTICES " << entries_.size() << '\n';
    for (const auto& [target_vertex, samples] : entries_) {
        output << "MAP " << target_vertex << ' ' << samples.size();
        for (const Sample& sample : samples) {
            output << ' ' << sample.source_vertex << ' ' << sample.coordinate;
        }
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<WeightCorrespondence> WeightCorrespondence::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<WeightCorrespondence>::failure(detail::parse_error(
            "weight correspondence text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kCorrespondenceMagic ||
        !detail::read_integer(input, version) || version != 1U) {
        return core::Result<WeightCorrespondence>::failure(detail::parse_error(
            "weight correspondence framing is invalid"));
    }
    std::size_t vertex_count = 0U;
    if (!detail::require_token(input, "VERTICES") ||
        !detail::read_count(input, kMaxTargetVertices, vertex_count)) {
        return core::Result<WeightCorrespondence>::failure(detail::parse_error(
            "weight correspondence vertex count is invalid"));
    }
    WeightCorrespondence correspondence;
    for (std::size_t index = 0U; index < vertex_count; ++index) {
        std::uint32_t target_vertex = 0U;
        std::size_t sample_count = 0U;
        if (!detail::require_token(input, "MAP") ||
            !detail::read_integer(input, target_vertex) ||
            !detail::read_count(input, kMaxSamplesPerVertex, sample_count)) {
            return core::Result<WeightCorrespondence>::failure(detail::parse_error(
                "weight correspondence mapping is invalid"));
        }
        std::vector<Sample> samples;
        samples.reserve(sample_count);
        for (std::size_t sample = 0U; sample < sample_count; ++sample) {
            Sample entry;
            if (!detail::read_integer(input, entry.source_vertex) ||
                !detail::read_double(input, entry.coordinate)) {
                return core::Result<WeightCorrespondence>::failure(detail::parse_error(
                    "weight correspondence sample is invalid"));
            }
            samples.push_back(entry);
        }
        if (auto result = correspondence.map(target_vertex, std::move(samples));
            !result) {
            return core::Result<WeightCorrespondence>::failure(result.error());
        }
    }
    if (!detail::require_token(input, "END")) {
        return core::Result<WeightCorrespondence>::failure(detail::parse_error(
            "weight correspondence terminator is missing"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<WeightCorrespondence>::failure(detail::parse_error(
            "weight correspondence has trailing data"));
    }
    return core::Result<WeightCorrespondence>::success(std::move(correspondence));
}

core::Result<assets::Sha256Digest> WeightCorrespondence::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(serialize()));
}

// ---------------------------------------------------------------------------
// JointTransferMap

core::Result<void> JointTransferMap::pair(
    kinematic::JointId source,
    kinematic::JointId target) {
    if (!source || !target) {
        return core::Result<void>::failure(detail::invalid(
            "a joint transfer pair requires two non-zero joints"));
    }
    if (source == target) {
        return core::Result<void>::failure(detail::invalid(
            "a joint transfer pair requires two distinct joints; identity is "
            "expressed by leaving the joint unmapped"));
    }
    if (map_.size() >= kMaxEntries && map_.find(source) == map_.end()) {
        return core::Result<void>::failure(detail::invalid(
            "joint transfer map exceeds its entry bound"));
    }
    if (map_.find(source) != map_.end()) {
        return core::Result<void>::failure(detail::invalid(
            "a source joint maps to only one target joint"));
    }
    const auto claimed = std::find_if(map_.begin(), map_.end(),
        [target](const auto& entry) { return entry.second == target; });
    if (claimed != map_.end()) {
        return core::Result<void>::failure(detail::invalid(
            "a target joint has only one source joint; the transfer gather "
            "would otherwise double-count it"));
    }
    map_.emplace(source, target);
    return core::Result<void>::success();
}

kinematic::JointId JointTransferMap::mapped(kinematic::JointId source) const noexcept {
    const auto found = map_.find(source);
    return found == map_.end() ? source : found->second;
}

core::Result<void> JointTransferMap::validate() const {
    if (map_.size() > kMaxEntries) {
        return core::Result<void>::failure(detail::invalid(
            "joint transfer map exceeds its entry bound"));
    }
    for (const auto& [source, target] : map_) {
        if (!source || !target || source == target) {
            return core::Result<void>::failure(detail::validation(
                "joint transfer map entries must be distinct non-zero joints"));
        }
        const auto claimed = std::find_if(map_.begin(), map_.end(),
            [target, source](const auto& entry) {
                return entry.second == target && entry.first != source;
            });
        if (claimed != map_.end()) {
            return core::Result<void>::failure(detail::validation(
                "joint transfer map must be injective"));
        }
    }
    return core::Result<void>::success();
}

std::string JointTransferMap::serialize() const {
    std::ostringstream output;
    output << kJointTransferMagic << " 1\n";
    output << "ENTRIES " << map_.size() << '\n';
    for (const auto& [source, target] : map_) {
        output << "ENTRY " << source.value << ' ' << target.value << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> JointTransferMap::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(serialize()));
}

// ---------------------------------------------------------------------------
// WeightTransferOptions

core::Result<void> WeightTransferOptions::validate() const {
    if (!output_layer_id) {
        return core::Result<void>::failure(detail::invalid(
            "weight transfer requires a layer identity"));
    }
    if (output_mesh_digest.is_zero() || output_mesh_asset == 0U) {
        return core::Result<void>::failure(detail::invalid(
            "weight transfer requires a content-addressed target mesh identity"));
    }
    if (max_influences_per_vertex < 1U ||
        max_influences_per_vertex >
            kinematic::SkinBinding::kMaxInfluencesPerVertex) {
        return core::Result<void>::failure(detail::invalid(
            "weight transfer influence count is outside 1..8"));
    }
    return core::Result<void>::success();
}

// ---------------------------------------------------------------------------
// WeightTransferReceipt

core::Result<void> WeightTransferReceipt::validate() const {
    if (correspondence_digest.is_zero() || input_binding_digest.is_zero() ||
        output_binding_digest.is_zero()) {
        return core::Result<void>::failure(detail::invalid(
            "weight transfer receipt requires correspondence and binding digests"));
    }
    if (transferred_vertex_count == 0U ||
        transferred_vertex_count >
            character::SkinWeightLayer::kMaxInfluencedVertices ||
        total_influence_count < transferred_vertex_count ||
        total_influence_count >
            transferred_vertex_count *
                static_cast<std::uint64_t>(
                    kinematic::SkinBinding::kMaxInfluencesPerVertex)) {
        return core::Result<void>::failure(detail::validation(
            "weight transfer receipt influence population is invalid"));
    }
    return core::Result<void>::success();
}

std::string WeightTransferReceipt::serialize() const {
    std::ostringstream output;
    output << kReceiptMagic << ' ' << kSchemaVersion << '\n';
    output << "CORRESPONDENCE " << correspondence_digest.hex() << '\n';
    output << "JOINT_MAP " << joint_map_digest.hex() << '\n';
    output << "LOCKS " << locks_digest.hex() << '\n';
    output << "INPUT " << input_binding_digest.hex() << '\n';
    output << "OUTPUT " << output_binding_digest.hex() << '\n';
    output << "VERTICES " << transferred_vertex_count << '\n';
    output << "INFLUENCES " << total_influence_count << '\n';
    output << "RIGID_SAMPLES " << rigid_sample_count << '\n';
    output << "END\n";
    return output.str();
}

core::Result<WeightTransferReceipt> WeightTransferReceipt::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<WeightTransferReceipt>::failure(detail::parse_error(
            "weight transfer receipt text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kReceiptMagic ||
        !detail::read_integer(input, version) || version != kSchemaVersion) {
        return core::Result<WeightTransferReceipt>::failure(detail::parse_error(
            "weight transfer receipt framing is invalid"));
    }
    WeightTransferReceipt receipt;
    if (!detail::require_token(input, "CORRESPONDENCE") ||
        !detail::read_digest(input, receipt.correspondence_digest) ||
        !detail::require_token(input, "JOINT_MAP") ||
        !detail::read_digest(input, receipt.joint_map_digest) ||
        !detail::require_token(input, "LOCKS") ||
        !detail::read_digest(input, receipt.locks_digest) ||
        !detail::require_token(input, "INPUT") ||
        !detail::read_digest(input, receipt.input_binding_digest) ||
        !detail::require_token(input, "OUTPUT") ||
        !detail::read_digest(input, receipt.output_binding_digest) ||
        !detail::require_token(input, "VERTICES") ||
        !detail::read_integer(input, receipt.transferred_vertex_count) ||
        !detail::require_token(input, "INFLUENCES") ||
        !detail::read_integer(input, receipt.total_influence_count) ||
        !detail::require_token(input, "RIGID_SAMPLES") ||
        !detail::read_integer(input, receipt.rigid_sample_count) ||
        !detail::require_token(input, "END")) {
        return core::Result<WeightTransferReceipt>::failure(detail::parse_error(
            "weight transfer receipt body is invalid"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<WeightTransferReceipt>::failure(detail::parse_error(
            "weight transfer receipt has trailing data"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<WeightTransferReceipt>::failure(result.error());
    }
    return core::Result<WeightTransferReceipt>::success(std::move(receipt));
}

core::Result<assets::Sha256Digest> WeightTransferReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(serialize()));
}

// ---------------------------------------------------------------------------
// transfer_weights

core::Result<WeightTransferOutput> transfer_weights(
    const kinematic::SkinBinding& binding_source,
    const character::SkinWeightLayer& layer_source,
    const WeightCorrespondence& correspondence,
    const JointTransferMap& joint_map,
    const WeightTransferOptions& options,
    const WeightLocks* locks) {
    if (auto result = binding_source.validate(); !result) {
        return core::Result<WeightTransferOutput>::failure(result.error());
    }
    if (auto result = layer_source.validate(); !result) {
        return core::Result<WeightTransferOutput>::failure(result.error());
    }
    if (layer_source.mesh_topology_digest != binding_source.mesh_digest ||
        layer_source.joint_graph_digest != binding_source.graph_digest) {
        return core::Result<WeightTransferOutput>::failure(detail::invalid(
            "weight transfer layer pins do not match the source binding"));
    }
    if (auto result = correspondence.validate(); !result) {
        return core::Result<WeightTransferOutput>::failure(result.error());
    }
    if (auto result = joint_map.validate(); !result) {
        return core::Result<WeightTransferOutput>::failure(result.error());
    }
    if (auto result = options.validate(); !result) {
        return core::Result<WeightTransferOutput>::failure(result.error());
    }
    if (locks != nullptr) {
        if (auto result = locks->validate(); !result) {
            return core::Result<WeightTransferOutput>::failure(result.error());
        }
    }
    if (!joint_map.entries().empty() && options.output_graph_digest.is_zero()) {
        return core::Result<WeightTransferOutput>::failure(detail::invalid(
            "a joint-remapping transfer requires the target graph digest"));
    }
    const auto correspondence_digest = correspondence.canonical_digest();
    if (!correspondence_digest) {
        return core::Result<WeightTransferOutput>::failure(correspondence_digest.error());
    }
    const auto input_digest = binding_source.canonical_digest();
    if (!input_digest) {
        return core::Result<WeightTransferOutput>::failure(input_digest.error());
    }

    kinematic::SkinBinding output;
    output.mesh_asset = options.output_mesh_asset;
    output.mesh_digest = options.output_mesh_digest;
    output.graph_digest = options.output_graph_digest.is_zero()
        ? binding_source.graph_digest
        : options.output_graph_digest;
    output.mesh_bind_transform = binding_source.mesh_bind_transform;

    std::uint64_t total_influences = 0U;
    std::uint64_t rigid_samples = 0U;
    // Map iteration is ascending target vertex order: one deterministic pass.
    for (const auto& [target_vertex, samples] : correspondence.entries()) {
        // Barycentric gather: each sample contributes its source vertex's
        // influences scaled by its coordinate, remapped through the joint
        // map. Accumulation into a joint-keyed map keeps the joint set
        // deduplicated and in ascending order.
        std::map<kinematic::JointId, double> gathered;
        for (const WeightCorrespondence::Sample& sample : samples) {
            const auto source = binding_source.influences.find(sample.source_vertex);
            if (source == binding_source.influences.end()) {
                ++rigid_samples;
                continue;
            }
            for (const kinematic::JointInfluence& influence : source->second) {
                gathered[joint_map.mapped(influence.joint)] +=
                    sample.coordinate * influence.weight;
            }
        }
        if (gathered.empty()) {
            return core::Result<WeightTransferOutput>::failure(
                detail::validation(
                    "weight transfer refused: a target vertex has no "
                    "corresponding source influences (every sample is rigid "
                    "or unmapped; such regions require an explicit authored "
                    "assignment)")
                .with_context("vertex=" + std::to_string(target_vertex)));
        }
        std::vector<kinematic::JointInfluence> working;
        working.reserve(gathered.size());
        for (const auto& [joint, weight] : gathered) {
            working.push_back(kinematic::JointInfluence{joint, weight});
        }
        if (working.size() > options.max_influences_per_vertex) {
            // Deterministic cap: strongest influences survive, lowest joint
            // id on ties; dropping a locked weight refuses instead of
            // silently discarding an artist's pin.
            std::sort(working.begin(), working.end(),
                [](const auto& a, const auto& b) {
                    if (a.weight != b.weight) return a.weight > b.weight;
                    return a.joint < b.joint;
                });
            if (locks != nullptr) {
                for (std::size_t index = options.max_influences_per_vertex;
                     index < working.size(); ++index) {
                    if (locks->locked(target_vertex, working[index].joint)) {
                        return core::Result<WeightTransferOutput>::failure(
                            detail::validation(
                                "the influence cap would drop a locked "
                                "transferred weight")
                            .with_context("vertex=" + std::to_string(target_vertex)));
                    }
                }
            }
            working.resize(options.max_influences_per_vertex);
        }
        if (auto result = detail::admit_edited_vertex(target_vertex, working, locks);
            !result) {
            return core::Result<WeightTransferOutput>::failure(result.error());
        }
        total_influences += working.size();
        output.influences.emplace(target_vertex, std::move(working));
    }
    if (auto result = output.validate(); !result) {
        return core::Result<WeightTransferOutput>::failure(result.error());
    }
    const auto output_digest = output.canonical_digest();
    if (!output_digest) {
        return core::Result<WeightTransferOutput>::failure(output_digest.error());
    }

    assets::Sha256Digest joint_map_digest{};
    if (!joint_map.entries().empty()) {
        const auto digest = joint_map.canonical_digest();
        if (!digest) return core::Result<WeightTransferOutput>::failure(digest.error());
        joint_map_digest = digest.value();
    }
    assets::Sha256Digest locks_digest{};
    if (locks != nullptr) {
        const auto digest = locks->canonical_digest();
        if (!digest) return core::Result<WeightTransferOutput>::failure(digest.error());
        locks_digest = digest.value();
    }

    WeightTransferReceipt receipt;
    receipt.correspondence_digest = correspondence_digest.value();
    receipt.joint_map_digest = joint_map_digest;
    receipt.locks_digest = locks_digest;
    receipt.input_binding_digest = input_digest.value();
    receipt.output_binding_digest = output_digest.value();
    receipt.transferred_vertex_count =
        static_cast<std::uint64_t>(correspondence.entries().size());
    receipt.total_influence_count = total_influences;
    receipt.rigid_sample_count = rigid_samples;
    if (auto result = receipt.validate(); !result) {
        return core::Result<WeightTransferOutput>::failure(result.error());
    }

    character::SkinWeightLayer layer;
    layer.layer_id = options.output_layer_id;
    layer.mesh_topology_digest = output.mesh_digest;
    layer.joint_graph_digest = output.graph_digest;
    layer.influenced_vertex_count = receipt.transferred_vertex_count;
    layer.total_influence_count = total_influences;
    if (locks != nullptr) layer.locks_digest = locks_digest;
    layer.provenance = character::WeightProvenance::transferred;
    layer.provenance_digest = correspondence_digest.value();
    if (auto result = layer.validate(); !result) {
        return core::Result<WeightTransferOutput>::failure(result.error());
    }

    WeightTransferOutput transferred;
    transferred.binding = std::move(output);
    transferred.layer = std::move(layer);
    transferred.receipt = std::move(receipt);
    return core::Result<WeightTransferOutput>::success(std::move(transferred));
}

} // namespace carto::weights
