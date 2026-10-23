#pragma once

// Internal serialization and validation helpers shared by the carto::physical
// records. This header is not a public contract surface; it exists so every
// record in the library frames, parses, and digests itself identically.

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/surface/process.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>

namespace carto::physical::detail {

inline core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

inline core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline core::Diagnostic version_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
}

inline core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

inline bool finite_nonnegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

inline bool finite_positive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

inline bool finite_unit(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

// Printable, single-line text without control bytes. Quotes and backslashes
// are handled by std::quoted on the wire; embedded newlines are never valid.
inline bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

inline std::ostream& write_revision(std::ostream& output, core::Revision revision) {
    output << revision.value();
    return output;
}

template <typename T>
bool read_integer(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(
        token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    value = parsed;
    return true;
}

inline bool read_revision(std::istream& input, core::Revision& value) {
    std::uint64_t parsed = 0U;
    if (!read_integer(input, parsed)) return false;
    value = core::Revision{parsed};
    return true;
}

inline bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

// Bounded collection counts: refuse hostile magnitudes before any allocation.
inline bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_integer(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

inline bool read_bool(std::istream& input, bool& value) {
    unsigned encoded = 0U;
    if (!read_integer(input, encoded) || encoded > 1U) return false;
    value = encoded != 0U;
    return true;
}

inline bool read_double(std::istream& input, double& value) {
    if (!(input >> value)) return false;
    return std::isfinite(value);
}

inline std::ostream& write_digest(
    std::ostream& output, const std::optional<assets::Sha256Digest>& digest) {
    output << (digest.has_value() ? digest->hex() : std::string{"-"});
    return output;
}

inline bool read_digest(
    std::istream& input, std::optional<assets::Sha256Digest>& digest) {
    std::string token;
    if (!(input >> token)) return false;
    if (token == "-") {
        digest = std::nullopt;
        return true;
    }
    const auto parsed = assets::Sha256Digest::from_hex(token);
    if (!parsed) return false;
    digest = parsed.value();
    return true;
}

inline std::ostream& write_digest(
    std::ostream& output, const assets::Sha256Digest& digest) {
    output << digest.hex();
    return output;
}

inline bool read_digest(std::istream& input, assets::Sha256Digest& digest) {
    std::optional<assets::Sha256Digest> parsed;
    if (!read_digest(input, parsed) || !parsed.has_value()) return false;
    digest = parsed.value();
    return true;
}

// Rejects a framing header whose magic or schema version does not match the
// exact record version. Callers own the magic string.
inline core::Result<void> read_frame(
    std::istream& input, std::string_view magic, std::uint32_t schema_version) {
    std::string found_magic;
    std::uint32_t found_version = 0U;
    if (!(input >> found_magic) || !read_integer(input, found_version)) {
        return core::Result<void>::failure(parse_error(
            std::string(magic) + " framing is incomplete"));
    }
    if (found_magic != magic) {
        return core::Result<void>::failure(parse_error(
            std::string(magic) + " framing magic is invalid"));
    }
    if (found_version != schema_version) {
        return core::Result<void>::failure(version_error(
            std::string(magic) + " schema version is unsupported"));
    }
    return core::Result<void>::success();
}

inline core::Result<void> read_end(std::istream& input, std::string_view magic) {
    if (!require_token(input, "END")) {
        return core::Result<void>::failure(parse_error(
            std::string(magic) + " is missing its END record"));
    }
    return core::Result<void>::success();
}

inline core::Result<void> check_text_size(
    std::string_view text, std::size_t maximum, std::string_view label) {
    if (text.empty() || text.size() > maximum) {
        return core::Result<void>::failure(parse_error(
            std::string(label) + " text is empty or exceeds its byte bound"));
    }
    return core::Result<void>::success();
}

// Symmetric 3x3 tensor in upper-triangular storage. Used for inertia tensors;
// symmetry is a type invariant so serialization can never introduce skew.
struct SymmetricTensor3 {
    double xx = 0.0;
    double xy = 0.0;
    double xz = 0.0;
    double yy = 0.0;
    double yz = 0.0;
    double zz = 0.0;

    [[nodiscard]] bool finite() const noexcept {
        return std::isfinite(xx) && std::isfinite(xy) && std::isfinite(xz) &&
            std::isfinite(yy) && std::isfinite(yz) && std::isfinite(zz);
    }

    [[nodiscard]] bool positive_diagonal() const noexcept {
        return xx > 0.0 && yy > 0.0 && zz > 0.0;
    }
};

inline std::ostream& write_tensor(std::ostream& output, const SymmetricTensor3& tensor) {
    output << tensor.xx << ' ' << tensor.xy << ' ' << tensor.xz << ' '
           << tensor.yy << ' ' << tensor.yz << ' ' << tensor.zz;
    return output;
}

inline bool read_tensor(std::istream& input, SymmetricTensor3& tensor) {
    return read_double(input, tensor.xx) && read_double(input, tensor.xy) &&
        read_double(input, tensor.xz) && read_double(input, tensor.yy) &&
        read_double(input, tensor.yz) && read_double(input, tensor.zz);
}

// Shared framing for embedded carto::surface::MaterialStratigraphy evidence.
void encode_stratigraphy(
    std::ostream& output, const surface::MaterialStratigraphy& stratigraphy);
core::Result<void> decode_stratigraphy(
    std::istream& input, surface::MaterialStratigraphy& stratigraphy);

} // namespace carto::physical::detail
