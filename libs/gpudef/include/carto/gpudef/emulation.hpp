#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/gpudef/layout.hpp>
#include <carto/kinematic/skin.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace carto::gpudef {

// Row-major 3x4 affine matrix: apply(p) = R * p + t, matching the linear
// blend skinning matrix convention used by GPU skinning kernels
// (skin_matrix = joint_global_pose * inverse_bind). This is the minimal
// matrix type the packed emulation kernel needs; it is not a general linear
// algebra facility.
struct Mat34 {
    // Rows 0..2, columns x, y, z, 1.
    std::array<double, 12> rows{};

    [[nodiscard]] static constexpr Mat34 identity() noexcept {
        Mat34 matrix{};
        matrix.rows = {1.0, 0.0, 0.0, 0.0,
                       0.0, 1.0, 0.0, 0.0,
                       0.0, 0.0, 1.0, 0.0};
        return matrix;
    }

    // Builds the matrix for a TRS transform with the same apply order as
    // core::Transform: rotate(scale * p) + translation.
    [[nodiscard]] static Mat34 from_transform(const core::Transform& transform) noexcept;

    [[nodiscard]] constexpr core::Vec3d apply(core::Vec3d point) const noexcept {
        return {
            rows[0] * point.x + rows[1] * point.y + rows[2] * point.z + rows[3],
            rows[4] * point.x + rows[5] * point.y + rows[6] * point.z + rows[7],
            rows[8] * point.x + rows[9] * point.y + rows[10] * point.z + rows[11],
        };
    }

    // Composition: result.apply(p) == this->apply(other.apply(p)).
    [[nodiscard]] Mat34 compose(const Mat34& other) const noexcept;

    [[nodiscard]] bool finite() const noexcept;

    // General affine inverse; fails closed when the linear part is singular
    // or near-singular relative to the given epsilon.
    [[nodiscard]] core::Result<Mat34> inverted(double epsilon = 1e-12) const noexcept;
};

// CPU reference LBS for one vertex: the weighted sum of skin-matrix
// applications in stored influence order, in double precision. This is the
// mathematical oracle every packed emulation result is compared against.
[[nodiscard]] core::Vec3d reference_deform_position(
    core::Vec3d bind_position,
    std::span<const kinematic::JointInfluence> influences,
    std::span<const Mat34> skin_matrices) noexcept;

[[nodiscard]] core::Result<std::vector<core::Vec3d>> reference_deform(
    const SkinInputView& skin,
    std::span<const Mat34> skin_matrices);

// Packed-buffer CPU emulation kernel: decodes the packed vertex stream and
// deforms it with the same reference math, so a parity harness runs headless
// with zero GPU. In fp32 mode the decode is lossless, so the emulation is
// bit-identical to reference_deform over the unpacked view (same influence
// order, same double arithmetic). In fp16/unorm16 modes the difference is
// exactly the quantization error recorded at pack time.
[[nodiscard]] core::Result<std::vector<core::Vec3d>> emulate_packed_deform(
    const PackedVertexBuffer& buffer,
    std::span<const Mat34> skin_matrices);

} // namespace carto::gpudef
