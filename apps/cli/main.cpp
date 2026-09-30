#include <carto/io/obj.hpp>
#include <carto/io/builtin_provider.hpp>
#include <carto/io/ply.hpp>
#include <carto/io/stl.hpp>
#include <carto/io/vanta_export.hpp>
#include <carto/application/application.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/providers/registry.hpp>
#include <carto/project/project.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using carto::core::Result;

void print_error(const carto::core::Diagnostic& diagnostic) {
    std::cerr << "error[" << carto::core::error_code_name(diagnostic.code) << "]: "
              << diagnostic.message << '\n';
    for (const auto& context : diagnostic.context) {
        std::cerr << "  context: " << context << '\n';
    }
}

int demo(const std::filesystem::path& path) {
    carto::application::ApplicationSession session;
    auto created_project = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::NewProjectAction{"Cartographer 0.1 sample"});
    if (!created_project) {
        print_error(created_project.error());
        return EXIT_FAILURE;
    }
    auto baseline = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SaveProjectAction{path});
    if (!baseline) {
        print_error(baseline.error());
        return EXIT_FAILURE;
    }
    auto created_object = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateBoxAction{"Cube", {2.0, 2.0, 2.0}});
    if (!created_object) {
        print_error(created_object.error());
        return EXIT_FAILURE;
    }
    auto receipt = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SaveProjectAction{std::nullopt});
    if (!receipt) {
        print_error(receipt.error());
        return EXIT_FAILURE;
    }
    const auto snapshot = session.snapshot();
    std::cout << "created " << path << "\n"
              << "objects=" << snapshot.objects.size() << " revision="
              << snapshot.project_revision.value() << " journaled=true"
              << "\n";
    return EXIT_SUCCESS;
}

int validate(const std::filesystem::path& path) {
    auto document = carto::project::ProjectDocument::load(path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    std::cout << "valid " << path << "\n"
              << "schema=" << document.value().schema_version() << " objects="
              << document.value().scene().size() << " meshes=" << document.value().meshes().size()
              << " revision=" << document.value().revision().value() << "\n";
    return EXIT_SUCCESS;
}

int export_obj(const std::filesystem::path& project_path, const std::filesystem::path& obj_path) {
    carto::providers::Registry providers;
    if (auto result = carto::io::register_builtin_obj_providers(providers); !result) {
        print_error(result.error().with_context("OBJ exporter provider"));
        return EXIT_FAILURE;
    }
    if (auto result = providers.resolve("geometry.export.obj"); !result) {
        print_error(result.error().with_context("OBJ exporter capability"));
        return EXIT_FAILURE;
    }
    auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    if (document.value().meshes().empty()) {
        std::cerr << "error: project has no mesh assets\n";
        return EXIT_FAILURE;
    }
    auto report = carto::io::export_obj(document.value().meshes().begin()->second, obj_path);
    if (!report) {
        print_error(report.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << obj_path << " vertices=" << report.value().vertices
              << " triangles=" << report.value().triangles << '\n';
    for (const auto& warning : report.value().warnings) {
        std::cout << "warning: " << warning << '\n';
    }
    return EXIT_SUCCESS;
}

int export_ply(const std::filesystem::path& project_path, const std::filesystem::path& ply_path) {
    carto::providers::Registry providers;
    if (auto result = carto::io::register_builtin_ply_providers(providers); !result) {
        print_error(result.error().with_context("PLY exporter provider"));
        return EXIT_FAILURE;
    }
    if (auto result = providers.resolve("geometry.export.ply"); !result) {
        print_error(result.error().with_context("PLY exporter capability"));
        return EXIT_FAILURE;
    }
    auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    if (document.value().meshes().empty()) {
        std::cerr << "error: project has no mesh assets\n";
        return EXIT_FAILURE;
    }
    auto report = carto::io::export_ply(document.value().meshes().begin()->second, ply_path);
    if (!report) {
        print_error(report.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << ply_path << " vertices=" << report.value().vertices
              << " faces=" << report.value().faces << " triangles=" << report.value().triangles << '\n';
    for (const auto& warning : report.value().warnings) std::cout << "warning: " << warning << '\n';
    return EXIT_SUCCESS;
}

int export_stl(const std::filesystem::path& project_path, const std::filesystem::path& stl_path) {
    carto::providers::Registry providers;
    if (auto result = carto::io::register_builtin_stl_providers(providers); !result) {
        print_error(result.error().with_context("STL exporter provider"));
        return EXIT_FAILURE;
    }
    if (auto result = providers.resolve("geometry.export.stl"); !result) {
        print_error(result.error().with_context("STL exporter capability"));
        return EXIT_FAILURE;
    }
    auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    if (document.value().meshes().empty()) {
        std::cerr << "error: project has no mesh assets\n";
        return EXIT_FAILURE;
    }
    auto report = carto::io::export_stl(document.value().meshes().begin()->second, stl_path);
    if (!report) {
        print_error(report.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << stl_path << " vertices=" << report.value().vertices
              << " triangles=" << report.value().triangles << '\n';
    for (const auto& warning : report.value().warnings) std::cout << "warning: " << warning << '\n';
    return EXIT_SUCCESS;
}

int import_mesh_to_project(
    const std::filesystem::path& source_path,
    const std::filesystem::path& project_path,
    carto::geometry::EditableMesh mesh,
    const carto::io::IoReport& report) {
    carto::application::ApplicationSession session;
    auto created_project = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::NewProjectAction{source_path.stem().string()});
    if (!created_project) {
        print_error(created_project.error());
        return EXIT_FAILURE;
    }
    auto baseline = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SaveProjectAction{project_path});
    if (!baseline) {
        print_error(baseline.error());
        return EXIT_FAILURE;
    }
    auto created_object = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::CreateMeshObjectAction{source_path.stem().string(), std::move(mesh)});
    if (!created_object) {
        print_error(created_object.error());
        return EXIT_FAILURE;
    }
    auto receipt = carto::application::HumanApplicationAccess::dispatch(
        session, carto::application::SaveProjectAction{std::nullopt});
    if (!receipt) {
        print_error(receipt.error());
        return EXIT_FAILURE;
    }
    std::cout << "imported " << source_path << " -> " << project_path
              << " vertices=" << report.vertices << " faces=" << report.faces
              << " triangles=" << report.triangles << " journaled=true\n";
    for (const auto& warning : report.warnings) std::cout << "warning: " << warning << '\n';
    return EXIT_SUCCESS;
}

int import_obj(const std::filesystem::path& obj_path, const std::filesystem::path& project_path) {
    carto::providers::Registry providers;
    if (auto result = carto::io::register_builtin_obj_providers(providers); !result) {
        print_error(result.error().with_context("OBJ importer provider"));
        return EXIT_FAILURE;
    }
    if (auto result = providers.resolve("geometry.import.obj"); !result) {
        print_error(result.error().with_context("OBJ importer capability"));
        return EXIT_FAILURE;
    }
    auto imported = carto::io::import_obj(obj_path);
    if (!imported) {
        print_error(imported.error());
        return EXIT_FAILURE;
    }
    return import_mesh_to_project(obj_path, project_path, std::move(imported.value().mesh),
                                  imported.value().report);
}

int import_ply(const std::filesystem::path& ply_path, const std::filesystem::path& project_path) {
    carto::providers::Registry providers;
    if (auto result = carto::io::register_builtin_ply_providers(providers); !result) {
        print_error(result.error().with_context("PLY importer provider"));
        return EXIT_FAILURE;
    }
    if (auto result = providers.resolve("geometry.import.ply"); !result) {
        print_error(result.error().with_context("PLY importer capability"));
        return EXIT_FAILURE;
    }
    auto imported = carto::io::import_ply(ply_path);
    if (!imported) {
        print_error(imported.error());
        return EXIT_FAILURE;
    }
    return import_mesh_to_project(ply_path, project_path, std::move(imported.value().mesh),
                                  imported.value().report);
}

int import_stl(const std::filesystem::path& stl_path, const std::filesystem::path& project_path) {
    carto::providers::Registry providers;
    if (auto result = carto::io::register_builtin_stl_providers(providers); !result) {
        print_error(result.error().with_context("STL importer provider"));
        return EXIT_FAILURE;
    }
    if (auto result = providers.resolve("geometry.import.stl"); !result) {
        print_error(result.error().with_context("STL importer capability"));
        return EXIT_FAILURE;
    }
    auto imported = carto::io::import_stl(stl_path);
    if (!imported) {
        print_error(imported.error());
        return EXIT_FAILURE;
    }
    return import_mesh_to_project(stl_path, project_path, std::move(imported.value().mesh),
                                  imported.value().report);
}

int export_vanta(const std::filesystem::path& project_path, const std::filesystem::path& gltf_path) {
    carto::providers::Registry providers;
    if (auto result = carto::io::register_builtin_gltf_providers(providers); !result) {
        print_error(result.error().with_context("glTF exporter provider"));
        return EXIT_FAILURE;
    }
    if (auto result = providers.resolve("geometry.export.gltf"); !result) {
        print_error(result.error().with_context("glTF exporter capability"));
        return EXIT_FAILURE;
    }
    auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) {
        print_error(document.error());
        return EXIT_FAILURE;
    }
    if (document.value().meshes().empty()) {
        std::cerr << "error: project has no mesh assets\n";
        return EXIT_FAILURE;
    }
    auto report = carto::io::export_vanta_gltf(document.value(), gltf_path);
    if (!report) {
        print_error(report.error());
        return EXIT_FAILURE;
    }
    std::cout << "exported " << gltf_path << " profile=cartographer-vanta-gltf-v1"
              << " objects=" << report.value().objects
              << " meshes=" << report.value().meshes
              << " vertices=" << report.value().vertices
              << " triangles=" << report.value().triangles
              << " index_bytes=" << report.value().index_bytes
              << " index_type=" << (report.value().used_uint32_indices ? "uint32" : "uint16")
              << '\n';
    for (const auto& warning : report.value().warnings) {
        std::cout << "warning: " << warning << '\n';
    }
    return EXIT_SUCCESS;
}

void usage(std::ostream& output) {
    output << "Cartographer " << CARTOGRAPHER_VERSION << "\n"
           << "Usage:\n"
           << "  cartographer_cli demo <project.carto>\n"
           << "  cartographer_cli validate <project.carto>\n"
           << "  cartographer_cli export-obj <project.carto> <mesh.obj>\n"
           << "  cartographer_cli export-ply <project.carto> <mesh.ply>\n"
           << "  cartographer_cli export-stl <project.carto> <mesh.stl>\n"
           << "  cartographer_cli export-vanta <project.carto> <scene.gltf>\n"
           << "  cartographer_cli import-obj <mesh.obj> <project.carto>\n"
           << "  cartographer_cli import-ply <mesh.ply> <project.carto>\n"
           << "  cartographer_cli import-stl <mesh.stl> <project.carto>\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(std::cerr);
        return EXIT_FAILURE;
    }
    const std::string_view command(argv[1]);
    if (command == "--help" || command == "help") {
        usage(std::cout);
        return EXIT_SUCCESS;
    }
    if (command == "--version") {
        std::cout << CARTOGRAPHER_VERSION << '\n';
        return EXIT_SUCCESS;
    }
    if (command == "demo" && argc == 3) {
        return demo(argv[2]);
    }
    if (command == "validate" && argc == 3) {
        return validate(argv[2]);
    }
    if (command == "export-obj" && argc == 4) {
        return export_obj(argv[2], argv[3]);
    }
    if (command == "export-ply" && argc == 4) {
        return export_ply(argv[2], argv[3]);
    }
    if (command == "export-stl" && argc == 4) {
        return export_stl(argv[2], argv[3]);
    }
    if (command == "export-vanta" && argc == 4) {
        return export_vanta(argv[2], argv[3]);
    }
    if (command == "import-obj" && argc == 4) {
        return import_obj(argv[2], argv[3]);
    }
    if (command == "import-ply" && argc == 4) {
        return import_ply(argv[2], argv[3]);
    }
    if (command == "import-stl" && argc == 4) {
        return import_stl(argv[2], argv[3]);
    }
    usage(std::cerr);
    return EXIT_FAILURE;
}
