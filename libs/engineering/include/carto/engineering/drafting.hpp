#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::engineering {

enum class DraftingUnit : std::uint8_t {
    millimeter = 0U,
    centimeter = 1U,
    meter = 2U,
    inch = 3U,
    foot = 4U,
    degree = 5U,
    radian = 6U,
};

enum class DraftingDimensionKind : std::uint8_t {
    distance = 0U,
    angle = 1U,
    radius = 2U,
    diameter = 3U,
};

enum class DraftingGuideKind : std::uint8_t {
    line = 0U,
    datum_plane = 1U,
};

struct DraftingSourceBinding final {
    std::string identity;
    core::Revision revision;
    assets::Sha256Digest digest;
    DraftingUnit coordinate_unit = DraftingUnit::millimeter;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const DraftingSourceBinding&) const noexcept = default;
};

struct DraftingDimension final {
    std::string id;
    DraftingDimensionKind kind = DraftingDimensionKind::distance;
    std::string anchor_a;
    std::string anchor_b;
    std::string anchor_c;
    double target = 0.0;
    double tolerance_plus = 0.0;
    double tolerance_minus = 0.0;
    DraftingUnit unit = DraftingUnit::millimeter;
    std::string label;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const DraftingDimension&) const noexcept = default;
};

struct DraftingGuide final {
    std::string id;
    DraftingGuideKind kind = DraftingGuideKind::line;
    std::vector<std::string> anchors;
    double offset = 0.0;
    DraftingUnit unit = DraftingUnit::millimeter;
    std::string label;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const DraftingGuide&) const noexcept = default;
};

struct DraftingEditReceipt final {
    core::Revision revision_before;
    core::Revision revision_after;
    std::string operation;
};

[[nodiscard]] core::Result<double> convert_drafting_unit(
    double value,
    DraftingUnit from,
    DraftingUnit to);

// Source-linked dimensional constraints and datum guides. This document is a
// bounded authoring value object; it does not measure source geometry or
// publish a manufacturing decision. Callers must resolve anchors against the
// exact bound source and retain their normal human-review boundary.
class DraftingDocument final {
public:
    static constexpr std::size_t kMaxSerializedBytes = 8U * 1024U * 1024U;
    static constexpr std::size_t kMaxRecords = 128U;
    static constexpr std::size_t kMaxHistoryEntries = 64U;

    [[nodiscard]] static core::Result<DraftingDocument> create(
        DraftingSourceBinding source);
    [[nodiscard]] core::Result<void> validate() const;

    [[nodiscard]] const DraftingSourceBinding& source() const noexcept { return source_; }
    [[nodiscard]] core::Revision revision() const noexcept { return revision_; }
    [[nodiscard]] const std::vector<DraftingDimension>& dimensions() const noexcept {
        return state_.dimensions;
    }
    [[nodiscard]] const std::vector<DraftingGuide>& guides() const noexcept {
        return state_.guides;
    }
    [[nodiscard]] std::size_t history_cursor() const noexcept { return history_cursor_; }
    [[nodiscard]] std::size_t history_size() const noexcept { return history_.size(); }

    [[nodiscard]] core::Result<DraftingEditReceipt> set_dimension(
        const DraftingSourceBinding& current_source,
        core::Revision expected_revision,
        DraftingDimension dimension,
        std::string justification);
    [[nodiscard]] core::Result<DraftingEditReceipt> remove_dimension(
        const DraftingSourceBinding& current_source,
        core::Revision expected_revision,
        std::string_view id,
        std::string justification);
    [[nodiscard]] core::Result<DraftingEditReceipt> set_guide(
        const DraftingSourceBinding& current_source,
        core::Revision expected_revision,
        DraftingGuide guide,
        std::string justification);
    [[nodiscard]] core::Result<DraftingEditReceipt> remove_guide(
        const DraftingSourceBinding& current_source,
        core::Revision expected_revision,
        std::string_view id,
        std::string justification);
    [[nodiscard]] core::Result<DraftingEditReceipt> undo(
        const DraftingSourceBinding& current_source,
        core::Revision expected_revision);
    [[nodiscard]] core::Result<DraftingEditReceipt> redo(
        const DraftingSourceBinding& current_source,
        core::Revision expected_revision);

    [[nodiscard]] core::Result<std::string> serialize() const;
    [[nodiscard]] static core::Result<DraftingDocument> deserialize(std::string_view text);

private:
    struct State final {
        std::vector<DraftingDimension> dimensions;
        std::vector<DraftingGuide> guides;
        [[nodiscard]] core::Result<void> validate() const;
    };

    struct HistoryEntry final {
        State state;
        std::string justification;
    };

    [[nodiscard]] core::Result<void> validate_edit(
        const DraftingSourceBinding& current_source,
        core::Revision expected_revision,
        std::string_view justification) const;
    [[nodiscard]] core::Result<DraftingEditReceipt> commit(
        State next_state,
        std::string justification,
        std::string operation);

    DraftingSourceBinding source_;
    core::Revision revision_;
    State state_;
    std::vector<HistoryEntry> history_;
    std::size_t history_cursor_ = 0U;
};

} // namespace carto::engineering
