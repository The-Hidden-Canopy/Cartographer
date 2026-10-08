#include <carto/project/project.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

#ifdef _WIN32
#    define NOMINMAX
#    include <windows.h>
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

Diagnostic exhausted_revision() {
    return Diagnostic(ErrorCode::invalid_state, "project revision space is exhausted");
}

constexpr std::uint64_t kMaxSerializedObjects = 1'000'000U;
constexpr std::uint64_t kMaxSerializedMeshes = 1'000'000U;
constexpr std::uint64_t kMaxSerializedVertices = 1'000'000U;
constexpr std::uint64_t kMaxSerializedFaces = 1'000'000U;
constexpr std::uint64_t kMaxSerializedFaceVertices = 1'000'000U;
constexpr std::uint64_t kMaxSerializedTopologyReceipts = 1'000'000U;
constexpr std::uint64_t kMaxSerializedAttributeDomains = 64U;
constexpr std::uint64_t kMaxSerializedAttributeLayers = 1'000'000U;
constexpr std::uint64_t kMaxSerializedAttributeValues = 1'000'000U;
constexpr std::uint64_t kMaxSerializedUvSets = 1'000'000U;
constexpr std::uintmax_t kMaxSerializedProjectBytes = 128ULL * 1024ULL * 1024ULL;
std::mutex g_project_save_mutex;
std::atomic<std::uint64_t> g_project_temp_counter{0U};

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
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::io_error, "atomic project replacement failed on Windows"));
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

} // namespace

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

core::Result<ProjectDocument> ProjectDocument::create(std::string name) {
    ProjectDocument document;
    if (auto result = document.set_name(std::move(name)); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    return core::Result<ProjectDocument>::success(std::move(document));
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
    if (auto result = scene_.remove_object(object); !result) {
        return result;
    }
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
    if (receipt.has_value()) {
        topology_receipts_.push_back(TopologyReceiptRecord{mesh_asset, std::move(*receipt)});
    }
    bump_revision();
    return core::Result<core::Revision>::success(applied_revision);
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
    meshes_.erase(mesh_asset);
    topology_receipts_.erase(
        std::remove_if(
            topology_receipts_.begin(), topology_receipts_.end(),
            [mesh_asset](const TopologyReceiptRecord& record) {
                return record.mesh_asset == mesh_asset;
            }),
        topology_receipts_.end());
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
    if (evaluation_graph_digest_.has_value() && evaluation_graph_digest_->is_zero()) {
        return core::Result<void>::failure(parse_error(
            "project evaluation graph digest must not be zero"));
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
    scene_.swap(other.scene_);
    meshes_.swap(other.meshes_);
    topology_receipts_.swap(other.topology_receipts_);
    std::swap(scientific_model_, other.scientific_model_);
    std::swap(world_model_, other.world_model_);
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
            for (const auto& value : layer.values) {
                output << "ATTRIBUTE_VALUE ";
                write_attribute_value(output, layer.descriptor.type, value);
                output << '\n';
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
    if (evaluation_graph_digest_.has_value()) {
        output << "EVALUATION_GRAPH_DIGEST " << evaluation_graph_digest_->hex() << '\n';
    }
    output << "SCIENTIFIC_MODEL " << std::quoted(scientific_model_.serialize()) << '\n';
    output << "WORLD_MODEL " << std::quoted(world_model_.serialize()) << '\n';
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

    if (auto result = require_line(input, "NAME"); !result) {
        return core::Result<ProjectDocument>::failure(result.error());
    }
    ProjectDocument document;
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
