#include <carto/io/gltf_material_import.hpp>
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
    std::cout << catalog.materials().size() << " validated material presets\n";
    return 0;
}
