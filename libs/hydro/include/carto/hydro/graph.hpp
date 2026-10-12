#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/hydro/domain.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace carto::hydro {

enum class HydroEdgeKind : std::uint8_t {
    channel,
    culvert,
    pipe,
    pump,
    gate,
    breach,
    overflow,
    boundary,
};

[[nodiscard]] std::string_view hydro_edge_kind_name(HydroEdgeKind kind) noexcept;

struct HydroCell final {
    std::uint64_t id = 0U;
    double area_m2 = 0.0;
    double bed_elevation_m = 0.0;
    double initial_water_depth_m = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] bool operator==(const HydroCell&) const noexcept = default;
};

struct HydroEdge final {
    std::uint64_t id = 0U;
    std::uint64_t from_cell = 0U;
    std::optional<std::uint64_t> to_cell;
    HydroEdgeKind kind = HydroEdgeKind::channel;
    double capacity_m3_per_s = 0.0;
    double roughness = 0.0;
    double sill_elevation_m = 0.0;
    bool enabled = true;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] bool operator==(const HydroEdge&) const noexcept = default;
};

struct HydroBoundaryFlux final {
    // Positive volume enters the referenced cell; negative volume leaves it.
    std::uint64_t edge_id = 0U;
    double volume_m3 = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

struct HydroCellState final {
    std::uint64_t cell_id = 0U;
    double water_depth_m = 0.0;
    core::Vec3d velocity_mps{};

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] bool operator==(const HydroCellState& other) const noexcept {
        return cell_id == other.cell_id && water_depth_m == other.water_depth_m &&
            velocity_mps.x == other.velocity_mps.x &&
            velocity_mps.y == other.velocity_mps.y &&
            velocity_mps.z == other.velocity_mps.z;
    }
};

class HydroGraph;

struct HydroState final {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 8U * 1024U * 1024U;
    static constexpr std::size_t kMaxCells = 65'536U;

    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    std::map<std::uint64_t, HydroCellState> cells;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<HydroState> deserialize(std::string_view text);
};

struct HydroVolumeLedger final {
    double initial_volume_m3 = 0.0;
    double internal_transfer_m3 = 0.0;
    double boundary_inflow_m3 = 0.0;
    double boundary_outflow_m3 = 0.0;
    double final_volume_m3 = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

struct HydroStepResult final {
    HydroState state;
    HydroVolumeLedger ledger;
    assets::Sha256Digest graph_digest;
    assets::Sha256Digest receipt_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

// The graph contains authored topology and static cell geometry. Dynamic
// water state is carried by HydroState and is advanced without mutating the
// graph, so derived consumers cannot silently become hydro authority.
class HydroGraph final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxCells = 65'536U;
    static constexpr std::size_t kMaxEdges = 65'536U;

    explicit HydroGraph(HydroDomain domain = {});

    [[nodiscard]] const HydroDomain& domain() const noexcept { return domain_; }
    [[nodiscard]] const std::map<std::uint64_t, HydroCell>& cells() const noexcept {
        return cells_;
    }
    [[nodiscard]] const std::map<std::uint64_t, HydroEdge>& edges() const noexcept {
        return edges_;
    }

    [[nodiscard]] core::Result<void> insert_cell(HydroCell cell);
    [[nodiscard]] core::Result<void> insert_edge(HydroEdge edge);
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<HydroState> initial_state() const;
    [[nodiscard]] core::Result<double> total_volume_m3(const HydroState& state) const;
    [[nodiscard]] core::Result<HydroStepResult> advance(
        const HydroState& state,
        double delta_seconds,
        std::span<const HydroBoundaryFlux> boundary_fluxes = {}) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<HydroGraph> deserialize(std::string_view text);

private:
    HydroDomain domain_;
    std::map<std::uint64_t, HydroCell> cells_;
    std::map<std::uint64_t, HydroEdge> edges_;
};

} // namespace carto::hydro
