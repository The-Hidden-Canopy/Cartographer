#include <carto/kinematic/identity.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <system_error>
#include <utility>

namespace carto::kinematic {
namespace {

constexpr std::string_view kCorrespondenceMagic =
    "CARTOGRAPHER_KINEMATIC_CORRESPONDENCE";
constexpr std::string_view kReceiptMagic =
    "CARTOGRAPHER_KINEMATIC_REIMPORT_RECEIPT";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic version_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

bool sorted_unique(const std::vector<JointId>& joints) {
    return std::is_sorted(joints.begin(), joints.end()) &&
        std::adjacent_find(joints.begin(), joints.end()) == joints.end();
}

bool disjoint_sorted(const std::vector<JointId>& left, const std::vector<JointId>& right) {
    std::size_t left_index = 0U;
    std::size_t right_index = 0U;
    while (left_index < left.size() && right_index < right.size()) {
        if (left[left_index] == right[right_index]) return false;
        if (left[left_index] < right[right_index]) {
            ++left_index;
        } else {
            ++right_index;
        }
    }
    return true;
}

bool same_transform(const core::Transform& left, const core::Transform& right) {
    return left.translation.x == right.translation.x &&
        left.translation.y == right.translation.y &&
        left.translation.z == right.translation.z &&
        left.rotation.x == right.rotation.x &&
        left.rotation.y == right.rotation.y &&
        left.rotation.z == right.rotation.z &&
        left.rotation.w == right.rotation.w &&
        left.scale.x == right.scale.x && left.scale.y == right.scale.y &&
        left.scale.z == right.scale.z;
}

template <typename T>
bool read_integer(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(
        token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    value = parsed;
    return true;
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_integer(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

void write_transform(std::ostream& output, const core::Transform& transform) {
    output << transform.translation.x << ' ' << transform.translation.y << ' '
           << transform.translation.z << ' ' << transform.rotation.x << ' '
           << transform.rotation.y << ' ' << transform.rotation.z << ' '
           << transform.rotation.w << ' ' << transform.scale.x << ' '
           << transform.scale.y << ' ' << transform.scale.z;
}

bool read_transform(std::istream& input, core::Transform& transform) {
    return static_cast<bool>(
        input >> transform.translation.x >> transform.translation.y >>
        transform.translation.z >> transform.rotation.x >> transform.rotation.y >>
        transform.rotation.z >> transform.rotation.w >> transform.scale.x >>
        transform.scale.y >> transform.scale.z);
}

void write_digest(std::ostream& output, const assets::Sha256Digest& digest) {
    output << digest.hex();
}

bool read_digest(std::istream& input, assets::Sha256Digest& digest) {
    std::string token;
    if (!(input >> token)) return false;
    const auto parsed = assets::Sha256Digest::from_hex(token);
    if (!parsed) return false;
    digest = parsed.value();
    return true;
}

void write_ids(std::ostream& output, std::string_view tag, const std::vector<JointId>& ids) {
    output << tag << ' ' << ids.size();
    for (const JointId joint : ids) output << ' ' << joint.value;
    output << '\n';
}

bool read_ids(
    std::istream& input,
    std::string_view expected,
    std::size_t maximum,
    std::vector<JointId>& ids) {
    std::size_t count = 0U;
    if (!require_token(input, expected) || !read_count(input, maximum, count)) {
        return false;
    }
    ids.clear();
    ids.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        JointId joint;
        if (!read_integer(input, joint.value) || !joint) return false;
        ids.push_back(joint);
    }
    return true;
}

} // namespace

core::Result<void> MeshRevisionBinding::validate() const {
    if (mesh_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "mesh revision binding must name its mesh content digest"));
    }
    if (!binding_rest.finite()) {
        return core::Result<void>::failure(invalid(
            "mesh revision binding rest transform is not finite"));
    }
    return core::Result<void>::success();
}

core::Result<void> JointIdentityRecord::validate() const {
    if (!joint || !safe_text(stable_name, JointCorrespondence::kMaxStableNameBytes)) {
        return core::Result<void>::failure(invalid(
            "joint identity or stable name is invalid"));
    }
    if (parent.has_value() && (!parent.value() || parent.value() == joint)) {
        return core::Result<void>::failure(validation(
            "joint identity parent is invalid or self-referential"));
    }
    if (!authored_rest.finite()) {
        return core::Result<void>::failure(invalid(
            "joint identity rest transform is not finite"));
    }
    if (bindings.size() > JointCorrespondence::kMaxBindingsPerJoint ||
        !std::is_sorted(
            bindings.begin(), bindings.end(),
            [](const MeshRevisionBinding& left, const MeshRevisionBinding& right) {
                return left.mesh_revision < right.mesh_revision;
            })) {
        return core::Result<void>::failure(validation(
            "joint identity bindings must be bounded and in ascending revision order"));
    }
    for (std::size_t index = 0U; index < bindings.size(); ++index) {
        if (auto result = bindings[index].validate(); !result) return result;
        if (index > 0U &&
            bindings[index - 1U].mesh_revision == bindings[index].mesh_revision) {
            return core::Result<void>::failure(validation(
                "joint identity bindings must be unique per mesh revision"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> JointCorrespondence::insert(JointIdentityRecord record) {
    if (joints_.size() >= kMaxJoints) {
        return core::Result<void>::failure(validation(
            "joint correspondence has reached its bounded joint capacity"));
    }
    if (auto result = record.validate(); !result) return result;
    const auto [iterator, inserted] =
        joints_.emplace(record.joint, std::move(record));
    static_cast<void>(iterator);
    if (!inserted) {
        return core::Result<void>::failure(invalid(
            "joint identity is duplicated"));
    }
    return core::Result<void>::success();
}

const JointIdentityRecord* JointCorrespondence::find(JointId joint) const noexcept {
    const auto found = joints_.find(joint);
    return found == joints_.end() ? nullptr : &found->second;
}

core::Result<const MeshRevisionBinding*> JointCorrespondence::resolve(
    JointId joint,
    core::Revision revision) const {
    const JointIdentityRecord* record = find(joint);
    if (record == nullptr) {
        return core::Result<const MeshRevisionBinding*>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "joint identity is unknown"));
    }
    for (const MeshRevisionBinding& binding : record->bindings) {
        if (binding.mesh_revision == revision) {
            return core::Result<const MeshRevisionBinding*>::success(&binding);
        }
    }
    return core::Result<const MeshRevisionBinding*>::failure(stale(
        "joint identity has no binding at the requested mesh revision"));
}

core::Result<void> JointCorrespondence::validate() const {
    if (!safe_text(source_namespace, kMaxNamespaceBytes)) {
        return core::Result<void>::failure(invalid(
            "joint correspondence source namespace is invalid"));
    }
    if (mesh_asset == 0U || mesh_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "joint correspondence must name its mesh identity and digest"));
    }
    if (joints_.size() > kMaxJoints) {
        return core::Result<void>::failure(validation(
            "joint correspondence exceeds its bounded joint capacity"));
    }
    for (const auto& [joint, record] : joints_) {
        if (joint != record.joint) {
            return core::Result<void>::failure(validation(
                "joint correspondence map identity does not match its record"));
        }
        if (auto result = record.validate(); !result) return result;
        if (record.parent.has_value() && !joints_.contains(record.parent.value())) {
            return core::Result<void>::failure(validation(
                "joint identity references a missing parent"));
        }
    }
    for (const auto& [joint, record] : joints_) {
        std::set<JointId> visited;
        const JointIdentityRecord* current = &record;
        while (current->parent.has_value()) {
            if (!visited.insert(current->joint).second) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::cycle_detected,
                    "joint identity hierarchy contains a cycle"));
            }
            current = &joints_.at(current->parent.value());
        }
    }
    return core::Result<void>::success();
}

std::string JointCorrespondence::serialize() const {
    std::ostringstream output;
    output << kCorrespondenceMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "SOURCE " << std::quoted(source_namespace) << '\n';
    output << "MESH " << mesh_asset << ' ' << mesh_revision.value() << ' ';
    write_digest(output, mesh_digest);
    output << '\n';
    output << "JOINTS " << joints_.size() << '\n';
    for (const auto& [joint, record] : joints_) {
        output << "JOINT " << joint.value << ' ' << std::quoted(record.stable_name)
               << ' ' << (record.parent.has_value() ? record.parent->value : 0U)
               << ' ';
        write_transform(output, record.authored_rest);
        output << ' ' << record.bindings.size() << '\n';
        for (const MeshRevisionBinding& binding : record.bindings) {
            output << "BINDING " << binding.mesh_revision.value() << ' ';
            write_digest(output, binding.mesh_digest);
            output << ' ' << binding.surface_anchor << ' ';
            write_transform(output, binding.binding_rest);
            output << '\n';
        }
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> JointCorrespondence::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<JointCorrespondence> JointCorrespondence::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<JointCorrespondence>::failure(parse_error(
            "joint correspondence text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kCorrespondenceMagic) {
        return core::Result<JointCorrespondence>::failure(parse_error(
            "joint correspondence framing is invalid"));
    }
    if (!read_integer(input, version)) {
        return core::Result<JointCorrespondence>::failure(parse_error(
            "joint correspondence schema version is missing"));
    }
    if (version != kSchemaVersion) {
        return core::Result<JointCorrespondence>::failure(version_error(
            "joint correspondence schema version is not supported"));
    }

    JointCorrespondence correspondence;
    std::uint64_t mesh_revision = 0U;
    if (!require_token(input, "SOURCE") ||
        !(input >> std::quoted(correspondence.source_namespace)) ||
        !require_token(input, "MESH") ||
        !read_integer(input, correspondence.mesh_asset) ||
        !read_integer(input, mesh_revision) ||
        !read_digest(input, correspondence.mesh_digest)) {
        return core::Result<JointCorrespondence>::failure(parse_error(
            "joint correspondence header is invalid"));
    }
    correspondence.mesh_revision = core::Revision{mesh_revision};

    std::size_t joint_count = 0U;
    if (!require_token(input, "JOINTS") ||
        !read_count(input, kMaxJoints, joint_count)) {
        return core::Result<JointCorrespondence>::failure(parse_error(
            "joint correspondence joint header is invalid"));
    }
    for (std::size_t index = 0U; index < joint_count; ++index) {
        JointIdentityRecord record;
        std::uint64_t parent = 0U;
        std::size_t binding_count = 0U;
        if (!require_token(input, "JOINT") ||
            !read_integer(input, record.joint.value) ||
            !(input >> std::quoted(record.stable_name)) ||
            !read_integer(input, parent) ||
            !read_transform(input, record.authored_rest) ||
            !read_count(input, kMaxBindingsPerJoint, binding_count)) {
            return core::Result<JointCorrespondence>::failure(parse_error(
                "joint identity record is malformed"));
        }
        record.parent = parent == 0U
            ? std::optional<JointId>{}
            : std::optional<JointId>{JointId{parent}};
        record.bindings.reserve(binding_count);
        for (std::size_t binding_index = 0U; binding_index < binding_count;
             ++binding_index) {
            MeshRevisionBinding binding;
            std::uint64_t revision = 0U;
            if (!require_token(input, "BINDING") ||
                !read_integer(input, revision) ||
                !read_digest(input, binding.mesh_digest) ||
                !read_integer(input, binding.surface_anchor) ||
                !read_transform(input, binding.binding_rest)) {
                return core::Result<JointCorrespondence>::failure(parse_error(
                    "mesh revision binding record is malformed"));
            }
            binding.mesh_revision = core::Revision{revision};
            record.bindings.push_back(std::move(binding));
        }
        if (auto result = correspondence.insert(std::move(record)); !result) {
            return core::Result<JointCorrespondence>::failure(result.error());
        }
    }

    if (!require_token(input, "END")) {
        return core::Result<JointCorrespondence>::failure(parse_error(
            "joint correspondence is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<JointCorrespondence>::failure(parse_error(
            "joint correspondence contains trailing unbound data"));
    }
    if (auto result = correspondence.validate(); !result) {
        return core::Result<JointCorrespondence>::failure(result.error());
    }
    return core::Result<JointCorrespondence>::success(std::move(correspondence));
}

core::Result<void> KinematicReimportReceipt::validate() const {
    if (mesh_asset == 0U || mesh_digest_before.is_zero() ||
        mesh_digest_after.is_zero()) {
        return core::Result<void>::failure(validation(
            "reimport receipt must name its mesh identity and digests"));
    }
    if (revision_after < revision_before) {
        return core::Result<void>::failure(validation(
            "reimport receipt revisions must not regress"));
    }
    const std::vector<JointId>* lists[] = {
        &preserved_joints, &rebound_joints, &removed_joints, &added_joints};
    for (const std::vector<JointId>* list : lists) {
        if (list->size() > kMaxJoints || !sorted_unique(*list)) {
            return core::Result<void>::failure(validation(
                "reimport receipt joint lists must be bounded, sorted, and unique"));
        }
    }
    for (std::size_t first = 0U; first < 4U; ++first) {
        for (std::size_t second = first + 1U; second < 4U; ++second) {
            if (!disjoint_sorted(*lists[first], *lists[second])) {
                return core::Result<void>::failure(validation(
                    "reimport receipt joint lists must be disjoint"));
            }
        }
    }
    if (receipt_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "reimport receipt digest is missing"));
    }
    const std::string canonical = canonical_without_digest();
    const assets::Sha256Digest expected = assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
    if (!(expected == receipt_digest)) {
        return core::Result<void>::failure(validation(
            "reimport receipt digest does not match its content"));
    }
    return core::Result<void>::success();
}

std::string KinematicReimportReceipt::canonical_without_digest() const {
    std::ostringstream output;
    output << kReceiptMagic << ' ' << kSchemaVersion << '\n';
    output << "REVISION " << revision_before.value() << ' '
           << revision_after.value() << '\n';
    output << "MESH " << mesh_asset << ' ';
    write_digest(output, mesh_digest_before);
    output << ' ';
    write_digest(output, mesh_digest_after);
    output << '\n';
    write_ids(output, "PRESERVED", preserved_joints);
    write_ids(output, "REBOUND", rebound_joints);
    write_ids(output, "REMOVED", removed_joints);
    write_ids(output, "ADDED", added_joints);
    return output.str();
}

core::Result<void> KinematicReimportReceipt::refresh_digest() {
    const std::string canonical = canonical_without_digest();
    receipt_digest = assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
    return core::Result<void>::success();
}

std::string KinematicReimportReceipt::serialize() const {
    std::ostringstream output;
    output << canonical_without_digest();
    output << "DIGEST ";
    write_digest(output, receipt_digest);
    output << '\n';
    output << "END\n";
    return output.str();
}

core::Result<KinematicReimportReceipt> KinematicReimportReceipt::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<KinematicReimportReceipt>::failure(parse_error(
            "reimport receipt text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kReceiptMagic) {
        return core::Result<KinematicReimportReceipt>::failure(parse_error(
            "reimport receipt framing is invalid"));
    }
    if (!read_integer(input, version)) {
        return core::Result<KinematicReimportReceipt>::failure(parse_error(
            "reimport receipt schema version is missing"));
    }
    if (version != kSchemaVersion) {
        return core::Result<KinematicReimportReceipt>::failure(version_error(
            "reimport receipt schema version is not supported"));
    }

    KinematicReimportReceipt receipt;
    std::uint64_t revision_before = 0U;
    std::uint64_t revision_after = 0U;
    if (!require_token(input, "REVISION") ||
        !read_integer(input, revision_before) ||
        !read_integer(input, revision_after) ||
        !require_token(input, "MESH") ||
        !read_integer(input, receipt.mesh_asset) ||
        !read_digest(input, receipt.mesh_digest_before) ||
        !read_digest(input, receipt.mesh_digest_after) ||
        !read_ids(input, "PRESERVED", kMaxJoints, receipt.preserved_joints) ||
        !read_ids(input, "REBOUND", kMaxJoints, receipt.rebound_joints) ||
        !read_ids(input, "REMOVED", kMaxJoints, receipt.removed_joints) ||
        !read_ids(input, "ADDED", kMaxJoints, receipt.added_joints) ||
        !require_token(input, "DIGEST") ||
        !read_digest(input, receipt.receipt_digest)) {
        return core::Result<KinematicReimportReceipt>::failure(parse_error(
            "reimport receipt record is malformed"));
    }
    receipt.revision_before = core::Revision{revision_before};
    receipt.revision_after = core::Revision{revision_after};

    if (!require_token(input, "END")) {
        return core::Result<KinematicReimportReceipt>::failure(parse_error(
            "reimport receipt is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<KinematicReimportReceipt>::failure(parse_error(
            "reimport receipt contains trailing unbound data"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<KinematicReimportReceipt>::failure(result.error());
    }
    return core::Result<KinematicReimportReceipt>::success(std::move(receipt));
}

core::Result<ReimportReconciliation> reconcile_reimport(
    const JointCorrespondence& before,
    const KinematicGraph& current,
    core::Revision new_mesh_revision,
    const assets::Sha256Digest& new_mesh_digest) {
    if (auto result = before.validate(); !result) {
        return core::Result<ReimportReconciliation>::failure(result.error());
    }
    if (auto result = current.validate(); !result) {
        return core::Result<ReimportReconciliation>::failure(result.error());
    }
    if (new_mesh_digest.is_zero()) {
        return core::Result<ReimportReconciliation>::failure(invalid(
            "reimport must name the new mesh content digest"));
    }
    if (new_mesh_revision < before.mesh_revision) {
        return core::Result<ReimportReconciliation>::failure(invalid(
            "reimport mesh revision must not regress"));
    }
    if (new_mesh_revision == before.mesh_revision &&
        !(before.mesh_digest == new_mesh_digest)) {
        return core::Result<ReimportReconciliation>::failure(stale(
            "mesh content changed at an unchanged revision"));
    }

    ReimportReconciliation reconciliation;
    KinematicReimportReceipt& receipt = reconciliation.receipt;
    receipt.revision_before = before.mesh_revision;
    receipt.revision_after = new_mesh_revision;
    receipt.mesh_asset = before.mesh_asset;
    receipt.mesh_digest_before = before.mesh_digest;
    receipt.mesh_digest_after = new_mesh_digest;

    JointCorrespondence& after = reconciliation.correspondence;
    after.source_namespace = before.source_namespace;
    after.mesh_asset = before.mesh_asset;
    after.mesh_revision = new_mesh_revision;
    after.mesh_digest = new_mesh_digest;

    const bool advance = new_mesh_revision > before.mesh_revision;
    for (const auto& [joint, record] : before.joints()) {
        const KinematicNode* current_node = current.find(joint);
        if (current_node == nullptr) {
            receipt.removed_joints.push_back(joint);
            continue;
        }
        const bool same_parent =
            (!record.parent.has_value() && !current_node->parent.has_value()) ||
            (record.parent.has_value() && current_node->parent.has_value() &&
                record.parent.value() == current_node->parent.value());
        JointIdentityRecord updated = record;
        if (same_parent && same_transform(record.authored_rest, current_node->rest)) {
            receipt.preserved_joints.push_back(joint);
        } else {
            receipt.rebound_joints.push_back(joint);
            updated.parent = current_node->parent;
            updated.authored_rest = current_node->rest;
        }
        if (advance) {
            MeshRevisionBinding binding;
            binding.mesh_revision = new_mesh_revision;
            binding.mesh_digest = new_mesh_digest;
            binding.surface_anchor = record.bindings.empty()
                ? 0U
                : record.bindings.back().surface_anchor;
            binding.binding_rest = current_node->rest;
            updated.bindings.push_back(std::move(binding));
        }
        if (auto inserted = after.insert(std::move(updated)); !inserted) {
            return core::Result<ReimportReconciliation>::failure(inserted.error());
        }
    }

    for (const auto& [joint, node] : current.nodes()) {
        if (before.find(joint) != nullptr) continue;
        if (!safe_text(node.semantic_type, JointCorrespondence::kMaxStableNameBytes)) {
            return core::Result<ReimportReconciliation>::failure(invalid(
                "joint added by reimport lacks a stable authored name"));
        }
        JointIdentityRecord record;
        record.joint = joint;
        record.stable_name = node.semantic_type;
        record.parent = node.parent;
        record.authored_rest = node.rest;
        if (advance) {
            MeshRevisionBinding binding;
            binding.mesh_revision = new_mesh_revision;
            binding.mesh_digest = new_mesh_digest;
            binding.surface_anchor = 0U;
            binding.binding_rest = node.rest;
            record.bindings.push_back(std::move(binding));
        }
        receipt.added_joints.push_back(joint);
        if (auto inserted = after.insert(std::move(record)); !inserted) {
            return core::Result<ReimportReconciliation>::failure(inserted.error());
        }
    }

    if (auto refreshed = receipt.refresh_digest(); !refreshed) {
        return core::Result<ReimportReconciliation>::failure(refreshed.error());
    }
    return core::Result<ReimportReconciliation>::success(
        std::move(reconciliation));
}

} // namespace carto::kinematic
