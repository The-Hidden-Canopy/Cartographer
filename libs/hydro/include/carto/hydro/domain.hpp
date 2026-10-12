#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace carto::hydro {

// A hydro body describes authored domain identity only. It is not a render
// mesh, collider, solver allocation, or game-owned consequence.
enum class HydroBodyKind : std::uint8_t {
    ocean,
    lake,
    river,
    creek,
    pond,
    reservoir,
    canal,
    flood,
    compartment,
};

[[nodiscard]] std::string_view hydro_body_kind_name(HydroBodyKind kind) noexcept;

struct HydroDomain final {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 4U * 1024U * 1024U;
    static constexpr std::size_t kMaxIdentityBytes = 256U;
    static constexpr std::size_t kMaxCoordinateFrameBytes = 128U;
    static constexpr std::size_t kMaxProvenanceBytes = 256U;

    std::string stable_id;
    HydroBodyKind kind = HydroBodyKind::pond;
    std::string coordinate_frame;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    std::string provenance;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<HydroDomain> deserialize(std::string_view text);
};

} // namespace carto::hydro
