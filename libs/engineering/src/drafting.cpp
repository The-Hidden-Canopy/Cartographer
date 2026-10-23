#include <carto/engineering/drafting.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace carto::engineering {
namespace {

constexpr std::string_view kMagic = "CARTO_DRAFTING_DOCUMENT_V1";
constexpr std::size_t kMaxIdentityBytes = 128U;
constexpr std::size_t kMaxLabelBytes = 256U;
constexpr std::size_t kMaxJustificationBytes = 512U;
constexpr double kMaxMagnitude = 1.0e9;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_identity(std::string_view value) noexcept {
    return !value.empty() && value.size() <= kMaxIdentityBytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x21U && byte <= 0x7eU;
        });
}

bool valid_unit(DraftingUnit unit) noexcept {
    return unit >= DraftingUnit::millimeter && unit <= DraftingUnit::radian;
}

bool valid_dimension_kind(DraftingDimensionKind kind) noexcept {
    return kind >= DraftingDimensionKind::distance &&
        kind <= DraftingDimensionKind::diameter;
}

bool valid_guide_kind(DraftingGuideKind kind) noexcept {
    return kind >= DraftingGuideKind::line && kind <= DraftingGuideKind::datum_plane;
}

bool is_angle_unit(DraftingUnit unit) noexcept {
    return unit == DraftingUnit::degree || unit == DraftingUnit::radian;
}

bool dimension_uses_angle(DraftingDimensionKind kind) noexcept {
    return kind == DraftingDimensionKind::angle;
}

bool finite_bounded(double value) noexcept {
    return std::isfinite(value) && std::abs(value) <= kMaxMagnitude;
}

bool valid_label(std::string_view value) noexcept {
    return value.size() <= kMaxLabelBytes &&
        std::none_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte < 0x20U && byte != '\t';
        });
}

assets::Sha256Digest digest_text(std::string_view text) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
    return assets::sha256(std::span<const std::uint8_t>(bytes, text.size()));
}

} // namespace

core::Result<double> convert_drafting_unit(
    double value,
    DraftingUnit from,
    DraftingUnit to) {
    if (!finite_bounded(value) || !valid_unit(from) || !valid_unit(to)) {
        return core::Result<double>::failure(invalid(
            "drafting unit conversion has an invalid value or unit"));
    }
    if (is_angle_unit(from) != is_angle_unit(to)) {
        return core::Result<double>::failure(validation(
            "drafting unit conversion cannot mix length and angle units"));
    }

    constexpr double pi = 3.141592653589793238462643383279502884;
    const auto scale = [](DraftingUnit unit) -> double {
        switch (unit) {
        case DraftingUnit::millimeter: return 0.001;
        case DraftingUnit::centimeter: return 0.01;
        case DraftingUnit::meter: return 1.0;
        case DraftingUnit::inch: return 0.0254;
        case DraftingUnit::foot: return 0.3048;
        case DraftingUnit::degree: return pi / 180.0;
        case DraftingUnit::radian: return 1.0;
        }
        return 0.0;
    };
    const double converted = value * scale(from) / scale(to);
    if (!finite_bounded(converted)) {
        return core::Result<double>::failure(invalid(
            "drafting unit conversion exceeds the supported range"));
    }
    return core::Result<double>::success(converted);
}

core::Result<void> DraftingSourceBinding::validate() const {
    if (!valid_identity(identity) || digest.is_zero() || !valid_unit(coordinate_unit) ||
        is_angle_unit(coordinate_unit)) {
        return core::Result<void>::failure(invalid(
            "drafting source binding requires an identity, digest, revision, and length unit"));
    }
    return core::Result<void>::success();
}

core::Result<void> DraftingDimension::validate() const {
    if (!valid_identity(id) || !valid_dimension_kind(kind) ||
        !valid_identity(anchor_a) || !valid_identity(anchor_b) ||
        anchor_a == anchor_b || !valid_unit(unit) || !valid_label(label) ||
        !finite_bounded(target) || target <= 0.0 ||
        !finite_bounded(tolerance_plus) || tolerance_plus < 0.0 ||
        !finite_bounded(tolerance_minus) || tolerance_minus < 0.0) {
        return core::Result<void>::failure(invalid(
            "drafting dimension fields are invalid or outside their bounds"));
    }
    if (is_angle_unit(unit) != dimension_uses_angle(kind)) {
        return core::Result<void>::failure(validation(
            "drafting dimension unit does not match its measurement kind"));
    }
    if (kind == DraftingDimensionKind::angle) {
        if (!valid_identity(anchor_c) || anchor_c == anchor_a || anchor_c == anchor_b) {
            return core::Result<void>::failure(invalid(
                "angle dimension requires three distinct source anchors"));
        }
    } else if (!anchor_c.empty()) {
        return core::Result<void>::failure(invalid(
            "length dimension has an unexpected third source anchor"));
    }
    return core::Result<void>::success();
}

core::Result<void> DraftingGuide::validate() const {
    if (!valid_identity(id) || !valid_guide_kind(kind) || !valid_unit(unit) ||
        is_angle_unit(unit) || !finite_bounded(offset) || !valid_label(label) ||
        anchors.size() != (kind == DraftingGuideKind::line ? 2U : 3U)) {
        return core::Result<void>::failure(invalid(
            "drafting guide fields are invalid or outside their bounds"));
    }
    std::set<std::string> distinct;
    for (const auto& anchor : anchors) {
        if (!valid_identity(anchor) || !distinct.insert(anchor).second) {
            return core::Result<void>::failure(invalid(
                "drafting guide requires distinct stable source anchors"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> DraftingDocument::State::validate() const {
    if (dimensions.size() > DraftingDocument::kMaxRecords ||
        guides.size() > DraftingDocument::kMaxRecords) {
        return core::Result<void>::failure(invalid(
            "drafting record count exceeds the document limit"));
    }
    std::set<std::string> ids;
    for (const auto& dimension : dimensions) {
        if (auto checked = dimension.validate(); !checked) return checked;
        if (!ids.insert(dimension.id).second) {
            return core::Result<void>::failure(validation(
                "drafting document contains duplicate record IDs"));
        }
    }
    for (const auto& guide : guides) {
        if (auto checked = guide.validate(); !checked) return checked;
        if (!ids.insert(guide.id).second) {
            return core::Result<void>::failure(validation(
                "drafting document contains duplicate record IDs"));
        }
    }
    return core::Result<void>::success();
}

core::Result<DraftingDocument> DraftingDocument::create(DraftingSourceBinding source) {
    if (auto checked = source.validate(); !checked) {
        return core::Result<DraftingDocument>::failure(checked.error());
    }
    DraftingDocument document;
    document.source_ = std::move(source);
    document.history_.push_back(HistoryEntry{document.state_, "initial"});
    return core::Result<DraftingDocument>::success(std::move(document));
}

core::Result<void> DraftingDocument::validate() const {
    if (auto checked = source_.validate(); !checked) return checked;
    if (history_.empty() || history_.size() > kMaxHistoryEntries ||
        history_cursor_ >= history_.size()) {
        return core::Result<void>::failure(validation(
            "drafting history cursor or size is invalid"));
    }
    if (auto checked = state_.validate(); !checked) return checked;
    if (!(state_.dimensions == history_[history_cursor_].state.dimensions) ||
        !(state_.guides == history_[history_cursor_].state.guides)) {
        return core::Result<void>::failure(validation(
            "drafting current state does not match its history cursor"));
    }
    for (const auto& entry : history_) {
        if (entry.justification.empty() ||
            entry.justification.size() > kMaxJustificationBytes ||
            !valid_label(entry.justification)) {
            return core::Result<void>::failure(validation(
                "drafting history justification is invalid"));
        }
        if (auto checked = entry.state.validate(); !checked) return checked;
    }
    if (revision_.value() < history_cursor_) {
        return core::Result<void>::failure(validation(
            "drafting revision precedes its history cursor"));
    }
    return core::Result<void>::success();
}

core::Result<void> DraftingDocument::validate_edit(
    const DraftingSourceBinding& current_source,
    core::Revision expected_revision,
    std::string_view justification) const {
    if (auto checked = current_source.validate(); !checked) return checked;
    if (current_source != source_) {
        return core::Result<void>::failure(stale(
            "drafting source identity, revision, digest, or units changed"));
    }
    if (expected_revision != revision_) {
        return core::Result<void>::failure(stale(
            "drafting document revision changed before the edit"));
    }
    if (revision_.exhausted()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "drafting document revision is exhausted"));
    }
    if (justification.empty() || justification.size() > kMaxJustificationBytes ||
        !valid_label(justification)) {
        return core::Result<void>::failure(invalid(
            "drafting edit requires a bounded, non-empty justification"));
    }
    return core::Result<void>::success();
}

core::Result<DraftingEditReceipt> DraftingDocument::commit(
    State next_state,
    std::string justification,
    std::string operation) {
    if (auto checked = next_state.validate(); !checked) {
        return core::Result<DraftingEditReceipt>::failure(checked.error());
    }
    if (history_.size() >= kMaxHistoryEntries && history_cursor_ + 1U >= history_.size()) {
        return core::Result<DraftingEditReceipt>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "drafting history limit is reached"));
    }
    const auto next_revision = revision_.next();
    if (next_revision == revision_) {
        return core::Result<DraftingEditReceipt>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "drafting document revision is exhausted"));
    }

    // Allocate/copy before truncating redo history so allocation failure leaves
    // the authored state and branch intact.
    State current_state = next_state;
    HistoryEntry entry{std::move(next_state), std::move(justification)};
    history_.reserve(history_cursor_ + 2U);
    if (history_cursor_ + 1U < history_.size()) {
        history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(history_cursor_ + 1U),
            history_.end());
    }
    history_.push_back(std::move(entry));
    const auto before = revision_;
    revision_ = next_revision;
    ++history_cursor_;
    state_ = std::move(current_state);
    return core::Result<DraftingEditReceipt>::success(
        DraftingEditReceipt{before, revision_, std::move(operation)});
}

core::Result<DraftingEditReceipt> DraftingDocument::set_dimension(
    const DraftingSourceBinding& current_source,
    core::Revision expected_revision,
    DraftingDimension dimension,
    std::string justification) {
    if (auto checked = validate_edit(current_source, expected_revision, justification); !checked) {
        return core::Result<DraftingEditReceipt>::failure(checked.error());
    }
    if (auto checked = dimension.validate(); !checked) {
        return core::Result<DraftingEditReceipt>::failure(checked.error());
    }
    State next = state_;
    const auto existing = std::find_if(next.dimensions.begin(), next.dimensions.end(),
        [&dimension](const DraftingDimension& item) { return item.id == dimension.id; });
    const auto conflicting_guide = std::find_if(next.guides.begin(), next.guides.end(),
        [&dimension](const DraftingGuide& item) { return item.id == dimension.id; });
    if (conflicting_guide != next.guides.end()) {
        return core::Result<DraftingEditReceipt>::failure(validation(
            "drafting record ID is already used by a guide"));
    }
    if (existing == next.dimensions.end()) {
        next.dimensions.push_back(std::move(dimension));
    } else {
        *existing = std::move(dimension);
    }
    return commit(std::move(next), std::move(justification), "set_dimension");
}

core::Result<DraftingEditReceipt> DraftingDocument::remove_dimension(
    const DraftingSourceBinding& current_source,
    core::Revision expected_revision,
    std::string_view id,
    std::string justification) {
    if (auto checked = validate_edit(current_source, expected_revision, justification); !checked) {
        return core::Result<DraftingEditReceipt>::failure(checked.error());
    }
    State next = state_;
    const auto existing = std::find_if(next.dimensions.begin(), next.dimensions.end(),
        [id](const DraftingDimension& item) { return item.id == id; });
    if (existing == next.dimensions.end()) {
        return core::Result<DraftingEditReceipt>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "drafting dimension was not found"));
    }
    next.dimensions.erase(existing);
    return commit(std::move(next), std::move(justification), "remove_dimension");
}

core::Result<DraftingEditReceipt> DraftingDocument::set_guide(
    const DraftingSourceBinding& current_source,
    core::Revision expected_revision,
    DraftingGuide guide,
    std::string justification) {
    if (auto checked = validate_edit(current_source, expected_revision, justification); !checked) {
        return core::Result<DraftingEditReceipt>::failure(checked.error());
    }
    if (auto checked = guide.validate(); !checked) {
        return core::Result<DraftingEditReceipt>::failure(checked.error());
    }
    State next = state_;
    const auto existing = std::find_if(next.guides.begin(), next.guides.end(),
        [&guide](const DraftingGuide& item) { return item.id == guide.id; });
    const auto conflicting_dimension = std::find_if(next.dimensions.begin(), next.dimensions.end(),
        [&guide](const DraftingDimension& item) { return item.id == guide.id; });
    if (conflicting_dimension != next.dimensions.end()) {
        return core::Result<DraftingEditReceipt>::failure(validation(
            "drafting record ID is already used by a dimension"));
    }
    if (existing == next.guides.end()) {
        next.guides.push_back(std::move(guide));
    } else {
        *existing = std::move(guide);
    }
    return commit(std::move(next), std::move(justification), "set_guide");
}

core::Result<DraftingEditReceipt> DraftingDocument::remove_guide(
    const DraftingSourceBinding& current_source,
    core::Revision expected_revision,
    std::string_view id,
    std::string justification) {
    if (auto checked = validate_edit(current_source, expected_revision, justification); !checked) {
        return core::Result<DraftingEditReceipt>::failure(checked.error());
    }
    State next = state_;
    const auto existing = std::find_if(next.guides.begin(), next.guides.end(),
        [id](const DraftingGuide& item) { return item.id == id; });
    if (existing == next.guides.end()) {
        return core::Result<DraftingEditReceipt>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "drafting guide was not found"));
    }
    next.guides.erase(existing);
    return commit(std::move(next), std::move(justification), "remove_guide");
}

core::Result<DraftingEditReceipt> DraftingDocument::undo(
    const DraftingSourceBinding& current_source,
    core::Revision expected_revision) {
    if (auto checked = validate_edit(current_source, expected_revision, "undo"); !checked) {
        return core::Result<DraftingEditReceipt>::failure(checked.error());
    }
    if (history_cursor_ == 0U) {
        return core::Result<DraftingEditReceipt>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "drafting history has no earlier state"));
    }
    State previous = history_[history_cursor_ - 1U].state;
    const auto before = revision_;
    --history_cursor_;
    state_ = std::move(previous);
    revision_ = revision_.next();
    return core::Result<DraftingEditReceipt>::success(
        DraftingEditReceipt{before, revision_, "undo"});
}

core::Result<DraftingEditReceipt> DraftingDocument::redo(
    const DraftingSourceBinding& current_source,
    core::Revision expected_revision) {
    if (auto checked = validate_edit(current_source, expected_revision, "redo"); !checked) {
        return core::Result<DraftingEditReceipt>::failure(checked.error());
    }
    if (history_cursor_ + 1U >= history_.size()) {
        return core::Result<DraftingEditReceipt>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "drafting history has no later state"));
    }
    State next = history_[history_cursor_ + 1U].state;
    const auto before = revision_;
    ++history_cursor_;
    state_ = std::move(next);
    revision_ = revision_.next();
    return core::Result<DraftingEditReceipt>::success(
        DraftingEditReceipt{before, revision_, "redo"});
}

core::Result<std::string> DraftingDocument::serialize() const {
    if (auto checked = validate(); !checked) {
        return core::Result<std::string>::failure(checked.error());
    }
    std::ostringstream output;
    output << kMagic << '\n'
        << "SOURCE " << std::quoted(source_.identity) << ' '
        << source_.revision.value() << ' ' << source_.digest.hex() << ' '
        << static_cast<unsigned>(source_.coordinate_unit) << '\n'
        << "DOCUMENT " << revision_.value() << ' ' << history_cursor_ << ' '
        << history_.size() << '\n'
        << std::setprecision(std::numeric_limits<double>::max_digits10);
    for (const auto& entry : history_) {
        output << "ENTRY " << std::quoted(entry.justification) << ' '
            << entry.state.dimensions.size() << ' ' << entry.state.guides.size() << '\n';
        for (const auto& item : entry.state.dimensions) {
            output << "DIM " << std::quoted(item.id) << ' '
                << static_cast<unsigned>(item.kind) << ' '
                << std::quoted(item.anchor_a) << ' ' << std::quoted(item.anchor_b) << ' '
                << std::quoted(item.anchor_c) << ' ' << item.target << ' '
                << item.tolerance_plus << ' ' << item.tolerance_minus << ' '
                << static_cast<unsigned>(item.unit) << ' ' << std::quoted(item.label) << '\n';
        }
        for (const auto& item : entry.state.guides) {
            output << "GUIDE " << std::quoted(item.id) << ' '
                << static_cast<unsigned>(item.kind) << ' ' << item.anchors.size();
            for (const auto& anchor : item.anchors) output << ' ' << std::quoted(anchor);
            output << ' ' << item.offset << ' ' << static_cast<unsigned>(item.unit)
                << ' ' << std::quoted(item.label) << '\n';
        }
    }
    const auto body = output.str();
    if (body.size() + 74U > kMaxSerializedBytes) {
        return core::Result<std::string>::failure(invalid(
            "drafting document exceeds the serialization byte limit"));
    }
    const auto checksum = digest_text(body);
    return core::Result<std::string>::success(
        body + "CHECKSUM " + checksum.hex() + "\n");
}

core::Result<DraftingDocument> DraftingDocument::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<DraftingDocument>::failure(invalid(
            "drafting document is empty or exceeds the byte limit"));
    }
    const auto checksum_start = text.rfind("CHECKSUM ");
    if (checksum_start == std::string_view::npos ||
        (checksum_start != 0U && text[checksum_start - 1U] != '\n')) {
        return core::Result<DraftingDocument>::failure(validation(
            "drafting document checksum record is missing"));
    }
    const auto body = text.substr(0U, checksum_start);
    const auto checksum_line = text.substr(checksum_start);
    std::istringstream checksum_input{std::string(checksum_line)};
    std::string checksum_tag;
    std::string checksum_hex;
    if (!(checksum_input >> checksum_tag >> checksum_hex) || checksum_tag != "CHECKSUM") {
        return core::Result<DraftingDocument>::failure(validation(
            "drafting document checksum record is invalid"));
    }
    checksum_input >> std::ws;
    if (!checksum_input.eof()) {
        return core::Result<DraftingDocument>::failure(validation(
            "drafting document has trailing checksum data"));
    }
    const auto stored_checksum = assets::Sha256Digest::from_hex(checksum_hex);
    if (!stored_checksum || stored_checksum.value() != digest_text(body)) {
        return core::Result<DraftingDocument>::failure(validation(
            "drafting document checksum does not match its content"));
    }

    std::istringstream input{std::string(body)};
    std::string magic;
    if (!std::getline(input, magic) || magic != kMagic) {
        return core::Result<DraftingDocument>::failure(validation(
            "drafting document version is unsupported"));
    }
    std::string line;
    if (!std::getline(input, line)) {
        return core::Result<DraftingDocument>::failure(validation(
            "drafting source binding is missing"));
    }
    std::istringstream source_line{line};
    std::string tag;
    DraftingSourceBinding source;
    std::uint64_t source_revision = 0U;
    std::string digest_hex;
    unsigned source_unit = 0U;
    if (!(source_line >> tag >> std::quoted(source.identity) >> source_revision >>
            digest_hex >> source_unit) || tag != "SOURCE" || source_unit > 255U) {
        return core::Result<DraftingDocument>::failure(validation(
            "drafting source binding record is malformed"));
    }
    source_line >> std::ws;
    if (!source_line.eof()) {
        return core::Result<DraftingDocument>::failure(validation(
            "drafting source binding has trailing data"));
    }
    const auto source_digest = assets::Sha256Digest::from_hex(digest_hex);
    if (!source_digest) {
        return core::Result<DraftingDocument>::failure(source_digest.error());
    }
    source.revision = core::Revision(source_revision);
    source.digest = source_digest.value();
    source.coordinate_unit = static_cast<DraftingUnit>(source_unit);
    if (auto checked = source.validate(); !checked) {
        return core::Result<DraftingDocument>::failure(checked.error());
    }
    auto created = create(std::move(source));
    if (!created) return created;
    auto document = std::move(created).value();

    if (!std::getline(input, line)) {
        return core::Result<DraftingDocument>::failure(validation(
            "drafting document state header is missing"));
    }
    std::istringstream state_line{line};
    std::uint64_t document_revision = 0U;
    std::size_t cursor = 0U;
    std::size_t history_count = 0U;
    if (!(state_line >> tag >> document_revision >> cursor >> history_count) ||
        tag != "DOCUMENT" || history_count == 0U ||
        history_count > kMaxHistoryEntries || cursor >= history_count) {
        return core::Result<DraftingDocument>::failure(validation(
            "drafting document state header is invalid"));
    }
    state_line >> std::ws;
    if (!state_line.eof()) {
        return core::Result<DraftingDocument>::failure(validation(
            "drafting document state header has trailing data"));
    }

    std::vector<HistoryEntry> history;
    history.reserve(history_count);
    for (std::size_t index = 0U; index < history_count; ++index) {
        if (!std::getline(input, line)) {
            return core::Result<DraftingDocument>::failure(validation(
                "drafting history entry is truncated"));
        }
        std::istringstream entry_line{line};
        HistoryEntry entry;
        std::size_t dimension_count = 0U;
        std::size_t guide_count = 0U;
        if (!(entry_line >> tag >> std::quoted(entry.justification) >>
                dimension_count >> guide_count) || tag != "ENTRY" ||
            dimension_count > kMaxRecords || guide_count > kMaxRecords) {
            return core::Result<DraftingDocument>::failure(validation(
                "drafting history entry header is invalid"));
        }
        entry_line >> std::ws;
        if (!entry_line.eof()) {
            return core::Result<DraftingDocument>::failure(validation(
                "drafting history entry header has trailing data"));
        }
        entry.state.dimensions.reserve(dimension_count);
        entry.state.guides.reserve(guide_count);
        for (std::size_t record = 0U; record < dimension_count; ++record) {
            if (!std::getline(input, line)) {
                return core::Result<DraftingDocument>::failure(validation(
                    "drafting dimension record is truncated"));
            }
            std::istringstream record_line{line};
            DraftingDimension item;
            unsigned kind = 0U;
            unsigned unit = 0U;
            if (!(record_line >> tag >> std::quoted(item.id) >> kind >>
                    std::quoted(item.anchor_a) >> std::quoted(item.anchor_b) >>
                    std::quoted(item.anchor_c) >> item.target >> item.tolerance_plus >>
                    item.tolerance_minus >> unit >> std::quoted(item.label)) ||
                tag != "DIM" || kind > 255U || unit > 255U) {
                return core::Result<DraftingDocument>::failure(validation(
                    "drafting dimension record is malformed"));
            }
            record_line >> std::ws;
            if (!record_line.eof()) {
                return core::Result<DraftingDocument>::failure(validation(
                    "drafting dimension record has trailing data"));
            }
            item.kind = static_cast<DraftingDimensionKind>(kind);
            item.unit = static_cast<DraftingUnit>(unit);
            if (auto checked = item.validate(); !checked) {
                return core::Result<DraftingDocument>::failure(checked.error());
            }
            entry.state.dimensions.push_back(std::move(item));
        }
        for (std::size_t record = 0U; record < guide_count; ++record) {
            if (!std::getline(input, line)) {
                return core::Result<DraftingDocument>::failure(validation(
                    "drafting guide record is truncated"));
            }
            std::istringstream record_line{line};
            DraftingGuide item;
            unsigned kind = 0U;
            unsigned unit = 0U;
            std::size_t anchor_count = 0U;
            if (!(record_line >> tag >> std::quoted(item.id) >> kind >> anchor_count) ||
                tag != "GUIDE" || kind > 255U || anchor_count > 3U) {
                return core::Result<DraftingDocument>::failure(validation(
                    "drafting guide record header is malformed"));
            }
            item.anchors.resize(anchor_count);
            for (auto& anchor : item.anchors) {
                if (!(record_line >> std::quoted(anchor))) {
                    return core::Result<DraftingDocument>::failure(validation(
                        "drafting guide anchor list is truncated"));
                }
            }
            if (!(record_line >> item.offset >> unit >> std::quoted(item.label)) || unit > 255U) {
                return core::Result<DraftingDocument>::failure(validation(
                    "drafting guide record is malformed"));
            }
            record_line >> std::ws;
            if (!record_line.eof()) {
                return core::Result<DraftingDocument>::failure(validation(
                    "drafting guide record has trailing data"));
            }
            item.kind = static_cast<DraftingGuideKind>(kind);
            item.unit = static_cast<DraftingUnit>(unit);
            if (auto checked = item.validate(); !checked) {
                return core::Result<DraftingDocument>::failure(checked.error());
            }
            entry.state.guides.push_back(std::move(item));
        }
        if (auto checked = entry.state.validate(); !checked) {
            return core::Result<DraftingDocument>::failure(checked.error());
        }
        if (entry.justification.empty() || entry.justification.size() > kMaxJustificationBytes ||
            !valid_label(entry.justification)) {
            return core::Result<DraftingDocument>::failure(validation(
                "drafting history justification is invalid"));
        }
        history.push_back(std::move(entry));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<DraftingDocument>::failure(validation(
            "drafting document body has trailing records"));
    }
    document.history_ = std::move(history);
    document.history_cursor_ = cursor;
    document.revision_ = core::Revision(document_revision);
    document.state_ = document.history_[cursor].state;
    if (auto checked = document.validate(); !checked) {
        return core::Result<DraftingDocument>::failure(checked.error());
    }
    return core::Result<DraftingDocument>::success(std::move(document));
}

} // namespace carto::engineering
