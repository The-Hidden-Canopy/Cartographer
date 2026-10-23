#include <carto/project/project.hpp>
#include <carto/biological/proxy_mesh.hpp>
#include <carto/character/rig_module.hpp>
#include <carto/mesh_attributes/provenance.hpp>
#include <carto/motion/curve_edit.hpp>
#include <carto/pose/pose_library.hpp>
#include <carto/weights/paint.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <sstream>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

#ifdef _WIN32
#    define NOMINMAX
#    include <windows.h>
#else
#    include <unistd.h>
#endif

namespace carto::project {

namespace {

using core::Diagnostic;
using core::ErrorCode;

Diagnostic invalid(std::string message) {
    return Diagnostic(ErrorCode::invalid_argument, std::move(message));
}

Diagnostic parse_error(std::string message) {
    return Diagnostic(ErrorCode::validation_failed, std::move(message));
}

core::Result<core::Revision> persistent_mesh_patch_revision(std::string_view bytes) {
    const auto vertex_patch = geometry::MeshPatch::deserialize(bytes);
    if (vertex_patch) {
        return core::Result<core::Revision>::success(
            vertex_patch.value().expected_revision);
    }
    const auto face_deletion = geometry::FaceDeletionPatch::deserialize(bytes);
    if (face_deletion) {
        return core::Result<core::Revision>::success(face_deletion.value().expected_revision);
    }
    const auto edge_split = geometry::EdgeSplitPatch::deserialize(bytes);
    if (edge_split) {
        return core::Result<core::Revision>::success(edge_split.value().expected_revision);
    }
    const auto edge_dissolve = geometry::EdgeDissolvePatch::deserialize(bytes);
    if (edge_dissolve) {
        return core::Result<core::Revision>::success(
            edge_dissolve.value().expected_revision);
    }
    const auto merge_by_distance = geometry::MergeByDistancePatch::deserialize(bytes);
    if (merge_by_distance) {
        return core::Result<core::Revision>::success(
            merge_by_distance.value().expected_revision);
    }
    const auto vertex_merge = geometry::VertexMergePatch::deserialize(bytes);
    if (vertex_merge) {
        return core::Result<core::Revision>::success(
            vertex_merge.value().expected_revision);
    }
    const auto knife_cut = geometry::KnifeCutPatch::deserialize(bytes);
    if (knife_cut) {
        return core::Result<core::Revision>::success(
            knife_cut.value().expected_revision);
    }
    const auto loop_cut = geometry::LoopCutPatch::deserialize(bytes);
    if (loop_cut) {
        return core::Result<core::Revision>::success(loop_cut.value().expected_revision);
    }
    const auto bevel_edge = geometry::BevelEdgePatch::deserialize(bytes);
    if (bevel_edge) {
        return core::Result<core::Revision>::success(bevel_edge.value().expected_revision);
    }
    const auto bevel_vertex = geometry::BevelVertexPatch::deserialize(bytes);
    if (bevel_vertex) {
        return core::Result<core::Revision>::success(bevel_vertex.value().expected_revision);
    }
    const auto face_extrusion = geometry::FaceExtrusionPatch::deserialize(bytes);
    if (face_extrusion) {
        return core::Result<core::Revision>::success(
            face_extrusion.value().expected_revision);
    }
    const auto face_inset = geometry::FaceInsetPatch::deserialize(bytes);
    if (face_inset) {
        return core::Result<core::Revision>::success(face_inset.value().expected_revision);
    }
    const auto face_poke = geometry::FacePokePatch::deserialize(bytes);
    if (face_poke) {
        return core::Result<core::Revision>::success(face_poke.value().expected_revision);
    }
    const auto tri_to_quad = geometry::TriToQuadPatch::deserialize(bytes);
    if (tri_to_quad) {
        return core::Result<core::Revision>::success(
            tri_to_quad.value().expected_revision);
    }
    return core::Result<core::Revision>::failure(parse_error(
        "persistent mesh patch bytes are not a supported canonical patch"));
}

Diagnostic exhausted_revision() {
    return Diagnostic(ErrorCode::invalid_state, "project revision space is exhausted");
}

core::Result<character::RigModuleTemplate> parse_rig_module_template(
    std::string_view bytes) {
    if (bytes.empty() || bytes.size() > character::RigModuleTemplate::kMaxSerializedBytes) {
        return core::Result<character::RigModuleTemplate>::failure(invalid(
            "rig-module template bytes are outside the supported bounds"));
    }
    auto decoded = character::RigModuleTemplate::deserialize(bytes);
    if (!decoded) return decoded;
    const auto canonical = decoded.value().serialize();
    if (!canonical || canonical.value() != bytes) {
        return core::Result<character::RigModuleTemplate>::failure(parse_error(
            "rig-module template bytes are not canonical"));
    }
    return decoded;
}

constexpr std::uint64_t kMaxSerializedObjects = 1'000'000U;
constexpr std::uint64_t kMaxSerializedMeshes = 1'000'000U;
constexpr std::uint64_t kMaxSerializedVertices = 1'000'000U;
constexpr std::uint64_t kMaxSerializedFaces = 1'000'000U;
constexpr std::uint64_t kMaxSerializedFaceVertices = 1'000'000U;
constexpr std::uint64_t kMaxSerializedTopologyReceipts = 1'000'000U;
constexpr std::uint64_t kMaxSerializedMeshPatchRecords = 100'000U;
constexpr std::uint64_t kMaxSerializedMeshPatchAggregateBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedAttributeDomains = 64U;
constexpr std::uint64_t kMaxSerializedAttributeLayers = 1'000'000U;
constexpr std::uint64_t kMaxSerializedAttributeValues = 1'000'000U;
constexpr std::uint64_t kMaxSerializedUvSets = 1'000'000U;
constexpr std::uint64_t kMaxSerializedSculptStacks = 100'000U;
constexpr std::uint64_t kMaxSerializedGarmentAssets = 100'000U;
constexpr std::uint64_t kMaxSerializedClothRestPoseReceipts = 100'000U;
constexpr std::uintmax_t kMaxSerializedProjectBytes = 128ULL * 1024ULL * 1024ULL;
// Mirrors carto::semantic::SemanticSidecar::kMaxSerializedBytes. The value is
// duplicated here so libs/project keeps no dependency on carto_semantic.
constexpr std::uint64_t kMaxSerializedSemanticSidecarBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedSculptStackBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedGarmentAssetBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedClothRestPoseReceiptBytes = 16ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedEngineeringAuthoringBytes =
    32ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedConditionAuthoringBytes =
    32ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedBiologicalAuthoringSources = 4'096U;
constexpr std::uint64_t kMaxSerializedBiologicalAuthoringBytes =
    biological::BiologicalAuthoringDocument::kMaxSerializedBytes;
constexpr std::uint64_t kMaxSerializedBiologicalAuthoringAggregateBytes =
    64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedBiologicalProxyInstanceReceipts = 4'096U;
constexpr std::uint64_t kMaxSerializedBiologicalProxyInstanceReceiptAggregateBytes =
    16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedConditionVegetationPlacementReceipts = 50'000U;
constexpr std::uint64_t kMaxSerializedRigModuleTemplates = 4'096U;
constexpr std::uint64_t kMaxSerializedRigModuleTemplateAggregateBytes =
    64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedDecalPlacements = 100'000U;
constexpr std::uint64_t kMaxSerializedDecalPlacementBytes = 1ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedDecalPlacementAggregateBytes =
    64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedPoseLibraries = 4'096U;
constexpr std::uint64_t kMaxSerializedPoseLibraryAggregateBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedWeightEditReceipts = 100'000U;
constexpr std::uint64_t kMaxSerializedWeightEditReceiptAggregateBytes =
    16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedCurveEditReceipts = 100'000U;
constexpr std::uint64_t kMaxSerializedCurveEditReceiptAggregateBytes =
    16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedCurvePaths = 4'096U;
constexpr std::uint64_t kMaxSerializedCurvePathBytes = 8ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedCurvePathAggregateBytes = 32ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedRoadSources = 4'096U;
constexpr std::uint64_t kMaxSerializedRoadSourceBytes = RoadSource::kMaxSerializedBytes;
constexpr std::uint64_t kMaxSerializedRoadSourceAggregateBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedCurvePathLineage = kMaxSerializedCurvePaths;
constexpr std::uint64_t kMaxSerializedRoadSourceLineage = kMaxSerializedRoadSources;
constexpr std::uint64_t kMaxSerializedSweepSources = 4'096U;
constexpr std::uint64_t kMaxSerializedSweepSourceBytes =
    geometry::SweepSource::kMaxSerializedBytes;
constexpr std::uint64_t kMaxSerializedSweepSourceAggregateBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedSweepSourceLineage = kMaxSerializedSweepSources;
constexpr std::uint64_t kMaxSerializedSignTextSources = 4'096U;
constexpr std::uint64_t kMaxSerializedSignTextSourceBytes =
    geometry::SignTextSource::kMaxTextBytes * 32ULL;
constexpr std::uint64_t kMaxSerializedSignTextSourceAggregateBytes =
    16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedSignTextSourceLineage =
    kMaxSerializedSignTextSources;
constexpr std::uint64_t kMaxSerializedSignTextInstanceReceipts = 4'096U;
constexpr std::uint64_t kMaxSerializedSignTextInstanceReceiptAggregateBytes =
    16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedRoadSurfaceInstanceReceipts = 4'096U;
constexpr std::uint64_t kMaxSerializedRoadSurfaceInstanceReceiptAggregateBytes =
    32ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSerializedSpatialFields = 512U;
constexpr std::uint64_t kMaxSerializedSpatialFieldLineage = 4096U;
constexpr std::uint64_t kMaxSerializedSpatialFieldBytes =
    spatial_field::ScalarField2D::kMaxSerializedBytes;
constexpr std::uint64_t kMaxSerializedSpatialFieldAggregateBytes = 32ULL * 1024ULL * 1024ULL;

const MaterialRegionAssignment* material_assignment_at(
    const MaterialCatalog& catalog,
    MaterialRegionKey key) {
    const auto found = catalog.assignments().find(key);
    return found == catalog.assignments().end() ? nullptr : &found->second;
}

bool same_material_assignment(
    const MaterialRegionAssignment* left,
    const MaterialRegionAssignment* right) noexcept {
    if (left == nullptr || right == nullptr) return left == right;
    return left->key == right->key && left->material == right->material &&
        left->variant == right->variant;
}

core::Result<void> copy_material_assignment(
    MaterialCatalog& destination,
    const MaterialCatalog& source,
    MaterialRegionKey key) {
    const auto* source_assignment = material_assignment_at(source, key);
    const auto destination_found = destination.assignments().find(key);
    if (source_assignment == nullptr) {
        if (destination_found == destination.assignments().end()) {
            return core::Result<void>::success();
        }
        return destination.erase_assignment(key);
    }
    if (destination_found == destination.assignments().end()) {
        return destination.insert_assignment(*source_assignment);
    }
    return destination.replace_assignment(*source_assignment);
}

core::Result<MaterialCatalog> material_catalog_without_assignments(
    const MaterialCatalog& catalog) {
    MaterialCatalog result = catalog;
    std::vector<MaterialRegionKey> keys;
    keys.reserve(result.assignments().size());
    for (const auto& [key, assignment] : result.assignments()) {
        static_cast<void>(assignment);
        keys.push_back(key);
    }
    for (const auto key : keys) {
        if (auto erased = result.erase_assignment(key); !erased) {
            return core::Result<MaterialCatalog>::failure(erased.error());
        }
    }
    return core::Result<MaterialCatalog>::success(std::move(result));
}

std::mutex g_project_save_mutex;
std::atomic<std::uint64_t> g_project_temp_counter{0U};
std::atomic<std::uint64_t> g_project_namespace_counter{0U};

bool valid_source_namespace(std::string_view value) {
    return !value.empty() && value.size() <= 128U &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') ||
                (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
                byte == '-';
        });
}

bool valid_spatial_field_identity(std::string_view value) {
    return !value.empty() && value.size() <= 128U &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
        });
}

bool valid_curve_or_road_identity(std::string_view value) {
    return !value.empty() && value.size() <= 128U && value.front() != '.' &&
        value.front() != '-' && value.front() != '_' &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
        });
}

// Pose library identities follow the pose library name contract: 1..64
// characters of [A-Za-z0-9_.-].
bool valid_pose_library_identity(std::string_view value) {
    return !value.empty() && value.size() <= pose::PoseLibrary::kMaxNameLength &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
        });
}

// Digests bind opaque serialized records: the stored digest must equal the
// SHA-256 of the exact bytes and the bytes must decode through their codec.
core::Result<void> bind_opaque_record(
    std::string_view bytes,
    const assets::Sha256Digest& digest,
    std::string_view description) {
    if (digest.is_zero() || bytes.empty()) {
        return core::Result<void>::failure(invalid(
            std::string(description) + " bytes or digest are outside the supported bounds"));
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    if (assets::sha256(std::span<const std::uint8_t>(data, bytes.size())) != digest) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::validation_failed,
            std::string(description) + " digest does not bind its serialized bytes"));
    }
    return core::Result<void>::success();
}

std::string namespace_from_seed(std::string_view seed, std::string_view prefix) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(seed.data());
    const auto digest = assets::sha256(std::span<const std::uint8_t>(data, seed.size()));
    return std::string(prefix) + digest.hex().substr(0U, 32U);
}

std::string new_source_namespace() {
    std::ostringstream seed;
    seed << std::chrono::steady_clock::now().time_since_epoch().count() << ':'
         << std::chrono::system_clock::now().time_since_epoch().count() << ':'
         << std::hash<std::thread::id>{}(std::this_thread::get_id()) << ':';
#ifdef _WIN32
    seed << GetCurrentProcessId() << ':';
#else
    seed << getpid() << ':';
#endif
    seed
         << g_project_namespace_counter.fetch_add(1U, std::memory_order_relaxed);
    try {
        std::random_device entropy;
        for (std::size_t index = 0U; index < 4U; ++index) {
            seed << ':' << entropy();
        }
    } catch (...) {
        // Identity uniqueness does not grant authority or provide secrecy.
        // Clock, thread, and process-local sequence remain a safe fallback.
    }
    return namespace_from_seed(seed.str(), "cartographer.project.");
}

core::Result<void> validate_serialized_count(
    std::uint64_t count,
    std::uint64_t limit,
    std::string_view field) {
    if (count > limit) {
        return core::Result<void>::failure(parse_error(
            std::string(field) + " exceeds the Cartographer v1 limit of " +
            std::to_string(limit)));
    }
    return core::Result<void>::success();
}

std::string encode_hex_bytes(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(bytes.size() * 2U);
    for (const char character : bytes) {
        const auto byte = static_cast<unsigned char>(character);
        encoded.push_back(digits[byte >> 4U]);
        encoded.push_back(digits[byte & 0x0fU]);
    }
    return encoded;
}

std::optional<std::string> decode_hex_bytes(
    std::string_view encoded,
    std::size_t maximum_bytes) {
    if ((encoded.size() % 2U) != 0U || encoded.size() / 2U > maximum_bytes) {
        return std::nullopt;
    }
    const auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    std::string decoded;
    decoded.reserve(encoded.size() / 2U);
    for (std::size_t index = 0U; index < encoded.size(); index += 2U) {
        const int high = nibble(encoded[index]);
        const int low = nibble(encoded[index + 1U]);
        if (high < 0 || low < 0) return std::nullopt;
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return decoded;
}

core::Result<void> require_line(std::istream& input, std::string_view expected) {
    std::string token;
    if (!(input >> token) || token != expected) {
        return core::Result<void>::failure(
            parse_error("project file is missing the expected " + std::string(expected) + " record"));
    }
    return core::Result<void>::success();
}

core::Result<std::uint64_t> read_uint(std::istream& input, std::string_view field) {
    std::string token;
    if (!(input >> token)) {
        return core::Result<std::uint64_t>::failure(
            parse_error("project field is not a valid unsigned integer: " + std::string(field)));
    }
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
        return core::Result<std::uint64_t>::failure(
            parse_error("project field is not a valid unsigned integer: " + std::string(field)));
    }
    return core::Result<std::uint64_t>::success(value);
}

core::Result<std::int64_t> read_int(std::istream& input, std::string_view field) {
    std::string token;
    if (!(input >> token)) {
        return core::Result<std::int64_t>::failure(
            parse_error("project field is not a valid signed integer: " + std::string(field)));
    }
    std::int64_t value = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
        return core::Result<std::int64_t>::failure(
            parse_error("project field is not a valid signed integer: " + std::string(field)));
    }
    return core::Result<std::int64_t>::success(value);
}

template <typename T>
core::Result<T> read_scalar(std::istream& input, std::string_view field) {
    T value{};
    if (!(input >> value)) {
        return core::Result<T>::failure(
            parse_error("project field is not a valid scalar: " + std::string(field)));
    }
    return core::Result<T>::success(value);
}

void write_attribute_value(
    std::ostream& output,
    attributes::AttributeType type,
    const attributes::AttributeValue& value) {
    switch (type) {
    case attributes::AttributeType::boolean:
        output << (std::get<bool>(value) ? 1 : 0);
        return;
    case attributes::AttributeType::int32:
        output << std::get<std::int32_t>(value);
        return;
    case attributes::AttributeType::uint32:
        output << std::get<std::uint32_t>(value);
        return;
    case attributes::AttributeType::float32:
        output << std::get<float>(value);
        return;
    case attributes::AttributeType::float64:
        output << std::get<double>(value);
        return;
    case attributes::AttributeType::vec2: {
        const auto& candidate = std::get<core::Vec2d>(value);
        output << candidate.x << ' ' << candidate.y;
        return;
    }
    case attributes::AttributeType::vec3: {
        const auto& candidate = std::get<core::Vec3d>(value);
        output << candidate.x << ' ' << candidate.y << ' ' << candidate.z;
        return;
    }
    case attributes::AttributeType::vec4:
    case attributes::AttributeType::color4: {
        const auto& candidate = std::get<core::Vec4d>(value);
        output << candidate.x << ' ' << candidate.y << ' '
               << candidate.z << ' ' << candidate.w;
        return;
    }
    case attributes::AttributeType::string_id:
    case attributes::AttributeType::object_id:
    case attributes::AttributeType::vertex_id:
        output << std::get<std::uint64_t>(value);
        return;
    }
}

std::vector<std::size_t> serialized_attribute_value_order(
    const geometry::EditableMesh& mesh,
    attributes::AttributeDomain domain) {
    const auto topology = mesh.topology();
    if (!topology) return {};

    std::vector<std::size_t> order;
    if (domain == attributes::AttributeDomain::edge) {
        auto source_edges = topology.value().edges;
        std::sort(source_edges.begin(), source_edges.end(), [](const auto& left, const auto& right) {
            return left.id < right.id;
        });
        const auto edge_key = [](geometry::VertexId first, geometry::VertexId second) {
            return std::make_pair(
                std::min(first.value, second.value), std::max(first.value, second.value));
        };
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> source_indices;
        for (std::size_t index = 0U; index < source_edges.size(); ++index) {
            const auto key = edge_key(source_edges[index].first, source_edges[index].second);
            source_indices.emplace(key, index);
        }
        std::set<std::pair<std::uint64_t, std::uint64_t>> emitted;
        for (const auto& face : mesh.faces_sorted()) {
            for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
                const auto key = edge_key(
                    face.vertices[index], face.vertices[(index + 1U) % face.vertices.size()]);
                if (!emitted.insert(key).second) continue;
                const auto source = source_indices.find(key);
                if (source == source_indices.end()) return {};
                order.push_back(source->second);
            }
        }
        return order;
    }

    if (domain == attributes::AttributeDomain::corner) {
        auto source_corners = topology.value().corners;
        std::sort(source_corners.begin(), source_corners.end(), [](const auto& left, const auto& right) {
            return left.id < right.id;
        });
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> source_indices;
        for (std::size_t index = 0U; index < source_corners.size(); ++index) {
            source_indices.emplace(
                std::make_pair(source_corners[index].face.value,
                    source_corners[index].vertex.value), index);
        }
        for (const auto& face : mesh.faces_sorted()) {
            for (const geometry::VertexId vertex : face.vertices) {
                const auto source = source_indices.find(std::make_pair(face.id.value, vertex.value));
                if (source == source_indices.end()) return {};
                order.push_back(source->second);
            }
        }
        return order;
    }

    const auto count = mesh.attributes().domain_count(domain);
    if (!count.has_value()) return {};
    order.reserve(*count);
    for (std::size_t index = 0U; index < *count; ++index) order.push_back(index);
    return order;
}

std::string canonical_mesh_state_bytes(const geometry::EditableMesh& mesh) {
    std::ostringstream output;
    output << "MESH_STATE 1\nREVISION " << mesh.revision().value() << '\n'
           << std::setprecision(17);
    const auto vertices = mesh.vertices_sorted();
    output << "VERTICES " << vertices.size() << '\n';
    for (const auto& vertex : vertices) {
        output << "VERTEX " << vertex.id.value << ' ' << vertex.position.x << ' '
               << vertex.position.y << ' ' << vertex.position.z << '\n';
    }
    const auto faces = mesh.faces_sorted();
    output << "FACES " << faces.size() << '\n';
    for (const auto& face : faces) {
        output << "FACE " << face.id.value << ' ' << face.vertices.size();
        for (const auto vertex : face.vertices) output << ' ' << vertex.value;
        output << '\n';
    }
    output << "DOMAIN_COUNTS " << mesh.attributes().domain_counts().size() << '\n';
    for (const auto& [domain, count] : mesh.attributes().domain_counts()) {
        output << "DOMAIN_COUNT " << static_cast<std::uint64_t>(domain) << ' '
               << count << '\n';
    }
    output << "LAYERS " << mesh.attributes().layer_count() << '\n';
    for (const auto& [layer_id, layer] : mesh.attributes().layers()) {
        output << "LAYER " << std::quoted(layer_id) << ' '
               << static_cast<std::uint64_t>(layer.descriptor.domain) << ' '
               << static_cast<std::uint64_t>(layer.descriptor.type) << ' '
               << layer.values.size() << '\n';
        const auto value_order = serialized_attribute_value_order(mesh, layer.descriptor.domain);
        if (value_order.size() == layer.values.size()) {
            for (const std::size_t index : value_order) {
                output << "ATTRIBUTE_VALUE ";
                write_attribute_value(output, layer.descriptor.type, layer.values.at(index));
                output << '\n';
            }
        } else {
            for (const auto& value : layer.values) {
                output << "ATTRIBUTE_VALUE ";
                write_attribute_value(output, layer.descriptor.type, value);
                output << '\n';
            }
        }
    }
    output << "UV_SETS " << mesh.uv_sets().descriptors().size() << '\n';
    for (const auto& [uv_id, descriptor] : mesh.uv_sets().descriptors()) {
        output << "UV_SET " << uv_id << ' ' << std::quoted(descriptor.name) << ' '
               << std::quoted(descriptor.layer_id) << ' '
               << (descriptor.active_for_editing ? 1 : 0) << ' '
               << (descriptor.active_for_render ? 1 : 0) << ' '
               << (descriptor.lightmap_candidate ? 1 : 0) << ' '
               << descriptor.tile_policy_version << '\n';
    }
    return output.str();
}

assets::Sha256Digest digest_text(std::string_view bytes) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    return assets::sha256(std::span<const std::uint8_t>(data, bytes.size()));
}

assets::Sha256Digest mesh_state_digest(const geometry::EditableMesh& mesh) {
    return digest_text(canonical_mesh_state_bytes(mesh));
}

core::Result<attributes::AttributeValue> read_attribute_value(
    std::istream& input,
    attributes::AttributeType type) {
    switch (type) {
    case attributes::AttributeType::boolean: {
        auto encoded = read_uint(input, "boolean attribute value");
        if (!encoded || encoded.value() > 1U) {
            return core::Result<attributes::AttributeValue>::failure(parse_error(
                "boolean attribute value must be 0 or 1"));
        }
        return core::Result<attributes::AttributeValue>::success(encoded.value() != 0U);
    }
    case attributes::AttributeType::int32: {
        auto encoded = read_int(input, "int32 attribute value");
        if (!encoded || encoded.value() < std::numeric_limits<std::int32_t>::min() ||
            encoded.value() > std::numeric_limits<std::int32_t>::max()) {
            return core::Result<attributes::AttributeValue>::failure(parse_error(
                "int32 attribute value is out of range"));
        }
        return core::Result<attributes::AttributeValue>::success(
            static_cast<std::int32_t>(encoded.value()));
    }
    case attributes::AttributeType::uint32: {
        auto encoded = read_uint(input, "uint32 attribute value");
        if (!encoded || encoded.value() > std::numeric_limits<std::uint32_t>::max()) {
            return core::Result<attributes::AttributeValue>::failure(parse_error(
                "uint32 attribute value is out of range"));
        }
        return core::Result<attributes::AttributeValue>::success(
            static_cast<std::uint32_t>(encoded.value()));
    }
    case attributes::AttributeType::float32: {
        auto encoded = read_scalar<float>(input, "float32 attribute value");
        if (!encoded) return core::Result<attributes::AttributeValue>::failure(encoded.error());
        return core::Result<attributes::AttributeValue>::success(encoded.value());
    }
    case attributes::AttributeType::float64: {
        auto encoded = read_scalar<double>(input, "float64 attribute value");
        if (!encoded) return core::Result<attributes::AttributeValue>::failure(encoded.error());
        return core::Result<attributes::AttributeValue>::success(encoded.value());
    }
    case attributes::AttributeType::vec2: {
        auto x = read_scalar<double>(input, "vec2 x attribute value");
        auto y = read_scalar<double>(input, "vec2 y attribute value");
        if (!x || !y) return core::Result<attributes::AttributeValue>::failure(parse_error(
            "vec2 attribute value is invalid"));
        return core::Result<attributes::AttributeValue>::success(core::Vec2d{x.value(), y.value()});
    }
    case attributes::AttributeType::vec3: {
        auto x = read_scalar<double>(input, "vec3 x attribute value");
        auto y = read_scalar<double>(input, "vec3 y attribute value");
        auto z = read_scalar<double>(input, "vec3 z attribute value");
        if (!x || !y || !z) return core::Result<attributes::AttributeValue>::failure(parse_error(
            "vec3 attribute value is invalid"));
        return core::Result<attributes::AttributeValue>::success(
            core::Vec3d{x.value(), y.value(), z.value()});
    }
    case attributes::AttributeType::vec4:
    case attributes::AttributeType::color4: {
        auto x = read_scalar<double>(input, "vec4 x attribute value");
        auto y = read_scalar<double>(input, "vec4 y attribute value");
        auto z = read_scalar<double>(input, "vec4 z attribute value");
        auto w = read_scalar<double>(input, "vec4 w attribute value");
        if (!x || !y || !z || !w) return core::Result<attributes::AttributeValue>::failure(
            parse_error("vec4 attribute value is invalid"));
        return core::Result<attributes::AttributeValue>::success(
            core::Vec4d{x.value(), y.value(), z.value(), w.value()});
    }
    case attributes::AttributeType::string_id:
    case attributes::AttributeType::object_id:
    case attributes::AttributeType::vertex_id: {
        auto encoded = read_uint(input, "uint64 attribute value");
        if (!encoded) return core::Result<attributes::AttributeValue>::failure(encoded.error());
        return core::Result<attributes::AttributeValue>::success(encoded.value());
    }
    }
    return core::Result<attributes::AttributeValue>::failure(parse_error(
        "attribute type is invalid"));
}

std::filesystem::path temporary_path(const std::filesystem::path& target) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto counter = g_project_temp_counter.fetch_add(1U, std::memory_order_relaxed);
    return target.parent_path() / (target.filename().string() + ".carto.tmp-" +
        std::to_string(static_cast<unsigned long long>(ticks)) + "-" +
        std::to_string(static_cast<unsigned long long>(thread)) + "-" +
        std::to_string(static_cast<unsigned long long>(counter)));
}

core::Result<void> atomic_replace(
    const std::filesystem::path& temporary,
    const std::filesystem::path& target) {
#ifdef _WIN32
    if (!MoveFileExW(
            temporary.wstring().c_str(),
            target.wstring().c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto win32_error = static_cast<unsigned long>(GetLastError());
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::io_error, "atomic project replacement failed on Windows (error " +
                std::to_string(win32_error) + ")"));
    }
    return core::Result<void>::success();
#else
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::io_error, "atomic project replacement failed: " + error.message()));
    }
    return core::Result<void>::success();
#endif
}

bool mesh_declares_material_region(
    const geometry::EditableMesh& mesh,
    std::uint32_t region_id) {
    for (const std::string_view layer_id : {
             std::string_view{"condition.material_region"},
             std::string_view{"cartographer.material_slot"}}) {
        const auto* layer = mesh.attributes().find(layer_id);
        if (layer == nullptr || layer->descriptor.domain != attributes::AttributeDomain::face ||
            layer->descriptor.type != attributes::AttributeType::uint32) {
            continue;
        }
        if (std::any_of(layer->values.begin(), layer->values.end(),
                [region_id](const attributes::AttributeValue& value) {
                    const auto* encoded = std::get_if<std::uint32_t>(&value);
                    return encoded != nullptr && *encoded == region_id;
                })) {
            return true;
        }
    }
    return false;
}

} // namespace

core::Result<void> SignTextInstanceReceipt::validate() const {
    if (!object || mesh_asset == 0U || source_identity.empty() ||
        source_revision.value() == 0U || source_snapshot.empty() ||
        source_snapshot.size() > geometry::SignTextSource::kMaxTextBytes * 32U) {
        return core::Result<void>::failure(parse_error(
            "sign-text instance receipt identity or source snapshot is invalid"));
    }
    const auto* snapshot_bytes = reinterpret_cast<const std::uint8_t*>(source_snapshot.data());
    const auto computed_digest = assets::sha256(
        std::span<const std::uint8_t>(snapshot_bytes, source_snapshot.size()));
    if (computed_digest != source_digest) {
        return core::Result<void>::failure(parse_error(
            "sign-text instance receipt digest does not match its source snapshot"));
    }
    const auto source = geometry::SignTextSource::deserialize(source_snapshot);
    if (!source || source.value().source_identity != source_identity ||
        source.value().source_revision != source_revision ||
        source.value().serialize() != source_snapshot) {
        return core::Result<void>::failure(parse_error(
            "sign-text instance receipt does not bind a canonical matching source"));
    }
    return core::Result<void>::success();
}

core::Result<void> RoadSurfaceInstanceReceipt::validate() const {
    if (!object || mesh_asset == 0U || road_identity.empty() ||
        road_revision.value() == 0U || centerline_identity.empty() ||
        centerline_revision.value() == 0U || samples_per_segment == 0U ||
        samples_per_segment > geometry::CurvePath::kMaxSamplesPerSegment ||
        generated_mesh_digest.is_zero() || road_snapshot.empty() ||
        road_snapshot.size() > RoadSource::kMaxSerializedBytes ||
        centerline_snapshot.empty() ||
        centerline_snapshot.size() > geometry::CurvePath::kMaxSerializedBytes) {
        return core::Result<void>::failure(parse_error(
            "road surface instance receipt identity, sample count, or source snapshots are invalid"));
    }

    const auto* road_bytes = reinterpret_cast<const std::uint8_t*>(road_snapshot.data());
    const auto computed_road_digest = assets::sha256(
        std::span<const std::uint8_t>(road_bytes, road_snapshot.size()));
    if (computed_road_digest != road_digest) {
        return core::Result<void>::failure(parse_error(
            "road surface receipt digest does not match its road source snapshot"));
    }
    const auto road = RoadSource::deserialize(road_snapshot);
    if (!road || road.value().source_identity != road_identity ||
        road.value().source_revision != road_revision ||
        road.value().surface_class != surface_class ||
        road.value().centerline_identity != centerline_identity ||
        road.value().centerline_revision != centerline_revision ||
        road.value().serialize() != road_snapshot ||
        road.value().source_digest() != road_digest) {
        return core::Result<void>::failure(parse_error(
            "road surface receipt does not bind a canonical matching road source"));
    }

    const auto* centerline_bytes =
        reinterpret_cast<const std::uint8_t*>(centerline_snapshot.data());
    const auto computed_centerline_digest = assets::sha256(
        std::span<const std::uint8_t>(centerline_bytes, centerline_snapshot.size()));
    if (computed_centerline_digest != centerline_digest) {
        return core::Result<void>::failure(parse_error(
            "road surface receipt digest does not match its centerline snapshot"));
    }
    const auto centerline = geometry::CurvePath::deserialize(centerline_snapshot);
    if (!centerline || centerline.value().source_identity != centerline_identity ||
        centerline.value().source_revision != centerline_revision ||
        centerline.value().serialize() != centerline_snapshot) {
        return core::Result<void>::failure(parse_error(
            "road surface receipt does not bind a canonical matching centerline"));
    }
    return core::Result<void>::success();
}

core::Result<void> BiologicalProxyInstanceReceipt::validate() const {
    if (!object || mesh_asset == 0U || source_namespace.empty() ||
        source_namespace.size() > 256U || source_asset_id == 0U ||
        source_revision.value() == 0U || source_revision.exhausted() ||
        structure_id == 0U || source_snapshot.empty() ||
        source_snapshot.size() > biological::BiologicalAuthoringDocument::kMaxSerializedBytes ||
        source_snapshot_digest.is_zero() || product_digest.is_zero()) {
        return core::Result<void>::failure(parse_error(
            "biological proxy receipt identity or source snapshot is invalid"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(source_snapshot.data());
    if (assets::sha256(std::span<const std::uint8_t>(bytes, source_snapshot.size())) !=
        source_snapshot_digest) {
        return core::Result<void>::failure(parse_error(
            "biological proxy receipt digest does not bind its source snapshot"));
    }
    const auto source = biological::BiologicalAuthoringDocument::deserialize(source_snapshot);
    if (!source || source.value().serialize() != source_snapshot ||
        source.value().source.source_namespace != source_namespace ||
        source.value().source.asset_id != source_asset_id ||
        source.value().source.revision != source_revision) {
        return core::Result<void>::failure(parse_error(
            "biological proxy receipt does not bind a canonical matching source"));
    }
    const auto product = biological::compile_ellipsoid_proxy(
        source.value(), biological::StructureId{structure_id});
    if (!product || product.value().product_digest != product_digest) {
        return core::Result<void>::failure(parse_error(
            "biological proxy receipt product does not match its exact source profile"));
    }
    return core::Result<void>::success();
}

core::Result<void> ConditionVegetationPlacementReceipt::validate() const {
    constexpr double kTau = 6.28318530717958647692;
    if (!object || mesh_asset == 0U || stable_placement_id == 0U || recipe_id == 0U ||
        !valid_curve_or_road_identity(plant_source_identity) ||
        plant_source_generation.value() == 0U || plant_source_generation.exhausted() ||
        plant_source_digest.is_zero() || source_project_revision.value() == 0U ||
        source_project_revision.exhausted() || authoring_revision.value() == 0U ||
        authoring_revision.exhausted() || authoring_digest.is_zero() ||
        preview_digest.is_zero() || fields.size() != 5U || !position.finite() ||
        !std::isfinite(yaw_radians) || yaw_radians < 0.0 || yaw_radians >= kTau ||
        !std::isfinite(uniform_scale) || uniform_scale <= 0.0 || uniform_scale > 1'000.0 ||
        !std::isfinite(suitability) || suitability < 0.0 || suitability > 1.0) {
        return core::Result<void>::failure(parse_error(
            "CONDITION vegetation placement identity, provenance, or transform is invalid"));
    }
    std::set<spatial_field::FieldSemantic> semantics;
    std::set<std::string> identities;
    for (const auto& field : fields) {
        if (!valid_spatial_field_identity(field.identity) ||
            field.source_revision.value() == 0U || field.source_revision.exhausted() ||
            field.digest.is_zero() || !semantics.insert(field.semantic).second ||
            !identities.insert(field.identity).second) {
            return core::Result<void>::failure(parse_error(
                "CONDITION vegetation placement field receipt is invalid or duplicated"));
        }
    }
    constexpr std::array expected_semantics{
        spatial_field::FieldSemantic::moisture,
        spatial_field::FieldSemantic::shade,
        spatial_field::FieldSemantic::disturbance,
        spatial_field::FieldSemantic::vegetation_suitability,
        spatial_field::FieldSemantic::terrain_elevation,
    };
    if (!std::all_of(expected_semantics.begin(), expected_semantics.end(),
            [&semantics](spatial_field::FieldSemantic semantic) {
                return semantics.contains(semantic);
            })) {
        return core::Result<void>::failure(parse_error(
            "CONDITION vegetation placement must bind all five required field semantics"));
    }
    return core::Result<void>::success();
}

core::Result<void> AssetReference::validate() const {
    if (relative_path.empty()) {
        return core::Result<void>::failure(invalid("asset reference must not be empty"));
    }
    const std::filesystem::path path(relative_path);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_argument, "asset reference must be project-relative"));
    }
    for (const auto& part : path) {
        if (part == "..") {
            return core::Result<void>::failure(
                Diagnostic(ErrorCode::invalid_argument, "asset reference may not traverse the project root"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> TopologyReceiptRecord::validate() const {
    if (mesh_asset == 0U) {
        return core::Result<void>::failure(invalid(
            "topology receipt record mesh asset id must be non-zero"));
    }
    return receipt.validate();
}

core::Result<void> MeshPatchRecord::validate() const {
    if (mesh_asset == 0U || patch_bytes.empty() ||
        patch_bytes.size() > geometry::MeshPatch::kMaxSerializedBytes ||
        patch_digest.is_zero() || source_mesh_digest.is_zero() || result_mesh_digest.is_zero()) {
        return core::Result<void>::failure(parse_error(
            "persistent mesh patch identity, digest, or size is invalid"));
    }
    if (digest_text(patch_bytes) != patch_digest) {
        return core::Result<void>::failure(parse_error(
            "persistent mesh patch digest does not bind its serialized bytes"));
    }
    if (!persistent_mesh_patch_revision(patch_bytes)) {
        return core::Result<void>::failure(parse_error(
            "persistent mesh patch bytes are not canonical"));
    }
    return core::Result<void>::success();
}

core::Result<geometry::EditableMesh> MeshPatchRecord::replay(
    const geometry::EditableMesh& source) const {
    if (auto result = validate(); !result) {
        return core::Result<geometry::EditableMesh>::failure(result.error());
    }
    const auto patch_revision = persistent_mesh_patch_revision(patch_bytes);
    if (!patch_revision) {
        return core::Result<geometry::EditableMesh>::failure(patch_revision.error());
    }
    if (source.revision() != patch_revision.value() ||
        mesh_state_digest(source) != source_mesh_digest) {
        return core::Result<geometry::EditableMesh>::failure(Diagnostic(
            ErrorCode::stale_data,
            "persistent mesh patch source revision or digest is stale"));
    }
    geometry::EditableMesh replayed = source;
    const auto vertex_patch = geometry::MeshPatch::deserialize(patch_bytes);
    if (vertex_patch) {
        if (auto result = replayed.apply_patch(vertex_patch.value()); !result) {
            return core::Result<geometry::EditableMesh>::failure(result.error());
        }
    } else {
        const auto face_deletion = geometry::FaceDeletionPatch::deserialize(patch_bytes);
        if (face_deletion) {
            const auto deleted = replayed.delete_face(
                face_deletion.value().face, face_deletion.value().remove_orphaned_vertices);
            if (!deleted) {
                return core::Result<geometry::EditableMesh>::failure(deleted.error());
            }
        } else {
            const auto edge_split = geometry::EdgeSplitPatch::deserialize(patch_bytes);
            if (edge_split) {
                const auto split = replayed.split_edge(edge_split.value().edge,
                    edge_split.value().factor);
                if (!split) {
                    return core::Result<geometry::EditableMesh>::failure(split.error());
                }
            } else {
                const auto loop_cut = geometry::LoopCutPatch::deserialize(patch_bytes);
                if (loop_cut) {
                    const auto cut = replayed.loop_cut(loop_cut.value().edge);
                    if (!cut) {
                        return core::Result<geometry::EditableMesh>::failure(cut.error());
                    }
                } else {
                    const auto bevel_edge = geometry::BevelEdgePatch::deserialize(patch_bytes);
                    if (bevel_edge) {
                        const auto beveled = replayed.bevel_edge(
                            bevel_edge.value().edge, bevel_edge.value().width,
                            bevel_edge.value().segments);
                        if (!beveled) {
                            return core::Result<geometry::EditableMesh>::failure(beveled.error());
                        }
                    } else {
                        const auto bevel_vertex =
                            geometry::BevelVertexPatch::deserialize(patch_bytes);
                        if (bevel_vertex) {
                            const auto beveled = replayed.bevel_vertex(
                                bevel_vertex.value().vertex, bevel_vertex.value().width,
                                bevel_vertex.value().segments);
                            if (!beveled) {
                                return core::Result<geometry::EditableMesh>::failure(
                                    beveled.error());
                            }
                        } else {
                            const auto face_extrusion =
                                geometry::FaceExtrusionPatch::deserialize(patch_bytes);
                            if (face_extrusion) {
                                const auto extruded = replayed.extrude_face_with_receipt(
                                    face_extrusion.value().face,
                                    face_extrusion.value().distance);
                                if (!extruded) {
                                    return core::Result<geometry::EditableMesh>::failure(
                                        extruded.error());
                                }
                            } else {
                                const auto face_inset =
                                    geometry::FaceInsetPatch::deserialize(patch_bytes);
                                if (face_inset) {
                                    const auto inset = replayed.inset_face(
                                        face_inset.value().face,
                                        face_inset.value().distance);
                                    if (!inset) {
                                        return core::Result<geometry::EditableMesh>::failure(
                                            inset.error());
                                    }
                                } else {
                                    const auto face_poke =
                                        geometry::FacePokePatch::deserialize(patch_bytes);
                                    if (face_poke) {
                                        const auto poked = replayed.poke_face(
                                            face_poke.value().face);
                                        if (!poked) {
                                            return core::Result<geometry::EditableMesh>::failure(
                                                poked.error());
                                        }
                                    } else {
                                        const auto tri_to_quad =
                                            geometry::TriToQuadPatch::deserialize(patch_bytes);
                                        if (tri_to_quad) {
                                            const auto converted = replayed.tri_to_quad(
                                                tri_to_quad.value().first_face,
                                                tri_to_quad.value().second_face);
                                            if (!converted) {
                                                return core::Result<geometry::EditableMesh>::failure(
                                                    converted.error());
                                            }
                                        } else {
                                            const auto edge_dissolve =
                                                geometry::EdgeDissolvePatch::deserialize(
                                                    patch_bytes);
                                            if (edge_dissolve) {
                                                const auto dissolved = replayed.dissolve_edges(
                                                    edge_dissolve.value().edges);
                                                if (!dissolved) {
                                                    return core::Result<geometry::EditableMesh>::failure(
                                                        dissolved.error());
                                                }
                                            } else {
                                                const auto merge_by_distance =
                                                    geometry::MergeByDistancePatch::deserialize(
                                                        patch_bytes);
                                                if (merge_by_distance) {
                                                    const auto merged = replayed.merge_by_distance(
                                                        merge_by_distance.value().candidates,
                                                        merge_by_distance.value().distance);
                                                    if (!merged) {
                                                        return core::Result<geometry::EditableMesh>::failure(
                                                            merged.error());
                                                    }
                                                } else {
                                                    const auto vertex_merge =
                                                        geometry::VertexMergePatch::deserialize(
                                                            patch_bytes);
                                                    if (vertex_merge) {
                                                        const auto merged =
                                                            mesh_attributes::merge_vertices_with_attribute_transfer(
                                                                replayed,
                                                                vertex_merge.value().target,
                                                                vertex_merge.value().source,
                                                                vertex_merge.value().mode);
                                                        if (!merged) {
                                                            return core::Result<geometry::EditableMesh>::failure(
                                                                merged.error());
                                                        }
                                                    } else {
                                                        const auto knife_cut =
                                                            geometry::KnifeCutPatch::deserialize(
                                                                patch_bytes);
                                                        if (!knife_cut) {
                                                            return core::Result<geometry::EditableMesh>::failure(
                                                                knife_cut.error());
                                                        }
                                                        const auto cut =
                                                            mesh_attributes::knife_cut_with_attribute_transfer(
                                                            replayed,
                                                            knife_cut.value().face,
                                                            knife_cut.value().path);
                                                        if (!cut) {
                                                            return core::Result<geometry::EditableMesh>::failure(
                                                                cut.error());
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if (mesh_state_digest(replayed) != result_mesh_digest) {
        return core::Result<geometry::EditableMesh>::failure(parse_error(
            "persistent mesh patch replay did not reproduce its result digest"));
    }
    return core::Result<geometry::EditableMesh>::success(std::move(replayed));
}

ProjectDocument::ProjectDocument()
    : source_namespace_(new_source_namespace()) {}

core::Result<ProjectDocument> ProjectDocument::create(std::string name) {
    ProjectDocument document;
    if (auto result = document.set_name(std::move(name)); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    return core::Result<ProjectDocument>::success(std::move(document));
}

core::Result<ProjectDocument> ProjectDocument::merge_disjoint_scene_mesh_changes(
    const ProjectDocument& base,
    const ProjectDocument& current,
    const ProjectDocument& speculative,
    const std::vector<scene::ObjectId>& read_objects,
    const std::vector<std::uint64_t>& read_mesh_assets,
    const std::vector<scene::ObjectId>& write_objects,
    const std::vector<std::uint64_t>& write_mesh_assets,
    const std::vector<MaterialRegionKey>& read_material_regions,
    const std::vector<MaterialRegionKey>& write_material_regions) {
    const auto stale = [](std::string message) {
        return core::Result<ProjectDocument>::failure(Diagnostic(
            ErrorCode::stale_data, std::move(message)));
    };
    const auto invalid_merge = [](std::string message) {
        return core::Result<ProjectDocument>::failure(Diagnostic(
            ErrorCode::invalid_state, std::move(message)));
    };
    if (auto result = base.validate(); !result) {
        return core::Result<ProjectDocument>::failure(
            result.error().with_context("checkpoint rebase base"));
    }
    if (auto result = current.validate(); !result) {
        return core::Result<ProjectDocument>::failure(
            result.error().with_context("checkpoint rebase current source"));
    }
    if (auto result = speculative.validate(); !result) {
        return core::Result<ProjectDocument>::failure(
            result.error().with_context("checkpoint rebase speculative artifact"));
    }
    if (base.source_namespace_ != current.source_namespace_ ||
        base.source_namespace_ != speculative.source_namespace_ ||
        base.name_ != speculative.name_ || current.revision_ < base.revision_ ||
        speculative.revision_ <= base.revision_) {
        return stale("checkpoint rebase documents do not share a valid source lineage");
    }
    if (current.revision_ == base.revision_ && current.serialize() != base.serialize()) {
        return stale("checkpoint rebase source changed without advancing its revision");
    }
    if (current.revision_.exhausted()) {
        return core::Result<ProjectDocument>::failure(exhausted_revision());
    }
    if (read_objects.size() > kMaxSerializedObjects ||
        read_mesh_assets.size() > kMaxSerializedMeshes ||
        write_objects.size() > kMaxSerializedObjects ||
        write_mesh_assets.size() > kMaxSerializedMeshes ||
        read_material_regions.size() > MaterialCatalog::kMaxRecordsPerKind ||
        write_material_regions.size() > MaterialCatalog::kMaxRecordsPerKind ||
        !std::is_sorted(read_objects.begin(), read_objects.end()) ||
        std::adjacent_find(read_objects.begin(), read_objects.end()) != read_objects.end() ||
        !std::is_sorted(read_mesh_assets.begin(), read_mesh_assets.end()) ||
        std::adjacent_find(read_mesh_assets.begin(), read_mesh_assets.end()) !=
            read_mesh_assets.end() ||
        !std::is_sorted(write_objects.begin(), write_objects.end()) ||
        std::adjacent_find(write_objects.begin(), write_objects.end()) != write_objects.end() ||
        !std::is_sorted(write_mesh_assets.begin(), write_mesh_assets.end()) ||
        std::adjacent_find(write_mesh_assets.begin(), write_mesh_assets.end()) !=
            write_mesh_assets.end() ||
        !std::is_sorted(read_material_regions.begin(), read_material_regions.end()) ||
        std::adjacent_find(read_material_regions.begin(), read_material_regions.end()) !=
            read_material_regions.end() ||
        !std::is_sorted(write_material_regions.begin(), write_material_regions.end()) ||
        std::adjacent_find(write_material_regions.begin(), write_material_regions.end()) !=
            write_material_regions.end() ||
        std::any_of(read_material_regions.begin(), read_material_regions.end(),
            [](MaterialRegionKey key) { return key.mesh_asset == 0U || key.region_id == 0U; }) ||
        std::any_of(write_material_regions.begin(), write_material_regions.end(),
            [](MaterialRegionKey key) { return key.mesh_asset == 0U || key.region_id == 0U; })) {
        return invalid_merge("checkpoint rebase write sets are not canonical and bounded");
    }
    const std::set<scene::ObjectId> object_writes(
        write_objects.begin(), write_objects.end());
    const std::set<scene::ObjectId> object_reads(
        read_objects.begin(), read_objects.end());
    const std::set<std::uint64_t> mesh_reads(
        read_mesh_assets.begin(), read_mesh_assets.end());
    const std::set<std::uint64_t> mesh_writes(
        write_mesh_assets.begin(), write_mesh_assets.end());
    const std::set<MaterialRegionKey> material_reads(
        read_material_regions.begin(), read_material_regions.end());
    const std::set<MaterialRegionKey> material_writes(
        write_material_regions.begin(), write_material_regions.end());
    const auto subset = [](const auto& values, const auto& superset) {
        return std::all_of(values.begin(), values.end(), [&superset](const auto value) {
            return superset.contains(value);
        });
    };
    if (!subset(object_writes, object_reads) || !subset(mesh_writes, mesh_reads) ||
        !subset(material_writes, material_reads) ||
        std::any_of(read_material_regions.begin(), read_material_regions.end(),
            [&mesh_reads](MaterialRegionKey key) {
                return !mesh_reads.contains(key.mesh_asset);
            }) ||
        std::any_of(write_material_regions.begin(), write_material_regions.end(),
            [&mesh_writes](MaterialRegionKey key) {
                return !mesh_writes.contains(key.mesh_asset);
            })) {
        return invalid_merge("checkpoint rebase write sets expand beyond their read sets");
    }
    if (!read_material_regions.empty()) {
        auto base_sources = material_catalog_without_assignments(base.material_catalog_);
        auto current_sources = material_catalog_without_assignments(current.material_catalog_);
        if (!base_sources || !current_sources) {
            return invalid_merge("checkpoint rebase could not inspect material catalog dependencies");
        }
        if (base_sources.value().serialize() != current_sources.value().serialize()) {
            return stale("material source definitions changed since the checkpoint source");
        }
        for (const MaterialRegionKey key : read_material_regions) {
            if (!same_material_assignment(
                    material_assignment_at(base.material_catalog_, key),
                    material_assignment_at(current.material_catalog_, key))) {
                return stale("material region assignment changed since the checkpoint source");
            }
        }
    }
    const auto same_transform = [](const core::Transform& left,
                                   const core::Transform& right) {
        return left.translation.x == right.translation.x &&
            left.translation.y == right.translation.y &&
            left.translation.z == right.translation.z &&
            left.rotation.x == right.rotation.x && left.rotation.y == right.rotation.y &&
            left.rotation.z == right.rotation.z && left.rotation.w == right.rotation.w &&
            left.scale.x == right.scale.x && left.scale.y == right.scale.y &&
            left.scale.z == right.scale.z;
    };
    const auto same_object_metadata = [](const scene::SceneObject& left,
                                         const scene::SceneObject& right) {
        return left.id == right.id && left.name == right.name &&
            left.parent == right.parent && left.mesh_asset == right.mesh_asset &&
            left.visible == right.visible && left.locked == right.locked;
    };
    const auto same_object = [&same_transform, &same_object_metadata](
        const scene::SceneObject* left, const scene::SceneObject* right) {
        if (left == nullptr || right == nullptr) return left == right;
        return same_object_metadata(*left, *right) &&
            same_transform(left->local_transform, right->local_transform);
    };
    const auto same_mesh_projection = [&base](
        const ProjectDocument& left,
        const ProjectDocument& right,
        std::uint64_t asset) {
        const auto left_mesh = left.meshes_.find(asset);
        const auto right_mesh = right.meshes_.find(asset);
        if (left_mesh == left.meshes_.end() || right_mesh == right.meshes_.end()) {
            return left_mesh == left.meshes_.end() && right_mesh == right.meshes_.end();
        }
        ProjectDocument left_projection;
        left_projection.source_namespace_ = base.source_namespace_;
        left_projection.name_ = base.name_;
        left_projection.meshes_.emplace(asset, left_mesh->second);
        ProjectDocument right_projection = left_projection;
        right_projection.meshes_[asset] = right_mesh->second;
        return left_projection.serialize() == right_projection.serialize();
    };

    if (base.condition_authoring_ != current.condition_authoring_ ||
        base.engineering_authoring_ != current.engineering_authoring_ ||
        base.authoring_standards_.serialize() != current.authoring_standards_.serialize() ||
        base.authoring_standards_.serialize() != speculative.authoring_standards_.serialize()) {
        return stale("checkpoint rebase semantic sidecars changed without field-level provenance");
    }
    for (const scene::ObjectId object : object_reads) {
        if (!same_object(base.scene_.find(object), current.scene_.find(object))) {
            return stale("checkpoint rebase read object changed or disappeared");
        }
    }
    for (const std::uint64_t asset : mesh_reads) {
        if (!base.meshes_.contains(asset) || !current.meshes_.contains(asset) ||
            !same_mesh_projection(base, current, asset)) {
            return stale("checkpoint rebase read mesh changed or disappeared");
        }
    }

    std::set<scene::ObjectId> object_ids;
    for (const auto& object : base.scene_.objects_sorted()) object_ids.insert(object.id);
    for (const auto& object : speculative.scene_.objects_sorted()) object_ids.insert(object.id);
    if (object_ids.size() != base.scene_.size() ||
        object_ids.size() != speculative.scene_.size()) {
        return invalid_merge("speculative checkpoint added or removed scene objects");
    }
    for (const scene::ObjectId object : object_ids) {
        const auto* before = base.scene_.find(object);
        const auto* proposed = speculative.scene_.find(object);
        if (before == nullptr || proposed == nullptr ||
            !same_object_metadata(*before, *proposed)) {
            return invalid_merge("speculative checkpoint changed scene metadata outside the rebase contract");
        }
        if (!same_transform(before->local_transform, proposed->local_transform) &&
            !object_writes.contains(object)) {
            return invalid_merge("speculative checkpoint changed an object outside its write set");
        }
    }
    for (const scene::ObjectId object : object_writes) {
        const auto* before = base.scene_.find(object);
        const auto* latest = current.scene_.find(object);
        const auto* proposed = speculative.scene_.find(object);
        if (before == nullptr || latest == nullptr || proposed == nullptr ||
            !same_object(before, latest) || !same_object_metadata(*before, *proposed)) {
            return stale("checkpoint rebase object write target changed or disappeared");
        }
    }

    std::set<std::uint64_t> mesh_ids;
    for (const auto& [asset, mesh] : base.meshes_) {
        static_cast<void>(mesh);
        mesh_ids.insert(asset);
    }
    for (const auto& [asset, mesh] : speculative.meshes_) {
        static_cast<void>(mesh);
        mesh_ids.insert(asset);
    }
    if (mesh_ids.size() != base.meshes_.size() ||
        mesh_ids.size() != speculative.meshes_.size()) {
        return invalid_merge("speculative checkpoint added or removed mesh assets");
    }
    for (const std::uint64_t asset : mesh_ids) {
        const bool changed = !same_mesh_projection(base, speculative, asset);
        if (changed && !mesh_writes.contains(asset)) {
            return invalid_merge("speculative checkpoint changed a mesh outside its write set");
        }
    }
    for (const std::uint64_t asset : mesh_writes) {
        if (!base.meshes_.contains(asset) || !current.meshes_.contains(asset) ||
            !speculative.meshes_.contains(asset) ||
            !same_mesh_projection(base, current, asset)) {
            return stale("checkpoint rebase mesh write target changed or disappeared");
        }
    }
    if (!mesh_writes.empty()) {
        std::set<scene::ObjectId> source_object_ids;
        for (const auto& object : base.scene_.objects_sorted()) source_object_ids.insert(object.id);
        for (const auto& object : current.scene_.objects_sorted()) source_object_ids.insert(object.id);
        for (const scene::ObjectId object : source_object_ids) {
            const auto* before = base.scene_.find(object);
            const auto* latest = current.scene_.find(object);
            const auto is_written_asset = [&mesh_writes](const scene::SceneObject* value) {
                return value != nullptr && value->mesh_asset.has_value() &&
                    mesh_writes.contains(*value->mesh_asset);
            };
            if (is_written_asset(before) || is_written_asset(latest)) {
                if (before == nullptr || latest == nullptr ||
                    before->mesh_asset != latest->mesh_asset) {
                    return stale("checkpoint rebase changed membership of a written mesh asset");
                }
            }
        }
    }

    if (speculative.topology_receipts_.size() < base.topology_receipts_.size()) {
        return invalid_merge("speculative checkpoint removed topology receipt history");
    }
    for (std::size_t index = 0U; index < base.topology_receipts_.size(); ++index) {
        const auto& before = base.topology_receipts_[index];
        const auto& proposed = speculative.topology_receipts_[index];
        if (before.mesh_asset != proposed.mesh_asset ||
            before.receipt.serialize() != proposed.receipt.serialize()) {
            return invalid_merge("speculative checkpoint rewrote topology receipt history");
        }
    }
    for (std::size_t index = base.topology_receipts_.size();
         index < speculative.topology_receipts_.size(); ++index) {
        const auto& added = speculative.topology_receipts_[index];
        if (!mesh_writes.contains(added.mesh_asset)) {
            return invalid_merge("speculative checkpoint added topology evidence outside its write set");
        }
        if (auto result = added.validate(); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
    }
    if (speculative.mesh_patches_.size() < base.mesh_patches_.size()) {
        return invalid_merge("speculative checkpoint removed mesh patch history");
    }
    for (std::size_t index = 0U; index < base.mesh_patches_.size(); ++index) {
        const auto& before = base.mesh_patches_[index];
        const auto& proposed = speculative.mesh_patches_[index];
        if (before.mesh_asset != proposed.mesh_asset ||
            before.patch_digest != proposed.patch_digest ||
            before.source_mesh_digest != proposed.source_mesh_digest ||
            before.result_mesh_digest != proposed.result_mesh_digest ||
            before.patch_bytes != proposed.patch_bytes) {
            return invalid_merge("speculative checkpoint rewrote mesh patch history");
        }
    }
    for (std::size_t index = base.mesh_patches_.size();
         index < speculative.mesh_patches_.size(); ++index) {
        const auto& added = speculative.mesh_patches_[index];
        if (!mesh_writes.contains(added.mesh_asset)) {
            return invalid_merge("speculative checkpoint added mesh patch evidence outside its write set");
        }
        if (auto result = added.validate(); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
    }

    std::set<std::uint64_t> cloth_receipt_ids;
    for (const auto& [asset, receipt] : base.cloth_rest_pose_receipts_) {
        static_cast<void>(receipt);
        cloth_receipt_ids.insert(asset);
    }
    for (const auto& [asset, receipt] : speculative.cloth_rest_pose_receipts_) {
        static_cast<void>(receipt);
        cloth_receipt_ids.insert(asset);
    }
    for (const std::uint64_t asset : cloth_receipt_ids) {
        const auto before = base.cloth_rest_pose_receipts_.find(asset);
        const auto proposed = speculative.cloth_rest_pose_receipts_.find(asset);
        const bool same = before == base.cloth_rest_pose_receipts_.end()
            ? proposed == speculative.cloth_rest_pose_receipts_.end()
            : proposed != speculative.cloth_rest_pose_receipts_.end() &&
                before->second == proposed->second;
        if (!same && !mesh_writes.contains(asset)) {
            return invalid_merge("speculative checkpoint changed cloth provenance outside its write set");
        }
    }

    ProjectDocument expected_speculative = base;
    for (const scene::ObjectId object : write_objects) {
        const auto* proposed = speculative.scene_.find(object);
        if (auto result = expected_speculative.scene_.set_local_transform(
                object, proposed->local_transform); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
    }
    for (const std::uint64_t asset : write_mesh_assets) {
        expected_speculative.meshes_[asset] = speculative.meshes_.at(asset);
        const auto receipt = speculative.cloth_rest_pose_receipts_.find(asset);
        if (receipt == speculative.cloth_rest_pose_receipts_.end()) {
            expected_speculative.cloth_rest_pose_receipts_.erase(asset);
        } else {
            expected_speculative.cloth_rest_pose_receipts_[asset] = receipt->second;
        }
    }
    MaterialCatalog expected_material_catalog = base.material_catalog_;
    for (const MaterialRegionKey key : write_material_regions) {
        if (auto copied = copy_material_assignment(
                expected_material_catalog, speculative.material_catalog_, key); !copied) {
            return core::Result<ProjectDocument>::failure(copied.error());
        }
    }
    if (expected_material_catalog.serialize() != speculative.material_catalog_.serialize()) {
        return invalid_merge(
            "speculative checkpoint changed material catalog data outside declared region assignments");
    }
    expected_speculative.material_catalog_ = std::move(expected_material_catalog);
    expected_speculative.topology_receipts_ = speculative.topology_receipts_;
    expected_speculative.mesh_patches_ = speculative.mesh_patches_;
    expected_speculative.revision_ = speculative.revision_;
    if (expected_speculative.serialize() != speculative.serialize()) {
        return invalid_merge("speculative checkpoint contains changes outside scene, mesh, and edit provenance writes");
    }

    ProjectDocument merged = current;
    for (const scene::ObjectId object : write_objects) {
        const auto* proposed = speculative.scene_.find(object);
        if (auto result = merged.scene_.set_local_transform(
                object, proposed->local_transform); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
    }
    for (const std::uint64_t asset : write_mesh_assets) {
        merged.meshes_[asset] = speculative.meshes_.at(asset);
        const auto receipt = speculative.cloth_rest_pose_receipts_.find(asset);
        if (receipt == speculative.cloth_rest_pose_receipts_.end()) {
            merged.cloth_rest_pose_receipts_.erase(asset);
        } else {
            merged.cloth_rest_pose_receipts_[asset] = receipt->second;
        }
    }
    MaterialCatalog merged_material_catalog = current.material_catalog_;
    for (const MaterialRegionKey key : write_material_regions) {
        if (auto copied = copy_material_assignment(
                merged_material_catalog, speculative.material_catalog_, key); !copied) {
            return core::Result<ProjectDocument>::failure(copied.error());
        }
    }
    merged.material_catalog_ = std::move(merged_material_catalog);
    merged.topology_receipts_.insert(
        merged.topology_receipts_.end(),
        speculative.topology_receipts_.begin() +
            static_cast<std::ptrdiff_t>(base.topology_receipts_.size()),
        speculative.topology_receipts_.end());
    merged.mesh_patches_.insert(
        merged.mesh_patches_.end(),
        speculative.mesh_patches_.begin() +
            static_cast<std::ptrdiff_t>(base.mesh_patches_.size()),
        speculative.mesh_patches_.end());
    const std::uint64_t replay_revision_delta =
        speculative.revision_.value() - base.revision_.value();
    if (replay_revision_delta >
        core::Revision::max_value() - current.revision_.value()) {
        return core::Result<ProjectDocument>::failure(exhausted_revision());
    }
    merged.revision_ = core::Revision(
        current.revision_.value() + replay_revision_delta);
    if (auto result = merged.validate(); !result) {
        return core::Result<ProjectDocument>::failure(
            result.error().with_context("merged checkpoint rebase project"));
    }
    return core::Result<ProjectDocument>::success(std::move(merged));
}

core::Result<ProjectDocument> ProjectDocument::select_agent_rebase_step_changes(
    const ProjectDocument& base,
    const ProjectDocument& speculative,
    const std::vector<scene::ObjectId>& write_objects,
    const std::vector<std::uint64_t>& write_mesh_assets,
    std::uint64_t revision_delta) {
    const auto invalid_selection = [](std::string message) {
        return core::Result<ProjectDocument>::failure(Diagnostic(
            ErrorCode::invalid_state, std::move(message)));
    };
    if (auto result = base.validate(); !result) {
        return core::Result<ProjectDocument>::failure(
            result.error().with_context("checkpoint rebase step base"));
    }
    if (auto result = speculative.validate(); !result) {
        return core::Result<ProjectDocument>::failure(
            result.error().with_context("checkpoint rebase step artifact"));
    }
    if (base.source_namespace_ != speculative.source_namespace_ ||
        base.name_ != speculative.name_ || speculative.revision_ <= base.revision_ ||
        revision_delta == 0U ||
        revision_delta > core::Revision::max_value() - base.revision_.value()) {
        return invalid_selection(
            "checkpoint rebase step artifacts do not share a bounded source lineage");
    }
    if (write_objects.size() > kMaxSerializedObjects ||
        write_mesh_assets.size() > kMaxSerializedMeshes ||
        !std::is_sorted(write_objects.begin(), write_objects.end()) ||
        std::adjacent_find(write_objects.begin(), write_objects.end()) != write_objects.end() ||
        !std::is_sorted(write_mesh_assets.begin(), write_mesh_assets.end()) ||
        std::adjacent_find(write_mesh_assets.begin(), write_mesh_assets.end()) !=
            write_mesh_assets.end()) {
        return invalid_selection(
            "checkpoint rebase step write sets are not canonical and bounded");
    }

    ProjectDocument selected = base;
    for (const scene::ObjectId object : write_objects) {
        const auto* before = base.scene_.find(object);
        const auto* after = speculative.scene_.find(object);
        if (before == nullptr || after == nullptr || before->id != after->id ||
            before->name != after->name || before->parent != after->parent ||
            before->mesh_asset != after->mesh_asset || before->visible != after->visible ||
            before->locked != after->locked) {
            return invalid_selection(
                "checkpoint rebase step changed or removed an object write target");
        }
        if (auto result = selected.scene_.set_local_transform(
                object, after->local_transform); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
    }
    for (const std::uint64_t asset : write_mesh_assets) {
        const auto before = base.meshes_.find(asset);
        const auto after = speculative.meshes_.find(asset);
        if (before == base.meshes_.end() || after == speculative.meshes_.end()) {
            return invalid_selection(
                "checkpoint rebase step changed or removed a mesh write target");
        }
        selected.meshes_[asset] = after->second;
        const auto receipt = speculative.cloth_rest_pose_receipts_.find(asset);
        if (receipt == speculative.cloth_rest_pose_receipts_.end()) {
            selected.cloth_rest_pose_receipts_.erase(asset);
        } else {
            selected.cloth_rest_pose_receipts_[asset] = receipt->second;
        }
    }
    if (speculative.topology_receipts_.size() < base.topology_receipts_.size()) {
        return invalid_selection(
            "checkpoint rebase step removed topology receipt history");
    }
    for (std::size_t index = 0U; index < base.topology_receipts_.size(); ++index) {
        const auto& before = base.topology_receipts_[index];
        const auto& after = speculative.topology_receipts_[index];
        if (before.mesh_asset != after.mesh_asset ||
            before.receipt.serialize() != after.receipt.serialize()) {
            return invalid_selection(
                "checkpoint rebase step rewrote topology receipt history");
        }
    }
    const std::set<std::uint64_t> selected_assets(
        write_mesh_assets.begin(), write_mesh_assets.end());
    for (std::size_t index = base.topology_receipts_.size();
         index < speculative.topology_receipts_.size(); ++index) {
        const auto& receipt = speculative.topology_receipts_[index];
        if (selected_assets.contains(receipt.mesh_asset)) {
            if (auto result = receipt.validate(); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            selected.topology_receipts_.push_back(receipt);
        }
    }
    if (speculative.mesh_patches_.size() < base.mesh_patches_.size()) {
        return invalid_selection("checkpoint rebase step removed mesh patch history");
    }
    for (std::size_t index = 0U; index < base.mesh_patches_.size(); ++index) {
        const auto& before = base.mesh_patches_[index];
        const auto& after = speculative.mesh_patches_[index];
        if (before.mesh_asset != after.mesh_asset ||
            before.patch_digest != after.patch_digest ||
            before.source_mesh_digest != after.source_mesh_digest ||
            before.result_mesh_digest != after.result_mesh_digest ||
            before.patch_bytes != after.patch_bytes) {
            return invalid_selection("checkpoint rebase step rewrote mesh patch history");
        }
    }
    for (std::size_t index = base.mesh_patches_.size();
         index < speculative.mesh_patches_.size(); ++index) {
        const auto& patch = speculative.mesh_patches_[index];
        if (selected_assets.contains(patch.mesh_asset)) {
            if (auto result = patch.validate(); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            selected.mesh_patches_.push_back(patch);
        }
    }
    selected.revision_ = core::Revision(
        base.revision_.value() + revision_delta);
    if (auto result = selected.validate(); !result) {
        return core::Result<ProjectDocument>::failure(
            result.error().with_context("selected checkpoint rebase step"));
    }
    return core::Result<ProjectDocument>::success(std::move(selected));
}

core::Result<void> ProjectDocument::set_name(std::string name) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (name.empty()) {
        return core::Result<void>::failure(invalid("project name must not be empty"));
    }
    name_ = std::move(name);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<scene::ObjectId> ProjectDocument::create_object(
    std::string name,
    core::Transform transform) {
    if (revision_.exhausted()) {
        return core::Result<scene::ObjectId>::failure(exhausted_revision());
    }
    auto result = scene_.create_object(std::move(name), transform);
    if (!result) {
        return result;
    }
    bump_revision();
    return result;
}

core::Result<void> ProjectDocument::insert_object(scene::SceneObject object) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (object.mesh_asset.has_value() && !meshes_.contains(*object.mesh_asset)) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::not_found, "cannot insert an object with a missing mesh asset"));
    }
    if (auto result = scene_.insert_object(std::move(object)); !result) {
        return result;
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::remove_object(scene::ObjectId object) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    for (const auto& [identity, area] : world_model_.areas()) {
        static_cast<void>(identity);
        if (area.geometry.kind == world::AuthoringBindingKind::scene_object &&
            area.geometry.identity == object.value) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "cannot remove a scene object referenced by a semantic area"));
        }
    }
    if (presentation_catalog_.cameras().contains(object) ||
        presentation_catalog_.lights().contains(object)) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state,
            "cannot remove a scene object referenced by authored presentation data"));
    }
    if (auto result = scene_.remove_object(object); !result) {
        return result;
    }
    sign_text_instance_receipts_.erase(object);
    road_surface_instance_receipts_.erase(object);
    biological_proxy_instance_receipts_.erase(object);
    condition_vegetation_placement_receipts_.erase(object);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<std::uint64_t> ProjectDocument::add_mesh(geometry::EditableMesh mesh) {
    if (revision_.exhausted()) {
        return core::Result<std::uint64_t>::failure(exhausted_revision());
    }
    if (auto result = mesh.validate(); !result) {
        return core::Result<std::uint64_t>::failure(result.error());
    }
    if (mesh.revision().exhausted()) {
        return core::Result<std::uint64_t>::failure(
            Diagnostic(ErrorCode::invalid_state, "mesh asset revision space is exhausted"));
    }
    if (next_mesh_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<std::uint64_t>::failure(
            Diagnostic(ErrorCode::invalid_state, "mesh asset id space is exhausted"));
    }
    const std::uint64_t id = next_mesh_id_++;
    meshes_.emplace(id, std::move(mesh));
    bump_revision();
    return core::Result<std::uint64_t>::success(id);
}

core::Result<void> ProjectDocument::insert_mesh(
    std::uint64_t mesh_asset,
    geometry::EditableMesh mesh) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (mesh_asset == 0) {
        return core::Result<void>::failure(invalid("mesh asset id must be non-zero"));
    }
    if (meshes_.contains(mesh_asset)) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_state, "duplicate mesh asset id"));
    }
    if (auto result = mesh.validate(); !result) {
        return result;
    }
    if (mesh.revision().exhausted()) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_state, "mesh asset revision space is exhausted"));
    }
    meshes_.emplace(mesh_asset, std::move(mesh));
    if (mesh_asset < std::numeric_limits<std::uint64_t>::max()) {
        next_mesh_id_ = std::max(next_mesh_id_, mesh_asset + 1U);
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::replace_mesh(
    std::uint64_t mesh_asset,
    geometry::EditableMesh mesh) {
    const auto iterator = meshes_.find(mesh_asset);
    if (iterator == meshes_.end()) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::not_found, "cannot replace a missing mesh asset"));
    }
    auto result = replace_mesh_if_revision(mesh_asset, iterator->second.revision(), std::move(mesh));
    if (!result) {
        return core::Result<void>::failure(result.error());
    }
    return core::Result<void>::success();
}

core::Result<core::Revision> ProjectDocument::replace_mesh_if_revision(
    std::uint64_t mesh_asset,
    core::Revision expected_revision,
    geometry::EditableMesh mesh,
    std::optional<geometry::TopologyEditReceipt> receipt) {
    if (revision_.exhausted()) {
        return core::Result<core::Revision>::failure(exhausted_revision());
    }
    const auto iterator = meshes_.find(mesh_asset);
    if (iterator == meshes_.end()) {
        return core::Result<core::Revision>::failure(
            Diagnostic(ErrorCode::not_found, "cannot replace a missing mesh asset"));
    }
    if (iterator->second.revision() != expected_revision) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data,
            "mesh replacement revision does not match the current asset"));
    }
    if (receipt.has_value()) {
        if (auto result = receipt->validate(); !result) {
            return core::Result<core::Revision>::failure(
                result.error().with_context("topology receipt"));
        }
        if (receipt->revision_before != expected_revision ||
            receipt->revision_after != mesh.revision()) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::stale_data,
                "topology receipt does not bind to the replaced mesh revisions"));
        }
        if (topology_receipts_.size() >= kMaxSerializedTopologyReceipts) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::validation_failed,
                "project topology receipt ledger is at its bounded capacity"));
        }
        for (const auto& record : topology_receipts_) {
            if (record.mesh_asset == mesh_asset &&
                record.receipt.revision_before == receipt->revision_before &&
                record.receipt.revision_after == receipt->revision_after) {
                return core::Result<core::Revision>::failure(Diagnostic(
                    ErrorCode::invalid_state,
                    "project already contains this topology receipt interval"));
            }
        }
    }
    if (auto result = mesh.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    for (const auto& [identity, assignment] : material_catalog_.assignments()) {
        static_cast<void>(identity);
        if (assignment.key.mesh_asset == mesh_asset &&
            !mesh_declares_material_region(mesh, assignment.key.region_id)) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::validation_failed,
                "mesh replacement would orphan a material-region assignment"));
        }
    }
    if (iterator->second.revision().exhausted() || mesh.revision().exhausted()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::invalid_state,
            "mesh asset revision space is exhausted"));
    }
    const core::Revision minimum_revision = iterator->second.revision().next();
    if (mesh.revision() < minimum_revision) {
        if (auto result = mesh.restore_revision(minimum_revision); !result) {
            return core::Result<core::Revision>::failure(result.error());
        }
    }
    const core::Revision applied_revision = mesh.revision();
    iterator->second = std::move(mesh);
    cloth_rest_pose_receipts_.erase(mesh_asset);
    if (receipt.has_value()) {
        topology_receipts_.push_back(TopologyReceiptRecord{mesh_asset, std::move(*receipt)});
    }
    bump_revision();
    return core::Result<core::Revision>::success(applied_revision);
}

core::Result<core::Revision> ProjectDocument::apply_mesh_patch(
    std::uint64_t mesh_asset,
    const geometry::MeshPatch& patch) {
    if (mesh_patches_.size() >= kMaxSerializedMeshPatchRecords) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger is at its bounded capacity"));
    }
    const auto mesh = meshes_.find(mesh_asset);
    if (mesh == meshes_.end()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::not_found, "cannot patch a missing mesh asset"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    for (const auto& record : mesh_patches_) {
        const auto recorded_revision = persistent_mesh_patch_revision(record.patch_bytes);
        if (!recorded_revision) {
            return core::Result<core::Revision>::failure(recorded_revision.error());
        }
        if (record.mesh_asset == mesh_asset &&
            recorded_revision.value() == patch.expected_revision) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "project already contains a mesh patch for this source revision"));
        }
    }

    MeshPatchRecord record;
    record.mesh_asset = mesh_asset;
    record.patch_bytes = patch.serialize();
    std::uint64_t aggregate_patch_bytes = 0U;
    for (const auto& existing : mesh_patches_) {
        if (existing.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
            return core::Result<core::Revision>::failure(parse_error(
                "project mesh patch ledger exceeds its aggregate byte limit"));
        }
        aggregate_patch_bytes += static_cast<std::uint64_t>(existing.patch_bytes.size());
    }
    if (record.patch_bytes.empty() || record.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger exceeds its aggregate byte limit"));
    }
    record.patch_digest = digest_text(record.patch_bytes);
    record.source_mesh_digest = mesh_state_digest(mesh->second);
    if (patch.expected_revision != mesh->second.revision()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh patch revision does not match the current asset"));
    }
    geometry::EditableMesh after = mesh->second;
    if (auto result = after.apply_patch(patch); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    record.result_mesh_digest = mesh_state_digest(after);
    if (auto result = record.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    mesh_patches_.reserve(mesh_patches_.size() + 1U);
    const auto replaced = replace_mesh_if_revision(mesh_asset, mesh->second.revision(),
        std::move(after));
    if (!replaced) return replaced;
    mesh_patches_.push_back(std::move(record));
    return replaced;
}

core::Result<core::Revision> ProjectDocument::apply_face_deletion_patch(
    std::uint64_t mesh_asset,
    const geometry::FaceDeletionPatch& patch) {
    if (mesh_patches_.size() >= kMaxSerializedMeshPatchRecords) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger is at its bounded capacity"));
    }
    const auto mesh = meshes_.find(mesh_asset);
    if (mesh == meshes_.end()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::not_found, "cannot patch a missing mesh asset"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    for (const auto& record : mesh_patches_) {
        const auto recorded_revision = persistent_mesh_patch_revision(record.patch_bytes);
        if (!recorded_revision) {
            return core::Result<core::Revision>::failure(recorded_revision.error());
        }
        if (record.mesh_asset == mesh_asset &&
            recorded_revision.value() == patch.expected_revision) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "project already contains a mesh patch for this source revision"));
        }
    }

    MeshPatchRecord record;
    record.mesh_asset = mesh_asset;
    record.patch_bytes = patch.serialize();
    std::uint64_t aggregate_patch_bytes = 0U;
    for (const auto& existing : mesh_patches_) {
        if (existing.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
            return core::Result<core::Revision>::failure(parse_error(
                "project mesh patch ledger exceeds its aggregate byte limit"));
        }
        aggregate_patch_bytes += static_cast<std::uint64_t>(existing.patch_bytes.size());
    }
    if (record.patch_bytes.empty() || record.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger exceeds its aggregate byte limit"));
    }
    record.patch_digest = digest_text(record.patch_bytes);
    record.source_mesh_digest = mesh_state_digest(mesh->second);
    if (patch.expected_revision != mesh->second.revision()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh patch revision does not match the current asset"));
    }

    geometry::EditableMesh after = mesh->second;
    auto deleted = after.delete_face(patch.face, patch.remove_orphaned_vertices);
    if (!deleted) return core::Result<core::Revision>::failure(deleted.error());
    record.result_mesh_digest = mesh_state_digest(after);
    if (auto result = record.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    mesh_patches_.reserve(mesh_patches_.size() + 1U);
    topology_receipts_.reserve(topology_receipts_.size() + 1U);
    const auto replaced = replace_mesh_if_revision(
        mesh_asset, mesh->second.revision(), std::move(after), std::move(deleted.value()));
    if (!replaced) return replaced;
    mesh_patches_.push_back(std::move(record));
    return replaced;
}

core::Result<core::Revision> ProjectDocument::apply_edge_split_patch(
    std::uint64_t mesh_asset,
    const geometry::EdgeSplitPatch& patch) {
    if (mesh_patches_.size() >= kMaxSerializedMeshPatchRecords) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger is at its bounded capacity"));
    }
    const auto mesh = meshes_.find(mesh_asset);
    if (mesh == meshes_.end()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::not_found, "cannot patch a missing mesh asset"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    for (const auto& record : mesh_patches_) {
        const auto recorded_revision = persistent_mesh_patch_revision(record.patch_bytes);
        if (!recorded_revision) {
            return core::Result<core::Revision>::failure(recorded_revision.error());
        }
        if (record.mesh_asset == mesh_asset &&
            recorded_revision.value() == patch.expected_revision) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "project already contains a mesh patch for this source revision"));
        }
    }

    MeshPatchRecord record;
    record.mesh_asset = mesh_asset;
    record.patch_bytes = patch.serialize();
    std::uint64_t aggregate_patch_bytes = 0U;
    for (const auto& existing : mesh_patches_) {
        if (existing.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
            return core::Result<core::Revision>::failure(parse_error(
                "project mesh patch ledger exceeds its aggregate byte limit"));
        }
        aggregate_patch_bytes += static_cast<std::uint64_t>(existing.patch_bytes.size());
    }
    if (record.patch_bytes.empty() || record.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger exceeds its aggregate byte limit"));
    }
    record.patch_digest = digest_text(record.patch_bytes);
    record.source_mesh_digest = mesh_state_digest(mesh->second);
    if (patch.expected_revision != mesh->second.revision()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh patch revision does not match the current asset"));
    }

    geometry::EditableMesh after = mesh->second;
    auto split = after.split_edge(patch.edge, patch.factor);
    if (!split) return core::Result<core::Revision>::failure(split.error());
    record.result_mesh_digest = mesh_state_digest(after);
    if (auto result = record.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    mesh_patches_.reserve(mesh_patches_.size() + 1U);
    topology_receipts_.reserve(topology_receipts_.size() + 1U);
    const auto replaced = replace_mesh_if_revision(
        mesh_asset, mesh->second.revision(), std::move(after), std::move(split.value()));
    if (!replaced) return replaced;
    mesh_patches_.push_back(std::move(record));
    return replaced;
}

core::Result<core::Revision> ProjectDocument::apply_edge_dissolve_patch(
    std::uint64_t mesh_asset,
    const geometry::EdgeDissolvePatch& patch) {
    if (mesh_patches_.size() >= kMaxSerializedMeshPatchRecords) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger is at its bounded capacity"));
    }
    const auto mesh = meshes_.find(mesh_asset);
    if (mesh == meshes_.end()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::not_found, "cannot patch a missing mesh asset"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    for (const auto& record : mesh_patches_) {
        const auto recorded_revision = persistent_mesh_patch_revision(record.patch_bytes);
        if (!recorded_revision) {
            return core::Result<core::Revision>::failure(recorded_revision.error());
        }
        if (record.mesh_asset == mesh_asset &&
            recorded_revision.value() == patch.expected_revision) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "project already contains a mesh patch for this source revision"));
        }
    }

    MeshPatchRecord record;
    record.mesh_asset = mesh_asset;
    record.patch_bytes = patch.serialize();
    std::uint64_t aggregate_patch_bytes = 0U;
    for (const auto& existing : mesh_patches_) {
        if (existing.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
            return core::Result<core::Revision>::failure(parse_error(
                "project mesh patch ledger exceeds its aggregate byte limit"));
        }
        aggregate_patch_bytes += static_cast<std::uint64_t>(existing.patch_bytes.size());
    }
    if (record.patch_bytes.empty() || record.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger exceeds its aggregate byte limit"));
    }
    record.patch_digest = digest_text(record.patch_bytes);
    record.source_mesh_digest = mesh_state_digest(mesh->second);
    if (patch.expected_revision != mesh->second.revision()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh patch revision does not match the current asset"));
    }

    geometry::EditableMesh after = mesh->second;
    auto dissolved = after.dissolve_edges(patch.edges);
    if (!dissolved) return core::Result<core::Revision>::failure(dissolved.error());
    record.result_mesh_digest = mesh_state_digest(after);
    if (auto result = record.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    mesh_patches_.reserve(mesh_patches_.size() + 1U);
    topology_receipts_.reserve(topology_receipts_.size() + 1U);
    const auto replaced = replace_mesh_if_revision(
        mesh_asset, mesh->second.revision(), std::move(after), std::move(dissolved.value()));
    if (!replaced) return replaced;
    mesh_patches_.push_back(std::move(record));
    return replaced;
}

core::Result<core::Revision> ProjectDocument::apply_merge_by_distance_patch(
    std::uint64_t mesh_asset,
    const geometry::MergeByDistancePatch& patch) {
    if (mesh_patches_.size() >= kMaxSerializedMeshPatchRecords) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger is at its bounded capacity"));
    }
    const auto mesh = meshes_.find(mesh_asset);
    if (mesh == meshes_.end()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::not_found, "cannot patch a missing mesh asset"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    for (const auto& record : mesh_patches_) {
        const auto recorded_revision = persistent_mesh_patch_revision(record.patch_bytes);
        if (!recorded_revision) {
            return core::Result<core::Revision>::failure(recorded_revision.error());
        }
        if (record.mesh_asset == mesh_asset &&
            recorded_revision.value() == patch.expected_revision) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "project already contains a mesh patch for this source revision"));
        }
    }

    MeshPatchRecord record;
    record.mesh_asset = mesh_asset;
    record.patch_bytes = patch.serialize();
    std::uint64_t aggregate_patch_bytes = 0U;
    for (const auto& existing : mesh_patches_) {
        if (existing.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
            return core::Result<core::Revision>::failure(parse_error(
                "project mesh patch ledger exceeds its aggregate byte limit"));
        }
        aggregate_patch_bytes += static_cast<std::uint64_t>(existing.patch_bytes.size());
    }
    if (record.patch_bytes.empty() || record.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger exceeds its aggregate byte limit"));
    }
    record.patch_digest = digest_text(record.patch_bytes);
    record.source_mesh_digest = mesh_state_digest(mesh->second);
    if (patch.expected_revision != mesh->second.revision()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh patch revision does not match the current asset"));
    }

    geometry::EditableMesh after = mesh->second;
    auto merged = after.merge_by_distance(patch.candidates, patch.distance);
    if (!merged) return core::Result<core::Revision>::failure(merged.error());
    record.result_mesh_digest = mesh_state_digest(after);
    if (auto result = record.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    mesh_patches_.reserve(mesh_patches_.size() + 1U);
    topology_receipts_.reserve(topology_receipts_.size() + 1U);
    const auto replaced = replace_mesh_if_revision(
        mesh_asset, mesh->second.revision(), std::move(after), std::move(merged.value()));
    if (!replaced) return replaced;
    mesh_patches_.push_back(std::move(record));
    return replaced;
}

core::Result<core::Revision> ProjectDocument::apply_vertex_merge_patch(
    std::uint64_t mesh_asset,
    const geometry::VertexMergePatch& patch) {
    if (mesh_patches_.size() >= kMaxSerializedMeshPatchRecords) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger is at its bounded capacity"));
    }
    const auto mesh = meshes_.find(mesh_asset);
    if (mesh == meshes_.end()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::not_found, "cannot patch a missing mesh asset"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    for (const auto& record : mesh_patches_) {
        const auto recorded_revision = persistent_mesh_patch_revision(record.patch_bytes);
        if (!recorded_revision) {
            return core::Result<core::Revision>::failure(recorded_revision.error());
        }
        if (record.mesh_asset == mesh_asset &&
            recorded_revision.value() == patch.expected_revision) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "project already contains a mesh patch for this source revision"));
        }
    }

    MeshPatchRecord record;
    record.mesh_asset = mesh_asset;
    record.patch_bytes = patch.serialize();
    std::uint64_t aggregate_patch_bytes = 0U;
    for (const auto& existing : mesh_patches_) {
        if (existing.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
            return core::Result<core::Revision>::failure(parse_error(
                "project mesh patch ledger exceeds its aggregate byte limit"));
        }
        aggregate_patch_bytes += static_cast<std::uint64_t>(existing.patch_bytes.size());
    }
    if (record.patch_bytes.empty() || record.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger exceeds its aggregate byte limit"));
    }
    record.patch_digest = digest_text(record.patch_bytes);
    record.source_mesh_digest = mesh_state_digest(mesh->second);
    if (patch.expected_revision != mesh->second.revision()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh patch revision does not match the current asset"));
    }

    geometry::EditableMesh after = mesh->second;
    auto merged = mesh_attributes::merge_vertices_with_attribute_transfer(
        after, patch.target, patch.source, patch.mode);
    if (!merged) return core::Result<core::Revision>::failure(merged.error());
    record.result_mesh_digest = mesh_state_digest(after);
    if (auto result = record.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    mesh_patches_.reserve(mesh_patches_.size() + 1U);
    topology_receipts_.reserve(topology_receipts_.size() + 1U);
    const auto replaced = replace_mesh_if_revision(
        mesh_asset, mesh->second.revision(), std::move(after), std::move(merged.value()));
    if (!replaced) return replaced;
    mesh_patches_.push_back(std::move(record));
    return replaced;
}

core::Result<core::Revision> ProjectDocument::apply_loop_cut_patch(
    std::uint64_t mesh_asset,
    const geometry::LoopCutPatch& patch) {
    if (mesh_patches_.size() >= kMaxSerializedMeshPatchRecords) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger is at its bounded capacity"));
    }
    const auto mesh = meshes_.find(mesh_asset);
    if (mesh == meshes_.end()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::not_found, "cannot patch a missing mesh asset"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    for (const auto& record : mesh_patches_) {
        const auto recorded_revision = persistent_mesh_patch_revision(record.patch_bytes);
        if (!recorded_revision) {
            return core::Result<core::Revision>::failure(recorded_revision.error());
        }
        if (record.mesh_asset == mesh_asset &&
            recorded_revision.value() == patch.expected_revision) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "project already contains a mesh patch for this source revision"));
        }
    }

    MeshPatchRecord record;
    record.mesh_asset = mesh_asset;
    record.patch_bytes = patch.serialize();
    std::uint64_t aggregate_patch_bytes = 0U;
    for (const auto& existing : mesh_patches_) {
        if (existing.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
            return core::Result<core::Revision>::failure(parse_error(
                "project mesh patch ledger exceeds its aggregate byte limit"));
        }
        aggregate_patch_bytes += static_cast<std::uint64_t>(existing.patch_bytes.size());
    }
    if (record.patch_bytes.empty() || record.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger exceeds its aggregate byte limit"));
    }
    record.patch_digest = digest_text(record.patch_bytes);
    record.source_mesh_digest = mesh_state_digest(mesh->second);
    if (patch.expected_revision != mesh->second.revision()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh patch revision does not match the current asset"));
    }
    geometry::EditableMesh after = mesh->second;
    auto cut = after.loop_cut(patch.edge);
    if (!cut) return core::Result<core::Revision>::failure(cut.error());
    record.result_mesh_digest = mesh_state_digest(after);
    if (auto result = record.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    mesh_patches_.reserve(mesh_patches_.size() + 1U);
    topology_receipts_.reserve(topology_receipts_.size() + 1U);
    const auto replaced = replace_mesh_if_revision(
        mesh_asset, mesh->second.revision(), std::move(after), std::move(cut.value()));
    if (!replaced) return replaced;
    mesh_patches_.push_back(std::move(record));
    return replaced;
}

core::Result<core::Revision> ProjectDocument::apply_knife_cut_patch(
    std::uint64_t mesh_asset,
    const geometry::KnifeCutPatch& patch) {
    if (mesh_patches_.size() >= kMaxSerializedMeshPatchRecords) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger is at its bounded capacity"));
    }
    const auto mesh = meshes_.find(mesh_asset);
    if (mesh == meshes_.end()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::not_found, "cannot patch a missing mesh asset"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    for (const auto& record : mesh_patches_) {
        const auto recorded_revision = persistent_mesh_patch_revision(record.patch_bytes);
        if (!recorded_revision) {
            return core::Result<core::Revision>::failure(recorded_revision.error());
        }
        if (record.mesh_asset == mesh_asset &&
            recorded_revision.value() == patch.expected_revision) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "project already contains a mesh patch for this source revision"));
        }
    }

    MeshPatchRecord record;
    record.mesh_asset = mesh_asset;
    record.patch_bytes = patch.serialize();
    std::uint64_t aggregate_patch_bytes = 0U;
    for (const auto& existing : mesh_patches_) {
        if (existing.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
            return core::Result<core::Revision>::failure(parse_error(
                "project mesh patch ledger exceeds its aggregate byte limit"));
        }
        aggregate_patch_bytes += static_cast<std::uint64_t>(existing.patch_bytes.size());
    }
    if (record.patch_bytes.empty() || record.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger exceeds its aggregate byte limit"));
    }
    record.patch_digest = digest_text(record.patch_bytes);
    record.source_mesh_digest = mesh_state_digest(mesh->second);
    if (patch.expected_revision != mesh->second.revision()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh patch revision does not match the current asset"));
    }

    geometry::EditableMesh after = mesh->second;
    auto cut = mesh_attributes::knife_cut_with_attribute_transfer(
        after, patch.face, patch.path);
    if (!cut) return core::Result<core::Revision>::failure(cut.error());
    record.result_mesh_digest = mesh_state_digest(after);
    if (auto result = record.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    mesh_patches_.reserve(mesh_patches_.size() + 1U);
    topology_receipts_.reserve(topology_receipts_.size() + 1U);
    const auto replaced = replace_mesh_if_revision(
        mesh_asset, mesh->second.revision(), std::move(after), std::move(cut.value()));
    if (!replaced) return replaced;
    mesh_patches_.push_back(std::move(record));
    return replaced;
}

core::Result<core::Revision> ProjectDocument::apply_bevel_edge_patch(
    std::uint64_t mesh_asset,
    const geometry::BevelEdgePatch& patch) {
    if (mesh_patches_.size() >= kMaxSerializedMeshPatchRecords) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger is at its bounded capacity"));
    }
    const auto mesh = meshes_.find(mesh_asset);
    if (mesh == meshes_.end()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::not_found, "cannot patch a missing mesh asset"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    for (const auto& record : mesh_patches_) {
        const auto recorded_revision = persistent_mesh_patch_revision(record.patch_bytes);
        if (!recorded_revision) {
            return core::Result<core::Revision>::failure(recorded_revision.error());
        }
        if (record.mesh_asset == mesh_asset &&
            recorded_revision.value() == patch.expected_revision) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "project already contains a mesh patch for this source revision"));
        }
    }

    MeshPatchRecord record;
    record.mesh_asset = mesh_asset;
    record.patch_bytes = patch.serialize();
    std::uint64_t aggregate_patch_bytes = 0U;
    for (const auto& existing : mesh_patches_) {
        if (existing.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
            return core::Result<core::Revision>::failure(parse_error(
                "project mesh patch ledger exceeds its aggregate byte limit"));
        }
        aggregate_patch_bytes += static_cast<std::uint64_t>(existing.patch_bytes.size());
    }
    if (record.patch_bytes.empty() || record.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger exceeds its aggregate byte limit"));
    }
    record.patch_digest = digest_text(record.patch_bytes);
    record.source_mesh_digest = mesh_state_digest(mesh->second);
    if (patch.expected_revision != mesh->second.revision()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh patch revision does not match the current asset"));
    }
    geometry::EditableMesh after = mesh->second;
    auto beveled = after.bevel_edge(patch.edge, patch.width, patch.segments);
    if (!beveled) return core::Result<core::Revision>::failure(beveled.error());
    record.result_mesh_digest = mesh_state_digest(after);
    if (auto result = record.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    mesh_patches_.reserve(mesh_patches_.size() + 1U);
    topology_receipts_.reserve(topology_receipts_.size() + 1U);
    const auto replaced = replace_mesh_if_revision(
        mesh_asset, mesh->second.revision(), std::move(after), std::move(beveled.value()));
    if (!replaced) return replaced;
    mesh_patches_.push_back(std::move(record));
    return replaced;
}

core::Result<core::Revision> ProjectDocument::apply_bevel_vertex_patch(
    std::uint64_t mesh_asset,
    const geometry::BevelVertexPatch& patch) {
    if (mesh_patches_.size() >= kMaxSerializedMeshPatchRecords) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger is at its bounded capacity"));
    }
    const auto mesh = meshes_.find(mesh_asset);
    if (mesh == meshes_.end()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::not_found, "cannot patch a missing mesh asset"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    for (const auto& record : mesh_patches_) {
        const auto recorded_revision = persistent_mesh_patch_revision(record.patch_bytes);
        if (!recorded_revision) {
            return core::Result<core::Revision>::failure(recorded_revision.error());
        }
        if (record.mesh_asset == mesh_asset &&
            recorded_revision.value() == patch.expected_revision) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "project already contains a mesh patch for this source revision"));
        }
    }

    MeshPatchRecord record;
    record.mesh_asset = mesh_asset;
    record.patch_bytes = patch.serialize();
    std::uint64_t aggregate_patch_bytes = 0U;
    for (const auto& existing : mesh_patches_) {
        if (existing.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
            return core::Result<core::Revision>::failure(parse_error(
                "project mesh patch ledger exceeds its aggregate byte limit"));
        }
        aggregate_patch_bytes += static_cast<std::uint64_t>(existing.patch_bytes.size());
    }
    if (record.patch_bytes.empty() || record.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger exceeds its aggregate byte limit"));
    }
    record.patch_digest = digest_text(record.patch_bytes);
    record.source_mesh_digest = mesh_state_digest(mesh->second);
    if (patch.expected_revision != mesh->second.revision()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh patch revision does not match the current asset"));
    }
    geometry::EditableMesh after = mesh->second;
    auto beveled = after.bevel_vertex(patch.vertex, patch.width, patch.segments);
    if (!beveled) return core::Result<core::Revision>::failure(beveled.error());
    record.result_mesh_digest = mesh_state_digest(after);
    if (auto result = record.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    mesh_patches_.reserve(mesh_patches_.size() + 1U);
    topology_receipts_.reserve(topology_receipts_.size() + 1U);
    const auto replaced = replace_mesh_if_revision(
        mesh_asset, mesh->second.revision(), std::move(after), std::move(beveled.value()));
    if (!replaced) return replaced;
    mesh_patches_.push_back(std::move(record));
    return replaced;
}

core::Result<core::Revision> ProjectDocument::apply_face_extrusion_patch(
    std::uint64_t mesh_asset,
    const geometry::FaceExtrusionPatch& patch) {
    if (mesh_patches_.size() >= kMaxSerializedMeshPatchRecords) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger is at its bounded capacity"));
    }
    const auto mesh = meshes_.find(mesh_asset);
    if (mesh == meshes_.end()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::not_found, "cannot patch a missing mesh asset"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    for (const auto& record : mesh_patches_) {
        const auto recorded_revision = persistent_mesh_patch_revision(record.patch_bytes);
        if (!recorded_revision) {
            return core::Result<core::Revision>::failure(recorded_revision.error());
        }
        if (record.mesh_asset == mesh_asset &&
            recorded_revision.value() == patch.expected_revision) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "project already contains a mesh patch for this source revision"));
        }
    }

    MeshPatchRecord record;
    record.mesh_asset = mesh_asset;
    record.patch_bytes = patch.serialize();
    std::uint64_t aggregate_patch_bytes = 0U;
    for (const auto& existing : mesh_patches_) {
        if (existing.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
            return core::Result<core::Revision>::failure(parse_error(
                "project mesh patch ledger exceeds its aggregate byte limit"));
        }
        aggregate_patch_bytes += static_cast<std::uint64_t>(existing.patch_bytes.size());
    }
    if (record.patch_bytes.empty() || record.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger exceeds its aggregate byte limit"));
    }
    record.patch_digest = digest_text(record.patch_bytes);
    record.source_mesh_digest = mesh_state_digest(mesh->second);
    if (patch.expected_revision != mesh->second.revision()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh patch revision does not match the current asset"));
    }
    geometry::EditableMesh after = mesh->second;
    auto extruded = after.extrude_face_with_receipt(patch.face, patch.distance);
    if (!extruded) return core::Result<core::Revision>::failure(extruded.error());
    record.result_mesh_digest = mesh_state_digest(after);
    if (auto result = record.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    mesh_patches_.reserve(mesh_patches_.size() + 1U);
    topology_receipts_.reserve(topology_receipts_.size() + 1U);
    const auto replaced = replace_mesh_if_revision(
        mesh_asset, mesh->second.revision(), std::move(after), std::move(extruded.value()));
    if (!replaced) return replaced;
    mesh_patches_.push_back(std::move(record));
    return replaced;
}

core::Result<core::Revision> ProjectDocument::apply_face_inset_patch(
    std::uint64_t mesh_asset,
    const geometry::FaceInsetPatch& patch) {
    if (mesh_patches_.size() >= kMaxSerializedMeshPatchRecords) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger is at its bounded capacity"));
    }
    const auto mesh = meshes_.find(mesh_asset);
    if (mesh == meshes_.end()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::not_found, "cannot patch a missing mesh asset"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    for (const auto& record : mesh_patches_) {
        const auto recorded_revision = persistent_mesh_patch_revision(record.patch_bytes);
        if (!recorded_revision) {
            return core::Result<core::Revision>::failure(recorded_revision.error());
        }
        if (record.mesh_asset == mesh_asset &&
            recorded_revision.value() == patch.expected_revision) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "project already contains a mesh patch for this source revision"));
        }
    }

    MeshPatchRecord record;
    record.mesh_asset = mesh_asset;
    record.patch_bytes = patch.serialize();
    std::uint64_t aggregate_patch_bytes = 0U;
    for (const auto& existing : mesh_patches_) {
        if (existing.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
            return core::Result<core::Revision>::failure(parse_error(
                "project mesh patch ledger exceeds its aggregate byte limit"));
        }
        aggregate_patch_bytes += static_cast<std::uint64_t>(existing.patch_bytes.size());
    }
    if (record.patch_bytes.empty() || record.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger exceeds its aggregate byte limit"));
    }
    record.patch_digest = digest_text(record.patch_bytes);
    record.source_mesh_digest = mesh_state_digest(mesh->second);
    if (patch.expected_revision != mesh->second.revision()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh patch revision does not match the current asset"));
    }
    geometry::EditableMesh after = mesh->second;
    auto inset = after.inset_face(patch.face, patch.distance);
    if (!inset) return core::Result<core::Revision>::failure(inset.error());
    record.result_mesh_digest = mesh_state_digest(after);
    if (auto result = record.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    mesh_patches_.reserve(mesh_patches_.size() + 1U);
    topology_receipts_.reserve(topology_receipts_.size() + 1U);
    const auto replaced = replace_mesh_if_revision(
        mesh_asset, mesh->second.revision(), std::move(after), std::move(inset.value()));
    if (!replaced) return replaced;
    mesh_patches_.push_back(std::move(record));
    return replaced;
}

core::Result<core::Revision> ProjectDocument::apply_face_poke_patch(
    std::uint64_t mesh_asset,
    const geometry::FacePokePatch& patch) {
    if (mesh_patches_.size() >= kMaxSerializedMeshPatchRecords) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger is at its bounded capacity"));
    }
    const auto mesh = meshes_.find(mesh_asset);
    if (mesh == meshes_.end()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::not_found, "cannot patch a missing mesh asset"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    for (const auto& record : mesh_patches_) {
        const auto recorded_revision = persistent_mesh_patch_revision(record.patch_bytes);
        if (!recorded_revision) {
            return core::Result<core::Revision>::failure(recorded_revision.error());
        }
        if (record.mesh_asset == mesh_asset &&
            recorded_revision.value() == patch.expected_revision) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "project already contains a mesh patch for this source revision"));
        }
    }

    MeshPatchRecord record;
    record.mesh_asset = mesh_asset;
    record.patch_bytes = patch.serialize();
    std::uint64_t aggregate_patch_bytes = 0U;
    for (const auto& existing : mesh_patches_) {
        if (existing.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
            return core::Result<core::Revision>::failure(parse_error(
                "project mesh patch ledger exceeds its aggregate byte limit"));
        }
        aggregate_patch_bytes += static_cast<std::uint64_t>(existing.patch_bytes.size());
    }
    if (record.patch_bytes.empty() || record.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger exceeds its aggregate byte limit"));
    }
    record.patch_digest = digest_text(record.patch_bytes);
    record.source_mesh_digest = mesh_state_digest(mesh->second);
    if (patch.expected_revision != mesh->second.revision()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh patch revision does not match the current asset"));
    }
    geometry::EditableMesh after = mesh->second;
    auto poked = after.poke_face(patch.face);
    if (!poked) return core::Result<core::Revision>::failure(poked.error());
    record.result_mesh_digest = mesh_state_digest(after);
    if (auto result = record.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    mesh_patches_.reserve(mesh_patches_.size() + 1U);
    topology_receipts_.reserve(topology_receipts_.size() + 1U);
    const auto replaced = replace_mesh_if_revision(
        mesh_asset, mesh->second.revision(), std::move(after), std::move(poked.value()));
    if (!replaced) return replaced;
    mesh_patches_.push_back(std::move(record));
    return replaced;
}

core::Result<core::Revision> ProjectDocument::apply_tri_to_quad_patch(
    std::uint64_t mesh_asset,
    const geometry::TriToQuadPatch& patch) {
    if (mesh_patches_.size() >= kMaxSerializedMeshPatchRecords) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger is at its bounded capacity"));
    }
    const auto mesh = meshes_.find(mesh_asset);
    if (mesh == meshes_.end()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::not_found, "cannot patch a missing mesh asset"));
    }
    if (auto result = patch.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    for (const auto& record : mesh_patches_) {
        const auto recorded_revision = persistent_mesh_patch_revision(record.patch_bytes);
        if (!recorded_revision) {
            return core::Result<core::Revision>::failure(recorded_revision.error());
        }
        if (record.mesh_asset == mesh_asset &&
            recorded_revision.value() == patch.expected_revision) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "project already contains a mesh patch for this source revision"));
        }
    }

    MeshPatchRecord record;
    record.mesh_asset = mesh_asset;
    record.patch_bytes = patch.serialize();
    std::uint64_t aggregate_patch_bytes = 0U;
    for (const auto& existing : mesh_patches_) {
        if (existing.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
            return core::Result<core::Revision>::failure(parse_error(
                "project mesh patch ledger exceeds its aggregate byte limit"));
        }
        aggregate_patch_bytes += static_cast<std::uint64_t>(existing.patch_bytes.size());
    }
    if (record.patch_bytes.empty() || record.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
        return core::Result<core::Revision>::failure(parse_error(
            "project mesh patch ledger exceeds its aggregate byte limit"));
    }
    record.patch_digest = digest_text(record.patch_bytes);
    record.source_mesh_digest = mesh_state_digest(mesh->second);
    if (patch.expected_revision != mesh->second.revision()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data, "mesh patch revision does not match the current asset"));
    }

    geometry::EditableMesh after = mesh->second;
    auto converted = after.tri_to_quad(patch.first_face, patch.second_face);
    if (!converted) return core::Result<core::Revision>::failure(converted.error());
    record.result_mesh_digest = mesh_state_digest(after);
    if (auto result = record.validate(); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    mesh_patches_.reserve(mesh_patches_.size() + 1U);
    topology_receipts_.reserve(topology_receipts_.size() + 1U);
    const auto replaced = replace_mesh_if_revision(
        mesh_asset, mesh->second.revision(), std::move(after), std::move(converted.value()));
    if (!replaced) return replaced;
    mesh_patches_.push_back(std::move(record));
    return replaced;
}

core::Result<void> ProjectDocument::remove_mesh(std::uint64_t mesh_asset) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (!meshes_.contains(mesh_asset)) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::not_found, "cannot remove a missing mesh asset"));
    }
    for (const auto& object : scene_.objects_sorted()) {
        if (object.mesh_asset.has_value() && *object.mesh_asset == mesh_asset) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "cannot remove a mesh asset still referenced by a scene object"));
        }
    }
    for (const auto& [identity, area] : world_model_.areas()) {
        static_cast<void>(identity);
        if (area.geometry.kind == world::AuthoringBindingKind::mesh_asset &&
            area.geometry.identity == mesh_asset) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "cannot remove a mesh asset referenced by a semantic area"));
        }
    }
    for (const auto& [identity, assignment] : material_catalog_.assignments()) {
        static_cast<void>(identity);
        if (assignment.key.mesh_asset == mesh_asset) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "cannot remove a mesh asset referenced by a material-region assignment"));
        }
    }
    for (const auto& [identity, variant] : material_catalog_.variants()) {
        static_cast<void>(identity);
        if (std::binary_search(
                variant.permitted_mesh_assets.begin(),
                variant.permitted_mesh_assets.end(), mesh_asset)) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "cannot remove a mesh asset referenced by a material-variant scope"));
        }
    }
    meshes_.erase(mesh_asset);
    sculpt_stacks_.erase(mesh_asset);
    garment_assets_.erase(mesh_asset);
    cloth_rest_pose_receipts_.erase(mesh_asset);
    topology_receipts_.erase(
        std::remove_if(
            topology_receipts_.begin(), topology_receipts_.end(),
            [mesh_asset](const TopologyReceiptRecord& record) {
                return record.mesh_asset == mesh_asset;
            }),
        topology_receipts_.end());
    mesh_patches_.erase(
        std::remove_if(
            mesh_patches_.begin(), mesh_patches_.end(),
            [mesh_asset](const MeshPatchRecord& record) {
                return record.mesh_asset == mesh_asset;
            }),
        mesh_patches_.end());
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::attach_mesh(scene::ObjectId object, std::uint64_t mesh_asset) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (mesh_asset == 0 || !meshes_.contains(mesh_asset)) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::not_found, "cannot attach a missing mesh asset"));
    }
    auto result = scene_.attach_mesh(object, mesh_asset);
    if (!result) {
        return result;
    }
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_evaluation_graph_digest(
    std::optional<assets::Sha256Digest> digest) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (digest.has_value() && digest->is_zero()) {
        return core::Result<void>::failure(
            invalid("evaluation graph digest must not be zero"));
    }
    if (evaluation_graph_digest_ == digest) {
        return core::Result<void>::success();
    }
    evaluation_graph_digest_ = digest;
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_scientific_model(
    scientific::ScientificModel model) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (auto result = model.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "scientific model admission"));
    }
    if (scientific_model_.serialize() == model.serialize()) {
        return core::Result<void>::success();
    }
    scientific_model_ = std::move(model);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_world_model(world::WorldModel model) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (auto result = model.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "world model admission"));
    }
    for (const auto& [identity, area] : model.areas()) {
        static_cast<void>(identity);
        if (area.geometry.kind == world::AuthoringBindingKind::scene_object &&
            scene_.find(scene::ObjectId{area.geometry.identity}) == nullptr) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::not_found,
                "semantic area references a missing scene object"));
        }
        if (area.geometry.kind == world::AuthoringBindingKind::mesh_asset &&
            !meshes_.contains(area.geometry.identity)) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::not_found,
                "semantic area references a missing mesh asset"));
        }
    }
    for (const auto& [identity, layer] : model.variant_layers()) {
        static_cast<void>(identity);
        if (layer.base_project_revision > revision_) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::stale_data,
                "variant layer base revision is newer than the project"));
        }
    }
    if (world_model_.serialize() == model.serialize()) {
        return core::Result<void>::success();
    }
    world_model_ = std::move(model);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_material_catalog(MaterialCatalog catalog) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (auto result = catalog.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "material catalog admission"));
    }
    for (const auto& [identity, variant] : catalog.variants()) {
        static_cast<void>(identity);
        for (const std::uint64_t mesh_asset : variant.permitted_mesh_assets) {
            if (!meshes_.contains(mesh_asset)) {
                return core::Result<void>::failure(Diagnostic(
                    ErrorCode::not_found,
                    "material variant scope references a missing mesh asset"));
            }
        }
    }
    for (const auto& [identity, assignment] : catalog.assignments()) {
        static_cast<void>(identity);
        const auto mesh = meshes_.find(assignment.key.mesh_asset);
        if (mesh == meshes_.end()) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::not_found,
                "material-region assignment references a missing mesh asset"));
        }
        if (!mesh_declares_material_region(mesh->second, assignment.key.region_id)) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::not_found,
                "material-region assignment references a missing authored region"));
        }
    }
    if (material_catalog_.serialize() == catalog.serialize()) {
        return core::Result<void>::success();
    }
    material_catalog_ = std::move(catalog);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_presentation_catalog(
    PresentationCatalog catalog) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (auto result = catalog.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "presentation catalog admission"));
    }
    for (const auto& [object, camera] : catalog.cameras()) {
        static_cast<void>(camera);
        if (scene_.find(object) == nullptr) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::not_found, "authored camera references a missing scene object"));
        }
    }
    for (const auto& [object, light] : catalog.lights()) {
        static_cast<void>(light);
        if (scene_.find(object) == nullptr) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::not_found, "authored light references a missing scene object"));
        }
    }
    if (presentation_catalog_.serialize() == catalog.serialize()) {
        return core::Result<void>::success();
    }
    presentation_catalog_ = std::move(catalog);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_authoring_standards(
    ProjectAuthoringStandards standards) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (auto result = standards.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "project authoring standards admission"));
    }
    for (const auto& [id, standard] : standards.records()) {
        static_cast<void>(id);
        if (standard.evidence.has_value() &&
            standard.evidence->source_revision > revision_) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::stale_data,
                "project standard evidence is newer than the project revision"));
        }
    }
    if (authoring_standards_.serialize() == standards.serialize()) {
        return core::Result<void>::success();
    }
    authoring_standards_ = std::move(standards);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_curve_path(geometry::CurvePath path) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (auto result = path.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "curve path admission"));
    }
    const std::string identity = path.source_identity;
    const auto existing = curve_paths_.find(identity);
    const auto lineage = curve_path_lineage_.find(identity);
    if (existing == curve_paths_.end()) {
        if (lineage != curve_path_lineage_.end() && lineage->second.exhausted()) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "curve source revision space is exhausted"));
        }
        const core::Revision expected = lineage == curve_path_lineage_.end()
            ? core::Revision{1U} : lineage->second.next();
        if (path.source_revision != expected) {
            return core::Result<void>::failure(invalid(
                "new curve source must advance its retained identity lineage"));
        }
        if (curve_paths_.size() >= kMaxSerializedCurvePaths) {
            return core::Result<void>::failure(parse_error(
                "project curve source count exceeds its limit"));
        }
    } else {
        if (lineage == curve_path_lineage_.end() ||
            lineage->second != existing->second.source_revision ||
            existing->second.source_revision.exhausted() ||
            path.source_revision != existing->second.source_revision.next()) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::stale_data,
                "curve source update must advance exactly one source revision"));
        }
    }

    const std::string encoded = path.serialize();
    if (encoded.size() > kMaxSerializedCurvePathBytes) {
        return core::Result<void>::failure(parse_error(
            "curve source payload exceeds its project byte limit"));
    }
    std::uint64_t aggregate_bytes = encoded.size();
    for (const auto& [stored_identity, stored_path] : curve_paths_) {
        if (stored_identity != identity) {
            const std::size_t stored_size = stored_path.serialize().size();
            if (stored_size > kMaxSerializedCurvePathAggregateBytes - aggregate_bytes) {
                return core::Result<void>::failure(parse_error(
                    "project curve sources exceed their aggregate byte limit"));
            }
            aggregate_bytes += static_cast<std::uint64_t>(stored_size);
        }
    }
    if (aggregate_bytes > kMaxSerializedCurvePathAggregateBytes) {
        return core::Result<void>::failure(parse_error(
            "project curve sources exceed their aggregate byte limit"));
    }
    if (lineage == curve_path_lineage_.end() &&
        curve_path_lineage_.size() >= kMaxSerializedCurvePathLineage) {
        return core::Result<void>::failure(parse_error(
            "project curve source lineage count exceeds its limit"));
    }

    curve_path_lineage_.insert_or_assign(identity, path.source_revision);
    curve_paths_.insert_or_assign(identity, std::move(path));
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::erase_curve_path_if_revision(
    std::string_view identity,
    core::Revision expected_source_revision) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    const std::string key(identity);
    const auto existing = curve_paths_.find(key);
    const auto lineage = curve_path_lineage_.find(key);
    if (existing == curve_paths_.end() || lineage == curve_path_lineage_.end() ||
        existing->second.source_revision != expected_source_revision ||
        lineage->second != expected_source_revision) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data,
            "curve source no longer matches the requested source revision"));
    }
    if (std::any_of(road_sources_.begin(), road_sources_.end(),
            [&key](const auto& entry) {
                return entry.second.centerline_identity == key;
            })) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state,
            "curve source cannot be removed while a road source references it"));
    }
    if (std::any_of(sweep_sources_.begin(), sweep_sources_.end(),
            [&key](const auto& entry) {
                return entry.second.centerline_identity == key;
            })) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state,
            "curve source cannot be removed while a sweep source references it"));
    }
    curve_paths_.erase(existing);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_road_source(RoadSource road) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (auto result = road.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "road source admission"));
    }
    const auto centerline = curve_paths_.find(road.centerline_identity);
    if (centerline == curve_paths_.end()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::not_found, "road source references a missing centerline"));
    }
    if (centerline->second.source_revision != road.centerline_revision) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data, "road source centerline revision is stale"));
    }

    const std::string identity = road.source_identity;
    const auto existing = road_sources_.find(identity);
    const auto lineage = road_source_lineage_.find(identity);
    if (existing == road_sources_.end()) {
        if (lineage != road_source_lineage_.end() && lineage->second.exhausted()) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "road source revision space is exhausted"));
        }
        const core::Revision expected = lineage == road_source_lineage_.end()
            ? core::Revision{1U} : lineage->second.next();
        if (road.source_revision != expected) {
            return core::Result<void>::failure(invalid(
                "new road source must advance its retained identity lineage"));
        }
        if (road_sources_.size() >= kMaxSerializedRoadSources) {
            return core::Result<void>::failure(parse_error(
                "project road source count exceeds its limit"));
        }
    } else if (lineage == road_source_lineage_.end() ||
        lineage->second != existing->second.source_revision ||
        existing->second.source_revision.exhausted() ||
        road.source_revision != existing->second.source_revision.next()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data,
            "road source update must advance exactly one source revision"));
    }

    const std::string encoded = road.serialize();
    if (encoded.size() > kMaxSerializedRoadSourceBytes) {
        return core::Result<void>::failure(parse_error(
            "road source payload exceeds its project byte limit"));
    }
    std::uint64_t aggregate_bytes = encoded.size();
    for (const auto& [stored_identity, stored_road] : road_sources_) {
        if (stored_identity != identity) {
            const std::size_t stored_size = stored_road.serialize().size();
            if (stored_size > kMaxSerializedRoadSourceAggregateBytes - aggregate_bytes) {
                return core::Result<void>::failure(parse_error(
                    "project road sources exceed their aggregate byte limit"));
            }
            aggregate_bytes += static_cast<std::uint64_t>(stored_size);
        }
    }
    if (aggregate_bytes > kMaxSerializedRoadSourceAggregateBytes) {
        return core::Result<void>::failure(parse_error(
            "project road sources exceed their aggregate byte limit"));
    }

    if (lineage == road_source_lineage_.end() &&
        road_source_lineage_.size() >= kMaxSerializedRoadSourceLineage) {
        return core::Result<void>::failure(parse_error(
            "project road-source lineage count exceeds its limit"));
    }
    road_source_lineage_.insert_or_assign(identity, road.source_revision);
    road_sources_.insert_or_assign(identity, std::move(road));
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::erase_road_source_if_revision(
    std::string_view identity,
    core::Revision expected_source_revision) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    const std::string key(identity);
    const auto existing = road_sources_.find(key);
    const auto lineage = road_source_lineage_.find(key);
    if (existing == road_sources_.end() || lineage == road_source_lineage_.end() ||
        existing->second.source_revision != expected_source_revision ||
        lineage->second != expected_source_revision) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data,
            "road source no longer matches the requested source revision"));
    }
    road_sources_.erase(existing);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_sweep_source(geometry::SweepSource source) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (auto result = source.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "sweep source admission"));
    }
    const auto centerline = curve_paths_.find(source.centerline_identity);
    if (centerline == curve_paths_.end()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::not_found, "sweep source references a missing centerline"));
    }
    if (centerline->second.source_revision != source.centerline_revision) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data, "sweep source centerline revision is stale"));
    }

    const std::string identity = source.source_identity;
    const auto existing = sweep_sources_.find(identity);
    const auto lineage = sweep_source_lineage_.find(identity);
    if (existing == sweep_sources_.end()) {
        if (lineage != sweep_source_lineage_.end() && lineage->second.exhausted()) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state, "sweep source revision space is exhausted"));
        }
        const core::Revision expected = lineage == sweep_source_lineage_.end()
            ? core::Revision{1U} : lineage->second.next();
        if (source.source_revision != expected) {
            return core::Result<void>::failure(invalid(
                "new sweep source must advance its retained identity lineage"));
        }
        if (sweep_sources_.size() >= kMaxSerializedSweepSources) {
            return core::Result<void>::failure(parse_error(
                "project sweep source count exceeds its limit"));
        }
    } else if (lineage == sweep_source_lineage_.end() ||
        lineage->second != existing->second.source_revision ||
        existing->second.source_revision.exhausted() ||
        source.source_revision != existing->second.source_revision.next()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data,
            "sweep source update must advance exactly one source revision"));
    }

    const std::string encoded = source.serialize();
    if (encoded.size() > kMaxSerializedSweepSourceBytes) {
        return core::Result<void>::failure(parse_error(
            "sweep source payload exceeds its project byte limit"));
    }
    std::uint64_t aggregate_bytes = encoded.size();
    for (const auto& [stored_identity, stored_source] : sweep_sources_) {
        if (stored_identity != identity) {
            const std::size_t stored_size = stored_source.serialize().size();
            if (stored_size > kMaxSerializedSweepSourceAggregateBytes - aggregate_bytes) {
                return core::Result<void>::failure(parse_error(
                    "project sweep sources exceed their aggregate byte limit"));
            }
            aggregate_bytes += static_cast<std::uint64_t>(stored_size);
        }
    }
    if (aggregate_bytes > kMaxSerializedSweepSourceAggregateBytes) {
        return core::Result<void>::failure(parse_error(
            "project sweep sources exceed their aggregate byte limit"));
    }
    if (lineage == sweep_source_lineage_.end() &&
        sweep_source_lineage_.size() >= kMaxSerializedSweepSourceLineage) {
        return core::Result<void>::failure(parse_error(
            "project sweep-source lineage count exceeds its limit"));
    }
    sweep_source_lineage_.insert_or_assign(identity, source.source_revision);
    sweep_sources_.insert_or_assign(identity, std::move(source));
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::erase_sweep_source_if_revision(
    std::string_view identity,
    core::Revision expected_source_revision) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    const std::string key(identity);
    const auto existing = sweep_sources_.find(key);
    const auto lineage = sweep_source_lineage_.find(key);
    if (existing == sweep_sources_.end() || lineage == sweep_source_lineage_.end() ||
        existing->second.source_revision != expected_source_revision ||
        lineage->second != expected_source_revision) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data,
            "sweep source no longer matches the requested source revision"));
    }
    sweep_sources_.erase(existing);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_sign_text_source(
    geometry::SignTextSource source) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (auto result = source.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "sign-text source admission"));
    }
    const std::string identity = source.source_identity;
    const auto existing = sign_text_sources_.find(identity);
    const auto lineage = sign_text_source_lineage_.find(identity);
    if (existing == sign_text_sources_.end()) {
        if (lineage != sign_text_source_lineage_.end() && lineage->second.exhausted()) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state, "sign-text source revision space is exhausted"));
        }
        const core::Revision expected = lineage == sign_text_source_lineage_.end()
            ? core::Revision{1U} : lineage->second.next();
        if (source.source_revision != expected) {
            return core::Result<void>::failure(invalid(
                "new sign-text source must advance its retained identity lineage"));
        }
        if (sign_text_sources_.size() >= kMaxSerializedSignTextSources) {
            return core::Result<void>::failure(parse_error(
                "project sign-text source count exceeds its limit"));
        }
    } else if (lineage == sign_text_source_lineage_.end() ||
        lineage->second != existing->second.source_revision ||
        existing->second.source_revision.exhausted() ||
        source.source_revision != existing->second.source_revision.next()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data,
            "sign-text source update must advance exactly one source revision"));
    }
    const std::string encoded = source.serialize();
    if (encoded.size() > kMaxSerializedSignTextSourceBytes) {
        return core::Result<void>::failure(parse_error(
            "sign-text source payload exceeds its project byte limit"));
    }
    std::uint64_t aggregate_bytes = encoded.size();
    for (const auto& [stored_identity, stored_source] : sign_text_sources_) {
        if (stored_identity == identity) continue;
        const std::size_t stored_size = stored_source.serialize().size();
        if (stored_size > kMaxSerializedSignTextSourceAggregateBytes - aggregate_bytes) {
            return core::Result<void>::failure(parse_error(
                "project sign-text sources exceed their aggregate byte limit"));
        }
        aggregate_bytes += static_cast<std::uint64_t>(stored_size);
    }
    if (aggregate_bytes > kMaxSerializedSignTextSourceAggregateBytes) {
        return core::Result<void>::failure(parse_error(
            "project sign-text sources exceed their aggregate byte limit"));
    }
    if (lineage == sign_text_source_lineage_.end() &&
        sign_text_source_lineage_.size() >= kMaxSerializedSignTextSourceLineage) {
        return core::Result<void>::failure(parse_error(
            "project sign-text source-lineage count exceeds its limit"));
    }
    sign_text_source_lineage_.insert_or_assign(identity, source.source_revision);
    sign_text_sources_.insert_or_assign(identity, std::move(source));
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::erase_sign_text_source_if_revision(
    std::string_view identity,
    core::Revision expected_source_revision) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    const std::string key(identity);
    const auto existing = sign_text_sources_.find(key);
    const auto lineage = sign_text_source_lineage_.find(key);
    if (existing == sign_text_sources_.end() || lineage == sign_text_source_lineage_.end() ||
        existing->second.source_revision != expected_source_revision ||
        lineage->second != expected_source_revision) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data,
            "sign-text source no longer matches the requested source revision"));
    }
    sign_text_sources_.erase(existing);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_spatial_field(
    spatial_field::ScalarField2D field) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (auto result = field.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "spatial field admission"));
    }
    const std::string identity = field.identity();
    const auto existing = spatial_fields_.find(identity);
    const auto lineage = spatial_field_lineage_.find(identity);
    const std::string encoded = field.serialize();
    if (existing != spatial_fields_.end() && existing->second.serialize() == encoded) {
        return core::Result<void>::success();
    }
    if (existing == spatial_fields_.end()) {
        if (lineage != spatial_field_lineage_.end() && lineage->second.exhausted()) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state,
                "spatial field source revision space is exhausted"));
        }
        const core::Revision expected = lineage == spatial_field_lineage_.end()
            ? core::Revision{1U} : lineage->second.next();
        if (field.source_revision() != expected) {
            return core::Result<void>::failure(invalid(
                "new spatial field must advance its retained identity lineage"));
        }
        if (spatial_fields_.size() >= kMaxSerializedSpatialFields) {
            return core::Result<void>::failure(parse_error(
                "project spatial-field count exceeds its limit"));
        }
    } else if (lineage == spatial_field_lineage_.end() ||
        lineage->second != existing->second.source_revision() ||
        existing->second.source_revision().exhausted() ||
        field.source_revision() != existing->second.source_revision().next()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data,
            "spatial field update must advance exactly one source revision"));
    }
    if (encoded.size() > kMaxSerializedSpatialFieldBytes) {
        return core::Result<void>::failure(parse_error(
            "spatial field payload exceeds its project byte limit"));
    }
    std::uint64_t aggregate_bytes = encoded.size();
    for (const auto& [stored_identity, stored_field] : spatial_fields_) {
        if (stored_identity == identity) continue;
        const std::size_t stored_size = stored_field.serialize().size();
        if (stored_size > kMaxSerializedSpatialFieldAggregateBytes - aggregate_bytes) {
            return core::Result<void>::failure(parse_error(
                "project spatial fields exceed their aggregate byte limit"));
        }
        aggregate_bytes += static_cast<std::uint64_t>(stored_size);
    }
    if (aggregate_bytes > kMaxSerializedSpatialFieldAggregateBytes) {
        return core::Result<void>::failure(parse_error(
            "project spatial fields exceed their aggregate byte limit"));
    }
    if (lineage == spatial_field_lineage_.end() &&
        spatial_field_lineage_.size() >= kMaxSerializedSpatialFieldLineage) {
        return core::Result<void>::failure(parse_error(
            "project spatial-field lineage count exceeds its limit"));
    }
    spatial_field_lineage_.insert_or_assign(identity, field.source_revision());
    spatial_fields_.insert_or_assign(identity, std::move(field));
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::erase_spatial_field_if_revision(
    std::string_view identity,
    core::Revision expected_source_revision) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    const auto existing = spatial_fields_.find(std::string(identity));
    const auto lineage = spatial_field_lineage_.find(std::string(identity));
    if (existing == spatial_fields_.end() || lineage == spatial_field_lineage_.end() ||
        existing->second.source_revision() != expected_source_revision ||
        lineage->second != expected_source_revision) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data,
            "spatial field no longer matches the requested source revision"));
    }
    spatial_fields_.erase(existing);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_semantic_sidecar(
    std::string serialized_sidecar,
    assets::Sha256Digest digest) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (serialized_sidecar.empty() ||
        serialized_sidecar.size() > kMaxSerializedSemanticSidecarBytes) {
        return core::Result<void>::failure(invalid(
            "semantic sidecar bytes are empty or exceed the serialized byte bound"));
    }
    if (digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "semantic sidecar digest must not be zero"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(serialized_sidecar.data());
    if (assets::sha256(std::span<const std::uint8_t>(bytes, serialized_sidecar.size())) != digest) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::validation_failed,
            "semantic sidecar digest does not bind the serialized bytes"));
    }
    if (semantic_sidecar_bytes_.has_value() &&
        *semantic_sidecar_bytes_ == serialized_sidecar &&
        semantic_sidecar_digest_.has_value() && *semantic_sidecar_digest_ == digest) {
        return core::Result<void>::success();
    }
    semantic_sidecar_bytes_ = std::move(serialized_sidecar);
    semantic_sidecar_digest_ = digest;
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_sculpt_stack(
    std::uint64_t mesh_asset,
    std::string serialized_stack,
    assets::Sha256Digest digest) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (mesh_asset == 0U || !meshes_.contains(mesh_asset)) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::not_found, "SculptStack target mesh asset does not exist"));
    }
    if (serialized_stack.empty() ||
        serialized_stack.size() > kMaxSerializedSculptStackBytes || digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "SculptStack bytes or digest are outside the supported bounds"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(serialized_stack.data());
    if (assets::sha256(std::span<const std::uint8_t>(bytes, serialized_stack.size())) != digest) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::validation_failed, "SculptStack digest does not bind its serialized bytes"));
    }
    const auto existing = sculpt_stacks_.find(mesh_asset);
    if (existing != sculpt_stacks_.end() && existing->second.bytes == serialized_stack &&
        existing->second.digest == digest) {
        return core::Result<void>::success();
    }
    if (existing == sculpt_stacks_.end() &&
        sculpt_stacks_.size() >= kMaxSerializedSculptStacks) {
        return core::Result<void>::failure(invalid(
            "project contains the maximum number of serialized SculptStacks"));
    }
    sculpt_stacks_[mesh_asset] = SerializedSculptStack{
        std::move(serialized_stack), digest};
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::erase_sculpt_stack_if_revision(
    std::uint64_t mesh_asset,
    core::Revision expected_revision) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (revision_ != expected_revision) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data,
            "SculptStack removal revision does not match the current project"));
    }
    const auto existing = sculpt_stacks_.find(mesh_asset);
    if (existing == sculpt_stacks_.end()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::not_found, "project SculptStack does not exist"));
    }
    sculpt_stacks_.erase(existing);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_garment_asset(
    std::uint64_t mesh_asset,
    std::string serialized_asset,
    assets::Sha256Digest digest) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (mesh_asset == 0U || !meshes_.contains(mesh_asset)) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::not_found, "GarmentAsset target mesh asset does not exist"));
    }
    if (serialized_asset.empty() ||
        serialized_asset.size() > kMaxSerializedGarmentAssetBytes || digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "GarmentAsset bytes or digest are outside the supported bounds"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(serialized_asset.data());
    if (assets::sha256(std::span<const std::uint8_t>(bytes, serialized_asset.size())) != digest) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::validation_failed,
            "GarmentAsset digest does not bind its serialized bytes"));
    }
    const auto existing = garment_assets_.find(mesh_asset);
    if (existing != garment_assets_.end() && existing->second.bytes == serialized_asset &&
        existing->second.digest == digest) {
        return core::Result<void>::success();
    }
    if (existing == garment_assets_.end() &&
        garment_assets_.size() >= kMaxSerializedGarmentAssets) {
        return core::Result<void>::failure(invalid(
            "project contains the maximum number of serialized GarmentAssets"));
    }
    garment_assets_[mesh_asset] = SerializedGarmentAsset{
        std::move(serialized_asset), digest};
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::insert_rig_module_template(
    std::string serialized_template) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    const auto decoded = parse_rig_module_template(serialized_template);
    if (!decoded) {
        return core::Result<void>::failure(decoded.error().with_context(
            "project rig-module template insert"));
    }
    const std::string& identity = decoded.value().name;
    if (rig_module_templates_.contains(identity)) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state,
            "rig-module template identity already exists; use revision-checked replacement"));
    }
    if (rig_module_templates_.size() >= kMaxSerializedRigModuleTemplates) {
        return core::Result<void>::failure(invalid(
            "project contains the maximum number of rig-module templates"));
    }
    std::uint64_t aggregate_bytes = 0U;
    for (const auto& [existing_identity, entry] : rig_module_templates_) {
        (void)existing_identity;
        if (entry.bytes.size() >
            kMaxSerializedRigModuleTemplateAggregateBytes - aggregate_bytes) {
            return core::Result<void>::failure(invalid(
                "project rig-module template bytes exceed their aggregate limit"));
        }
        aggregate_bytes += static_cast<std::uint64_t>(entry.bytes.size());
    }
    if (serialized_template.size() >
        kMaxSerializedRigModuleTemplateAggregateBytes - aggregate_bytes) {
        return core::Result<void>::failure(invalid(
            "project rig-module template bytes exceed their aggregate limit"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(serialized_template.data());
    const auto digest = assets::sha256(std::span<const std::uint8_t>(
        bytes, serialized_template.size()));
    rig_module_templates_.emplace(identity, SerializedRigModuleTemplate{
        core::Revision{1U}, std::move(serialized_template), digest});
    bump_revision();
    return core::Result<void>::success();
}

core::Result<core::Revision> ProjectDocument::replace_rig_module_template_if_revision(
    std::string_view identity,
    core::Revision expected_revision,
    assets::Sha256Digest expected_digest,
    std::string serialized_template) {
    if (revision_.exhausted()) {
        return core::Result<core::Revision>::failure(exhausted_revision());
    }
    const auto existing = rig_module_templates_.find(std::string(identity));
    if (existing == rig_module_templates_.end()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::not_found, "rig-module template identity does not exist"));
    }
    if (existing->second.revision != expected_revision ||
        existing->second.digest != expected_digest) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data,
            "rig-module template revision or digest no longer matches the caller's snapshot"));
    }
    if (existing->second.revision.exhausted()) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::invalid_state, "rig-module template revision space is exhausted"));
    }
    const auto decoded = parse_rig_module_template(serialized_template);
    if (!decoded) {
        return core::Result<core::Revision>::failure(decoded.error().with_context(
            "project rig-module template replacement"));
    }
    if (decoded.value().name != identity) {
        return core::Result<core::Revision>::failure(invalid(
            "replacement rig-module template name does not match its stable identity"));
    }
    if (existing->second.bytes == serialized_template) {
        return core::Result<core::Revision>::success(existing->second.revision);
    }
    std::uint64_t aggregate_bytes = static_cast<std::uint64_t>(
        existing->second.bytes.size());
    for (const auto& [existing_identity, entry] : rig_module_templates_) {
        if (existing_identity == identity) continue;
        if (entry.bytes.size() >
            kMaxSerializedRigModuleTemplateAggregateBytes - aggregate_bytes) {
            return core::Result<core::Revision>::failure(invalid(
                "project rig-module template bytes exceed their aggregate limit"));
        }
        aggregate_bytes += static_cast<std::uint64_t>(entry.bytes.size());
    }
    aggregate_bytes -= static_cast<std::uint64_t>(existing->second.bytes.size());
    if (serialized_template.size() >
        kMaxSerializedRigModuleTemplateAggregateBytes - aggregate_bytes) {
        return core::Result<core::Revision>::failure(invalid(
            "project rig-module template bytes exceed their aggregate limit"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(serialized_template.data());
    const auto digest = assets::sha256(std::span<const std::uint8_t>(
        bytes, serialized_template.size()));
    const core::Revision next_revision = existing->second.revision.next();
    existing->second = SerializedRigModuleTemplate{
        next_revision, std::move(serialized_template), digest};
    bump_revision();
    return core::Result<core::Revision>::success(next_revision);
}

core::Result<core::Revision> ProjectDocument::set_decal_placement(
    std::uint64_t placement_id,
    core::Revision expected_revision,
    std::string serialized_placement,
    assets::Sha256Digest digest) {
    if (revision_.exhausted()) {
        return core::Result<core::Revision>::failure(exhausted_revision());
    }
    if (placement_id == 0U || serialized_placement.empty() ||
        serialized_placement.size() > kMaxSerializedDecalPlacementBytes || digest.is_zero()) {
        return core::Result<core::Revision>::failure(invalid(
            "decal placement identity, bytes, or digest are outside the supported bounds"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(serialized_placement.data());
    if (assets::sha256(std::span<const std::uint8_t>(bytes, serialized_placement.size())) !=
        digest) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::validation_failed,
            "decal placement digest does not bind its serialized bytes"));
    }
    const auto existing = decal_placements_.find(placement_id);
    if (existing == decal_placements_.end()) {
        if (expected_revision != core::Revision{}) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::stale_data, "decal placement does not exist at the expected revision"));
        }
        if (decal_placements_.size() >= kMaxSerializedDecalPlacements) {
            return core::Result<core::Revision>::failure(invalid(
                "project contains the maximum number of decal placements"));
        }
    } else {
        if (existing->second.revision != expected_revision) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::stale_data, "decal placement revision no longer matches"));
        }
        if (existing->second.bytes == serialized_placement && existing->second.digest == digest) {
            return core::Result<core::Revision>::success(existing->second.revision);
        }
        if (existing->second.revision.exhausted()) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::invalid_state, "decal placement revision space is exhausted"));
        }
    }
    std::uint64_t aggregate_bytes = 0U;
    for (const auto& [id, entry] : decal_placements_) {
        if (id == placement_id) continue;
        if (entry.bytes.size() >
            kMaxSerializedDecalPlacementAggregateBytes - aggregate_bytes) {
            return core::Result<core::Revision>::failure(invalid(
                "decal placement bytes exceed their aggregate limit"));
        }
        aggregate_bytes += static_cast<std::uint64_t>(entry.bytes.size());
    }
    if (serialized_placement.size() >
        kMaxSerializedDecalPlacementAggregateBytes - aggregate_bytes) {
        return core::Result<core::Revision>::failure(invalid(
            "decal placement bytes exceed their aggregate limit"));
    }
    const core::Revision next_revision = existing == decal_placements_.end()
        ? core::Revision{1U} : existing->second.revision.next();
    decal_placements_[placement_id] = SerializedDecalPlacement{
        next_revision, std::move(serialized_placement), digest};
    bump_revision();
    return core::Result<core::Revision>::success(next_revision);
}

core::Result<core::Revision> ProjectDocument::set_pose_library(
    std::string identity,
    core::Revision expected_revision,
    std::string serialized_library,
    assets::Sha256Digest digest) {
    if (revision_.exhausted()) {
        return core::Result<core::Revision>::failure(exhausted_revision());
    }
    if (!valid_pose_library_identity(identity) ||
        serialized_library.empty() ||
        serialized_library.size() > pose::PoseLibrary::kMaxSerializedBytes ||
        digest.is_zero()) {
        return core::Result<core::Revision>::failure(invalid(
            "pose library identity, bytes, or digest are outside the supported bounds"));
    }
    if (auto bound = bind_opaque_record(serialized_library, digest, "pose library");
        !bound) {
        return core::Result<core::Revision>::failure(bound.error());
    }
    auto decoded = pose::PoseLibrary::deserialize(serialized_library);
    if (!decoded) {
        return core::Result<core::Revision>::failure(decoded.error().with_context(
            "project pose library"));
    }
    if (decoded.value().serialize() != serialized_library) {
        return core::Result<core::Revision>::failure(parse_error(
            "pose library bytes are not canonical"));
    }
    const auto existing = pose_libraries_.find(identity);
    if (existing == pose_libraries_.end()) {
        if (expected_revision != core::Revision{}) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::stale_data,
                "pose library does not exist at the expected revision"));
        }
        if (pose_libraries_.size() >= kMaxSerializedPoseLibraries) {
            return core::Result<core::Revision>::failure(invalid(
                "project contains the maximum number of pose libraries"));
        }
    } else {
        if (existing->second.revision != expected_revision) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::stale_data, "pose library revision no longer matches"));
        }
        if (existing->second.bytes == serialized_library &&
            existing->second.digest == digest) {
            return core::Result<core::Revision>::success(existing->second.revision);
        }
        if (existing->second.revision.exhausted()) {
            return core::Result<core::Revision>::failure(Diagnostic(
                ErrorCode::invalid_state, "pose library revision space is exhausted"));
        }
    }
    std::uint64_t aggregate_bytes = 0U;
    for (const auto& [existing_identity, entry] : pose_libraries_) {
        if (existing_identity == identity) continue;
        if (entry.bytes.size() >
            kMaxSerializedPoseLibraryAggregateBytes - aggregate_bytes) {
            return core::Result<core::Revision>::failure(invalid(
                "pose library bytes exceed their aggregate limit"));
        }
        aggregate_bytes += static_cast<std::uint64_t>(entry.bytes.size());
    }
    if (serialized_library.size() >
        kMaxSerializedPoseLibraryAggregateBytes - aggregate_bytes) {
        return core::Result<core::Revision>::failure(invalid(
            "pose library bytes exceed their aggregate limit"));
    }
    const core::Revision next_revision = existing == pose_libraries_.end()
        ? core::Revision{1U} : existing->second.revision.next();
    pose_libraries_[identity] = SerializedPoseLibrary{
        next_revision, std::move(serialized_library), digest};
    bump_revision();
    return core::Result<core::Revision>::success(next_revision);
}

core::Result<void> ProjectDocument::append_weight_edit_receipt(
    std::string serialized_receipt,
    assets::Sha256Digest digest) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (serialized_receipt.empty() ||
        serialized_receipt.size() > weights::WeightEditReceipt::kMaxSerializedBytes ||
        digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "weight edit receipt bytes or digest are outside the supported bounds"));
    }
    if (auto bound = bind_opaque_record(serialized_receipt, digest, "weight edit receipt");
        !bound) {
        return core::Result<void>::failure(bound.error());
    }
    auto decoded = weights::WeightEditReceipt::deserialize(serialized_receipt);
    if (!decoded) {
        return core::Result<void>::failure(decoded.error().with_context(
            "project weight edit receipt"));
    }
    if (weight_edit_receipts_.size() >= kMaxSerializedWeightEditReceipts) {
        return core::Result<void>::failure(invalid(
            "project contains the maximum number of weight edit receipts"));
    }
    std::uint64_t aggregate_bytes = 0U;
    for (const auto& record : weight_edit_receipts_) {
        if (record.bytes.size() >
            kMaxSerializedWeightEditReceiptAggregateBytes - aggregate_bytes) {
            return core::Result<void>::failure(invalid(
                "weight edit receipt bytes exceed their aggregate limit"));
        }
        aggregate_bytes += static_cast<std::uint64_t>(record.bytes.size());
    }
    if (serialized_receipt.size() >
        kMaxSerializedWeightEditReceiptAggregateBytes - aggregate_bytes) {
        return core::Result<void>::failure(invalid(
            "weight edit receipt bytes exceed their aggregate limit"));
    }
    weight_edit_receipts_.push_back(SerializedWeightEditReceipt{
        std::move(serialized_receipt), digest});
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::append_curve_edit_receipt(
    std::string serialized_receipt,
    assets::Sha256Digest digest) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (serialized_receipt.empty() ||
        serialized_receipt.size() > motion::CurveEditReceipt::kMaxSerializedBytes ||
        digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "curve edit receipt bytes or digest are outside the supported bounds"));
    }
    if (auto bound = bind_opaque_record(serialized_receipt, digest, "curve edit receipt");
        !bound) {
        return core::Result<void>::failure(bound.error());
    }
    auto decoded = motion::CurveEditReceipt::deserialize(serialized_receipt);
    if (!decoded) {
        return core::Result<void>::failure(decoded.error().with_context(
            "project curve edit receipt"));
    }
    if (curve_edit_receipts_.size() >= kMaxSerializedCurveEditReceipts) {
        return core::Result<void>::failure(invalid(
            "project contains the maximum number of curve edit receipts"));
    }
    std::uint64_t aggregate_bytes = 0U;
    for (const auto& record : curve_edit_receipts_) {
        if (record.bytes.size() >
            kMaxSerializedCurveEditReceiptAggregateBytes - aggregate_bytes) {
            return core::Result<void>::failure(invalid(
                "curve edit receipt bytes exceed their aggregate limit"));
        }
        aggregate_bytes += static_cast<std::uint64_t>(record.bytes.size());
    }
    if (serialized_receipt.size() >
        kMaxSerializedCurveEditReceiptAggregateBytes - aggregate_bytes) {
        return core::Result<void>::failure(invalid(
            "curve edit receipt bytes exceed their aggregate limit"));
    }
    curve_edit_receipts_.push_back(SerializedCurveEditReceipt{
        std::move(serialized_receipt), digest});
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_cloth_rest_pose_receipt(
    std::uint64_t mesh_asset,
    std::string serialized_receipt,
    assets::Sha256Digest digest) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (mesh_asset == 0U || !meshes_.contains(mesh_asset)) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::not_found,
            "cloth rest-pose receipt target mesh asset does not exist"));
    }
    if (serialized_receipt.empty() ||
        serialized_receipt.size() > kMaxSerializedClothRestPoseReceiptBytes ||
        digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "cloth rest-pose receipt bytes or digest are outside the supported bounds"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(serialized_receipt.data());
    if (assets::sha256(std::span<const std::uint8_t>(bytes, serialized_receipt.size())) != digest) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::validation_failed,
            "cloth rest-pose receipt digest does not bind its serialized bytes"));
    }
    const auto existing = cloth_rest_pose_receipts_.find(mesh_asset);
    if (existing != cloth_rest_pose_receipts_.end() &&
        existing->second.bytes == serialized_receipt && existing->second.digest == digest) {
        return core::Result<void>::success();
    }
    if (existing == cloth_rest_pose_receipts_.end() &&
        cloth_rest_pose_receipts_.size() >= kMaxSerializedClothRestPoseReceipts) {
        return core::Result<void>::failure(invalid(
            "project contains the maximum number of cloth rest-pose receipts"));
    }
    cloth_rest_pose_receipts_[mesh_asset] = SerializedClothRestPoseReceipt{
        std::move(serialized_receipt), digest};
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_engineering_authoring(
    std::string serialized_authoring,
    assets::Sha256Digest digest) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (serialized_authoring.empty() ||
        serialized_authoring.size() > kMaxSerializedEngineeringAuthoringBytes ||
        digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "engineering authoring bytes or digest are outside the supported bounds"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(serialized_authoring.data());
    if (assets::sha256(std::span<const std::uint8_t>(bytes, serialized_authoring.size())) != digest) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::validation_failed,
            "engineering authoring digest does not bind the serialized bytes"));
    }
    const auto decoded = engineering::AuthoringDocument::deserialize(serialized_authoring);
    if (!decoded) {
        return core::Result<void>::failure(decoded.error().with_context(
            "engineering authoring admission"));
    }
    if (engineering_authoring_.has_value() &&
        engineering_authoring_->bytes == serialized_authoring &&
        engineering_authoring_->digest == digest) {
        return core::Result<void>::success();
    }
    engineering_authoring_ = SerializedEngineeringAuthoring{
        std::move(serialized_authoring), digest};
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::erase_engineering_authoring_if_revision(
    core::Revision expected_revision) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (revision_ != expected_revision) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data,
            "engineering authoring removal revision does not match the current project"));
    }
    if (!engineering_authoring_.has_value()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::not_found, "project engineering authoring does not exist"));
    }
    engineering_authoring_.reset();
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_condition_authoring(
    std::string serialized_authoring,
    assets::Sha256Digest digest) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (serialized_authoring.empty() ||
        serialized_authoring.size() > kMaxSerializedConditionAuthoringBytes ||
        digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "CONDITION authoring bytes or digest are outside the supported bounds"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(serialized_authoring.data());
    if (assets::sha256(std::span<const std::uint8_t>(bytes, serialized_authoring.size())) != digest) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::validation_failed,
            "CONDITION authoring digest does not bind the serialized bytes"));
    }
    const auto decoded = condition::AuthoringDocument::deserialize(serialized_authoring);
    if (!decoded) {
        return core::Result<void>::failure(decoded.error().with_context(
            "CONDITION authoring admission"));
    }
    if (condition_authoring_.has_value() &&
        condition_authoring_->bytes == serialized_authoring &&
        condition_authoring_->digest == digest) {
        return core::Result<void>::success();
    }
    condition_authoring_ = SerializedConditionAuthoring{
        std::move(serialized_authoring), digest};
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::erase_condition_authoring_if_revision(
    core::Revision expected_revision) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (revision_ != expected_revision) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data,
            "CONDITION authoring removal revision does not match the current project"));
    }
    if (!condition_authoring_.has_value()) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::not_found, "project CONDITION authoring does not exist"));
    }
    condition_authoring_.reset();
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_biological_authoring(
    biological::BiologicalAuthoringDocument source) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    if (auto result = source.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "biological authoring admission"));
    }
    if (source.source.revision.value() == 0U) {
        return core::Result<void>::failure(invalid(
            "biological authoring source revision must be non-zero"));
    }

    const BiologicalAuthoringKey key{
        source.source.source_namespace, source.source.asset_id};
    const auto existing = biological_authoring_sources_.find(key);
    const auto lineage = biological_authoring_lineage_.find(key);
    if (existing != biological_authoring_sources_.end()) {
        if (existing->second.bytes == source.serialize()) {
            return core::Result<void>::success();
        }
        auto previous = biological::BiologicalAuthoringDocument::deserialize(
            existing->second.bytes);
        if (!previous) {
            return core::Result<void>::failure(previous.error().with_context(
                "stored biological authoring source"));
        }
        if (lineage == biological_authoring_lineage_.end() ||
            lineage->second != previous.value().source.revision ||
            lineage->second.exhausted() ||
            source.source.revision != lineage->second.next()) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::stale_data,
                "biological authoring update must advance exactly one retained source revision"));
        }
        if (auto result = biological::verify_reimport_continuity(
                previous.value(), source); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "biological authoring reimport continuity"));
        }
    } else if (lineage != biological_authoring_lineage_.end()) {
        if (lineage->second.exhausted() ||
            source.source.revision != lineage->second.next()) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::stale_data,
                "recreated biological source must advance its retained identity lineage"));
        }
    } else if (biological_authoring_sources_.size() >=
        kMaxSerializedBiologicalAuthoringSources) {
        return core::Result<void>::failure(parse_error(
            "project biological authoring source count exceeds its limit"));
    }

    if (lineage == biological_authoring_lineage_.end() &&
        biological_authoring_lineage_.size() >=
            kMaxSerializedBiologicalAuthoringSources) {
        return core::Result<void>::failure(parse_error(
            "project biological authoring lineage count exceeds its limit"));
    }
    const std::string encoded = source.serialize();
    if (encoded.empty() || encoded.size() > kMaxSerializedBiologicalAuthoringBytes) {
        return core::Result<void>::failure(parse_error(
            "biological authoring source exceeds its project byte limit"));
    }
    std::uint64_t aggregate_bytes = static_cast<std::uint64_t>(encoded.size());
    for (const auto& [stored_key, stored_source] : biological_authoring_sources_) {
        if (stored_key == key) continue;
        if (stored_source.bytes.size() >
            kMaxSerializedBiologicalAuthoringAggregateBytes - aggregate_bytes) {
            return core::Result<void>::failure(parse_error(
                "project biological authoring sources exceed their aggregate byte limit"));
        }
        aggregate_bytes += static_cast<std::uint64_t>(stored_source.bytes.size());
    }
    if (aggregate_bytes > kMaxSerializedBiologicalAuthoringAggregateBytes) {
        return core::Result<void>::failure(parse_error(
            "project biological authoring sources exceed their aggregate byte limit"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
    const assets::Sha256Digest digest = assets::sha256(
        std::span<const std::uint8_t>(bytes, encoded.size()));
    biological_authoring_lineage_.insert_or_assign(key, source.source.revision);
    biological_authoring_sources_.insert_or_assign(key,
        SerializedBiologicalAuthoring{encoded, digest});
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::erase_biological_authoring_if_revision(
    std::string_view source_namespace,
    std::uint64_t asset_id,
    core::Revision expected_source_revision) {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    const BiologicalAuthoringKey key{std::string(source_namespace), asset_id};
    const auto existing = biological_authoring_sources_.find(key);
    const auto lineage = biological_authoring_lineage_.find(key);
    if (existing == biological_authoring_sources_.end() ||
        lineage == biological_authoring_lineage_.end() ||
        lineage->second != expected_source_revision) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data,
            "biological authoring source no longer matches the requested source revision"));
    }
    const auto source = biological::BiologicalAuthoringDocument::deserialize(
        existing->second.bytes);
    if (!source || source.value().source.revision != expected_source_revision) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::stale_data,
            "biological authoring source revision is stale or malformed"));
    }
    biological_authoring_sources_.erase(existing);
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::apply_source_reimport_plan(
    const assets::SourceReimportPlan& plan) {
    const auto updated = assets::apply_source_reimport_plan(source_asset_registry_, plan);
    if (!updated) return core::Result<void>::failure(updated.error());
    if (updated.value().serialize() == source_asset_registry_.serialize()) {
        return core::Result<void>::success();
    }
    if (revision_.exhausted()) {
        return core::Result<void>::failure(exhausted_revision());
    }
    source_asset_registry_ = std::move(updated.value());
    bump_revision();
    return core::Result<void>::success();
}

core::Result<void> ProjectDocument::set_object_transform(
    scene::ObjectId object,
    core::Transform transform) {
    auto result = set_object_transform_if_revision(object, revision_, transform);
    if (!result) {
        return core::Result<void>::failure(result.error());
    }
    return core::Result<void>::success();
}

core::Result<core::Revision> ProjectDocument::set_object_transform_if_revision(
    scene::ObjectId object,
    core::Revision expected_revision,
    core::Transform transform) {
    if (revision_.exhausted()) {
        return core::Result<core::Revision>::failure(exhausted_revision());
    }
    if (revision_ != expected_revision) {
        return core::Result<core::Revision>::failure(Diagnostic(
            ErrorCode::stale_data,
            "object transform revision does not match the current project"));
    }
    if (auto result = scene_.set_local_transform(object, transform); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    bump_revision();
    return core::Result<core::Revision>::success(revision_);
}

core::Result<void> ProjectDocument::validate() const {
    if (revision_.exhausted()) {
        return core::Result<void>::failure(parse_error("project revision space is exhausted"));
    }
    if (name_.empty()) {
        return core::Result<void>::failure(parse_error("project name is empty"));
    }
    if (!valid_source_namespace(source_namespace_)) {
        return core::Result<void>::failure(parse_error(
            "project source namespace is invalid"));
    }
    if (auto result = source_asset_registry_.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "project source asset registry"));
    }
    if (auto result = scene_.validate(); !result) {
        return result;
    }
    for (const auto& [mesh_id, mesh] : meshes_) {
        if (mesh_id == 0) {
            return core::Result<void>::failure(parse_error("project contains a zero mesh asset id"));
        }
        if (auto result = mesh.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context("mesh asset"));
        }
        if (mesh.revision().exhausted()) {
            return core::Result<void>::failure(parse_error("mesh revision space is exhausted"));
        }
    }
    if (topology_receipts_.size() > kMaxSerializedTopologyReceipts) {
        return core::Result<void>::failure(parse_error(
            "project contains too many topology receipt records"));
    }
    std::set<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> receipt_keys;
    for (const auto& record : topology_receipts_) {
        if (auto result = record.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "project topology receipt"));
        }
        const auto mesh = meshes_.find(record.mesh_asset);
        if (mesh == meshes_.end()) {
            return core::Result<void>::failure(parse_error(
                "project topology receipt references a missing mesh asset"));
        }
        if (record.receipt.revision_after > mesh->second.revision()) {
            return core::Result<void>::failure(parse_error(
                "project topology receipt is newer than its mesh asset"));
        }
        const auto key = std::make_tuple(
            record.mesh_asset, record.receipt.revision_before.value(),
            record.receipt.revision_after.value());
        if (!receipt_keys.insert(key).second) {
            return core::Result<void>::failure(parse_error(
                "project contains a duplicate topology receipt record"));
        }
    }
    if (mesh_patches_.size() > kMaxSerializedMeshPatchRecords) {
        return core::Result<void>::failure(parse_error(
            "project contains too many persistent mesh patch records"));
    }
    std::uint64_t aggregate_patch_bytes = 0U;
    std::set<std::pair<std::uint64_t, std::uint64_t>> patch_keys;
    for (const auto& record : mesh_patches_) {
        if (auto result = record.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "project persistent mesh patch"));
        }
        if (record.patch_bytes.size() >
            kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
            return core::Result<void>::failure(parse_error(
                "project persistent mesh patch bytes exceed their aggregate limit"));
        }
        aggregate_patch_bytes += static_cast<std::uint64_t>(record.patch_bytes.size());
        const auto mesh = meshes_.find(record.mesh_asset);
        const auto patch_revision = persistent_mesh_patch_revision(record.patch_bytes);
        if (mesh == meshes_.end() || !patch_revision) {
            return core::Result<void>::failure(parse_error(
                "project persistent mesh patch references an invalid mesh or patch"));
        }
        if (patch_revision.value().next() > mesh->second.revision()) {
            return core::Result<void>::failure(parse_error(
                "project persistent mesh patch is newer than its mesh asset"));
        }
        if (!patch_keys.emplace(record.mesh_asset,
                                patch_revision.value().value()).second) {
            return core::Result<void>::failure(parse_error(
                "project contains a duplicate persistent mesh patch source revision"));
        }
    }
    if (evaluation_graph_digest_.has_value() && evaluation_graph_digest_->is_zero()) {
        return core::Result<void>::failure(parse_error(
            "project evaluation graph digest must not be zero"));
    }
    if (semantic_sidecar_digest_.has_value() != semantic_sidecar_bytes_.has_value()) {
        return core::Result<void>::failure(parse_error(
            "project semantic sidecar bytes and digest must be set together"));
    }
    if (semantic_sidecar_bytes_.has_value()) {
        if (semantic_sidecar_bytes_->empty() ||
            semantic_sidecar_bytes_->size() > kMaxSerializedSemanticSidecarBytes) {
            return core::Result<void>::failure(parse_error(
                "project semantic sidecar bytes exceed the serialized byte bound"));
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(
            semantic_sidecar_bytes_->data());
        if (assets::sha256(std::span<const std::uint8_t>(
                bytes, semantic_sidecar_bytes_->size())) != *semantic_sidecar_digest_) {
            return core::Result<void>::failure(parse_error(
                "project semantic sidecar digest does not bind its bytes"));
        }
    }
    if (sculpt_stacks_.size() > kMaxSerializedSculptStacks) {
        return core::Result<void>::failure(parse_error(
            "project contains too many serialized SculptStacks"));
    }
    for (const auto& [mesh_asset, stack] : sculpt_stacks_) {
        if (mesh_asset == 0U || !meshes_.contains(mesh_asset)) {
            return core::Result<void>::failure(parse_error(
                "SculptStack references a missing mesh asset"));
        }
        if (stack.bytes.empty() || stack.bytes.size() > kMaxSerializedSculptStackBytes ||
            stack.digest.is_zero()) {
            return core::Result<void>::failure(parse_error(
                "project SculptStack bytes or digest are outside the serialized bounds"));
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(stack.bytes.data());
        if (assets::sha256(std::span<const std::uint8_t>(bytes, stack.bytes.size())) !=
            stack.digest) {
            return core::Result<void>::failure(parse_error(
                "project SculptStack digest does not bind its serialized bytes"));
        }
    }
    if (garment_assets_.size() > kMaxSerializedGarmentAssets) {
        return core::Result<void>::failure(parse_error(
            "project contains too many serialized GarmentAssets"));
    }
    for (const auto& [mesh_asset, asset] : garment_assets_) {
        if (mesh_asset == 0U || !meshes_.contains(mesh_asset)) {
            return core::Result<void>::failure(parse_error(
                "GarmentAsset references a missing mesh asset"));
        }
        if (asset.bytes.empty() || asset.bytes.size() > kMaxSerializedGarmentAssetBytes ||
            asset.digest.is_zero()) {
            return core::Result<void>::failure(parse_error(
                "project GarmentAsset bytes or digest are outside the serialized bounds"));
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(asset.bytes.data());
        if (assets::sha256(std::span<const std::uint8_t>(bytes, asset.bytes.size())) !=
            asset.digest) {
            return core::Result<void>::failure(parse_error(
                "project GarmentAsset digest does not bind its serialized bytes"));
        }
    }
    if (rig_module_templates_.size() > kMaxSerializedRigModuleTemplates) {
        return core::Result<void>::failure(parse_error(
            "project contains too many serialized rig-module templates"));
    }
    std::uint64_t aggregate_rig_module_bytes = 0U;
    for (const auto& [identity, entry] : rig_module_templates_) {
        if (entry.revision.value() == 0U || entry.revision.exhausted() ||
            entry.bytes.empty() ||
            entry.bytes.size() > character::RigModuleTemplate::kMaxSerializedBytes ||
            entry.bytes.size() >
                kMaxSerializedRigModuleTemplateAggregateBytes - aggregate_rig_module_bytes ||
            entry.digest.is_zero()) {
            return core::Result<void>::failure(parse_error(
                "project rig-module template revision, bytes, or digest exceed their bounds"));
        }
        aggregate_rig_module_bytes += static_cast<std::uint64_t>(entry.bytes.size());
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(entry.bytes.data());
        if (assets::sha256(std::span<const std::uint8_t>(bytes, entry.bytes.size())) !=
            entry.digest) {
            return core::Result<void>::failure(parse_error(
                "project rig-module template digest does not bind its bytes"));
        }
        const auto decoded = parse_rig_module_template(entry.bytes);
        if (!decoded || decoded.value().name != identity) {
            return core::Result<void>::failure(parse_error(
                "project rig-module template identity or canonical payload is invalid"));
        }
    }
    if (decal_placements_.size() > kMaxSerializedDecalPlacements) {
        return core::Result<void>::failure(parse_error(
            "project contains too many serialized decal placements"));
    }
    std::uint64_t aggregate_decal_bytes = 0U;
    for (const auto& [placement_id, entry] : decal_placements_) {
        if (placement_id == 0U || entry.revision.value() == 0U || entry.revision.exhausted() ||
            entry.bytes.empty() || entry.bytes.size() > kMaxSerializedDecalPlacementBytes ||
            entry.bytes.size() >
                kMaxSerializedDecalPlacementAggregateBytes - aggregate_decal_bytes ||
            entry.digest.is_zero()) {
            return core::Result<void>::failure(parse_error(
                "project decal placement identity, revision, or bytes exceed their bounds"));
        }
        aggregate_decal_bytes += static_cast<std::uint64_t>(entry.bytes.size());
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(entry.bytes.data());
        if (assets::sha256(std::span<const std::uint8_t>(bytes, entry.bytes.size())) !=
            entry.digest) {
            return core::Result<void>::failure(parse_error(
                "project decal placement digest does not bind its bytes"));
        }
    }
    if (pose_libraries_.size() > kMaxSerializedPoseLibraries) {
        return core::Result<void>::failure(parse_error(
            "project contains too many pose libraries"));
    }
    std::uint64_t aggregate_pose_library_bytes = 0U;
    for (const auto& [identity, entry] : pose_libraries_) {
        if (!valid_pose_library_identity(identity) || entry.revision.value() == 0U ||
            entry.revision.exhausted() || entry.bytes.empty() ||
            entry.bytes.size() > pose::PoseLibrary::kMaxSerializedBytes ||
            entry.bytes.size() >
                kMaxSerializedPoseLibraryAggregateBytes - aggregate_pose_library_bytes ||
            entry.digest.is_zero()) {
            return core::Result<void>::failure(parse_error(
                "project pose library identity, revision, or bytes exceed their bounds"));
        }
        aggregate_pose_library_bytes += static_cast<std::uint64_t>(entry.bytes.size());
        const auto decoded = pose::PoseLibrary::deserialize(entry.bytes);
        if (!decoded || decoded.value().serialize() != entry.bytes) {
            return core::Result<void>::failure(parse_error(
                "project pose library payload is not a canonical pose library"));
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(entry.bytes.data());
        if (assets::sha256(std::span<const std::uint8_t>(bytes, entry.bytes.size())) !=
            entry.digest) {
            return core::Result<void>::failure(parse_error(
                "project pose library digest does not bind its bytes"));
        }
    }
    if (weight_edit_receipts_.size() > kMaxSerializedWeightEditReceipts) {
        return core::Result<void>::failure(parse_error(
            "project contains too many weight edit receipts"));
    }
    std::uint64_t aggregate_weight_receipt_bytes = 0U;
    for (const auto& record : weight_edit_receipts_) {
        if (record.bytes.empty() ||
            record.bytes.size() > weights::WeightEditReceipt::kMaxSerializedBytes ||
            record.bytes.size() >
                kMaxSerializedWeightEditReceiptAggregateBytes -
                    aggregate_weight_receipt_bytes ||
            record.digest.is_zero()) {
            return core::Result<void>::failure(parse_error(
                "project weight edit receipt bytes or digest exceed their bounds"));
        }
        aggregate_weight_receipt_bytes += static_cast<std::uint64_t>(record.bytes.size());
        const auto decoded = weights::WeightEditReceipt::deserialize(record.bytes);
        if (!decoded) {
            return core::Result<void>::failure(parse_error(
                "project weight edit receipt payload is invalid"));
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(record.bytes.data());
        if (assets::sha256(std::span<const std::uint8_t>(bytes, record.bytes.size())) !=
            record.digest) {
            return core::Result<void>::failure(parse_error(
                "project weight edit receipt digest does not bind its serialized bytes"));
        }
    }
    if (curve_edit_receipts_.size() > kMaxSerializedCurveEditReceipts) {
        return core::Result<void>::failure(parse_error(
            "project contains too many curve edit receipts"));
    }
    std::uint64_t aggregate_curve_receipt_bytes = 0U;
    for (const auto& record : curve_edit_receipts_) {
        if (record.bytes.empty() ||
            record.bytes.size() > motion::CurveEditReceipt::kMaxSerializedBytes ||
            record.bytes.size() >
                kMaxSerializedCurveEditReceiptAggregateBytes -
                    aggregate_curve_receipt_bytes ||
            record.digest.is_zero()) {
            return core::Result<void>::failure(parse_error(
                "project curve edit receipt bytes or digest exceed their bounds"));
        }
        aggregate_curve_receipt_bytes += static_cast<std::uint64_t>(record.bytes.size());
        const auto decoded = motion::CurveEditReceipt::deserialize(record.bytes);
        if (!decoded) {
            return core::Result<void>::failure(parse_error(
                "project curve edit receipt payload is invalid"));
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(record.bytes.data());
        if (assets::sha256(std::span<const std::uint8_t>(bytes, record.bytes.size())) !=
            record.digest) {
            return core::Result<void>::failure(parse_error(
                "project curve edit receipt digest does not bind its serialized bytes"));
        }
    }
    if (cloth_rest_pose_receipts_.size() > kMaxSerializedClothRestPoseReceipts) {
        return core::Result<void>::failure(parse_error(
            "project contains too many cloth rest-pose receipts"));
    }
    for (const auto& [mesh_asset, receipt] : cloth_rest_pose_receipts_) {
        if (mesh_asset == 0U || !meshes_.contains(mesh_asset)) {
            return core::Result<void>::failure(parse_error(
                "cloth rest-pose receipt references a missing mesh asset"));
        }
        if (receipt.bytes.empty() ||
            receipt.bytes.size() > kMaxSerializedClothRestPoseReceiptBytes ||
            receipt.digest.is_zero()) {
            return core::Result<void>::failure(parse_error(
                "cloth rest-pose receipt bytes or digest are outside the serialized bounds"));
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(receipt.bytes.data());
        if (assets::sha256(std::span<const std::uint8_t>(bytes, receipt.bytes.size())) !=
            receipt.digest) {
            return core::Result<void>::failure(parse_error(
                "cloth rest-pose receipt digest does not bind its serialized bytes"));
        }
    }
    if (engineering_authoring_.has_value()) {
        const auto& authoring = *engineering_authoring_;
        if (authoring.bytes.empty() ||
            authoring.bytes.size() > kMaxSerializedEngineeringAuthoringBytes ||
            authoring.digest.is_zero()) {
            return core::Result<void>::failure(parse_error(
                "project engineering authoring bytes or digest are outside the serialized bounds"));
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(authoring.bytes.data());
        if (assets::sha256(std::span<const std::uint8_t>(bytes, authoring.bytes.size())) !=
            authoring.digest) {
            return core::Result<void>::failure(parse_error(
                "project engineering authoring digest does not bind its bytes"));
        }
        const auto decoded = engineering::AuthoringDocument::deserialize(authoring.bytes);
        if (!decoded) {
            return core::Result<void>::failure(decoded.error().with_context(
                "project engineering authoring"));
        }
    }
    if (condition_authoring_.has_value()) {
        const auto& authoring = *condition_authoring_;
        if (authoring.bytes.empty() ||
            authoring.bytes.size() > kMaxSerializedConditionAuthoringBytes ||
            authoring.digest.is_zero()) {
            return core::Result<void>::failure(parse_error(
                "project CONDITION authoring bytes or digest are outside the serialized bounds"));
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(authoring.bytes.data());
        if (assets::sha256(std::span<const std::uint8_t>(bytes, authoring.bytes.size())) !=
            authoring.digest) {
            return core::Result<void>::failure(parse_error(
                "project CONDITION authoring digest does not bind its bytes"));
        }
        const auto decoded = condition::AuthoringDocument::deserialize(authoring.bytes);
        if (!decoded) {
            return core::Result<void>::failure(decoded.error().with_context(
                "project CONDITION authoring"));
        }
    }
    if (biological_authoring_sources_.size() >
            kMaxSerializedBiologicalAuthoringSources ||
        biological_authoring_lineage_.size() >
            kMaxSerializedBiologicalAuthoringSources) {
        return core::Result<void>::failure(parse_error(
            "project biological authoring source or lineage count exceeds its limit"));
    }
    for (const auto& [key, revision] : biological_authoring_lineage_) {
        const bool safe_namespace = !key.first.empty() && key.first.size() <= 256U &&
            std::all_of(key.first.begin(), key.first.end(), [](unsigned char byte) {
                return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
                    byte != '\r' && byte != '\n';
            });
        if (!safe_namespace || key.second == 0U || revision.value() == 0U ||
            revision.exhausted()) {
            return core::Result<void>::failure(parse_error(
                "project biological authoring lineage identity or revision is invalid"));
        }
        const auto active = biological_authoring_sources_.find(key);
        if (active != biological_authoring_sources_.end()) {
            const auto decoded = biological::BiologicalAuthoringDocument::deserialize(
                active->second.bytes);
            if (!decoded || decoded.value().source.revision != revision) {
                return core::Result<void>::failure(parse_error(
                    "active biological authoring source disagrees with retained lineage"));
            }
        }
    }
    std::uint64_t biological_authoring_bytes = 0U;
    for (const auto& [key, authoring] : biological_authoring_sources_) {
        if (authoring.bytes.empty() ||
            authoring.bytes.size() > kMaxSerializedBiologicalAuthoringBytes ||
            authoring.digest.is_zero()) {
            return core::Result<void>::failure(parse_error(
                "project biological authoring bytes or digest exceed their bounds"));
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(authoring.bytes.data());
        if (assets::sha256(std::span<const std::uint8_t>(bytes, authoring.bytes.size())) !=
            authoring.digest) {
            return core::Result<void>::failure(parse_error(
                "project biological authoring digest does not bind its bytes"));
        }
        const auto decoded = biological::BiologicalAuthoringDocument::deserialize(
            authoring.bytes);
        if (!decoded || decoded.value().serialize() != authoring.bytes ||
            decoded.value().source.source_namespace != key.first ||
            decoded.value().source.asset_id != key.second) {
            return core::Result<void>::failure(parse_error(
                "project biological authoring key or canonical source is invalid"));
        }
        const auto lineage = biological_authoring_lineage_.find(key);
        if (lineage == biological_authoring_lineage_.end() ||
            lineage->second != decoded.value().source.revision) {
            return core::Result<void>::failure(parse_error(
                "active biological authoring source lacks matching retained lineage"));
        }
        const auto encoded_size = static_cast<std::uint64_t>(authoring.bytes.size());
        if (encoded_size > kMaxSerializedBiologicalAuthoringAggregateBytes -
                biological_authoring_bytes) {
            return core::Result<void>::failure(parse_error(
                "project biological authoring sources exceed their aggregate byte limit"));
        }
        biological_authoring_bytes += encoded_size;
    }
    if (biological_proxy_instance_receipts_.size() >
        kMaxSerializedBiologicalProxyInstanceReceipts) {
        return core::Result<void>::failure(parse_error(
            "project biological proxy receipt count exceeds its limit"));
    }
    std::uint64_t biological_proxy_receipt_bytes = 0U;
    for (const auto& [object_id, receipt] : biological_proxy_instance_receipts_) {
        if (object_id != receipt.object) {
            return core::Result<void>::failure(parse_error(
                "biological proxy receipt key does not match its object identity"));
        }
        if (auto result = receipt.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "project biological proxy instance receipt"));
        }
        const auto object = scene_.find(object_id);
        const auto lineage = biological_authoring_lineage_.find(
            BiologicalAuthoringKey{receipt.source_namespace, receipt.source_asset_id});
        if (!object || !object->mesh_asset.has_value() ||
            *object->mesh_asset != receipt.mesh_asset || !meshes_.contains(receipt.mesh_asset) ||
            lineage == biological_authoring_lineage_.end() ||
            lineage->second.value() < receipt.source_revision.value()) {
            return core::Result<void>::failure(parse_error(
                "biological proxy receipt is detached from its object, mesh, or retained source lineage"));
        }
        const auto encoded_size = static_cast<std::uint64_t>(receipt.source_snapshot.size());
        if (encoded_size > biological::BiologicalAuthoringDocument::kMaxSerializedBytes ||
            encoded_size > kMaxSerializedBiologicalProxyInstanceReceiptAggregateBytes -
                biological_proxy_receipt_bytes) {
            return core::Result<void>::failure(parse_error(
                "biological proxy source snapshots exceed their bounded storage limit"));
        }
        biological_proxy_receipt_bytes += encoded_size;
    }
    if (condition_vegetation_placement_receipts_.size() >
        kMaxSerializedConditionVegetationPlacementReceipts) {
        return core::Result<void>::failure(parse_error(
            "project CONDITION vegetation placement receipt count exceeds its limit"));
    }
    std::set<std::pair<condition::Id, std::uint64_t>> stable_placement_ids;
    for (const auto& [object_id, receipt] : condition_vegetation_placement_receipts_) {
        if (object_id != receipt.object) {
            return core::Result<void>::failure(parse_error(
                "CONDITION vegetation receipt key does not match its object identity"));
        }
        if (auto result = receipt.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "project CONDITION vegetation placement receipt"));
        }
        const auto object = scene_.find(object_id);
        if (!object || !object->mesh_asset.has_value() ||
            *object->mesh_asset != receipt.mesh_asset || !meshes_.contains(receipt.mesh_asset) ||
            !stable_placement_ids.emplace(receipt.recipe_id,
                receipt.stable_placement_id).second) {
            return core::Result<void>::failure(parse_error(
                "CONDITION vegetation receipt is detached or duplicates a recipe placement"));
        }
        constexpr double kTransformTolerance = 1.0e-12;
        const double half_yaw = receipt.yaw_radians * 0.5;
        const core::Quaternion expected_rotation{
            0.0, std::sin(half_yaw), 0.0, std::cos(half_yaw)};
        const auto& transform = object->local_transform;
        if ((transform.translation - receipt.position).length() > kTransformTolerance ||
            std::abs(transform.rotation.x - expected_rotation.x) > kTransformTolerance ||
            std::abs(transform.rotation.y - expected_rotation.y) > kTransformTolerance ||
            std::abs(transform.rotation.z - expected_rotation.z) > kTransformTolerance ||
            std::abs(transform.rotation.w - expected_rotation.w) > kTransformTolerance ||
            std::abs(transform.scale.x - receipt.uniform_scale) > kTransformTolerance ||
            std::abs(transform.scale.y - receipt.uniform_scale) > kTransformTolerance ||
            std::abs(transform.scale.z - receipt.uniform_scale) > kTransformTolerance) {
            return core::Result<void>::failure(parse_error(
                "CONDITION vegetation placement receipt transform differs from its scene object"));
        }
    }
    if (auto result = world_model_.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "project world model"));
    }
    for (const auto& [identity, area] : world_model_.areas()) {
        static_cast<void>(identity);
        switch (area.geometry.kind) {
        case world::AuthoringBindingKind::none:
            break;
        case world::AuthoringBindingKind::scene_object:
            if (scene_.find(scene::ObjectId{area.geometry.identity}) == nullptr) {
                return core::Result<void>::failure(parse_error(
                    "semantic area references a missing scene object"));
            }
            break;
        case world::AuthoringBindingKind::mesh_asset:
            if (!meshes_.contains(area.geometry.identity)) {
                return core::Result<void>::failure(parse_error(
                    "semantic area references a missing mesh asset"));
            }
            break;
        }
    }
    for (const auto& [identity, layer] : world_model_.variant_layers()) {
        static_cast<void>(identity);
        if (layer.base_project_revision > revision_) {
            return core::Result<void>::failure(parse_error(
                "variant layer base revision is newer than the project"));
        }
    }
    if (auto result = material_catalog_.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "project material catalog"));
    }
    if (auto result = presentation_catalog_.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "project presentation catalog"));
    }
    if (auto result = authoring_standards_.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "project authoring standards"));
    }
    for (const auto& [id, standard] : authoring_standards_.records()) {
        static_cast<void>(id);
        if (standard.evidence.has_value() &&
            standard.evidence->source_revision >= revision_) {
            return core::Result<void>::failure(parse_error(
                "project standard evidence is not bound to an earlier project revision"));
        }
    }
    if (curve_paths_.size() > kMaxSerializedCurvePaths) {
        return core::Result<void>::failure(parse_error(
            "project curve source count exceeds its limit"));
    }
    if (curve_path_lineage_.size() > kMaxSerializedCurvePathLineage) {
        return core::Result<void>::failure(parse_error(
            "project curve source lineage count exceeds its limit"));
    }
    for (const auto& [identity, revision] : curve_path_lineage_) {
        if (!valid_curve_or_road_identity(identity) || revision.value() == 0U) {
            return core::Result<void>::failure(parse_error(
                "project curve source lineage record is invalid"));
        }
        const auto active = curve_paths_.find(identity);
        if (active != curve_paths_.end() && active->second.source_revision != revision) {
            return core::Result<void>::failure(parse_error(
                "active curve source disagrees with retained source lineage"));
        }
    }
    std::uint64_t curve_source_bytes = 0U;
    for (const auto& [identity, path] : curve_paths_) {
        if (identity != path.source_identity) {
            return core::Result<void>::failure(parse_error(
                "project curve source key does not match its embedded identity"));
        }
        if (auto result = path.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "project curve source"));
        }
        const auto lineage = curve_path_lineage_.find(identity);
        if (lineage == curve_path_lineage_.end() ||
            lineage->second != path.source_revision) {
            return core::Result<void>::failure(parse_error(
                "active curve source lacks its matching source lineage"));
        }
        const std::uint64_t encoded_size = path.serialize().size();
        if (encoded_size > kMaxSerializedCurvePathBytes ||
            encoded_size > kMaxSerializedCurvePathAggregateBytes - curve_source_bytes) {
            return core::Result<void>::failure(parse_error(
                "project curve source bytes exceed their bounded storage limit"));
        }
        curve_source_bytes += encoded_size;
    }
    if (road_sources_.size() > kMaxSerializedRoadSources) {
        return core::Result<void>::failure(parse_error(
            "project road source count exceeds its limit"));
    }
    if (road_source_lineage_.size() > kMaxSerializedRoadSourceLineage) {
        return core::Result<void>::failure(parse_error(
            "project road-source lineage count exceeds its limit"));
    }
    for (const auto& [identity, revision] : road_source_lineage_) {
        if (!valid_curve_or_road_identity(identity) || revision.value() == 0U) {
            return core::Result<void>::failure(parse_error(
                "project road-source lineage record is invalid"));
        }
        const auto active = road_sources_.find(identity);
        if (active != road_sources_.end() && active->second.source_revision != revision) {
            return core::Result<void>::failure(parse_error(
                "active road source disagrees with retained source lineage"));
        }
    }
    std::uint64_t road_source_bytes = 0U;
    for (const auto& [identity, road] : road_sources_) {
        if (identity != road.source_identity) {
            return core::Result<void>::failure(parse_error(
                "project road source key does not match its embedded identity"));
        }
        if (auto result = road.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "project road source"));
        }
        const auto lineage = road_source_lineage_.find(identity);
        if (lineage == road_source_lineage_.end() ||
            lineage->second != road.source_revision) {
            return core::Result<void>::failure(parse_error(
                "active road source lacks its matching source lineage"));
        }
        const auto centerline = curve_paths_.find(road.centerline_identity);
        if (centerline == curve_paths_.end() ||
            centerline->second.source_revision != road.centerline_revision) {
            return core::Result<void>::failure(parse_error(
                "project road source has a missing or stale centerline binding"));
        }
        const std::uint64_t encoded_size = road.serialize().size();
        if (encoded_size > kMaxSerializedRoadSourceBytes ||
            encoded_size > kMaxSerializedRoadSourceAggregateBytes - road_source_bytes) {
            return core::Result<void>::failure(parse_error(
                "project road source bytes exceed their bounded storage limit"));
        }
        road_source_bytes += encoded_size;
    }
    if (sweep_sources_.size() > kMaxSerializedSweepSources) {
        return core::Result<void>::failure(parse_error(
            "project sweep source count exceeds its limit"));
    }
    if (sweep_source_lineage_.size() > kMaxSerializedSweepSourceLineage) {
        return core::Result<void>::failure(parse_error(
            "project sweep-source lineage count exceeds its limit"));
    }
    for (const auto& [identity, revision] : sweep_source_lineage_) {
        if (!valid_curve_or_road_identity(identity) || revision.value() == 0U) {
            return core::Result<void>::failure(parse_error(
                "project sweep-source lineage record is invalid"));
        }
        const auto active = sweep_sources_.find(identity);
        if (active != sweep_sources_.end() && active->second.source_revision != revision) {
            return core::Result<void>::failure(parse_error(
                "active sweep source disagrees with retained source lineage"));
        }
    }
    std::uint64_t sweep_source_bytes = 0U;
    for (const auto& [identity, source] : sweep_sources_) {
        if (identity != source.source_identity) {
            return core::Result<void>::failure(parse_error(
                "project sweep source key does not match its embedded identity"));
        }
        if (auto result = source.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "project sweep source"));
        }
        const auto lineage = sweep_source_lineage_.find(identity);
        if (lineage == sweep_source_lineage_.end() ||
            lineage->second != source.source_revision) {
            return core::Result<void>::failure(parse_error(
                "active sweep source lacks its matching source lineage"));
        }
        const auto centerline = curve_paths_.find(source.centerline_identity);
        if (centerline == curve_paths_.end() ||
            centerline->second.source_revision != source.centerline_revision) {
            return core::Result<void>::failure(parse_error(
                "project sweep source has a missing or stale centerline binding"));
        }
        const std::uint64_t encoded_size = source.serialize().size();
        if (encoded_size > kMaxSerializedSweepSourceBytes ||
            encoded_size > kMaxSerializedSweepSourceAggregateBytes - sweep_source_bytes) {
            return core::Result<void>::failure(parse_error(
                "project sweep source bytes exceed their bounded storage limit"));
        }
        sweep_source_bytes += encoded_size;
    }
    if (sign_text_sources_.size() > kMaxSerializedSignTextSources ||
        sign_text_source_lineage_.size() > kMaxSerializedSignTextSourceLineage) {
        return core::Result<void>::failure(parse_error(
            "project sign-text source or lineage count exceeds its limit"));
    }
    for (const auto& [identity, revision] : sign_text_source_lineage_) {
        geometry::SignTextSource identity_probe;
        identity_probe.source_identity = identity;
        identity_probe.source_revision = revision;
        identity_probe.text = "A";
        if (!identity_probe.validate()) {
            return core::Result<void>::failure(parse_error(
                "project sign-text source-lineage record is invalid"));
        }
        const auto active = sign_text_sources_.find(identity);
        if (active != sign_text_sources_.end() && active->second.source_revision != revision) {
            return core::Result<void>::failure(parse_error(
                "active sign-text source disagrees with retained source lineage"));
        }
    }
    std::uint64_t sign_text_source_bytes = 0U;
    for (const auto& [identity, source] : sign_text_sources_) {
        if (identity != source.source_identity) {
            return core::Result<void>::failure(parse_error(
                "project sign-text source key does not match its embedded identity"));
        }
        if (auto result = source.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "project sign-text source"));
        }
        const auto lineage = sign_text_source_lineage_.find(identity);
        if (lineage == sign_text_source_lineage_.end() ||
            lineage->second != source.source_revision) {
            return core::Result<void>::failure(parse_error(
                "active sign-text source lacks its matching source lineage"));
        }
        const std::uint64_t encoded_size = source.serialize().size();
        if (encoded_size > kMaxSerializedSignTextSourceBytes ||
            encoded_size > kMaxSerializedSignTextSourceAggregateBytes - sign_text_source_bytes) {
            return core::Result<void>::failure(parse_error(
                "project sign-text source bytes exceed their bounded storage limit"));
        }
        sign_text_source_bytes += encoded_size;
    }
    if (sign_text_instance_receipts_.size() > kMaxSerializedSignTextInstanceReceipts) {
        return core::Result<void>::failure(parse_error(
            "project sign-text instance receipt count exceeds its limit"));
    }
    std::uint64_t sign_text_receipt_bytes = 0U;
    for (const auto& [object_id, receipt] : sign_text_instance_receipts_) {
        if (object_id != receipt.object) {
            return core::Result<void>::failure(parse_error(
                "sign-text receipt key does not match its object identity"));
        }
        if (auto result = receipt.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "project sign-text instance receipt"));
        }
        const auto object = scene_.find(object_id);
        const auto lineage = sign_text_source_lineage_.find(receipt.source_identity);
        if (!object || !object->mesh_asset.has_value() ||
            *object->mesh_asset != receipt.mesh_asset || !meshes_.contains(receipt.mesh_asset) ||
            lineage == sign_text_source_lineage_.end() ||
            lineage->second.value() < receipt.source_revision.value()) {
            return core::Result<void>::failure(parse_error(
                "sign-text receipt is detached from its object, mesh, or retained source lineage"));
        }
        const std::uint64_t encoded_size = receipt.source_snapshot.size();
        if (encoded_size > geometry::SignTextSource::kMaxTextBytes * 32U ||
            encoded_size > kMaxSerializedSignTextInstanceReceiptAggregateBytes -
                sign_text_receipt_bytes) {
            return core::Result<void>::failure(parse_error(
                "sign-text source snapshots exceed their bounded storage limit"));
        }
        sign_text_receipt_bytes += encoded_size;
    }
    if (road_surface_instance_receipts_.size() >
        kMaxSerializedRoadSurfaceInstanceReceipts) {
        return core::Result<void>::failure(parse_error(
            "project road-surface instance receipt count exceeds its limit"));
    }
    std::uint64_t road_surface_receipt_bytes = 0U;
    for (const auto& [object_id, receipt] : road_surface_instance_receipts_) {
        if (object_id != receipt.object) {
            return core::Result<void>::failure(parse_error(
                "road-surface receipt key does not match its object identity"));
        }
        if (auto result = receipt.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "project road-surface instance receipt"));
        }
        const auto object = scene_.find(object_id);
        const auto road_lineage = road_source_lineage_.find(receipt.road_identity);
        const auto centerline_lineage = curve_path_lineage_.find(receipt.centerline_identity);
        if (!object || !object->mesh_asset.has_value() ||
            *object->mesh_asset != receipt.mesh_asset || !meshes_.contains(receipt.mesh_asset) ||
            road_lineage == road_source_lineage_.end() ||
            road_lineage->second.value() < receipt.road_revision.value() ||
            centerline_lineage == curve_path_lineage_.end() ||
            centerline_lineage->second.value() < receipt.centerline_revision.value()) {
            return core::Result<void>::failure(parse_error(
                "road-surface receipt is detached from its object, mesh, or retained source lineages"));
        }
        const std::uint64_t road_bytes = receipt.road_snapshot.size();
        const std::uint64_t centerline_bytes = receipt.centerline_snapshot.size();
        if (road_bytes > RoadSource::kMaxSerializedBytes ||
            centerline_bytes > geometry::CurvePath::kMaxSerializedBytes ||
            road_bytes + centerline_bytes >
                kMaxSerializedRoadSurfaceInstanceReceiptAggregateBytes -
                    road_surface_receipt_bytes) {
            return core::Result<void>::failure(parse_error(
                "road-surface source snapshots exceed their bounded storage limit"));
        }
        road_surface_receipt_bytes += road_bytes + centerline_bytes;
    }
    if (spatial_fields_.size() > kMaxSerializedSpatialFields) {
        return core::Result<void>::failure(parse_error(
            "project spatial-field count exceeds its limit"));
    }
    if (spatial_field_lineage_.size() > kMaxSerializedSpatialFieldLineage) {
        return core::Result<void>::failure(parse_error(
            "project spatial-field lineage count exceeds its limit"));
    }
    for (const auto& [identity, revision] : spatial_field_lineage_) {
        if (!valid_spatial_field_identity(identity) || revision.value() == 0U) {
            return core::Result<void>::failure(parse_error(
                "project spatial-field lineage record is invalid"));
        }
        const auto active = spatial_fields_.find(identity);
        if (active != spatial_fields_.end() && active->second.source_revision() != revision) {
            return core::Result<void>::failure(parse_error(
                "active spatial field disagrees with retained source lineage"));
        }
    }
    std::uint64_t spatial_field_bytes = 0U;
    for (const auto& [identity, field] : spatial_fields_) {
        if (identity != field.identity()) {
            return core::Result<void>::failure(parse_error(
                "project spatial-field key does not match its embedded identity"));
        }
        if (auto result = field.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "project spatial field"));
        }
        const auto lineage = spatial_field_lineage_.find(identity);
        if (lineage == spatial_field_lineage_.end() ||
            lineage->second != field.source_revision()) {
            return core::Result<void>::failure(parse_error(
                "active spatial field lacks its matching source lineage"));
        }
        const std::uint64_t encoded_size = field.serialize().size();
        if (encoded_size > kMaxSerializedSpatialFieldBytes ||
            encoded_size > kMaxSerializedSpatialFieldAggregateBytes - spatial_field_bytes) {
            return core::Result<void>::failure(parse_error(
                "project spatial fields exceed their bounded storage limit"));
        }
        spatial_field_bytes += encoded_size;
    }
    for (const auto& [object, camera] : presentation_catalog_.cameras()) {
        static_cast<void>(camera);
        if (scene_.find(object) == nullptr) {
            return core::Result<void>::failure(parse_error(
                "authored camera references a missing scene object"));
        }
    }
    for (const auto& [object, light] : presentation_catalog_.lights()) {
        static_cast<void>(light);
        if (scene_.find(object) == nullptr) {
            return core::Result<void>::failure(parse_error(
                "authored light references a missing scene object"));
        }
    }
    for (const auto& [identity, variant] : material_catalog_.variants()) {
        static_cast<void>(identity);
        for (const std::uint64_t mesh_asset : variant.permitted_mesh_assets) {
            if (!meshes_.contains(mesh_asset)) {
                return core::Result<void>::failure(parse_error(
                    "material variant scope references a missing mesh asset"));
            }
        }
    }
    for (const auto& [identity, assignment] : material_catalog_.assignments()) {
        static_cast<void>(identity);
        const auto mesh = meshes_.find(assignment.key.mesh_asset);
        if (mesh == meshes_.end()) {
            return core::Result<void>::failure(parse_error(
                "material-region assignment references a missing mesh asset"));
        }
        if (!mesh_declares_material_region(mesh->second, assignment.key.region_id)) {
            return core::Result<void>::failure(parse_error(
                "material-region assignment references a missing authored region"));
        }
    }
    for (const auto& object : scene_.objects_sorted()) {
        if (object.mesh_asset.has_value() && !meshes_.contains(*object.mesh_asset)) {
            return core::Result<void>::failure(
                parse_error("scene object references a missing mesh asset"));
        }
    }
    return core::Result<void>::success();
}

void ProjectDocument::swap(ProjectDocument& other) noexcept {
    name_.swap(other.name_);
    source_namespace_.swap(other.source_namespace_);
    scene_.swap(other.scene_);
    meshes_.swap(other.meshes_);
    topology_receipts_.swap(other.topology_receipts_);
    mesh_patches_.swap(other.mesh_patches_);
    std::swap(scientific_model_, other.scientific_model_);
    std::swap(world_model_, other.world_model_);
    std::swap(material_catalog_, other.material_catalog_);
    std::swap(presentation_catalog_, other.presentation_catalog_);
    std::swap(authoring_standards_, other.authoring_standards_);
    curve_paths_.swap(other.curve_paths_);
    curve_path_lineage_.swap(other.curve_path_lineage_);
    road_sources_.swap(other.road_sources_);
    road_source_lineage_.swap(other.road_source_lineage_);
    sweep_sources_.swap(other.sweep_sources_);
    sweep_source_lineage_.swap(other.sweep_source_lineage_);
    sign_text_sources_.swap(other.sign_text_sources_);
    sign_text_source_lineage_.swap(other.sign_text_source_lineage_);
    sign_text_instance_receipts_.swap(other.sign_text_instance_receipts_);
    road_surface_instance_receipts_.swap(other.road_surface_instance_receipts_);
    biological_proxy_instance_receipts_.swap(other.biological_proxy_instance_receipts_);
    condition_vegetation_placement_receipts_.swap(
        other.condition_vegetation_placement_receipts_);
    spatial_fields_.swap(other.spatial_fields_);
    spatial_field_lineage_.swap(other.spatial_field_lineage_);
    std::swap(semantic_sidecar_bytes_, other.semantic_sidecar_bytes_);
    std::swap(semantic_sidecar_digest_, other.semantic_sidecar_digest_);
    sculpt_stacks_.swap(other.sculpt_stacks_);
    garment_assets_.swap(other.garment_assets_);
    cloth_rest_pose_receipts_.swap(other.cloth_rest_pose_receipts_);
    std::swap(engineering_authoring_, other.engineering_authoring_);
    std::swap(condition_authoring_, other.condition_authoring_);
    biological_authoring_sources_.swap(other.biological_authoring_sources_);
    biological_authoring_lineage_.swap(other.biological_authoring_lineage_);
    rig_module_templates_.swap(other.rig_module_templates_);
    decal_placements_.swap(other.decal_placements_);
    pose_libraries_.swap(other.pose_libraries_);
    weight_edit_receipts_.swap(other.weight_edit_receipts_);
    curve_edit_receipts_.swap(other.curve_edit_receipts_);
    std::swap(source_asset_registry_, other.source_asset_registry_);
    std::swap(evaluation_graph_digest_, other.evaluation_graph_digest_);
    std::swap(next_mesh_id_, other.next_mesh_id_);
    std::swap(revision_, other.revision_);
}

std::string ProjectDocument::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << "AUTHORING " << std::quoted(std::string(kAuthoringFormat)) << ' '
           << std::quoted(std::string(kAuthoringUnits)) << ' '
           << std::quoted(std::string(kAuthoringCoordinateSystem)) << '\n';
    output << "SOURCE_NAMESPACE " << std::quoted(source_namespace_) << '\n';
    output << "NAME " << std::quoted(name_) << '\n';
    output << "REVISION " << revision_.value() << '\n';

    const auto objects = scene_.objects_sorted();
    output << "OBJECTS " << objects.size() << '\n';
    output << std::setprecision(17);
    for (const auto& object : objects) {
        output << "OBJECT " << object.id.value << ' '
               << (object.parent.has_value() ? object.parent->value : 0U) << ' '
               << std::quoted(object.name) << ' '
               << object.local_transform.translation.x << ' '
               << object.local_transform.translation.y << ' '
               << object.local_transform.translation.z << ' '
               << object.local_transform.rotation.x << ' '
               << object.local_transform.rotation.y << ' '
               << object.local_transform.rotation.z << ' '
               << object.local_transform.rotation.w << ' '
               << object.local_transform.scale.x << ' '
               << object.local_transform.scale.y << ' '
               << object.local_transform.scale.z << ' '
               << (object.visible ? 1 : 0) << ' '
               << (object.locked ? 1 : 0) << ' '
               << (object.mesh_asset.has_value() ? object.mesh_asset.value() : 0U) << '\n';
    }

    output << "MESHES " << meshes_.size() << '\n';
    for (const auto& [mesh_id, mesh] : meshes_) {
        output << "MESH " << mesh_id << ' ' << mesh.revision().value() << '\n';
        const auto vertices = mesh.vertices_sorted();
        output << "VERTICES " << vertices.size() << '\n';
        for (const auto& vertex : vertices) {
            output << "VERTEX " << vertex.id.value << ' ' << vertex.position.x << ' '
                   << vertex.position.y << ' ' << vertex.position.z << '\n';
        }
        const auto faces = mesh.faces_sorted();
        output << "FACES " << faces.size() << '\n';
        for (const auto& face : faces) {
            output << "FACE " << face.id.value << ' ' << face.vertices.size();
            for (const auto vertex : face.vertices) {
                output << ' ' << vertex.value;
            }
            output << '\n';
        }
        output << "ATTRIBUTES\n";
        output << "DOMAIN_COUNTS " << mesh.attributes().domain_counts().size() << '\n';
        for (const auto& [domain, count] : mesh.attributes().domain_counts()) {
            output << "DOMAIN_COUNT " << static_cast<std::uint64_t>(domain) << ' '
                   << count << '\n';
        }
        output << "LAYERS " << mesh.attributes().layer_count() << '\n';
        for (const auto& [layer_id, layer] : mesh.attributes().layers()) {
            output << "LAYER " << std::quoted(layer_id) << ' '
                   << static_cast<std::uint64_t>(layer.descriptor.domain) << ' '
                   << static_cast<std::uint64_t>(layer.descriptor.type) << ' '
                   << layer.values.size() << '\n';
            const auto value_order = serialized_attribute_value_order(
                mesh, layer.descriptor.domain);
            if (value_order.size() == layer.values.size()) {
                for (const std::size_t index : value_order) {
                    output << "ATTRIBUTE_VALUE ";
                    write_attribute_value(output, layer.descriptor.type, layer.values.at(index));
                    output << '\n';
                }
            } else {
                for (const auto& value : layer.values) {
                    output << "ATTRIBUTE_VALUE ";
                    write_attribute_value(output, layer.descriptor.type, value);
                    output << '\n';
                }
            }
        }
        output << "UV_SETS " << mesh.uv_sets().descriptors().size() << '\n';
        for (const auto& [uv_id, descriptor] : mesh.uv_sets().descriptors()) {
            output << "UV_SET " << uv_id << ' ' << std::quoted(descriptor.name) << ' '
                   << std::quoted(descriptor.layer_id) << ' '
                   << (descriptor.active_for_editing ? 1 : 0) << ' '
                   << (descriptor.active_for_render ? 1 : 0) << ' '
                   << (descriptor.lightmap_candidate ? 1 : 0) << ' '
                   << descriptor.tile_policy_version << '\n';
        }
    }
    output << "TOPOLOGY_RECEIPTS " << topology_receipts_.size() << '\n';
    for (const auto& record : topology_receipts_) {
        output << "TOPOLOGY_RECEIPT " << record.mesh_asset << ' '
               << std::quoted(record.receipt.serialize()) << '\n';
    }
    output << "MESH_PATCHES " << mesh_patches_.size() << '\n';
    for (const auto& record : mesh_patches_) {
        output << "MESH_PATCH " << record.mesh_asset << ' '
               << record.patch_digest.hex() << ' '
               << record.source_mesh_digest.hex() << ' '
               << record.result_mesh_digest.hex() << ' '
               << std::quoted(record.patch_bytes) << '\n';
    }
    if (evaluation_graph_digest_.has_value()) {
        output << "EVALUATION_GRAPH_DIGEST " << evaluation_graph_digest_->hex() << '\n';
    }
    output << "SCIENTIFIC_MODEL " << std::quoted(scientific_model_.serialize()) << '\n';
    output << "WORLD_MODEL " << std::quoted(world_model_.serialize()) << '\n';
    output << "MATERIAL_CATALOG " << std::quoted(material_catalog_.serialize()) << '\n';
    if (semantic_sidecar_bytes_.has_value()) {
        output << "SEMANTIC_SIDECAR " << semantic_sidecar_digest_->hex() << ' '
               << std::quoted(*semantic_sidecar_bytes_) << '\n';
    }
    output << "SCULPT_STACKS " << sculpt_stacks_.size() << '\n';
    for (const auto& [mesh_asset, stack] : sculpt_stacks_) {
        output << "SCULPT_STACK " << mesh_asset << ' ' << stack.digest.hex() << ' '
               << std::quoted(stack.bytes) << '\n';
    }
    output << "GARMENT_ASSETS " << garment_assets_.size() << '\n';
    for (const auto& [mesh_asset, asset] : garment_assets_) {
        output << "GARMENT_ASSET " << mesh_asset << ' ' << asset.digest.hex() << ' '
               << std::quoted(asset.bytes) << '\n';
    }
    output << "CLOTH_REST_POSE_RECEIPTS " << cloth_rest_pose_receipts_.size() << '\n';
    for (const auto& [mesh_asset, receipt] : cloth_rest_pose_receipts_) {
        output << "CLOTH_REST_POSE_RECEIPT " << mesh_asset << ' '
               << receipt.digest.hex() << ' ' << std::quoted(receipt.bytes) << '\n';
    }
    if (engineering_authoring_.has_value()) {
        output << "ENGINEERING_AUTHORING " << engineering_authoring_->digest.hex() << ' '
               << std::quoted(engineering_authoring_->bytes) << '\n';
    }
    if (condition_authoring_.has_value()) {
        output << "CONDITION_AUTHORING " << condition_authoring_->digest.hex() << ' '
               << std::quoted(condition_authoring_->bytes) << '\n';
    } else {
        output << "CONDITION_AUTHORING NONE\n";
    }
    output << "PRESENTATION_CATALOG "
           << std::quoted(presentation_catalog_.serialize()) << '\n';
    output << "CURVE_PATHS " << curve_paths_.size() << '\n';
    for (const auto& [identity, path] : curve_paths_) {
        output << "CURVE_PATH " << std::quoted(identity) << ' '
               << std::quoted(path.serialize()) << '\n';
    }
    output << "ROAD_SOURCES " << road_sources_.size() << '\n';
    for (const auto& [identity, road] : road_sources_) {
        output << "ROAD_SOURCE " << std::quoted(identity) << ' '
               << std::quoted(road.serialize()) << '\n';
    }
    output << "SWEEP_SOURCES " << sweep_sources_.size() << '\n';
    for (const auto& [identity, source] : sweep_sources_) {
        output << "SWEEP_SOURCE " << std::quoted(identity) << ' '
               << std::quoted(source.serialize()) << '\n';
    }
    output << "SIGN_TEXT_SOURCES " << sign_text_sources_.size() << '\n';
    for (const auto& [identity, source] : sign_text_sources_) {
        output << "SIGN_TEXT_SOURCE " << std::quoted(identity) << ' '
               << std::quoted(source.serialize()) << '\n';
    }
    output << "SPATIAL_FIELDS " << spatial_fields_.size() << '\n';
    for (const auto& [identity, field] : spatial_fields_) {
        output << "SPATIAL_FIELD " << std::quoted(identity) << ' '
               << std::quoted(field.serialize()) << '\n';
    }
    output << "SPATIAL_FIELD_LINEAGE " << spatial_field_lineage_.size() << '\n';
    for (const auto& [identity, revision] : spatial_field_lineage_) {
        output << "SPATIAL_FIELD_LINEAGE_RECORD " << std::quoted(identity) << ' '
               << revision.value() << '\n';
    }
    output << "CURVE_PATH_LINEAGE " << curve_path_lineage_.size() << '\n';
    for (const auto& [identity, revision] : curve_path_lineage_) {
        output << "CURVE_PATH_LINEAGE_RECORD " << std::quoted(identity) << ' '
               << revision.value() << '\n';
    }
    output << "ROAD_SOURCE_LINEAGE " << road_source_lineage_.size() << '\n';
    for (const auto& [identity, revision] : road_source_lineage_) {
        output << "ROAD_SOURCE_LINEAGE_RECORD " << std::quoted(identity) << ' '
               << revision.value() << '\n';
    }
    output << "SWEEP_SOURCE_LINEAGE " << sweep_source_lineage_.size() << '\n';
    for (const auto& [identity, revision] : sweep_source_lineage_) {
        output << "SWEEP_SOURCE_LINEAGE_RECORD " << std::quoted(identity) << ' '
               << revision.value() << '\n';
    }
    output << "SIGN_TEXT_SOURCE_LINEAGE " << sign_text_source_lineage_.size() << '\n';
    for (const auto& [identity, revision] : sign_text_source_lineage_) {
        output << "SIGN_TEXT_SOURCE_LINEAGE_RECORD " << std::quoted(identity) << ' '
               << revision.value() << '\n';
    }
    output << "SIGN_TEXT_INSTANCE_RECEIPTS "
           << sign_text_instance_receipts_.size() << '\n';
    for (const auto& [object, receipt] : sign_text_instance_receipts_) {
        output << "SIGN_TEXT_INSTANCE_RECEIPT " << object.value << ' '
               << receipt.mesh_asset << ' ' << std::quoted(receipt.source_identity) << ' '
               << receipt.source_revision.value() << ' ' << receipt.source_digest.hex() << ' '
               << encode_hex_bytes(receipt.source_snapshot) << '\n';
    }
    output << "ROAD_SURFACE_INSTANCE_RECEIPTS "
           << road_surface_instance_receipts_.size() << '\n';
    for (const auto& [object, receipt] : road_surface_instance_receipts_) {
        output << "ROAD_SURFACE_INSTANCE_RECEIPT " << object.value << ' '
               << receipt.mesh_asset << ' ' << std::quoted(receipt.road_identity) << ' '
               << receipt.road_revision.value() << ' '
               << static_cast<unsigned>(receipt.surface_class) << ' '
               << std::quoted(receipt.centerline_identity) << ' '
               << receipt.centerline_revision.value() << ' '
               << receipt.samples_per_segment << ' '
               << receipt.road_digest.hex() << ' '
               << receipt.centerline_digest.hex() << ' '
               << receipt.generated_mesh_digest.hex() << ' '
               << encode_hex_bytes(receipt.road_snapshot) << ' '
               << encode_hex_bytes(receipt.centerline_snapshot) << '\n';
    }
    output << "SOURCE_ASSET_REGISTRY_HEX "
           << encode_hex_bytes(source_asset_registry_.serialize()) << '\n';
    output << "BIOLOGICAL_AUTHORING_SOURCES "
           << biological_authoring_sources_.size() << '\n';
    for (const auto& [key, source] : biological_authoring_sources_) {
        output << "BIOLOGICAL_AUTHORING_SOURCE " << std::quoted(key.first) << ' '
               << key.second << ' ' << source.digest.hex() << ' '
               << std::quoted(source.bytes) << '\n';
    }
    output << "BIOLOGICAL_AUTHORING_LINEAGE "
           << biological_authoring_lineage_.size() << '\n';
    for (const auto& [key, revision] : biological_authoring_lineage_) {
        output << "BIOLOGICAL_AUTHORING_LINEAGE_RECORD " << std::quoted(key.first) << ' '
               << key.second << ' ' << revision.value() << '\n';
    }
    output << "BIOLOGICAL_PROXY_INSTANCE_RECEIPTS "
           << biological_proxy_instance_receipts_.size() << '\n';
    for (const auto& [object, receipt] : biological_proxy_instance_receipts_) {
        output << "BIOLOGICAL_PROXY_INSTANCE_RECEIPT " << object.value << ' '
               << receipt.mesh_asset << ' ' << std::quoted(receipt.source_namespace) << ' '
               << receipt.source_asset_id << ' ' << receipt.source_revision.value() << ' '
               << receipt.structure_id << ' ' << receipt.source_snapshot_digest.hex() << ' '
               << receipt.product_digest.hex() << ' '
               << encode_hex_bytes(receipt.source_snapshot) << '\n';
    }
    output << "CONDITION_VEGETATION_PLACEMENT_RECEIPTS "
           << condition_vegetation_placement_receipts_.size() << '\n';
    for (const auto& [object, receipt] : condition_vegetation_placement_receipts_) {
        output << "CONDITION_VEGETATION_PLACEMENT_RECEIPT " << object.value << ' '
               << receipt.mesh_asset << ' ' << receipt.stable_placement_id << ' '
               << receipt.recipe_id << ' ' << receipt.species_asset_id << ' '
               << std::quoted(receipt.plant_source_identity) << ' '
               << receipt.plant_source_generation.value() << ' '
               << receipt.plant_source_digest.hex() << ' '
               << receipt.source_project_revision.value() << ' '
               << receipt.authoring_revision.value() << ' '
               << receipt.authoring_digest.hex() << ' ' << receipt.preview_digest.hex() << ' '
               << std::setprecision(17) << receipt.position.x << ' '
               << receipt.position.y << ' ' << receipt.position.z << ' '
               << receipt.yaw_radians << ' ' << receipt.uniform_scale << ' '
               << receipt.suitability << ' ' << receipt.fields.size() << '\n';
        for (const auto& field : receipt.fields) {
            output << "CONDITION_VEGETATION_FIELD_RECEIPT "
                   << std::quoted(field.identity) << ' '
                   << spatial_field::field_semantic_name(field.semantic) << ' '
                   << field.source_revision.value() << ' ' << field.digest.hex() << '\n';
        }
    }
    output << "RIG_MODULE_TEMPLATES " << rig_module_templates_.size() << '\n';
    for (const auto& [identity, entry] : rig_module_templates_) {
        output << "RIG_MODULE_TEMPLATE " << std::quoted(identity) << ' '
               << entry.revision.value() << ' ' << entry.digest.hex() << ' '
               << std::quoted(entry.bytes) << '\n';
    }
    output << "DECAL_PLACEMENTS " << decal_placements_.size() << '\n';
    for (const auto& [placement_id, entry] : decal_placements_) {
        output << "DECAL_PLACEMENT " << placement_id << ' ' << entry.revision.value() << ' '
               << entry.digest.hex() << ' ' << std::quoted(entry.bytes) << '\n';
    }
    output << "AUTHORING_STANDARDS_HEX "
           << encode_hex_bytes(authoring_standards_.serialize()) << '\n';
    output << "POSE_LIBRARIES " << pose_libraries_.size() << '\n';
    for (const auto& [identity, entry] : pose_libraries_) {
        output << "POSE_LIBRARY " << std::quoted(identity) << ' '
               << entry.revision.value() << ' ' << entry.digest.hex() << ' '
               << std::quoted(entry.bytes) << '\n';
    }
    output << "WEIGHT_EDIT_RECEIPTS " << weight_edit_receipts_.size() << '\n';
    for (const auto& record : weight_edit_receipts_) {
        output << "WEIGHT_EDIT_RECEIPT " << record.digest.hex() << ' '
               << std::quoted(record.bytes) << '\n';
    }
    output << "CURVE_EDIT_RECEIPTS " << curve_edit_receipts_.size() << '\n';
    for (const auto& record : curve_edit_receipts_) {
        output << "CURVE_EDIT_RECEIPT " << record.digest.hex() << ' '
               << std::quoted(record.bytes) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<ProjectDocument> ProjectDocument::deserialize(std::string_view text) {
    if (text.size() > kMaxSerializedProjectBytes) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project text exceeds the Cartographer v1 size limit of 128 MiB"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    if (!(input >> magic) || magic != kMagic) {
        return core::Result<ProjectDocument>::failure(
            parse_error("project magic is missing or invalid"));
    }
    auto version = read_uint(input, "schema version");
    if (!version) {
        return core::Result<ProjectDocument>::failure(version.error());
    }
    if (version.value() < kMinimumReadableSchemaVersion || version.value() > kSchemaVersion) {
        return core::Result<ProjectDocument>::failure(
            Diagnostic(ErrorCode::version_mismatch, "unsupported Cartographer project schema version"));
    }

    if (version.value() >= 2U) {
        if (auto result = require_line(input, "AUTHORING"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        std::string authoring_format;
        std::string authoring_units;
        std::string authoring_coordinate_system;
        if (!(input >> std::quoted(authoring_format) >> std::quoted(authoring_units) >>
              std::quoted(authoring_coordinate_system)) ||
            authoring_format != kAuthoringFormat || authoring_units != kAuthoringUnits ||
            authoring_coordinate_system != kAuthoringCoordinateSystem) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project AUTHORING record does not describe the supported Cartographer coordinate contract"));
        }
    }

    ProjectDocument document;
    if (version.value() >= 9U) {
        if (auto result = require_line(input, "SOURCE_NAMESPACE"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        if (!(input >> std::quoted(document.source_namespace_)) ||
            !valid_source_namespace(document.source_namespace_)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project source namespace is invalid"));
        }
    } else {
        document.source_namespace_ = namespace_from_seed(
            text, "cartographer.project.migrated.");
    }

    if (auto result = require_line(input, "NAME"); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    if (!(input >> std::quoted(document.name_)) || document.name_.empty()) {
        return core::Result<ProjectDocument>::failure(parse_error("project name is invalid"));
    }
    if (auto result = require_line(input, "REVISION"); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    auto revision = read_uint(input, "revision");
    if (!revision) {
        return core::Result<ProjectDocument>::failure(revision.error());
    }
    document.revision_ = core::Revision(revision.value());
    if (document.revision_.exhausted()) {
        return core::Result<ProjectDocument>::failure(
            parse_error("project revision space is exhausted"));
    }

    if (auto result = require_line(input, "OBJECTS"); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    auto object_count = read_uint(input, "object count");
    if (!object_count) {
        return core::Result<ProjectDocument>::failure(object_count.error());
    }
    if (auto result = validate_serialized_count(
            object_count.value(), kMaxSerializedObjects, "project object count");
        !result) {
            return core::Result<ProjectDocument>::failure(result.error());
    }
    std::vector<std::pair<scene::ObjectId, std::optional<scene::ObjectId>>> parent_links;
    parent_links.reserve(static_cast<std::size_t>(object_count.value()));
    for (std::uint64_t index = 0; index < object_count.value(); ++index) {
        if (auto result = require_line(input, "OBJECT"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        scene::SceneObject object;
        auto serialized_object_id = read_uint(input, "object id");
        auto parent_id = read_uint(input, "object parent id");
        int visible = 0;
        int locked = 0;
        if (!serialized_object_id || !parent_id || !(input >> std::quoted(object.name) >>
              object.local_transform.translation.x >> object.local_transform.translation.y >>
              object.local_transform.translation.z >> object.local_transform.rotation.x >>
              object.local_transform.rotation.y >> object.local_transform.rotation.z >>
              object.local_transform.rotation.w >> object.local_transform.scale.x >>
              object.local_transform.scale.y >> object.local_transform.scale.z >> visible >> locked)) {
            return core::Result<ProjectDocument>::failure(parse_error("invalid scene object record"));
        }
        auto mesh_asset = read_uint(input, "scene object mesh id");
        if (!mesh_asset) {
            return core::Result<ProjectDocument>::failure(mesh_asset.error());
        }
        object.id.value = serialized_object_id.value();
        if (visible != 0 && visible != 1) {
            return core::Result<ProjectDocument>::failure(
                parse_error("scene object visibility flag must be 0 or 1"));
        }
        if (locked != 0 && locked != 1) {
            return core::Result<ProjectDocument>::failure(
                parse_error("scene object lock flag must be 0 or 1"));
        }
        if (parent_id.value() != 0) {
            object.parent = scene::ObjectId{parent_id.value()};
        }
        object.visible = visible != 0;
        object.locked = locked != 0;
        if (mesh_asset.value() != 0) {
            object.mesh_asset = mesh_asset.value();
        }
        const scene::ObjectId object_id = object.id;
        const auto parent_link = object.parent;
        object.parent.reset();
        if (auto result = document.scene_.insert_object(std::move(object)); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        parent_links.emplace_back(object_id, parent_link);
    }
    for (const auto& [object, parent] : parent_links) {
        if (auto result = document.scene_.set_parent(object, parent); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
    }

    if (auto result = require_line(input, "MESHES"); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    auto mesh_count = read_uint(input, "mesh count");
    if (!mesh_count) {
        return core::Result<ProjectDocument>::failure(mesh_count.error());
    }
    if (auto result = validate_serialized_count(
            mesh_count.value(), kMaxSerializedMeshes, "project mesh count");
        !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    for (std::uint64_t mesh_index = 0; mesh_index < mesh_count.value(); ++mesh_index) {
        if (auto result = require_line(input, "MESH"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        auto mesh_id = read_uint(input, "mesh id");
        auto ignored_mesh_revision = read_uint(input, "mesh revision");
        if (!mesh_id || !ignored_mesh_revision || mesh_id.value() == 0) {
            return core::Result<ProjectDocument>::failure(parse_error("invalid mesh record"));
        }
        if (ignored_mesh_revision.value() == core::Revision::max_value()) {
            return core::Result<ProjectDocument>::failure(
                parse_error("mesh revision space is exhausted"));
        }
        geometry::EditableMesh mesh;
        if (auto result = require_line(input, "VERTICES"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        auto vertex_count = read_uint(input, "vertex count");
        if (!vertex_count) {
            return core::Result<ProjectDocument>::failure(vertex_count.error());
        }
        if (auto result = validate_serialized_count(
                vertex_count.value(), kMaxSerializedVertices, "mesh vertex count");
            !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        std::vector<geometry::Vertex> mesh_vertices;
        mesh_vertices.reserve(static_cast<std::size_t>(vertex_count.value()));
        std::map<std::uint64_t, geometry::VertexId> vertex_ids;
        for (std::uint64_t vertex_index = 0; vertex_index < vertex_count.value(); ++vertex_index) {
            if (auto result = require_line(input, "VERTEX"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            core::Vec3d position;
            auto serialized_id = read_uint(input, "vertex id");
            if (!serialized_id || !(input >> position.x >> position.y >> position.z) ||
                serialized_id.value() == 0 || !position.finite()) {
                return core::Result<ProjectDocument>::failure(parse_error("invalid mesh vertex record"));
            }
            const geometry::VertexId vertex_id{serialized_id.value()};
            mesh_vertices.push_back(geometry::Vertex{vertex_id, position});
            vertex_ids.emplace(serialized_id.value(), vertex_id);
        }
        if (auto result = require_line(input, "FACES"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        auto face_count = read_uint(input, "face count");
        if (!face_count) {
            return core::Result<ProjectDocument>::failure(face_count.error());
        }
        if (auto result = validate_serialized_count(
                face_count.value(), kMaxSerializedFaces, "mesh face count");
            !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        std::vector<geometry::Face> mesh_faces;
        mesh_faces.reserve(static_cast<std::size_t>(face_count.value()));
        for (std::uint64_t face_index = 0; face_index < face_count.value(); ++face_index) {
            if (auto result = require_line(input, "FACE"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            auto serialized_face_id = read_uint(input, "face id");
            auto face_vertex_count = read_uint(input, "face vertex count");
            if (!serialized_face_id || !face_vertex_count || serialized_face_id.value() == 0 ||
                face_vertex_count.value() > kMaxSerializedFaceVertices) {
                return core::Result<ProjectDocument>::failure(parse_error("invalid mesh face record"));
            }
            std::vector<geometry::VertexId> face_vertices;
            face_vertices.reserve(static_cast<std::size_t>(face_vertex_count.value()));
            for (std::uint64_t vertex_index = 0; vertex_index < face_vertex_count.value(); ++vertex_index) {
                auto serialized_vertex = read_uint(input, "face vertex id");
                if (!serialized_vertex || !vertex_ids.contains(serialized_vertex.value())) {
                    return core::Result<ProjectDocument>::failure(
                        parse_error("mesh face references an unknown serialized vertex"));
                }
                face_vertices.push_back(vertex_ids.at(serialized_vertex.value()));
            }
            const geometry::FaceId face_id{serialized_face_id.value()};
            mesh_faces.push_back(geometry::Face{face_id, std::move(face_vertices)});
        }
        attributes::AttributeSet mesh_attributes;
        attributes::UvSetTable mesh_uv_sets;
        if (version.value() >= 6U) {
            if (auto result = require_line(input, "ATTRIBUTES"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            if (auto result = require_line(input, "DOMAIN_COUNTS"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            auto domain_count = read_uint(input, "attribute domain count");
            if (!domain_count) {
                return core::Result<ProjectDocument>::failure(domain_count.error());
            }
            if (auto result = validate_serialized_count(
                    domain_count.value(), kMaxSerializedAttributeDomains,
                    "mesh attribute domain count");
                !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::set<attributes::AttributeDomain> seen_domains;
            for (std::uint64_t domain_index = 0U;
                 domain_index < domain_count.value(); ++domain_index) {
                if (auto result = require_line(input, "DOMAIN_COUNT"); !result) {
                    return core::Result<ProjectDocument>::failure(result.error());
                }
                auto encoded_domain = read_uint(input, "attribute domain");
                auto encoded_count = read_uint(input, "attribute domain element count");
                if (!encoded_domain || !encoded_count ||
                    encoded_count.value() > std::numeric_limits<std::size_t>::max()) {
                    return core::Result<ProjectDocument>::failure(parse_error(
                        "invalid mesh attribute domain record"));
                }
                const auto domain = static_cast<attributes::AttributeDomain>(
                    encoded_domain.value());
                if (!attributes::valid_domain(domain) || !seen_domains.insert(domain).second) {
                    return core::Result<ProjectDocument>::failure(parse_error(
                        "mesh attribute domain is invalid or duplicated"));
                }
                if (auto result = mesh_attributes.set_domain_count(
                        domain, static_cast<std::size_t>(encoded_count.value()));
                    !result) {
                    return core::Result<ProjectDocument>::failure(result.error());
                }
            }

            if (auto result = require_line(input, "LAYERS"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            auto layer_count = read_uint(input, "attribute layer count");
            if (!layer_count) {
                return core::Result<ProjectDocument>::failure(layer_count.error());
            }
            if (auto result = validate_serialized_count(
                    layer_count.value(), kMaxSerializedAttributeLayers,
                    "mesh attribute layer count");
                !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            for (std::uint64_t layer_index = 0U;
                 layer_index < layer_count.value(); ++layer_index) {
                if (auto result = require_line(input, "LAYER"); !result) {
                    return core::Result<ProjectDocument>::failure(result.error());
                }
                std::string layer_id;
                if (!(input >> std::quoted(layer_id)) || layer_id.empty()) {
                    return core::Result<ProjectDocument>::failure(parse_error(
                        "invalid mesh attribute layer record"));
                }
                auto encoded_domain = read_uint(input, "attribute layer domain");
                auto encoded_type = read_uint(input, "attribute layer type");
                auto value_count = read_uint(input, "attribute layer value count");
                if (!encoded_domain || !encoded_type || !value_count) {
                    return core::Result<ProjectDocument>::failure(parse_error(
                        "invalid mesh attribute layer record"));
                }
                if (auto result = validate_serialized_count(
                        value_count.value(), kMaxSerializedAttributeValues,
                        "mesh attribute value count");
                    !result) {
                    return core::Result<ProjectDocument>::failure(result.error());
                }
                const auto domain = static_cast<attributes::AttributeDomain>(
                    encoded_domain.value());
                const auto type = static_cast<attributes::AttributeType>(encoded_type.value());
                if (!attributes::valid_domain(domain) || !attributes::valid_type(type)) {
                    return core::Result<ProjectDocument>::failure(parse_error(
                        "mesh attribute layer has an unknown domain or type"));
                }
                std::vector<attributes::AttributeValue> values;
                values.reserve(static_cast<std::size_t>(value_count.value()));
                for (std::uint64_t value_index = 0U;
                     value_index < value_count.value(); ++value_index) {
                    if (auto result = require_line(input, "ATTRIBUTE_VALUE"); !result) {
                        return core::Result<ProjectDocument>::failure(result.error());
                    }
                    auto value = read_attribute_value(input, type);
                    if (!value) {
                        return core::Result<ProjectDocument>::failure(value.error());
                    }
                    values.push_back(std::move(value.value()));
                }
                if (auto result = mesh_attributes.add_layer(attributes::AttributeLayer{
                        {std::move(layer_id), domain, type}, std::move(values)});
                    !result) {
                    return core::Result<ProjectDocument>::failure(result.error());
                }
            }

            if (auto result = require_line(input, "UV_SETS"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            auto uv_set_count = read_uint(input, "UV set count");
            if (!uv_set_count) {
                return core::Result<ProjectDocument>::failure(uv_set_count.error());
            }
            if (auto result = validate_serialized_count(
                    uv_set_count.value(), kMaxSerializedUvSets, "mesh UV set count");
                !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            for (std::uint64_t uv_index = 0U;
                 uv_index < uv_set_count.value(); ++uv_index) {
                if (auto result = require_line(input, "UV_SET"); !result) {
                    return core::Result<ProjectDocument>::failure(result.error());
                }
                attributes::UvSetDescriptor descriptor;
                auto encoded_id = read_uint(input, "UV set id");
                int active_for_editing = 0;
                int active_for_render = 0;
                int lightmap_candidate = 0;
                if (!encoded_id ||
                    !(input >> std::quoted(descriptor.name) >> std::quoted(descriptor.layer_id) >>
                      active_for_editing >> active_for_render >> lightmap_candidate >>
                      descriptor.tile_policy_version) ||
                    encoded_id.value() == 0U ||
                    (active_for_editing != 0 && active_for_editing != 1) ||
                    (active_for_render != 0 && active_for_render != 1) ||
                    (lightmap_candidate != 0 && lightmap_candidate != 1)) {
                    return core::Result<ProjectDocument>::failure(parse_error(
                        "invalid mesh UV set record"));
                }
                descriptor.id = encoded_id.value();
                descriptor.active_for_editing = active_for_editing != 0;
                descriptor.active_for_render = active_for_render != 0;
                descriptor.lightmap_candidate = lightmap_candidate != 0;
                if (auto result = mesh_uv_sets.add(std::move(descriptor)); !result) {
                    return core::Result<ProjectDocument>::failure(result.error());
                }
            }
        }
        if (auto added = mesh.insert_bulk(std::move(mesh_vertices), std::move(mesh_faces)); !added) {
            return core::Result<ProjectDocument>::failure(
                added.error().with_context("bulk serialized mesh admission"));
        }
        if (version.value() >= 6U) {
            if (auto result = mesh.set_attribute_payload(
                    std::move(mesh_attributes), std::move(mesh_uv_sets));
                !result) {
                return core::Result<ProjectDocument>::failure(
                    result.error().with_context("serialized mesh attribute admission"));
            }
        }
        if (auto result = mesh.restore_revision(core::Revision(ignored_mesh_revision.value())); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        const auto [mesh_iterator, inserted] = document.meshes_.emplace(mesh_id.value(), std::move(mesh));
        static_cast<void>(mesh_iterator);
        if (!inserted) {
            return core::Result<ProjectDocument>::failure(
                parse_error("project contains a duplicate mesh asset id"));
        }
        if (mesh_id.value() < std::numeric_limits<std::uint64_t>::max()) {
            document.next_mesh_id_ = std::max(document.next_mesh_id_, mesh_id.value() + 1U);
        }
    }

    std::string terminal_record;
    bool topology_receipts_seen = false;
    bool mesh_patches_seen = false;
    if (!(input >> terminal_record)) {
        return core::Result<ProjectDocument>::failure(
            parse_error("project is missing its terminal record"));
    }
    if (terminal_record == "TOPOLOGY_RECEIPTS") {
        topology_receipts_seen = true;
        if (version.value() < 4U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "topology receipt records require project schema version 4"));
        }
        auto receipt_count = read_uint(input, "topology receipt count");
        if (!receipt_count) {
            return core::Result<ProjectDocument>::failure(receipt_count.error());
        }
        if (auto result = validate_serialized_count(
                receipt_count.value(), kMaxSerializedTopologyReceipts,
                "project topology receipt count");
            !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        std::set<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> receipt_keys;
        for (std::uint64_t receipt_index = 0U;
             receipt_index < receipt_count.value(); ++receipt_index) {
            if (auto result = require_line(input, "TOPOLOGY_RECEIPT"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            auto mesh_asset = read_uint(input, "topology receipt mesh asset");
            std::string encoded_receipt;
            if (!mesh_asset || mesh_asset.value() == 0U ||
                !(input >> std::quoted(encoded_receipt))) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "invalid topology receipt record"));
            }
            if (!document.meshes_.contains(mesh_asset.value())) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "topology receipt references an unknown mesh asset"));
            }
            auto receipt = geometry::TopologyEditReceipt::deserialize(encoded_receipt);
            if (!receipt) {
                return core::Result<ProjectDocument>::failure(
                    receipt.error().with_context("serialized topology receipt"));
            }
            const auto key = std::make_tuple(
                mesh_asset.value(), receipt.value().revision_before.value(),
                receipt.value().revision_after.value());
            if (!receipt_keys.insert(key).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project contains a duplicate topology receipt record"));
            }
            TopologyReceiptRecord record{mesh_asset.value(), std::move(receipt.value())};
            if (auto result = record.validate(); !result) {
                return core::Result<ProjectDocument>::failure(result.error().with_context(
                    "serialized topology receipt"));
            }
            document.topology_receipts_.push_back(std::move(record));
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(
                parse_error("project is missing its terminal record"));
        }
    }
    if (version.value() >= 4U && !topology_receipts_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v4 is missing its topology receipt record"));
    }
    if (terminal_record == "MESH_PATCHES") {
        if (version.value() < 29U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "persistent mesh patches require project schema version 29"));
        }
        mesh_patches_seen = true;
        const auto patch_count = read_uint(input, "persistent mesh patch count");
        if (!patch_count) {
            return core::Result<ProjectDocument>::failure(patch_count.error());
        }
        if (auto result = validate_serialized_count(
                patch_count.value(), kMaxSerializedMeshPatchRecords,
                "project persistent mesh patch count");
            !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        std::uint64_t aggregate_patch_bytes = 0U;
        std::set<std::pair<std::uint64_t, std::uint64_t>> patch_keys;
        for (std::uint64_t index = 0U; index < patch_count.value(); ++index) {
            if (auto result = require_line(input, "MESH_PATCH"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            const auto mesh_asset = read_uint(input, "persistent mesh patch asset");
            std::string encoded_patch_digest;
            std::string encoded_source_digest;
            std::string encoded_result_digest;
            std::string patch_bytes;
            if (!mesh_asset || mesh_asset.value() == 0U ||
                !(input >> encoded_patch_digest >> encoded_source_digest >>
                    encoded_result_digest >> std::quoted(patch_bytes))) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "persistent mesh patch record is malformed"));
            }
            if (!document.meshes_.contains(mesh_asset.value())) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "persistent mesh patch references an unknown mesh asset"));
            }
            if (patch_bytes.size() >
                kMaxSerializedMeshPatchAggregateBytes - aggregate_patch_bytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project persistent mesh patch bytes exceed their aggregate limit"));
            }
            aggregate_patch_bytes += static_cast<std::uint64_t>(patch_bytes.size());
            const auto patch_digest = assets::Sha256Digest::from_hex(encoded_patch_digest);
            const auto source_digest = assets::Sha256Digest::from_hex(encoded_source_digest);
            const auto result_digest = assets::Sha256Digest::from_hex(encoded_result_digest);
            if (!patch_digest || !source_digest || !result_digest) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "persistent mesh patch digest encoding is invalid"));
            }
            MeshPatchRecord record{
                mesh_asset.value(), patch_digest.value(), source_digest.value(),
                result_digest.value(), std::move(patch_bytes)};
            if (auto result = record.validate(); !result) {
                return core::Result<ProjectDocument>::failure(result.error().with_context(
                    "serialized persistent mesh patch"));
            }
            const auto patch_revision = persistent_mesh_patch_revision(record.patch_bytes);
            if (!patch_revision || !patch_keys.emplace(
                    record.mesh_asset, patch_revision.value().value()).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project contains an invalid or duplicate persistent mesh patch"));
            }
            document.mesh_patches_.push_back(std::move(record));
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(
                parse_error("project is missing its terminal record"));
        }
    }
    if (version.value() >= 29U && !mesh_patches_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v29 is missing its persistent mesh patch table"));
    }
    if (terminal_record == "EVALUATION_GRAPH_DIGEST") {
        if (version.value() < 3U) {
            return core::Result<ProjectDocument>::failure(
                Diagnostic(ErrorCode::version_mismatch,
                           "evaluation graph references require project schema version 3"));
        }
        std::string encoded_digest;
        if (!(input >> encoded_digest)) {
            return core::Result<ProjectDocument>::failure(
                parse_error("project evaluation graph digest is missing"));
        }
        const auto digest = assets::Sha256Digest::from_hex(encoded_digest);
        if (!digest || digest.value().is_zero()) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project evaluation graph digest is invalid"));
        }
        document.evaluation_graph_digest_ = digest.value();
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(
                parse_error("project is missing its terminal record"));
        }
    }
    bool scientific_model_seen = false;
    if (terminal_record == "SCIENTIFIC_MODEL") {
        if (version.value() < 5U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "scientific model records require project schema version 5"));
        }
        std::string encoded_scientific_model;
        if (!(input >> std::quoted(encoded_scientific_model))) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project scientific model record is invalid"));
        }
        if (encoded_scientific_model.size() > scientific::ScientificModel::kMaxSerializedBytes) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project scientific model record exceeds its size limit"));
        }
        const auto scientific_model = scientific::ScientificModel::deserialize(
            encoded_scientific_model);
        if (!scientific_model) {
            return core::Result<ProjectDocument>::failure(
                scientific_model.error().with_context("serialized scientific model"));
        }
        document.scientific_model_ = scientific_model.value();
        scientific_model_seen = true;
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(
                parse_error("project is missing its terminal record"));
        }
    }
    if (version.value() >= 5U && !scientific_model_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v5 is missing its scientific model record"));
    }
    bool world_model_seen = false;
    if (terminal_record == "WORLD_MODEL") {
        if (version.value() < 7U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "world model records require project schema version 7"));
        }
        std::string encoded_world_model;
        if (!(input >> std::quoted(encoded_world_model))) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project world model record is invalid"));
        }
        if (encoded_world_model.size() > world::WorldModel::kMaxSerializedBytes) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project world model record exceeds its size limit"));
        }
        const auto world_model = world::WorldModel::deserialize(encoded_world_model);
        if (!world_model) {
            return core::Result<ProjectDocument>::failure(
                world_model.error().with_context("serialized world model"));
        }
        document.world_model_ = world_model.value();
        world_model_seen = true;
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(
                parse_error("project is missing its terminal record"));
        }
    }
    if (version.value() >= 7U && !world_model_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v7 is missing its world model record"));
    }
    bool material_catalog_seen = false;
    if (terminal_record == "MATERIAL_CATALOG") {
        if (version.value() < 8U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "material catalog records require project schema version 8"));
        }
        std::string encoded_material_catalog;
        if (!(input >> std::quoted(encoded_material_catalog))) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project material catalog record is invalid"));
        }
        if (encoded_material_catalog.size() > MaterialCatalog::kMaxSerializedBytes) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project material catalog record exceeds its size limit"));
        }
        const auto material_catalog = MaterialCatalog::deserialize(encoded_material_catalog);
        if (!material_catalog) {
            return core::Result<ProjectDocument>::failure(
                material_catalog.error().with_context("serialized material catalog"));
        }
        document.material_catalog_ = material_catalog.value();
        material_catalog_seen = true;
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(
                parse_error("project is missing its terminal record"));
        }
    }
    if (version.value() >= 8U && !material_catalog_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v8 or newer is missing its material catalog record"));
    }
    if (terminal_record == "SEMANTIC_SIDECAR") {
        if (version.value() < 10U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "semantic sidecar records require project schema version 10"));
        }
        std::string encoded_digest;
        std::string encoded_sidecar;
        if (!(input >> encoded_digest >> std::quoted(encoded_sidecar))) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project semantic sidecar record is invalid"));
        }
        if (encoded_sidecar.empty() ||
            encoded_sidecar.size() > kMaxSerializedSemanticSidecarBytes) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project semantic sidecar record exceeds its size limit"));
        }
        const auto digest = assets::Sha256Digest::from_hex(encoded_digest);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded_sidecar.data());
        if (!digest || digest.value().is_zero() ||
            assets::sha256(std::span<const std::uint8_t>(
                bytes, encoded_sidecar.size())) != digest.value()) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project semantic sidecar digest does not bind its bytes"));
        }
        document.semantic_sidecar_bytes_ = std::move(encoded_sidecar);
        document.semantic_sidecar_digest_ = digest.value();
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(
                parse_error("project is missing its terminal record"));
        }
    }
    bool sculpt_stacks_seen = false;
    if (terminal_record == "SCULPT_STACKS") {
        if (version.value() < 11U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "SculptStack records require project schema version 11"));
        }
        sculpt_stacks_seen = true;
        auto stack_count = read_uint(input, "SculptStack count");
        if (!stack_count) {
            return core::Result<ProjectDocument>::failure(stack_count.error());
        }
        if (auto result = validate_serialized_count(
                stack_count.value(), kMaxSerializedSculptStacks, "project SculptStack count");
            !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        for (std::uint64_t stack_index = 0U; stack_index < stack_count.value(); ++stack_index) {
            if (auto result = require_line(input, "SCULPT_STACK"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            auto mesh_asset = read_uint(input, "SculptStack mesh asset");
            std::string encoded_digest;
            std::string encoded_stack;
            if (!mesh_asset || mesh_asset.value() == 0U ||
                !(input >> encoded_digest >> std::quoted(encoded_stack))) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project SculptStack record is invalid"));
            }
            if (!document.meshes_.contains(mesh_asset.value())) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project SculptStack references an unknown mesh asset"));
            }
            if (encoded_stack.empty() ||
                encoded_stack.size() > kMaxSerializedSculptStackBytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project SculptStack record exceeds its size limit"));
            }
            const auto digest = assets::Sha256Digest::from_hex(encoded_digest);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded_stack.data());
            if (!digest || digest.value().is_zero() ||
                assets::sha256(std::span<const std::uint8_t>(
                    bytes, encoded_stack.size())) != digest.value()) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project SculptStack digest does not bind its serialized bytes"));
            }
            if (!document.sculpt_stacks_.emplace(mesh_asset.value(), SerializedSculptStack{
                    std::move(encoded_stack), digest.value()}).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project contains a duplicate SculptStack mesh binding"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(
                parse_error("project is missing its terminal record"));
        }
    }
    if (version.value() >= 11U && !sculpt_stacks_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v11 is missing its SculptStack record"));
    }
    bool garment_assets_seen = false;
    if (terminal_record == "GARMENT_ASSETS") {
        if (version.value() < 12U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "GarmentAsset records require project schema version 12"));
        }
        garment_assets_seen = true;
        auto asset_count = read_uint(input, "GarmentAsset count");
        if (!asset_count) {
            return core::Result<ProjectDocument>::failure(asset_count.error());
        }
        if (auto result = validate_serialized_count(
                asset_count.value(), kMaxSerializedGarmentAssets,
                "project GarmentAsset count"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        for (std::uint64_t asset_index = 0U; asset_index < asset_count.value(); ++asset_index) {
            if (auto result = require_line(input, "GARMENT_ASSET"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            auto mesh_asset = read_uint(input, "GarmentAsset mesh asset");
            std::string encoded_digest;
            std::string encoded_asset;
            if (!mesh_asset || mesh_asset.value() == 0U ||
                !(input >> encoded_digest >> std::quoted(encoded_asset))) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project GarmentAsset record is invalid"));
            }
            if (!document.meshes_.contains(mesh_asset.value())) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project GarmentAsset references an unknown mesh asset"));
            }
            if (encoded_asset.empty() || encoded_asset.size() > kMaxSerializedGarmentAssetBytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project GarmentAsset record exceeds its size limit"));
            }
            const auto digest = assets::Sha256Digest::from_hex(encoded_digest);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded_asset.data());
            if (!digest || digest.value().is_zero() ||
                assets::sha256(std::span<const std::uint8_t>(
                    bytes, encoded_asset.size())) != digest.value()) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project GarmentAsset digest does not bind its serialized bytes"));
            }
            if (!document.garment_assets_.emplace(mesh_asset.value(), SerializedGarmentAsset{
                    std::move(encoded_asset), digest.value()}).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project contains a duplicate GarmentAsset mesh binding"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(
                parse_error("project is missing its terminal record"));
        }
    }
    if (version.value() >= 12U && !garment_assets_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v12 is missing its GarmentAsset record"));
    }
    bool cloth_rest_pose_receipts_seen = false;
    if (terminal_record == "CLOTH_REST_POSE_RECEIPTS") {
        if (version.value() < 13U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "cloth rest-pose receipt records require project schema version 13"));
        }
        cloth_rest_pose_receipts_seen = true;
        auto receipt_count = read_uint(input, "cloth rest-pose receipt count");
        if (!receipt_count) {
            return core::Result<ProjectDocument>::failure(receipt_count.error());
        }
        if (auto result = validate_serialized_count(
                receipt_count.value(), kMaxSerializedClothRestPoseReceipts,
                "project cloth rest-pose receipt count"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        for (std::uint64_t receipt_index = 0U;
             receipt_index < receipt_count.value(); ++receipt_index) {
            if (auto result = require_line(input, "CLOTH_REST_POSE_RECEIPT"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            auto mesh_asset = read_uint(input, "cloth rest-pose receipt mesh asset");
            std::string encoded_digest;
            std::string encoded_receipt;
            if (!mesh_asset || mesh_asset.value() == 0U ||
                !(input >> encoded_digest >> std::quoted(encoded_receipt))) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project cloth rest-pose receipt record is invalid"));
            }
            if (!document.meshes_.contains(mesh_asset.value())) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "cloth rest-pose receipt references an unknown mesh asset"));
            }
            if (encoded_receipt.empty() ||
                encoded_receipt.size() > kMaxSerializedClothRestPoseReceiptBytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "cloth rest-pose receipt record exceeds its size limit"));
            }
            const auto digest = assets::Sha256Digest::from_hex(encoded_digest);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded_receipt.data());
            if (!digest || digest.value().is_zero() ||
                assets::sha256(std::span<const std::uint8_t>(
                    bytes, encoded_receipt.size())) != digest.value()) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "cloth rest-pose receipt digest does not bind its serialized bytes"));
            }
            if (!document.cloth_rest_pose_receipts_.emplace(
                    mesh_asset.value(), SerializedClothRestPoseReceipt{
                        std::move(encoded_receipt), digest.value()}).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project contains a duplicate cloth rest-pose receipt mesh binding"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(
                parse_error("project is missing its terminal record"));
        }
    }
    if (version.value() >= 13U && !cloth_rest_pose_receipts_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v13 is missing its cloth rest-pose receipt record"));
    }
    if (terminal_record == "ENGINEERING_AUTHORING") {
        if (version.value() < 14U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "engineering authoring records require project schema version 14"));
        }
        std::string encoded_digest;
        std::string encoded_authoring;
        if (!(input >> encoded_digest >> std::quoted(encoded_authoring))) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project engineering authoring record is invalid"));
        }
        if (encoded_authoring.empty() ||
            encoded_authoring.size() > kMaxSerializedEngineeringAuthoringBytes) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project engineering authoring record exceeds its size limit"));
        }
        const auto digest = assets::Sha256Digest::from_hex(encoded_digest);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded_authoring.data());
        if (!digest || digest.value().is_zero() ||
            assets::sha256(std::span<const std::uint8_t>(
                bytes, encoded_authoring.size())) != digest.value()) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project engineering authoring digest does not bind its bytes"));
        }
        const auto decoded = engineering::AuthoringDocument::deserialize(encoded_authoring);
        if (!decoded) {
            return core::Result<ProjectDocument>::failure(decoded.error().with_context(
                "serialized engineering authoring"));
        }
        document.engineering_authoring_ = SerializedEngineeringAuthoring{
            std::move(encoded_authoring), digest.value()};
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(
                parse_error("project is missing its terminal record"));
        }
    }
    bool condition_authoring_seen = false;
    if (terminal_record == "CONDITION_AUTHORING") {
        if (version.value() < 14U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "CONDITION authoring records require project schema version 14"));
        }
        condition_authoring_seen = true;
        std::string encoded_digest;
        if (!(input >> encoded_digest)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project CONDITION authoring record is invalid"));
        }
        if (encoded_digest != "NONE") {
            std::string encoded_authoring;
            if (!(input >> std::quoted(encoded_authoring)) || encoded_authoring.empty() ||
                encoded_authoring.size() > kMaxSerializedConditionAuthoringBytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project CONDITION authoring record exceeds its size limit"));
            }
            const auto digest = assets::Sha256Digest::from_hex(encoded_digest);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded_authoring.data());
            if (!digest || digest.value().is_zero() ||
                assets::sha256(std::span<const std::uint8_t>(
                    bytes, encoded_authoring.size())) != digest.value()) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project CONDITION authoring digest does not bind its bytes"));
            }
            const auto decoded = condition::AuthoringDocument::deserialize(encoded_authoring);
            if (!decoded) {
                return core::Result<ProjectDocument>::failure(decoded.error().with_context(
                    "serialized CONDITION authoring"));
            }
            document.condition_authoring_ = SerializedConditionAuthoring{
                std::move(encoded_authoring), digest.value()};
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(
                parse_error("project is missing its terminal record"));
        }
    }
    if (version.value() >= 14U && !condition_authoring_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v14 is missing its CONDITION authoring record"));
    }
    bool presentation_catalog_seen = false;
    if (terminal_record == "PRESENTATION_CATALOG") {
        if (version.value() < 15U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "presentation catalog records require project schema version 15"));
        }
        std::string encoded_presentation_catalog;
        if (!(input >> std::quoted(encoded_presentation_catalog)) ||
            encoded_presentation_catalog.size() > PresentationCatalog::kMaxSerializedBytes) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project presentation catalog record is invalid or oversized"));
        }
        const auto presentation_catalog =
            PresentationCatalog::deserialize(encoded_presentation_catalog);
        if (!presentation_catalog) {
            return core::Result<ProjectDocument>::failure(
                presentation_catalog.error().with_context("serialized presentation catalog"));
        }
        document.presentation_catalog_ = presentation_catalog.value();
        presentation_catalog_seen = true;
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(
                parse_error("project is missing its terminal record"));
        }
    }
    if (version.value() >= 15U && !presentation_catalog_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v15 is missing its presentation catalog record"));
    }
    bool curve_paths_seen = false;
    if (terminal_record == "CURVE_PATHS") {
        if (version.value() < 16U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "curve path records require project schema version 16"));
        }
        curve_paths_seen = true;
        auto curve_path_count = read_uint(input, "curve path count");
        if (!curve_path_count) {
            return core::Result<ProjectDocument>::failure(curve_path_count.error());
        }
        if (auto result = validate_serialized_count(
                curve_path_count.value(), kMaxSerializedCurvePaths,
                "project curve path count"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        std::uint64_t aggregate_bytes = 0U;
        for (std::uint64_t index = 0U; index < curve_path_count.value(); ++index) {
            if (auto result = require_line(input, "CURVE_PATH"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::string identity;
            std::string encoded_path;
            if (!(input >> std::quoted(identity) >> std::quoted(encoded_path)) ||
                encoded_path.empty() || encoded_path.size() > kMaxSerializedCurvePathBytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project curve path record is invalid or oversized"));
            }
            if (encoded_path.size() >
                kMaxSerializedCurvePathAggregateBytes - aggregate_bytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project curve paths exceed their aggregate byte limit"));
            }
            aggregate_bytes += static_cast<std::uint64_t>(encoded_path.size());
            auto path = geometry::CurvePath::deserialize(encoded_path);
            if (!path) {
                return core::Result<ProjectDocument>::failure(path.error().with_context(
                    "serialized project curve path"));
            }
            if (path.value().source_identity != identity ||
                !document.curve_paths_.emplace(identity, std::move(path.value())).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project curve path identity is duplicate or unbound"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its terminal record"));
        }
    }
    if (version.value() >= 16U && !curve_paths_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v16 is missing its curve path table"));
    }
    bool road_sources_seen = false;
    if (terminal_record == "ROAD_SOURCES") {
        if (version.value() < 17U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "road source records require project schema version 17"));
        }
        road_sources_seen = true;
        auto road_source_count = read_uint(input, "road source count");
        if (!road_source_count) {
            return core::Result<ProjectDocument>::failure(road_source_count.error());
        }
        if (auto result = validate_serialized_count(
                road_source_count.value(), kMaxSerializedRoadSources,
                "project road source count"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        std::uint64_t aggregate_bytes = 0U;
        for (std::uint64_t index = 0U; index < road_source_count.value(); ++index) {
            if (auto result = require_line(input, "ROAD_SOURCE"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::string identity;
            std::string encoded_road;
            if (!(input >> std::quoted(identity) >> std::quoted(encoded_road)) ||
                encoded_road.empty() || encoded_road.size() > kMaxSerializedRoadSourceBytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project road source record is invalid or oversized"));
            }
            if (encoded_road.size() >
                kMaxSerializedRoadSourceAggregateBytes - aggregate_bytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project road sources exceed their aggregate byte limit"));
            }
            aggregate_bytes += static_cast<std::uint64_t>(encoded_road.size());
            auto road = RoadSource::deserialize(encoded_road);
            if (!road) {
                return core::Result<ProjectDocument>::failure(road.error().with_context(
                    "serialized project road source"));
            }
            if (road.value().source_identity != identity ||
                !document.road_sources_.emplace(identity, std::move(road.value())).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project road source identity is duplicate or unbound"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its terminal record"));
        }
    }
    if (version.value() >= 17U && !road_sources_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v17 is missing its road source table"));
    }
    bool sweep_sources_seen = false;
    if (terminal_record == "SWEEP_SOURCES") {
        if (version.value() < 22U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "sweep source records require project schema version 22"));
        }
        sweep_sources_seen = true;
        auto source_count = read_uint(input, "sweep source count");
        if (!source_count) {
            return core::Result<ProjectDocument>::failure(source_count.error());
        }
        if (auto result = validate_serialized_count(
                source_count.value(), kMaxSerializedSweepSources,
                "project sweep-source count"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        std::uint64_t aggregate_bytes = 0U;
        for (std::uint64_t index = 0U; index < source_count.value(); ++index) {
            if (auto result = require_line(input, "SWEEP_SOURCE"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::string identity;
            std::string encoded_source;
            if (!(input >> std::quoted(identity) >> std::quoted(encoded_source)) ||
                encoded_source.empty() ||
                encoded_source.size() > kMaxSerializedSweepSourceBytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project sweep source record is invalid or oversized"));
            }
            if (encoded_source.size() >
                kMaxSerializedSweepSourceAggregateBytes - aggregate_bytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project sweep sources exceed their aggregate byte limit"));
            }
            aggregate_bytes += static_cast<std::uint64_t>(encoded_source.size());
            auto source = geometry::SweepSource::deserialize(encoded_source);
            if (!source) {
                return core::Result<ProjectDocument>::failure(source.error().with_context(
                    "serialized project sweep source"));
            }
            if (source.value().source_identity != identity ||
                !document.sweep_sources_.emplace(identity, std::move(source.value())).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project sweep source identity is duplicate or unbound"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its terminal record"));
        }
    }
    if (version.value() >= 22U && !sweep_sources_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v22 is missing its sweep-source table"));
    }
    bool sign_text_sources_seen = false;
    if (terminal_record == "SIGN_TEXT_SOURCES") {
        if (version.value() < 23U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "sign-text source records require project schema version 23"));
        }
        sign_text_sources_seen = true;
        auto source_count = read_uint(input, "sign-text source count");
        if (!source_count) {
            return core::Result<ProjectDocument>::failure(source_count.error());
        }
        if (auto result = validate_serialized_count(
                source_count.value(), kMaxSerializedSignTextSources,
                "project sign-text source count"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        std::uint64_t aggregate_bytes = 0U;
        for (std::uint64_t index = 0U; index < source_count.value(); ++index) {
            if (auto result = require_line(input, "SIGN_TEXT_SOURCE"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::string identity;
            std::string encoded_source;
            if (!(input >> std::quoted(identity) >> std::quoted(encoded_source)) ||
                encoded_source.empty() ||
                encoded_source.size() > kMaxSerializedSignTextSourceBytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project sign-text source record is invalid or oversized"));
            }
            if (encoded_source.size() >
                kMaxSerializedSignTextSourceAggregateBytes - aggregate_bytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project sign-text sources exceed their aggregate byte limit"));
            }
            aggregate_bytes += static_cast<std::uint64_t>(encoded_source.size());
            auto source = geometry::SignTextSource::deserialize(encoded_source);
            if (!source) {
                return core::Result<ProjectDocument>::failure(source.error().with_context(
                    "serialized project sign-text source"));
            }
            if (source.value().source_identity != identity ||
                !document.sign_text_sources_.emplace(identity, std::move(source.value())).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project sign-text source identity is duplicate or unbound"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its terminal record"));
        }
    }
    if (version.value() >= 23U && !sign_text_sources_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v23 is missing its sign-text source table"));
    }
    bool spatial_fields_seen = false;
    if (terminal_record == "SPATIAL_FIELDS") {
        if (version.value() < 18U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "spatial field records require project schema version 18"));
        }
        spatial_fields_seen = true;
        auto field_count = read_uint(input, "spatial field count");
        if (!field_count) {
            return core::Result<ProjectDocument>::failure(field_count.error());
        }
        if (auto result = validate_serialized_count(
                field_count.value(), kMaxSerializedSpatialFields,
                "project spatial-field count"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        std::uint64_t aggregate_bytes = 0U;
        for (std::uint64_t index = 0U; index < field_count.value(); ++index) {
            if (auto result = require_line(input, "SPATIAL_FIELD"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::string identity;
            std::string encoded_field;
            if (!(input >> std::quoted(identity) >> std::quoted(encoded_field)) ||
                encoded_field.empty() || encoded_field.size() > kMaxSerializedSpatialFieldBytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project spatial field record is invalid or oversized"));
            }
            if (encoded_field.size() >
                kMaxSerializedSpatialFieldAggregateBytes - aggregate_bytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project spatial fields exceed their aggregate byte limit"));
            }
            aggregate_bytes += static_cast<std::uint64_t>(encoded_field.size());
            auto field = spatial_field::ScalarField2D::deserialize(encoded_field);
            if (!field) {
                return core::Result<ProjectDocument>::failure(field.error().with_context(
                    "serialized project spatial field"));
            }
            if (field.value().identity() != identity ||
                !document.spatial_fields_.emplace(identity, std::move(field.value())).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project spatial-field identity is duplicate or unbound"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its terminal record"));
        }
    }
    if (version.value() >= 18U && !spatial_fields_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v18 is missing its spatial-field table"));
    }
    bool spatial_field_lineage_seen = false;
    if (terminal_record == "SPATIAL_FIELD_LINEAGE") {
        if (version.value() < 19U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "spatial field lineage records require project schema version 19"));
        }
        spatial_field_lineage_seen = true;
        auto lineage_count = read_uint(input, "spatial-field lineage count");
        if (!lineage_count) {
            return core::Result<ProjectDocument>::failure(lineage_count.error());
        }
        if (auto result = validate_serialized_count(
                lineage_count.value(), kMaxSerializedSpatialFieldLineage,
                "project spatial-field lineage count"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        for (std::uint64_t index = 0U; index < lineage_count.value(); ++index) {
            if (auto result = require_line(input, "SPATIAL_FIELD_LINEAGE_RECORD"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::string identity;
            if (!(input >> std::quoted(identity)) || !valid_spatial_field_identity(identity)) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project spatial-field lineage identity is malformed"));
            }
            auto lineage_revision = read_uint(input, "spatial-field lineage revision");
            if (!lineage_revision || lineage_revision.value() == 0U ||
                !document.spatial_field_lineage_.emplace(
                    std::move(identity), core::Revision{lineage_revision.value()}).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project spatial-field lineage record is invalid or duplicated"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its terminal record"));
        }
    }
    if (version.value() >= 19U && !spatial_field_lineage_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v19 is missing its spatial-field lineage table"));
    }
    if (version.value() < 19U) {
        for (const auto& [identity, field] : document.spatial_fields_) {
            document.spatial_field_lineage_.emplace(identity, field.source_revision());
        }
    }
    bool curve_path_lineage_seen = false;
    if (terminal_record == "CURVE_PATH_LINEAGE") {
        if (version.value() < 20U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "curve source lineage records require project schema version 20"));
        }
        curve_path_lineage_seen = true;
        auto lineage_count = read_uint(input, "curve source lineage count");
        if (!lineage_count) {
            return core::Result<ProjectDocument>::failure(lineage_count.error());
        }
        if (auto result = validate_serialized_count(
                lineage_count.value(), kMaxSerializedCurvePathLineage,
                "project curve source lineage count"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        for (std::uint64_t index = 0U; index < lineage_count.value(); ++index) {
            if (auto result = require_line(input, "CURVE_PATH_LINEAGE_RECORD"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::string identity;
            if (!(input >> std::quoted(identity)) ||
                !valid_curve_or_road_identity(identity)) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project curve source lineage identity is malformed"));
            }
            auto lineage_revision = read_uint(input, "curve source lineage revision");
            if (!lineage_revision || lineage_revision.value() == 0U ||
                !document.curve_path_lineage_.emplace(
                    std::move(identity), core::Revision{lineage_revision.value()}).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project curve source lineage record is invalid or duplicated"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its terminal record"));
        }
    }
    if (version.value() >= 20U && !curve_path_lineage_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v20 is missing its curve-source lineage table"));
    }
    bool road_source_lineage_seen = false;
    if (terminal_record == "ROAD_SOURCE_LINEAGE") {
        if (version.value() < 20U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "road source lineage records require project schema version 20"));
        }
        road_source_lineage_seen = true;
        auto lineage_count = read_uint(input, "road source lineage count");
        if (!lineage_count) {
            return core::Result<ProjectDocument>::failure(lineage_count.error());
        }
        if (auto result = validate_serialized_count(
                lineage_count.value(), kMaxSerializedRoadSourceLineage,
                "project road-source lineage count"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        for (std::uint64_t index = 0U; index < lineage_count.value(); ++index) {
            if (auto result = require_line(input, "ROAD_SOURCE_LINEAGE_RECORD"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::string identity;
            if (!(input >> std::quoted(identity)) ||
                !valid_curve_or_road_identity(identity)) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project road-source lineage identity is malformed"));
            }
            auto lineage_revision = read_uint(input, "road source lineage revision");
            if (!lineage_revision || lineage_revision.value() == 0U ||
                !document.road_source_lineage_.emplace(
                    std::move(identity), core::Revision{lineage_revision.value()}).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project road-source lineage record is invalid or duplicated"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its terminal record"));
        }
    }
    if (version.value() >= 20U && !road_source_lineage_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v20 is missing its road-source lineage table"));
    }
    if (version.value() < 20U) {
        for (const auto& [identity, path] : document.curve_paths_) {
            document.curve_path_lineage_.emplace(identity, path.source_revision);
        }
        for (const auto& [identity, road] : document.road_sources_) {
            document.road_source_lineage_.emplace(identity, road.source_revision);
        }
    }
    bool sweep_source_lineage_seen = false;
    if (terminal_record == "SWEEP_SOURCE_LINEAGE") {
        if (version.value() < 22U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "sweep-source lineage records require project schema version 22"));
        }
        sweep_source_lineage_seen = true;
        auto lineage_count = read_uint(input, "sweep-source lineage count");
        if (!lineage_count) {
            return core::Result<ProjectDocument>::failure(lineage_count.error());
        }
        if (auto result = validate_serialized_count(
                lineage_count.value(), kMaxSerializedSweepSourceLineage,
                "project sweep-source lineage count"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        for (std::uint64_t index = 0U; index < lineage_count.value(); ++index) {
            if (auto result = require_line(input, "SWEEP_SOURCE_LINEAGE_RECORD"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::string identity;
            if (!(input >> std::quoted(identity)) ||
                !valid_curve_or_road_identity(identity)) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project sweep-source lineage identity is malformed"));
            }
            auto lineage_revision = read_uint(input, "sweep-source lineage revision");
            if (!lineage_revision || lineage_revision.value() == 0U ||
                !document.sweep_source_lineage_.emplace(
                    std::move(identity), core::Revision{lineage_revision.value()}).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project sweep-source lineage record is invalid or duplicated"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its terminal record"));
        }
    }
    if (version.value() >= 22U && !sweep_source_lineage_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v22 is missing its sweep-source lineage table"));
    }
    bool sign_text_source_lineage_seen = false;
    if (terminal_record == "SIGN_TEXT_SOURCE_LINEAGE") {
        if (version.value() < 23U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "sign-text source lineage requires project schema version 23"));
        }
        sign_text_source_lineage_seen = true;
        auto lineage_count = read_uint(input, "sign-text source lineage count");
        if (!lineage_count) {
            return core::Result<ProjectDocument>::failure(lineage_count.error());
        }
        if (auto result = validate_serialized_count(
                lineage_count.value(), kMaxSerializedSignTextSourceLineage,
                "project sign-text source lineage count"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        for (std::uint64_t index = 0U; index < lineage_count.value(); ++index) {
            if (auto result = require_line(input, "SIGN_TEXT_SOURCE_LINEAGE_RECORD"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::string identity;
            if (!(input >> std::quoted(identity))) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project sign-text source lineage identity is malformed"));
            }
            auto lineage_revision = read_uint(input, "sign-text source lineage revision");
            geometry::SignTextSource identity_probe;
            identity_probe.source_identity = identity;
            identity_probe.source_revision = core::Revision{
                lineage_revision ? lineage_revision.value() : 0U};
            identity_probe.text = "A";
            if (!lineage_revision || !identity_probe.validate() ||
                !document.sign_text_source_lineage_.emplace(
                    std::move(identity), core::Revision{lineage_revision.value()}).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project sign-text source lineage record is invalid or duplicated"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its terminal record"));
        }
    }
    if (version.value() >= 23U && !sign_text_source_lineage_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v23 is missing its sign-text source lineage table"));
    }
    bool sign_text_instance_receipts_seen = false;
    if (terminal_record == "SIGN_TEXT_INSTANCE_RECEIPTS") {
        if (version.value() < 23U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "sign-text instance receipts require project schema version 23"));
        }
        sign_text_instance_receipts_seen = true;
        auto receipt_count = read_uint(input, "sign-text instance receipt count");
        if (!receipt_count) {
            return core::Result<ProjectDocument>::failure(receipt_count.error());
        }
        if (auto result = validate_serialized_count(
                receipt_count.value(), kMaxSerializedSignTextInstanceReceipts,
                "project sign-text instance receipt count"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        std::uint64_t aggregate_bytes = 0U;
        for (std::uint64_t index = 0U; index < receipt_count.value(); ++index) {
            if (auto result = require_line(input, "SIGN_TEXT_INSTANCE_RECEIPT"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            auto object_id = read_uint(input, "sign-text instance object identity");
            auto mesh_asset = read_uint(input, "sign-text instance mesh asset");
            std::string source_identity;
            if (!object_id || !mesh_asset || mesh_asset.value() == 0U ||
                !(input >> std::quoted(source_identity))) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project sign-text instance identity fields are malformed"));
            }
            auto source_revision = read_uint(input, "sign-text source revision");
            std::string encoded_digest;
            std::string encoded_source;
            if (!source_revision || !(input >> encoded_digest >> encoded_source) ||
                encoded_source.size() > kMaxSerializedSignTextSourceBytes * 2U ||
                encoded_source.size() / 2U >
                    kMaxSerializedSignTextInstanceReceiptAggregateBytes - aggregate_bytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project sign-text instance receipt is malformed or oversized"));
            }
            const auto digest = assets::Sha256Digest::from_hex(encoded_digest);
            const auto source_snapshot = decode_hex_bytes(
                encoded_source, static_cast<std::size_t>(kMaxSerializedSignTextSourceBytes));
            if (!digest || !source_snapshot.has_value()) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "sign-text receipt digest or source snapshot encoding is invalid"));
            }
            aggregate_bytes += static_cast<std::uint64_t>(source_snapshot->size());
            SignTextInstanceReceipt receipt{
                scene::ObjectId{object_id.value()}, mesh_asset.value(),
                std::move(source_identity), core::Revision{source_revision.value()},
                std::move(*source_snapshot), digest.value()};
            if (auto result = receipt.validate(); !result) {
                return core::Result<ProjectDocument>::failure(result.error().with_context(
                    "serialized project sign-text instance receipt"));
            }
            if (!document.sign_text_instance_receipts_.emplace(
                    receipt.object, std::move(receipt)).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project sign-text instance object identity is duplicated"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its terminal record"));
        }
    }
    if (version.value() >= 23U && !sign_text_instance_receipts_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v23 is missing its sign-text instance receipt table"));
    }
    bool road_surface_instance_receipts_seen = false;
    if (terminal_record == "ROAD_SURFACE_INSTANCE_RECEIPTS") {
        if (version.value() < 24U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "road-surface instance receipts require project schema version 24"));
        }
        road_surface_instance_receipts_seen = true;
        auto receipt_count = read_uint(input, "road-surface instance receipt count");
        if (!receipt_count) {
            return core::Result<ProjectDocument>::failure(receipt_count.error());
        }
        if (auto result = validate_serialized_count(
                receipt_count.value(), kMaxSerializedRoadSurfaceInstanceReceipts,
                "project road-surface instance receipt count"); !result) {
            return core::Result<ProjectDocument>::failure(result.error());
        }
        std::uint64_t aggregate_bytes = 0U;
        for (std::uint64_t index = 0U; index < receipt_count.value(); ++index) {
            if (auto result = require_line(input, "ROAD_SURFACE_INSTANCE_RECEIPT"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            auto object_id = read_uint(input, "road-surface instance object identity");
            auto mesh_asset = read_uint(input, "road-surface instance mesh asset");
            std::string road_identity;
            if (!object_id || !mesh_asset || mesh_asset.value() == 0U ||
                !(input >> std::quoted(road_identity))) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project road-surface instance identity fields are malformed"));
            }
            auto road_revision = read_uint(input, "road-surface source revision");
            auto surface_class = read_uint(input, "road-surface class");
            std::string centerline_identity;
            if (!road_revision || !surface_class ||
                surface_class.value() > static_cast<unsigned>(RoadSurfaceClass::trail) ||
                !(input >> std::quoted(centerline_identity))) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project road-surface class or source revision is malformed"));
            }
            auto centerline_revision = read_uint(input, "road-surface centerline revision");
            auto samples_per_segment = read_uint(input, "road-surface sample density");
            std::string encoded_road_digest;
            std::string encoded_centerline_digest;
            std::string encoded_mesh_digest;
            std::string encoded_road_source;
            std::string encoded_centerline;
            if (!centerline_revision || !samples_per_segment ||
                samples_per_segment.value() > geometry::CurvePath::kMaxSamplesPerSegment ||
                !(input >> encoded_road_digest >> encoded_centerline_digest >>
                    encoded_mesh_digest >> encoded_road_source >> encoded_centerline) ||
                encoded_road_source.size() > RoadSource::kMaxSerializedBytes * 2U ||
                encoded_centerline.size() > geometry::CurvePath::kMaxSerializedBytes * 2U ||
                encoded_road_source.size() / 2U >
                    kMaxSerializedRoadSurfaceInstanceReceiptAggregateBytes - aggregate_bytes ||
                encoded_centerline.size() / 2U >
                    kMaxSerializedRoadSurfaceInstanceReceiptAggregateBytes - aggregate_bytes -
                        encoded_road_source.size() / 2U) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project road-surface source snapshots are malformed or oversized"));
            }
            const auto road_digest = assets::Sha256Digest::from_hex(encoded_road_digest);
            const auto centerline_digest =
                assets::Sha256Digest::from_hex(encoded_centerline_digest);
            const auto generated_mesh_digest =
                assets::Sha256Digest::from_hex(encoded_mesh_digest);
            const auto road_source_snapshot = decode_hex_bytes(
                encoded_road_source, RoadSource::kMaxSerializedBytes);
            const auto centerline_snapshot = decode_hex_bytes(
                encoded_centerline, geometry::CurvePath::kMaxSerializedBytes);
            if (!road_digest || !centerline_digest || !generated_mesh_digest ||
                !road_source_snapshot.has_value() || !centerline_snapshot.has_value()) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "road-surface receipt digest or source snapshot encoding is invalid"));
            }
            aggregate_bytes += static_cast<std::uint64_t>(
                road_source_snapshot->size() + centerline_snapshot->size());
            RoadSurfaceInstanceReceipt receipt{
                scene::ObjectId{object_id.value()}, mesh_asset.value(),
                std::move(road_identity), core::Revision{road_revision.value()},
                static_cast<RoadSurfaceClass>(surface_class.value()),
                std::move(*road_source_snapshot), road_digest.value(),
                std::move(centerline_identity), core::Revision{centerline_revision.value()},
                std::move(*centerline_snapshot), centerline_digest.value(),
                static_cast<std::uint32_t>(samples_per_segment.value()),
                generated_mesh_digest.value()};
            if (auto result = receipt.validate(); !result) {
                return core::Result<ProjectDocument>::failure(result.error().with_context(
                    "serialized project road-surface instance receipt"));
            }
            if (!document.road_surface_instance_receipts_.emplace(
                    receipt.object, std::move(receipt)).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project road-surface instance object identity is duplicated"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its terminal record"));
        }
    }
    if (version.value() >= 24U && !road_surface_instance_receipts_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v24 is missing its road-surface instance receipt table"));
    }
    bool source_asset_registry_seen = false;
    if (terminal_record == "SOURCE_ASSET_REGISTRY_HEX") {
        if (version.value() < 21U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "source asset registry records require project schema version 21"));
        }
        source_asset_registry_seen = true;
        std::string encoded_registry;
        if (!(input >> encoded_registry)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project source asset registry is malformed or oversized"));
        }
        const auto decoded_registry = decode_hex_bytes(
            encoded_registry, assets::ProjectAssetRegistry::kMaxSerializedBytes);
        if (!decoded_registry.has_value()) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project source asset registry encoding is invalid or oversized"));
        }
        auto registry = assets::ProjectAssetRegistry::deserialize(*decoded_registry);
        if (!registry) {
            return core::Result<ProjectDocument>::failure(registry.error().with_context(
                "serialized project source asset registry"));
        }
        if (version.value() < 27U && std::any_of(
                registry.value().records().begin(), registry.value().records().end(),
                [](const auto& entry) {
                    return entry.second.botanical_generation.has_value();
                })) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "botanical generation receipts require project schema version 27"));
        }
        document.source_asset_registry_ = std::move(registry.value());
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its terminal record"));
        }
    }
    if (version.value() >= 21U && !source_asset_registry_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v21 is missing its source asset registry"));
    }
    bool biological_authoring_sources_seen = false;
    if (terminal_record == "BIOLOGICAL_AUTHORING_SOURCES") {
        if (version.value() < 25U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "biological authoring records require project schema version 25"));
        }
        biological_authoring_sources_seen = true;
        const auto source_count = read_uint(input, "biological authoring source count");
        if (!source_count || source_count.value() >
                kMaxSerializedBiologicalAuthoringSources) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project biological authoring source count is invalid"));
        }
        for (std::uint64_t index = 0U; index < source_count.value(); ++index) {
            std::string record;
            std::string source_namespace;
            std::string encoded_digest;
            std::string encoded_source;
            if (!(input >> record) || record != "BIOLOGICAL_AUTHORING_SOURCE" ||
                !(input >> std::quoted(source_namespace))) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project biological authoring source record is invalid"));
            }
            const auto asset_id = read_uint(input, "biological authoring asset identity");
            if (!asset_id || !(input >> encoded_digest >> std::quoted(encoded_source)) ||
                encoded_source.empty() ||
                encoded_source.size() > kMaxSerializedBiologicalAuthoringBytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project biological authoring source payload is invalid or oversized"));
            }
            const auto digest = assets::Sha256Digest::from_hex(encoded_digest);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded_source.data());
            if (!digest || digest.value().is_zero() ||
                assets::sha256(std::span<const std::uint8_t>(
                    bytes, encoded_source.size())) != digest.value()) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project biological authoring digest does not bind its bytes"));
            }
            auto source = biological::BiologicalAuthoringDocument::deserialize(encoded_source);
            if (!source || source.value().serialize() != encoded_source ||
                source.value().source.source_namespace != source_namespace ||
                source.value().source.asset_id != asset_id.value()) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project biological authoring source identity or canonical data is invalid"));
            }
            const BiologicalAuthoringKey key{std::move(source_namespace), asset_id.value()};
            if (!document.biological_authoring_sources_.emplace(
                    key, SerializedBiologicalAuthoring{
                        std::move(encoded_source), digest.value()}).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project biological authoring identity is duplicated"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its biological authoring lineage table"));
        }
    }
    bool biological_authoring_lineage_seen = false;
    if (terminal_record == "BIOLOGICAL_AUTHORING_LINEAGE") {
        if (version.value() < 25U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "biological authoring lineage requires project schema version 25"));
        }
        biological_authoring_lineage_seen = true;
        const auto lineage_count = read_uint(input, "biological authoring lineage count");
        if (!lineage_count || lineage_count.value() >
                kMaxSerializedBiologicalAuthoringSources) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project biological authoring lineage count is invalid"));
        }
        for (std::uint64_t index = 0U; index < lineage_count.value(); ++index) {
            std::string record;
            std::string source_namespace;
            if (!(input >> record) || record != "BIOLOGICAL_AUTHORING_LINEAGE_RECORD" ||
                !(input >> std::quoted(source_namespace))) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project biological authoring lineage record is invalid"));
            }
            const auto asset_id = read_uint(input, "biological lineage asset identity");
            const auto lineage_revision = read_uint(
                input, "biological source lineage revision");
            if (!asset_id || !lineage_revision || lineage_revision.value() == 0U ||
                lineage_revision.value() == std::numeric_limits<std::uint64_t>::max()) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project biological authoring lineage identity or revision is invalid"));
            }
            const BiologicalAuthoringKey key{std::move(source_namespace), asset_id.value()};
            if (!document.biological_authoring_lineage_.emplace(
                    key, core::Revision{lineage_revision.value()}).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project biological authoring lineage identity is duplicated"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its biological proxy receipt table"));
        }
    }
    bool biological_proxy_instance_receipts_seen = false;
    if (terminal_record == "BIOLOGICAL_PROXY_INSTANCE_RECEIPTS") {
        if (version.value() < 26U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "biological proxy instance receipts require project schema version 26"));
        }
        biological_proxy_instance_receipts_seen = true;
        const auto receipt_count = read_uint(input, "biological proxy receipt count");
        if (!receipt_count || receipt_count.value() >
                kMaxSerializedBiologicalProxyInstanceReceipts) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project biological proxy receipt count is invalid"));
        }
        std::uint64_t aggregate_bytes = 0U;
        for (std::uint64_t index = 0U; index < receipt_count.value(); ++index) {
            if (auto result = require_line(input, "BIOLOGICAL_PROXY_INSTANCE_RECEIPT");
                !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            const auto object_id = read_uint(input, "biological proxy object identity");
            const auto mesh_asset = read_uint(input, "biological proxy mesh asset");
            std::string source_namespace;
            if (!object_id || !mesh_asset || mesh_asset.value() == 0U ||
                !(input >> std::quoted(source_namespace))) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project biological proxy receipt object or mesh identity is malformed"));
            }
            const auto source_asset_id = read_uint(input, "biological proxy source asset");
            const auto source_revision = read_uint(input, "biological proxy source revision");
            const auto structure_id = read_uint(input, "biological proxy structure identity");
            std::string encoded_source_digest;
            std::string encoded_product_digest;
            std::string encoded_snapshot;
            if (!source_asset_id || !source_revision || !structure_id ||
                !(input >> encoded_source_digest >> encoded_product_digest >> encoded_snapshot) ||
                encoded_snapshot.size() >
                    kMaxSerializedBiologicalProxyInstanceReceiptAggregateBytes * 2U ||
                encoded_snapshot.size() / 2U >
                    kMaxSerializedBiologicalProxyInstanceReceiptAggregateBytes -
                        aggregate_bytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project biological proxy receipt payload is malformed or oversized"));
            }
            const auto source_digest = assets::Sha256Digest::from_hex(encoded_source_digest);
            const auto product_digest = assets::Sha256Digest::from_hex(encoded_product_digest);
            auto source_snapshot = decode_hex_bytes(
                encoded_snapshot, biological::BiologicalAuthoringDocument::kMaxSerializedBytes);
            if (!source_digest || !product_digest || !source_snapshot.has_value() ||
                source_snapshot->size() >
                    kMaxSerializedBiologicalProxyInstanceReceiptAggregateBytes -
                        aggregate_bytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project biological proxy receipt digests or source encoding are invalid"));
            }
            aggregate_bytes += static_cast<std::uint64_t>(source_snapshot->size());
            BiologicalProxyInstanceReceipt receipt{
                scene::ObjectId{object_id.value()}, mesh_asset.value(),
                std::move(source_namespace), source_asset_id.value(),
                core::Revision{source_revision.value()}, structure_id.value(),
                std::move(*source_snapshot), source_digest.value(), product_digest.value()};
            if (auto result = receipt.validate(); !result) {
                return core::Result<ProjectDocument>::failure(result.error().with_context(
                    "serialized project biological proxy instance receipt"));
            }
            if (!document.biological_proxy_instance_receipts_.emplace(
                    receipt.object, std::move(receipt)).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project biological proxy object identity is duplicated"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its END record"));
        }
    }
    if (version.value() >= 25U &&
        (!biological_authoring_sources_seen || !biological_authoring_lineage_seen)) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v25 is missing biological authoring sources or lineage"));
    }
    if (version.value() >= 26U && !biological_proxy_instance_receipts_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v26 is missing its biological proxy instance receipt table"));
    }
    bool condition_vegetation_placement_receipts_seen = false;
    if (terminal_record == "CONDITION_VEGETATION_PLACEMENT_RECEIPTS") {
        if (version.value() < 28U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "CONDITION vegetation placement receipts require project schema version 28"));
        }
        condition_vegetation_placement_receipts_seen = true;
        const auto receipt_count = read_uint(
            input, "CONDITION vegetation placement receipt count");
        if (!receipt_count || receipt_count.value() >
                kMaxSerializedConditionVegetationPlacementReceipts) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project CONDITION vegetation placement receipt count is invalid"));
        }
        for (std::uint64_t index = 0U; index < receipt_count.value(); ++index) {
            if (auto result = require_line(
                    input, "CONDITION_VEGETATION_PLACEMENT_RECEIPT"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            const auto object = read_uint(input, "vegetation placement object");
            const auto mesh_asset = read_uint(input, "vegetation placement mesh asset");
            const auto stable_id = read_uint(input, "vegetation stable placement id");
            const auto recipe_id = read_uint(input, "vegetation recipe id");
            const auto species_id = read_uint(input, "vegetation species asset id");
            std::string source_identity;
            if (!object || !mesh_asset || !stable_id || !recipe_id || !species_id ||
                !(input >> std::quoted(source_identity))) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project CONDITION vegetation placement identity is malformed"));
            }
            const auto source_generation = read_uint(
                input, "vegetation plant-source generation");
            std::string encoded_source_digest;
            if (!source_generation || !(input >> encoded_source_digest)) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project CONDITION vegetation plant-source receipt is malformed"));
            }
            const auto source_project_revision = read_uint(
                input, "vegetation source project revision");
            const auto authoring_revision = read_uint(
                input, "vegetation authoring revision");
            std::string encoded_authoring_digest;
            std::string encoded_preview_digest;
            if (!source_project_revision || !authoring_revision || !(input >> encoded_authoring_digest >>
                    encoded_preview_digest)) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project CONDITION vegetation preview receipt is malformed"));
            }
            const auto x = read_scalar<double>(input, "vegetation placement x");
            const auto y = read_scalar<double>(input, "vegetation placement y");
            const auto z = read_scalar<double>(input, "vegetation placement z");
            const auto yaw = read_scalar<double>(input, "vegetation placement yaw");
            const auto scale = read_scalar<double>(input, "vegetation placement scale");
            const auto suitability = read_scalar<double>(
                input, "vegetation placement suitability");
            const auto field_count = read_uint(input, "vegetation placement field count");
            if (!x || !y || !z || !yaw || !scale || !suitability || !field_count ||
                field_count.value() != 5U) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project CONDITION vegetation placement transform or field count is malformed"));
            }
            const auto source_digest = assets::Sha256Digest::from_hex(encoded_source_digest);
            const auto authoring_digest = assets::Sha256Digest::from_hex(
                encoded_authoring_digest);
            const auto preview_digest = assets::Sha256Digest::from_hex(encoded_preview_digest);
            if (!source_digest || !authoring_digest || !preview_digest) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project CONDITION vegetation placement digest is invalid"));
            }
            ConditionVegetationPlacementReceipt receipt;
            receipt.object = scene::ObjectId{object.value()};
            receipt.mesh_asset = mesh_asset.value();
            receipt.stable_placement_id = stable_id.value();
            receipt.recipe_id = recipe_id.value();
            receipt.species_asset_id = species_id.value();
            receipt.plant_source_identity = std::move(source_identity);
            receipt.plant_source_generation = core::Revision{source_generation.value()};
            receipt.plant_source_digest = source_digest.value();
            receipt.source_project_revision = core::Revision{source_project_revision.value()};
            receipt.authoring_revision = core::Revision{authoring_revision.value()};
            receipt.authoring_digest = authoring_digest.value();
            receipt.preview_digest = preview_digest.value();
            receipt.position = core::Vec3d{x.value(), y.value(), z.value()};
            receipt.yaw_radians = yaw.value();
            receipt.uniform_scale = scale.value();
            receipt.suitability = suitability.value();
            receipt.fields.reserve(5U);
            for (std::uint64_t field_index = 0U; field_index < field_count.value();
                 ++field_index) {
                if (auto result = require_line(
                        input, "CONDITION_VEGETATION_FIELD_RECEIPT"); !result) {
                    return core::Result<ProjectDocument>::failure(result.error());
                }
                std::string identity;
                std::string semantic_name;
                if (!(input >> std::quoted(identity) >> semantic_name)) {
                    return core::Result<ProjectDocument>::failure(parse_error(
                        "project CONDITION vegetation field identity is malformed"));
                }
                const auto source_revision = read_uint(
                    input, "vegetation field source revision");
                std::string encoded_digest;
                if (!source_revision || !(input >> encoded_digest)) {
                    return core::Result<ProjectDocument>::failure(parse_error(
                        "project CONDITION vegetation field evidence is malformed"));
                }
                const auto field_digest = assets::Sha256Digest::from_hex(encoded_digest);
                if (!field_digest) {
                    return core::Result<ProjectDocument>::failure(parse_error(
                        "project CONDITION vegetation field digest is invalid"));
                }
                std::optional<spatial_field::FieldSemantic> semantic;
                for (const auto candidate : {
                         spatial_field::FieldSemantic::moisture,
                         spatial_field::FieldSemantic::shade,
                         spatial_field::FieldSemantic::disturbance,
                         spatial_field::FieldSemantic::vegetation_suitability,
                         spatial_field::FieldSemantic::terrain_elevation}) {
                    if (spatial_field::field_semantic_name(candidate) == semantic_name) {
                        semantic = candidate;
                        break;
                    }
                }
                if (!semantic.has_value()) {
                    return core::Result<ProjectDocument>::failure(parse_error(
                        "project CONDITION vegetation field semantic is unsupported"));
                }
                receipt.fields.push_back(condition::VegetationFieldReceipt{
                    std::move(identity), *semantic,
                    core::Revision{source_revision.value()}, field_digest.value()});
            }
            if (auto result = receipt.validate(); !result) {
                return core::Result<ProjectDocument>::failure(result.error().with_context(
                    "serialized CONDITION vegetation placement receipt"));
            }
            if (!document.condition_vegetation_placement_receipts_.emplace(
                    receipt.object, std::move(receipt)).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project CONDITION vegetation placement object identity is duplicated"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its END record"));
        }
    }
    if (version.value() >= 28U && !condition_vegetation_placement_receipts_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v28 is missing its CONDITION vegetation placement receipts"));
    }
    bool rig_module_templates_seen = false;
    if (terminal_record == "RIG_MODULE_TEMPLATES") {
        if (version.value() < 30U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "rig-module template library requires project schema version 30"));
        }
        rig_module_templates_seen = true;
        const auto template_count = read_uint(input, "rig-module template count");
        if (!template_count || template_count.value() > kMaxSerializedRigModuleTemplates) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project rig-module template count is invalid"));
        }
        std::uint64_t aggregate_bytes = 0U;
        for (std::uint64_t index = 0U; index < template_count.value(); ++index) {
            if (auto result = require_line(input, "RIG_MODULE_TEMPLATE"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::string identity;
            std::string encoded_digest;
            std::string serialized_template;
            if (!(input >> std::quoted(identity))) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project rig-module template identity is malformed"));
            }
            const auto template_revision = read_uint(input, "rig-module template revision");
            if (!template_revision || template_revision.value() == 0U ||
                template_revision.value() == std::numeric_limits<std::uint64_t>::max() ||
                !(input >> encoded_digest >> std::quoted(serialized_template)) ||
                serialized_template.empty() ||
                serialized_template.size() > character::RigModuleTemplate::kMaxSerializedBytes ||
                serialized_template.size() >
                    kMaxSerializedRigModuleTemplateAggregateBytes - aggregate_bytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project rig-module template revision or payload is invalid or oversized"));
            }
            aggregate_bytes += static_cast<std::uint64_t>(serialized_template.size());
            const auto digest = assets::Sha256Digest::from_hex(encoded_digest);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(
                serialized_template.data());
            const auto decoded = parse_rig_module_template(serialized_template);
            if (!digest || digest.value().is_zero() ||
                assets::sha256(std::span<const std::uint8_t>(
                    bytes, serialized_template.size())) != digest.value() ||
                !decoded || decoded.value().name != identity) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project rig-module template digest, identity, or canonical payload is invalid"));
            }
            if (!document.rig_module_templates_.emplace(
                    identity, SerializedRigModuleTemplate{
                        core::Revision{template_revision.value()},
                        std::move(serialized_template), digest.value()}).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project rig-module template identity is duplicated"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its END record"));
        }
    }
    if (version.value() >= 30U && !rig_module_templates_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v30 is missing its rig-module template library"));
    }
    bool decal_placements_seen = false;
    if (terminal_record == "DECAL_PLACEMENTS") {
        if (version.value() < 31U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "decal placement library requires project schema version 31"));
        }
        decal_placements_seen = true;
        const auto placement_count = read_uint(input, "decal placement count");
        if (!placement_count || placement_count.value() > kMaxSerializedDecalPlacements) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project decal placement count is invalid"));
        }
        std::uint64_t aggregate_bytes = 0U;
        for (std::uint64_t index = 0U; index < placement_count.value(); ++index) {
            if (auto result = require_line(input, "DECAL_PLACEMENT"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            const auto placement_id = read_uint(input, "decal placement identity");
            const auto placement_revision = read_uint(input, "decal placement revision");
            std::string encoded_digest;
            std::string serialized_placement;
            if (!placement_id || placement_id.value() == 0U || !placement_revision ||
                placement_revision.value() == 0U ||
                placement_revision.value() == std::numeric_limits<std::uint64_t>::max() ||
                !(input >> encoded_digest >> std::quoted(serialized_placement)) ||
                serialized_placement.empty() ||
                serialized_placement.size() > kMaxSerializedDecalPlacementBytes ||
                serialized_placement.size() >
                    kMaxSerializedDecalPlacementAggregateBytes - aggregate_bytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project decal placement revision or payload is invalid or oversized"));
            }
            aggregate_bytes += static_cast<std::uint64_t>(serialized_placement.size());
            const auto digest = assets::Sha256Digest::from_hex(encoded_digest);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(
                serialized_placement.data());
            if (!digest || digest.value().is_zero() ||
                assets::sha256(std::span<const std::uint8_t>(
                    bytes, serialized_placement.size())) != digest.value() ||
                !document.decal_placements_.emplace(
                    placement_id.value(), SerializedDecalPlacement{
                        core::Revision{placement_revision.value()},
                        std::move(serialized_placement), digest.value()}).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project decal placement digest is invalid or identity is duplicated"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its END record"));
        }
    }
    if (version.value() >= 31U && !decal_placements_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v31 is missing its decal placement library"));
    }
    bool authoring_standards_seen = false;
    if (terminal_record == "AUTHORING_STANDARDS_HEX") {
        if (version.value() < 23U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "project authoring standards require project schema version 23 or later"));
        }
        authoring_standards_seen = true;
        std::string encoded_standards;
        if (!(input >> encoded_standards)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project authoring standards encoding is missing or oversized"));
        }
        const auto decoded_standards = decode_hex_bytes(
            encoded_standards, ProjectAuthoringStandards::kMaxSerializedBytes);
        if (!decoded_standards.has_value()) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project authoring standards encoding is invalid or oversized"));
        }
        auto standards = ProjectAuthoringStandards::deserialize(*decoded_standards);
        if (!standards) {
            return core::Result<ProjectDocument>::failure(standards.error().with_context(
                "serialized project authoring standards"));
        }
        document.authoring_standards_ = std::move(standards.value());
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its END record"));
        }
    }
    if (version.value() >= 32U && !authoring_standards_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v32 is missing its authoring standards"));
    }
    bool pose_libraries_seen = false;
    if (terminal_record == "POSE_LIBRARIES") {
        if (version.value() < 33U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "pose libraries require project schema version 33"));
        }
        pose_libraries_seen = true;
        const auto library_count = read_uint(input, "pose library count");
        if (!library_count || library_count.value() > kMaxSerializedPoseLibraries) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project pose library count is invalid"));
        }
        std::uint64_t aggregate_bytes = 0U;
        for (std::uint64_t index = 0U; index < library_count.value(); ++index) {
            if (auto result = require_line(input, "POSE_LIBRARY"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::string identity;
            std::string encoded_digest;
            std::string serialized_library;
            if (!(input >> std::quoted(identity)) ||
                !valid_pose_library_identity(identity)) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project pose library identity is malformed"));
            }
            const auto library_revision = read_uint(input, "pose library revision");
            if (!library_revision || library_revision.value() == 0U ||
                library_revision.value() == std::numeric_limits<std::uint64_t>::max() ||
                !(input >> encoded_digest >> std::quoted(serialized_library)) ||
                serialized_library.empty() ||
                serialized_library.size() > pose::PoseLibrary::kMaxSerializedBytes ||
                serialized_library.size() >
                    kMaxSerializedPoseLibraryAggregateBytes - aggregate_bytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project pose library revision or payload is invalid or oversized"));
            }
            aggregate_bytes += static_cast<std::uint64_t>(serialized_library.size());
            const auto digest = assets::Sha256Digest::from_hex(encoded_digest);
            auto decoded = pose::PoseLibrary::deserialize(serialized_library);
            if (!digest || digest.value().is_zero() ||
                assets::sha256(std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(serialized_library.data()),
                    serialized_library.size())) != digest.value() ||
                !decoded || decoded.value().serialize() != serialized_library ||
                !document.pose_libraries_.emplace(
                    identity,
                    SerializedPoseLibrary{
                        core::Revision{library_revision.value()},
                        std::move(serialized_library),
                        digest.value()}).second) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project pose library digest, identity, or canonical payload is invalid"));
            }
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its END record"));
        }
    }
    if (version.value() >= 33U && !pose_libraries_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v33 is missing its pose library table"));
    }
    bool weight_edit_receipts_seen = false;
    if (terminal_record == "WEIGHT_EDIT_RECEIPTS") {
        if (version.value() < 33U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "weight edit receipts require project schema version 33"));
        }
        weight_edit_receipts_seen = true;
        const auto receipt_count = read_uint(input, "weight edit receipt count");
        if (!receipt_count || receipt_count.value() > kMaxSerializedWeightEditReceipts) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project weight edit receipt count is invalid"));
        }
        std::uint64_t aggregate_bytes = 0U;
        for (std::uint64_t index = 0U; index < receipt_count.value(); ++index) {
            if (auto result = require_line(input, "WEIGHT_EDIT_RECEIPT"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::string encoded_digest;
            std::string serialized_receipt;
            if (!(input >> encoded_digest >> std::quoted(serialized_receipt)) ||
                serialized_receipt.empty() ||
                serialized_receipt.size() > weights::WeightEditReceipt::kMaxSerializedBytes ||
                serialized_receipt.size() >
                    kMaxSerializedWeightEditReceiptAggregateBytes - aggregate_bytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project weight edit receipt payload is invalid or oversized"));
            }
            aggregate_bytes += static_cast<std::uint64_t>(serialized_receipt.size());
            const auto digest = assets::Sha256Digest::from_hex(encoded_digest);
            auto decoded = weights::WeightEditReceipt::deserialize(serialized_receipt);
            if (!digest || digest.value().is_zero() ||
                assets::sha256(std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(serialized_receipt.data()),
                    serialized_receipt.size())) != digest.value() ||
                !decoded) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project weight edit receipt digest or canonical payload is invalid"));
            }
            document.weight_edit_receipts_.push_back(SerializedWeightEditReceipt{
                std::move(serialized_receipt), digest.value()});
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its END record"));
        }
    }
    if (version.value() >= 33U && !weight_edit_receipts_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v33 is missing its weight edit receipts"));
    }
    bool curve_edit_receipts_seen = false;
    if (terminal_record == "CURVE_EDIT_RECEIPTS") {
        if (version.value() < 33U) {
            return core::Result<ProjectDocument>::failure(Diagnostic(
                ErrorCode::version_mismatch,
                "curve edit receipts require project schema version 33"));
        }
        curve_edit_receipts_seen = true;
        const auto receipt_count = read_uint(input, "curve edit receipt count");
        if (!receipt_count || receipt_count.value() > kMaxSerializedCurveEditReceipts) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project curve edit receipt count is invalid"));
        }
        std::uint64_t aggregate_bytes = 0U;
        for (std::uint64_t index = 0U; index < receipt_count.value(); ++index) {
            if (auto result = require_line(input, "CURVE_EDIT_RECEIPT"); !result) {
                return core::Result<ProjectDocument>::failure(result.error());
            }
            std::string encoded_digest;
            std::string serialized_receipt;
            if (!(input >> encoded_digest >> std::quoted(serialized_receipt)) ||
                serialized_receipt.empty() ||
                serialized_receipt.size() > motion::CurveEditReceipt::kMaxSerializedBytes ||
                serialized_receipt.size() >
                    kMaxSerializedCurveEditReceiptAggregateBytes - aggregate_bytes) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project curve edit receipt payload is invalid or oversized"));
            }
            aggregate_bytes += static_cast<std::uint64_t>(serialized_receipt.size());
            const auto digest = assets::Sha256Digest::from_hex(encoded_digest);
            auto decoded = motion::CurveEditReceipt::deserialize(serialized_receipt);
            if (!digest || digest.value().is_zero() ||
                assets::sha256(std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(serialized_receipt.data()),
                    serialized_receipt.size())) != digest.value() ||
                !decoded) {
                return core::Result<ProjectDocument>::failure(parse_error(
                    "project curve edit receipt digest or canonical payload is invalid"));
            }
            document.curve_edit_receipts_.push_back(SerializedCurveEditReceipt{
                std::move(serialized_receipt), digest.value()});
        }
        if (!(input >> terminal_record)) {
            return core::Result<ProjectDocument>::failure(parse_error(
                "project is missing its END record"));
        }
    }
    if (version.value() >= 33U && !curve_edit_receipts_seen) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project schema v33 is missing its curve edit receipts"));
    }
    if (terminal_record != "END") {
        return core::Result<ProjectDocument>::failure(
            parse_error("project is missing its END record"));
    }
    std::string trailing_token;
    if (input >> trailing_token) {
        return core::Result<ProjectDocument>::failure(
            parse_error("project contains unexpected data after END"));
    }
    if (auto result = document.validate(); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    return core::Result<ProjectDocument>::success(std::move(document));
}

core::Result<ProjectDocument> ProjectDocument::load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<ProjectDocument>::failure(
            Diagnostic(ErrorCode::io_error, "unable to open project file for reading"));
    }
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        return core::Result<ProjectDocument>::failure(
            Diagnostic(ErrorCode::io_error, "unable to inspect project file size"));
    }
    if (file_size > kMaxSerializedProjectBytes) {
        return core::Result<ProjectDocument>::failure(parse_error(
            "project file exceeds the Cartographer size limit of 128 MiB"));
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        return core::Result<ProjectDocument>::failure(
            Diagnostic(ErrorCode::io_error, "failed while reading project file"));
    }
    return deserialize(contents.str());
}

core::Result<SaveReceipt> ProjectDocument::save_atomic(const std::filesystem::path& path) const {
    FileLock file_lock(path);
    if (auto result = file_lock.acquire(); !result) {
        return core::Result<SaveReceipt>::failure(result.error());
    }
    return save_atomic_unlocked(path);
}

core::Result<SaveReceipt> ProjectDocument::save_atomic_unlocked(
    const std::filesystem::path& path) const {
    std::lock_guard lock(g_project_save_mutex);
    if (path.empty() || path.filename().empty()) {
        return core::Result<SaveReceipt>::failure(invalid("project save path must name a file"));
    }
    if (auto result = validate(); !result) {
        return core::Result<SaveReceipt>::failure(result.error());
    }
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code parent_error;
        const bool parent_exists = std::filesystem::exists(parent, parent_error);
        if (parent_error) {
            return core::Result<SaveReceipt>::failure(
                Diagnostic(ErrorCode::io_error, "unable to inspect project save parent directory"));
        }
        if (!parent_exists || !std::filesystem::is_directory(parent, parent_error) || parent_error) {
            return core::Result<SaveReceipt>::failure(
                Diagnostic(ErrorCode::io_error, "project save parent directory does not exist"));
        }
    }

    const std::string serialized = serialize();
    if (auto result = deserialize(serialized); !result) {
        return core::Result<SaveReceipt>::failure(
            result.error().with_context("serialized project failed pre-commit validation"));
    }

    const auto temporary = temporary_path(path);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Result<SaveReceipt>::failure(
            Diagnostic(ErrorCode::io_error, "unable to open project temporary file"));
    }
    output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    output.flush();
    if (!output) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<SaveReceipt>::failure(
            Diagnostic(ErrorCode::io_error, "failed while writing project temporary file"));
    }
    output.close();
    if (!output) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<SaveReceipt>::failure(
            Diagnostic(ErrorCode::io_error, "failed while closing project temporary file"));
    }

    auto replacement = atomic_replace(temporary, path);
    if (!replacement) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<SaveReceipt>::failure(replacement.error());
    }
    return core::Result<SaveReceipt>::success(
        SaveReceipt{path, static_cast<std::uint64_t>(serialized.size()), kSchemaVersion, revision_});
}

} // namespace carto::project
