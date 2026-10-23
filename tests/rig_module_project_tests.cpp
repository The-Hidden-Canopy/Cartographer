#include <carto/character/rig_module.hpp>
#include <carto/journal/journal.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

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
            ("cartographer-rig-module-project-tests-" + std::to_string(stamp));
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

character::RigModuleTemplate module(std::string semantic_type = "arm.root") {
    character::RigModuleTemplate result;
    result.name = "arm.left";
    kinematic::KinematicNode root;
    root.id = kinematic::JointId{10U};
    root.kind = kinematic::JointKind::ball;
    root.semantic_type = std::move(semantic_type);
    result.joints.push_back(std::move(root));
    return result;
}

std::string serialized(const character::RigModuleTemplate& value) {
    const auto bytes = value.serialize();
    require(static_cast<bool>(bytes), "fixture module serializes canonically");
    return bytes.value();
}

assets::Sha256Digest digest(std::string_view bytes) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    return assets::sha256(std::span<const std::uint8_t>(data, bytes.size()));
}

void erase_v33_character_record_tables(std::string& serialized) {
    for (const std::string table : {
            "POSE_LIBRARIES 0\n",
            "WEIGHT_EDIT_RECEIPTS 0\n",
            "CURVE_EDIT_RECEIPTS 0\n"}) {
        const auto record = serialized.find(table);
        require(record != std::string::npos,
            "fixture removes a schema v33 character record table");
        serialized.erase(record, table.size());
    }
}

void rig_module_library_is_transactional_revisioned_and_migrates() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "rig-module-events.log");
    const std::string original_bytes = serialized(module());

    auto insert = project::ProjectTransaction::begin(document, journal, "rig-module-tests");
    require(insert && insert.value().insert_rig_module_template(original_bytes),
        "valid canonical module inserts through a project transaction");
    const auto inserted = insert.value().commit("rig.module.insert");
    require(inserted && inserted.value().revision_after == core::Revision{1U},
        "module insert commits one project revision and journal transition");
    const auto original_entry = document.rig_module_templates().at("arm.left");
    require(original_entry.revision == core::Revision{1U} &&
            original_entry.bytes == original_bytes && original_entry.digest == digest(original_bytes),
        "inserted library entry has revision one and a byte-bound digest");

    auto duplicate = project::ProjectTransaction::begin(document, journal, "rig-module-tests");
    require(duplicate && !duplicate.value().insert_rig_module_template(
                serialized(module("arm.changed"))),
        "insert refuses to overwrite an existing stable module identity");
    require(static_cast<bool>(duplicate.value().rollback()) &&
            document.rig_module_templates().at("arm.left") == original_entry,
        "duplicate refusal leaves the project entry unchanged");

    auto malformed = project::ProjectTransaction::begin(document, journal, "rig-module-tests");
    require(malformed && !malformed.value().insert_rig_module_template(original_bytes + "x"),
        "non-canonical or trailing template data is rejected before publication");
    require(static_cast<bool>(malformed.value().rollback()),
        "malformed template transaction rolls back");

    auto stale = project::ProjectTransaction::begin(document, journal, "rig-module-tests");
    require(stale && !stale.value().replace_rig_module_template_if_revision(
                "arm.left", core::Revision{2U}, original_entry.digest,
                serialized(module("arm.stale"))),
        "replacement refuses a revision that does not match the current entry");
    require(static_cast<bool>(stale.value().rollback()) &&
            document.rig_module_templates().at("arm.left") == original_entry,
        "stale replacement leaves project data unchanged");

    auto renamed = module("arm.updated");
    renamed.name = "arm.right";
    auto wrong_identity = project::ProjectTransaction::begin(
        document, journal, "rig-module-tests");
    require(wrong_identity && !wrong_identity.value().replace_rig_module_template_if_revision(
                "arm.left", original_entry.revision, original_entry.digest,
                serialized(renamed)),
        "replacement cannot move content to a different stable identity");
    require(static_cast<bool>(wrong_identity.value().rollback()),
        "identity mismatch replacement rolls back");

    const std::string updated_bytes = serialized(module("arm.updated"));
    auto replace = project::ProjectTransaction::begin(document, journal, "rig-module-tests");
    require(static_cast<bool>(replace), "replacement transaction begins");
    const auto replacement_revision = replace.value().replace_rig_module_template_if_revision(
        "arm.left", original_entry.revision, original_entry.digest, updated_bytes);
    require(replacement_revision && replacement_revision.value() == core::Revision{2U},
        "matching revision and digest replace the module and advance its library revision");
    const auto replaced = replace.value().commit("rig.module.replace");
    require(replaced && replaced.value().revision_after == core::Revision{2U},
        "replacement commits through the project journal");

    auto stale_after_replace = project::ProjectTransaction::begin(
        document, journal, "rig-module-tests");
    require(stale_after_replace && !stale_after_replace.value().replace_rig_module_template_if_revision(
                "arm.left", original_entry.revision, original_entry.digest, original_bytes),
        "an old revision and digest cannot roll back a newer template");
    require(static_cast<bool>(stale_after_replace.value().rollback()),
        "post-replacement stale transaction rolls back");

    const auto project_path = directory.path() / "rig-modules.carto";
    require(static_cast<bool>(document.save_atomic(project_path)),
        "module library saves as part of the project document");
    const auto reopened = project::ProjectDocument::load(project_path);
    require(reopened && reopened.value().rig_module_templates().at("arm.left").revision ==
                core::Revision{2U} &&
            reopened.value().rig_module_templates().at("arm.left").bytes == updated_bytes,
        "save and reopen preserve the replaced module bytes and revision");
    const auto decoded = character::RigModuleTemplate::deserialize(
        reopened.value().rig_module_templates().at("arm.left").bytes);
    require(decoded && decoded.value().name == "arm.left" &&
            decoded.value().joints.front().semantic_type == "arm.updated",
        "reopened module remains instantiable canonical authoring data");
    kinematic::KinematicGraph target;
    kinematic::KinematicNode target_root;
    target_root.id = kinematic::JointId{1U};
    target_root.kind = kinematic::JointKind::ball;
    target_root.semantic_type = "spine";
    require(static_cast<bool>(target.insert_node(target_root)) &&
            static_cast<bool>(target.validate()),
        "fixture target rig has a valid attachment root");
    character::ControlGraph target_controls;
    const auto instance = decoded
        ? character::instantiate_rig_module(decoded.value(), target, target_controls,
            character::ModuleIdPolicy::allocate_fresh_ids)
        : core::Result<character::RigModuleInstance>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "reopened rig module did not decode"));
    require(instance && instance.value().kinematic_graph.nodes().size() == 2U,
        "a module loaded from project storage still instantiates through the character API");

    const auto events = journal.read_all();
    require(events && events.value().size() == 2U &&
            events.value()[0U].event_type == "rig.module.insert" &&
            events.value()[1U].event_type == "rig.module.replace",
        "only successful module library transitions produce journal events");

    std::string schema_v30 = project::ProjectDocument{}.serialize();
    const auto v30_decal_table = schema_v30.rfind("DECAL_PLACEMENTS 0\n");
    require(v30_decal_table != std::string::npos,
        "current serialization carries the mandatory decal placement table");
    schema_v30.erase(v30_decal_table, std::string("DECAL_PLACEMENTS 0\n").size());
    const auto v30_standards = schema_v30.rfind("AUTHORING_STANDARDS_HEX ");
    require(v30_standards != std::string::npos,
        "current serialization carries the mandatory authoring standards record");
    const auto v30_standards_end = schema_v30.find('\n', v30_standards);
    schema_v30.erase(v30_standards, v30_standards_end - v30_standards + 1U);
    erase_v33_character_record_tables(schema_v30);
    schema_v30.replace(0U, std::string("CARTOGRAPHER_PROJECT 33\n").size(),
        "CARTOGRAPHER_PROJECT 30\n");
    const auto migrated_v30 = project::ProjectDocument::deserialize(schema_v30);
    require(migrated_v30 && migrated_v30.value().decal_placements().empty() &&
            migrated_v30.value().authoring_standards().empty() &&
            migrated_v30.value().serialize().find("CARTOGRAPHER_PROJECT 33\n") == 0U &&
            migrated_v30.value().serialize().find("DECAL_PLACEMENTS 0\n") !=
                std::string::npos,
        "schema v30 migrates to v33 with empty decal, standards, and character record tables");

    std::string schema_v29 = project::ProjectDocument{}.serialize();
    const auto table = schema_v29.rfind("RIG_MODULE_TEMPLATES 0\n");
    require(table != std::string::npos,
        "current serialization carries the mandatory empty-or-populated library table");
    schema_v29.erase(table, std::string("RIG_MODULE_TEMPLATES 0\n").size());
    const auto decal_table = schema_v29.rfind("DECAL_PLACEMENTS 0\n");
    require(decal_table != std::string::npos,
        "current serialization carries the mandatory decal placement table");
    schema_v29.erase(decal_table, std::string("DECAL_PLACEMENTS 0\n").size());
    const auto v29_standards = schema_v29.rfind("AUTHORING_STANDARDS_HEX ");
    require(v29_standards != std::string::npos,
        "current serialization carries the mandatory authoring standards record");
    const auto v29_standards_end = schema_v29.find('\n', v29_standards);
    schema_v29.erase(v29_standards, v29_standards_end - v29_standards + 1U);
    erase_v33_character_record_tables(schema_v29);
    const auto header_end = schema_v29.find('\n');
    require(header_end != std::string::npos && schema_v29.substr(0U, header_end) ==
                "CARTOGRAPHER_PROJECT 33",
        "fixture begins at the expected current schema");
    schema_v29.replace(0U, header_end, "CARTOGRAPHER_PROJECT 29");
    const auto migrated = project::ProjectDocument::deserialize(schema_v29);
    require(migrated && migrated.value().rig_module_templates().empty() &&
            migrated.value().decal_placements().empty() &&
            migrated.value().authoring_standards().empty() &&
            migrated.value().serialize().find("CARTOGRAPHER_PROJECT 33\n") == 0U &&
            migrated.value().serialize().find("RIG_MODULE_TEMPLATES 0\n") !=
                std::string::npos,
        "schema v29 migrates to v33 with empty module, decal, standards, and character record tables");
}

} // namespace

int main() {
    rig_module_library_is_transactional_revisioned_and_migrates();
    return EXIT_SUCCESS;
}
