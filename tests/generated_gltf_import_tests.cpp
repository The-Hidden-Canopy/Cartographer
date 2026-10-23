#include <carto/io/generated_mesh.hpp>

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void overwrite_u32(std::vector<std::uint8_t>& out, std::size_t offset,
                   std::uint32_t value) {
    require(offset <= out.size() && out.size() - offset >= 4U,
        "fixture u32 patch is in bounds");
    for (std::uint32_t byte = 0U; byte < 4U; ++byte) {
        out[offset + byte] = static_cast<std::uint8_t>(
            (value >> (byte * 8U)) & 0xffU);
    }
}

void append_float(std::vector<std::uint8_t>& out, float value) {
    append_u32(out, std::bit_cast<std::uint32_t>(value));
}

std::string base64(const std::vector<std::uint8_t>& bytes) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t index = 0U; index < bytes.size(); index += 3U) {
        const std::uint32_t a = bytes[index];
        const std::uint32_t b = index + 1U < bytes.size() ? bytes[index + 1U] : 0U;
        const std::uint32_t c = index + 2U < bytes.size() ? bytes[index + 2U] : 0U;
        const std::uint32_t block = (a << 16U) | (b << 8U) | c;
        out.push_back(alphabet[(block >> 18U) & 63U]);
        out.push_back(alphabet[(block >> 12U) & 63U]);
        out.push_back(index + 1U < bytes.size() ? alphabet[(block >> 6U) & 63U] : '=');
        out.push_back(index + 2U < bytes.size() ? alphabet[block & 63U] : '=');
    }
    return out;
}

std::vector<std::uint8_t> binary_fixture(bool invalid_index = false) {
    std::vector<std::uint8_t> bytes;
    const auto triangle = [&bytes](float x) {
        append_float(bytes, x); append_float(bytes, 0.0F); append_float(bytes, 0.0F);
        append_float(bytes, x + 1.0F); append_float(bytes, 0.0F); append_float(bytes, 0.0F);
        append_float(bytes, x); append_float(bytes, 1.0F); append_float(bytes, 0.0F);
    };
    triangle(0.0F);
    for (int vertex = 0; vertex < 3; ++vertex) {
        append_float(bytes, 0.0F); append_float(bytes, 0.0F); append_float(bytes, 1.0F);
    }
    append_u32(bytes, 0U); append_u32(bytes, 1U);
    append_u32(bytes, invalid_index ? 3U : 2U);
    triangle(2.0F);
    for (int vertex = 0; vertex < 3; ++vertex) {
        append_float(bytes, 0.0F); append_float(bytes, 0.0F); append_float(bytes, 1.0F);
    }
    append_u32(bytes, 0U); append_u32(bytes, 1U); append_u32(bytes, 2U);
    append_float(bytes, 0.0F); append_float(bytes, 0.0F);
    append_float(bytes, 1.0F); append_float(bytes, 0.0F);
    append_float(bytes, 0.0F); append_float(bytes, 1.0F);
    append_float(bytes, 0.25F); append_float(bytes, 0.25F);
    append_float(bytes, 0.75F); append_float(bytes, 0.25F);
    append_float(bytes, 0.5F); append_float(bytes, 0.75F);
    return bytes;
}

std::string make_gltf(std::string buffer_data, std::size_t buffer_size,
    std::string scene_node = "{\"mesh\":0}", bool include_uvs = true,
    std::size_t uv_count = 3U, bool second_primitive_has_uvs = true) {
    const std::string first_uv_attribute = include_uvs ? ",\"TEXCOORD_0\":6" : "";
    const std::string second_uv_attribute =
        include_uvs && second_primitive_has_uvs ? ",\"TEXCOORD_0\":7" : "";
    return "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Cartographer\"},"
        "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[" + scene_node + "],"
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1" + first_uv_attribute +
        "},\"indices\":2,\"material\":0,\"mode\":4},"
        "{\"attributes\":{\"POSITION\":3,\"NORMAL\":4" + second_uv_attribute +
        "},\"indices\":5,\"material\":1,\"mode\":4}]}],"
        "\"materials\":[{\"name\":\"Stem\"},{\"name\":\"Foliage\"}],"
        "\"buffers\":[{\"byteLength\":" + std::to_string(buffer_size) +
        ",\"uri\":\"data:application/octet-stream;base64," + buffer_data + "\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":12,\"target\":34963},"
        "{\"buffer\":0,\"byteOffset\":84,\"byteLength\":36,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":120,\"byteLength\":36,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":156,\"byteLength\":12,\"target\":34963},"
        "{\"buffer\":0,\"byteOffset\":168,\"byteLength\":24,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":192,\"byteLength\":24,\"target\":34962}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"byteOffset\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"byteOffset\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"byteOffset\":0,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"},"
        "{\"bufferView\":3,\"byteOffset\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":4,\"byteOffset\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":5,\"byteOffset\":0,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"},"
        "{\"bufferView\":6,\"byteOffset\":0,\"componentType\":5126,\"count\":" +
        std::to_string(uv_count) + ",\"type\":\"VEC2\"},"
        "{\"bufferView\":7,\"byteOffset\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"extras\":{\"cartographer\":{\"sourceIdentity\":\"pine-fixture\","
        "\"sourceRevision\":\"7\"}}}";
}

std::vector<std::uint8_t> make_glb(
    const std::vector<std::uint8_t>& embedded_data,
    std::size_t declared_buffer_length,
    std::vector<std::uint8_t> binary_chunk = {}) {
    constexpr std::uint32_t kGlbMagic = 0x46546c67U;
    constexpr std::uint32_t kJsonChunkType = 0x4e4f534aU;
    constexpr std::uint32_t kBinChunkType = 0x004e4942U;

    std::string json = make_gltf(base64(embedded_data), declared_buffer_length);
    const std::string uri_marker =
        ",\"uri\":\"data:application/octet-stream;base64,";
    const std::size_t uri_start = json.find(uri_marker);
    require(uri_start != std::string::npos, "GLB fixture JSON includes an embedded buffer URI");
    const std::size_t uri_end = json.find('"', uri_start + uri_marker.size());
    require(uri_end != std::string::npos, "GLB fixture data URI closes");
    json.erase(uri_start, uri_end - uri_start + 1U);
    while ((json.size() & 3U) != 0U) json.push_back(' ');

    if (binary_chunk.empty()) binary_chunk = embedded_data;
    require((binary_chunk.size() & 3U) == 0U,
        "GLB fixture BIN chunk is four-byte aligned");
    const std::size_t total_length = 12U + 8U + json.size() + 8U + binary_chunk.size();
    require(total_length <= std::numeric_limits<std::uint32_t>::max(),
        "GLB fixture total length fits the container header");
    std::vector<std::uint8_t> glb;
    glb.reserve(total_length);
    append_u32(glb, kGlbMagic);
    append_u32(glb, 2U);
    append_u32(glb, static_cast<std::uint32_t>(total_length));
    append_u32(glb, static_cast<std::uint32_t>(json.size()));
    append_u32(glb, kJsonChunkType);
    glb.insert(glb.end(), json.begin(), json.end());
    append_u32(glb, static_cast<std::uint32_t>(binary_chunk.size()));
    append_u32(glb, kBinChunkType);
    glb.insert(glb.end(), binary_chunk.begin(), binary_chunk.end());
    return glb;
}

std::vector<std::uint8_t> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

void admits_exact_bounded_mesh_and_material_regions() {
    const auto buffer = binary_fixture();
    const auto json = make_gltf(base64(buffer), buffer.size());
    const auto payload = bytes(json);
    const auto digest = carto::assets::sha256(payload);
    const auto imported = carto::io::import_generated_botanical_gltf(
        payload, "pine-fixture", carto::core::Revision{7U}, digest);
    require(static_cast<bool>(imported), "valid job-bound glTF is admitted");
    require(imported.value().mesh.vertex_count() == 6U &&
        imported.value().mesh.face_count() == 2U &&
        imported.value().source_identity == "pine-fixture" &&
        imported.value().source_revision == carto::core::Revision{7U} &&
        imported.value().artifact_digest == digest,
        "import retains exact source and artifact receipts");
    const auto* slots = imported.value().mesh.attributes().find("gltf.material_index");
    require(slots != nullptr && slots->values.size() == 2U &&
        std::get<std::uint32_t>(slots->values[0]) == 0U &&
        std::get<std::uint32_t>(slots->values[1]) == 1U,
        "per-face primitive material identity is preserved");
    const auto* uvs = imported.value().mesh.attributes().find("cartographer.uv0");
    require(uvs != nullptr && uvs->values.size() == 6U &&
        std::get<carto::core::Vec2d>(uvs->values[0]).x == 0.0 &&
        std::get<carto::core::Vec2d>(uvs->values[1]).x == 1.0 &&
        std::get<carto::core::Vec2d>(uvs->values[5]).y == 0.75,
        "corner-domain botanical UVs survive topology admission");
    const auto* active_uv = imported.value().mesh.uv_sets().active_for_render();
    require(active_uv != nullptr && active_uv->layer_id == "cartographer.uv0",
        "imported botanical UV set is active for rendering");

    const auto legacy_payload = bytes(make_gltf(base64(buffer), buffer.size(),
        "{\"mesh\":0}", false));
    const auto legacy = carto::io::import_generated_botanical_gltf(
        legacy_payload, "pine-fixture", carto::core::Revision{7U},
        carto::assets::sha256(legacy_payload));
    require(static_cast<bool>(legacy) &&
        legacy.value().mesh.attributes().find("cartographer.uv0") == nullptr,
        "legacy generated glTF without UVs remains importable");
}

void rejects_stale_tampered_or_unbounded_data() {
    const auto buffer = binary_fixture();
    const auto payload = bytes(make_gltf(base64(buffer), buffer.size()));
    const auto digest = carto::assets::sha256(payload);
    require(!carto::io::import_generated_botanical_gltf(payload, "other-source",
        carto::core::Revision{7U}, digest), "different source identity is rejected");
    require(!carto::io::import_generated_botanical_gltf(payload, "pine-fixture",
        carto::core::Revision{8U}, digest), "different source revision is rejected");
    auto changed_digest = digest;
    changed_digest.bytes[0] ^= 0xffU;
    require(!carto::io::import_generated_botanical_gltf(payload, "pine-fixture",
        carto::core::Revision{7U}, changed_digest), "wrong job digest is rejected");

    const auto bad_index_buffer = binary_fixture(true);
    const auto bad_index = bytes(make_gltf(base64(bad_index_buffer), bad_index_buffer.size()));
    require(!carto::io::import_generated_botanical_gltf(bad_index, "pine-fixture",
        carto::core::Revision{7U}, carto::assets::sha256(bad_index)),
        "out-of-range primitive indices are rejected");

    const auto mixed_uv_payload = bytes(make_gltf(base64(buffer), buffer.size(),
        "{\"mesh\":0}", true, 3U, false));
    require(!carto::io::import_generated_botanical_gltf(mixed_uv_payload, "pine-fixture",
        carto::core::Revision{7U}, carto::assets::sha256(mixed_uv_payload)),
        "mixed UV presence across primitives is rejected rather than partially retained");

    const auto wrong_uv_count = bytes(make_gltf(base64(buffer), buffer.size(),
        "{\"mesh\":0}", true, 2U));
    require(!carto::io::import_generated_botanical_gltf(wrong_uv_count, "pine-fixture",
        carto::core::Revision{7U}, carto::assets::sha256(wrong_uv_count)),
        "UV accessor cardinality mismatch is rejected");

    auto nonfinite_uv_buffer = binary_fixture();
    nonfinite_uv_buffer[168U] = 0U;
    nonfinite_uv_buffer[169U] = 0U;
    nonfinite_uv_buffer[170U] = 128U;
    nonfinite_uv_buffer[171U] = 127U;
    const auto nonfinite_uv = bytes(make_gltf(
        base64(nonfinite_uv_buffer), nonfinite_uv_buffer.size()));
    require(!carto::io::import_generated_botanical_gltf(nonfinite_uv, "pine-fixture",
        carto::core::Revision{7U}, carto::assets::sha256(nonfinite_uv)),
        "non-finite UV values are rejected before mesh mutation");

    const auto transformed = bytes(make_gltf(base64(buffer), buffer.size(),
        "{\"mesh\":0,\"translation\":[1,0,0]}"));
    require(!carto::io::import_generated_botanical_gltf(transformed, "pine-fixture",
        carto::core::Revision{7U}, carto::assets::sha256(transformed)),
        "ignored node transforms are rejected rather than silently discarded");

    std::string wrong_uri = make_gltf("not-an-embedded-buffer", buffer.size());
    const auto uri_pos = wrong_uri.find("data:application/octet-stream;base64,");
    require(uri_pos != std::string::npos, "fixture includes a data URI");
    wrong_uri.replace(uri_pos, std::string("data:application/octet-stream;base64,").size(),
        "https://outside.example/mesh.bin");
    const auto external = bytes(wrong_uri);
    require(!carto::io::import_generated_botanical_gltf(external, "pine-fixture",
        carto::core::Revision{7U}, carto::assets::sha256(external)),
        "external buffer URLs are rejected");

    std::string duplicate_revision = make_gltf(base64(buffer), buffer.size());
    const auto revision_pos = duplicate_revision.find("\"sourceRevision\":\"7\"");
    require(revision_pos != std::string::npos, "fixture includes a source revision");
    duplicate_revision.insert(revision_pos + std::string("\"sourceRevision\":\"7\"").size(),
        ",\"sourceRevision\":\"8\"");
    const auto duplicate_metadata = bytes(duplicate_revision);
    require(!carto::io::import_generated_botanical_gltf(duplicate_metadata, "pine-fixture",
        carto::core::Revision{7U}, carto::assets::sha256(duplicate_metadata)),
        "duplicate provenance keys are rejected rather than parser-order dependent");

    std::string padded_view = make_gltf(base64(buffer), buffer.size());
    const auto view_offset = padded_view.find("\"byteOffset\":36");
    require(view_offset != std::string::npos, "fixture includes a nonzero view offset");
    padded_view.replace(view_offset, std::string("\"byteOffset\":36").size(),
        "\"byteOffset\":35");
    const auto misaligned = bytes(padded_view);
    require(!carto::io::import_generated_botanical_gltf(misaligned, "pine-fixture",
        carto::core::Revision{7U}, carto::assets::sha256(misaligned)),
        "misaligned binary attribute view is rejected");
}

void admits_and_rejects_glb_container_boundaries() {
    const auto buffer = binary_fixture();
    const auto valid_glb = make_glb(buffer, buffer.size());
    const auto digest = carto::assets::sha256(valid_glb);
    const auto imported = carto::io::import_generated_botanical_glb(
        valid_glb, "pine-fixture", carto::core::Revision{7U}, digest);
    require(static_cast<bool>(imported), "valid GLB JSON and BIN chunks are admitted");
    require(imported.value().mesh.vertex_count() == 6U &&
            imported.value().mesh.face_count() == 2U &&
            imported.value().source_identity == "pine-fixture" &&
            imported.value().source_revision == carto::core::Revision{7U} &&
            imported.value().artifact_digest == digest,
        "GLB import preserves exact source revision and whole-container digest");

    std::vector<std::uint8_t> padded_chunk = buffer;
    padded_chunk.resize(buffer.size() + 4U, 0U);
    const auto valid_padding_glb = make_glb(buffer, buffer.size() + 1U, padded_chunk);
    require(static_cast<bool>(carto::io::import_generated_botanical_glb(
                valid_padding_glb, "pine-fixture", carto::core::Revision{7U},
                carto::assets::sha256(valid_padding_glb))),
        "zero BIN alignment padding of up to three bytes is admitted");

    const auto rejects_with_fresh_digest = [](std::vector<std::uint8_t> candidate,
                                               const char* message) {
        require(!carto::io::import_generated_botanical_glb(
                    candidate, "pine-fixture", carto::core::Revision{7U},
                    carto::assets::sha256(candidate)), message);
    };
    auto bad_header = valid_glb;
    bad_header[0U] ^= 0xffU;
    rejects_with_fresh_digest(std::move(bad_header),
        "malformed GLB magic is rejected after digest admission");

    auto wrong_version = valid_glb;
    overwrite_u32(wrong_version, 4U, 1U);
    rejects_with_fresh_digest(std::move(wrong_version),
        "unsupported GLB version is rejected after digest admission");

    auto wrong_total_length = valid_glb;
    overwrite_u32(wrong_total_length, 8U,
        static_cast<std::uint32_t>(valid_glb.size() + 4U));
    rejects_with_fresh_digest(std::move(wrong_total_length),
        "GLB total length mismatch is rejected");

    auto wrong_chunk_order = valid_glb;
    overwrite_u32(wrong_chunk_order, 16U, 0x004e4942U);
    rejects_with_fresh_digest(std::move(wrong_chunk_order),
        "BIN-before-JSON chunk order is rejected");

    auto extra_chunk = valid_glb;
    append_u32(extra_chunk, 4U);
    append_u32(extra_chunk, 0x12345678U);
    append_u32(extra_chunk, 0xfeedbeefU);
    overwrite_u32(extra_chunk, 8U, static_cast<std::uint32_t>(extra_chunk.size()));
    rejects_with_fresh_digest(std::move(extra_chunk),
        "unsupported trailing GLB chunk is rejected");

    auto bad_bin_padding = padded_chunk;
    bad_bin_padding[buffer.size() + 1U] = 0x7fU;
    const auto malformed_padding_glb = make_glb(
        buffer, buffer.size() + 1U, std::move(bad_bin_padding));
    rejects_with_fresh_digest(std::move(malformed_padding_glb),
        "non-zero BIN alignment padding is rejected");

    auto wrong_digest = digest;
    wrong_digest.bytes[0U] ^= 0x80U;
    require(!carto::io::import_generated_botanical_glb(
                valid_glb, "pine-fixture", carto::core::Revision{7U}, wrong_digest),
        "GLB digest mismatch is rejected");
    require(!carto::io::import_generated_botanical_glb(
                valid_glb, "other-source", carto::core::Revision{7U}, digest),
        "GLB source identity mismatch is rejected");
    require(!carto::io::import_generated_botanical_glb(
                valid_glb, "pine-fixture", carto::core::Revision{8U}, digest),
        "GLB source revision mismatch is rejected");
}

void admits_actual_botanical_output_when_configured() {
    const char* artifact_path = std::getenv("CARTOGRAPHER_GENERATED_BOTANICAL_TEST_ARTIFACT");
    if (artifact_path == nullptr || *artifact_path == '\0') return;
    std::ifstream input(artifact_path, std::ios::binary);
    require(static_cast<bool>(input), "configured botanical artifact is readable");
    const std::string text(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{});
    const std::vector<std::uint8_t> payload(text.begin(), text.end());
    const auto digest = carto::assets::sha256(payload);
    const auto imported = carto::io::import_generated_botanical_gltf(
        payload, "botanical-fixture", carto::core::Revision{7U}, digest);
    require(static_cast<bool>(imported),
        "public importer admits the actual digest-bound botanical artifact");
    const auto* uvs = imported.value().mesh.attributes().find("cartographer.uv0");
    const auto* slots = imported.value().mesh.attributes().find("gltf.material_index");
    const auto topology = imported.value().mesh.topology();
    require(uvs != nullptr && slots != nullptr &&
        topology && uvs->values.size() == topology.value().corners.size() &&
        slots->values.size() == imported.value().mesh.face_count(),
        "actual botanical output retains its corner UVs and per-face material roles");
    const auto* active_uv = imported.value().mesh.uv_sets().active_for_render();
    require(active_uv != nullptr && active_uv->layer_id == "cartographer.uv0",
        "actual botanical output retains an active render UV set");
    for (const auto& value : uvs->values) {
        const auto* uv = std::get_if<carto::core::Vec2d>(&value);
        require(uv != nullptr && uv->x >= 0.0 && uv->x <= 1.0 &&
                uv->y >= 0.0 && uv->y <= 1.0,
            "actual botanical output UVs are finite and normalized");
    }
    bool has_stem = false;
    bool has_foliage = false;
    for (const auto& value : slots->values) {
        const auto* material = std::get_if<std::uint32_t>(&value);
        require(material != nullptr && *material <= 1U,
            "actual botanical output material index stays within declared roles");
        has_stem = has_stem || *material == 0U;
        has_foliage = has_foliage || *material == 1U;
    }
    require(has_stem && has_foliage,
        "actual botanical output preserves both stem and foliage material roles");
}

} // namespace

int main() {
    admits_exact_bounded_mesh_and_material_regions();
    rejects_stale_tampered_or_unbounded_data();
    admits_and_rejects_glb_container_boundaries();
    admits_actual_botanical_output_when_configured();
    std::cout << "generated glTF import tests passed\n";
    return 0;
}
