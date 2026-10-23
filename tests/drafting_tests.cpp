#include <carto/engineering/drafting.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace carto;
using namespace carto::engineering;

template <typename Condition>
void require(const Condition& condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

DraftingSourceBinding source_binding() {
    assets::Sha256Digest digest;
    digest.bytes[0U] = 0x42U;
    return DraftingSourceBinding{
        "drawing.source.17", core::Revision(12U), digest, DraftingUnit::millimeter};
}

DraftingDimension distance(std::string id = "dim.length") {
    return DraftingDimension{
        std::move(id), DraftingDimensionKind::distance, "edge.start", "edge.end", "",
        25.4, 0.1, 0.1, DraftingUnit::millimeter, "Overall length"};
}

DraftingGuide line_guide() {
    return DraftingGuide{
        "guide.center", DraftingGuideKind::line,
        {"datum.left", "datum.right"}, 0.0, DraftingUnit::millimeter,
        "Centerline"};
}

void dimensional_units_and_typed_constraints_validate() {
    const auto inches = convert_drafting_unit(
        25.4, DraftingUnit::millimeter, DraftingUnit::inch);
    require(inches && inches.value() == 1.0,
        "length conversion must use exact inch definition");
    const auto degrees = convert_drafting_unit(
        3.14159265358979323846, DraftingUnit::radian, DraftingUnit::degree);
    require(degrees && std::abs(degrees.value() - 180.0) < 1.0e-12,
        "angle conversion must use radians and degrees consistently");
    require(!convert_drafting_unit(1.0, DraftingUnit::millimeter, DraftingUnit::degree),
        "length and angle units must not mix");

    auto linear = distance();
    require(static_cast<bool>(linear.validate()),
        "valid linear dimension should be accepted");
    linear.unit = DraftingUnit::radian;
    require(!linear.validate(), "angle unit on a linear constraint must be rejected");

    DraftingDimension angle{
        "dim.angle", DraftingDimensionKind::angle, "vertex.a", "vertex.b", "vertex.c",
        90.0, 0.25, 0.25, DraftingUnit::degree, "Included angle"};
    require(static_cast<bool>(angle.validate()),
        "angle constraint requires three distinct anchors");
    angle.anchor_c = angle.anchor_b;
    require(!angle.validate(), "duplicate angle anchors must be rejected");
}

void edits_are_source_bound_revisioned_and_undoable() {
    const auto source = source_binding();
    auto created = DraftingDocument::create(source);
    require(static_cast<bool>(created), "source-bound drafting document should be created");
    auto document = std::move(created).value();

    const auto added_dimension = document.set_dimension(
        source, document.revision(), distance(), "Record reviewed edge length.");
    require(added_dimension && document.dimensions().size() == 1U &&
            added_dimension.value().revision_after == core::Revision(1U),
        "dimension edit must advance the document revision");
    const auto added_guide = document.set_guide(
        source, document.revision(), line_guide(), "Add the center datum guide.");
    require(added_guide && document.guides().size() == 1U,
        "guide edit must be recorded in the same revisioned document");

    auto stale_source = source;
    stale_source.revision = core::Revision(13U);
    require(!document.set_dimension(stale_source, document.revision(), distance("stale"),
                "This source has changed."),
        "source revision drift must block edits");
    require(!document.set_dimension(source, core::Revision(0U), distance("stale"),
                "This document revision is old."),
        "stale document revision must block edits");
    require(!document.remove_guide(source, document.revision(), "missing",
                "Do not remove a missing guide."),
        "missing guide deletion must fail without mutation");
    require(document.revision() == core::Revision(2U) &&
            document.dimensions().size() == 1U && document.guides().size() == 1U,
        "refused edits must leave authoring state intact");

    const auto undone = document.undo(source, document.revision());
    require(undone && document.guides().empty() && document.dimensions().size() == 1U,
        "undo must restore the prior guide state");
    const auto redone = document.redo(source, document.revision());
    require(redone && document.guides().size() == 1U,
        "redo must restore the authored guide");
}

void persistence_preserves_history_cursor_and_redo_branch() {
    const auto source = source_binding();
    auto created = DraftingDocument::create(source);
    require(static_cast<bool>(created), "source-bound drafting document should be created");
    auto document = std::move(created).value();
    require(document.set_dimension(source, document.revision(), distance(),
                "Add nominal dimension."),
        "dimension should be added");
    require(document.set_guide(source, document.revision(), line_guide(),
                "Add a source datum guide."),
        "guide should be added");
    require(document.undo(source, document.revision()), "guide edit should undo");

    const auto serialized = document.serialize();
    require(static_cast<bool>(serialized), "drafting document should serialize");
    static std::atomic<unsigned> suffix{0U};
    const auto timestamp = std::chrono::high_resolution_clock::now()
        .time_since_epoch().count();
    auto folder = std::filesystem::temp_directory_path() /
        ("carto-drafting-" + std::to_string(timestamp) + "-" +
            std::to_string(suffix.fetch_add(1U)));
    bool created_folder = false;
    for (unsigned attempt = 0U; attempt < 16U; ++attempt) {
        if (std::filesystem::create_directory(folder)) {
            created_folder = true;
            break;
        }
        folder = std::filesystem::temp_directory_path() /
            ("carto-drafting-" + std::to_string(timestamp) + "-" +
                std::to_string(suffix.fetch_add(1U)));
    }
    require(created_folder,
        "test must create its unique temporary directory");
    const auto file = folder / "drawing.carto-drafting";
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output.write(serialized.value().data(),
            static_cast<std::streamsize>(serialized.value().size()));
        require(static_cast<bool>(output), "drafting document should save to disk");
    }
    std::ifstream input(file, std::ios::binary);
    const std::string saved{std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    input.close();
    const auto reopened = DraftingDocument::deserialize(saved);
    require(reopened && reopened.value().history_cursor() == document.history_cursor() &&
            reopened.value().history_size() == document.history_size() &&
            reopened.value().dimensions() == document.dimensions() &&
            reopened.value().guides() == document.guides(),
        "file reopen must preserve source-bound state and its redo branch");
    auto restored = reopened.value();
    const auto redone = restored.redo(source, restored.revision());
    require(redone && restored.guides().size() == 1U,
        "reopened history must retain the redo state");
    const auto branch = restored.set_dimension(source, restored.revision(),
        distance("dim.branch"), "Create a reviewed alternate branch.");
    require(branch && restored.history_cursor() + 1U == restored.history_size(),
        "new edits after a rewind must discard later redo states");
    std::filesystem::remove_all(folder);
}

void malformed_and_tampered_documents_fail_closed() {
    const auto source = source_binding();
    auto created = DraftingDocument::create(source);
    require(static_cast<bool>(created), "source-bound drafting document should be created");
    auto document = std::move(created).value();
    require(document.set_dimension(source, document.revision(), distance(),
                "Add a dimension."), "dimension should be added");
    const auto serialized = document.serialize();
    require(static_cast<bool>(serialized), "drafting document should serialize");

    auto tampered = serialized.value();
    const auto label = tampered.find("Overall length");
    require(label != std::string::npos, "serialized test label should be present");
    tampered[label] = 'X';
    require(!DraftingDocument::deserialize(tampered),
        "tampered draft must fail checksum validation");
    require(!DraftingDocument::deserialize(serialized.value() + "extra\n"),
        "trailing data must not be silently ignored");
    require(!DraftingDocument::deserialize("CARTO_DRAFTING_DOCUMENT_V99\n"),
        "unknown document versions must be rejected");

    auto invalid = distance();
    invalid.target = std::numeric_limits<double>::infinity();
    require(!document.set_dimension(source, document.revision(), invalid,
                "Non-finite values must be refused."),
        "non-finite dimension values must be rejected");
    require(document.revision() == core::Revision(1U),
        "invalid record must not change the document revision");
}

} // namespace

int main() {
    dimensional_units_and_typed_constraints_validate();
    edits_are_source_bound_revisioned_and_undoable();
    persistence_preserves_history_cursor_and_redo_branch();
    malformed_and_tampered_documents_fail_closed();
    std::cout << "drafting checks passed\n";
    return EXIT_SUCCESS;
}
