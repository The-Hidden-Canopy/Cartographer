#include <carto/character/autorig_graph_record.hpp>

#include <algorithm>
#include <iomanip>
#include <locale>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <utility>

namespace carto::character {
namespace {
core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}
assets::Sha256Digest hash(std::string_view bytes) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}
bool token(std::istream& in, std::string_view expected) {
    std::string word;
    return static_cast<bool>(in >> word) && word == expected;
}
bool read_digest(std::istream& in, assets::Sha256Digest& value) {
    std::string hex;
    if (!(in >> hex)) return false;
    auto parsed = assets::Sha256Digest::from_hex(hex);
    if (!parsed) return false;
    value = parsed.value();
    return true;
}
}

core::Result<void> AcceptedAutorigGraphRecord::validate() const {
    if (auto result = acceptance.validate(); !result) return result;
    if (graph_bytes.empty() || graph_bytes.size() > kinematic::KinematicGraph::kMaxSerializedBytes ||
        graph_digest.is_zero() || hash(graph_bytes) != graph_digest)
        return core::Result<void>::failure(invalid("accepted autorig graph digest or size is invalid"));
    auto graph = kinematic::KinematicGraph::deserialize(graph_bytes);
    if (!graph || graph.value().serialize() != graph_bytes)
        return core::Result<void>::failure(invalid("accepted autorig graph is not canonical"));
    if (joints.empty() || joints.size() != graph.value().nodes().size() ||
        joints.size() > AutorigCandidatePolicy::kHardMaxJointsPerCandidate)
        return core::Result<void>::failure(invalid("accepted autorig mapping has invalid size"));
    std::map<AutorigCandidateJointId, kinematic::JointId> mappings;
    std::set<kinematic::JointId> durable;
    AutorigCandidateJointId previous{};
    for (const auto& mapping : joints) {
        if (!mapping.preview || !mapping.durable || mapping.preview <= previous ||
            !mappings.emplace(mapping.preview, mapping.durable).second ||
            !durable.insert(mapping.durable).second || !graph.value().find(mapping.durable))
            return core::Result<void>::failure(invalid("accepted autorig mapping is invalid or unsorted"));
        previous = mapping.preview;
    }
    for (const auto& mapping : joints) {
        const auto* node = graph.value().find(mapping.durable);
        if (!mapping.preview_parent) {
            if (node->parent) return core::Result<void>::failure(invalid("mapping root does not match graph"));
        } else {
            const auto parent = mappings.find(*mapping.preview_parent);
            if (parent == mappings.end() || !node->parent || *node->parent != parent->second)
                return core::Result<void>::failure(invalid("mapping parent does not match graph"));
        }
    }
    return core::Result<void>::success();
}

std::string AcceptedAutorigGraphRecord::serialize() const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    const auto& a = acceptance;
    out << "CARTO_ACCEPTED_AUTORIG_GRAPH 1\nSOURCE " << a.source.asset_id << ' '
        << a.source.content_digest.hex() << ' ' << a.mesh_revision.value() << ' '
        << a.mesh_geometry_digest.hex() << '\n';
    out << "REVIEW " << a.constraints_digest.hex() << ' ' << a.policy_digest.hex() << ' '
        << a.candidate_set_digest.hex() << ' ' << a.candidate_digest.hex() << ' '
        << a.digest.hex() << '\n';
    out << "DECISION " << static_cast<unsigned>(a.reviewer_decision.origin) << ' '
        << std::quoted(a.reviewer_decision.assertion_id) << ' '
        << std::quoted(a.reviewer_decision.author_id) << ' '
        << std::quoted(a.reviewer_decision.rationale) << ' '
        << std::quoted(a.reviewer_decision.evidence_reference) << '\n';
    out << "GRAPH " << graph_digest.hex() << ' ' << std::quoted(graph_bytes) << '\n';
    out << "JOINTS " << joints.size() << '\n';
    for (const auto& j : joints)
        out << "JOINT " << j.preview.value << ' '
            << (j.preview_parent ? j.preview_parent->value : 0U) << ' ' << j.durable.value << '\n';
    out << "END\n";
    return out.str();
}

core::Result<assets::Sha256Digest> AcceptedAutorigGraphRecord::canonical_digest() const {
    if (auto result = validate(); !result)
        return core::Result<assets::Sha256Digest>::failure(result.error());
    const auto bytes = serialize();
    if (bytes.size() > kMaxSerializedBytes)
        return core::Result<assets::Sha256Digest>::failure(invalid("accepted autorig record exceeds its size limit"));
    return core::Result<assets::Sha256Digest>::success(hash(bytes));
}

core::Result<AcceptedAutorigGraphRecord> AcceptedAutorigGraphRecord::deserialize(std::string_view bytes) {
    using Result = core::Result<AcceptedAutorigGraphRecord>;
    if (bytes.empty() || bytes.size() > kMaxSerializedBytes)
        return Result::failure(invalid("accepted autorig record exceeds its size limit"));
    std::istringstream in{std::string(bytes)};
    in.imbue(std::locale::classic());
    AcceptedAutorigGraphRecord record;
    auto& a = record.acceptance;
    unsigned version = 0U, origin = 0U;
    std::uint64_t revision = 0U;
    std::size_t count = 0U;
    if (!token(in, "CARTO_ACCEPTED_AUTORIG_GRAPH") || !(in >> version) || version != 1U ||
        !token(in, "SOURCE") || !(in >> a.source.asset_id) || !read_digest(in, a.source.content_digest) ||
        !(in >> revision) || !read_digest(in, a.mesh_geometry_digest) ||
        !token(in, "REVIEW") || !read_digest(in, a.constraints_digest) ||
        !read_digest(in, a.policy_digest) || !read_digest(in, a.candidate_set_digest) ||
        !read_digest(in, a.candidate_digest) || !read_digest(in, a.digest) ||
        !token(in, "DECISION") || !(in >> origin) || origin != 0U ||
        !(in >> std::quoted(a.reviewer_decision.assertion_id) >> std::quoted(a.reviewer_decision.author_id)
          >> std::quoted(a.reviewer_decision.rationale) >> std::quoted(a.reviewer_decision.evidence_reference)) ||
        !token(in, "GRAPH") || !read_digest(in, record.graph_digest) || !(in >> std::quoted(record.graph_bytes)) ||
        !token(in, "JOINTS") || !(in >> count) || count > AutorigCandidatePolicy::kHardMaxJointsPerCandidate)
        return Result::failure(invalid("accepted autorig record header is malformed"));
    a.mesh_revision = core::Revision{revision};
    a.reviewer_decision.origin = AutorigConstraintOrigin::human_authored;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t preview = 0U, parent = 0U, durable = 0U;
        if (!token(in, "JOINT") || !(in >> preview >> parent >> durable))
            return Result::failure(invalid("accepted autorig joint mapping is malformed"));
        record.joints.push_back({{preview}, parent ? std::optional{AutorigCandidateJointId{parent}} : std::nullopt, {durable}});
    }
    if (!token(in, "END")) return Result::failure(invalid("accepted autorig record is incomplete"));
    std::string trailing;
    if (in >> trailing) return Result::failure(invalid("accepted autorig record has trailing data"));
    if (auto checked = record.validate(); !checked) return Result::failure(checked.error());
    if (record.serialize() != bytes) return Result::failure(invalid("accepted autorig record is not canonical"));
    return Result::success(std::move(record));
}

core::Result<AcceptedAutorigGraphRecord> make_accepted_autorig_graph_record(
    const geometry::EditableMesh& mesh, const AutorigAnalysisRequest& request,
    const AutorigCandidateSet& candidates, const AutorigReviewAcceptance& acceptance,
    const std::vector<AutorigFinalNode>& nodes) {
    using Result = core::Result<AcceptedAutorigGraphRecord>;
    auto prepared = prepare_accepted_autorig_graph(mesh, request, candidates, acceptance, nodes);
    if (!prepared) return Result::failure(prepared.error());
    const auto candidate = std::find_if(candidates.ranked_candidates.begin(), candidates.ranked_candidates.end(),
        [&acceptance](const auto& item) { return item.digest == acceptance.candidate_digest; });
    std::map<AutorigCandidateJointId, kinematic::JointId> ids;
    for (const auto& node : nodes) ids.emplace(node.preview_joint, node.durable_joint);
    AcceptedAutorigGraphRecord record{acceptance, prepared.value().graph.serialize(), prepared.value().graph_digest, {}};
    for (const auto& joint : candidate->joints)
        record.joints.push_back({joint.id, joint.parent, ids.at(joint.id)});
    std::sort(record.joints.begin(), record.joints.end(), [](const auto& a, const auto& b) { return a.preview < b.preview; });
    if (auto checked = record.canonical_digest(); !checked) return Result::failure(checked.error());
    return Result::success(std::move(record));
}
} // namespace carto::character
