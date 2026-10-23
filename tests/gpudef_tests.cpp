#include <carto/gpudef/capability.hpp>
#include <carto/gpudef/deform_cases.hpp>
#include <carto/gpudef/emulation.hpp>
#include <carto/gpudef/evidence.hpp>
#include <carto/gpudef/layout.hpp>
#include <carto/gpudef/parity.hpp>
#include <carto/gpudef/perf.hpp>
#include <carto/gpudef/throughput.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

using namespace carto;

struct FixtureSkin {
    std::vector<core::Vec3d> positions;
    std::vector<std::uint32_t> counts;
    std::vector<kinematic::JointInfluence> influences;

    [[nodiscard]] gpudef::SkinInputView view(std::uint32_t joints = 3U) const {
        return gpudef::SkinInputView{positions, counts, influences, joints, std::nullopt};
    }
};

// Three vertices, two joints. Values chosen exactly representable in fp32
// and fp16 so fp32 roundtrips are bit-exact and fp16 errors are measurable
// against the declared bound.
FixtureSkin make_skin() {
    FixtureSkin skin;
    skin.positions = {
        {0.25, 0.5, -0.75},
        {1.0, -1.5, 2.0},
        {0.0, 0.0, 0.0},
    };
    // Vertex 0: two influences on joints 1 and 2.
    skin.counts = {2U, 1U, 0U};
    skin.influences = {
        {kinematic::JointId{1U}, 0.75},
        {kinematic::JointId{2U}, 0.25},
        {kinematic::JointId{1U}, 1.0},
    };
    return skin;
}

gpudef::Mat34 translation_matrix(double x, double y, double z) {
    gpudef::Mat34 matrix = gpudef::Mat34::identity();
    matrix.rows[3] = x;
    matrix.rows[7] = y;
    matrix.rows[11] = z;
    return matrix;
}

void test_pack_unpack_fp32_exact() {
    const FixtureSkin skin = make_skin();
    auto packed = gpudef::pack(
        skin.view(), gpudef::InfluenceLayout::influences_8,
        gpudef::QuantizationMode::fp32);
    require(static_cast<bool>(packed), "fp32 pack succeeds");
    if (!packed) return;
    require(packed.value().buffer.stride_bytes() == 60U,
        "fp32 8-influence stride is 60 bytes");
    require(packed.value().buffer.payload.size() == 3U * 60U,
        "fp32 payload matches vertex count times stride");
    require(packed.value().error.max_position_component_error == 0.0,
        "fp32 position roundtrip is exact");
    require(packed.value().error.max_weight_error == 0.0,
        "fp32 weight roundtrip is exact for representable values");

    auto decoded = gpudef::unpack(packed.value().buffer);
    require(static_cast<bool>(decoded), "fp32 unpack succeeds");
    if (!decoded) return;
    require(decoded.value().positions.size() == 3U, "decoded vertex count matches");
    for (std::size_t index = 0U; index < 3U; ++index) {
        const core::Vec3d& expected = skin.positions[index];
        const core::Vec3d& actual = decoded.value().positions[index];
        require(actual.x == expected.x && actual.y == expected.y &&
                actual.z == expected.z,
            "fp32 decoded position is bit-exact");
    }
    require(decoded.value().influence_counts == skin.counts,
        "fp32 decoded influence counts match (zero-weight padding dropped)");
    require(decoded.value().influences.size() == 3U,
        "fp32 decoded influence population matches");
    require(decoded.value().influences[0].joint.value == 1U &&
            decoded.value().influences[0].weight == 0.75,
        "fp32 decoded influence values are exact");
}

void test_fp16_error_bounded_by_receipt() {
    FixtureSkin skin = make_skin();
    // 0.7/0.3 are not exactly representable in binary16, so quantization
    // error is actually exercised (1.0 and the quarter grid stay exact).
    skin.influences[0U].weight = 0.7;
    skin.influences[1U].weight = 0.3;
    auto packed = gpudef::pack(
        skin.view(), gpudef::InfluenceLayout::influences_8,
        gpudef::QuantizationMode::fp16);
    require(static_cast<bool>(packed), "fp16 pack succeeds");
    if (!packed) return;
    require(packed.value().buffer.stride_bytes() == 38U,
        "fp16 8-influence stride is 38 bytes");
    require(packed.value().error.max_weight_error > 0.0,
        "fp16 weight quantization error is measured");
    require(packed.value().error.max_weight_error <=
            packed.value().error.declared_weight_error_bound + 1e-15,
        "fp16 measured weight error within declared bound");
    require(packed.value().error.declared_weight_error_bound == std::ldexp(1.0, -11),
        "fp16 declared weight bound is half an ulp of 1.0");
    auto decoded = gpudef::unpack(packed.value().buffer);
    require(static_cast<bool>(decoded), "fp16 unpack succeeds");
    if (!decoded) return;
    for (std::size_t index = 0U; index < decoded.value().positions.size(); ++index) {
        const double error =
            (decoded.value().positions[index] - skin.positions[index]).length();
        require(error <= 3.0 * packed.value().error.declared_position_error_bound + 1e-12,
            "fp16 decoded position within declared bound per axis");
    }
}

void test_unorm16_error_bounded_by_receipt() {
    FixtureSkin skin = make_skin();
    gpudef::PositionQuantizationBounds bounds{{-2.0, -2.0, -2.0}, {2.0, 2.0, 2.0}};
    gpudef::SkinInputView view = skin.view();
    view.position_bounds = bounds;
    auto packed = gpudef::pack(
        view, gpudef::InfluenceLayout::influences_4,
        gpudef::QuantizationMode::unorm16);
    require(static_cast<bool>(packed), "unorm16 pack succeeds");
    if (!packed) return;
    require(packed.value().buffer.stride_bytes() == 22U,
        "unorm16 4-influence stride is 22 bytes");
    require(packed.value().error.declared_weight_error_bound == 0.5 / 65535.0,
        "unorm16 declared weight bound is half a quantum");
    require(packed.value().error.max_weight_error <= 0.5 / 65535.0 + 1e-18,
        "unorm16 measured weight error within half a quantum");
    auto decoded = gpudef::unpack(packed.value().buffer);
    require(static_cast<bool>(decoded), "unorm16 unpack succeeds");
    if (!decoded) return;
    for (std::size_t index = 0U; index < decoded.value().positions.size(); ++index) {
        const core::Vec3d delta = decoded.value().positions[index] - skin.positions[index];
        const double quantum = 4.0 / 65535.0;
        require(std::abs(delta.x) <= 0.5 * quantum + 1e-15 &&
                std::abs(delta.y) <= 0.5 * quantum + 1e-15 &&
                std::abs(delta.z) <= 0.5 * quantum + 1e-15,
            "unorm16 decoded position within half-quantum per axis");
    }
}

void test_layout_digests_differ() {
    const FixtureSkin skin = make_skin();
    auto eight = gpudef::pack(
        skin.view(), gpudef::InfluenceLayout::influences_8,
        gpudef::QuantizationMode::fp32);
    auto four = gpudef::pack(
        skin.view(), gpudef::InfluenceLayout::influences_4,
        gpudef::QuantizationMode::fp32);
    require(static_cast<bool>(eight) && static_cast<bool>(four),
        "both layout packs succeed");
    if (!eight || !four) return;
    auto digest_eight = eight.value().buffer.canonical_digest();
    auto digest_four = four.value().buffer.canonical_digest();
    require(static_cast<bool>(digest_eight) && static_cast<bool>(digest_four),
        "layout digests compute");
    require(digest_eight.value().bytes != digest_four.value().bytes,
        "8-influence and 4-influence digests differ");
}

void test_pack_refuses_overcapacity() {
    FixtureSkin skin;
    skin.positions = {{0.0, 0.0, 0.0}};
    skin.counts = {5U};
    skin.influences = {
        {kinematic::JointId{1U}, 0.2},
        {kinematic::JointId{2U}, 0.2},
        {kinematic::JointId{3U}, 0.2},
        {kinematic::JointId{1U}, 0.2}, // duplicate joint: also invalid
        {kinematic::JointId{2U}, 0.2},
    };
    auto refused = gpudef::pack(
        skin.view(), gpudef::InfluenceLayout::influences_4,
        gpudef::QuantizationMode::fp32);
    require(!refused, "over-capacity 4-influence pack fails closed");
    if (!refused) {
        require(refused.error().code == core::ErrorCode::invalid_argument ||
                refused.error().code == core::ErrorCode::unsupported,
            "over-capacity refusal is explicit");
    }

    // Valid 5-influence vertex reduced explicitly, then packs.
    std::vector<std::uint32_t> counts{5U};
    std::vector<kinematic::JointInfluence> influences = {
        {kinematic::JointId{1U}, 0.4},
        {kinematic::JointId{2U}, 0.25},
        {kinematic::JointId{3U}, 0.2},
        {kinematic::JointId{4U}, 0.1},
        {kinematic::JointId{5U}, 0.05},
    };
    auto reduced = gpudef::reduce_to_top_four(counts, influences);
    require(static_cast<bool>(reduced), "explicit top-four reduction succeeds");
    if (!reduced) return;
    require(reduced.value().influence_counts.size() == 1U &&
            reduced.value().influence_counts[0U] == 4U,
        "reduction keeps four influences");
    require(reduced.value().max_discarded_weight_mass == 0.05,
        "reduction reports discarded mass honestly");
    double sum = 0.0;
    for (const auto& influence : reduced.value().influences) sum += influence.weight;
    require(std::abs(sum - 1.0) < 1e-12, "reduction renormalizes to one");

    FixtureSkin reduced_skin;
    reduced_skin.positions = {{0.0, 0.0, 0.0}};
    reduced_skin.counts = reduced.value().influence_counts;
    reduced_skin.influences = reduced.value().influences;
    // Six joints referenced (index 5 is the largest), so declare six.
    auto packed = gpudef::pack(
        reduced_skin.view(6U), gpudef::InfluenceLayout::influences_4,
        gpudef::QuantizationMode::fp32);
    require(static_cast<bool>(packed), "reduced skin packs into 4-influence layout");
}

void test_emulation_matches_hand_computed_joint() {
    // One vertex at (1, 0, 0), one joint with rotation +90 degrees about Z
    // and translation (0, 0, 2). Expected: rotate (1,0,0) -> (0,1,0), then
    // translate -> (0, 1, 2).
    FixtureSkin skin;
    skin.positions = {{1.0, 0.0, 0.0}};
    skin.counts = {1U};
    skin.influences = {{kinematic::JointId{1U}, 1.0}};

    core::Transform pose = core::Transform::identity();
    pose.rotation = {0.0, 0.0, std::sqrt(0.5), std::sqrt(0.5)};
    pose.translation = {0.0, 0.0, 2.0};
    const gpudef::Mat34 skin_matrix = gpudef::Mat34::from_transform(pose);

    auto packed = gpudef::pack(
        skin.view(), gpudef::InfluenceLayout::influences_8,
        gpudef::QuantizationMode::fp32);
    require(static_cast<bool>(packed), "single-joint pack succeeds");
    if (!packed) return;
    const std::vector<gpudef::Mat34> matrices{gpudef::Mat34::identity(), skin_matrix};
    auto emulated = gpudef::emulate_packed_deform(packed.value().buffer, matrices);
    require(static_cast<bool>(emulated), "single-joint emulation succeeds");
    if (!emulated) return;
    require(emulated.value().size() == 1U, "single vertex out");
    require(std::abs(emulated.value()[0U].x - 0.0) < 1e-12 &&
            std::abs(emulated.value()[0U].y - 1.0) < 1e-12 &&
            std::abs(emulated.value()[0U].z - 2.0) < 1e-12,
        "emulation matches hand-computed rotated+translated vertex");

    auto reference = gpudef::reference_deform(skin.view(), matrices);
    require(static_cast<bool>(reference), "reference deform succeeds");
    if (!reference) return;
    require(reference.value()[0U].x == emulated.value()[0U].x &&
            reference.value()[0U].y == emulated.value()[0U].y &&
            reference.value()[0U].z == emulated.value()[0U].z,
        "fp32 emulation is bit-identical to the CPU reference");
}

void test_matrix_inverse_roundtrip() {
    core::Transform transform = core::Transform::identity();
    transform.rotation = {0.0, 0.0, std::sqrt(0.5), std::sqrt(0.5)};
    transform.translation = {1.0, -2.0, 3.0};
    transform.scale = {2.0, 1.0, 0.5};
    const gpudef::Mat34 matrix = gpudef::Mat34::from_transform(transform);
    auto inverse = matrix.inverted();
    require(static_cast<bool>(inverse), "TRS matrix inverts");
    if (!inverse) return;
    const core::Vec3d point{0.25, 4.0, -1.0};
    const core::Vec3d roundtrip = inverse.value().apply(matrix.apply(point));
    require((roundtrip - point).length() < 1e-12,
        "matrix inverse roundtrips a point");
    const gpudef::Mat34 singular{};
    require(!singular.inverted(), "singular matrix inversion fails closed");
}

void test_parity_harness_cpu_tier() {
    const FixtureSkin skin = make_skin();
    const std::vector<gpudef::Mat34> matrices{
        gpudef::Mat34::identity(),
        translation_matrix(1.0, 0.0, 0.0),
        translation_matrix(0.0, 2.0, 0.0),
    };
    auto reference = gpudef::reference_deform(skin.view(), matrices);
    require(static_cast<bool>(reference), "oracle reference computes");
    const std::vector<core::Vec3d> oracle =
        reference ? reference.value() : std::vector<core::Vec3d>{};

    gpudef::ParityInput input;
    input.skin = skin.view();
    input.layout = gpudef::InfluenceLayout::influences_8;
    input.mode = gpudef::QuantizationMode::fp32;
    input.skin_matrices = matrices;
    input.reference_positions = oracle;
    auto receipt = gpudef::run_parity(input);
    require(static_cast<bool>(receipt), "parity run succeeds");
    if (!receipt) return;
    require(receipt.value().max_position_error == 0.0,
        "fp32 parity against CPU oracle is exact");
    require(receipt.value().tier == gpudef::EvidenceTier::cpu_reference,
        "parity tier stays cpu-reference without device profile");
    require(!receipt.value().device_profile.has_value(),
        "device profile explicitly unset for CPU tier");
    require(gpudef::effective_tier(receipt.value()) == gpudef::EvidenceTier::cpu_reference,
        "effective tier is cpu-reference without device evidence");
    require(static_cast<bool>(gpudef::require_evidence_tier(
            receipt.value(), gpudef::EvidenceTier::cpu_reference)),
        "cpu-reference requirement is satisfiable");
    require(!gpudef::require_evidence_tier(
            receipt.value(), gpudef::EvidenceTier::native_device),
        "native-device requirement fails closed without evidence");
    require(!gpudef::claim_integrated_product(),
        "integrated-product claim is an honest refusal");
    require(static_cast<bool>(receipt.value().canonical_digest()),
        "parity receipt digest computes");
    require(receipt.value().bounding_diagonal > 0.0,
        "parity receipt records a bounding diagonal");

    // Device-measured claim without a profile is refused outright.
    gpudef::ParityInput lying = input;
    lying.reference_is_device_measured = true;
    require(!gpudef::run_parity(lying),
        "device-measured claim without named hardware fails closed");

    // A fully named profile with device-measured reference upgrades the
    // tier — the only path to native_device.
    gpudef::ParityInput device_run = input;
    device_run.reference_is_device_measured = true;
    device_run.device_profile = gpudef::DeviceProfile{
        "adapter-0", "Reference GPU", "driver 1.0", "Test OS", "fp32-lbs"};
    auto device_receipt = gpudef::run_parity(device_run);
    require(static_cast<bool>(device_receipt),
        "named device parity run succeeds");
    if (device_receipt) {
        require(device_receipt.value().tier == gpudef::EvidenceTier::native_device,
            "named device evidence reaches native-device tier");
        require(static_cast<bool>(gpudef::require_evidence_tier(
                device_receipt.value(), gpudef::EvidenceTier::native_device)),
            "native-device requirement passes with named evidence");
    }
}

void test_parity_determinism() {
    const FixtureSkin skin = make_skin();
    const std::vector<gpudef::Mat34> matrices{
        gpudef::Mat34::identity(),
        translation_matrix(1.0, 0.0, 0.0),
        translation_matrix(0.0, 2.0, 0.0),
    };
    auto reference = gpudef::reference_deform(skin.view(), matrices);
    const std::vector<core::Vec3d> oracle =
        reference ? reference.value() : std::vector<core::Vec3d>{};
    gpudef::ParityInput input;
    input.skin = skin.view();
    input.mode = gpudef::QuantizationMode::fp16;
    input.skin_matrices = matrices;
    input.reference_positions = oracle;
    auto first = gpudef::run_parity(input);
    auto second = gpudef::run_parity(input);
    require(static_cast<bool>(first) && static_cast<bool>(second),
        "deterministic parity runs succeed");
    if (!first || !second) return;
    auto digest_first = first.value().canonical_digest();
    auto digest_second = second.value().canonical_digest();
    require(static_cast<bool>(digest_first) && static_cast<bool>(digest_second),
        "deterministic digests compute");
    require(digest_first.value().bytes == digest_second.value().bytes,
        "parity receipts are deterministic across runs");
}

void test_perf_receipt_honesty() {
    gpudef::PerfReceipt cpu_measurement;
    cpu_measurement.kernel_version = "gpudef-emulation 1.0";
    cpu_measurement.timing_slots = {{"emulate_packed_deform", 1'250'000U, 40U}};
    cpu_measurement.memory = {1U << 20U, 2U << 20U, 3U};
    cpu_measurement.animated_character_count = 1U;
    cpu_measurement.vertex_count = 3U;
    cpu_measurement.triangle_count = 1U;
    cpu_measurement.frame_first = 0U;
    cpu_measurement.frame_last = 39U;
    require(static_cast<bool>(cpu_measurement.validate()),
        "CPU-tier perf receipt validates with not-run acceptance");
    require(gpudef::effective_tier(cpu_measurement) ==
            gpudef::EvidenceTier::cpu_reference,
        "perf tier stays cpu-reference without device profile");
    require(!gpudef::require_evidence_tier(
            cpu_measurement, gpudef::EvidenceTier::native_device),
        "native perf claim without profile fails closed");

    // Acceptance without named hardware is not valid evidence.
    cpu_measurement.acceptance = gpudef::PerfAcceptance::pass;
    require(!cpu_measurement.validate(),
        "acceptance decision without named hardware fails closed");

    gpudef::PerfReceipt device_measurement = cpu_measurement;
    device_measurement.device_profile = gpudef::DeviceProfile{
        "adapter-0", "Reference GPU", "driver 1.0", "Test OS", "fp32-lbs"};
    require(static_cast<bool>(device_measurement.validate()),
        "named device perf receipt validates");
    require(gpudef::effective_tier(device_measurement) ==
            gpudef::EvidenceTier::native_device,
        "named device perf evidence reaches native-device tier");
    require(static_cast<bool>(gpudef::require_evidence_tier(
            device_measurement, gpudef::EvidenceTier::native_device)),
        "native perf requirement passes with named evidence");

    // Wall time is measurement evidence, not identity: changing timings must
    // not change the canonical digest.
    auto digest_a = device_measurement.canonical_digest();
    device_measurement.timing_slots[0U].wall_time_ns = 9'999'999U;
    device_measurement.memory.peak_committed_bytes = 4U << 20U;
    device_measurement.acceptance = gpudef::PerfAcceptance::fail;
    auto digest_b = device_measurement.canonical_digest();
    require(static_cast<bool>(digest_a) && static_cast<bool>(digest_b),
        "perf digests compute across re-measurement");
    require(digest_a.value().bytes == digest_b.value().bytes,
        "perf canonical digest excludes wall-time and memory measurements");

    // Device profile serialization roundtrips.
    const std::string serialized = device_measurement.device_profile->serialize();
    auto parsed = gpudef::DeviceProfile::deserialize(serialized);
    require(static_cast<bool>(parsed) && parsed.value().gpu_name == "Reference GPU",
        "device profile serializes and deserializes");
}

void test_packed_buffer_serialization_roundtrip() {
    const FixtureSkin skin = make_skin();
    auto packed = gpudef::pack(
        skin.view(), gpudef::InfluenceLayout::influences_8,
        gpudef::QuantizationMode::fp32);
    require(static_cast<bool>(packed), "pack for serialization succeeds");
    if (!packed) return;
    const std::string serialized = packed.value().buffer.serialize();
    auto parsed = gpudef::PackedVertexBuffer::deserialize(serialized);
    require(static_cast<bool>(parsed), "packed buffer deserializes");
    if (!parsed) return;
    auto digest_a = packed.value().buffer.canonical_digest();
    auto digest_b = parsed.value().canonical_digest();
    require(static_cast<bool>(digest_a) && static_cast<bool>(digest_b) &&
            digest_a.value().bytes == digest_b.value().bytes,
        "packed buffer digest survives serialization roundtrip");
    require(!gpudef::PackedVertexBuffer::deserialize("garbage"),
        "hostile packed buffer text fails closed");
}

void test_hostile_inputs() {
    FixtureSkin skin = make_skin();
    gpudef::SkinInputView view = skin.view();
    view.joint_count = 0U;
    require(!gpudef::pack(view, gpudef::InfluenceLayout::influences_8,
            gpudef::QuantizationMode::fp32),
        "zero joint count fails closed");

    view = skin.view();
    skin.positions[0U] = {std::numeric_limits<double>::infinity(), 0.0, 0.0};
    require(!gpudef::pack(view, gpudef::InfluenceLayout::influences_8,
            gpudef::QuantizationMode::fp32),
        "non-finite position fails closed");
    skin.positions[0U] = {0.25, 0.5, -0.75};

    view = skin.view();
    require(!gpudef::pack(view, gpudef::InfluenceLayout::influences_8,
            gpudef::QuantizationMode::unorm16),
        "unorm16 without bounds fails closed");

    // Influence sum off by more than the admitted tolerance.
    FixtureSkin bad;
    bad.positions = {{0.0, 0.0, 0.0}};
    bad.counts = {1U};
    bad.influences = {{kinematic::JointId{1U}, 0.5}};
    require(!gpudef::pack(bad.view(), gpudef::InfluenceLayout::influences_8,
            gpudef::QuantizationMode::fp32),
        "un-normalized influence sum fails closed via the shared admission contract");

    // Joint reference outside the declared joint count.
    FixtureSkin out_of_range;
    out_of_range.positions = {{0.0, 0.0, 0.0}};
    out_of_range.counts = {1U};
    out_of_range.influences = {{kinematic::JointId{9U}, 1.0}};
    require(!gpudef::pack(out_of_range.view(), gpudef::InfluenceLayout::influences_8,
            gpudef::QuantizationMode::fp32),
        "out-of-range joint reference fails closed");
}

void test_half_codec_known_values() {
    require(gpudef::half_bits_to_float(0U) == 0.0F, "half zero decodes");
    require(gpudef::half_bits_to_float(0x3C00U) == 1.0F, "half one decodes");
    require(gpudef::half_bits_to_float(0xC000U) == -2.0F, "half -2 decodes");
    require(gpudef::float_to_half_bits(1.0F) == 0x3C00U, "float one encodes");
    require(gpudef::float_to_half_bits(-2.0F) == 0xC000U, "float -2 encodes");
    // Round-to-nearest-even: exactly halfway between 1.0 and 1+2^-10 ties to
    // the even candidate 0x3C02 (0x3C01 is odd).
    require(gpudef::float_to_half_bits(1.0F + std::ldexp(1.0F, -11)) == 0x3C00U,
        "half rounding ties to even (down)");
    require(gpudef::float_to_half_bits(1.0F + 3.0F * std::ldexp(1.0F, -11)) == 0x3C02U,
        "half rounding ties to even (up to even candidate)");
    // Strictly above halfway rounds up.
    require(gpudef::float_to_half_bits(
            1.0F + std::ldexp(1.0F, -11) + std::ldexp(1.0F, -12)) == 0x3C01U,
        "half rounding above halfway rounds up");
    // Subnormal boundary: 2^-24 is the smallest positive half; 2^-25 ties to zero.
    require(gpudef::half_bits_to_float(1U) == std::ldexp(1.0F, -24),
        "half subnormal quantum decodes");
    require(gpudef::float_to_half_bits(std::ldexp(1.0F, -25)) == 0U,
        "half underflow ties to zero");
}

// Rigid skin matrices: identity, +90 degrees about Z with translation, and
// -90 degrees about Z with a different translation. Joints are 1 and 2.
std::vector<gpudef::Mat34> rigid_skin_matrices() {
    core::Transform rotate_positive = core::Transform::identity();
    rotate_positive.rotation = {0.0, 0.0, std::sqrt(0.5), std::sqrt(0.5)};
    rotate_positive.translation = {0.0, 0.0, 2.0};
    core::Transform rotate_negative = core::Transform::identity();
    rotate_negative.rotation = {0.0, 0.0, -std::sqrt(0.5), std::sqrt(0.5)};
    rotate_negative.translation = {1.0, 0.0, 0.0};
    return {
        gpudef::Mat34::identity(),
        gpudef::Mat34::from_transform(rotate_positive),
        gpudef::Mat34::from_transform(rotate_negative),
    };
}

gpudef::CaseParityInput make_lbs_case_input(
    const FixtureSkin& skin,
    std::span<const gpudef::Mat34> matrices,
    gpudef::QuantizationMode mode) {
    gpudef::CaseParityInput input;
    input.deformation_case = gpudef::DeformationCase::linear_blend_skinning;
    input.skin = skin.view();
    input.mode = mode;
    input.skin_matrices = matrices;
    return input;
}

void test_case_parity_lbs_exact_and_deterministic() {
    const FixtureSkin skin = make_skin();
    const std::vector<gpudef::Mat34> matrices = rigid_skin_matrices();
    const gpudef::CaseParityInput input =
        make_lbs_case_input(skin, matrices, gpudef::QuantizationMode::fp32);
    auto receipt = gpudef::run_case_parity(input);
    require(static_cast<bool>(receipt), "LBS case parity run succeeds");
    if (!receipt) return;
    require(receipt.value().max_position_error == 0.0,
        "LBS fp32 case parity is exact");
    require(receipt.value().exact_match_declared,
        "LBS fp32 declares an exact-match envelope");
    require(receipt.value().envelope_admitted,
        "LBS fp32 measured error admits the exact envelope");
    require(receipt.value().tier == gpudef::EvidenceTier::cpu_reference,
        "LBS case parity stays cpu-reference without device evidence");
    require(!receipt.value().packed_buffer_digest.is_zero(),
        "LBS case receipt is bound to a packed stream digest");
    require(gpudef::admitted_by_envelope(receipt.value()),
        "self-contained envelope verdict admits the LBS receipt");

    // Determinism across runs: identical receipts digest identically.
    auto again = gpudef::run_case_parity(input);
    require(static_cast<bool>(again), "repeat LBS case parity run succeeds");
    if (!again) return;
    auto digest_a = receipt.value().canonical_digest();
    auto digest_b = again.value().canonical_digest();
    require(static_cast<bool>(digest_a) && static_cast<bool>(digest_b),
        "LBS case digests compute");
    require(digest_a.value().bytes == digest_b.value().bytes,
        "LBS case parity receipts are deterministic across runs");

    // Bounded mode: fp16 with non-representable weights measures real error
    // inside a declared envelope (0.7/0.3 are not binary16-exact).
    FixtureSkin quantized_skin = make_skin();
    quantized_skin.influences[0U].weight = 0.7;
    quantized_skin.influences[1U].weight = 0.3;
    const gpudef::CaseParityInput quantized = make_lbs_case_input(
        quantized_skin, matrices, gpudef::QuantizationMode::fp16);
    auto bounded = gpudef::run_case_parity(quantized);
    require(static_cast<bool>(bounded), "LBS fp16 case parity run succeeds");
    if (!bounded) return;
    require(!bounded.value().exact_match_declared,
        "LBS fp16 declares a bounded-error envelope");
    require(bounded.value().max_position_error > 0.0,
        "LBS fp16 measures non-zero quantization error");
    require(bounded.value().max_position_error <=
            bounded.value().declared_position_error_bound,
        "LBS fp16 measured error stays inside the declared envelope");
}

void test_case_parity_dqs() {
    const FixtureSkin skin = make_skin();
    const std::vector<gpudef::Mat34> matrices = rigid_skin_matrices();
    const gpudef::CaseParityInput input =
        make_lbs_case_input(skin, matrices, gpudef::QuantizationMode::fp32);
    gpudef::CaseParityInput dqs_input = input;
    dqs_input.deformation_case = gpudef::DeformationCase::dual_quaternion_skinning;
    auto receipt = gpudef::run_case_parity(dqs_input);
    require(static_cast<bool>(receipt), "DQS case parity run succeeds");
    if (!receipt) return;
    require(receipt.value().max_position_error == 0.0,
        "DQS fp32 case parity is exact");
    require(receipt.value().envelope_admitted,
        "DQS fp32 measured error admits its envelope");

    // The DQS lane is genuinely a different kernel: on a bending blend it
    // must not silently equal the LBS lane.
    auto emulation = gpudef::run_case_emulation(dqs_input);
    auto lbs_emulation = gpudef::run_case_emulation(input);
    require(static_cast<bool>(emulation) && static_cast<bool>(lbs_emulation),
        "DQS and LBS emulation lanes both run");
    if (emulation && lbs_emulation) {
        double max_difference = 0.0;
        for (std::size_t vertex = 0U; vertex < emulation.value().size(); ++vertex) {
            max_difference = std::max(max_difference,
                (emulation.value()[vertex] - lbs_emulation.value()[vertex]).length());
        }
        require(max_difference > 0.0,
            "DQS lane differs from the LBS lane on a bending blend");
    }

    // Non-rigid joint transforms are refused, never approximated.
    core::Transform scaled = core::Transform::identity();
    scaled.scale = {2.0, 1.0, 1.0};
    gpudef::CaseParityInput shear = dqs_input;
    const std::vector<gpudef::Mat34> non_rigid{
        gpudef::Mat34::identity(),
        gpudef::Mat34::from_transform(scaled),
        gpudef::Mat34::from_transform(scaled),
    };
    shear.skin_matrices = non_rigid;
    auto refused = gpudef::run_case_parity(shear);
    require(!refused, "DQS with non-rigid joint transforms fails closed");
    if (!refused) {
        require(refused.error().code == core::ErrorCode::unsupported,
            "DQS non-rigid refusal is explicit");
    }
}

void test_case_parity_morph_stage() {
    const FixtureSkin skin = make_skin();
    const std::vector<gpudef::MorphDeltaBinding> deltas{
        {0U, {0.125, -0.25, 0.5}},
        {2U, {1.0, 1.0, 1.0}},
    };

    gpudef::CaseParityInput input;
    input.deformation_case = gpudef::DeformationCase::morph_stage;
    input.skin = skin.view();
    input.morph_deltas = deltas;
    input.morph_weight = 0.5;
    auto exact = gpudef::run_case_parity(input);
    require(static_cast<bool>(exact), "morph stage fp32 case parity succeeds");
    if (exact) {
        require(exact.value().max_position_error == 0.0,
            "morph stage fp32 case parity is exact");
        require(exact.value().envelope_admitted,
            "morph stage fp32 measured error admits its envelope");
    }

    // Weight outside [0, 1] is refused, not clamped silently.
    gpudef::CaseParityInput bad_weight = input;
    bad_weight.morph_weight = 1.5;
    require(!gpudef::run_case_parity(bad_weight),
        "morph weight outside [0, 1] fails closed");

    // Quantized mode: the delta path is exact, so only the position
    // quantization error shows up, inside the declared envelope. The
    // positions are chosen off the binary16 grid so error is actually
    // measured.
    FixtureSkin quantized_skin = skin;
    quantized_skin.positions[0U] = {0.1, -0.2, 0.35};
    gpudef::CaseParityInput quantized = input;
    quantized.skin = quantized_skin.view();
    quantized.mode = gpudef::QuantizationMode::fp16;
    auto bounded = gpudef::run_case_parity(quantized);
    require(static_cast<bool>(bounded), "morph stage fp16 case parity succeeds");
    if (bounded) {
        require(bounded.value().max_position_error > 0.0,
            "morph stage fp16 measures position quantization error");
        require(bounded.value().max_position_error <=
                bounded.value().declared_position_error_bound,
            "morph stage fp16 measured error stays inside the declared envelope");
    }
}

void test_case_parity_corrective_driver() {
    const FixtureSkin skin = make_skin();

    character::CorrectiveDriver driver;
    driver.id = character::CorrectiveDriverId{1U};
    character::DriverInput control_input;
    control_input.kind = character::DriverInput::Kind::control;
    control_input.control = character::ControlId{7U};
    driver.inputs = {control_input};
    driver.domain_minimum = {0.0};
    driver.domain_maximum = {1.0};
    driver.basis = core::Transform::identity();
    driver.interpolation = character::DriverInterpolation::linear;
    driver.order_index = 0U;
    require(static_cast<bool>(driver.validate()), "fixture driver validates");

    deform::DriverChannelValues channels;
    channels.controls.emplace(character::ControlId{7U}, 0.5);

    gpudef::CaseParityInput input;
    input.deformation_case = gpudef::DeformationCase::corrective_driver;
    input.skin = skin.view();
    input.corrective_driver = &driver;
    input.corrective_channels = &channels;
    auto receipt = gpudef::run_case_parity(input);
    require(static_cast<bool>(receipt),
        "corrective driver fp32 case parity succeeds");
    if (!receipt) return;
    require(receipt.value().max_position_error == 0.0,
        "corrective driver fp32 case parity is exact");
    require(receipt.value().envelope_admitted,
        "corrective driver fp32 measured error admits its envelope");
    require(receipt.value().tier == gpudef::EvidenceTier::cpu_reference,
        "corrective driver case parity stays cpu-reference");

    // Unposed channels fail closed with the driver's own diagnostic.
    deform::DriverChannelValues empty_channels;
    gpudef::CaseParityInput unposed = input;
    unposed.corrective_channels = &empty_channels;
    auto missing = gpudef::run_case_parity(unposed);
    require(!missing, "corrective driver with unposed channels fails closed");

    // A claimed device measurement without a named profile is refused.
    gpudef::CaseParityInput lying = input;
    lying.reference_is_device_measured = true;
    require(!gpudef::run_case_parity(lying),
        "device-measured case claim without named hardware fails closed");
}

void test_envelope_admission_and_rejection() {
    const FixtureSkin skin = make_skin();
    const std::vector<gpudef::Mat34> matrices = rigid_skin_matrices();
    const gpudef::CaseParityInput input =
        make_lbs_case_input(skin, matrices, gpudef::QuantizationMode::fp32);
    auto receipt = gpudef::run_case_parity(input);
    require(static_cast<bool>(receipt), "envelope test receipt runs");
    if (!receipt) return;
    // A zero-error pack receipt is what the fp32 fixture produced; the
    // context terms below are arbitrary because exact match ignores them.
    gpudef::QuantizationErrorReceipt zero_error;
    zero_error.vertex_count = 3U;
    auto envelope = gpudef::declare_case_envelope(
        gpudef::DeformationCase::linear_blend_skinning,
        gpudef::InfluenceLayout::influences_8,
        gpudef::QuantizationMode::fp32,
        zero_error,
        gpudef::EnvelopeContext{});
    require(static_cast<bool>(envelope), "fixture envelope declares");
    require(envelope.value().exact_match, "zero pack error declares exact match");
    require(gpudef::admitted_by_envelope(envelope.value(), receipt.value()),
        "matching envelope admits the receipt");

    // Case mismatch fails closed.
    gpudef::CaseEnvelope wrong_case = envelope.value();
    wrong_case.deformation_case =
        gpudef::DeformationCase::dual_quaternion_skinning;
    require(!gpudef::admitted_by_envelope(wrong_case, receipt.value()),
        "case-mismatched envelope is rejected");

    // A measured error under an exact envelope is rejected, and the receipt
    // no longer validates: envelope violation is not mintable evidence.
    gpudef::CaseParityReceipt violated = receipt.value();
    violated.max_position_error = 1e-9;
    require(!gpudef::admitted_by_envelope(violated),
        "non-zero error under an exact envelope is rejected");
    require(!violated.validate(),
        "envelope-violating receipt fails validation closed");
    require(!violated.canonical_digest(),
        "envelope-violating receipt has no canonical digest");

    // The same violation cannot survive a text round-trip: the serialized
    // verdict no longer matches the measured error.
    const std::string serialized = violated.serialize();
    require(!gpudef::CaseParityReceipt::deserialize(serialized),
        "envelope-violating receipt text fails closed on re-admission");

    // Tightening the declared bound below a real measured error rejects.
    // The fp16 lane with non-representable weights measures actual error.
    FixtureSkin noisy_skin = make_skin();
    noisy_skin.influences[0U].weight = 0.7;
    noisy_skin.influences[1U].weight = 0.3;
    const gpudef::CaseParityInput noisy_input = make_lbs_case_input(
        noisy_skin, matrices, gpudef::QuantizationMode::fp16);
    auto noisy = gpudef::run_case_parity(noisy_input);
    require(static_cast<bool>(noisy), "noisy receipt runs for bound tightening");
    if (noisy) {
        require(noisy.value().max_position_error > 0.0,
            "noisy receipt measures real error");
        gpudef::CaseParityReceipt bounded = noisy.value();
        bounded.declared_position_error_bound =
            noisy.value().max_position_error * 0.5;
        require(!gpudef::admitted_by_envelope(bounded),
            "tightened bound below measured error is rejected");
        require(!bounded.validate(),
            "declared bound below measured error fails validation closed");
    }

    // A tampered stream digest breaks the receipt's binding.
    gpudef::CaseParityReceipt rebind = receipt.value();
    rebind.packed_buffer_digest = assets::Sha256Digest{};
    require(!rebind.validate(),
        "receipt without a packed stream digest fails closed");
}

void test_case_receipt_serialization_roundtrip() {
    const FixtureSkin skin = make_skin();
    const std::vector<gpudef::Mat34> matrices = rigid_skin_matrices();
    const gpudef::CaseParityInput cpu_input =
        make_lbs_case_input(skin, matrices, gpudef::QuantizationMode::fp16);
    auto receipt = gpudef::run_case_parity(cpu_input);
    require(static_cast<bool>(receipt), "roundtrip receipt runs");
    if (!receipt) return;
    const std::string serialized = receipt.value().serialize();
    auto parsed = gpudef::CaseParityReceipt::deserialize(serialized);
    require(static_cast<bool>(parsed), "case parity receipt deserializes");
    if (!parsed) return;
    auto digest_a = receipt.value().canonical_digest();
    auto digest_b = parsed.value().canonical_digest();
    require(static_cast<bool>(digest_a) && static_cast<bool>(digest_b) &&
            digest_a.value().bytes == digest_b.value().bytes,
        "case parity receipt digest survives serialization roundtrip");
    require(!gpudef::CaseParityReceipt::deserialize("garbage"),
        "hostile case parity receipt text fails closed");

    // Envelope roundtrip.
    const std::string envelope_text = gpudef::CaseEnvelope{
        gpudef::DeformationCase::morph_stage,
        gpudef::InfluenceLayout::influences_4,
        gpudef::QuantizationMode::unorm16,
        false, 1e-4, 0.5 / 65535.0}.serialize();
    auto parsed_envelope = gpudef::CaseEnvelope::deserialize(envelope_text);
    require(static_cast<bool>(parsed_envelope) &&
            parsed_envelope.value().deformation_case ==
                gpudef::DeformationCase::morph_stage &&
            !parsed_envelope.value().exact_match,
        "case envelope serializes and deserializes");

    // Native-tier receipt round-trips with its full named-hardware profile.
    gpudef::CaseParityInput device_input = cpu_input;
    device_input.mode = gpudef::QuantizationMode::fp32;
    device_input.reference_is_device_measured = true;
    device_input.device_profile = gpudef::DeviceProfile{
        "adapter-0", "Reference GPU", "driver 1.0", "Test OS", "fp32-lbs"};
    auto native = gpudef::run_case_parity(device_input);
    require(static_cast<bool>(native), "named device case parity run succeeds");
    if (!native) return;
    require(native.value().tier == gpudef::EvidenceTier::native_device,
        "named device case evidence reaches native-device tier");
    require(static_cast<bool>(gpudef::require_evidence_tier(
            native.value(), gpudef::EvidenceTier::native_device)),
        "native-device case requirement passes with named evidence");
    auto parsed_native = gpudef::CaseParityReceipt::deserialize(
        native.value().serialize());
    require(static_cast<bool>(parsed_native) &&
            parsed_native.value().device_profile &&
            parsed_native.value().device_profile->gpu_name == "Reference GPU",
        "native-tier case receipt round-trips its device profile");
    require(!gpudef::require_evidence_tier(
            receipt.value(), gpudef::EvidenceTier::native_device),
        "native-device case requirement fails closed without named evidence");
}

void test_throughput_receipt_windows() {
    require(gpudef::window_index_for_rate(0.0) == 0U,
        "non-positive rate lands in window zero");
    require(gpudef::window_index_for_rate(
            static_cast<double>(gpudef::kThroughputWindowBase)) == 0U,
        "base rate lands in window zero");
    require(gpudef::window_index_for_rate(
            2.0 * static_cast<double>(gpudef::kThroughputWindowBase)) == 1U,
        "double base rate lands in window one");
    require(gpudef::window_index_for_rate(1e300) ==
            gpudef::kThroughputWindowCount - 1U,
        "extreme rate clamps to the top window");

    const FixtureSkin skin = make_skin();
    const std::vector<gpudef::Mat34> matrices = rigid_skin_matrices();
    const gpudef::CaseParityInput input =
        make_lbs_case_input(skin, matrices, gpudef::QuantizationMode::fp32);
    auto receipt = gpudef::measure_case_throughput(input, 20U, "gpudef-emulation 1.0");
    require(static_cast<bool>(receipt), "throughput capture succeeds");
    if (!receipt) return;
    require(receipt.value().slots.size() == 1U, "capture produces one slot");
    require(receipt.value().slots[0U].repetitions == 20U,
        "capture records the declared repetition count");
    require(receipt.value().slots[0U].vertex_count == 3U,
        "capture records the workload vertex count");
    require(receipt.value().slots[0U].window_index < gpudef::kThroughputWindowCount,
        "measured throughput lands inside the declared window table");
    require(receipt.value().tier == gpudef::EvidenceTier::cpu_reference,
        "throughput capture stays cpu-reference");
    require(!gpudef::require_evidence_tier(
            receipt.value(), gpudef::EvidenceTier::native_device),
        "native throughput claim without named hardware fails closed");
    require(receipt.value().serialize().find("wall") == std::string::npos,
        "throughput receipt text carries no wall-time field");

    // Measurement evidence stays out of the identity: rebinning the window
    // or re-measuring must not mint a new digest.
    auto digest_a = receipt.value().canonical_digest();
    gpudef::ThroughputReceipt rebinned = receipt.value();
    rebinned.slots[0U].window_index = gpudef::kThroughputWindowCount - 1U;
    auto digest_b = rebinned.canonical_digest();
    require(static_cast<bool>(digest_a) && static_cast<bool>(digest_b) &&
            digest_a.value().bytes == digest_b.value().bytes,
        "throughput canonical digest excludes the measured window");

    // Protocol shape is identity: changing repetitions mints a new digest.
    gpudef::ThroughputReceipt reshaped = receipt.value();
    reshaped.slots[0U].repetitions = 21U;
    auto digest_c = reshaped.canonical_digest();
    require(!digest_c ||
            digest_c.value().bytes != digest_a.value().bytes,
        "changing the repetition protocol changes the identity");

    // Round-trip.
    const std::string serialized = receipt.value().serialize();
    require(serialized.find("wall") == std::string::npos,
        "serialized throughput receipt carries no wall-time field");
    auto parsed = gpudef::ThroughputReceipt::deserialize(serialized);
    require(static_cast<bool>(parsed), "throughput receipt deserializes");
    if (parsed) {
        auto digest_parsed = parsed.value().canonical_digest();
        require(static_cast<bool>(digest_parsed) &&
                digest_parsed.value().bytes == digest_a.value().bytes,
            "throughput receipt digest survives serialization roundtrip");
    }
    require(!gpudef::ThroughputReceipt::deserialize("garbage"),
        "hostile throughput receipt text fails closed");

    // CPU capture can never mint native-device evidence: attaching a named
    // profile to the capture call is refused outright.
    gpudef::CaseParityInput device_claim = input;
    device_claim.device_profile = gpudef::DeviceProfile{
        "adapter-0", "Reference GPU", "driver 1.0", "Test OS", "fp32-lbs"};
    auto refused = gpudef::measure_case_throughput(
        device_claim, 20U, "gpudef-emulation 1.0");
    require(!refused, "throughput capture with a device profile fails closed");
    if (!refused) {
        require(refused.error().code == core::ErrorCode::unsupported,
            "CPU-capture native claim refusal is explicit");
    }
    require(!gpudef::measure_case_throughput(input, 0U, "gpudef-emulation 1.0"),
        "zero repetitions fail closed");
}

void test_capability_summary_receipt() {
    const std::vector<gpudef::DeformationCase> all_cases{
        gpudef::DeformationCase::linear_blend_skinning,
        gpudef::DeformationCase::dual_quaternion_skinning,
        gpudef::DeformationCase::morph_stage,
        gpudef::DeformationCase::corrective_driver,
    };

    // With no receipts every feature stays honestly CPU-reference-only.
    auto empty = gpudef::summarize_capabilities(all_cases, {}, {});
    require(static_cast<bool>(empty), "empty summary builds");
    if (!empty) return;
    require(empty.value().features.size() == 4U,
        "summary reports every requested feature");
    require(empty.value().tier == gpudef::EvidenceTier::cpu_reference,
        "receipt-less summary stays cpu-reference");
    for (const gpudef::FeatureCapability& feature : empty.value().features) {
        require(feature.evidence ==
                gpudef::FeatureEvidence::cpu_reference_only,
            "feature without receipts stays cpu-reference-only");
        require(feature.receipt_count == 0U &&
                feature.latest_receipt_digest.is_zero(),
            "unbound feature carries no receipt count or digest");
    }
    require(!gpudef::require_evidence_tier(
            empty.value(), gpudef::EvidenceTier::native_device),
        "native summary claim without evidence fails closed");

    // No features at all is a fail-closed error, not an empty claim.
    require(!gpudef::summarize_capabilities({}, {}, {}),
        "summary without features fails closed");

    // Bind measured LBS and morph receipts; the other features stay honest.
    const FixtureSkin skin = make_skin();
    const std::vector<gpudef::Mat34> matrices = rigid_skin_matrices();
    const gpudef::CaseParityInput lbs_input =
        make_lbs_case_input(skin, matrices, gpudef::QuantizationMode::fp32);
    auto lbs_receipt = gpudef::run_case_parity(lbs_input);
    require(static_cast<bool>(lbs_receipt), "summary LBS receipt runs");

    gpudef::CaseParityInput morph_input;
    morph_input.deformation_case = gpudef::DeformationCase::morph_stage;
    morph_input.skin = skin.view();
    const std::vector<gpudef::MorphDeltaBinding> deltas{{1U, {0.5, 0.5, 0.5}}};
    morph_input.morph_deltas = deltas;
    morph_input.morph_weight = 1.0;
    auto morph_receipt = gpudef::run_case_parity(morph_input);
    require(static_cast<bool>(morph_receipt), "summary morph receipt runs");
    if (!lbs_receipt || !morph_receipt) return;

    const gpudef::CaseParityReceipt bound_receipts[] = {
        lbs_receipt.value(), morph_receipt.value()};
    auto partial = gpudef::summarize_capabilities(
        all_cases, bound_receipts, {});
    require(static_cast<bool>(partial), "partial summary builds");
    if (!partial) return;
    require(partial.value().tier == gpudef::EvidenceTier::cpu_reference,
        "mixed-evidence summary stays cpu-reference");
    for (const gpudef::FeatureCapability& feature : partial.value().features) {
        const bool expect_bound =
            feature.feature == gpudef::DeformationCase::linear_blend_skinning ||
            feature.feature == gpudef::DeformationCase::morph_stage;
        require((feature.evidence == gpudef::FeatureEvidence::packed_parity) ==
                expect_bound,
            "summary upgrades only features with bound receipts");
        require(feature.receipt_count == (expect_bound ? 1U : 0U),
            "summary receipt counts match the bound evidence");
        require(feature.latest_receipt_digest.is_zero() == !expect_bound,
            "summary digests attach exactly to bound features");
    }

    // Round-trip preserves the summary identity.
    const std::string serialized = partial.value().serialize();
    auto parsed = gpudef::CapabilitySummaryReceipt::deserialize(serialized);
    require(static_cast<bool>(parsed), "capability summary deserializes");
    if (parsed) {
        auto digest_a = partial.value().canonical_digest();
        auto digest_b = parsed.value().canonical_digest();
        require(static_cast<bool>(digest_a) && static_cast<bool>(digest_b) &&
                digest_a.value().bytes == digest_b.value().bytes,
            "capability summary digest survives serialization roundtrip");
    }
    require(!gpudef::CapabilitySummaryReceipt::deserialize("garbage"),
        "hostile capability summary text fails closed");

    // Native evidence: a named device LBS receipt upgrades the summary only
    // when every reported feature carries native evidence; reporting the
    // whole feature set with a single native receipt stays honest.
    gpudef::CaseParityInput device_input = lbs_input;
    device_input.reference_is_device_measured = true;
    device_input.device_profile = gpudef::DeviceProfile{
        "adapter-0", "Reference GPU", "driver 1.0", "Test OS", "fp32-lbs"};
    auto native_receipt = gpudef::run_case_parity(device_input);
    require(static_cast<bool>(native_receipt), "native summary receipt runs");
    if (!native_receipt) return;
    const std::vector<gpudef::DeformationCase> lbs_only{
        gpudef::DeformationCase::linear_blend_skinning};
    const gpudef::CaseParityReceipt native_receipts[] = {native_receipt.value()};
    auto native_summary = gpudef::summarize_capabilities(
        lbs_only, native_receipts, {});
    require(static_cast<bool>(native_summary), "native summary builds");
    if (native_summary) {
        require(native_summary.value().tier == gpudef::EvidenceTier::native_device,
            "all-native evidence upgrades the summary tier");
        require(static_cast<bool>(gpudef::require_evidence_tier(
                native_summary.value(), gpudef::EvidenceTier::native_device)),
            "native summary requirement passes with all-native evidence");
    }
    const gpudef::CaseParityReceipt mixed_receipts[] = {
        native_receipt.value(), morph_receipt.value()};
    auto mixed = gpudef::summarize_capabilities(all_cases, mixed_receipts, {});
    require(static_cast<bool>(mixed), "mixed summary builds");
    if (mixed) {
        require(mixed.value().tier == gpudef::EvidenceTier::cpu_reference,
            "one cpu-reference receipt keeps the whole summary cpu-reference");
    }

    // A summary never reaches integrated product.
    require(!gpudef::claim_integrated_product(),
        "capability summary integrated-product claim stays an honest refusal");
}

} // namespace

int main() {
    test_pack_unpack_fp32_exact();
    test_fp16_error_bounded_by_receipt();
    test_unorm16_error_bounded_by_receipt();
    test_layout_digests_differ();
    test_pack_refuses_overcapacity();
    test_emulation_matches_hand_computed_joint();
    test_matrix_inverse_roundtrip();
    test_parity_harness_cpu_tier();
    test_parity_determinism();
    test_perf_receipt_honesty();
    test_packed_buffer_serialization_roundtrip();
    test_hostile_inputs();
    test_half_codec_known_values();
    test_case_parity_lbs_exact_and_deterministic();
    test_case_parity_dqs();
    test_case_parity_morph_stage();
    test_case_parity_corrective_driver();
    test_envelope_admission_and_rejection();
    test_case_receipt_serialization_roundtrip();
    test_throughput_receipt_windows();
    test_capability_summary_receipt();

    if (failures != 0) {
        std::cerr << failures << " gpudef test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "gpudef tests passed\n";
    return EXIT_SUCCESS;
}
