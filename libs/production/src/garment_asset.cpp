#include <carto/production/garment_asset.hpp>

#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace carto::production {
namespace {

constexpr std::string_view kAssetMagicV1 = "CARTO_GARMENT_ASSET 1";
constexpr std::string_view kAssetMagicV2 = "CARTO_GARMENT_ASSET 2";
constexpr std::size_t kMaxGarmentIdentityBytes = 128U;
constexpr std::size_t kMaxSourceIdentityBytes = 1024U;
constexpr std::size_t kMaxPanelFaces = 1'000'000U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU;
        });
}

std::string encode_hex(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2U);
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        encoded.push_back(digits[byte >> 4U]);
        encoded.push_back(digits[byte & 0x0fU]);
    }
    return encoded;
}

std::optional<std::string> decode_hex(std::string_view value, std::size_t maximum_bytes) {
    if (value.size() % 2U != 0U || value.size() / 2U > maximum_bytes) return std::nullopt;
    const auto nibble = [](char character) -> std::optional<std::uint8_t> {
        if (character >= '0' && character <= '9') {
            return static_cast<std::uint8_t>(character - '0');
        }
        if (character >= 'a' && character <= 'f') {
            return static_cast<std::uint8_t>(10 + character - 'a');
        }
        return std::nullopt;
    };
    std::string decoded;
    decoded.reserve(value.size() / 2U);
    for (std::size_t index = 0U; index < value.size(); index += 2U) {
        const auto high = nibble(value[index]);
        const auto low = nibble(value[index + 1U]);
        if (!high || !low) return std::nullopt;
        decoded.push_back(static_cast<char>((*high << 4U) | *low));
    }
    return decoded;
}

std::string hex_u64(std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded(16U, '0');
    for (std::size_t index = 0U; index < encoded.size(); ++index) {
        const std::size_t shift = (encoded.size() - index - 1U) * 4U;
        encoded[index] = digits[(value >> shift) & 0x0fU];
    }
    return encoded;
}

std::string hex_f64(double value) {
    return hex_u64(std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
}

std::optional<double> parse_f64_hex(std::string_view value) {
    if (value.size() != 16U) return std::nullopt;
    std::uint64_t bits = 0U;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), bits, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    const double result = std::bit_cast<double>(bits);
    if (!std::isfinite(result)) return std::nullopt;
    return result;
}

std::optional<std::uint64_t> read_u64(std::istringstream& record) {
    std::string token;
    if (!(record >> token)) return std::nullopt;
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
        return std::nullopt;
    }
    return value;
}

bool record_finished(std::istringstream& record) {
    std::string token;
    return !(record >> token);
}

std::optional<std::string> read_line(std::istringstream& input) {
    std::string line;
    if (!std::getline(input, line)) return std::nullopt;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

std::optional<std::istringstream> read_record(
    std::istringstream& input,
    std::string_view expected_tag) {
    const auto line = read_line(input);
    if (!line) return std::nullopt;
    std::istringstream record(*line);
    std::string tag;
    if (!(record >> tag) || tag != expected_tag) return std::nullopt;
    return record;
}

std::pair<geometry::VertexId, geometry::VertexId> edge_key(
    geometry::VertexId first,
    geometry::VertexId second) {
    return first < second ? std::pair{first, second} : std::pair{second, first};
}

bool ordered_identities(const std::vector<std::string>& identities) {
    return std::is_sorted(identities.begin(), identities.end()) &&
        std::adjacent_find(identities.begin(), identities.end()) == identities.end();
}

} // namespace

core::Result<void> GarmentFabricIntent::validate() const {
    if (!std::isfinite(areal_density) || areal_density <= 0.0 || areal_density > 1000.0 ||
        !std::isfinite(stretch_compliance) || stretch_compliance < 0.0 ||
        stretch_compliance > 1.0 || !std::isfinite(shear_compliance) ||
        shear_compliance < 0.0 || shear_compliance > 1.0 ||
        !std::isfinite(bend_compliance) || bend_compliance < 0.0 ||
        bend_compliance > 1.0 || !std::isfinite(damping) || damping < 0.0 ||
        damping >= 1.0 || !std::isfinite(thickness) || thickness <= 0.0 ||
        thickness > 1.0) {
        return core::Result<void>::failure(invalid(
            "GarmentAsset fabric intent is outside the supported physical bounds"));
    }
    return core::Result<void>::success();
}

core::Result<void> GarmentAsset::validate() const {
    if ((format_version != 1U && format_version != kGarmentAssetVersion) ||
        (format_version == 1U && !attachments.empty()) ||
        !safe_text(identity, kMaxGarmentIdentityBytes) ||
        !safe_text(project_namespace, kMaxGarmentIdentityBytes) || mesh_asset == 0U ||
        source.identity != garment_mesh_source_identity(project_namespace, mesh_asset)) {
        return core::Result<void>::failure(invalid(
            "GarmentAsset identity, namespace, mesh binding, or version is invalid"));
    }
    if (auto result = source.validate(); !result) return result;
    if (auto result = fabric.validate(); !result) return result;
    if (panels.empty() || panels.size() > kMaxGarmentPanels ||
        seams.size() > kMaxGarmentSeams ||
        collision_capsules.size() > kMaxGarmentCollisionCapsules ||
        pins.size() > kMaxGarmentPins || attachments.size() > kMaxGarmentAttachments) {
        return core::Result<void>::failure(invalid(
            "GarmentAsset panel, seam, pin, or attachment count is outside its bound"));
    }
    std::vector<std::string> panel_ids;
    std::vector<std::string> seam_ids;
    std::vector<std::string> collision_ids;
    std::vector<std::string> pin_ids;
    std::vector<std::string> attachment_ids;
    panel_ids.reserve(panels.size());
    seam_ids.reserve(seams.size());
    collision_ids.reserve(collision_capsules.size());
    pin_ids.reserve(pins.size());
    attachment_ids.reserve(attachments.size());
    std::uint64_t total_panel_faces = 0U;
    for (const GarmentPanel& panel : panels) {
        if (!safe_text(panel.identity, kMaxGarmentIdentityBytes) || panel.faces.empty() ||
            panel.faces.size() > kMaxPanelFaces ||
            !std::is_sorted(panel.faces.begin(), panel.faces.end()) ||
            std::adjacent_find(panel.faces.begin(), panel.faces.end()) != panel.faces.end() ||
            std::any_of(panel.faces.begin(), panel.faces.end(),
                [](geometry::FaceId face) { return !face; })) {
            return core::Result<void>::failure(validation(
                "GarmentAsset panel identity or face list is invalid"));
        }
        panel_ids.push_back(panel.identity);
        total_panel_faces += static_cast<std::uint64_t>(panel.faces.size());
    }
    if (!ordered_identities(panel_ids)) {
        return core::Result<void>::failure(validation(
            "GarmentAsset panel identities must be unique and sorted"));
    }
    std::set<std::string> all_identities(panel_ids.begin(), panel_ids.end());
    std::set<geometry::FaceId> panel_face_ids;
    for (const GarmentPanel& panel : panels) {
        for (const geometry::FaceId face : panel.faces) {
            if (!panel_face_ids.insert(face).second) {
                return core::Result<void>::failure(validation(
                    "GarmentAsset face may belong to only one panel"));
            }
        }
    }
    std::set<std::pair<geometry::VertexId, geometry::VertexId>> seam_pairs;
    for (const GarmentSeam& seam : seams) {
        if (!safe_text(seam.identity, kMaxGarmentIdentityBytes) ||
            seam.first_chain.size() < 2U || seam.first_chain.size() != seam.second_chain.size() ||
            seam.first_chain.size() > kMaxGarmentChainVertices ||
            !std::isfinite(seam.compliance) || seam.compliance < 0.0 || seam.compliance > 1.0) {
            return core::Result<void>::failure(validation(
                "GarmentAsset seam identity, chains, or compliance is invalid"));
        }
        seam_ids.push_back(seam.identity);
        if (!all_identities.insert(seam.identity).second) {
            return core::Result<void>::failure(validation(
                "GarmentAsset source record identities must be globally unique"));
        }
        std::set<geometry::VertexId> first_seen;
        std::set<geometry::VertexId> second_seen;
        for (std::size_t i = 0U; i < seam.first_chain.size(); ++i) {
            if (!seam.first_chain[i] || !seam.second_chain[i] ||
                seam.first_chain[i] == seam.second_chain[i] ||
                !first_seen.insert(seam.first_chain[i]).second ||
                !second_seen.insert(seam.second_chain[i]).second ||
                !seam_pairs.insert(edge_key(seam.first_chain[i], seam.second_chain[i])).second) {
                return core::Result<void>::failure(validation(
                    "GarmentAsset seam vertex correspondence is invalid or duplicated"));
            }
        }
    }
    if (!ordered_identities(seam_ids)) {
        return core::Result<void>::failure(validation(
            "GarmentAsset seam identities must be unique and sorted"));
    }
    for (const GarmentCollisionCapsule& capsule : collision_capsules) {
        if (!safe_text(capsule.identity, kMaxGarmentIdentityBytes) ||
            !capsule.start.finite() || !capsule.end.finite() ||
            std::abs(capsule.start.x) > 1e9 || std::abs(capsule.start.y) > 1e9 ||
            std::abs(capsule.start.z) > 1e9 || std::abs(capsule.end.x) > 1e9 ||
            std::abs(capsule.end.y) > 1e9 || std::abs(capsule.end.z) > 1e9 ||
            !std::isfinite(capsule.radius) || capsule.radius <= 0.0 || capsule.radius > 1000.0) {
            return core::Result<void>::failure(validation(
                "GarmentAsset collision capsule identity or dimensions are invalid"));
        }
        collision_ids.push_back(capsule.identity);
        if (!all_identities.insert(capsule.identity).second) {
            return core::Result<void>::failure(validation(
                "GarmentAsset source record identities must be globally unique"));
        }
    }
    if (!ordered_identities(collision_ids)) {
        return core::Result<void>::failure(validation(
            "GarmentAsset collision capsule identities must be unique and sorted"));
    }
    std::set<geometry::VertexId> pinned_vertices;
    for (const GarmentPin& pin : pins) {
        if (!safe_text(pin.identity, kMaxGarmentIdentityBytes) || !pin.vertex ||
            !pinned_vertices.insert(pin.vertex).second ||
            !pin.target_position.finite() || std::abs(pin.target_position.x) > 1e9 ||
            std::abs(pin.target_position.y) > 1e9 || std::abs(pin.target_position.z) > 1e9 ||
            !std::isfinite(pin.compliance) || pin.compliance < 0.0 || pin.compliance > 1.0) {
            return core::Result<void>::failure(validation(
                "GarmentAsset pin identity, target, or compliance is invalid"));
        }
        pin_ids.push_back(pin.identity);
        if (!all_identities.insert(pin.identity).second) {
            return core::Result<void>::failure(validation(
                "GarmentAsset source record identities must be globally unique"));
        }
    }
    if (!ordered_identities(pin_ids)) {
        return core::Result<void>::failure(validation(
            "GarmentAsset pin identities must be unique and sorted"));
    }
    for (const GarmentAttachment& attachment : attachments) {
        if (!safe_text(attachment.identity, kMaxGarmentIdentityBytes) ||
            !safe_text(attachment.target_identity, kMaxGarmentIdentityBytes) ||
            !attachment.vertex || !pinned_vertices.insert(attachment.vertex).second ||
            !attachment.target_position.finite() ||
            std::abs(attachment.target_position.x) > 1e9 ||
            std::abs(attachment.target_position.y) > 1e9 ||
            std::abs(attachment.target_position.z) > 1e9 ||
            !std::isfinite(attachment.compliance) || attachment.compliance < 0.0 ||
            attachment.compliance > 1.0) {
            return core::Result<void>::failure(validation(
                "GarmentAsset attachment identity, target, vertex, or compliance is invalid"));
        }
        attachment_ids.push_back(attachment.identity);
        if (!all_identities.insert(attachment.identity).second) {
            return core::Result<void>::failure(validation(
                "GarmentAsset source record identities must be globally unique"));
        }
    }
    if (!ordered_identities(attachment_ids)) {
        return core::Result<void>::failure(validation(
            "GarmentAsset attachment identities must be unique and sorted"));
    }
    if (total_panel_faces > kMaxPanelFaces) {
        return core::Result<void>::failure(invalid(
            "GarmentAsset panel assignments exceed the total face bound"));
    }
    return core::Result<void>::success();
}

core::Result<void> GarmentAsset::validate(
    const geometry::EditableMesh& source_mesh) const {
    if (auto result = validate(); !result) return result;
    if (auto result = source_mesh.validate(); !result) return result;
    const auto current = bind_bake_source(source.identity, source_mesh);
    if (!current) return core::Result<void>::failure(current.error());
    if (current.value() != source) {
        return core::Result<void>::failure(stale(
            "GarmentAsset source mesh revision or content digest is stale"));
    }
    std::set<geometry::FaceId> source_face_ids;
    for (const geometry::Face& face : source_mesh.faces_sorted()) {
        source_face_ids.insert(face.id);
    }
    std::set<geometry::FaceId> assigned_faces;
    for (const GarmentPanel& panel : panels) {
        for (const geometry::FaceId face : panel.faces) {
            if (!source_face_ids.contains(face) || !assigned_faces.insert(face).second) {
                return core::Result<void>::failure(validation(
                    "GarmentAsset panels contain a missing or multiply assigned face"));
            }
        }
    }
    if (assigned_faces != source_face_ids) {
        return core::Result<void>::failure(validation(
            "GarmentAsset panels must assign every source face exactly once"));
    }
    const auto topology = source_mesh.topology();
    if (!topology) return core::Result<void>::failure(topology.error());
    std::set<geometry::VertexId> source_vertices;
    for (const geometry::Vertex& vertex : source_mesh.vertices_sorted()) {
        source_vertices.insert(vertex.id);
    }
    std::set<std::pair<geometry::VertexId, geometry::VertexId>> boundary_edges;
    for (const geometry::EdgeRecord& edge : topology.value().edges) {
        if (!edge.second_half_edge.has_value()) {
            boundary_edges.insert(edge_key(edge.first, edge.second));
        }
    }
    for (const GarmentSeam& seam : seams) {
        for (std::size_t i = 0U; i < seam.first_chain.size(); ++i) {
            const geometry::VertexId first = seam.first_chain[i];
            const geometry::VertexId second = seam.second_chain[i];
            if (!source_vertices.contains(first) || !source_vertices.contains(second)) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::not_found,
                    "GarmentAsset seam references a missing source vertex"));
            }
            if (i > 0U && (!boundary_edges.contains(edge_key(seam.first_chain[i - 1U], first)) ||
                !boundary_edges.contains(edge_key(seam.second_chain[i - 1U], second)))) {
                return core::Result<void>::failure(validation(
                    "GarmentAsset seam chains must follow source boundary edges"));
            }
        }
    }
    for (const GarmentPin& pin : pins) {
        if (!source_vertices.contains(pin.vertex)) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::not_found, "GarmentAsset pin references a missing source vertex"));
        }
    }
    for (const GarmentAttachment& attachment : attachments) {
        if (!source_vertices.contains(attachment.vertex)) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "GarmentAsset attachment references a missing source vertex"));
        }
    }
    return core::Result<void>::success();
}

std::string GarmentAsset::serialize() const {
    if (!validate()) return {};
    std::ostringstream output;
    output << (format_version == 1U ? kAssetMagicV1 : kAssetMagicV2) << '\n';
    output << "IDENTITY " << encode_hex(identity) << '\n';
    output << "PROJECT " << encode_hex(project_namespace) << '\n';
    output << "MESH " << mesh_asset << '\n';
    output << "SOURCE " << encode_hex(source.identity) << ' ' << source.revision.value() << ' '
           << source.content_digest.hex() << '\n';
    output << "FABRIC " << hex_f64(fabric.areal_density) << ' '
           << hex_f64(fabric.stretch_compliance) << ' ' << hex_f64(fabric.shear_compliance) << ' '
           << hex_f64(fabric.bend_compliance) << ' ' << hex_f64(fabric.damping) << ' '
           << hex_f64(fabric.thickness) << '\n';
    output << "PANELS " << panels.size() << '\n';
    for (const GarmentPanel& panel : panels) {
        output << "PANEL " << encode_hex(panel.identity) << ' ' << panel.faces.size();
        for (const geometry::FaceId face : panel.faces) output << ' ' << face.value;
        output << '\n';
    }
    output << "SEAMS " << seams.size() << '\n';
    for (const GarmentSeam& seam : seams) {
        output << "SEAM " << encode_hex(seam.identity) << ' ' << seam.first_chain.size() << ' '
               << hex_f64(seam.compliance);
        for (const geometry::VertexId vertex : seam.first_chain) output << ' ' << vertex.value;
        for (const geometry::VertexId vertex : seam.second_chain) output << ' ' << vertex.value;
        output << '\n';
    }
    output << "COLLISION_CAPSULES " << collision_capsules.size() << '\n';
    for (const GarmentCollisionCapsule& capsule : collision_capsules) {
        output << "COLLISION_CAPSULE " << encode_hex(capsule.identity) << ' '
               << hex_f64(capsule.start.x) << ' ' << hex_f64(capsule.start.y) << ' '
               << hex_f64(capsule.start.z) << ' ' << hex_f64(capsule.end.x) << ' '
               << hex_f64(capsule.end.y) << ' ' << hex_f64(capsule.end.z) << ' '
               << hex_f64(capsule.radius) << '\n';
    }
    output << "PINS " << pins.size() << '\n';
    for (const GarmentPin& pin : pins) {
        output << "PIN " << encode_hex(pin.identity) << ' ' << pin.vertex.value << ' '
               << hex_f64(pin.target_position.x) << ' ' << hex_f64(pin.target_position.y) << ' '
               << hex_f64(pin.target_position.z) << ' ' << hex_f64(pin.compliance) << '\n';
    }
    if (format_version >= 2U) {
        output << "ATTACHMENTS " << attachments.size() << '\n';
        for (const GarmentAttachment& attachment : attachments) {
            output << "ATTACHMENT " << encode_hex(attachment.identity) << ' '
                   << encode_hex(attachment.target_identity) << ' ' << attachment.vertex.value << ' '
                   << hex_f64(attachment.target_position.x) << ' '
                   << hex_f64(attachment.target_position.y) << ' '
                   << hex_f64(attachment.target_position.z) << ' '
                   << hex_f64(attachment.compliance) << '\n';
        }
    }
    output << "END_GARMENT_ASSET\n";
    const std::string serialized = output.str();
    return serialized.size() <= kMaxGarmentAssetBytes ? serialized : std::string{};
}

core::Result<assets::Sha256Digest> GarmentAsset::digest() const {
    const std::string bytes = serialize();
    if (bytes.empty()) {
        return core::Result<assets::Sha256Digest>::failure(validation(
            "cannot digest an invalid or oversized GarmentAsset"));
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>(data, bytes.size())));
}

core::Result<GarmentAsset> GarmentAsset::deserialize(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxGarmentAssetBytes) {
        return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset is empty or exceeds its size bound"));
    }
    std::istringstream input{std::string(bytes)};
    const auto magic = read_line(input);
    if (!magic || (*magic != kAssetMagicV1 && *magic != kAssetMagicV2)) {
        return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset magic or version is invalid"));
    }
    GarmentAsset asset;
    asset.format_version = *magic == kAssetMagicV1 ? 1U : 2U;
    auto identity_record = read_record(input, "IDENTITY");
    std::string encoded_identity;
    if (!identity_record || !(*identity_record >> encoded_identity) ||
        !record_finished(*identity_record)) {
        return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset identity record is invalid"));
    }
    const auto identity = decode_hex(encoded_identity, kMaxGarmentIdentityBytes);
    if (!identity) return core::Result<GarmentAsset>::failure(validation(
        "serialized GarmentAsset identity encoding is invalid"));
    asset.identity = *identity;

    auto project_record = read_record(input, "PROJECT");
    std::string encoded_project;
    if (!project_record || !(*project_record >> encoded_project) ||
        !record_finished(*project_record)) {
        return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset project record is invalid"));
    }
    const auto project_namespace = decode_hex(encoded_project, kMaxGarmentIdentityBytes);
    if (!project_namespace) return core::Result<GarmentAsset>::failure(validation(
        "serialized GarmentAsset project namespace encoding is invalid"));
    asset.project_namespace = *project_namespace;

    auto mesh_record = read_record(input, "MESH");
    const auto mesh_asset = mesh_record ? read_u64(*mesh_record) : std::nullopt;
    if (!mesh_record || !mesh_asset || !record_finished(*mesh_record)) {
        return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset mesh record is invalid"));
    }
    asset.mesh_asset = *mesh_asset;

    auto source_record = read_record(input, "SOURCE");
    std::string source_identity_hex;
    const auto source_revision_identity = source_record ?
        static_cast<bool>(*source_record >> source_identity_hex) : false;
    const auto source_revision = source_record && source_revision_identity ?
        read_u64(*source_record) : std::nullopt;
    std::string source_digest_hex;
    if (!source_record || !source_revision || !(*source_record >> source_digest_hex) ||
        !record_finished(*source_record)) {
        return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset source record is invalid"));
    }
    const auto source_identity = decode_hex(source_identity_hex, kMaxSourceIdentityBytes);
    const auto source_digest = assets::Sha256Digest::from_hex(source_digest_hex);
    if (!source_identity || !source_digest || source_digest.value().is_zero()) {
        return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset source identity or digest is invalid"));
    }
    asset.source = BakeMeshBinding{
        *source_identity, core::Revision(*source_revision), source_digest.value()};

    auto fabric_record = read_record(input, "FABRIC");
    std::array<std::string, 6> fabric_values{};
    if (!fabric_record) return core::Result<GarmentAsset>::failure(validation(
        "serialized GarmentAsset fabric record is missing"));
    for (std::string& value : fabric_values) {
        if (!(*fabric_record >> value)) return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset fabric record is incomplete"));
    }
    if (!record_finished(*fabric_record)) return core::Result<GarmentAsset>::failure(validation(
        "serialized GarmentAsset fabric record has trailing data"));
    std::array<double, 6> parsed_fabric{};
    for (std::size_t i = 0U; i < parsed_fabric.size(); ++i) {
        const auto value = parse_f64_hex(fabric_values[i]);
        if (!value) return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset fabric numeric encoding is invalid"));
        parsed_fabric[i] = *value;
    }
    asset.fabric = {parsed_fabric[0], parsed_fabric[1], parsed_fabric[2],
        parsed_fabric[3], parsed_fabric[4], parsed_fabric[5]};

    auto panels_record = read_record(input, "PANELS");
    const auto panel_count = panels_record ? read_u64(*panels_record) : std::nullopt;
    if (!panels_record || !panel_count || !record_finished(*panels_record) ||
        *panel_count > kMaxGarmentPanels) {
        return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset panel count is invalid"));
    }
    asset.panels.reserve(static_cast<std::size_t>(*panel_count));
    std::uint64_t total_faces = 0U;
    for (std::uint64_t i = 0U; i < *panel_count; ++i) {
        auto panel_record = read_record(input, "PANEL");
        std::string panel_identity_hex;
        const bool panel_has_identity = panel_record &&
            static_cast<bool>(*panel_record >> panel_identity_hex);
        const auto face_count = panel_record && panel_has_identity ?
            read_u64(*panel_record) : std::nullopt;
        if (!panel_record || !panel_has_identity || !face_count ||
            *face_count == 0U || *face_count > kMaxPanelFaces ||
            total_faces > kMaxPanelFaces - *face_count) {
            return core::Result<GarmentAsset>::failure(validation(
                "serialized GarmentAsset panel record is invalid"));
        }
        const auto panel_identity = decode_hex(panel_identity_hex, kMaxGarmentIdentityBytes);
        if (!panel_identity) return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset panel identity encoding is invalid"));
        GarmentPanel panel;
        panel.identity = *panel_identity;
        panel.faces.reserve(static_cast<std::size_t>(*face_count));
        for (std::uint64_t face = 0U; face < *face_count; ++face) {
            const auto face_id = read_u64(*panel_record);
            if (!face_id || *face_id == 0U) return core::Result<GarmentAsset>::failure(validation(
                "serialized GarmentAsset panel face identity is invalid"));
            panel.faces.push_back(geometry::FaceId{*face_id});
        }
        if (!record_finished(*panel_record)) return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset panel record has trailing data"));
        total_faces += *face_count;
        asset.panels.push_back(std::move(panel));
    }

    auto seams_record = read_record(input, "SEAMS");
    const auto seam_count = seams_record ? read_u64(*seams_record) : std::nullopt;
    if (!seams_record || !seam_count || !record_finished(*seams_record) ||
        *seam_count > kMaxGarmentSeams) {
        return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset seam count is invalid"));
    }
    asset.seams.reserve(static_cast<std::size_t>(*seam_count));
    for (std::uint64_t i = 0U; i < *seam_count; ++i) {
        auto seam_record = read_record(input, "SEAM");
        std::string seam_identity_hex;
        const bool seam_has_identity = seam_record &&
            static_cast<bool>(*seam_record >> seam_identity_hex);
        const auto chain_count = seam_record && seam_has_identity ?
            read_u64(*seam_record) : std::nullopt;
        std::string compliance_hex;
        if (!seam_record || !seam_has_identity ||
            !(*seam_record >> compliance_hex) ||
            !chain_count || *chain_count < 2U || *chain_count > kMaxGarmentChainVertices) {
            return core::Result<GarmentAsset>::failure(validation(
                "serialized GarmentAsset seam record is invalid"));
        }
        const auto seam_identity = decode_hex(seam_identity_hex, kMaxGarmentIdentityBytes);
        const auto compliance = parse_f64_hex(compliance_hex);
        if (!seam_identity || !compliance) return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset seam identity or compliance is invalid"));
        GarmentSeam seam;
        seam.identity = *seam_identity;
        seam.compliance = *compliance;
        seam.first_chain.reserve(static_cast<std::size_t>(*chain_count));
        seam.second_chain.reserve(static_cast<std::size_t>(*chain_count));
        for (std::uint64_t vertex = 0U; vertex < *chain_count * 2U; ++vertex) {
            const auto vertex_id = read_u64(*seam_record);
            if (!vertex_id || *vertex_id == 0U) return core::Result<GarmentAsset>::failure(validation(
                "serialized GarmentAsset seam vertex identity is invalid"));
            auto& chain = vertex < *chain_count ? seam.first_chain : seam.second_chain;
            chain.push_back(geometry::VertexId{*vertex_id});
        }
        if (!record_finished(*seam_record)) return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset seam record has trailing data"));
        asset.seams.push_back(std::move(seam));
    }

    auto capsules_record = read_record(input, "COLLISION_CAPSULES");
    const auto capsule_count = capsules_record ? read_u64(*capsules_record) : std::nullopt;
    if (!capsules_record || !capsule_count || !record_finished(*capsules_record) ||
        *capsule_count > kMaxGarmentCollisionCapsules) {
        return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset collision capsule count is invalid"));
    }
    asset.collision_capsules.reserve(static_cast<std::size_t>(*capsule_count));
    for (std::uint64_t i = 0U; i < *capsule_count; ++i) {
        auto capsule_record = read_record(input, "COLLISION_CAPSULE");
        std::string capsule_identity_hex;
        std::array<std::string, 7> values{};
        if (!capsule_record || !(*capsule_record >> capsule_identity_hex)) {
            return core::Result<GarmentAsset>::failure(validation(
                "serialized GarmentAsset collision capsule record is invalid"));
        }
        for (std::string& value : values) {
            if (!(*capsule_record >> value)) return core::Result<GarmentAsset>::failure(validation(
                "serialized GarmentAsset collision capsule record is incomplete"));
        }
        if (!record_finished(*capsule_record)) {
            return core::Result<GarmentAsset>::failure(validation(
                "serialized GarmentAsset collision capsule record has trailing data"));
        }
        const auto capsule_identity = decode_hex(
            capsule_identity_hex, kMaxGarmentIdentityBytes);
        std::array<double, 7> parsed{};
        bool valid_values = true;
        for (std::size_t value = 0U; value < parsed.size(); ++value) {
            const auto decoded = parse_f64_hex(values[value]);
            if (!decoded) {
                valid_values = false;
                break;
            }
            parsed[value] = *decoded;
        }
        if (!capsule_identity || !valid_values) {
            return core::Result<GarmentAsset>::failure(validation(
                "serialized GarmentAsset collision capsule text or numeric encoding is invalid"));
        }
        asset.collision_capsules.push_back(GarmentCollisionCapsule{
            *capsule_identity, {parsed[0], parsed[1], parsed[2]},
            {parsed[3], parsed[4], parsed[5]}, parsed[6]});
    }

    auto pins_record = read_record(input, "PINS");
    const auto pin_count = pins_record ? read_u64(*pins_record) : std::nullopt;
    if (!pins_record || !pin_count || !record_finished(*pins_record) ||
        *pin_count > kMaxGarmentPins) {
        return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset pin count is invalid"));
    }
    asset.pins.reserve(static_cast<std::size_t>(*pin_count));
    for (std::uint64_t i = 0U; i < *pin_count; ++i) {
        auto pin_record = read_record(input, "PIN");
        std::string pin_identity_hex;
        const bool pin_has_identity = pin_record &&
            static_cast<bool>(*pin_record >> pin_identity_hex);
        const auto vertex = pin_record && pin_has_identity ?
            read_u64(*pin_record) : std::nullopt;
        std::array<std::string, 4> values{};
        if (!pin_record || !pin_has_identity || !vertex) {
            return core::Result<GarmentAsset>::failure(validation(
                "serialized GarmentAsset pin record is invalid"));
        }
        for (std::string& value : values) {
            if (!(*pin_record >> value)) return core::Result<GarmentAsset>::failure(validation(
                "serialized GarmentAsset pin record is incomplete"));
        }
        if (!record_finished(*pin_record)) return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset pin record has trailing data"));
        const auto pin_identity = decode_hex(pin_identity_hex, kMaxGarmentIdentityBytes);
        const auto x = parse_f64_hex(values[0]);
        const auto y = parse_f64_hex(values[1]);
        const auto z = parse_f64_hex(values[2]);
        const auto compliance = parse_f64_hex(values[3]);
        if (!pin_identity || !x || !y || !z || !compliance) {
            return core::Result<GarmentAsset>::failure(validation(
                "serialized GarmentAsset pin text or numeric encoding is invalid"));
        }
        asset.pins.push_back(GarmentPin{
            *pin_identity, geometry::VertexId{*vertex}, {*x, *y, *z}, *compliance});
    }
    if (asset.format_version >= 2U) {
        auto attachments_record = read_record(input, "ATTACHMENTS");
        const auto attachment_count = attachments_record ?
            read_u64(*attachments_record) : std::nullopt;
        if (!attachments_record || !attachment_count || !record_finished(*attachments_record) ||
            *attachment_count > kMaxGarmentAttachments) {
            return core::Result<GarmentAsset>::failure(validation(
                "serialized GarmentAsset attachment count is invalid"));
        }
        asset.attachments.reserve(static_cast<std::size_t>(*attachment_count));
        for (std::uint64_t i = 0U; i < *attachment_count; ++i) {
            auto attachment_record = read_record(input, "ATTACHMENT");
            std::string identity_hex;
            std::string target_identity_hex;
            const bool has_identities = attachment_record &&
                static_cast<bool>(*attachment_record >> identity_hex >> target_identity_hex);
            const auto vertex = attachment_record && has_identities ?
                read_u64(*attachment_record) : std::nullopt;
            std::array<std::string, 4> values{};
            if (!attachment_record || !has_identities || !vertex) {
                return core::Result<GarmentAsset>::failure(validation(
                    "serialized GarmentAsset attachment record is invalid"));
            }
            for (std::string& value : values) {
                if (!(*attachment_record >> value)) {
                    return core::Result<GarmentAsset>::failure(validation(
                        "serialized GarmentAsset attachment record is incomplete"));
                }
            }
            if (!record_finished(*attachment_record)) {
                return core::Result<GarmentAsset>::failure(validation(
                    "serialized GarmentAsset attachment record has trailing data"));
            }
            const auto attachment_identity = decode_hex(
                identity_hex, kMaxGarmentIdentityBytes);
            const auto target_identity = decode_hex(
                target_identity_hex, kMaxGarmentIdentityBytes);
            const auto x = parse_f64_hex(values[0]);
            const auto y = parse_f64_hex(values[1]);
            const auto z = parse_f64_hex(values[2]);
            const auto compliance = parse_f64_hex(values[3]);
            if (!attachment_identity || !target_identity || !x || !y || !z ||
                !compliance || *vertex == 0U) {
                return core::Result<GarmentAsset>::failure(validation(
                    "serialized GarmentAsset attachment text or numeric encoding is invalid"));
            }
            asset.attachments.push_back(GarmentAttachment{
                *attachment_identity, *target_identity, geometry::VertexId{*vertex},
                {*x, *y, *z}, *compliance});
        }
    }
    const auto terminator = read_line(input);
    if (!terminator || *terminator != "END_GARMENT_ASSET") {
        return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset terminator is missing"));
    }
    if (input >> std::ws, !input.eof()) return core::Result<GarmentAsset>::failure(validation(
        "serialized GarmentAsset contains trailing data"));
    if (auto result = asset.validate(); !result) {
        return core::Result<GarmentAsset>::failure(result.error());
    }
    if (asset.serialize() != bytes) {
        return core::Result<GarmentAsset>::failure(validation(
            "serialized GarmentAsset is not in canonical form"));
    }
    return core::Result<GarmentAsset>::success(std::move(asset));
}

std::string garment_mesh_source_identity(
    std::string_view project_namespace,
    std::uint64_t mesh_asset) {
    if (!safe_text(project_namespace, kMaxGarmentIdentityBytes) || mesh_asset == 0U) return {};
    return "carto.garment.mesh.v1/" + std::string(project_namespace) + "/" +
        std::to_string(mesh_asset);
}

core::Result<GarmentAsset> make_garment_asset(
    const project::ProjectDocument& document,
    std::uint64_t mesh_asset,
    std::string identity) {
    const auto mesh = document.meshes().find(mesh_asset);
    if (mesh == document.meshes().end()) {
        return core::Result<GarmentAsset>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "GarmentAsset target mesh asset does not exist"));
    }
    GarmentAsset asset;
    asset.identity = std::move(identity);
    asset.project_namespace = document.source_namespace();
    asset.mesh_asset = mesh_asset;
    const std::string source_identity = garment_mesh_source_identity(
        asset.project_namespace, mesh_asset);
    const auto binding = bind_bake_source(source_identity, mesh->second);
    if (!binding) return core::Result<GarmentAsset>::failure(binding.error());
    asset.source = binding.value();
    GarmentPanel panel;
    panel.identity = "panel.main";
    for (const geometry::Face& face : mesh->second.faces_sorted()) panel.faces.push_back(face.id);
    asset.panels.push_back(std::move(panel));
    if (auto result = asset.validate(mesh->second); !result) {
        return core::Result<GarmentAsset>::failure(result.error());
    }
    return core::Result<GarmentAsset>::success(std::move(asset));
}

core::Result<void> stage_garment_asset(
    project::ProjectTransaction& transaction,
    const GarmentAsset& asset,
    std::optional<assets::Sha256Digest> expected_current_digest) {
    const project::ProjectDocument& staged = transaction.staged_document();
    if (staged.source_namespace() != asset.project_namespace) {
        return core::Result<void>::failure(stale(
            "GarmentAsset belongs to another project namespace"));
    }
    const auto mesh = staged.meshes().find(asset.mesh_asset);
    if (mesh == staged.meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "GarmentAsset transaction mesh does not exist"));
    }
    if (auto result = asset.validate(mesh->second); !result) return result;
    const auto existing = staged.garment_assets().find(asset.mesh_asset);
    if (existing == staged.garment_assets().end()) {
        if (expected_current_digest.has_value()) {
            return core::Result<void>::failure(stale(
                "expected GarmentAsset digest is absent from the staged project"));
        }
    } else if (!expected_current_digest.has_value() ||
        existing->second.digest != *expected_current_digest) {
        return core::Result<void>::failure(stale(
            "updating an existing GarmentAsset requires its current digest"));
    }
    const std::string serialized = asset.serialize();
    const auto digest = asset.digest();
    if (serialized.empty() || !digest) {
        return core::Result<void>::failure(validation(
            "cannot stage an invalid or oversized GarmentAsset"));
    }
    return transaction.set_garment_asset(asset.mesh_asset, serialized, digest.value());
}

std::string garment_asset_transaction_operation(const assets::Sha256Digest& asset_digest) {
    if (asset_digest.is_zero()) return {};
    return "garment-asset.v1." + asset_digest.hex();
}

} // namespace carto::production
