#pragma once

#include <carto/character/bindings.hpp>
#include <carto/character/definition.hpp>
#include <carto/character/ids.hpp>
#include <carto/character/morph.hpp>
#include <carto/character/motion_document.hpp>
#include <carto/character/receipt.hpp>
#include <carto/character/time.hpp>
#include <carto/character/weights_record.hpp>

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/skin.hpp>
#include <carto/temporal/channel.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::workspace {

// One authored attachment: a source mesh pinned by content digest to one
// joint, with a joint-local offset and a role token. Attachments are keyed and
// sorted by (joint, mesh asset id); a duplicate key is a validation error.
struct AttachmentBinding {
    kinematic::JointId joint;
    character::MeshAssetRef mesh;
    core::Transform offset = core::Transform::identity();
    std::string role;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const AttachmentBinding& other) const noexcept {
        return joint == other.joint && mesh.asset_id == other.mesh.asset_id &&
            mesh.content_digest == other.mesh.content_digest && role == other.role &&
            offset.translation.x == other.offset.translation.x &&
            offset.translation.y == other.offset.translation.y &&
            offset.translation.z == other.offset.translation.z &&
            offset.rotation.x == other.offset.rotation.x &&
            offset.rotation.y == other.offset.rotation.y &&
            offset.rotation.z == other.offset.rotation.z &&
            offset.rotation.w == other.offset.rotation.w &&
            offset.scale.x == other.offset.scale.x &&
            offset.scale.y == other.offset.scale.y &&
            offset.scale.z == other.offset.scale.z;
    }
    // Canonical ordering key: (joint, mesh asset id).
    [[nodiscard]] bool operator<(const AttachmentBinding& other) const noexcept {
        if (joint != other.joint) return joint < other.joint;
        return mesh.asset_id < other.mesh.asset_id;
    }
};

// Bake job intent recorded by the simulation workspace. This headless session
// layer never fabricates solve results: a job stays queued until an external
// solver product is admitted through attach_bake_receipt, and cancellation is
// a forward record, so an old accepted cache remains available throughout a
// new bake (ch19).
enum class BakeJobStatus : std::uint8_t {
    queued,
    cancelled,
    receipted,
};

[[nodiscard]] std::string_view bake_job_status_name(BakeJobStatus status) noexcept;

struct BakeJob {
    static constexpr std::uint32_t kMaxFrames = 1'000'000U;

    std::uint64_t job_id = 0U;
    character::RationalTime frame_start;
    character::RationalTime frame_end;
    std::uint32_t max_frames = 0U;
    assets::Sha256Digest settings_digest;
    BakeJobStatus status = BakeJobStatus::queued;
    // Set exactly when status is receipted.
    std::optional<assets::Sha256Digest> receipt_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const BakeJob& other) const noexcept {
        return job_id == other.job_id &&
            frame_start.tick == other.frame_start.tick &&
            frame_start.ticks_per_second == other.frame_start.ticks_per_second &&
            frame_end.tick == other.frame_end.tick &&
            frame_end.ticks_per_second == other.frame_end.ticks_per_second &&
            max_frames == other.max_frames &&
            settings_digest == other.settings_digest && status == other.status &&
            receipt_digest == other.receipt_digest;
    }
    [[nodiscard]] bool operator<(const BakeJob& other) const noexcept {
        return job_id < other.job_id;
    }
};

// The complete character authoring source one workspace operates on: the
// digest-bound character definition plus the kinematic graph, rig (with
// control graph), optional skin binding and layer record, optional motion
// document and temporal channel, morph targets, attachments, garment/groom
// bindings, bake jobs, weight locks and the review gate ledger. Everything is
// durable authoring truth with stable identities; evaluated state is absent.
struct CharacterSourceBundle {
    static constexpr std::size_t kMaxMorphTargets = 4'096U;
    static constexpr std::size_t kMaxAttachments = 4'096U;
    static constexpr std::size_t kMaxGarments = 256U;
    static constexpr std::size_t kMaxGrooms = 256U;
    static constexpr std::size_t kMaxBakeJobs = 1'024U;
    static constexpr std::size_t kMaxReviewGates = 256U;
    static constexpr std::size_t kMaxGateIdBytes = 128U;

    character::CharacterDefinition definition;
    kinematic::KinematicGraph graph;
    character::RigDefinition rig;
    std::optional<kinematic::SkinBinding> skin;
    std::optional<character::SkinWeightLayer> skin_layer;
    std::optional<character::MotionDocument> motion;
    std::optional<temporal::TemporalChannel> channel;
    // Sorted and unique by morph target id.
    std::vector<character::MorphTarget> morphs;
    // Sorted and unique by (joint, mesh asset id).
    std::vector<AttachmentBinding> attachments;
    std::vector<character::GarmentBinding> garments;
    std::vector<character::GroomBinding> grooms;
    // Sorted and unique by job id.
    std::vector<BakeJob> bake_jobs;
    // Vertices excluded from automatic weight proposals, sorted and unique.
    std::vector<std::uint32_t> weight_locks;
    // Review gate ledger, sorted by gate id (ch19 review workspace).
    std::map<std::string, character::GateStatus> review_gates;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<CharacterSourceBundle> deserialize(
        std::string_view text);
};

// The workspace-owned character authoring store: the one authoring truth the
// session layer projects. Bundles are digest-bound to their source revisions;
// the store revision advances only through journaled forward commands, and a
// stale expected revision fails closed. The store is serialized as one
// deterministic document so save/reopen retains the full workspace state.
class CharacterSourceStore final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxCharacters = 4'096U;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U * 1024U;

    [[nodiscard]] core::Result<void> insert(CharacterSourceBundle bundle);
    [[nodiscard]] core::Result<void> remove(character::CharacterId id);
    [[nodiscard]] CharacterSourceBundle* find(character::CharacterId id) noexcept;
    [[nodiscard]] const CharacterSourceBundle* find(
        character::CharacterId id) const noexcept;
    [[nodiscard]] bool contains(character::CharacterId id) const noexcept;

    [[nodiscard]] const std::map<character::CharacterId, CharacterSourceBundle>&
    bundles() const noexcept { return bundles_; }
    [[nodiscard]] bool empty() const noexcept { return bundles_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return bundles_.size(); }

    [[nodiscard]] core::Revision revision() const noexcept { return revision_; }
    void advance_revision() noexcept { revision_ = revision_.next(); }

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<CharacterSourceStore> deserialize(
        std::string_view text);

private:
    std::map<character::CharacterId, CharacterSourceBundle> bundles_;
    core::Revision revision_;
};

} // namespace carto::workspace
