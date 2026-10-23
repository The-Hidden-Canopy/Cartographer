#include <carto/journal/journal.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/motion/curve_edit.hpp>
#include <carto/pose/pose_library.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>
#include <carto/weights/paint.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace carto;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::current_path() /
            ("cartographer-character-records-tests-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

assets::Sha256Digest digest_of(std::string_view bytes) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    return assets::sha256(std::span<const std::uint8_t>(data, bytes.size()));
}

std::string serialized(pose::PoseLibrary& library, double wave_height) {
    pose::LocalPose wave;
    core::Transform arm;
    arm.translation = {0.1, wave_height, 0.3};
    arm.rotation = {0.0, 0.0, 0.0, 1.0};
    wave.emplace(kinematic::JointId{1U}, arm);
    require(static_cast<bool>(library.add("wave.pose", wave)),
        "fixture pose library accepts its first pose");
    pose::LocalPose rest;
    core::Transform spine;
    spine.translation = {0.0, 1.0, 0.0};
    spine.rotation = {0.0, 0.0, 0.0, 1.0};
    rest.emplace(kinematic::JointId{2U}, spine);
    require(static_cast<bool>(library.add("rest.pose", rest)),
        "fixture pose library accepts its second pose");
    return library.serialize();
}

std::pair<std::string, assets::Sha256Digest> pose_library_record(double wave_height = 0.2) {
    pose::PoseLibrary library;
    const std::string bytes = serialized(library, wave_height);
    return {bytes, digest_of(bytes)};
}

std::pair<std::string, assets::Sha256Digest> weight_edit_record() {
    weights::WeightEditReceipt receipt;
    receipt.kind = weights::WeightEditKind::flood;
    receipt.joint = kinematic::JointId{1U};
    receipt.op = weights::PaintOp::set;
    receipt.value = 0.75;
    receipt.joint_map_digest = digest_of("fixture-joint-map");
    receipt.input_binding_digest = digest_of("fixture-input-binding");
    receipt.output_binding_digest = digest_of("fixture-output-binding");
    receipt.affected_vertex_count = 12U;
    require(static_cast<bool>(receipt.validate()), "fixture weight edit receipt validates");
    const std::string bytes = receipt.serialize();
    return {bytes, digest_of(bytes)};
}

std::pair<std::string, assets::Sha256Digest> curve_edit_record() {
    motion::KeyedCurve curve;
    curve.id = motion::MotionCurveId{7U};
    curve.timebase_ticks_per_second = 30U;
    curve.interpolation = motion::InterpolationKind::linear;
    curve.keys.push_back(motion::CurveKey{character::RationalTime{0, 30U}, 0.0, 0.0, 0.0});
    curve.keys.push_back(motion::CurveKey{character::RationalTime{30, 30U}, 1.0, 0.0, 0.0});
    require(static_cast<bool>(curve.validate()), "fixture keyed curve validates");
    const auto edited = motion::insert_key(
        curve,
        motion::CurveKey{character::RationalTime{15, 30U}, 0.5, 0.0, 0.0},
        false);
    require(edited && edited.value().receipt.affected_key_count == 1U,
        "fixture curve edit produces one receipt-bound key");
    const std::string bytes = edited.value().receipt.serialize();
    return {bytes, digest_of(bytes)};
}

void character_records_are_transactional_digest_bound_and_persist() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "character-records-events.log");

    const auto [pose_bytes, pose_digest] = pose_library_record();
    auto insert_library = project::ProjectTransaction::begin(
        document, journal, "character-records-tests");
    require(insert_library && insert_library.value().set_pose_library(
                "library.main", core::Revision{}, pose_bytes, pose_digest),
        "canonical pose library inserts through a project transaction");
    const auto inserted = insert_library.value().commit("pose.library.insert");
    require(inserted && inserted.value().revision_after == core::Revision{1U},
        "pose library insert commits one project revision");
    const auto original_entry = document.pose_libraries().at("library.main");
    require(original_entry.revision == core::Revision{1U} &&
            original_entry.bytes == pose_bytes && original_entry.digest == pose_digest,
        "inserted pose library entry has revision one and a byte-bound digest");

    const auto [second_pose_bytes, second_pose_digest] = pose_library_record(0.9);
    auto overwrite = project::ProjectTransaction::begin(document, journal, "character-records-tests");
    require(overwrite && !overwrite.value().set_pose_library(
                "library.main", core::Revision{}, second_pose_bytes, second_pose_digest),
        "unversioned replace of an existing pose library is a stale refusal");
    require(static_cast<bool>(overwrite.value().rollback()) &&
            document.pose_libraries().at("library.main") == original_entry,
        "stale pose library refusal leaves the project entry unchanged");

    auto tampered_library = project::ProjectTransaction::begin(
        document, journal, "character-records-tests");
    require(tampered_library && !tampered_library.value().set_pose_library(
                "library.main", core::Revision{1U}, pose_bytes + "\n",
                digest_of(pose_bytes + "\n")),
        "non-canonical pose library bytes are rejected before publication");
    auto bad_digest = project::ProjectTransaction::begin(document, journal, "character-records-tests");
    require(bad_digest && !bad_digest.value().set_pose_library(
                "library.hands", core::Revision{}, pose_bytes,
                digest_of("not the serialized library bytes")),
        "pose library digest must bind the exact serialized bytes");
    require(static_cast<bool>(tampered_library.value().rollback()) &&
            static_cast<bool>(bad_digest.value().rollback()),
        "pose library refusals roll back");

    auto replaced = project::ProjectTransaction::begin(document, journal, "character-records-tests");
    const auto replacement_revision = replaced.value().set_pose_library(
        "library.main", core::Revision{1U}, second_pose_bytes, second_pose_digest);
    require(replaced && replacement_revision &&
            replacement_revision.value() == core::Revision{2U},
        "matching revision replaces the pose library and advances its revision");
    require(static_cast<bool>(replaced.value().commit("pose.library.replace")),
        "pose library replacement commits through the project journal");

    auto stale_replace = project::ProjectTransaction::begin(document, journal, "character-records-tests");
    require(stale_replace && !stale_replace.value().set_pose_library(
                "library.main", core::Revision{1U}, pose_bytes, pose_digest),
        "an old revision cannot roll back a newer pose library");
    require(static_cast<bool>(stale_replace.value().rollback()),
        "post-replacement stale transaction rolls back");

    const auto [weight_bytes, weight_digest] = weight_edit_record();
    auto append_weight = project::ProjectTransaction::begin(document, journal, "character-records-tests");
    require(append_weight && append_weight.value().append_weight_edit_receipt(
                weight_bytes, weight_digest),
        "weight edit receipt appends through a project transaction");
    require(static_cast<bool>(append_weight.value().commit("weights.edit.receipt")),
        "weight edit receipt commits through the project journal");
    require(document.weight_edit_receipts().size() == 1U &&
            document.weight_edit_receipts().front().bytes == weight_bytes &&
            document.weight_edit_receipts().front().digest == weight_digest,
        "appended weight edit receipt binds its exact serialized bytes");

    auto hostile_weight = project::ProjectTransaction::begin(document, journal, "character-records-tests");
    weights::WeightEditReceipt hostile_receipt;
    hostile_receipt.kind = weights::WeightEditKind::flood;
    hostile_receipt.joint = kinematic::JointId{1U};
    hostile_receipt.op = weights::PaintOp::set;
    hostile_receipt.value = 0.5;
    hostile_receipt.input_binding_digest = assets::Sha256Digest{};
    hostile_receipt.output_binding_digest = digest_of("fixture-output-binding");
    require(!hostile_receipt.validate() && !hostile_weight.value().append_weight_edit_receipt(
                hostile_receipt.serialize(), digest_of(hostile_receipt.serialize())),
        "a weight edit receipt that binds no input digest is refused");
    require(static_cast<bool>(hostile_weight.value().rollback()) &&
            document.weight_edit_receipts().size() == 1U,
        "hostile weight receipt refusal leaves the receipt ledger unchanged");

    const auto [curve_bytes, curve_digest] = curve_edit_record();
    auto append_curve = project::ProjectTransaction::begin(document, journal, "character-records-tests");
    require(append_curve && append_curve.value().append_curve_edit_receipt(
                curve_bytes, curve_digest),
        "curve edit receipt appends through a project transaction");
    require(static_cast<bool>(append_curve.value().commit("curve.edit.receipt")),
        "curve edit receipt commits through the project journal");
    require(document.curve_edit_receipts().size() == 1U &&
            document.curve_edit_receipts().front().digest == curve_digest,
        "appended curve edit receipt binds its exact serialized bytes");

    const std::string revision_four = document.serialize();
    const project::ProjectDocument snapshot_after_curve = document;

    // A later unrelated commit advances the document; undo/redo restores whole
    // document snapshots, so earlier historical receipts must survive an undo
    // cycle and reappear with their exact bytes and digests on redo.
    auto mesh_commit = project::ProjectTransaction::begin(document, journal, "character-records-tests");
    require(mesh_commit && static_cast<bool>(mesh_commit.value().create_object("probe")),
        "unrelated object creation stages through a project transaction");
    require(static_cast<bool>(mesh_commit.value().commit("probe.object")),
        "unrelated object creation commits");
    require(document.pose_libraries().size() == 1U &&
            document.weight_edit_receipts().size() == 1U &&
            document.curve_edit_receipts().size() == 1U,
        "historical records ride along an unrelated journaled commit");

    const std::string revision_five = document.serialize();
    const auto undone = project::ProjectDocument::deserialize(revision_four);
    require(undone && undone.value().pose_libraries().at("library.main") ==
                snapshot_after_curve.pose_libraries().at("library.main") &&
            undone.value().weight_edit_receipts().front().digest == weight_digest &&
            undone.value().curve_edit_receipts().front().bytes == curve_bytes &&
            undone.value().scene().find(scene::ObjectId{1U}) == nullptr,
        "undo restores the pre-commit snapshot with every historical record intact");
    const auto redone = project::ProjectDocument::deserialize(revision_five);
    require(redone && redone.value().curve_edit_receipts().size() == 1U &&
            redone.value().scene().find(scene::ObjectId{1U}) != nullptr,
        "redo restores the post-commit snapshot with every historical record intact");

    const auto project_path = directory.path() / "character-records.carto";
    require(static_cast<bool>(document.save_atomic(project_path)),
        "character records save as part of the project document");
    const auto reopened = project::ProjectDocument::load(project_path);
    require(reopened && reopened.value().pose_libraries().size() == 1U &&
            reopened.value().weight_edit_receipts().size() == 1U &&
            reopened.value().curve_edit_receipts().size() == 1U,
        "save and reopen preserve all three character record tables");
    const auto& reopened_library = reopened.value().pose_libraries().at("library.main");
    require(reopened_library.revision == core::Revision{2U} &&
            reopened_library.bytes == second_pose_bytes &&
            reopened_library.digest == second_pose_digest,
        "reopened pose library retains its replaced bytes, revision, and digest");
    const auto decoded_library = pose::PoseLibrary::deserialize(reopened_library.bytes);
    require(decoded_library && decoded_library.value().entries().size() == 2U &&
            decoded_library.value().find("wave.pose") != nullptr,
        "reopened pose library remains canonical carto_pose data");
    const auto decoded_weight = weights::WeightEditReceipt::deserialize(
        reopened.value().weight_edit_receipts().front().bytes);
    require(decoded_weight && decoded_weight.value().input_binding_digest ==
                digest_of("fixture-input-binding") &&
            decoded_weight.value().output_binding_digest ==
                digest_of("fixture-output-binding"),
        "reopened weight edit receipt still binds its input/output binding digests");
    const auto decoded_curve = motion::CurveEditReceipt::deserialize(
        reopened.value().curve_edit_receipts().front().bytes);
    require(decoded_curve && !decoded_curve.value().source_digest.is_zero() &&
            !decoded_curve.value().result_digest.is_zero(),
        "reopened curve edit receipt still binds its source/result curve digests");

    const auto events = journal.read_all();
    require(events && events.value().size() == 5U,
        "only successful character record transitions produce journal events");

    std::string tampered = revision_five;
    const auto digest_pos = tampered.rfind(weight_digest.hex());
    require(digest_pos != std::string::npos, "saved text carries the weight receipt digest");
    tampered[digest_pos] = tampered[digest_pos] == '0' ? '1' : '0';
    require(!project::ProjectDocument::deserialize(tampered),
        "a tampered weight edit receipt digest fails closed on load");

    std::string stale_pose_record = revision_five;
    const auto pose_pos = stale_pose_record.rfind(second_pose_digest.hex());
    require(pose_pos != std::string::npos, "saved text carries the pose library digest");
    stale_pose_record[pose_pos] = stale_pose_record[pose_pos] == '0' ? '1' : '0';
    require(!project::ProjectDocument::deserialize(stale_pose_record),
        "a pose library record with a mismatched digest fails closed on load");

    std::string truncated = revision_five;
    const auto curve_table = truncated.rfind("CURVE_EDIT_RECEIPTS ");
    require(curve_table != std::string::npos, "saved text carries the curve receipt table");
    const auto curve_table_end = truncated.find('\n', curve_table);
    truncated.erase(curve_table, curve_table_end - curve_table + 1U);
    require(!project::ProjectDocument::deserialize(truncated),
        "a schema v33 project missing a mandatory table fails closed on load");
}

void character_record_tables_migrate_from_v32() {
    const auto empty_v33 = project::ProjectDocument::deserialize(
        project::ProjectDocument{}.serialize());
    require(empty_v33 && empty_v33.value().pose_libraries().empty() &&
            empty_v33.value().weight_edit_receipts().empty() &&
            empty_v33.value().curve_edit_receipts().empty(),
        "an empty schema v33 document round-trips through its mandatory tables");

    std::string schema_v32 = project::ProjectDocument{}.serialize();
    const auto pose_table = schema_v32.rfind("POSE_LIBRARIES 0\n");
    require(pose_table != std::string::npos,
        "current serialization carries the mandatory pose library table");
    schema_v32.erase(pose_table, std::string("POSE_LIBRARIES 0\n").size());
    const auto weight_table = schema_v32.rfind("WEIGHT_EDIT_RECEIPTS 0\n");
    require(weight_table != std::string::npos,
        "current serialization carries the mandatory weight edit receipt table");
    schema_v32.erase(weight_table, std::string("WEIGHT_EDIT_RECEIPTS 0\n").size());
    const auto curve_table = schema_v32.rfind("CURVE_EDIT_RECEIPTS 0\n");
    require(curve_table != std::string::npos,
        "current serialization carries the mandatory curve edit receipt table");
    schema_v32.erase(curve_table, std::string("CURVE_EDIT_RECEIPTS 0\n").size());
    schema_v32.replace(0U, std::string("CARTOGRAPHER_PROJECT 33\n").size(),
        "CARTOGRAPHER_PROJECT 32\n");
    const auto migrated = project::ProjectDocument::deserialize(schema_v32);
    require(migrated && migrated.value().pose_libraries().empty() &&
            migrated.value().weight_edit_receipts().empty() &&
            migrated.value().curve_edit_receipts().empty() &&
            migrated.value().serialize().find("CARTOGRAPHER_PROJECT 33\n") == 0U &&
            migrated.value().serialize().find("POSE_LIBRARIES 0\n") != std::string::npos,
        "schema v32 migrates to v33 with empty character record tables");

    std::string fenced = project::ProjectDocument{}.serialize();
    const auto header_end = fenced.find('\n');
    require(header_end != std::string::npos && fenced.substr(0U, header_end) ==
                "CARTOGRAPHER_PROJECT 33",
        "fixture begins at the expected current schema");
    fenced.replace(0U, header_end, "CARTOGRAPHER_PROJECT 32");
    require(!project::ProjectDocument::deserialize(fenced),
        "schema v32 files cannot carry schema v33 character record tables");
}

} // namespace

int main() {
    character_records_are_transactional_digest_bound_and_persist();
    character_record_tables_migrate_from_v32();
    return EXIT_SUCCESS;
}
