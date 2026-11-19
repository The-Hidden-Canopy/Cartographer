// Project text writes each double in its shortest exact form. Reloading must
// give bit-identical values, and a second save must give identical text.
#include <carto/application/application.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/project/project.hpp>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}
}  // namespace

int main() {
    try {
        using carto::core::Vec3d;
        const double third = 1.0 / 3.0;
        const std::vector<Vec3d> positions{
            {0.1, 0.2, 0.3},
            {third, -0.0, 1e-300},
            {std::numeric_limits<double>::denorm_min(), 1e20, -2.5e-8},
            {123456.789, std::nextafter(1.0, 2.0), 7.0},
        };
        carto::geometry::EditableMesh mesh;
        std::vector<carto::geometry::VertexId> ids;
        for (const auto& position : positions) {
            auto id = mesh.add_vertex(position);
            require(bool(id), "vertex added");
            ids.push_back(id.value());
        }
        require(bool(mesh.add_face({ids[0], ids[1], ids[2]})), "first face");
        require(bool(mesh.add_face({ids[0], ids[2], ids[3]})), "second face");

        carto::application::ApplicationSession session;
        require(bool(carto::application::HumanApplicationAccess::dispatch(
            session, carto::application::CreateMeshObjectAction{"Number format", std::move(mesh)})),
            "mesh object created");
        const auto root = std::filesystem::temp_directory_path() /
            ("carto-number-format-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        const auto path = root / "numbers.carto";
        require(bool(carto::application::HumanApplicationAccess::dispatch(
            session, carto::application::SaveProjectAction{path, {}, {}, {}, false})), "saved");

        const auto text = read_text(path);
        require(text.find("0.1 0.2 0.3") != std::string::npos, "short decimals are written as authored");
        require(text.find("0.10000000000000001") == std::string::npos, "no 17-digit padding");

        auto reopened = carto::project::ProjectDocument::load(path);
        require(bool(reopened), "reopened");
        require(reopened.value().meshes().size() == 1U, "one mesh");
        const auto& loaded = reopened.value().meshes().begin()->second;
        for (std::size_t index = 0U; index < ids.size(); ++index) {
            const auto* vertex = loaded.find_vertex(ids[index]);
            require(vertex != nullptr, "vertex identity survives");
            const auto& expected = positions[index];
            require(std::bit_cast<std::uint64_t>(vertex->position.x) == std::bit_cast<std::uint64_t>(expected.x) &&
                    std::bit_cast<std::uint64_t>(vertex->position.y) == std::bit_cast<std::uint64_t>(expected.y) &&
                    std::bit_cast<std::uint64_t>(vertex->position.z) == std::bit_cast<std::uint64_t>(expected.z),
                "reloaded doubles are bit-identical, including -0.0 and denormals");
        }
        require(reopened.value().serialize() == text, "save is a fixed point after reload");
        std::filesystem::remove_all(root);
        std::cout << "PASS shortest round-trip project numbers\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
