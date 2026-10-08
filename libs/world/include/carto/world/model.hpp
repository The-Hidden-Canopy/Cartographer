#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::world {

template <typename Tag>
struct Id {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const Id&) const noexcept = default;
};

struct AreaTag;
struct PathTag;
struct StructureGraphTag;
struct InstanceFieldTag;
struct VariantLayerTag;

using AreaId = Id<AreaTag>;
using PathId = Id<PathTag>;
using StructureGraphId = Id<StructureGraphTag>;
using InstanceFieldId = Id<InstanceFieldTag>;
using VariantLayerId = Id<VariantLayerTag>;

enum class AuthoringBindingKind : std::uint8_t {
    none,
    scene_object,
    mesh_asset,
};

[[nodiscard]] std::string_view authoring_binding_kind_name(
    AuthoringBindingKind kind) noexcept;

// A provider-neutral reference back to authoritative Cartographer authoring
// state. It carries no runtime handle and grants no mutation authority.
struct AuthoringBinding {
    AuthoringBindingKind kind = AuthoringBindingKind::none;
    std::uint64_t identity = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const AuthoringBinding&) const noexcept = default;
};

struct SemanticArea {
    AreaId id;
    std::string semantic_type;
    AuthoringBinding geometry;
    std::optional<AreaId> parent;
    std::vector<std::string> tags;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SemanticPath {
    PathId id;
    std::string semantic_type;
    std::vector<core::Vec3d> control_points;
    std::optional<AreaId> start_area;
    std::optional<AreaId> end_area;
    double width = 0.0;
    bool closed = false;

    [[nodiscard]] core::Result<void> validate() const;
};

struct StructureNode {
    std::uint64_t id = 0U;
    std::string semantic_type;
    std::optional<AreaId> area;

    [[nodiscard]] core::Result<void> validate() const;
};

struct StructureEdge {
    std::uint64_t id = 0U;
    std::uint64_t first_node = 0U;
    std::uint64_t second_node = 0U;
    std::string relationship;
    std::optional<PathId> path;
    bool directed = false;

    [[nodiscard]] core::Result<void> validate() const;
};

struct StructureGraph {
    StructureGraphId id;
    std::string semantic_type;
    std::map<std::uint64_t, StructureNode> nodes;
    std::map<std::uint64_t, StructureEdge> edges;

    [[nodiscard]] core::Result<void> validate() const;
};

struct InstanceField {
    InstanceFieldId id;
    std::string semantic_type;
    std::optional<AreaId> area;
    core::Bounds3d bounds;
    std::string recipe_identity;
    assets::Sha256Digest recipe_digest;
    std::uint64_t seed = 0U;
    std::uint64_t maximum_instances = 0U;
    // Stable generated instance identities omitted from reconstruction. The
    // vector must be sorted and unique so its canonical form is unambiguous.
    std::vector<std::uint64_t> suppressed_instances;

    [[nodiscard]] core::Result<void> validate() const;
};

enum class VariantTargetKind : std::uint8_t {
    semantic_area,
    semantic_path,
    structure_graph,
    instance_field,
};

enum class VariantOperation : std::uint8_t {
    set_value,
    remove_value,
};

[[nodiscard]] std::string_view variant_target_kind_name(
    VariantTargetKind kind) noexcept;
[[nodiscard]] std::string_view variant_operation_name(
    VariantOperation operation) noexcept;

// Sparse, bounded authoring override. The value is a canonical public value,
// not an executable command, provider payload, or runtime admission request.
struct VariantEdit {
    VariantTargetKind target_kind = VariantTargetKind::semantic_area;
    std::uint64_t target_identity = 0U;
    VariantOperation operation = VariantOperation::set_value;
    std::string property;
    std::string canonical_value;

    [[nodiscard]] core::Result<void> validate() const;
};

struct VariantLayer {
    VariantLayerId id;
    std::string name;
    core::Revision base_project_revision;
    std::vector<VariantEdit> edits;

    [[nodiscard]] core::Result<void> validate() const;
};

// Authoritative provider-neutral world semantics stored by ProjectDocument.
// Runtime materialization and adaptive representation policy are intentionally
// absent; consumers receive immutable, revision-bound project snapshots.
class WorldModel final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxPrimitivesPerKind = 65'536U;
    static constexpr std::size_t kMaxControlPointsPerPath = 65'536U;
    static constexpr std::size_t kMaxStructureItemsPerGraph = 65'536U;
    static constexpr std::size_t kMaxSuppressedInstances = 65'536U;
    static constexpr std::size_t kMaxVariantEdits = 65'536U;

    [[nodiscard]] core::Result<void> insert_area(SemanticArea area);
    [[nodiscard]] core::Result<void> insert_path(SemanticPath path);
    [[nodiscard]] core::Result<void> insert_structure_graph(StructureGraph graph);
    [[nodiscard]] core::Result<void> insert_instance_field(InstanceField field);
    [[nodiscard]] core::Result<void> insert_variant_layer(VariantLayer layer);

    [[nodiscard]] const std::map<AreaId, SemanticArea>& areas() const noexcept {
        return areas_;
    }
    [[nodiscard]] const std::map<PathId, SemanticPath>& paths() const noexcept {
        return paths_;
    }
    [[nodiscard]] const std::map<StructureGraphId, StructureGraph>&
    structure_graphs() const noexcept {
        return structure_graphs_;
    }
    [[nodiscard]] const std::map<InstanceFieldId, InstanceField>&
    instance_fields() const noexcept {
        return instance_fields_;
    }
    [[nodiscard]] const std::map<VariantLayerId, VariantLayer>&
    variant_layers() const noexcept {
        return variant_layers_;
    }

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<WorldModel> deserialize(std::string_view text);

private:
    std::map<AreaId, SemanticArea> areas_;
    std::map<PathId, SemanticPath> paths_;
    std::map<StructureGraphId, StructureGraph> structure_graphs_;
    std::map<InstanceFieldId, InstanceField> instance_fields_;
    std::map<VariantLayerId, VariantLayer> variant_layers_;
};

} // namespace carto::world
