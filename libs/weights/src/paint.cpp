#include <carto/weights/paint.hpp>

#include "weights_internal.hpp"

#include <carto/character/weights_record.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace carto::weights {
namespace {

constexpr std::string_view kMaskMagic = "CARTOGRAPHER_WEIGHTS_MASK";
constexpr std::string_view kLocksMagic = "CARTOGRAPHER_WEIGHTS_LOCKS";
constexpr std::string_view kReceiptMagic = "CARTOGRAPHER_WEIGHTS_PAINT_RECEIPT";

double falloff_factor(const BrushStamp& stamp, double distance) noexcept {
    if (stamp.falloff == BrushFalloff::vertex) return 1.0;
    return std::max(0.0, 1.0 - distance / stamp.radius);
}

core::Result<PaintOutput> apply_stamp_impl(
    const kinematic::SkinBinding& binding,
    const geometry::CompiledMesh& mesh,
    const std::vector<std::vector<std::uint32_t>>& adjacency,
    const BrushStamp& stamp,
    kinematic::JointId joint,
    const WeightMask* mask,
    const WeightLocks* locks,
    std::uint64_t stamp_index) {
    if (!joint) {
        return core::Result<PaintOutput>::failure(detail::invalid(
            "a paint stroke requires an owning joint"));
    }
    if (auto result = stamp.validate(); !result) {
        return core::Result<PaintOutput>::failure(result.error());
    }
    if (auto result = binding.validate(); !result) {
        return core::Result<PaintOutput>::failure(result.error());
    }
    bool joint_present = false;
    for (const auto& [vertex, influences] : binding.influences) {
        static_cast<void>(vertex);
        for (const kinematic::JointInfluence& influence : influences) {
            if (influence.joint == joint) {
                joint_present = true;
                break;
            }
        }
        if (joint_present) break;
    }
    if (!joint_present) {
        return core::Result<PaintOutput>::failure(detail::invalid(
            "paint stroke references a joint absent from the binding"));
    }
    if (mask != nullptr) {
        if (auto result = mask->validate(); !result) {
            return core::Result<PaintOutput>::failure(result.error());
        }
    }
    if (locks != nullptr) {
        if (auto result = locks->validate(); !result) {
            return core::Result<PaintOutput>::failure(result.error());
        }
    }

    // Effect per vertex, in ascending vertex order.
    std::vector<std::pair<std::uint32_t, double>> effects;
    for (std::size_t vertex = 0U; vertex < mesh.positions.size(); ++vertex) {
        const double distance = detail::distance_between(stamp.center, mesh.positions[vertex]);
        if (distance > stamp.radius) continue;
        double effect = stamp.strength * falloff_factor(stamp, distance);
        if (mask != nullptr) effect *= mask->factor(static_cast<std::uint32_t>(vertex));
        if (effect <= 0.0) continue;
        effects.emplace_back(static_cast<std::uint32_t>(vertex), effect);
    }

    kinematic::SkinBinding output = binding;
    for (const auto& [vertex, effect] : effects) {
        std::vector<kinematic::JointInfluence> working;
        const auto existing = binding.influences.find(vertex);
        if (existing != binding.influences.end()) working = existing->second;

        const auto is_locked = [locks, vertex](kinematic::JointId entry_joint) {
            return locks != nullptr && locks->locked(vertex, entry_joint);
        };
        const auto find_joint = [&working](kinematic::JointId entry_joint) {
            return std::find_if(working.begin(), working.end(),
                [entry_joint](const kinematic::JointInfluence& influence) {
                    return influence.joint == entry_joint;
                });
        };

        switch (stamp.op) {
        case PaintOp::add: {
            const auto found = find_joint(joint);
            if (found != working.end()) {
                if (!is_locked(joint)) found->weight += effect;
            } else {
                working.push_back(kinematic::JointInfluence{joint, effect});
            }
            break;
        }
        case PaintOp::set: {
            const auto found = find_joint(joint);
            if (found != working.end()) {
                if (!is_locked(joint)) found->weight = effect;
            } else {
                working.push_back(kinematic::JointInfluence{joint, effect});
            }
            break;
        }
        case PaintOp::smooth: {
            if (vertex >= adjacency.size() || adjacency[vertex].empty()) break;
            for (kinematic::JointInfluence& influence : working) {
                if (is_locked(influence.joint)) continue;
                double neighbor_mass = 0.0;
                for (const std::uint32_t neighbor : adjacency[vertex]) {
                    const auto& neighbor_influences = binding.influences.find(neighbor);
                    if (neighbor_influences == binding.influences.end()) continue;
                    const auto neighbor_entry = std::find_if(
                        neighbor_influences->second.begin(), neighbor_influences->second.end(),
                        [&](const kinematic::JointInfluence& entry) {
                            return entry.joint == influence.joint;
                        });
                    if (neighbor_entry != neighbor_influences->second.end()) {
                        neighbor_mass += neighbor_entry->weight;
                    }
                }
                const double average =
                    neighbor_mass / static_cast<double>(adjacency[vertex].size());
                influence.weight = std::max(
                    0.0, influence.weight + effect * (average - influence.weight));
            }
            break;
        }
        }

        // Prune dead influences (locked ones were checked above), then push
        // residual mass through the deterministic locked-aware normalizer.
        working.erase(
            std::remove_if(working.begin(), working.end(),
                [&](const kinematic::JointInfluence& influence) {
                    return influence.weight <= detail::kPruneEpsilon && !is_locked(influence.joint);
                }),
            working.end());
        if (auto result = detail::renormalize_with_locks(vertex, working, locks); !result) {
            return core::Result<PaintOutput>::failure(result.error());
        }
        if (working.empty()) {
            return core::Result<PaintOutput>::failure(detail::validation(
                "a paint stroke would leave a vertex without a valid influence")
                .with_context("vertex=" + std::to_string(vertex)));
        }
        std::sort(working.begin(), working.end(), [](const auto& a, const auto& b) {
            return a.joint < b.joint;
        });
        const auto admitted = character::validate_influences(working);
        if (!admitted) {
            return core::Result<PaintOutput>::failure(
                admitted.error().with_context("vertex=" + std::to_string(vertex)));
        }
        output.influences[vertex] = std::move(working);
    }

    if (auto result = output.validate(); !result) {
        return core::Result<PaintOutput>::failure(result.error());
    }
    const auto input_digest = binding.canonical_digest();
    const auto output_digest = output.canonical_digest();
    if (!input_digest) return core::Result<PaintOutput>::failure(input_digest.error());
    if (!output_digest) return core::Result<PaintOutput>::failure(output_digest.error());

    assets::Sha256Digest mask_digest{};
    assets::Sha256Digest locks_digest{};
    if (mask != nullptr) {
        const auto digest = mask->canonical_digest();
        if (!digest) return core::Result<PaintOutput>::failure(digest.error());
        mask_digest = digest.value();
    }
    if (locks != nullptr) {
        const auto digest = locks->canonical_digest();
        if (!digest) return core::Result<PaintOutput>::failure(digest.error());
        locks_digest = digest.value();
    }

    PaintReceipt receipt;
    receipt.stamp_index = stamp_index;
    receipt.op = stamp.op;
    receipt.falloff = stamp.falloff;
    receipt.center = stamp.center;
    receipt.radius = stamp.radius;
    receipt.strength = stamp.strength;
    receipt.joint = joint;
    receipt.mask_digest = mask_digest;
    receipt.locks_digest = locks_digest;
    receipt.input_binding_digest = input_digest.value();
    receipt.output_binding_digest = output_digest.value();
    receipt.affected_vertex_count = effects.size();

    PaintOutput painted;
    painted.binding = std::move(output);
    painted.receipt = std::move(receipt);
    return core::Result<PaintOutput>::success(std::move(painted));
}

} // namespace

std::string_view brush_falloff_name(BrushFalloff falloff) noexcept {
    switch (falloff) {
    case BrushFalloff::spherical: return "spherical";
    case BrushFalloff::vertex: return "vertex";
    }
    return "unknown";
}

std::string_view paint_op_name(PaintOp op) noexcept {
    switch (op) {
    case PaintOp::add: return "add";
    case PaintOp::set: return "set";
    case PaintOp::smooth: return "smooth";
    }
    return "unknown";
}

core::Result<void> BrushStamp::validate() const {
    if (!center.finite()) {
        return core::Result<void>::failure(detail::invalid(
            "brush stamp center must be finite"));
    }
    if (!std::isfinite(radius) || radius <= 0.0) {
        return core::Result<void>::failure(detail::invalid(
            "brush stamp radius must be positive and finite"));
    }
    if (!std::isfinite(strength) || strength < 0.0 || strength > 1.0) {
        return core::Result<void>::failure(detail::invalid(
            "brush stamp strength must be inside [0, 1]"));
    }
    return core::Result<void>::success();
}

core::Result<void> WeightMask::set(std::uint32_t vertex, double factor) {
    if (!std::isfinite(factor) || factor < 0.0 || factor > 1.0) {
        return core::Result<void>::failure(detail::invalid(
            "a paint mask factor must be inside [0, 1]"));
    }
    if (entries_.size() >= kMaxEntries && entries_.find(vertex) == entries_.end()) {
        return core::Result<void>::failure(detail::invalid(
            "paint mask exceeds its entry bound"));
    }
    entries_[vertex] = factor;
    return core::Result<void>::success();
}

double WeightMask::factor(std::uint32_t vertex) const noexcept {
    const auto found = entries_.find(vertex);
    return found == entries_.end() ? 1.0 : found->second;
}

core::Result<void> WeightMask::validate() const {
    if (entries_.size() > kMaxEntries) {
        return core::Result<void>::failure(detail::invalid(
            "paint mask exceeds its entry bound"));
    }
    for (const auto& [vertex, factor] : entries_) {
        static_cast<void>(vertex);
        if (!std::isfinite(factor) || factor < 0.0 || factor > 1.0) {
            return core::Result<void>::failure(detail::validation(
                "paint mask factors must be inside [0, 1]"));
        }
    }
    return core::Result<void>::success();
}

std::string WeightMask::serialize() const {
    std::ostringstream output;
    output << kMaskMagic << " 1\n";
    output << std::setprecision(17);
    output << "ENTRIES " << entries_.size() << '\n';
    for (const auto& [vertex, factor] : entries_) {
        output << "ENTRY " << vertex << ' ' << factor << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> WeightMask::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(serialize()));
}

core::Result<void> WeightLocks::lock(std::uint32_t vertex, kinematic::JointId joint) {
    if (!joint) {
        return core::Result<void>::failure(detail::invalid(
            "cannot lock the invalid joint"));
    }
    std::vector<kinematic::JointId>& joints = locked_[vertex];
    if (joints.size() >= kMaxEntries && std::find(joints.begin(), joints.end(), joint) == joints.end()) {
        return core::Result<void>::failure(detail::invalid(
            "weight locks exceed their entry bound"));
    }
    const auto position = std::lower_bound(joints.begin(), joints.end(), joint);
    if (position == joints.end() || *position != joint) {
        joints.insert(position, joint);
    }
    return core::Result<void>::success();
}

bool WeightLocks::locked(std::uint32_t vertex, kinematic::JointId joint) const {
    const auto found = locked_.find(vertex);
    if (found == locked_.end()) return false;
    return std::binary_search(found->second.begin(), found->second.end(), joint);
}

core::Result<void> WeightLocks::validate() const {
    std::size_t total = 0U;
    for (const auto& [vertex, joints] : locked_) {
        static_cast<void>(vertex);
        total += joints.size();
        if (total > kMaxEntries) {
            return core::Result<void>::failure(detail::invalid(
                "weight locks exceed their entry bound"));
        }
        if (!std::is_sorted(joints.begin(), joints.end()) ||
            std::adjacent_find(joints.begin(), joints.end()) != joints.end()) {
            return core::Result<void>::failure(detail::validation(
                "weight locks must be sorted and unique per vertex"));
        }
        for (const kinematic::JointId joint : joints) {
            if (!joint) {
                return core::Result<void>::failure(detail::validation(
                    "weight locks reference the invalid joint"));
            }
        }
    }
    return core::Result<void>::success();
}

std::string WeightLocks::serialize() const {
    std::ostringstream output;
    output << kLocksMagic << " 1\n";
    output << "VERTICES " << locked_.size() << '\n';
    for (const auto& [vertex, joints] : locked_) {
        output << "LOCKS " << vertex << ' ' << joints.size();
        for (const kinematic::JointId joint : joints) {
            output << ' ' << joint.value;
        }
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> WeightLocks::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(serialize()));
}

core::Result<void> PaintStroke::validate() const {
    if (stamps.size() > kMaxStamps) {
        return core::Result<void>::failure(detail::invalid(
            "paint stroke exceeds its stamp bound"));
    }
    for (const BrushStamp& stamp : stamps) {
        if (auto result = stamp.validate(); !result) {
            return core::Result<void>::failure(result.error());
        }
    }
    return core::Result<void>::success();
}

core::Result<void> PaintReceipt::validate() const {
    if (!center.finite()) {
        return core::Result<void>::failure(detail::invalid(
            "paint receipt center must be finite"));
    }
    if (!std::isfinite(radius) || radius <= 0.0) {
        return core::Result<void>::failure(detail::invalid(
            "paint receipt radius must be positive and finite"));
    }
    if (!std::isfinite(strength) || strength < 0.0 || strength > 1.0) {
        return core::Result<void>::failure(detail::validation(
            "paint receipt strength must be inside [0, 1]"));
    }
    if (!joint) {
        return core::Result<void>::failure(detail::invalid(
            "paint receipt requires the owning joint"));
    }
    if (input_binding_digest.is_zero() || output_binding_digest.is_zero()) {
        return core::Result<void>::failure(detail::invalid(
            "paint receipt requires input and output binding digests"));
    }
    return core::Result<void>::success();
}

std::string PaintReceipt::serialize() const {
    std::ostringstream output;
    output << kReceiptMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "STAMP_INDEX " << stamp_index << '\n';
    output << "OP " << paint_op_name(op) << '\n';
    output << "FALLOFF " << brush_falloff_name(falloff) << '\n';
    output << "CENTER ";
    detail::write_vec3(output, center);
    output << '\n';
    output << "RADIUS " << radius << '\n';
    output << "STRENGTH " << strength << '\n';
    output << "JOINT " << joint.value << '\n';
    output << "MASK " << mask_digest.hex() << '\n';
    output << "LOCKS " << locks_digest.hex() << '\n';
    output << "INPUT " << input_binding_digest.hex() << '\n';
    output << "OUTPUT " << output_binding_digest.hex() << '\n';
    output << "AFFECTED " << affected_vertex_count << '\n';
    output << "END\n";
    return output.str();
}

core::Result<PaintReceipt> PaintReceipt::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<PaintReceipt>::failure(detail::parse_error(
            "paint receipt text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kReceiptMagic ||
        !detail::read_integer(input, version) || version != kSchemaVersion) {
        return core::Result<PaintReceipt>::failure(detail::parse_error(
            "paint receipt framing is invalid"));
    }
    PaintReceipt receipt;
    std::string op_token;
    std::string falloff_token;
    if (!detail::require_token(input, "STAMP_INDEX") ||
        !detail::read_integer(input, receipt.stamp_index) ||
        !detail::require_token(input, "OP") ||
        !(input >> op_token) ||
        !detail::require_token(input, "FALLOFF") ||
        !(input >> falloff_token) ||
        !detail::require_token(input, "CENTER") ||
        !detail::read_vec3(input, receipt.center) ||
        !detail::require_token(input, "RADIUS") ||
        !detail::read_double(input, receipt.radius) ||
        !detail::require_token(input, "STRENGTH") ||
        !detail::read_double(input, receipt.strength) ||
        !detail::require_token(input, "JOINT") ||
        !detail::read_integer(input, receipt.joint.value) ||
        !detail::require_token(input, "MASK") ||
        !detail::read_digest(input, receipt.mask_digest) ||
        !detail::require_token(input, "LOCKS") ||
        !detail::read_digest(input, receipt.locks_digest) ||
        !detail::require_token(input, "INPUT") ||
        !detail::read_digest(input, receipt.input_binding_digest) ||
        !detail::require_token(input, "OUTPUT") ||
        !detail::read_digest(input, receipt.output_binding_digest) ||
        !detail::require_token(input, "AFFECTED") ||
        !detail::read_integer(input, receipt.affected_vertex_count) ||
        !detail::require_token(input, "END")) {
        return core::Result<PaintReceipt>::failure(detail::parse_error(
            "paint receipt body is invalid"));
    }
    if (op_token == "add") {
        receipt.op = PaintOp::add;
    } else if (op_token == "set") {
        receipt.op = PaintOp::set;
    } else if (op_token == "smooth") {
        receipt.op = PaintOp::smooth;
    } else {
        return core::Result<PaintReceipt>::failure(detail::parse_error(
            "paint receipt operation is unknown"));
    }
    if (falloff_token == "spherical") {
        receipt.falloff = BrushFalloff::spherical;
    } else if (falloff_token == "vertex") {
        receipt.falloff = BrushFalloff::vertex;
    } else {
        return core::Result<PaintReceipt>::failure(detail::parse_error(
            "paint receipt falloff is unknown"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<PaintReceipt>::failure(result.error());
    }
    return core::Result<PaintReceipt>::success(std::move(receipt));
}

core::Result<assets::Sha256Digest> PaintReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(serialize()));
}

core::Result<PaintOutput> apply_brush_stamp(
    const kinematic::SkinBinding& binding,
    const geometry::CompiledMesh& mesh,
    const BrushStamp& stamp,
    kinematic::JointId joint,
    const WeightMask* mask,
    const WeightLocks* locks) {
    const auto& adjacency = detail::build_adjacency(
        mesh.positions.size(), mesh.indices);
    return apply_stamp_impl(
        binding, mesh, adjacency, stamp, joint, mask, locks, 0U);
}

core::Result<PaintStrokeOutput> apply_paint_stroke(
    const kinematic::SkinBinding& binding,
    const geometry::CompiledMesh& mesh,
    const PaintStroke& stroke,
    kinematic::JointId joint,
    const WeightMask* mask,
    const WeightLocks* locks) {
    if (auto result = stroke.validate(); !result) {
        return core::Result<PaintStrokeOutput>::failure(result.error());
    }
    const auto& adjacency = detail::build_adjacency(
        mesh.positions.size(), mesh.indices);
    kinematic::SkinBinding current = binding;
    PaintStrokeOutput output;
    for (std::size_t index = 0U; index < stroke.stamps.size(); ++index) {
        const auto painted = apply_stamp_impl(
            current, mesh, adjacency, stroke.stamps[index], joint, mask, locks,
            static_cast<std::uint64_t>(index));
        if (!painted) {
            return core::Result<PaintStrokeOutput>::failure(
                painted.error().with_context("stamp=" + std::to_string(index)));
        }
        current = painted.value().binding;
        output.receipts.push_back(painted.value().receipt);
    }
    output.binding = std::move(current);
    return core::Result<PaintStrokeOutput>::success(std::move(output));
}

// ---------------------------------------------------------------------------
// Whole-region weight commands.

namespace {

constexpr std::string_view kMirrorMapMagic = "CARTOGRAPHER_WEIGHTS_JOINT_MIRROR";
constexpr std::string_view kEditReceiptMagic = "CARTOGRAPHER_WEIGHTS_EDIT_RECEIPT";

std::set<kinematic::JointId> binding_joints(const kinematic::SkinBinding& binding) {
    std::set<kinematic::JointId> joints;
    for (const auto& [vertex, influences] : binding.influences) {
        static_cast<void>(vertex);
        for (const kinematic::JointInfluence& influence : influences) {
            joints.insert(influence.joint);
        }
    }
    return joints;
}

core::Result<WeightEditOutput> seal_weight_edit(
    const kinematic::SkinBinding& input,
    kinematic::SkinBinding output,
    WeightEditReceipt receipt,
    const WeightLocks* locks) {
    if (auto result = output.validate(); !result) {
        return core::Result<WeightEditOutput>::failure(result.error());
    }
    const auto input_digest = input.canonical_digest();
    const auto output_digest = output.canonical_digest();
    if (!input_digest) return core::Result<WeightEditOutput>::failure(input_digest.error());
    if (!output_digest) return core::Result<WeightEditOutput>::failure(output_digest.error());
    if (locks != nullptr) {
        const auto digest = locks->canonical_digest();
        if (!digest) return core::Result<WeightEditOutput>::failure(digest.error());
        receipt.locks_digest = digest.value();
    }
    receipt.input_binding_digest = input_digest.value();
    receipt.output_binding_digest = output_digest.value();
    if (auto result = receipt.validate(); !result) {
        return core::Result<WeightEditOutput>::failure(result.error());
    }
    WeightEditOutput edited;
    edited.binding = std::move(output);
    edited.receipt = std::move(receipt);
    return core::Result<WeightEditOutput>::success(std::move(edited));
}

double axis_component(const core::Vec3d& value, MirrorAxis axis) noexcept {
    switch (axis) {
    case MirrorAxis::x: return value.x;
    case MirrorAxis::y: return value.y;
    case MirrorAxis::z: return value.z;
    }
    return value.x;
}

core::Vec3d reflect(core::Vec3d value, MirrorAxis axis) noexcept {
    switch (axis) {
    case MirrorAxis::x: value.x = -value.x; break;
    case MirrorAxis::y: value.y = -value.y; break;
    case MirrorAxis::z: value.z = -value.z; break;
    }
    return value;
}

} // namespace

std::string_view weight_edit_kind_name(WeightEditKind kind) noexcept {
    switch (kind) {
    case WeightEditKind::flood: return "flood";
    case WeightEditKind::prune: return "prune";
    case WeightEditKind::mirror: return "mirror";
    }
    return "unknown";
}

std::string_view mirror_axis_name(MirrorAxis axis) noexcept {
    switch (axis) {
    case MirrorAxis::x: return "x";
    case MirrorAxis::y: return "y";
    case MirrorAxis::z: return "z";
    }
    return "unknown";
}

std::string_view mirror_direction_name(MirrorDirection direction) noexcept {
    switch (direction) {
    case MirrorDirection::positive_to_negative: return "positive_to_negative";
    case MirrorDirection::negative_to_positive: return "negative_to_positive";
    }
    return "unknown";
}

core::Result<void> FloodRequest::validate() const {
    if (!joint) {
        return core::Result<void>::failure(detail::invalid(
            "a weight flood requires a target joint"));
    }
    if (op != PaintOp::add && op != PaintOp::set) {
        return core::Result<void>::failure(detail::invalid(
            "a weight flood supports only add or set"));
    }
    if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
        return core::Result<void>::failure(detail::invalid(
            "a weight flood value must be inside [0, 1]"));
    }
    if (vertices.empty() || vertices.size() > kinematic::SkinBinding::kMaxInfluencedVertices) {
        return core::Result<void>::failure(detail::invalid(
            "a weight flood requires a bounded non-empty vertex set"));
    }
    std::vector<std::uint32_t> sorted = vertices;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        return core::Result<void>::failure(detail::invalid(
            "a weight flood vertex set must not repeat vertices"));
    }
    return core::Result<void>::success();
}

core::Result<void> JointMirrorMap::pair(kinematic::JointId first, kinematic::JointId second) {
    if (!first || !second || first == second) {
        return core::Result<void>::failure(detail::invalid(
            "a joint mirror pair requires two distinct non-zero joints"));
    }
    const auto existing_first = mirror_.find(first);
    const auto existing_second = mirror_.find(second);
    if (existing_first != mirror_.end() || existing_second != mirror_.end()) {
        if (existing_first != mirror_.end() && existing_first->second == second) {
            return core::Result<void>::success();
        }
        return core::Result<void>::failure(detail::invalid(
            "a joint may belong to only one mirror pair"));
    }
    if (mirror_.size() + 2U > kMaxPairs * 2U) {
        return core::Result<void>::failure(detail::invalid(
            "joint mirror map exceeds its pair bound"));
    }
    mirror_.emplace(first, second);
    mirror_.emplace(second, first);
    return core::Result<void>::success();
}

kinematic::JointId JointMirrorMap::mirrored(kinematic::JointId joint) const noexcept {
    const auto found = mirror_.find(joint);
    return found == mirror_.end() ? joint : found->second;
}

core::Result<void> JointMirrorMap::validate() const {
    if (mirror_.size() > kMaxPairs * 2U) {
        return core::Result<void>::failure(detail::invalid(
            "joint mirror map exceeds its pair bound"));
    }
    for (const auto& [joint, partner] : mirror_) {
        const auto back = mirror_.find(partner);
        if (!joint || !partner || joint == partner || back == mirror_.end() ||
            back->second != joint) {
            return core::Result<void>::failure(detail::validation(
                "joint mirror map must be a symmetric pairing"));
        }
    }
    return core::Result<void>::success();
}

std::string JointMirrorMap::serialize() const {
    std::ostringstream output;
    output << kMirrorMapMagic << " 1\n";
    output << "PAIRS " << mirror_.size() / 2U << '\n';
    for (const auto& [joint, partner] : mirror_) {
        if (joint < partner) {
            output << "PAIR " << joint.value << ' ' << partner.value << '\n';
        }
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> JointMirrorMap::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(serialize()));
}

core::Result<void> MirrorRequest::validate() const {
    if (axis != MirrorAxis::x && axis != MirrorAxis::y && axis != MirrorAxis::z) {
        return core::Result<void>::failure(detail::invalid("mirror axis is not supported"));
    }
    if (direction != MirrorDirection::positive_to_negative &&
        direction != MirrorDirection::negative_to_positive) {
        return core::Result<void>::failure(detail::invalid(
            "mirror direction is not supported"));
    }
    if (!std::isfinite(tolerance) || tolerance <= 0.0) {
        return core::Result<void>::failure(detail::invalid(
            "mirror tolerance must be positive and finite"));
    }
    return core::Result<void>::success();
}

core::Result<void> WeightEditReceipt::validate() const {
    if (kind != WeightEditKind::flood && kind != WeightEditKind::prune &&
        kind != WeightEditKind::mirror) {
        return core::Result<void>::failure(detail::validation(
            "weight edit receipt kind is invalid"));
    }
    if (!std::isfinite(value) || value < 0.0 || value > 1.0 ||
        !std::isfinite(tolerance) || tolerance < 0.0) {
        return core::Result<void>::failure(detail::validation(
            "weight edit receipt parameters are invalid"));
    }
    if (kind == WeightEditKind::flood && (!joint ||
        (op != PaintOp::add && op != PaintOp::set))) {
        return core::Result<void>::failure(detail::validation(
            "flood receipt requires a joint and an add/set op"));
    }
    if (kind == WeightEditKind::mirror && tolerance <= 0.0) {
        return core::Result<void>::failure(detail::validation(
            "mirror receipt requires a positive tolerance"));
    }
    if (input_binding_digest == assets::Sha256Digest{} ||
        output_binding_digest == assets::Sha256Digest{}) {
        return core::Result<void>::failure(detail::validation(
            "weight edit receipt must bind input and output bindings"));
    }
    return core::Result<void>::success();
}

std::string WeightEditReceipt::serialize() const {
    std::ostringstream output;
    output << kEditReceiptMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "KIND " << weight_edit_kind_name(kind) << '\n';
    output << "JOINT " << joint.value << '\n';
    output << "OP " << paint_op_name(op) << '\n';
    output << "VALUE " << value << '\n';
    output << "AXIS " << mirror_axis_name(axis) << '\n';
    output << "DIRECTION " << mirror_direction_name(direction) << '\n';
    output << "TOLERANCE " << tolerance << '\n';
    output << "JOINT_MAP " << joint_map_digest.hex() << '\n';
    output << "LOCKS " << locks_digest.hex() << '\n';
    output << "INPUT " << input_binding_digest.hex() << '\n';
    output << "OUTPUT " << output_binding_digest.hex() << '\n';
    output << "AFFECTED " << affected_vertex_count << '\n';
    output << "END\n";
    return output.str();
}

core::Result<WeightEditReceipt> WeightEditReceipt::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<WeightEditReceipt>::failure(detail::parse_error(
            "weight edit receipt text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kEditReceiptMagic ||
        !detail::read_integer(input, version) || version != kSchemaVersion) {
        return core::Result<WeightEditReceipt>::failure(detail::parse_error(
            "weight edit receipt framing is invalid"));
    }
    WeightEditReceipt receipt;
    std::string kind_token;
    std::string op_token;
    std::string axis_token;
    std::string direction_token;
    if (!detail::require_token(input, "KIND") || !(input >> kind_token) ||
        !detail::require_token(input, "JOINT") ||
        !detail::read_integer(input, receipt.joint.value) ||
        !detail::require_token(input, "OP") || !(input >> op_token) ||
        !detail::require_token(input, "VALUE") ||
        !detail::read_double(input, receipt.value) ||
        !detail::require_token(input, "AXIS") || !(input >> axis_token) ||
        !detail::require_token(input, "DIRECTION") || !(input >> direction_token) ||
        !detail::require_token(input, "TOLERANCE") ||
        !detail::read_double(input, receipt.tolerance) ||
        !detail::require_token(input, "JOINT_MAP") ||
        !detail::read_digest(input, receipt.joint_map_digest) ||
        !detail::require_token(input, "LOCKS") ||
        !detail::read_digest(input, receipt.locks_digest) ||
        !detail::require_token(input, "INPUT") ||
        !detail::read_digest(input, receipt.input_binding_digest) ||
        !detail::require_token(input, "OUTPUT") ||
        !detail::read_digest(input, receipt.output_binding_digest) ||
        !detail::require_token(input, "AFFECTED") ||
        !detail::read_integer(input, receipt.affected_vertex_count) ||
        !detail::require_token(input, "END")) {
        return core::Result<WeightEditReceipt>::failure(detail::parse_error(
            "weight edit receipt body is invalid"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<WeightEditReceipt>::failure(detail::parse_error(
            "weight edit receipt has trailing data"));
    }
    bool parsed = true;
    if (kind_token == "flood") receipt.kind = WeightEditKind::flood;
    else if (kind_token == "prune") receipt.kind = WeightEditKind::prune;
    else if (kind_token == "mirror") receipt.kind = WeightEditKind::mirror;
    else parsed = false;
    if (op_token == "add") receipt.op = PaintOp::add;
    else if (op_token == "set") receipt.op = PaintOp::set;
    else if (op_token == "smooth") receipt.op = PaintOp::smooth;
    else parsed = false;
    if (axis_token == "x") receipt.axis = MirrorAxis::x;
    else if (axis_token == "y") receipt.axis = MirrorAxis::y;
    else if (axis_token == "z") receipt.axis = MirrorAxis::z;
    else parsed = false;
    if (direction_token == "positive_to_negative") {
        receipt.direction = MirrorDirection::positive_to_negative;
    } else if (direction_token == "negative_to_positive") {
        receipt.direction = MirrorDirection::negative_to_positive;
    } else {
        parsed = false;
    }
    if (!parsed) {
        return core::Result<WeightEditReceipt>::failure(detail::parse_error(
            "weight edit receipt contains an unknown enumeration"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<WeightEditReceipt>::failure(result.error());
    }
    return core::Result<WeightEditReceipt>::success(std::move(receipt));
}

core::Result<assets::Sha256Digest> WeightEditReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(serialize()));
}

core::Result<WeightEditOutput> flood_weights(
    const kinematic::SkinBinding& binding,
    const geometry::CompiledMesh& mesh,
    const FloodRequest& request,
    const WeightLocks* locks) {
    if (auto result = request.validate(); !result) {
        return core::Result<WeightEditOutput>::failure(result.error());
    }
    if (auto result = binding.validate(); !result) {
        return core::Result<WeightEditOutput>::failure(result.error());
    }
    if (locks != nullptr) {
        if (auto result = locks->validate(); !result) {
            return core::Result<WeightEditOutput>::failure(result.error());
        }
    }
    if (!binding_joints(binding).contains(request.joint)) {
        return core::Result<WeightEditOutput>::failure(detail::invalid(
            "weight flood references a joint absent from the binding"));
    }
    std::vector<std::uint32_t> vertices = request.vertices;
    std::sort(vertices.begin(), vertices.end());

    kinematic::SkinBinding output = binding;
    for (const std::uint32_t vertex : vertices) {
        if (vertex >= mesh.positions.size()) {
            return core::Result<WeightEditOutput>::failure(detail::invalid(
                "weight flood vertex is outside the mesh")
                .with_context("vertex=" + std::to_string(vertex)));
        }
        const auto existing = binding.influences.find(vertex);
        if (existing == binding.influences.end()) {
            return core::Result<WeightEditOutput>::failure(detail::invalid(
                "weight flood cannot add coverage to a rigid vertex")
                .with_context("vertex=" + std::to_string(vertex)));
        }
        std::vector<kinematic::JointInfluence> working = existing->second;
        const bool locked = locks != nullptr && locks->locked(vertex, request.joint);
        const auto found = std::find_if(working.begin(), working.end(),
            [&request](const kinematic::JointInfluence& influence) {
                return influence.joint == request.joint;
            });
        if (found != working.end()) {
            if (!locked) {
                found->weight = request.op == PaintOp::add ?
                    found->weight + request.value : request.value;
            }
        } else if (request.value > 0.0) {
            working.push_back(kinematic::JointInfluence{request.joint, request.value});
        }
        if (auto result = detail::admit_edited_vertex(vertex, working, locks); !result) {
            return core::Result<WeightEditOutput>::failure(result.error());
        }
        output.influences[vertex] = std::move(working);
    }

    WeightEditReceipt receipt;
    receipt.kind = WeightEditKind::flood;
    receipt.joint = request.joint;
    receipt.op = request.op;
    receipt.value = request.value;
    receipt.affected_vertex_count = vertices.size();
    return seal_weight_edit(binding, std::move(output), std::move(receipt), locks);
}

core::Result<WeightEditOutput> prune_weights(
    const kinematic::SkinBinding& binding,
    double threshold,
    const WeightLocks* locks) {
    if (!std::isfinite(threshold) || threshold <= 0.0 || threshold >= 1.0) {
        return core::Result<WeightEditOutput>::failure(detail::invalid(
            "weight prune threshold must be inside (0, 1)"));
    }
    if (auto result = binding.validate(); !result) {
        return core::Result<WeightEditOutput>::failure(result.error());
    }
    if (locks != nullptr) {
        if (auto result = locks->validate(); !result) {
            return core::Result<WeightEditOutput>::failure(result.error());
        }
    }
    kinematic::SkinBinding output = binding;
    std::uint64_t affected = 0U;
    for (const auto& [vertex, influences] : binding.influences) {
        const auto is_locked = [locks, vertex](kinematic::JointId joint) {
            return locks != nullptr && locks->locked(vertex, joint);
        };
        const auto removable = [&](const kinematic::JointInfluence& influence) {
            return influence.weight < threshold && !is_locked(influence.joint);
        };
        if (std::none_of(influences.begin(), influences.end(), removable)) continue;

        std::vector<kinematic::JointInfluence> working = influences;
        const bool all_removable =
            std::all_of(working.begin(), working.end(), removable);
        if (all_removable) {
            // Keep the strongest influence (lowest joint on ties) so pruning
            // never turns a skinned vertex rigid.
            const auto keep = std::max_element(working.begin(), working.end(),
                [](const auto& a, const auto& b) { return a.weight < b.weight; });
            const kinematic::JointInfluence kept = *keep;
            working.assign(1U, kept);
        } else {
            working.erase(
                std::remove_if(working.begin(), working.end(), removable), working.end());
        }
        if (auto result = detail::admit_edited_vertex(vertex, working, locks); !result) {
            return core::Result<WeightEditOutput>::failure(result.error());
        }
        output.influences[vertex] = std::move(working);
        ++affected;
    }

    WeightEditReceipt receipt;
    receipt.kind = WeightEditKind::prune;
    receipt.value = threshold;
    receipt.affected_vertex_count = affected;
    return seal_weight_edit(binding, std::move(output), std::move(receipt), locks);
}

core::Result<WeightEditOutput> mirror_weights(
    const kinematic::SkinBinding& binding,
    const geometry::CompiledMesh& mesh,
    const JointMirrorMap& joints,
    const MirrorRequest& request,
    const WeightLocks* locks) {
    if (auto result = request.validate(); !result) {
        return core::Result<WeightEditOutput>::failure(result.error());
    }
    if (auto result = joints.validate(); !result) {
        return core::Result<WeightEditOutput>::failure(result.error());
    }
    if (auto result = binding.validate(); !result) {
        return core::Result<WeightEditOutput>::failure(result.error());
    }
    if (locks != nullptr) {
        if (auto result = locks->validate(); !result) {
            return core::Result<WeightEditOutput>::failure(result.error());
        }
    }
    if (mesh.positions.size() > kinematic::SkinBinding::kMaxVertexIndex) {
        return core::Result<WeightEditOutput>::failure(detail::invalid(
            "mirror mesh exceeds the skin vertex index bound"));
    }
    const std::set<kinematic::JointId> known_joints = binding_joints(binding);
    for (const auto& [joint, partner] : joints.entries()) {
        if (known_joints.contains(joint) != known_joints.contains(partner)) {
            return core::Result<WeightEditOutput>::failure(detail::invalid(
                "a mirror pair maps a bound joint onto a joint absent from the binding")
                .with_context("joint=" + std::to_string(joint.value)));
        }
    }

    const double tolerance = request.tolerance;
    const double sign =
        request.direction == MirrorDirection::positive_to_negative ? 1.0 : -1.0;
    // Source candidates (the copied-from half plus the plane band) bucketed
    // into tolerance-sized cells; reflected lookups scan the 27 neighbours.
    using CellKey = std::array<std::int64_t, 3>;
    constexpr double kMaxCellCoordinate = 4.0e18;
    const auto cell_of = [tolerance](const core::Vec3d& position, CellKey& key) {
        const std::array<double, 3> coordinates{position.x, position.y, position.z};
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            const double cell = std::floor(coordinates[axis] / tolerance);
            if (!std::isfinite(cell) || std::abs(cell) > kMaxCellCoordinate) return false;
            key[axis] = static_cast<std::int64_t>(cell);
        }
        return true;
    };
    std::map<CellKey, std::vector<std::uint32_t>> source_cells;
    std::vector<std::uint32_t> destinations;
    for (std::size_t index = 0U; index < mesh.positions.size(); ++index) {
        const auto vertex = static_cast<std::uint32_t>(index);
        const double side = sign * axis_component(mesh.positions[index], request.axis);
        if (side >= -tolerance) {
            CellKey key{};
            if (!cell_of(mesh.positions[index], key)) {
                return core::Result<WeightEditOutput>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "mirror tolerance is too small for the vertex coordinates"));
            }
            source_cells[key].push_back(vertex);
        }
        if (side <= tolerance) destinations.push_back(vertex);
    }

    const auto remap = [&joints](const std::vector<kinematic::JointInfluence>& influences) {
        std::vector<kinematic::JointInfluence> mapped;
        mapped.reserve(influences.size());
        for (const kinematic::JointInfluence& influence : influences) {
            mapped.push_back({joints.mirrored(influence.joint), influence.weight});
        }
        std::sort(mapped.begin(), mapped.end(), [](const auto& a, const auto& b) {
            return a.joint < b.joint;
        });
        return mapped;
    };

    kinematic::SkinBinding output = binding;
    std::uint64_t affected = 0U;
    for (const std::uint32_t destination : destinations) {
        const core::Vec3d target = reflect(mesh.positions[destination], request.axis);
        CellKey center{};
        if (!cell_of(target, center)) {
            return core::Result<WeightEditOutput>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "mirror tolerance is too small for the vertex coordinates"));
        }
        std::optional<std::uint32_t> match;
        double best = std::numeric_limits<double>::infinity();
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dz = -1; dz <= 1; ++dz) {
                    const auto cell = source_cells.find(
                        CellKey{center[0] + dx, center[1] + dy, center[2] + dz});
                    if (cell == source_cells.end()) continue;
                    for (const std::uint32_t candidate : cell->second) {
                        const double distance =
                            detail::distance_between(mesh.positions[candidate], target);
                        if (distance > tolerance) continue;
                        if (distance < best || (distance == best && candidate < *match)) {
                            best = distance;
                            match = candidate;
                        }
                    }
                }
            }
        }
        if (!match.has_value()) {
            return core::Result<WeightEditOutput>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "mirror found no symmetric source vertex within tolerance")
                .with_context("vertex=" + std::to_string(destination)));
        }
        const auto source = binding.influences.find(*match);
        const auto existing = binding.influences.find(destination);
        if (source == binding.influences.end()) {
            if (existing == binding.influences.end()) continue;
            return core::Result<WeightEditOutput>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "mirror source vertex is rigid while its destination is skinned")
                .with_context("vertex=" + std::to_string(destination)));
        }
        if (locks != nullptr && existing != binding.influences.end()) {
            for (const kinematic::JointInfluence& influence : existing->second) {
                if (locks->locked(destination, influence.joint)) {
                    return core::Result<WeightEditOutput>::failure(detail::validation(
                        "mirror would overwrite a locked weight")
                        .with_context("vertex=" + std::to_string(destination)));
                }
            }
        }
        std::vector<kinematic::JointInfluence> working = remap(source->second);
        const bool on_plane =
            std::abs(axis_component(mesh.positions[destination], request.axis)) <= tolerance;
        if (on_plane && *match == destination) {
            // A plane vertex is its own mirror: symmetrize by averaging the
            // vertex with its remapped copy.
            std::map<kinematic::JointId, double> averaged;
            for (const kinematic::JointInfluence& influence : source->second) {
                averaged[influence.joint] += 0.5 * influence.weight;
            }
            for (const kinematic::JointInfluence& influence : working) {
                averaged[influence.joint] += 0.5 * influence.weight;
            }
            working.clear();
            for (const auto& [joint, weight] : averaged) working.push_back({joint, weight});
        } else if (*match == destination) {
            continue;
        }
        if (working.size() > kinematic::SkinBinding::kMaxInfluencesPerVertex) {
            return core::Result<WeightEditOutput>::failure(detail::validation(
                "mirrored plane vertex would exceed the influence bound")
                .with_context("vertex=" + std::to_string(destination)));
        }
        if (auto result = detail::admit_edited_vertex(destination, working, nullptr); !result) {
            return core::Result<WeightEditOutput>::failure(result.error());
        }
        if (existing == binding.influences.end() || existing->second.size() != working.size() ||
            !std::equal(working.begin(), working.end(), existing->second.begin(),
                [](const auto& a, const auto& b) {
                    return a.joint == b.joint && std::abs(a.weight - b.weight) <= detail::kPruneEpsilon;
                })) {
            ++affected;
        }
        output.influences[destination] = std::move(working);
    }

    const auto map_digest = joints.canonical_digest();
    if (!map_digest) return core::Result<WeightEditOutput>::failure(map_digest.error());
    WeightEditReceipt receipt;
    receipt.kind = WeightEditKind::mirror;
    receipt.axis = request.axis;
    receipt.direction = request.direction;
    receipt.tolerance = request.tolerance;
    receipt.joint_map_digest = map_digest.value();
    receipt.affected_vertex_count = affected;
    return seal_weight_edit(binding, std::move(output), std::move(receipt), locks);
}

} // namespace carto::weights
