#include <carto/workspace/session.hpp>

#include "codec.hpp"

#include <algorithm>
#include <utility>

namespace carto::workspace {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_token(std::string_view value, std::size_t maximum, bool allow_empty) {
    if ((value.empty() && !allow_empty) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU;
    });
}

core::Result<SelectionKind> selection_kind_from(std::string_view name) {
    for (std::uint8_t raw = 0U;
         raw <= static_cast<std::uint8_t>(SelectionKind::groom); ++raw) {
        const auto kind = static_cast<SelectionKind>(raw);
        if (selection_kind_name(kind) == name) {
            return core::Result<SelectionKind>::success(kind);
        }
    }
    return core::Result<SelectionKind>::failure(validation(
        "workspace selection kind is unknown"));
}

core::Result<WorkspaceDomain> domain_from(std::string_view name) {
    for (std::uint8_t raw = 0U;
         raw <= static_cast<std::uint8_t>(WorkspaceDomain::review); ++raw) {
        const auto domain = static_cast<WorkspaceDomain>(raw);
        if (workspace_domain_name(domain) == name) {
            return core::Result<WorkspaceDomain>::success(domain);
        }
    }
    return core::Result<WorkspaceDomain>::failure(validation(
        "workspace domain is unknown"));
}

} // namespace

std::string_view workspace_domain_name(WorkspaceDomain domain) noexcept {
    switch (domain) {
    case WorkspaceDomain::rig: return "rig";
    case WorkspaceDomain::skin: return "skin";
    case WorkspaceDomain::animate: return "animate";
    case WorkspaceDomain::face: return "face";
    case WorkspaceDomain::sim: return "sim";
    case WorkspaceDomain::review: return "review";
    }
    return "unknown";
}

std::string_view selection_kind_name(SelectionKind kind) noexcept {
    switch (kind) {
    case SelectionKind::joint: return "joint";
    case SelectionKind::control: return "control";
    case SelectionKind::vertex: return "vertex";
    case SelectionKind::clip: return "clip";
    case SelectionKind::morph_target: return "morph-target";
    case SelectionKind::garment: return "garment";
    case SelectionKind::groom: return "groom";
    }
    return "unknown";
}

core::Result<void> WorkspaceTool::validate() const {
    // Dots and dashes are tool-token separators; the printable-ASCII rule is
    // the hostile-input gate.
    if (!safe_token(token, kMaxTokenBytes, true)) {
        return core::Result<void>::failure(invalid(
            "workspace tool token is empty-unsafe, oversized, or contains control bytes"));
    }
    return core::Result<void>::success();
}

core::Result<void> SelectionEntry::validate() const {
    if (identity == 0U) {
        return core::Result<void>::failure(invalid(
            "workspace selection entry carries the invalid zero identity"));
    }
    return core::Result<void>::success();
}

core::Result<void> DomainViewState::set(std::string key, std::string value) {
    if (!safe_token(key, kMaxKeyBytes, false) ||
        !safe_token(value, kMaxValueBytes, true)) {
        return core::Result<void>::failure(invalid(
            "workspace view-state key or value is out of bounds"));
    }
    if (entries_.size() >= kMaxEntries && entries_.find(key) == entries_.end()) {
        return core::Result<void>::failure(validation(
            "workspace view state has reached its bounded capacity"));
    }
    entries_[std::move(key)] = std::move(value);
    return core::Result<void>::success();
}

const std::string* DomainViewState::find(std::string_view key) const noexcept {
    const auto found = entries_.find(std::string(key));
    return found == entries_.end() ? nullptr : &found->second;
}

core::Result<void> DomainViewState::validate() const {
    if (entries_.size() > kMaxEntries) {
        return core::Result<void>::failure(validation(
            "workspace view state exceeds its bounded capacity"));
    }
    for (const auto& [key, value] : entries_) {
        if (!safe_token(key, kMaxKeyBytes, false) ||
            !safe_token(value, kMaxValueBytes, true)) {
            return core::Result<void>::failure(invalid(
                "workspace view-state key or value is out of bounds"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> WorkspaceSession::validate() const {
    if (auto result = active_tool.validate(); !result) return result;
    if (selection.size() > kMaxSelectionEntries) {
        return core::Result<void>::failure(validation(
            "workspace selection exceeds its bounded capacity"));
    }
    for (const SelectionEntry& entry : selection) {
        if (auto result = entry.validate(); !result) return result;
    }
    if (!std::is_sorted(selection.begin(), selection.end()) ||
        std::adjacent_find(selection.begin(), selection.end()) != selection.end()) {
        return core::Result<void>::failure(validation(
            "workspace selection is not sorted and unique"));
    }
    return view_state.validate();
}

std::string WorkspaceSession::serialize() const {
    codec::ObjectWriter writer;
    writer.add("domain", workspace_domain_name(domain));
    writer.add("target_character", target_character.value);
    writer.add("active_tool", active_tool.token);
    std::string selection_json = "[";
    bool first = true;
    for (const SelectionEntry& entry : selection) {
        if (!first) selection_json.push_back(',');
        first = false;
        codec::ObjectWriter entry_writer;
        entry_writer.add("kind", selection_kind_name(entry.kind));
        entry_writer.add("identity", entry.identity);
        selection_json += std::move(entry_writer).finish();
    }
    selection_json.push_back(']');
    writer.add_raw("selection", selection_json);
    std::string view_json = "{";
    first = true;
    for (const auto& [key, value] : view_state.entries()) {
        if (!first) view_json.push_back(',');
        first = false;
        view_json += codec::quote(key);
        view_json.push_back(':');
        view_json += codec::quote(value);
    }
    view_json.push_back('}');
    writer.add_raw("view_state", view_json);
    return std::move(writer).finish();
}

core::Result<WorkspaceSession> WorkspaceSession::deserialize(std::string_view text) {
    const auto members = core::json::parse_object(text);
    if (!members) {
        return core::Result<WorkspaceSession>::failure(members.error());
    }
    WorkspaceSession session;
    const auto domain = codec::string_member(members.value(), "domain");
    if (!domain) return core::Result<WorkspaceSession>::failure(domain.error());
    const auto parsed_domain = domain_from(domain.value());
    if (!parsed_domain) {
        return core::Result<WorkspaceSession>::failure(parsed_domain.error());
    }
    session.domain = parsed_domain.value();
    const auto target = codec::uint_member(members.value(), "target_character");
    if (!target) return core::Result<WorkspaceSession>::failure(target.error());
    session.target_character = character::CharacterId{target.value()};
    const auto tool = codec::optional_string_member(members.value(), "active_tool");
    if (!tool) return core::Result<WorkspaceSession>::failure(tool.error());
    session.active_tool.token = tool.value();
    const auto selection = codec::array_member(members.value(), "selection");
    if (!selection) return core::Result<WorkspaceSession>::failure(selection.error());
    if (selection.value().size() > kMaxSelectionEntries) {
        return core::Result<WorkspaceSession>::failure(codec::malformed(
            "workspace selection exceeds its bounded capacity"));
    }
    for (const std::string_view raw_entry : selection.value()) {
        const auto entry_members = core::json::parse_object(raw_entry);
        if (!entry_members) {
            return core::Result<WorkspaceSession>::failure(entry_members.error());
        }
        const auto kind = codec::string_member(entry_members.value(), "kind");
        if (!kind) return core::Result<WorkspaceSession>::failure(kind.error());
        const auto parsed_kind = selection_kind_from(kind.value());
        if (!parsed_kind) {
            return core::Result<WorkspaceSession>::failure(parsed_kind.error());
        }
        const auto identity = codec::uint_member(entry_members.value(), "identity");
        if (!identity) return core::Result<WorkspaceSession>::failure(identity.error());
        session.selection.push_back(
            SelectionEntry{parsed_kind.value(), identity.value()});
    }
    const auto view = codec::object_member(members.value(), "view_state");
    if (!view) return core::Result<WorkspaceSession>::failure(view.error());
    for (const core::json::Member& member : view.value()) {
        const auto value = core::json::decode_string(member.raw_value);
        if (!value) return core::Result<WorkspaceSession>::failure(value.error());
        if (auto result = session.view_state.set(member.key, value.value());
            !result) {
            return core::Result<WorkspaceSession>::failure(result.error());
        }
    }
    if (auto result = session.validate(); !result) {
        return core::Result<WorkspaceSession>::failure(result.error());
    }
    return core::Result<WorkspaceSession>::success(std::move(session));
}

} // namespace carto::workspace
