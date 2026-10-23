#pragma once

#include <carto/character/ids.hpp>

#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace carto::workspace {

// The six native authoring workspaces (ch19). A session is a projection and
// controller of canonical source, never an alternate mutable model: it holds
// active-tool state, the target character, stable-id selections and persisted
// view state, and nothing else. Runtime handles, evaluated poses and playback
// state are deliberately absent.
enum class WorkspaceDomain : std::uint8_t {
    rig,
    skin,
    animate,
    face,
    sim,
    review,
};

[[nodiscard]] std::string_view workspace_domain_name(WorkspaceDomain domain) noexcept;

// The active tool as a bounded, locale-neutral token ("joint.move",
// "weight.paint", "curve.edit"). An empty token means no tool is active.
// Tokens are validated so hostile input can never smuggle markup into UI or
// serialized state.
struct WorkspaceTool {
    static constexpr std::size_t kMaxTokenBytes = 128U;

    std::string token;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const WorkspaceTool&) const noexcept = default;
};

// Every selectable entity class, keyed by stable identity only. Vertex
// selections use the mesh vertex index namespace of the owning binding; clip
// selections use the temporal clip identity namespace.
enum class SelectionKind : std::uint8_t {
    joint,
    control,
    vertex,
    clip,
    morph_target,
    garment,
    groom,
};

[[nodiscard]] std::string_view selection_kind_name(SelectionKind kind) noexcept;

struct SelectionEntry {
    SelectionKind kind = SelectionKind::joint;
    std::uint64_t identity = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const SelectionEntry&) const noexcept = default;
    [[nodiscard]] auto operator<=>(const SelectionEntry&) const noexcept = default;
};

// Per-domain persisted view state: a bounded, sorted token table. Values are
// canonical decimal strings (max_digits10) or opaque preference tokens; no
// runtime handles or engine pointers are ever persisted. This is local panel
// preference, not canonical project state, so it is saved with the workspace
// state file but is never journaled as a source mutation.
class DomainViewState final {
public:
    static constexpr std::size_t kMaxEntries = 256U;
    static constexpr std::size_t kMaxKeyBytes = 128U;
    static constexpr std::size_t kMaxValueBytes = 1'024U;

    [[nodiscard]] core::Result<void> set(std::string key, std::string value);
    [[nodiscard]] const std::string* find(std::string_view key) const noexcept;
    [[nodiscard]] const std::map<std::string, std::string>& entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] core::Result<void> validate() const;

    [[nodiscard]] bool operator==(const DomainViewState&) const noexcept = default;

private:
    std::map<std::string, std::string> entries_;
};

// One workspace session record. Exactly one session exists per domain in a
// controller; the record is serializable so the full workspace layout survives
// save/reopen without touching canonical source.
struct WorkspaceSession {
    static constexpr std::size_t kMaxSelectionEntries = 4'096U;

    WorkspaceDomain domain = WorkspaceDomain::rig;
    // Zero means the session has no target character yet.
    character::CharacterId target_character;
    WorkspaceTool active_tool;
    // Sorted and unique by (kind, identity).
    std::vector<SelectionEntry> selection;
    DomainViewState view_state;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const WorkspaceSession&) const noexcept = default;

    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<WorkspaceSession> deserialize(std::string_view text);
};

} // namespace carto::workspace
