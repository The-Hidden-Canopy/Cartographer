#include "application_access.hpp"
#include "../apps/desktop/starter_material_library.hpp"
#include <carto/io/gltf_material_import.hpp>
#include <carto/project/project.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <vector>
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) return 2;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    if (std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()) !=
        studio::kStarterMaterialLibrary) return 13;
    auto result = carto::io::import_gltf_materials(bytes);
    if (!result) { std::cerr << result.error().message << '\n'; return 3; }
    const auto& catalog = result.value().catalog;
    if (catalog.materials().size() != 24 || !catalog.textures().empty()) return 4;
    std::set<std::string> names;
    for (const auto& [id, mat] : catalog.materials()) {
        if (!mat.definition.validate() || !names.insert(mat.name).second ||
            mat.definition.base_color || mat.definition.metallic_roughness ||
            mat.definition.normal || mat.definition.occlusion || mat.definition.emissive) return 5;
    }
    namespace app = carto::application;
    const auto root = std::filesystem::temp_directory_path() /
        ("carto-starter-library-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto path = root / "starter.carto";
    app::ApplicationSession session;
    app::SaveProjectAction save_as{}; save_as.path = path;
    if (!app::testing::dispatch(session, save_as)) return 6;
    auto snapshot = session.snapshot();
    if (app::testing::dispatch(session, app::ImportGltfMaterialLibraryAction{
            snapshot.project_generation, snapshot.project_revision, bytes, false})) return 7;
    if (!app::testing::dispatch(session, app::ImportGltfMaterialLibraryAction{
            snapshot.project_generation, snapshot.project_revision, bytes, true})) return 8;
    snapshot = session.snapshot();
    if (snapshot.materials.size() != 24) return 9;
    if (app::testing::dispatch(session, app::ImportGltfMaterialLibraryAction{
            snapshot.project_generation, snapshot.project_revision, bytes, true})) return 10;
    if (!app::testing::dispatch(session, app::SaveProjectAction{})) return 11;
    auto reopened = carto::project::ProjectDocument::load(path);
    if (!reopened || reopened.value().material_catalog().materials().size() != 24 ||
        reopened.value().source_asset_registry().records().size() != 1) return 12;
    std::cout << catalog.materials().size() << " validated and persisted material presets\n";
    return 0;
}

