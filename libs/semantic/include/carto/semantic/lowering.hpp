#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/semantic/sidecar.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::semantic {

// One explicit tag-lowering binding: an authored world tag, the neutral
// lowered tag it compiles to, and evidence describing the transformation.
// Any precision narrowing or semantic transformation is recorded here as
// evidence rather than silently applied.
struct LoweredTagBinding {
    static constexpr std::size_t kMaxTagBytes = 256U;
    static constexpr std::size_t kMaxEvidenceBytes = 4096U;

    std::string authored_tag;
    std::string lowered_tag;
    std::string transformation_evidence;

    [[nodiscard]] core::Result<void> validate() const;
};

// Digest-bound map from authored tags to lowered tags. The map is bound to
// the exact sidecar digest it was compiled from; verifying it against any
// other sidecar fails closed. Querying an authored tag with no binding is a
// fail-closed lookup error, never a silent identity pass-through.
class TagLoweringMap final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 4U * 1024U * 1024U;
    static constexpr std::size_t kMaxBindingsPerMap = 65'536U;

    TagLoweringMap() = default;
    explicit TagLoweringMap(assets::Sha256Digest bound_sidecar_digest)
        : bound_sidecar_digest_(bound_sidecar_digest) {}

    [[nodiscard]] core::Result<void> insert_binding(LoweredTagBinding binding);

    [[nodiscard]] const assets::Sha256Digest& bound_sidecar_digest() const noexcept {
        return bound_sidecar_digest_;
    }
    [[nodiscard]] const std::vector<LoweredTagBinding>& bindings() const noexcept {
        return bindings_;
    }

    [[nodiscard]] const LoweredTagBinding* find(std::string_view authored_tag) const noexcept;
    [[nodiscard]] core::Result<const LoweredTagBinding*> binding_for(
        std::string_view authored_tag) const;

    [[nodiscard]] core::Result<void> verify_against(const SemanticSidecar& sidecar) const;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<TagLoweringMap> deserialize(std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;

private:
    assets::Sha256Digest bound_sidecar_digest_;
    std::vector<LoweredTagBinding> bindings_;
};

} // namespace carto::semantic
