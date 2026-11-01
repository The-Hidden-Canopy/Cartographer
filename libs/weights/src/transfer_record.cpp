#include <carto/weights/transfer_record.hpp>

#include "weights_internal.hpp"

#include <sstream>
#include <utility>

namespace carto::weights {
namespace {
constexpr std::string_view kMagic = "CARTOGRAPHER_TRANSFERRED_WEIGHT_RECORD 1";

core::Result<void> mismatch(const char* message) {
    return core::Result<void>::failure(detail::validation(message));
}

bool read_component(std::string_view text, std::size_t& offset,
                    std::string_view name, std::size_t maximum,
                    std::string_view& component) {
    const auto end = text.find('\n', offset);
    if (end == std::string_view::npos) return false;
    std::istringstream header{std::string(text.substr(offset, end - offset))};
    std::string token, trailing;
    std::size_t size = 0;
    if (!(header >> token) || token != name ||
        !detail::read_count(header, maximum, size) || (header >> trailing)) return false;
    offset = end + 1;
    if (size > text.size() - offset) return false;
    component = text.substr(offset, size);
    offset += size;
    return true;
}
} // namespace

core::Result<void> TransferredWeightRecord::validate() const {
    const auto& binding = output.binding;
    const auto& layer = output.layer;
    const auto& receipt = output.receipt;
    if (auto r = binding.validate(); !r) return r;
    if (auto r = layer.validate(); !r) return r;
    if (auto r = receipt.validate(); !r) return r;
    const auto digest = binding.canonical_digest();
    if (!digest) return core::Result<void>::failure(digest.error());
    std::uint64_t influences = 0;
    for (const auto& entry : binding.influences) influences += entry.second.size();
    if (layer.provenance != character::WeightProvenance::transferred ||
        layer.staleness != character::WeightStaleness::fresh ||
        layer.stale_since.value() != 0 || layer.masks_digest.has_value()) {
        return mismatch("transfer record must describe an unmodified fresh transfer output");
    }
    if (layer.mesh_topology_digest != binding.mesh_digest ||
        layer.joint_graph_digest != binding.graph_digest ||
        layer.provenance_digest != receipt.correspondence_digest ||
        receipt.output_binding_digest != digest.value() ||
        layer.influenced_vertex_count != binding.influences.size() ||
        layer.influenced_vertex_count != receipt.transferred_vertex_count ||
        layer.total_influence_count != influences ||
        receipt.total_influence_count != influences ||
        (layer.locks_digest.has_value() != !receipt.locks_digest.is_zero()) ||
        (layer.locks_digest && *layer.locks_digest != receipt.locks_digest)) {
        return mismatch("transfer binding, layer and receipt do not agree");
    }
    if (binding.serialize().size() > kinematic::SkinBinding::kMaxSerializedBytes ||
        receipt.serialize().size() > WeightTransferReceipt::kMaxSerializedBytes) {
        return mismatch("transfer record component exceeds its byte bound");
    }
    return core::Result<void>::success();
}

core::Result<void> TransferredWeightRecord::validate_against_graph(
    const kinematic::KinematicGraph& graph) const {
    if (auto r = validate(); !r) return r;
    const auto digest = graph.canonical_digest();
    if (!digest) return core::Result<void>::failure(digest.error());
    if (digest.value() != output.binding.graph_digest)
        return mismatch("transfer record graph digest does not match the supplied graph");
    for (const auto& [vertex, influences] : output.binding.influences) {
        (void)vertex;
        for (const auto& influence : influences) {
            if (graph.find(influence.joint) == nullptr)
                return mismatch("transfer record references a joint absent from the graph");
        }
    }
    return core::Result<void>::success();
}

std::string TransferredWeightRecord::serialize() const {
    const auto& layer = output.layer;
    const auto binding = output.binding.serialize();
    const auto receipt = output.receipt.serialize();
    const assets::Sha256Digest zero{};
    std::ostringstream stream;
    stream << kMagic << '\n'
           << "LAYER " << layer.layer_id.value << ' '
           << layer.mesh_topology_digest.hex() << ' ' << layer.joint_graph_digest.hex() << ' '
           << layer.influenced_vertex_count << ' ' << layer.total_influence_count << ' '
           << layer.locks_digest.value_or(zero).hex() << ' '
           << layer.masks_digest.value_or(zero).hex() << ' '
           << static_cast<unsigned>(layer.provenance) << ' ' << layer.provenance_digest.hex() << ' '
           << static_cast<unsigned>(layer.staleness) << ' ' << layer.stale_since.value() << '\n'
           << "BINDING " << binding.size() << '\n' << binding
           << "RECEIPT " << receipt.size() << '\n' << receipt << "END\n";
    return stream.str();
}

core::Result<TransferredWeightRecord> TransferredWeightRecord::deserialize(std::string_view text) {
    const auto fail = [] { return core::Result<TransferredWeightRecord>::failure(
        detail::parse_error("invalid or non-canonical transferred weight record")); };
    if (text.empty() || text.size() > kMaxSerializedBytes) return fail();
    const auto first = text.find('\n');
    if (first == std::string_view::npos || text.substr(0, first) != kMagic) return fail();
    const auto second = text.find('\n', first + 1);
    if (second == std::string_view::npos) return fail();
    std::istringstream line{std::string(text.substr(first + 1, second - first - 1))};
    TransferredWeightRecord record;
    auto& layer = record.output.layer;
    assets::Sha256Digest locks, masks;
    std::uint32_t provenance = 0, staleness = 0;
    std::uint64_t revision = 0;
    std::string trailing;
    if (!detail::require_token(line, "LAYER") || !detail::read_integer(line, layer.layer_id.value) ||
        !detail::read_digest(line, layer.mesh_topology_digest) ||
        !detail::read_digest(line, layer.joint_graph_digest) ||
        !detail::read_integer(line, layer.influenced_vertex_count) ||
        !detail::read_integer(line, layer.total_influence_count) ||
        !detail::read_digest(line, locks) || !detail::read_digest(line, masks) ||
        !detail::read_integer(line, provenance) || !detail::read_digest(line, layer.provenance_digest) ||
        !detail::read_integer(line, staleness) || !detail::read_integer(line, revision) ||
        (line >> trailing) || provenance != static_cast<std::uint32_t>(character::WeightProvenance::transferred) ||
        staleness != static_cast<std::uint32_t>(character::WeightStaleness::fresh)) return fail();
    if (!locks.is_zero()) layer.locks_digest = locks;
    if (!masks.is_zero()) layer.masks_digest = masks;
    layer.provenance = character::WeightProvenance::transferred;
    layer.staleness = character::WeightStaleness::fresh;
    layer.stale_since = core::Revision{revision};
    std::size_t offset = second + 1;
    std::string_view binding_text, receipt_text;
    if (!read_component(text, offset, "BINDING", kinematic::SkinBinding::kMaxSerializedBytes, binding_text) ||
        !read_component(text, offset, "RECEIPT", WeightTransferReceipt::kMaxSerializedBytes, receipt_text) ||
        text.substr(offset) != "END\n") return fail();
    auto binding = kinematic::SkinBinding::deserialize(binding_text);
    auto receipt = WeightTransferReceipt::deserialize(receipt_text);
    if (!binding || !receipt) return fail();
    record.output.binding = std::move(binding.value());
    record.output.receipt = std::move(receipt.value());
    if (!record.validate() || record.serialize() != text) return fail();
    return core::Result<TransferredWeightRecord>::success(std::move(record));
}

core::Result<assets::Sha256Digest> TransferredWeightRecord::canonical_digest() const {
    if (auto r = validate(); !r) return core::Result<assets::Sha256Digest>::failure(r.error());
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(serialize()));
}
} // namespace carto::weights
