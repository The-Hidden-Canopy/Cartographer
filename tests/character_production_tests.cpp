// Character production E2E: compose -> manifest closure -> publish ->
// tampered-product refusal -> rollback to last-good -> license-defective
// refusal -> manifest digest determinism.
//
// Release level: structural verification on CPU-reference evidence. The
// VANTA-side decoder and runtime admission live in another repository; these
// tests pin the producer contract and the golden fixture bytes that decoder
// implements against.

#include <carto/assets/blob_store.hpp>
#include <carto/character/coordinate.hpp>
#include <carto/character/definition.hpp>
#include <carto/character/manifest.hpp>
#include <carto/character/morph.hpp>
#include <carto/character/motion_document.hpp>
#include <carto/character/receipt.hpp>
#include <carto/character/weights_record.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/lowering.hpp>
#include <carto/kinematic/skin.hpp>
#include <carto/production/prepared_character.hpp>
#include <carto/production/prepared_character_publication.hpp>
#include <carto/production/prepared_publication.hpp>
#include <carto/production/prepared_scene.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

carto::assets::Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

std::vector<std::uint8_t> bytes_of(std::string_view value) {
    return std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(value.data()),
        reinterpret_cast<const std::uint8_t*>(value.data()) + value.size());
}

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-character-production-tests-" + std::to_string(stamp));
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

// The golden fixture: a capsule-style test mesh (8 vertices) skinned to a
// 2-joint chain, one morph target, one clip, one garment cache descriptor.
// All source content is digest-bound and deterministic.
struct GoldenFixture {
    carto::assets::Sha256Digest mesh_digest;
    carto::assets::Sha256Digest graph_digest;
    carto::assets::Sha256Digest skin_digest;
    carto::assets::Sha256Digest clip_digest;
    carto::assets::Sha256Digest garment_source_digest;
    carto::kinematic::KinematicGraph graph;
    carto::kinematic::SkinBinding binding;
    carto::kinematic::SkeletalAssetDescriptor skeleton;
    carto::character::CharacterDefinition definition;
    std::vector<std::uint8_t> morph_blob;
    carto::assets::Sha256Digest morph_blob_digest;
};

carto::core::Result<GoldenFixture> make_golden_fixture() {
    GoldenFixture fixture;

    // 8-vertex capsule proxy: 4 lower ring vertices weight to joint 1, 4
    // upper ring vertices weight to joint 2.
    fixture.mesh_digest = digest(
        "golden-capsule-mesh v1\n"
        "VERTICES 8\n"
        "v 0 -0.5 -0.5\nv 0 -0.5 0.5\nv 0 0.5 0.5\nv 0 0.5 -0.5\n"
        "v 0 -0.5 -0.5\nv 0 -0.5 0.5\nv 0 0.5 0.5\nv 0 0.5 -0.5\n"
        "TRIANGLES 12\n");
    fixture.garment_source_digest = digest("golden-garment-cache v1");
    fixture.clip_digest = digest("golden-clip wave v1");

    auto& graph = fixture.graph;
    if (auto result = graph.insert_node(carto::kinematic::KinematicNode{
            carto::kinematic::JointId{1U}, std::nullopt,
            carto::core::Transform::identity(), carto::kinematic::JointKind::ball,
            carto::kinematic::NodeClass::skinned, "capsule.root"});
        !result) {
        return carto::core::Result<GoldenFixture>::failure(result.error());
    }
    carto::core::Transform child_rest = carto::core::Transform::identity();
    child_rest.translation = {0.0, 1.0, 0.0};
    if (auto result = graph.insert_node(carto::kinematic::KinematicNode{
            carto::kinematic::JointId{2U}, carto::kinematic::JointId{1U},
            child_rest, carto::kinematic::JointKind::hinge,
            carto::kinematic::NodeClass::skinned, "capsule.limb"});
        !result) {
        return carto::core::Result<GoldenFixture>::failure(result.error());
    }
    if (auto result = graph.validate(); !result) {
        return carto::core::Result<GoldenFixture>::failure(result.error());
    }
    fixture.graph_digest = digest(graph.serialize());

    auto& binding = fixture.binding;
    binding.mesh_asset = 42U;
    binding.mesh_digest = fixture.mesh_digest;
    binding.graph_digest = fixture.graph_digest;
    for (std::uint32_t vertex = 0U; vertex < 8U; ++vertex) {
        const std::uint64_t joint = vertex < 4U ? 1U : 2U;
        binding.influences.emplace(vertex,
            std::vector<carto::kinematic::JointInfluence>{
                carto::kinematic::JointInfluence{carto::kinematic::JointId{joint}, 1.0}});
    }
    if (auto result = binding.validate(); !result) {
        return carto::core::Result<GoldenFixture>::failure(result.error());
    }
    fixture.skin_digest = digest(binding.serialize());

    auto& skeleton = fixture.skeleton;
    skeleton.mesh_asset = 42U;
    skeleton.graph_digest = fixture.graph_digest;
    skeleton.skin_digest = fixture.skin_digest;
    skeleton.joints.emplace(carto::kinematic::JointId{1U},
        carto::kinematic::SkeletalJointDescriptor{
            carto::kinematic::JointId{1U}, std::nullopt,
            carto::core::Transform::identity(), carto::kinematic::JointKind::ball,
            true});
    skeleton.joints.emplace(carto::kinematic::JointId{2U},
        carto::kinematic::SkeletalJointDescriptor{
            carto::kinematic::JointId{2U}, carto::kinematic::JointId{1U},
            child_rest, carto::kinematic::JointKind::hinge,
            true});
    if (auto result = skeleton.validate(); !result) {
        return carto::core::Result<GoldenFixture>::failure(result.error());
    }

    auto& definition = fixture.definition;
    definition.id = carto::character::CharacterId{7U};
    definition.project_namespace = "carto.test.character";
    definition.source_revision = carto::core::Revision(3U);
    definition.source_digest = digest("golden project snapshot r3");
    definition.root = carto::scene::ObjectId{1U};
    definition.mesh_refs.push_back(
        carto::character::MeshAssetRef{42U, fixture.mesh_digest});
    definition.rig = carto::character::RigRef{
        carto::character::RigId{9U}, fixture.graph_digest};
    definition.control_graph = carto::character::ControlGraphRef{
        carto::character::RigId{9U}, digest("golden control graph v1")};
    definition.coordinate_policy_digest = digest("golden coordinate policy v1");
    if (auto result = definition.validate(); !result) {
        return carto::core::Result<GoldenFixture>::failure(result.error());
    }

    // Morph delta blob: two displaced points, three f64 components each.
    const double deltas[] = {0.01, 0.0, 0.0, 0.0, 0.02, 0.0};
    fixture.morph_blob.resize(sizeof(deltas));
    std::memcpy(fixture.morph_blob.data(), deltas, sizeof(deltas));
    fixture.morph_blob_digest = carto::assets::sha256(fixture.morph_blob);

    return carto::core::Result<GoldenFixture>::success(std::move(fixture));
}

carto::character::SkinWeightLayer fresh_weight_layer(
    const GoldenFixture& fixture,
    carto::character::RigLayerId layer_id) {
    carto::character::SkinWeightLayer layer;
    layer.layer_id = layer_id;
    layer.mesh_topology_digest = digest("golden capsule topology v1");
    layer.joint_graph_digest = fixture.graph_digest;
    layer.influenced_vertex_count = 8U;
    layer.total_influence_count = 8U;
    layer.provenance = carto::character::WeightProvenance::authored_paint;
    layer.provenance_digest = digest("golden weight paint session v1");
    layer.staleness = carto::character::WeightStaleness::fresh;
    return layer;
}

carto::production::CharacterProductLicenseGrant grant_for(
    std::string product_identity,
    carto::assets::Sha256Digest source_digest,
    carto::production::CharacterLicenseOrigin origin =
        carto::production::CharacterLicenseOrigin::internal_new,
    carto::production::CharacterRedistribution redistribution =
        carto::production::CharacterRedistribution::private_only) {
    carto::production::CharacterProductLicenseGrant grant;
    grant.product_identity = std::move(product_identity);
    grant.origin = origin;
    grant.redistribution = redistribution;
    grant.commercial_use = carto::production::CharacterCommercialUse::not_cleared;
    grant.rights_holder = "Hidden Canopy Test Fixture";
    grant.source_content_digest = source_digest;
    return grant;
}

// Builds the full golden input. `revision`, `clip_digest` and `clip_bytes`
// let the rollback test build a distinct R2 revision of the same character.
carto::core::Result<carto::production::PreparedCharacterInput> make_golden_input(
    const GoldenFixture& fixture,
    std::uint64_t revision,
    carto::assets::Sha256Digest clip_digest,
    std::string clip_bytes) {
    using namespace carto::production;

    PreparedCharacterInput input;
    input.definition = fixture.definition;
    input.definition.source_revision = carto::core::Revision(revision);
    input.skeleton = fixture.skeleton;
    input.weights.binding = fixture.binding;
    input.weights.layer = fresh_weight_layer(fixture, carto::character::RigLayerId{1U});

    CharacterLoweredWeightsInput lowered;
    lowered.product_bytes = bytes_of(
        "CARTOGRAPHER_CHARACTER_WEIGHTS4 1\n"
        "MESH 42\n"
        "VERTEX 0 1\nINFLUENCE 1 1.0\n"
        "END\n");
    lowered.certified_max_abs_error = 2e-4;
    input.lowered_weights = std::move(lowered);

    carto::character::MorphTarget morph;
    morph.id = carto::character::MorphTargetId{1U};
    morph.source_mesh_asset = 42U;
    morph.source_mesh_digest = fixture.mesh_digest;
    morph.displaced_point_count = 2U;
    morph.delta_blob_digest = fixture.morph_blob_digest;
    morph.delta_blob_bytes = fixture.morph_blob.size();
    morph.topology_correspondence_digest = digest("golden morph topology map v1");
    morph.tangent_in = carto::character::TangentPolicy::source_preserved;
    morph.tangent_out = carto::character::TangentPolicy::recompute;
    morph.precision_bound = 0.05;
    input.morphs.push_back(
        CharacterMorphProductInput{morph, fixture.morph_blob});

    input.clips.push_back(CharacterClipProductInput{
        carto::character::ClipRef{carto::temporal::ClipId{5U}, clip_digest},
        bytes_of(clip_bytes)});

    CharacterSecondaryProductInput garment;
    garment.identity = "character.secondary.garment-1";
    garment.descriptor_kind = "garment";
    garment.descriptor_bytes = bytes_of(
        "CARTOGRAPHER_CHARACTER_GARMENT_CACHE 1\nPINS 2\nSOLVER bounded\nEND\n");
    garment.source_digest = fixture.garment_source_digest;
    input.secondary.push_back(std::move(garment));

    input.target_profile_id = "vanta.character.profile";
    input.target_profile_digest = digest("vanta character profile v1");
    input.coordinate_conversion.source = carto::character::CoordinatePolicy{};
    input.coordinate_conversion.target = carto::character::CoordinatePolicy{};
    input.producer_build = "cartographer-character-producer-tests 1.0";
    input.required_capabilities = {
        "character.skeletal-deformation", "character.skinning.8influence"};
    input.licenses = {
        grant_for("character.skeleton", fixture.graph_digest),
        grant_for("character.skin.weights8", fixture.mesh_digest),
        grant_for("character.skin.weights4", fixture.mesh_digest),
        grant_for("character.morphs", fixture.mesh_digest),
        grant_for("character.motion.clip.5", clip_digest),
        grant_for("character.secondary.garment-1", fixture.garment_source_digest),
    };
    return carto::core::Result<carto::production::PreparedCharacterInput>::success(
        std::move(input));
}

// Recomputes the closure table from payload bytes, keyed by manifest path.
std::map<std::string, carto::assets::Sha256Digest> payload_digests(
    const carto::production::PreparedCharacterPackage& package) {
    std::map<std::string, carto::assets::Sha256Digest> digests;
    for (const auto& product : package.manifest.products) {
        const auto payload = std::find_if(package.payloads.begin(), package.payloads.end(),
            [&product](const carto::production::PreparedProductPayload& candidate) {
                return carto::assets::sha256(candidate.bytes) == product.digest;
            });
        require(payload != package.payloads.end(), "every manifest product has a payload");
        if (payload != package.payloads.end()) {
            digests.emplace(product.path, carto::assets::sha256(payload->bytes));
        }
    }
    return digests;
}

void stage_package(const carto::production::PreparedCharacterPackage& package,
                   const std::filesystem::path& root) {
    const auto manifest_path = root / "manifest.carto-character";
    {
        const auto text = package.manifest.serialize();
        std::filesystem::create_directories(root);
        std::ofstream output(manifest_path, std::ios::binary);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    for (const auto& product : package.manifest.products) {
        const auto payload = std::find_if(package.payloads.begin(), package.payloads.end(),
            [&product](const carto::production::PreparedProductPayload& candidate) {
                return carto::assets::sha256(candidate.bytes) == product.digest;
            });
        require(payload != package.payloads.end(), "staged product payload found");
        if (payload == package.payloads.end()) return;
        const auto target = root / std::filesystem::path(product.path);
        std::filesystem::create_directories(target.parent_path());
        std::ofstream output(target, std::ios::binary);
        output.write(reinterpret_cast<const char*>(payload->bytes.data()),
            static_cast<std::streamsize>(payload->bytes.size()));
    }
}

void test_compose_manifest_closure(const GoldenFixture& fixture) {
    using namespace carto::production;
    const auto input = make_golden_input(fixture, 3U, fixture.clip_digest,
        "CARTO_CHARACTER_CLIP 1\nKEYS 2\nKEY 0 0.0\nKEY 24 1.0\nEND\n");
    require(static_cast<bool>(input), "golden input builds");
    if (!input) return;
    const auto package = compose_prepared_character(input.value());
    require(static_cast<bool>(package), "golden character composes");
    if (!package) {
        std::cerr << "  compose error: " << package.error().message << '\n';
        return;
    }
    require(package.value().manifest.products.size() == 6U,
        "golden package has six products");
    require(package.value().payloads.size() == 6U,
        "golden package has six payloads");
    require(package.value().receipts.size() == 6U,
        "golden package has six derivation receipts");
    require(static_cast<bool>(package.value().manifest.validate()),
        "manifest validates");
    require(static_cast<bool>(package.value().manifest.verify_closure(
            payload_digests(package.value()))),
        "manifest closure verifies against recomputed payload digests");

    bool saw_skeleton = false;
    bool saw_skin = false;
    bool saw_morphs = false;
    bool saw_motion = false;
    bool saw_simulation = false;
    for (const auto& product : package.value().manifest.products) {
        saw_skeleton = saw_skeleton || product.kind == "character.skeleton.v1";
        saw_skin = saw_skin || product.kind == "character.skin.v1";
        saw_morphs = saw_morphs || product.kind == "character.morphs.v1";
        saw_motion = saw_motion || product.kind == "character.motion.v1";
        saw_simulation = saw_simulation || product.kind == "character.simulation.v1";
    }
    require(saw_skeleton && saw_skin && saw_morphs && saw_motion && saw_simulation,
        "all ch18 product kinds are present");

    // Honest release level on every receipt: CPU reference, never GPU.
    for (const auto& receipt : package.value().receipts) {
        require(static_cast<bool>(receipt.validate()), "receipt validates");
        require(receipt.backend_identity == "cpu-reference",
            "receipt declares cpu-reference backend honestly");
    }

    // Independent in-memory re-read agrees.
    const auto reread = verify_prepared_character(
        package.value().manifest.serialize(), payload_digests(package.value()));
    require(static_cast<bool>(reread), "independent manifest re-read verifies");
}

void test_determinism(const GoldenFixture& fixture) {
    using namespace carto::production;
    const auto first = make_golden_input(fixture, 3U, fixture.clip_digest,
        "CARTO_CHARACTER_CLIP 1\nKEYS 2\nKEY 0 0.0\nKEY 24 1.0\nEND\n");
    const auto second = make_golden_input(fixture, 3U, fixture.clip_digest,
        "CARTO_CHARACTER_CLIP 1\nKEYS 2\nKEY 0 0.0\nKEY 24 1.0\nEND\n");
    require(static_cast<bool>(first) && static_cast<bool>(second), "inputs build");
    if (!first || !second) return;
    const auto package_a = compose_prepared_character(first.value());
    const auto package_b = compose_prepared_character(second.value());
    require(static_cast<bool>(package_a) && static_cast<bool>(package_b),
        "both compositions succeed");
    if (!package_a || !package_b) return;
    const auto digest_a = package_a.value().manifest.canonical_digest();
    const auto digest_b = package_b.value().manifest.canonical_digest();
    require(static_cast<bool>(digest_a) && static_cast<bool>(digest_b),
        "manifest digests compute");
    require(digest_a.value() == digest_b.value(),
        "manifest digest is deterministic across identical inputs");
    require(package_a.value().manifest.serialize() == package_b.value().manifest.serialize(),
        "manifest bytes are deterministic across identical inputs");
    require(package_a.value().payloads.size() == package_b.value().payloads.size(),
        "payload counts match");
    for (std::size_t index = 0U; index < package_a.value().payloads.size(); ++index) {
        require(package_a.value().payloads[index].bytes ==
                package_b.value().payloads[index].bytes,
            "payload bytes are deterministic across identical inputs");
    }
}

void test_publish_verify_tamper_and_rollback(const GoldenFixture& fixture) {
    using namespace carto::production;
    TempDirectory root;

    const auto input_r1 = make_golden_input(fixture, 3U, fixture.clip_digest,
        "CARTO_CHARACTER_CLIP 1\nKEYS 2\nKEY 0 0.0\nKEY 24 1.0\nEND\n");
    require(static_cast<bool>(input_r1), "R1 input builds");
    if (!input_r1) return;
    const auto r1 = compose_prepared_character(input_r1.value());
    require(static_cast<bool>(r1), "R1 composes");
    if (!r1) return;

    const auto published_r1 = publish_prepared_character(
        root.path(), r1.value(), std::nullopt);
    require(static_cast<bool>(published_r1), "R1 publishes with empty-store expectation");
    if (!published_r1) {
        std::cerr << "  publish error: " << published_r1.error().message << '\n';
        return;
    }
    require(static_cast<bool>(published_r1.value().validate()),
        "publication receipt validates");
    require(!published_r1.value().previous_selected_digest.has_value(),
        "first publication has no previous selection");

    const auto selected_r1 = read_selected_prepared_scene(root.path());
    require(static_cast<bool>(selected_r1), "selected revision re-reads");
    require(selected_r1.value().has_value(), "a revision is selected after R1");
    if (selected_r1.value().has_value()) {
        require(selected_r1.value()->prepared_scene_digest ==
                published_r1.value().selected_digest,
            "selector digest matches the R1 publication receipt");
    }

    // The staged immutable revision must independently re-verify, including
    // the character manifest closure and coordinate policy.
    const auto verified = verify_prepared_character_package(
        published_r1.value().revision_path);
    require(static_cast<bool>(verified), "staged R1 revision verifies end to end");
    if (!verified) {
        std::cerr << "  verify error: " << verified.error().message << '\n';
    }

    // R2: same character, higher revision, changed clip content.
    const auto clip_r2_digest = digest("golden-clip wave v2");
    const auto input_r2 = make_golden_input(fixture, 4U, clip_r2_digest,
        "CARTO_CHARACTER_CLIP 1\nKEYS 3\nKEY 0 0.0\nKEY 12 0.5\nKEY 24 1.0\nEND\n");
    require(static_cast<bool>(input_r2), "R2 input builds");
    if (!input_r2) return;
    const auto r2 = compose_prepared_character(input_r2.value());
    require(static_cast<bool>(r2), "R2 composes");
    if (!r2) return;

    // Tampered product refusal at publish: flip one byte of the clip payload
    // so the payload digest no longer matches the manifest record.
    PreparedCharacterPackage tampered = r2.value();
    bool flipped = false;
    for (auto& payload : tampered.payloads) {
        if (payload.identity == "character.motion.clip.5") {
            payload.bytes.front() ^= 0x01U;
            flipped = true;
        }
    }
    require(flipped, "clip payload located for tamper");
    const auto tampered_publish = publish_prepared_character(
        root.path(), tampered, published_r1.value().selected_digest);
    require(!tampered_publish, "publish refuses a tampered product payload");

    // Rollback to last-good: the failed attempt must leave R1 selected, and
    // the rollback receipt must prove it.
    const auto rollback = capture_character_rollback_receipt(
        root.path(), published_r1.value().selected_digest,
        "publish", "tampered clip payload digest mismatch");
    require(static_cast<bool>(rollback), "rollback receipt captures");
    if (rollback) {
        require(rollback.value().preserved_selected_digest.has_value() &&
                rollback.value().preserved_selected_digest.value() ==
                    published_r1.value().selected_digest,
            "rollback receipt proves the last-good selection is preserved");
        require(static_cast<bool>(rollback.value().canonical_digest()),
            "rollback receipt digest computes");
    }
    const auto still_r1 = read_selected_prepared_scene(root.path());
    require(static_cast<bool>(still_r1) && still_r1.value().has_value() &&
            still_r1.value()->prepared_scene_digest ==
                published_r1.value().selected_digest,
        "selector still resolves to R1 after the failed R2 attempt");

    // Clean R2 publish with the correct CAS expectation commits.
    const auto published_r2 = publish_prepared_character(
        root.path(), r2.value(), published_r1.value().selected_digest);
    require(static_cast<bool>(published_r2), "clean R2 publishes");
    if (published_r2) {
        require(published_r2.value().previous_selected_digest.has_value() &&
                published_r2.value().previous_selected_digest.value() ==
                    published_r1.value().selected_digest,
            "R2 receipt records R1 as the previous selection");
        const auto selected_r2 = read_selected_prepared_scene(root.path());
        require(static_cast<bool>(selected_r2) && selected_r2.value().has_value() &&
                selected_r2.value()->prepared_scene_digest ==
                    published_r2.value().selected_digest,
            "selector resolves to R2 after the clean publish");
        const auto verified_r2 =
            verify_prepared_character_package(published_r2.value().revision_path);
        require(static_cast<bool>(verified_r2), "staged R2 revision verifies end to end");

        // Silent historical mutation is refused: re-selecting the older R1
        // revision against a current R2 selector fails closed. An authorized
        // rollback carries a new decision/event receipt (ch27), not a silent
        // reuse of the publish path.
        const auto downgrade = publish_prepared_character(
            root.path(), r1.value(), published_r2.value().selected_digest);
        require(!downgrade, "publish refuses to silently select an older source revision");
    }
}

void test_package_tree_hostile_inputs(const GoldenFixture& fixture) {
    using namespace carto::production;
    TempDirectory root;
    const auto input = make_golden_input(fixture, 3U, fixture.clip_digest,
        "CARTO_CHARACTER_CLIP 1\nKEYS 2\nKEY 0 0.0\nKEY 24 1.0\nEND\n");
    require(static_cast<bool>(input), "input builds");
    if (!input) return;
    const auto package = compose_prepared_character(input.value());
    require(static_cast<bool>(package), "package composes");
    if (!package) return;
    stage_package(package.value(), root.path());

    // Undeclared extra file refuses.
    {
        const auto extra = root.path() / "products" / "surprise.bin";
        {
            std::ofstream output(extra, std::ios::binary);
            output << "boom";
        }
        const auto verified = verify_prepared_character_package(root.path());
        require(!verified, "undeclared file in the package tree refuses");
        std::filesystem::remove(extra);
    }
    // Missing declared product refuses.
    {
        std::filesystem::remove(root.path() / "products" / "character.skeleton.v1.txt");
        const auto verified = verify_prepared_character_package(root.path());
        require(!verified, "missing declared product refuses");
    }
    // Hostile manifest bytes refuse.
    {
        const auto verified = verify_prepared_character("not a manifest {}", {});
        require(!verified, "garbage manifest bytes refuse");
    }
    // Digest drift refuses at the closure re-check.
    {
        std::map<std::string, carto::assets::Sha256Digest> drifted;
        for (const auto& product : package.value().manifest.products) {
            drifted.emplace(product.path, product.digest);
        }
        drifted["products/character.skeleton.v1.txt"] = digest("tampered skeleton bytes");
        const auto verified = verify_prepared_character(
            package.value().manifest.serialize(), drifted);
        require(!verified, "payload digest drift refuses at closure re-check");
    }
    // A nonexistent package directory refuses.
    {
        const auto verified = verify_prepared_character_package(
            root.path() / "does-not-exist");
        require(!verified, "nonexistent package directory refuses");
    }
}

void test_license_gate_refusals(const GoldenFixture& fixture) {
    using namespace carto::production;
    const auto base = make_golden_input(fixture, 3U, fixture.clip_digest,
        "CARTO_CHARACTER_CLIP 1\nKEYS 2\nKEY 0 0.0\nKEY 24 1.0\nEND\n");
    require(static_cast<bool>(base), "base input builds");
    if (!base) return;

    {
        PreparedCharacterInput input = base.value();
        input.licenses[0].origin = CharacterLicenseOrigin::unreviewed;
        const auto package = compose_prepared_character(input);
        require(!package, "unreviewed origin refuses packaging");
        if (!package) {
            require(package.error().code == carto::core::ErrorCode::unsupported,
                "unreviewed origin reports unsupported");
        }
    }
    {
        PreparedCharacterInput input = base.value();
        input.licenses.pop_back(); // drop the garment grant
        const auto package = compose_prepared_character(input);
        require(!package, "missing grant refuses packaging");
    }
    {
        PreparedCharacterInput input = base.value();
        input.licenses[4].source_content_digest = digest("stale clip source");
        const auto package = compose_prepared_character(input);
        require(!package, "grant pinned to a stale source digest refuses");
    }
    {
        PreparedCharacterInput input = base.value();
        for (auto& grant : input.licenses) {
            grant.redistribution = CharacterRedistribution::public_redistribution;
        }
        const auto package = compose_prepared_character(input);
        require(!package, "public redistribution without commercial clearance refuses");
    }
    {
        PreparedCharacterInput input = base.value();
        input.licenses[1].attribution_required = true;
        input.licenses[1].attribution.clear();
        const auto package = compose_prepared_character(input);
        require(!package, "required-but-missing attribution refuses");
    }
    {
        // Direct gate check: a grant naming an uncomposed product refuses.
        std::map<std::string, carto::assets::Sha256Digest> products;
        products.emplace("character.skeleton", fixture.graph_digest);
        const std::vector<CharacterProductLicenseGrant> grants{
            grant_for("character.skeleton", fixture.graph_digest),
            grant_for("character.unknown", fixture.mesh_digest),
        };
        const auto gate = check_character_product_licenses(products, grants);
        require(!gate, "grant for an uncomposed product refuses");
    }
}

void test_stale_and_coordinate_refusals(const GoldenFixture& fixture) {
    using namespace carto::production;
    const auto base = make_golden_input(fixture, 3U, fixture.clip_digest,
        "CARTO_CHARACTER_CLIP 1\nKEYS 2\nKEY 0 0.0\nKEY 24 1.0\nEND\n");
    require(static_cast<bool>(base), "base input builds");
    if (!base) return;

    {
        PreparedCharacterInput input = base.value();
        input.skeleton.graph_digest = digest("foreign rig graph");
        const auto package = compose_prepared_character(input);
        require(!package, "skeleton lowered from a different rig refuses");
        if (!package) {
            require(package.error().code == carto::core::ErrorCode::stale_data,
                "stale rig reports stale_data");
        }
    }
    {
        PreparedCharacterInput input = base.value();
        input.weights.layer.staleness =
            carto::character::WeightStaleness::stale_mesh_topology;
        input.weights.layer.stale_since = carto::core::Revision(2U);
        const auto package = compose_prepared_character(input);
        require(!package, "stale weight layer refuses packaging");
    }
    {
        PreparedCharacterInput input = base.value();
        input.morphs[0].delta_blob[0] ^= 0x01U;
        const auto package = compose_prepared_character(input);
        require(!package, "morph blob digest drift refuses");
    }
    {
        // Non-identity conversion with an incomplete checklist fails closed
        // before any coordinate claim is made.
        PreparedCharacterInput input = base.value();
        input.coordinate_conversion.target.meters_per_unit = 100.0;
        const auto package = compose_prepared_character(input);
        require(!package, "incomplete coordinate conversion checklist refuses");
    }
    {
        // A fully declared conversion to a non-VANTA target refuses the
        // admission profile check instead.
        PreparedCharacterInput input = base.value();
        input.coordinate_conversion.target.handedness =
            carto::character::Handedness::left;
        for (std::size_t index = 0U;
             index < carto::character::kTransformedEntityClassCount; ++index) {
            input.coordinate_conversion.checklist.transformed[index] = true;
        }
        const auto package = compose_prepared_character(input);
        require(!package, "non-VANTA target coordinate refuses");
        if (!package) {
            require(package.error().code == carto::core::ErrorCode::unsupported,
                "non-VANTA coordinate reports unsupported");
        }
    }
}

void test_migration_receipt(const GoldenFixture& fixture) {
    using namespace carto::production;
    const auto input = make_golden_input(fixture, 3U, fixture.clip_digest,
        "CARTO_CHARACTER_CLIP 1\nKEYS 2\nKEY 0 0.0\nKEY 24 1.0\nEND\n");
    if (!input) return;
    const auto package = compose_prepared_character(input.value());
    if (!package) return;

    const auto receipt = make_character_migration_receipt(
        "carto.test.character/character:7",
        carto::core::Revision(3U),
        carto::core::Revision(4U),
        1U, 0U, 1U, 1U,
        bytes_of("original schema bytes"),
        package.value().manifest,
        true,
        {"legacy-control-binding", "obsolete-cache-tag"});
    require(static_cast<bool>(receipt), "migration receipt builds");
    if (!receipt) return;
    require(static_cast<bool>(receipt.value().validate()),
        "migration receipt validates");
    require(receipt.value().rollback_path_available,
        "migration receipt declares its rollback path");
    require(receipt.value().serialize().find("FROM_SCHEMA 1 0") != std::string::npos,
        "migration receipt serializes the schema bump");
    const auto digest_a = receipt.value().canonical_digest();
    const auto receipt_b = make_character_migration_receipt(
        "carto.test.character/character:7",
        carto::core::Revision(3U),
        carto::core::Revision(4U),
        1U, 0U, 1U, 1U,
        bytes_of("original schema bytes"),
        package.value().manifest,
        true,
        {"legacy-control-binding", "obsolete-cache-tag"});
    require(static_cast<bool>(receipt_b) && static_cast<bool>(digest_a),
        "duplicate migration receipt builds");
    if (receipt_b && digest_a) {
        const auto digest_b = receipt_b.value().canonical_digest();
        require(static_cast<bool>(digest_b) && digest_a.value() == digest_b.value(),
            "migration receipt digest is deterministic");
    }
    const auto duplicate_loss = make_character_migration_receipt(
        "carto.test.character/character:7",
        carto::core::Revision(3U),
        carto::core::Revision(4U),
        1U, 0U, 1U, 1U,
        bytes_of("original schema bytes"),
        package.value().manifest,
        true,
        {"same", "same"});
    require(!duplicate_loss, "duplicate loss report entries refuse");
}

void test_enum_kinds_round_trip(const GoldenFixture& fixture) {
    // The new PreparedProductKind values must survive the prepared-scene
    // envelope codec so character products ride scene publication unchanged.
    using namespace carto::production;
    TempDirectory root;
    const auto input = make_golden_input(fixture, 3U, fixture.clip_digest,
        "CARTO_CHARACTER_CLIP 1\nKEYS 2\nKEY 0 0.0\nKEY 24 1.0\nEND\n");
    if (!input) return;
    const auto package = compose_prepared_character(input.value());
    if (!package) return;
    const auto published =
        publish_prepared_character(root.path(), package.value(), std::nullopt);
    require(static_cast<bool>(published), "publish for enum round trip succeeds");
    if (!published) return;
    const auto selected = read_selected_prepared_scene(root.path());
    require(static_cast<bool>(selected), "published envelope re-reads");
    if (selected && selected.value().has_value()) {
        bool saw_skeleton = false;
        bool saw_weights = false;
        bool saw_morphs = false;
        bool saw_clips = false;
        bool saw_secondary = false;
        bool saw_manifest = false;
        for (const auto& product : selected.value()->envelope.products) {
            saw_skeleton = saw_skeleton ||
                product.kind == PreparedProductKind::character_skeleton;
            saw_weights = saw_weights ||
                product.kind == PreparedProductKind::character_weights;
            saw_morphs = saw_morphs ||
                product.kind == PreparedProductKind::character_morphs;
            saw_clips = saw_clips ||
                product.kind == PreparedProductKind::character_clips;
            saw_secondary = saw_secondary ||
                product.kind == PreparedProductKind::character_secondary;
            saw_manifest = saw_manifest ||
                product.kind == PreparedProductKind::character_manifest;
        }
        require(saw_skeleton && saw_weights && saw_morphs && saw_clips &&
                saw_secondary && saw_manifest,
            "new product kinds survive publication round trip");
        const std::string text = selected.value()->envelope.serialize();
        const auto reparsed = PreparedSceneEnvelope::deserialize(text);
        require(static_cast<bool>(reparsed), "envelope codec round trips with new kinds");
    }
}

} // namespace

int main() {
    const auto fixture = make_golden_fixture();
    require(static_cast<bool>(fixture), "golden fixture builds");
    if (fixture) {
        test_compose_manifest_closure(fixture.value());
        test_determinism(fixture.value());
        test_publish_verify_tamper_and_rollback(fixture.value());
        test_package_tree_hostile_inputs(fixture.value());
        test_license_gate_refusals(fixture.value());
        test_stale_and_coordinate_refusals(fixture.value());
        test_migration_receipt(fixture.value());
        test_enum_kinds_round_trip(fixture.value());
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " character production test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "character production tests passed\n";
    return EXIT_SUCCESS;
}
