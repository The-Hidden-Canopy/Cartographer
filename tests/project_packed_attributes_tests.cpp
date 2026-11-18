// Large float attribute layers are written as one packed record (float32 when
// every scalar is exactly a float32, float64 otherwise). Reloading must give
// bit-identical values, a second save identical text, and the per-value text
// form must still parse.
#include <carto/application/application.hpp>
#include <carto/attributes/set.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/project/project.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
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

std::vector<std::uint64_t> scalar_bits(const carto::attributes::AttributeLayer& layer) {
    std::vector<std::uint64_t> bits;
    for (const auto& value : layer.values) {
        if (const auto* f = std::get_if<float>(&value)) {
            bits.push_back(std::bit_cast<std::uint32_t>(*f));
        } else if (const auto* v3 = std::get_if<carto::core::Vec3d>(&value)) {
            bits.push_back(std::bit_cast<std::uint64_t>(v3->x));
            bits.push_back(std::bit_cast<std::uint64_t>(v3->y));
            bits.push_back(std::bit_cast<std::uint64_t>(v3->z));
        } else if (const auto* v2 = std::get_if<carto::core::Vec2d>(&value)) {
            bits.push_back(std::bit_cast<std::uint64_t>(v2->x));
            bits.push_back(std::bit_cast<std::uint64_t>(v2->y));
        } else if (const auto* d = std::get_if<double>(&value)) {
            bits.push_back(std::bit_cast<std::uint64_t>(*d));
        }
    }
    return bits;
}
}  // namespace

int main() {
    try {
        using namespace carto;
        constexpr int n = 40;  // 40x40 quads -> 3200 triangles, 9600 corners
        geometry::EditableMesh mesh;
        std::vector<geometry::VertexId> ids;
        for (int z = 0; z <= n; ++z)
            for (int x = 0; x <= n; ++x) {
                auto id = mesh.add_vertex(core::Vec3d{x * 0.01, 0.0, z * 0.01});
                require(bool(id), "vertex added");
                ids.push_back(id.value());
            }
        std::size_t faces = 0U;
        for (int z = 0; z < n; ++z)
            for (int x = 0; x < n; ++x) {
                const auto a = ids[z * (n + 1) + x], b = ids[z * (n + 1) + x + 1];
                const auto c = ids[(z + 1) * (n + 1) + x + 1], d = ids[(z + 1) * (n + 1) + x];
                require(bool(mesh.add_face({a, b, c})), "face a");
                require(bool(mesh.add_face({a, c, d})), "face b");
                faces += 2U;
            }
        const std::size_t corners = faces * 3U;
        std::vector<attributes::AttributeValue> normals, uvs, weights, floats;
        for (std::size_t i = 0; i < corners; ++i) {
            // float32-exact values (packed as f32), including -0.0
            const float f = static_cast<float>(i) / 7.0F;
            floats.emplace_back(f);
            normals.emplace_back(core::Vec3d{static_cast<double>(f), i % 2U ? -0.0 : 1.0, 0.1F});
            // not float32-exact (forces f64)
            uvs.emplace_back(core::Vec2d{static_cast<double>(i) / 3.0, 1.0 / (1.0 + i)});
        }
        for (std::size_t i = 0; i < faces; ++i) weights.emplace_back(static_cast<double>(i) * 0.1);
        attributes::AttributeSet set({{attributes::AttributeDomain::corner, corners},
                                      {attributes::AttributeDomain::face, faces}});
        require(bool(set.add_layer({{"test.normal", attributes::AttributeDomain::corner,
                                     attributes::AttributeType::vec3}, normals})), "normal layer");
        require(bool(set.add_layer({{"test.uv", attributes::AttributeDomain::corner,
                                     attributes::AttributeType::vec2}, uvs})), "uv layer");
        require(bool(set.add_layer({{"test.weight", attributes::AttributeDomain::face,
                                     attributes::AttributeType::float64}, weights})), "face layer");
        require(bool(set.add_layer({{"test.float", attributes::AttributeDomain::corner,
            attributes::AttributeType::float32}, floats})), "float layer");
        require(bool(mesh.set_attribute_payload(std::move(set), {})), "payload set");

        const auto expected_faces = mesh.faces_sorted();
        const auto expected_corners = mesh.topology().value().corners;
        application::ApplicationSession session;
        require(bool(application::HumanApplicationAccess::dispatch(
            session, application::CreateMeshObjectAction{"Packed", std::move(mesh)})), "object created");
        const auto root = std::filesystem::temp_directory_path() /
            ("carto-packed-attributes-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        const auto path = root / "packed.carto";
        require(bool(application::HumanApplicationAccess::dispatch(
            session, application::SaveProjectAction{path})), "saved");
        const auto text = read_text(path);
        require(text.find("ATTRIBUTE_PACKED f32") != std::string::npos, "float32-exact layer packs as f32");
        require(text.find("ATTRIBUTE_PACKED f64") != std::string::npos, "other layer packs as f64");
        require(text.find("ATTRIBUTE_VALUE ") != std::string::npos, "small layer keeps per-value text");

        auto reopened = project::ProjectDocument::load(path);
        require(bool(reopened), "reopened");
        require(reopened.value().serialize() == text, "save is a fixed point after reload");
        const auto& reopened_mesh = reopened.value().meshes().begin()->second;
        const auto actual_faces = reopened_mesh.faces_sorted();
        require(actual_faces.size() == expected_faces.size(), "face count retained");
        for (std::size_t i=0; i<actual_faces.size(); ++i) {
            require(actual_faces[i].id == expected_faces[i].id &&
                    actual_faces[i].vertices == expected_faces[i].vertices, "ordered face identity retained");
        }
        const auto actual_corners = reopened_mesh.topology().value().corners;
        require(actual_corners.size() == expected_corners.size(), "corner count retained");
        for (std::size_t i=0; i<actual_corners.size(); ++i)
            require(actual_corners[i].face == expected_corners[i].face &&
                    actual_corners[i].vertex == expected_corners[i].vertex, "ordered corner binding retained");
        const auto& after = reopened_mesh.attributes();
        const std::vector<std::pair<const char*, const std::vector<attributes::AttributeValue>*>> expected{
            {"test.float", &floats}, {"test.normal", &normals}, {"test.uv", &uvs}, {"test.weight", &weights}};
        for (const auto& [name, values] : expected) {
            const auto* b = after.find(name);
            require(b && b->values.size() == values->size(), "layer survives");
            attributes::AttributeLayer a;
            a.values = *values;
            require(scalar_bits(a) == scalar_bits(*b), "reloaded scalars are bit-identical");
        }

        // Legacy per-value form of a large layer must still parse.
        {
            std::string legacy = text;
            const auto at = legacy.find("ATTRIBUTE_PACKED f32", legacy.find("\"test.normal\""));
            const auto end = legacy.find('\n', at);
            std::ostringstream expanded;
            expanded.precision(17);
            const auto* layer = after.find("test.normal");
            for (const auto& value : layer->values) {
                const auto& v = std::get<core::Vec3d>(value);
                expanded << "ATTRIBUTE_VALUE " << v.x << ' ' << v.y << ' ' << v.z << '\n';
            }
            legacy.replace(at, end - at + 1U, expanded.str());
            const auto legacy_path = root / "legacy.carto";
            { std::ofstream out(legacy_path, std::ios::binary); out << legacy; }
            auto legacy_doc = project::ProjectDocument::load(legacy_path);
            require(bool(legacy_doc), "legacy per-value layer parses");
            require(scalar_bits(*legacy_doc.value().meshes().begin()->second.attributes().find("test.normal")) ==
                    scalar_bits(*layer), "legacy layer values match");
        }
        // Replace the packed float32 layer with adversarial finite f64 data.
        const auto float_layer = text.find("\"test.float\"");
        const auto float_record = text.find("ATTRIBUTE_PACKED", float_layer);
        const auto float_end = text.find('\n', float_record);
        const auto b64 = [](const std::vector<unsigned char>& bytes) {
            constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string result;
            for (std::size_t i=0; i<bytes.size(); i+=3) {
                unsigned v=unsigned(bytes[i])<<16;
                if(i+1<bytes.size()) v|=unsigned(bytes[i+1])<<8;
                if(i+2<bytes.size()) v|=bytes[i+2];
                result+=alphabet[(v>>18)&63]; result+=alphabet[(v>>12)&63];
                result+=i+1<bytes.size()?alphabet[(v>>6)&63]:'=';
                result+=i+2<bytes.size()?alphabet[v&63]:'=';
            }
            return result;
        };
        for (double bad_value : {1.0e300, 0.1, std::numeric_limits<double>::infinity()}) {
            std::vector<unsigned char> bytes(corners*8U, 0);
            auto bits=std::bit_cast<std::uint64_t>(bad_value);
            for(unsigned i=0;i<8;++i) bytes[i]=static_cast<unsigned char>(bits>>(i*8));
            auto bad=text;
            bad.replace(float_record,float_end-float_record,
                "ATTRIBUTE_PACKED f64 "+std::to_string(corners)+" "+b64(bytes));
            require(!project::ProjectDocument::deserialize(bad), "nonfinite, overflow and lossy float32 rejected");
        }
        // Canonical base64 padding: a valid small packed f64 layer is readable,
        // but alternate encodings with nonzero unused low bits are rejected.
        const auto weight_layer=text.find("\"test.weight\"");
        const auto weight_start=text.find('\n',weight_layer)+1U;
        auto weight_end=weight_start;
        for(std::size_t i=0;i<weights.size();++i) weight_end=text.find('\n',weight_end)+1U;
        std::vector<unsigned char> weight_bytes(weights.size()*8U);
        for(std::size_t i=0;i<weights.size();++i) {
            auto bits=std::bit_cast<std::uint64_t>(std::get<double>(weights[i]));
            for(unsigned j=0;j<8;++j) weight_bytes[i*8+j]=static_cast<unsigned char>(bits>>(j*8));
        }
        auto encoded_weights=b64(weight_bytes);
        auto packed_weights=text;
        const auto prefix="ATTRIBUTE_PACKED f64 "+std::to_string(weights.size())+" ";
        packed_weights.replace(weight_start,weight_end-weight_start,prefix+encoded_weights+"\n");
        require(bool(project::ProjectDocument::deserialize(packed_weights)), "small packed layer accepted");
        require(encoded_weights.ends_with("=="), "padding fixture has two pad bytes");
        const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const auto last=encoded_weights.size()-3U;
        encoded_weights[last]=alphabet[alphabet.find(encoded_weights[last])+1U];
        auto bad_padding=text;
        bad_padding.replace(weight_start,weight_end-weight_start,prefix+encoded_weights+"\n");
        require(!project::ProjectDocument::deserialize(bad_padding), "noncanonical base64 pad bits rejected");
        // True legacy schema-40 text with every packed record expanded.
        auto legacy_all = text;
        for (const auto& [name, ignored] : expected) {
            static_cast<void>(ignored);
            const auto layer_at=legacy_all.find(std::string("\"")+name+"\"");
            const auto record=legacy_all.find('\n',layer_at)+1U;
            if (legacy_all.compare(record,16,"ATTRIBUTE_PACKED") != 0) continue;
            const auto end=legacy_all.find('\n',record);
            std::ostringstream expanded; expanded.precision(17);
            for (const auto& v : after.find(name)->values) {
                expanded<<"ATTRIBUTE_VALUE ";
                if(auto f=std::get_if<float>(&v)) expanded<<*f;
                else if(auto d=std::get_if<double>(&v)) expanded<<*d;
                else if(auto v2=std::get_if<core::Vec2d>(&v)) expanded<<v2->x<<' '<<v2->y;
                else if(auto v3=std::get_if<core::Vec3d>(&v)) expanded<<v3->x<<' '<<v3->y<<' '<<v3->z;
                expanded<<'\n';
            }
            legacy_all.replace(record,end-record+1,expanded.str());
        }
        auto legacy_header=legacy_all.find('\n');
        legacy_all.replace(legacy_all.rfind(' ',legacy_header),legacy_header-legacy_all.rfind(' ',legacy_header)," 40");
        auto legacy_doc=project::ProjectDocument::deserialize(legacy_all);
        require(bool(legacy_doc), "legacy schema40 per-value project readable");
        for(const auto& [name, values] : expected)
            require(scalar_bits(*legacy_doc.value().meshes().begin()->second.attributes().find(name)) ==
                    scalar_bits(*after.find(name)), "legacy ordered layer values preserved");
        // A schema-40 reader must not be tricked into reading the new token.
        auto downgrade = text;
        const auto header_end = downgrade.find('\n');
        downgrade.replace(downgrade.rfind(' ', header_end), header_end - downgrade.rfind(' ', header_end), " 40");
        require(!project::ProjectDocument::deserialize(downgrade), "packed token forbidden in legacy schema");
        const auto packed_at = text.find("ATTRIBUTE_PACKED f32", text.find("\"test.normal\""));
        const auto packed_end = text.find('\n', packed_at);
        const auto payload_at = text.find(' ', text.find(' ', packed_at) + 1U) + 1U;
        const auto data_at = text.find(' ', payload_at) + 1U;
        const auto rejects = [&](std::string replacement) {
            auto bad = text;
            bad.replace(packed_at, packed_end - packed_at, replacement);
            require(!project::ProjectDocument::deserialize(bad), "malformed packed data rejected");
        };
        rejects("ATTRIBUTE_PACKED f32 999999999999999999 A");
        rejects("ATTRIBUTE_PACKED f16 28800 A");
        rejects(text.substr(packed_at, packed_end-packed_at-4U));
        rejects(text.substr(packed_at, packed_end-packed_at)+"AAAA");
        auto invalid = text.substr(packed_at, packed_end-packed_at);
        invalid[data_at-packed_at] = '!'; rejects(invalid);
        // Change declared counts around the four-values-per-face admission cap.
        auto oversized = text;
        const auto layer_line = oversized.rfind("\nLAYER \"test.normal\" ", packed_at);
        require(layer_line != std::string::npos && layer_line < packed_at,
                "oversized fixture locates exact normal LAYER header");
        const auto line_end = oversized.find('\n', layer_line + 1U);
        require(line_end != std::string::npos && line_end + 1U == packed_at,
                "normal layer header immediately precedes targeted packed record");
        const auto count_at = oversized.rfind(' ', line_end);
        require(count_at != std::string::npos && count_at > layer_line && count_at < line_end,
                "normal layer value count offset is valid");
        require(oversized.substr(count_at + 1U, line_end - count_at - 1U) == std::to_string(corners),
                "normal layer original count equals fixture corner count");
        // A corner layer past one million values is within the admission cap
        // (four values per face for the largest admitted mesh): this count is
        // rejected only because it no longer matches the fixture's corners.
        auto corner_scale = oversized;
        corner_scale.replace(count_at+1U, line_end-count_at-1U, "1000001");
        const auto corner_scale_result = project::ProjectDocument::deserialize(corner_scale);
        require(!corner_scale_result, "mismatched corner layer count rejected");
        require(corner_scale_result.error().message.find("exceeds the Cartographer v1 limit") == std::string::npos,
                "corner layer over one million values passes the value-count admission cap");
        oversized.replace(count_at+1U, line_end-count_at-1U, "4000001");
        const auto oversized_result = project::ProjectDocument::deserialize(oversized);
        require(!oversized_result, "oversized layer rejected");
        require(oversized_result.error().message.find(
                    "mesh attribute value count exceeds the Cartographer v1 limit of 4000000") != std::string::npos,
                "oversized layer fails specifically at the value-count admission cap");
        std::filesystem::remove_all(root);
        std::cout << "PASS packed attribute layers round-trip bit-exactly\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
