#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace carto::plugin_package {

namespace {

constexpr int64_t kFeMask = (1LL << 51) - 1;
using Fe = std::array<int64_t, 5>;
constexpr std::array<std::uint8_t, 32> kFieldPrimeBytes{
    0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f};
constexpr Fe kTwoP{
    (2LL << 51) - 38,
    (2LL << 51) - 2,
    (2LL << 51) - 2,
    (2LL << 51) - 2,
    (2LL << 51) - 2};
constexpr Fe kP{
    (1LL << 51) - 19,
    (1LL << 51) - 1,
    (1LL << 51) - 1,
    (1LL << 51) - 1,
    (1LL << 51) - 1};

constexpr Fe fe_0() { return Fe{0, 0, 0, 0, 0}; }
constexpr Fe fe_1() { return Fe{1, 0, 0, 0, 0}; }

void fe_carry(Fe& h) {
    for (std::size_t i = 0U; i < 4U; ++i) {
        int64_t c = h[i] >> 51;
        h[i] &= kFeMask;
        h[i + 1] += c;
    }
    int64_t c = h[4] >> 51;
    h[4] &= kFeMask;
    h[0] += 19 * c;
}

void fe_reduce(Fe& h) {
    for (std::size_t i = 0U; i < 4U; ++i) fe_carry(h);
    while (true) {
        bool greater_or_equal = true;
        for (std::size_t i = 5U; i-- > 0U;) {
            if (h[i] != kP[i]) {
                greater_or_equal = h[i] > kP[i];
                break;
            }
        }
        if (!greater_or_equal) break;
        int64_t borrow = 0;
        for (std::size_t i = 0U; i < 5U; ++i) {
            const int64_t before = h[i];
            h[i] = before - kP[i] - borrow;
            borrow = (before < kP[i]) || (borrow != 0 && before == kP[i]);
        }
    }
}

void fe_add(Fe& out, const Fe& f, const Fe& g) {
    for (std::size_t i = 0U; i < 5U; ++i) out[i] = f[i] + g[i];
    fe_reduce(out);
}

void fe_sub(Fe& out, const Fe& f, const Fe& g) {
    for (std::size_t i = 0U; i < 5U; ++i) out[i] = f[i] - g[i] + kTwoP[i];
    fe_reduce(out);
}

void fe_neg(Fe& out, const Fe& f) {
    fe_sub(out, fe_0(), f);
}

void fe_mul(Fe& out, const Fe& f, const Fe& g) {
    __int128 t[10] = {};
    for (std::size_t i = 0U; i < 5U; ++i) {
        for (std::size_t j = 0U; j < 5U; ++j) {
            t[i + j] += static_cast<__int128>(f[i]) * static_cast<__int128>(g[j]);
        }
    }
    t[0] += 19 * t[5];
    t[1] += 19 * t[6];
    t[2] += 19 * t[7];
    t[3] += 19 * t[8];
    t[4] += 19 * t[9];
    Fe h;
    for (std::size_t i = 0U; i < 4U; ++i) {
        int64_t c = static_cast<int64_t>(t[i] >> 51);
        h[i] = static_cast<int64_t>(t[i] & kFeMask);
        t[i + 1] += c;
    }
    int64_t c = static_cast<int64_t>(t[4] >> 51);
    h[4] = static_cast<int64_t>(t[4] & kFeMask);
    h[0] += 19 * c;
    fe_reduce(h);
    out = h;
}

void fe_sq(Fe& out, const Fe& f) {
    fe_mul(out, f, f);
}

void fe_invert(Fe& out, const Fe& z) {
    // p - 2 = 2^255 - 21.  A fixed square-and-multiply chain is slower than
    // the addition-chain version, but keeps this small implementation easy to
    // audit and avoids carrying an unverified hand-transcription of ref10.
    Fe result = fe_1();
    for (int bit = 254; bit >= 0; --bit) {
        fe_sq(result, result);
        if (bit != 2 && bit != 4) fe_mul(result, result, z);
    }
    out = result;
}

void fe_pow22523(Fe& out, const Fe& z) {
    // (p - 5) / 8 = 2^252 - 3.
    Fe result = fe_1();
    for (int bit = 251; bit >= 0; --bit) {
        fe_sq(result, result);
        if (bit != 1) fe_mul(result, result, z);
    }
    out = result;
}

bool fe_isnegative(const Fe& f) {
    Fe t = f;
    fe_reduce(t);
    return (t[0] & 1) != 0;
}

bool fe_isnonzero(const Fe& f) {
    Fe t = f;
    fe_reduce(t);
    return t[0] != 0 || t[1] != 0 || t[2] != 0 || t[3] != 0 || t[4] != 0;
}

void fe_tobytes(std::array<std::uint8_t, 32>& s, const Fe& h) {
    Fe t = h;
    fe_reduce(t);
    int64_t carry = 0;
    for (std::size_t i = 0U; i < 5U; ++i) {
        t[i] += carry;
        carry = t[i] >> 51;
        t[i] &= kFeMask;
    }
    t[0] += 19 * carry;
    fe_reduce(t);

    s[0] = static_cast<std::uint8_t>(t[0] & 0xff);
    s[1] = static_cast<std::uint8_t>((t[0] >> 8) & 0xff);
    s[2] = static_cast<std::uint8_t>((t[0] >> 16) & 0xff);
    s[3] = static_cast<std::uint8_t>((t[0] >> 24) & 0xff);
    s[4] = static_cast<std::uint8_t>((t[0] >> 32) & 0xff);
    s[5] = static_cast<std::uint8_t>((t[0] >> 40) & 0xff);
    s[6] = static_cast<std::uint8_t>(((t[0] >> 48) & 0x7) | ((t[1] & 0x1f) << 3));
    s[7] = static_cast<std::uint8_t>((t[1] >> 5) & 0xff);
    s[8] = static_cast<std::uint8_t>((t[1] >> 13) & 0xff);
    s[9] = static_cast<std::uint8_t>((t[1] >> 21) & 0xff);
    s[10] = static_cast<std::uint8_t>((t[1] >> 29) & 0xff);
    s[11] = static_cast<std::uint8_t>((t[1] >> 37) & 0xff);
    s[12] = static_cast<std::uint8_t>(((t[1] >> 45) & 0x3f) | ((t[2] & 0x3) << 6));
    s[13] = static_cast<std::uint8_t>((t[2] >> 2) & 0xff);
    s[14] = static_cast<std::uint8_t>((t[2] >> 10) & 0xff);
    s[15] = static_cast<std::uint8_t>((t[2] >> 18) & 0xff);
    s[16] = static_cast<std::uint8_t>((t[2] >> 26) & 0xff);
    s[17] = static_cast<std::uint8_t>((t[2] >> 34) & 0xff);
    s[18] = static_cast<std::uint8_t>((t[2] >> 42) & 0xff);
    s[19] = static_cast<std::uint8_t>(((t[2] >> 50) & 0x1) | ((t[3] & 0x7f) << 1));
    s[20] = static_cast<std::uint8_t>((t[3] >> 7) & 0xff);
    s[21] = static_cast<std::uint8_t>((t[3] >> 15) & 0xff);
    s[22] = static_cast<std::uint8_t>((t[3] >> 23) & 0xff);
    s[23] = static_cast<std::uint8_t>((t[3] >> 31) & 0xff);
    s[24] = static_cast<std::uint8_t>((t[3] >> 39) & 0xff);
    s[25] = static_cast<std::uint8_t>(((t[3] >> 47) & 0xf) | ((t[4] & 0xf) << 4));
    s[26] = static_cast<std::uint8_t>((t[4] >> 4) & 0xff);
    s[27] = static_cast<std::uint8_t>((t[4] >> 12) & 0xff);
    s[28] = static_cast<std::uint8_t>((t[4] >> 20) & 0xff);
    s[29] = static_cast<std::uint8_t>((t[4] >> 28) & 0xff);
    s[30] = static_cast<std::uint8_t>((t[4] >> 36) & 0xff);
    s[31] = static_cast<std::uint8_t>((t[4] >> 44) & 0x7f);
}

bool fe_frombytes(Fe& h, std::span<const std::uint8_t, 32> s) {
    std::array<std::uint8_t, 32> canonical = {};
    std::copy(s.begin(), s.end(), canonical.begin());
    canonical[31] &= 0x7fU;
    for (std::size_t index = canonical.size(); index-- > 0U;) {
        if (canonical[index] != kFieldPrimeBytes[index]) {
            if (canonical[index] > kFieldPrimeBytes[index]) return false;
            break;
        }
    }
    h[0] = static_cast<int64_t>(s[0]) |
        (static_cast<int64_t>(s[1]) << 8) |
        (static_cast<int64_t>(s[2]) << 16) |
        (static_cast<int64_t>(s[3]) << 24) |
        (static_cast<int64_t>(s[4]) << 32) |
        (static_cast<int64_t>(s[5]) << 40) |
        ((static_cast<int64_t>(s[6]) & 0x07) << 48);
    h[1] = (static_cast<int64_t>(s[6]) >> 3) |
        (static_cast<int64_t>(s[7]) << 5) |
        (static_cast<int64_t>(s[8]) << 13) |
        (static_cast<int64_t>(s[9]) << 21) |
        (static_cast<int64_t>(s[10]) << 29) |
        (static_cast<int64_t>(s[11]) << 37) |
        ((static_cast<int64_t>(s[12]) & 0x3f) << 45);
    h[2] = (static_cast<int64_t>(s[12]) >> 6) |
        (static_cast<int64_t>(s[13]) << 2) |
        (static_cast<int64_t>(s[14]) << 10) |
        (static_cast<int64_t>(s[15]) << 18) |
        (static_cast<int64_t>(s[16]) << 26) |
        (static_cast<int64_t>(s[17]) << 34) |
        (static_cast<int64_t>(s[18]) << 42) |
        ((static_cast<int64_t>(s[19]) & 0x01) << 50);
    h[3] = (static_cast<int64_t>(s[19]) >> 1) |
        (static_cast<int64_t>(s[20]) << 7) |
        (static_cast<int64_t>(s[21]) << 15) |
        (static_cast<int64_t>(s[22]) << 23) |
        (static_cast<int64_t>(s[23]) << 31) |
        (static_cast<int64_t>(s[24]) << 39) |
        ((static_cast<int64_t>(s[25]) & 0x0f) << 47);
    h[4] = (static_cast<int64_t>(s[25]) >> 4) |
        (static_cast<int64_t>(s[26]) << 4) |
        (static_cast<int64_t>(s[27]) << 12) |
        (static_cast<int64_t>(s[28]) << 20) |
        (static_cast<int64_t>(s[29]) << 28) |
        (static_cast<int64_t>(s[30]) << 36) |
        ((static_cast<int64_t>(s[31]) & 0x7f) << 44);
    return true;
}

using ScalarWide = std::array<std::uint64_t, 5>;

constexpr std::array<std::uint8_t, 32> kScalarModulus{
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};

ScalarWide scalar_modulus_wide() {
    ScalarWide result{};
    for (unsigned index = 0U; index < 4U; ++index) {
        for (unsigned byte = 0U; byte < 8U; ++byte) {
            result[index] |= static_cast<std::uint64_t>(kScalarModulus[index * 8U + byte]) << (byte * 8U);
        }
    }
    return result;
}

bool scalar_less(const ScalarWide& left, const ScalarWide& right) noexcept {
    for (std::size_t index = left.size(); index-- > 0U;) {
        if (left[index] != right[index]) return left[index] < right[index];
    }
    return false;
}

bool scalar_is_canonical(std::span<const std::uint8_t, 32> scalar) noexcept {
    for (std::size_t index = scalar.size(); index-- > 0U;) {
        if (scalar[index] != kScalarModulus[index]) return scalar[index] < kScalarModulus[index];
    }
    return false;
}

void scalar_subtract(ScalarWide& value, const ScalarWide& subtrahend) noexcept {
    std::uint64_t borrow = 0U;
    for (std::size_t index = 0U; index < value.size(); ++index) {
        const std::uint64_t before = value[index];
        value[index] = before - subtrahend[index] - borrow;
        borrow = (before < subtrahend[index]) || (borrow != 0U && before == subtrahend[index]);
    }
}

void scalar_add_mod(ScalarWide& value, const ScalarWide& addend, const ScalarWide& modulus) noexcept {
    std::uint64_t carry = 0U;
    for (std::size_t index = 0U; index < value.size(); ++index) {
        const std::uint64_t before = value[index];
        const std::uint64_t first = before + addend[index];
        const bool first_carry = first < before;
        const std::uint64_t second = first + carry;
        const bool second_carry = second < first;
        value[index] = second;
        carry = (first_carry || second_carry) ? 1U : 0U;
    }
    if (carry != 0U || !scalar_less(value, modulus)) scalar_subtract(value, modulus);
}

ScalarWide scalar_from_bytes(std::span<const std::uint8_t, 32> bytes) {
    ScalarWide result{};
    for (unsigned index = 0U; index < 4U; ++index) {
        for (unsigned byte = 0U; byte < 8U; ++byte) {
            result[index] |= static_cast<std::uint64_t>(bytes[index * 8U + byte]) << (byte * 8U);
        }
    }
    return result;
}

void scalar_to_bytes(std::array<std::uint8_t, 32>& output, const ScalarWide& value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        for (unsigned byte = 0U; byte < 8U; ++byte) {
            output[index * 8U + byte] = static_cast<std::uint8_t>(value[index] >> (byte * 8U));
        }
    }
}

void scalar_reduce_safe(std::array<std::uint8_t, 32>& output, std::span<const std::uint8_t> input) {
    const ScalarWide modulus = scalar_modulus_wide();
    ScalarWide remainder{};
    for (std::size_t bit = input.size() * 8U; bit-- > 0U;) {
        std::uint64_t carry = static_cast<std::uint64_t>((input[bit / 8U] >> (bit % 8U)) & 1U);
        for (auto& limb : remainder) {
            const std::uint64_t next = limb >> 63U;
            limb = (limb << 1U) | carry;
            carry = next;
        }
        if (!scalar_less(remainder, modulus)) scalar_subtract(remainder, modulus);
    }
    scalar_to_bytes(output, remainder);
}

void scalar_muladd_safe(
    std::array<std::uint8_t, 32>& output,
    std::span<const std::uint8_t, 32> left,
    std::span<const std::uint8_t, 32> right,
    std::span<const std::uint8_t, 32> addend) {
    const ScalarWide modulus = scalar_modulus_wide();
    ScalarWide result{};
    std::array<std::uint8_t, 32> reduced_left{};
    std::array<std::uint8_t, 32> reduced_right{};
    std::array<std::uint8_t, 32> reduced_addend{};
    scalar_reduce_safe(reduced_left, left);
    scalar_reduce_safe(reduced_right, right);
    scalar_reduce_safe(reduced_addend, addend);
    ScalarWide factor = scalar_from_bytes(reduced_right);
    const ScalarWide addend_wide = scalar_from_bytes(reduced_addend);
    for (std::size_t bit = 0U; bit < 256U; ++bit) {
        if (((reduced_left[bit / 8U] >> (bit % 8U)) & 1U) != 0U) {
            scalar_add_mod(result, factor, modulus);
        }
        const ScalarWide previous = factor;
        scalar_add_mod(factor, previous, modulus);
    }
    scalar_add_mod(result, addend_wide, modulus);
    scalar_to_bytes(output, result);
}

struct Point {
    Fe x;
    Fe y;
};

constexpr Point identity_point() { return Point{fe_0(), fe_1()}; }

const Fe& d() {
    static Fe value{};
    static bool init = false;
    if (!init) {
        Fe num{
            kFeMask - 121683,
            kFeMask,
            kFeMask,
            kFeMask,
            kFeMask};
        Fe den{}; den[0] = 121666;
        fe_invert(den, den);
        fe_mul(value, num, den);
        init = true;
    }
    return value;
}

const Fe& sqrt_minus_1() {
    static Fe value{};
    static bool init = false;
    if (!init) {
        Fe two{}; two[0] = 2;
        Fe root;
        fe_pow22523(root, two);
        fe_sq(value, root);
        fe_mul(value, value, two);
        init = true;
    }
    return value;
}

bool point_frombytes(Point& p, std::span<const std::uint8_t, 32> s) {
    Fe y;
    if (!fe_frombytes(y, s)) return false;
    Fe y2, u, v, ratio, x, check;
    fe_sq(y2, y);
    fe_sub(u, y2, fe_1());
    v = fe_1();
    Fe d_y2;
    fe_mul(d_y2, d(), y2);
    fe_add(v, v, d_y2);
    Fe v_inverse;
    fe_invert(v_inverse, v);
    fe_mul(ratio, u, v_inverse);
    x = ratio;
    fe_pow22523(x, x);
    fe_mul(x, x, ratio);
    fe_sq(check, x);
    fe_sub(check, check, ratio);
    if (fe_isnonzero(check)) {
        fe_mul(x, x, sqrt_minus_1());
        fe_sq(check, x);
        fe_sub(check, check, ratio);
        if (fe_isnonzero(check)) return false;
    }
    if (fe_isnegative(x) != ((s[31] >> 7) & 1)) {
        fe_neg(x, x);
    }
    p.x = x;
    p.y = y;
    return true;
}

void point_tobytes(std::array<std::uint8_t, 32>& s, const Point& p) {
    fe_tobytes(s, p.y);
    s[31] |= static_cast<std::uint8_t>(fe_isnegative(p.x) << 7);
}

void point_add(Point& r, const Point& p, const Point& q) {
    Fe x1y2, y1x2, x1x2, y1y2, dprod, den_x, den_y, num_x, num_y;
    fe_mul(x1y2, p.x, q.y);
    fe_mul(y1x2, p.y, q.x);
    fe_mul(x1x2, p.x, q.x);
    fe_mul(y1y2, p.y, q.y);
    fe_mul(dprod, d(), x1x2);
    fe_mul(dprod, dprod, y1y2);
    fe_add(den_x, fe_1(), dprod);
    fe_sub(den_y, fe_1(), dprod);
    fe_add(num_x, x1y2, y1x2);
    fe_add(num_y, y1y2, x1x2);
    fe_invert(den_x, den_x);
    fe_invert(den_y, den_y);
    fe_mul(r.x, num_x, den_x);
    fe_mul(r.y, num_y, den_y);
}

void point_double(Point& r, const Point& p) {
    Fe x2, y2, num_x, den_x, num_y, den_y, dprod;
    fe_sq(x2, p.x);
    fe_sq(y2, p.y);
    fe_mul(num_x, p.x, p.y);
    num_x[0] <<= 1; num_x[1] <<= 1; num_x[2] <<= 1; num_x[3] <<= 1; num_x[4] <<= 1;
    fe_reduce(num_x);
    fe_mul(dprod, d(), x2);
    fe_mul(dprod, dprod, y2);
    fe_add(den_x, fe_1(), dprod);
    fe_sub(den_y, fe_1(), dprod);
    fe_add(num_y, y2, x2);
    fe_invert(den_x, den_x);
    fe_invert(den_y, den_y);
    fe_mul(r.x, num_x, den_x);
    fe_mul(r.y, num_y, den_y);
}

void point_negate(Point& r, const Point& p) {
    fe_neg(r.x, p.x);
    r.y = p.y;
}

Point base_point() {
    Point b;
    static constexpr std::array<std::uint8_t, 32> x_bytes{
        0x1a, 0xd5, 0x25, 0x8f, 0x60, 0x2d, 0x56, 0xc9,
        0xb2, 0xa7, 0x25, 0x95, 0x60, 0xc7, 0x2c, 0x69,
        0x5c, 0xdc, 0xd6, 0xfd, 0x31, 0xe2, 0xa4, 0xc0,
        0xfe, 0x53, 0x6e, 0xcd, 0xd3, 0x36, 0x69, 0x21};
    static constexpr std::array<std::uint8_t, 32> y_bytes{
        0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
        0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
        0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
        0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66};
    static_cast<void>(fe_frombytes(b.x, x_bytes));
    static_cast<void>(fe_frombytes(b.y, y_bytes));
    return b;
}

void point_scalarmult(Point& r, std::span<const std::uint8_t, 32> scalar, const Point& base) {
    r = identity_point();
    Point acc = base;
    for (std::size_t i = 0U; i < 256U; ++i) {
        const std::size_t byte = i / 8U;
        const unsigned bit = static_cast<unsigned>(i % 8U);
        if ((scalar[byte] >> bit) & 1) {
            point_add(r, r, acc);
        }
        point_double(acc, acc);
    }
}

bool point_is_identity(const Point& point) {
    Fe x = point.x;
    Fe y = point.y;
    fe_reduce(x);
    fe_reduce(y);
    return x == fe_0() && y == fe_1();
}

bool point_has_nontrivial_order(const Point& point) {
    std::array<std::uint8_t, 32> cofactor{};
    cofactor[0] = 8U;
    Point multiple;
    point_scalarmult(multiple, cofactor, point);
    return !point_is_identity(multiple);
}

} // namespace

std::array<std::uint8_t, 32> ed25519_public_key_from_seed(std::span<const std::uint8_t, 32> seed) {
    const auto h = sha512(seed);
    std::array<std::uint8_t, 32> a;
    std::copy(h.begin(), h.begin() + 32, a.begin());
    a[0] &= 248;
    a[31] &= 127;
    a[31] |= 64;
    Point A;
    point_scalarmult(A, a, base_point());
    std::array<std::uint8_t, 32> pk;
    point_tobytes(pk, A);
    return pk;
}

std::array<std::uint8_t, 64> ed25519_sign(
    std::span<const std::uint8_t, 32> seed,
    std::span<const std::uint8_t> message) {
    const auto h = sha512(seed);
    std::array<std::uint8_t, 32> a;
    std::copy(h.begin(), h.begin() + 32, a.begin());
    a[0] &= 248;
    a[31] &= 127;
    a[31] |= 64;

    Point A;
    point_scalarmult(A, a, base_point());
    std::array<std::uint8_t, 32> pk;
    point_tobytes(pk, A);

    std::vector<std::uint8_t> r_input(h.begin() + 32, h.end());
    r_input.insert(r_input.end(), message.begin(), message.end());
    const auto r_hash = sha512(r_input);
    std::array<std::uint8_t, 32> r;
    scalar_reduce_safe(r, r_hash);

    Point R;
    point_scalarmult(R, r, base_point());
    std::array<std::uint8_t, 32> R_bytes;
    point_tobytes(R_bytes, R);

    std::vector<std::uint8_t> k_input;
    k_input.insert(k_input.end(), R_bytes.begin(), R_bytes.end());
    k_input.insert(k_input.end(), pk.begin(), pk.end());
    k_input.insert(k_input.end(), message.begin(), message.end());
    const auto k_hash = sha512(k_input);
    std::array<std::uint8_t, 32> k;
    scalar_reduce_safe(k, k_hash);

    std::array<std::uint8_t, 64> sig;
    std::copy(R_bytes.begin(), R_bytes.end(), sig.begin());
    std::array<std::uint8_t, 32> scalar;
    scalar_muladd_safe(scalar, k, a, r);
    std::copy(scalar.begin(), scalar.end(), sig.begin() + 32);
    return sig;
}

bool ed25519_verify(
    std::span<const std::uint8_t, 32> pubkey,
    std::span<const std::uint8_t> message,
    std::span<const std::uint8_t, 64> sig) {
    if ((sig[63] & 0xe0) != 0 || !scalar_is_canonical(
        std::span<const std::uint8_t, 32>(sig.data() + 32, 32U))) return false;
    Point A, R;
    if (!point_frombytes(A, pubkey)) return false;
    if (!point_frombytes(R, std::span<const std::uint8_t, 32>(sig.data(), 32))) return false;
    if (!point_has_nontrivial_order(A) || !point_has_nontrivial_order(R)) return false;

    std::vector<std::uint8_t> k_input;
    k_input.insert(k_input.end(), sig.begin(), sig.begin() + 32);
    k_input.insert(k_input.end(), pubkey.begin(), pubkey.end());
    k_input.insert(k_input.end(), message.begin(), message.end());
    const auto k_hash = sha512(k_input);
    std::array<std::uint8_t, 32> k;
    scalar_reduce_safe(k, k_hash);

    Point sB, kA, lhs;
    point_scalarmult(sB, std::span<const std::uint8_t, 32>(sig.data() + 32, 32), base_point());
    point_scalarmult(kA, k, A);
    point_negate(kA, kA);
    point_add(lhs, sB, kA);

    std::array<std::uint8_t, 32> lhs_bytes;
    point_tobytes(lhs_bytes, lhs);
    return std::equal(lhs_bytes.begin(), lhs_bytes.end(), sig.begin());
}

} // namespace carto::plugin_package
