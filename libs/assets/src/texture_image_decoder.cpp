#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <carto/assets/texture_image_decoder.hpp>

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::assets {
namespace {

using Microsoft::WRL::ComPtr;

core::Diagnostic decode_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic unavailable(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

std::uint32_t read_be32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
        static_cast<std::uint32_t>(bytes[offset + 3U]);
}

class PngIdatByteReader final {
public:
    explicit PngIdatByteReader(std::span<const std::uint8_t> encoded)
        : encoded_(encoded) {}

    [[nodiscard]] bool read(std::uint8_t& value) {
        for (;;) {
            if (remaining_ != 0U) {
                value = encoded_[data_offset_++];
                --remaining_;
                return true;
            }
            if (finished_ || cursor_ >= encoded_.size() ||
                encoded_.size() - cursor_ < 12U) {
                return false;
            }
            const std::uint32_t chunk_size = read_be32(encoded_, cursor_);
            if (chunk_size > encoded_.size() - cursor_ - 12U) {
                finished_ = true;
                return false;
            }
            const std::size_t type_offset = cursor_ + 4U;
            const std::size_t next_offset = cursor_ + 12U + chunk_size;
            const std::string_view type{
                reinterpret_cast<const char*>(encoded_.data() + type_offset), 4U};
            data_offset_ = cursor_ + 8U;
            cursor_ = next_offset;
            if (type == "IDAT") {
                saw_idat_ = true;
                remaining_ = chunk_size;
                continue;
            }
            if (saw_idat_) finished_ = true;
        }
    }

private:
    std::span<const std::uint8_t> encoded_;
    std::size_t cursor_ = 8U;
    std::size_t data_offset_ = 0U;
    std::size_t remaining_ = 0U;
    bool saw_idat_ = false;
    bool finished_ = false;
};

class PngDeflateBitReader final {
public:
    explicit PngDeflateBitReader(PngIdatByteReader& input) : input_(input) {}

    [[nodiscard]] bool read_bits(unsigned count, std::uint32_t& value) {
        if (count > 24U) return false;
        while (available_bits_ < count) {
            std::uint8_t next = 0U;
            if (!input_.read(next)) return false;
            bit_buffer_ |= static_cast<std::uint64_t>(next) << available_bits_;
            available_bits_ += 8U;
        }
        const std::uint64_t mask = count == 0U
            ? 0U : ((std::uint64_t{1U} << count) - 1U);
        value = static_cast<std::uint32_t>(bit_buffer_ & mask);
        bit_buffer_ >>= count;
        available_bits_ -= count;
        return true;
    }

    void align_to_byte() noexcept {
        const unsigned discard = available_bits_ % 8U;
        bit_buffer_ >>= discard;
        available_bits_ -= discard;
    }

    [[nodiscard]] bool read_aligned_byte(std::uint8_t& value) {
        if (available_bits_ != 0U) return false;
        return input_.read(value);
    }

    [[nodiscard]] bool has_trailing_idat_byte() {
        std::uint8_t ignored = 0U;
        return input_.read(ignored);
    }

private:
    PngIdatByteReader& input_;
    std::uint64_t bit_buffer_ = 0U;
    unsigned available_bits_ = 0U;
};

class PngHuffmanTable final {
public:
    [[nodiscard]] bool build(std::span<const std::uint8_t> lengths,
                             bool allow_empty = false,
                             bool allow_single_symbol = false) {
        if (lengths.size() > symbols_.size()) return false;
        counts_.fill(0U);
        first_codes_.fill(0U);
        first_symbols_.fill(0U);
        symbol_count_ = 0U;
        for (const std::uint8_t length : lengths) {
            if (length > kMaxCodeLength) return false;
            if (length != 0U) ++counts_[length];
        }
        for (unsigned length = 1U; length <= kMaxCodeLength; ++length) {
            symbol_count_ = static_cast<std::uint16_t>(
                symbol_count_ + counts_[length]);
        }
        if (symbol_count_ == 0U) return allow_empty;

        std::int32_t remaining_codes = 1;
        for (unsigned length = 1U; length <= kMaxCodeLength; ++length) {
            remaining_codes = (remaining_codes << 1) - counts_[length];
            if (remaining_codes < 0) return false;
        }
        // Literal/distance alphabets may use one one-bit symbol; code-length
        // alphabets and all other nonempty trees must fill the code space.
        if (remaining_codes != 0 &&
            !(allow_single_symbol && symbol_count_ == 1U && counts_[1U] == 1U)) return false;

        std::uint32_t next_code = 0U;
        std::uint16_t symbol_offset = 0U;
        for (unsigned length = 1U; length <= kMaxCodeLength; ++length) {
            next_code = (next_code + counts_[length - 1U]) << 1U;
            first_codes_[length] = next_code;
            first_symbols_[length] = symbol_offset;
            symbol_offset = static_cast<std::uint16_t>(
                symbol_offset + counts_[length]);
        }

        std::uint16_t output = 0U;
        for (unsigned length = 1U; length <= kMaxCodeLength; ++length) {
            for (std::size_t symbol = 0U; symbol < lengths.size(); ++symbol) {
                if (lengths[symbol] == length) {
                    symbols_[output++] = static_cast<std::uint16_t>(symbol);
                }
            }
        }
        return output == symbol_count_;
    }

    [[nodiscard]] bool decode(PngDeflateBitReader& bits, std::uint16_t& symbol) const {
        if (symbol_count_ == 0U) return false;
        std::uint32_t code = 0U;
        for (unsigned length = 1U; length <= kMaxCodeLength; ++length) {
            std::uint32_t bit = 0U;
            if (!bits.read_bits(1U, bit)) return false;
            code = (code << 1U) | bit;
            const std::uint32_t first = first_codes_[length];
            const std::uint32_t count = counts_[length];
            if (code >= first && code - first < count) {
                const std::uint32_t index = first_symbols_[length] + code - first;
                if (index >= symbol_count_) return false;
                symbol = symbols_[index];
                return true;
            }
        }
        return false;
    }

private:
    static constexpr unsigned kMaxCodeLength = 15U;
    std::array<std::uint16_t, kMaxCodeLength + 1U> counts_{};
    std::array<std::uint32_t, kMaxCodeLength + 1U> first_codes_{};
    std::array<std::uint16_t, kMaxCodeLength + 1U> first_symbols_{};
    std::array<std::uint16_t, 288U> symbols_{};
    std::uint16_t symbol_count_ = 0U;
};

struct PngPassLayout {
    std::uint64_t rows = 0U;
    std::uint64_t row_bytes = 0U;
};

struct PngScanlineLayout {
    std::array<PngPassLayout, 7U> passes{};
    std::size_t pass_count = 0U;
    std::uint64_t expected_bytes = 0U;
};

bool make_png_scanline_layout(
    std::span<const std::uint8_t> encoded,
    PngScanlineLayout& layout) {
    if (encoded.size() < 29U || read_be32(encoded, 8U) != 13U ||
        std::string_view(reinterpret_cast<const char*>(encoded.data() + 12U), 4U) !=
            "IHDR") {
        return false;
    }
    const std::uint32_t width = read_be32(encoded, 16U);
    const std::uint32_t height = read_be32(encoded, 20U);
    const std::uint8_t bit_depth = encoded[24U];
    const std::uint8_t color_type = encoded[25U];
    const std::uint8_t interlace = encoded[28U];
    std::uint32_t channels = 0U;
    bool valid_bit_depth = false;
    switch (color_type) {
    case 0U:
        channels = 1U;
        valid_bit_depth = bit_depth == 1U || bit_depth == 2U || bit_depth == 4U ||
            bit_depth == 8U || bit_depth == 16U;
        break;
    case 2U:
        channels = 3U;
        valid_bit_depth = bit_depth == 8U || bit_depth == 16U;
        break;
    case 3U:
        channels = 1U;
        valid_bit_depth = bit_depth == 1U || bit_depth == 2U || bit_depth == 4U ||
            bit_depth == 8U;
        break;
    case 4U:
        channels = 2U;
        valid_bit_depth = bit_depth == 8U || bit_depth == 16U;
        break;
    case 6U:
        channels = 4U;
        valid_bit_depth = bit_depth == 8U || bit_depth == 16U;
        break;
    default: return false;
    }
    if (width == 0U || height == 0U || !valid_bit_depth ||
        (interlace != 0U && interlace != 1U)) {
        return false;
    }
    const std::uint64_t bits_per_pixel =
        static_cast<std::uint64_t>(channels) * bit_depth;
    const auto pass_extent = [](std::uint32_t full_extent,
                                std::uint32_t start,
                                std::uint32_t step) -> std::uint64_t {
        if (full_extent <= start) return 0U;
        return (static_cast<std::uint64_t>(full_extent - start) + step - 1U) / step;
    };
    const auto append_pass = [&layout, bits_per_pixel](
        std::uint64_t pass_width, std::uint64_t pass_height) {
        if (pass_width == 0U || pass_height == 0U ||
            layout.pass_count >= layout.passes.size()) return true;
        if (pass_width > (std::numeric_limits<std::uint64_t>::max() - 7U) /
                bits_per_pixel) return false;
        const std::uint64_t row_bits = pass_width * bits_per_pixel;
        const std::uint64_t row_bytes = (row_bits + 7U) / 8U;
        if (row_bytes == std::numeric_limits<std::uint64_t>::max()) return false;
        const std::uint64_t filtered_row_bytes = row_bytes + 1U;
        if (pass_height >
            (std::numeric_limits<std::uint64_t>::max() - layout.expected_bytes) /
                filtered_row_bytes) return false;
        layout.expected_bytes += pass_height * filtered_row_bytes;
        layout.passes[layout.pass_count++] = {pass_height, row_bytes};
        return true;
    };

    if (interlace == 0U) {
        if (!append_pass(width, height)) return false;
    } else {
        constexpr std::array<std::uint32_t, 7U> start_x{0U, 4U, 0U, 2U, 0U, 1U, 0U};
        constexpr std::array<std::uint32_t, 7U> start_y{0U, 0U, 4U, 0U, 2U, 0U, 1U};
        constexpr std::array<std::uint32_t, 7U> step_x{8U, 8U, 4U, 4U, 2U, 2U, 1U};
        constexpr std::array<std::uint32_t, 7U> step_y{8U, 8U, 8U, 4U, 4U, 2U, 2U};
        for (std::size_t pass = 0U; pass < start_x.size(); ++pass) {
            if (!append_pass(
                    pass_extent(width, start_x[pass], step_x[pass]),
                    pass_extent(height, start_y[pass], step_y[pass]))) return false;
        }
    }
    return layout.pass_count != 0U && layout.expected_bytes != 0U;
}

class PngScanlineTracker final {
public:
    explicit PngScanlineTracker(const PngScanlineLayout& layout) : layout_(layout) {
        advance_to_nonempty_pass();
    }

    [[nodiscard]] bool consume(std::uint8_t value) {
        if (complete_ || pass_index_ >= layout_.pass_count) return false;
        const auto& pass = layout_.passes[pass_index_];
        if (filter_pending_) {
            if (value > 4U) return false;
            filter_pending_ = false;
            bytes_in_row_ = pass.row_bytes;
            if (bytes_in_row_ == 0U) finish_row();
            return true;
        }
        if (bytes_in_row_ == 0U) return false;
        --bytes_in_row_;
        if (bytes_in_row_ == 0U) finish_row();
        return true;
    }

    [[nodiscard]] bool complete() const noexcept { return complete_; }

private:
    void finish_row() {
        ++row_index_;
        if (row_index_ >= layout_.passes[pass_index_].rows) {
            ++pass_index_;
            row_index_ = 0U;
            advance_to_nonempty_pass();
        } else {
            filter_pending_ = true;
        }
    }

    void advance_to_nonempty_pass() {
        while (pass_index_ < layout_.pass_count &&
            layout_.passes[pass_index_].rows == 0U) {
            ++pass_index_;
        }
        complete_ = pass_index_ >= layout_.pass_count;
        filter_pending_ = !complete_;
        bytes_in_row_ = 0U;
    }

    const PngScanlineLayout& layout_;
    std::size_t pass_index_ = 0U;
    std::uint64_t row_index_ = 0U;
    std::uint64_t bytes_in_row_ = 0U;
    bool filter_pending_ = true;
    bool complete_ = false;
};

class PngInflatedImage final {
public:
    PngInflatedImage(const PngScanlineLayout& layout, std::uint32_t window_size)
        : expected_bytes_(layout.expected_bytes),
          window_size_(window_size), tracker_(layout) {}

    [[nodiscard]] bool emit(std::uint8_t value) {
        if (written_ >= expected_bytes_ || !tracker_.consume(value)) return false;
        history_[history_position_] = value;
        history_position_ = (history_position_ + 1U) & (history_.size() - 1U);
        adler_a_ += value;
        if (adler_a_ >= 65521U) adler_a_ -= 65521U;
        adler_b_ += adler_a_;
        if (adler_b_ >= 65521U) adler_b_ -= 65521U;
        ++written_;
        return true;
    }

    [[nodiscard]] bool copy(std::uint32_t distance, std::uint32_t length) {
        if (distance == 0U || distance > window_size_ ||
            distance > history_.size() || distance > written_ ||
            length > expected_bytes_ - written_) return false;
        for (std::uint32_t index = 0U; index < length; ++index) {
            const std::size_t source = (history_position_ + history_.size() - distance) &
                (history_.size() - 1U);
            if (!emit(history_[source])) return false;
        }
        return true;
    }

    [[nodiscard]] bool complete() const noexcept {
        return written_ == expected_bytes_ && tracker_.complete();
    }
    [[nodiscard]] std::uint32_t adler32() const noexcept {
        return (adler_b_ << 16U) | adler_a_;
    }

private:
    static constexpr std::size_t kHistoryBytes = 32'768U;
    static_assert((kHistoryBytes & (kHistoryBytes - 1U)) == 0U);
    std::array<std::uint8_t, kHistoryBytes> history_{};
    std::uint64_t expected_bytes_ = 0U;
    std::uint64_t written_ = 0U;
    std::uint32_t window_size_ = 0U;
    std::size_t history_position_ = 0U;
    std::uint32_t adler_a_ = 1U;
    std::uint32_t adler_b_ = 0U;
    PngScanlineTracker tracker_;
};

bool build_fixed_huffman_tables(
    PngHuffmanTable& literal_length,
    PngHuffmanTable& distance) {
    std::array<std::uint8_t, 288U> literal_lengths{};
    std::fill(literal_lengths.begin(), literal_lengths.begin() + 144, 8U);
    std::fill(literal_lengths.begin() + 144, literal_lengths.begin() + 256, 9U);
    std::fill(literal_lengths.begin() + 256, literal_lengths.begin() + 280, 7U);
    std::fill(literal_lengths.begin() + 280, literal_lengths.end(), 8U);
    std::array<std::uint8_t, 32U> distance_lengths{};
    distance_lengths.fill(5U);
    return literal_length.build(literal_lengths) && distance.build(distance_lengths);
}

bool build_dynamic_huffman_tables(
    PngDeflateBitReader& bits,
    PngHuffmanTable& literal_length,
    PngHuffmanTable& distance) {
    std::uint32_t field = 0U;
    if (!bits.read_bits(5U, field)) return false;
    const std::uint32_t literal_count = field + 257U;
    if (literal_count > 286U || !bits.read_bits(5U, field)) return false;
    const std::uint32_t distance_count = field + 1U;
    if (distance_count > 32U || !bits.read_bits(4U, field)) return false;
    const std::uint32_t code_length_count = field + 4U;
    constexpr std::array<std::uint8_t, 19U> code_order{
        16U, 17U, 18U, 0U, 8U, 7U, 9U, 6U, 10U, 5U,
        11U, 4U, 12U, 3U, 13U, 2U, 14U, 1U, 15U};
    std::array<std::uint8_t, 19U> code_length_lengths{};
    for (std::uint32_t index = 0U; index < code_length_count; ++index) {
        if (!bits.read_bits(3U, field)) return false;
        code_length_lengths[code_order[index]] = static_cast<std::uint8_t>(field);
    }
    PngHuffmanTable code_length_table;
    if (!code_length_table.build(code_length_lengths)) return false;

    const std::size_t combined_count =
        static_cast<std::size_t>(literal_count + distance_count);
    std::array<std::uint8_t, 318U> combined_lengths{};
    std::size_t output = 0U;
    while (output < combined_count) {
        std::uint16_t symbol = 0U;
        if (!code_length_table.decode(bits, symbol)) return false;
        if (symbol <= 15U) {
            combined_lengths[output++] = static_cast<std::uint8_t>(symbol);
            continue;
        }
        std::uint32_t extra = 0U;
        std::uint32_t repeat = 0U;
        std::uint8_t repeated_length = 0U;
        if (symbol == 16U) {
            if (output == 0U || !bits.read_bits(2U, extra)) return false;
            repeat = extra + 3U;
            repeated_length = combined_lengths[output - 1U];
        } else if (symbol == 17U) {
            if (!bits.read_bits(3U, extra)) return false;
            repeat = extra + 3U;
        } else if (symbol == 18U) {
            if (!bits.read_bits(7U, extra)) return false;
            repeat = extra + 11U;
        } else {
            return false;
        }
        if (repeat > combined_count - output) return false;
        std::fill_n(combined_lengths.begin() + static_cast<std::ptrdiff_t>(output),
            repeat, repeated_length);
        output += repeat;
    }

    if (combined_lengths[256U] == 0U) return false;
    return literal_length.build(std::span<const std::uint8_t>{combined_lengths}.first(
                literal_count), false, true) &&
        distance.build(std::span<const std::uint8_t>{combined_lengths}.subspan(
                literal_count, distance_count), true, true);
}

bool inflate_png_huffman_block(
    PngDeflateBitReader& bits,
    const PngHuffmanTable& literal_length,
    const PngHuffmanTable& distance,
    PngInflatedImage& image) {
    constexpr std::array<std::uint16_t, 29U> length_bases{
        3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 13U,
        15U, 17U, 19U, 23U, 27U, 31U, 35U, 43U, 51U, 59U,
        67U, 83U, 99U, 115U, 131U, 163U, 195U, 227U, 258U};
    constexpr std::array<std::uint8_t, 29U> length_extra_bits{
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U,
        1U, 1U, 2U, 2U, 2U, 2U, 3U, 3U, 3U, 3U,
        4U, 4U, 4U, 4U, 5U, 5U, 5U, 5U, 0U};
    constexpr std::array<std::uint32_t, 30U> distance_bases{
        1U, 2U, 3U, 4U, 5U, 7U, 9U, 13U, 17U, 25U,
        33U, 49U, 65U, 97U, 129U, 193U, 257U, 385U, 513U, 769U,
        1025U, 1537U, 2049U, 3073U, 4097U, 6145U, 8193U, 12289U,
        16385U, 24577U};
    constexpr std::array<std::uint8_t, 30U> distance_extra_bits{
        0U, 0U, 0U, 0U, 1U, 1U, 2U, 2U, 3U, 3U,
        4U, 4U, 5U, 5U, 6U, 6U, 7U, 7U, 8U, 8U,
        9U, 9U, 10U, 10U, 11U, 11U, 12U, 12U, 13U, 13U};

    for (;;) {
        std::uint16_t symbol = 0U;
        if (!literal_length.decode(bits, symbol)) return false;
        if (symbol <= 255U) {
            if (!image.emit(static_cast<std::uint8_t>(symbol))) return false;
            continue;
        }
        if (symbol == 256U) return true;
        if (symbol < 257U || symbol > 285U) return false;
        const std::size_t length_index = symbol - 257U;
        std::uint32_t extra = 0U;
        if (length_extra_bits[length_index] != 0U &&
            !bits.read_bits(length_extra_bits[length_index], extra)) return false;
        const std::uint32_t length = length_bases[length_index] + extra;

        std::uint16_t distance_symbol = 0U;
        if (!distance.decode(bits, distance_symbol) || distance_symbol >= 30U) return false;
        extra = 0U;
        if (distance_extra_bits[distance_symbol] != 0U &&
            !bits.read_bits(distance_extra_bits[distance_symbol], extra)) return false;
        const std::uint32_t distance_value = distance_bases[distance_symbol] + extra;
        if (!image.copy(distance_value, length)) return false;
    }
}

core::Result<void> validate_png_deflate_stream(
    std::span<const std::uint8_t> encoded) {
    using Result = core::Result<void>;
    PngScanlineLayout layout;
    if (!make_png_scanline_layout(encoded, layout)) {
        return Result::failure(decode_error(
            "PNG scanline layout is invalid or exceeds its bounded source dimensions"));
    }

    PngIdatByteReader input(encoded);
    std::uint8_t cmf = 0U;
    std::uint8_t flg = 0U;
    if (!input.read(cmf) || !input.read(flg)) {
        return Result::failure(decode_error("PNG zlib header is truncated"));
    }
    const std::uint16_t zlib_header = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(cmf) << 8U) | flg);
    if ((cmf & 0x0fU) != 8U || (cmf >> 4U) > 7U || zlib_header % 31U != 0U ||
        (flg & 0x20U) != 0U) {
        return Result::failure(decode_error("PNG zlib header is invalid"));
    }

    PngDeflateBitReader bits(input);
    const std::uint32_t window_size = std::uint32_t{1U} << ((cmf >> 4U) + 8U);
    PngInflatedImage image(layout, window_size);
    PngHuffmanTable fixed_literal_lengths;
    PngHuffmanTable fixed_distance_lengths;
    if (!build_fixed_huffman_tables(fixed_literal_lengths, fixed_distance_lengths)) {
        return Result::failure(decode_error("PNG fixed DEFLATE tables are invalid"));
    }

    bool final_block = false;
    while (!final_block) {
        std::uint32_t final_flag = 0U;
        std::uint32_t block_type = 0U;
        if (!bits.read_bits(1U, final_flag) || !bits.read_bits(2U, block_type)) {
            return Result::failure(decode_error("PNG DEFLATE block header is truncated"));
        }
        final_block = final_flag != 0U;
        if (block_type == 0U) {
            bits.align_to_byte();
            std::uint32_t length = 0U;
            std::uint32_t inverse_length = 0U;
            if (!bits.read_bits(16U, length) || !bits.read_bits(16U, inverse_length) ||
                ((length ^ 0xffffU) & 0xffffU) != inverse_length) {
                return Result::failure(decode_error(
                    "PNG uncompressed DEFLATE block length is invalid"));
            }
            for (std::uint32_t index = 0U; index < length; ++index) {
                std::uint32_t value = 0U;
                if (!bits.read_bits(8U, value) ||
                    !image.emit(static_cast<std::uint8_t>(value))) {
                    return Result::failure(decode_error(
                        "PNG uncompressed DEFLATE block exceeds its scanline layout"));
                }
            }
            continue;
        }
        if (block_type == 3U) {
            return Result::failure(decode_error(
                "PNG DEFLATE stream uses a reserved block type"));
        }

        if (block_type == 1U) {
            if (!inflate_png_huffman_block(
                    bits, fixed_literal_lengths, fixed_distance_lengths, image)) {
                return Result::failure(decode_error(
                    "PNG fixed-Huffman DEFLATE block is malformed"));
            }
            continue;
        }

        PngHuffmanTable literal_length;
        PngHuffmanTable distance;
        if (!build_dynamic_huffman_tables(bits, literal_length, distance) ||
            !inflate_png_huffman_block(bits, literal_length, distance, image)) {
            return Result::failure(decode_error(
                "PNG dynamic-Huffman DEFLATE block is malformed"));
        }
    }

    bits.align_to_byte();
    std::array<std::uint8_t, 4U> adler_bytes{};
    for (auto& byte : adler_bytes) {
        if (!bits.read_aligned_byte(byte)) {
            return Result::failure(decode_error("PNG zlib Adler-32 trailer is truncated"));
        }
    }
    const std::uint32_t expected_adler =
        (static_cast<std::uint32_t>(adler_bytes[0U]) << 24U) |
        (static_cast<std::uint32_t>(adler_bytes[1U]) << 16U) |
        (static_cast<std::uint32_t>(adler_bytes[2U]) << 8U) |
        static_cast<std::uint32_t>(adler_bytes[3U]);
    if (!image.complete() || image.adler32() != expected_adler) {
        return Result::failure(decode_error(
            "PNG zlib stream has an invalid checksum or scanline byte count"));
    }
    if (bits.has_trailing_idat_byte()) {
        return Result::failure(decode_error(
            "PNG IDAT stream contains bytes after its zlib trailer"));
    }
    return Result::success();
}

// Huffman admission checks coded blocks, not their pixel values. Other JPEG
// processes retain the existing codec path and are not validated here.
struct JpegHuffmanTable {
    std::array<std::uint16_t, 16U> counts{};
    std::array<std::uint8_t, 256U> symbols{};
    bool present = false;
};

class JpegEntropyReader final {
public:
    JpegEntropyReader(std::span<const std::uint8_t> bytes, std::size_t offset)
        : bytes_(bytes), cursor_(offset) {}

    bool bit(unsigned& value) {
        if (remaining_ == 0U) {
            if (cursor_ >= bytes_.size()) return false;
            current_ = bytes_[cursor_++];
            if (current_ == 0xffU) {
                if (cursor_ >= bytes_.size() || bytes_[cursor_] != 0U) return false;
                ++cursor_;
            }
            remaining_ = 8U;
        }
        value = (current_ >> --remaining_) & 1U;
        return true;
    }

    bool skip(unsigned count) {
        unsigned value = 0U;
        for (unsigned i = 0U; i < count; ++i) if (!bit(value)) return false;
        return true;
    }

    bool read(unsigned count, unsigned& value) {
        value = 0U;
        for (unsigned i = 0U; i < count; ++i) {
            unsigned next = 0U;
            if (!bit(next)) return false;
            value = (value << 1U) | next;
        }
        return true;
    }

    bool align() {
        if (remaining_ != 0U &&
            (current_ & ((1U << remaining_) - 1U)) != ((1U << remaining_) - 1U)) {
            return false;
        }
        remaining_ = 0U;
        return true;
    }

    bool restart(unsigned expected) {
        if (!align() || cursor_ >= bytes_.size() || bytes_[cursor_++] != 0xffU) return false;
        while (cursor_ < bytes_.size() && bytes_[cursor_] == 0xffU) ++cursor_;
        if (cursor_ >= bytes_.size() || bytes_[cursor_++] != 0xd0U + expected) return false;
        return true;
    }

    bool end_scan(std::size_t& marker_offset) {
        if (!align() || cursor_ >= bytes_.size() || bytes_[cursor_] != 0xffU) return false;
        marker_offset = cursor_;
        return true;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_;
    std::uint8_t current_ = 0U;
    unsigned remaining_ = 0U;
};

bool jpeg_huffman_symbol(JpegEntropyReader& bits, const JpegHuffmanTable& table,
                         std::uint8_t& symbol) {
    if (!table.present) return false;
    unsigned code = 0U;
    unsigned first = 0U;
    unsigned index = 0U;
    for (unsigned length = 0U; length < 16U; ++length) {
        unsigned value = 0U;
        if (!bits.bit(value)) return false;
        code = (code << 1U) | value;
        const unsigned count = table.counts[length];
        if (code >= first && code - first < count) {
            symbol = table.symbols[index + code - first];
            return true;
        }
        index += count;
        first = (first + count) << 1U;
    }
    return false;
}

bool jpeg_baseline_block(JpegEntropyReader& bits, const JpegHuffmanTable& dc,
                         const JpegHuffmanTable& ac) {
    std::uint8_t symbol = 0U;
    if (!jpeg_huffman_symbol(bits, dc, symbol) || symbol > 11U || !bits.skip(symbol)) return false;
    unsigned coefficient = 1U;
    while (coefficient < 64U) {
        if (!jpeg_huffman_symbol(bits, ac, symbol)) return false;
        const unsigned run = symbol >> 4U;
        const unsigned size = symbol & 15U;
        if (size == 0U) {
            if (run == 0U) return true;
            if (run != 15U || coefficient + 16U > 64U) return false;
            coefficient += 16U;
        } else {
            if (size > 10U || coefficient + run >= 64U || !bits.skip(size)) return false;
            coefficient += run + 1U;
        }
    }
    return true;
}

bool jpeg_progressive_ac_block(JpegEntropyReader& bits, const JpegHuffmanTable& ac,
                               std::uint64_t& nonzero, unsigned start, unsigned end,
                               bool refinement, std::uint32_t& eob_run) {
    auto refine_remaining = [&](unsigned from) {
        for (unsigned k = from; k <= end; ++k) {
            if ((nonzero & (std::uint64_t{1U} << k)) != 0U && !bits.skip(1U)) return false;
        }
        return true;
    };
    if (eob_run != 0U) {
        if (refinement && !refine_remaining(start)) return false;
        --eob_run;
        return true;
    }
    unsigned k = start;
    while (k <= end) {
        std::uint8_t symbol = 0U;
        if (!jpeg_huffman_symbol(bits, ac, symbol)) return false;
        const unsigned run = symbol >> 4U;
        const unsigned size = symbol & 15U;
        if (size == 0U && run < 15U) {
            unsigned extra = 0U;
            if (!bits.read(run, extra)) return false;
            eob_run = (std::uint32_t{1U} << run) + extra - 1U;
            return !refinement || refine_remaining(k);
        }
        if (!refinement) {
            if (size == 0U) {
                if (run != 15U || k + 16U > end + 1U) return false;
                k += 16U;
            } else {
                if (size > 10U || k + run > end || !bits.skip(size)) return false;
                k += run;
                nonzero |= std::uint64_t{1U} << k++;
            }
            continue;
        }
        if (size > 1U || (size == 0U && run != 15U)) return false;
        if (size == 1U && !bits.skip(1U)) return false; // sign of new coefficient
        unsigned zeros = size == 0U ? 16U : run;
        bool placed = false;
        while (k <= end) {
            const auto mask = std::uint64_t{1U} << k;
            if ((nonzero & mask) != 0U) {
                if (!bits.skip(1U)) return false;
                ++k;
            } else if (zeros != 0U) {
                --zeros;
                ++k;
                if (size == 0U && zeros == 0U) break;
            } else {
                nonzero |= mask;
                ++k;
                placed = true;
                break;
            }
        }
        if (zeros != 0U || (size == 1U && !placed)) return false;
    }
    return true;
}

core::Result<void> validate_jpeg_huffman_entropy(std::span<const std::uint8_t> bytes) {
    using Result = core::Result<void>;
    const auto fail = [] { return Result::failure(decode_error(
        "JPEG Huffman entropy or scan progression is invalid or truncated")); };
    struct Component {
        std::uint8_t id = 0U, h = 0U, v = 0U;
        bool scanned = false;
        std::array<std::int8_t, 64U> approximation{};
        std::uint64_t stride = 0U, rows = 0U;
        std::vector<std::uint64_t> nonzero;
    };
    std::array<Component, 4U> components{};
    std::array<JpegHuffmanTable, 4U> dc{}, ac{};
    unsigned component_count = 0U, max_h = 0U, max_v = 0U, restart_interval = 0U;
    unsigned width = 0U, height = 0U;
    bool baseline = false, progressive = false;
    const auto ceil_div = [](std::uint64_t n, std::uint64_t d) { return (n + d - 1U) / d; };
    std::size_t cursor = 2U;
    while (cursor < bytes.size()) {
        if (bytes[cursor++] != 0xffU) return fail();
        while (cursor < bytes.size() && bytes[cursor] == 0xffU) ++cursor;
        if (cursor >= bytes.size()) return fail();
        const auto marker = bytes[cursor++];
        if (marker == 0xd9U) {
            if (!baseline && !progressive) return Result::success();
            for (unsigned i = 0U; i < component_count; ++i) {
                if ((baseline && !components[i].scanned) ||
                    (progressive && components[i].approximation[0U] < 0)) return fail();
            }
            return Result::success();
        }
        // Other frame processes retain their existing WIC behavior.
        if (marker == 0xc1U || marker == 0xc3U ||
            (marker >= 0xc5U && marker <= 0xc7U) ||
            (marker >= 0xc9U && marker <= 0xcbU) ||
            (marker >= 0xcdU && marker <= 0xcfU)) return Result::success();
        if (bytes.size() - cursor < 2U) return fail();
        const unsigned length = (static_cast<unsigned>(bytes[cursor]) << 8U) | bytes[cursor + 1U];
        if (length < 2U || length > bytes.size() - cursor) return fail();
        const std::size_t end = cursor + length;
        if (marker == 0xc0U || marker == 0xc2U) {
            if (length < 11U) return fail();
            // SOF2 permits other precision; preserve the previous WIC route
            // rather than applying the eight-bit Huffman limits to it.
            if (marker == 0xc2U && bytes[cursor + 2U] != 8U) return Result::success();
            if (bytes[cursor + 2U] != 8U) return fail();
            height = (static_cast<unsigned>(bytes[cursor + 3U]) << 8U) | bytes[cursor + 4U];
            width = (static_cast<unsigned>(bytes[cursor + 5U]) << 8U) | bytes[cursor + 6U];
            component_count = bytes[cursor + 7U];
            if (component_count == 0U || component_count > 4U || length != 8U + 3U * component_count) return fail();
            for (unsigned i = 0U; i < component_count; ++i) {
                auto& c = components[i];
                c.id = bytes[cursor + 8U + 3U * i];
                c.h = bytes[cursor + 9U + 3U * i] >> 4U;
                c.v = bytes[cursor + 9U + 3U * i] & 15U;
                c.approximation.fill(-1);
                if (c.h == 0U || c.h > 4U || c.v == 0U || c.v > 4U) return fail();
                max_h = std::max(max_h, static_cast<unsigned>(c.h));
                max_v = std::max(max_v, static_cast<unsigned>(c.v));
            }
            baseline = marker == 0xc0U;
            progressive = marker == 0xc2U;
            for (unsigned i = 0U; i < component_count; ++i) {
                components[i].stride = ceil_div(width, 8U * max_h) * components[i].h;
                components[i].rows = ceil_div(height, 8U * max_v) * components[i].v;
            }
        } else if (marker == 0xc4U) {
            std::size_t p = cursor + 2U;
            while (p < end) {
                const unsigned id = bytes[p++];
                if ((id >> 4U) > 1U || (id & 15U) > 3U || end - p < 16U) return fail();
                auto& table = (id >> 4U) == 0U ? dc[id & 15U] : ac[id & 15U];
                unsigned total = 0U, available = 1U;
                for (unsigned i = 0U; i < 16U; ++i) {
                    table.counts[i] = bytes[p++];
                    available *= 2U;
                    if (table.counts[i] > available) return fail();
                    available -= table.counts[i];
                    total += table.counts[i];
                }
                // JPEG reserves the all-ones code so scan padding cannot be a symbol.
                if (total == 0U || total > 256U || total > end - p || available == 0U) return fail();
                for (unsigned i = 0U; i < total; ++i) table.symbols[i] = bytes[p++];
                table.present = true;
            }
        } else if (marker == 0xddU) {
            if (length != 4U) return fail();
            restart_interval = (static_cast<unsigned>(bytes[cursor + 2U]) << 8U) | bytes[cursor + 3U];
        } else if (marker == 0xdaU) {
            if (!baseline && !progressive) return Result::success();
            const unsigned count = bytes[cursor + 2U];
            if (count == 0U || count > component_count || length != 6U + 2U * count) return fail();
            const unsigned start = bytes[end - 3U], finish = bytes[end - 2U];
            const unsigned high = bytes[end - 1U] >> 4U, low = bytes[end - 1U] & 15U;
            if (baseline && (start != 0U || finish != 63U || high != 0U || low != 0U)) return fail();
            if (progressive && (start > finish || finish > 63U || low > 13U || high > 13U ||
                (high != 0U && high != low + 1U) ||
                (start == 0U && finish != 0U) || (start != 0U && count != 1U))) return fail();
            std::array<unsigned, 4U> selected{}, dc_ids{}, ac_ids{};
            unsigned blocks_per_mcu = 0U;
            for (unsigned i = 0U; i < count; ++i) {
                const auto id = bytes[cursor + 3U + 2U * i];
                unsigned j = 0U;
                while (j < component_count && components[j].id != id) ++j;
                if (j == component_count || (baseline && components[j].scanned)) return fail();
                if (baseline) components[j].scanned = true;
                selected[i] = j;
                const auto tables = bytes[cursor + 4U + 2U * i];
                dc_ids[i] = tables >> 4U; ac_ids[i] = tables & 15U;
                if (dc_ids[i] > 3U || ac_ids[i] > 3U) return fail();
                blocks_per_mcu += components[j].h * components[j].v;
                if (progressive) {
                    auto& component = components[j];
                    if (start != 0U && component.approximation[0U] < 0) return fail();
                    for (unsigned k = start; k <= finish; ++k) {
                        const int previous = component.approximation[k];
                        if ((high == 0U && previous != -1) ||
                            (high != 0U && previous != static_cast<int>(high))) return fail();
                    }
                    if (start != 0U && component.nonzero.empty()) {
                        const std::uint64_t blocks = component.stride * component.rows;
                        if (blocks > component.nonzero.max_size()) return fail();
                        try {
                            component.nonzero.resize(static_cast<std::size_t>(blocks), 0U);
                        } catch (const std::bad_alloc&) {
                            return Result::failure(decode_error("JPEG progressive coefficient state allocation failed"));
                        }
                    }
                }
            }
            if (count > 1U && blocks_per_mcu > 10U) return fail();
            const auto& first = components[selected[0U]];
            const std::uint64_t columns = count == 1U ? ceil_div(static_cast<std::uint64_t>(width) * first.h, 8U * max_h) : ceil_div(width, 8U * max_h);
            const std::uint64_t rows = count == 1U ? ceil_div(static_cast<std::uint64_t>(height) * first.v, 8U * max_v) : ceil_div(height, 8U * max_v);
            const std::uint64_t mcus = columns * rows;
            JpegEntropyReader bits(bytes, end);
            unsigned restart_number = 0U;
            std::uint32_t eob_run = 0U;
            for (std::uint64_t mcu = 0U; mcu < mcus; ++mcu) {
                if (restart_interval != 0U && mcu != 0U && mcu % restart_interval == 0U) {
                    if (eob_run != 0U || !bits.restart(restart_number)) return fail();
                    restart_number = (restart_number + 1U) & 7U;
                }
                for (unsigned i = 0U; i < count; ++i) {
                    auto& c = components[selected[i]];
                    const unsigned blocks = count == 1U ? 1U : c.h * c.v;
                    for (unsigned block = 0U; block < blocks; ++block) {
                        if (baseline) {
                            if (!jpeg_baseline_block(bits, dc[dc_ids[i]], ac[ac_ids[i]])) return fail();
                        } else if (start == 0U) {
                            if (high != 0U) {
                                if (!bits.skip(1U)) return fail();
                            } else {
                                std::uint8_t symbol = 0U;
                                if (!jpeg_huffman_symbol(bits, dc[dc_ids[i]], symbol) ||
                                    symbol > 11U || !bits.skip(symbol)) return fail();
                            }
                        } else {
                            const std::uint64_t index = (mcu / columns) * c.stride + (mcu % columns);
                            if (index >= c.nonzero.size() ||
                                !jpeg_progressive_ac_block(bits, ac[ac_ids[i]],
                                    c.nonzero[static_cast<std::size_t>(index)], start, finish,
                                    high != 0U, eob_run)) return fail();
                        }
                    }
                }
            }
            if (eob_run != 0U || !bits.end_scan(cursor)) return fail();
            if (progressive) {
                for (unsigned i = 0U; i < count; ++i) {
                    for (unsigned k = start; k <= finish; ++k)
                        components[selected[i]].approximation[k] = static_cast<std::int8_t>(low);
                }
            }
            continue;
        }
        cursor = end;
    }
    return fail();
}

class ComApartment final {
public:
    ComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
        initialized_here_ = SUCCEEDED(result_);
    }

    ~ComApartment() {
        if (initialized_here_) CoUninitialize();
    }

    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_ = E_FAIL;
    bool initialized_here_ = false;
};

} // namespace

static core::Result<DecodedRgba8Texture> decode_texture_image_wic(
    std::vector<std::uint8_t> encoded_source,
    const TextureSourceLimits& limits) {
    auto inspected = inspect_texture_source(encoded_source, {}, limits);
    if (!inspected) {
        return core::Result<DecodedRgba8Texture>::failure(
            inspected.error().with_context("texture-image container admission"));
    }
    if (inspected.value().format != TextureSourceFormat::png &&
        inspected.value().format != TextureSourceFormat::jpeg) {
        return core::Result<DecodedRgba8Texture>::failure(decode_error(
            "Windows texture-image decoder accepts only admitted PNG or JPEG sources"));
    }
    if (encoded_source.size() > std::numeric_limits<DWORD>::max()) {
        return core::Result<DecodedRgba8Texture>::failure(decode_error(
            "texture-image source exceeds the Windows codec stream limit"));
    }

    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(inspected.value().width) * inspected.value().height;
    if (pixel_count > std::numeric_limits<std::uint64_t>::max() / 4U) {
        return core::Result<DecodedRgba8Texture>::failure(decode_error(
            "decoded source image byte count overflows the bounded RGBA8 size"));
    }
    const std::uint64_t output_bytes = pixel_count * 4U;
    if (pixel_count == 0U || pixel_count > limits.max_pixels ||
        output_bytes > std::numeric_limits<UINT>::max() ||
        output_bytes > std::numeric_limits<std::size_t>::max()) {
        return core::Result<DecodedRgba8Texture>::failure(decode_error(
            "decoded source image exceeds the bounded RGBA8 allocation limit"));
    }
    if (inspected.value().format == TextureSourceFormat::png) {
        const auto deflate_validation = validate_png_deflate_stream(encoded_source);
        if (!deflate_validation) {
            return core::Result<DecodedRgba8Texture>::failure(
                deflate_validation.error().with_context("PNG DEFLATE admission"));
        }
    } else if (inspected.value().format == TextureSourceFormat::jpeg) {
        const auto entropy = validate_jpeg_huffman_entropy(encoded_source);
        if (!entropy) {
            return core::Result<DecodedRgba8Texture>::failure(
                entropy.error().with_context("JPEG Huffman entropy admission"));
        }
    }

    ComApartment apartment;
    if (FAILED(apartment.result()) && apartment.result() != RPC_E_CHANGED_MODE) {
        return core::Result<DecodedRgba8Texture>::failure(unavailable(
            "Windows Imaging Component could not initialize COM on this thread"));
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT status = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(status)) {
        return core::Result<DecodedRgba8Texture>::failure(unavailable(
            "Windows Imaging Component factory is unavailable"));
    }

    ComPtr<IWICStream> stream;
    status = factory->CreateStream(stream.GetAddressOf());
    if (FAILED(status)) {
        return core::Result<DecodedRgba8Texture>::failure(unavailable(
            "Windows Imaging Component could not create an in-memory stream"));
    }
    status = stream->InitializeFromMemory(encoded_source.data(),
        static_cast<DWORD>(encoded_source.size()));
    if (FAILED(status)) {
        return core::Result<DecodedRgba8Texture>::failure(decode_error(
            "Windows Imaging Component rejected the verified image byte stream"));
    }

    ComPtr<IWICBitmapDecoder> decoder;
    status = factory->CreateDecoderFromStream(stream.Get(), nullptr,
        WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
    if (FAILED(status)) {
        return core::Result<DecodedRgba8Texture>::failure(decode_error(
            "Windows Imaging Component could not decode the admitted image"));
    }
    UINT frame_count = 0U;
    status = decoder->GetFrameCount(&frame_count);
    if (FAILED(status) || frame_count != 1U) {
        return core::Result<DecodedRgba8Texture>::failure(decode_error(
            "texture-image decoder requires exactly one image frame"));
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    status = decoder->GetFrame(0U, frame.GetAddressOf());
    if (FAILED(status)) {
        return core::Result<DecodedRgba8Texture>::failure(decode_error(
            "Windows Imaging Component could not read the image frame"));
    }
    UINT width = 0U;
    UINT height = 0U;
    status = frame->GetSize(&width, &height);
    if (FAILED(status) || width != inspected.value().width ||
        height != inspected.value().height) {
        return core::Result<DecodedRgba8Texture>::failure(decode_error(
            "decoded image dimensions do not match the admitted source header"));
    }

    ComPtr<IWICFormatConverter> converter;
    status = factory->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(status)) {
        return core::Result<DecodedRgba8Texture>::failure(unavailable(
            "Windows Imaging Component could not create an RGBA8 converter"));
    }
    status = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(status)) {
        return core::Result<DecodedRgba8Texture>::failure(decode_error(
            "Windows Imaging Component could not convert this image to RGBA8"));
    }

    DecodedRgba8Texture decoded;
    decoded.admission = inspected.value();
    decoded.admission.stage = TextureSourceStage::decoded;
    decoded.admission.channels = 4U;
    decoded.admission.bit_depth = 8U;
    decoded.pixels.resize(static_cast<std::size_t>(output_bytes));
    const UINT stride = width * 4U;
    status = converter->CopyPixels(nullptr, stride,
        static_cast<UINT>(output_bytes), decoded.pixels.data());
    if (FAILED(status)) {
        return core::Result<DecodedRgba8Texture>::failure(decode_error(
            "Windows Imaging Component could not copy bounded RGBA8 pixels"));
    }
    if (auto valid = decoded.validate(limits); !valid) {
        return core::Result<DecodedRgba8Texture>::failure(valid.error());
    }
    return core::Result<DecodedRgba8Texture>::success(std::move(decoded));
}

core::Result<DecodedRgba8Texture> decode_texture_image_rgba8(
    std::vector<std::uint8_t> encoded_source,
    const Sha256Digest& expected_digest,
    std::uint32_t expected_width,
    std::uint32_t expected_height,
    const TextureSourceLimits& limits) {
    auto inspected = inspect_texture_source(encoded_source, {}, limits);
    if (!inspected) {
        return core::Result<DecodedRgba8Texture>::failure(
            inspected.error().with_context("texture-image source admission"));
    }
    if (inspected.value().source.digest != expected_digest ||
        inspected.value().width != expected_width ||
        inspected.value().height != expected_height) {
        return core::Result<DecodedRgba8Texture>::failure(stale(
            "texture-image pixels do not match the current source binding"));
    }
    if (inspected.value().has_color_encoding_metadata) {
        return core::Result<DecodedRgba8Texture>::failure({core::ErrorCode::unsupported,
            "texture-image color metadata is unsupported by the RGBA8 decode path"});
    }

    core::Result<DecodedRgba8Texture> decoded = [&]() {
        switch (inspected.value().format) {
        case TextureSourceFormat::tga:
            return decode_tga_rgba8(encoded_source, limits);
        case TextureSourceFormat::png:
        case TextureSourceFormat::jpeg:
            return decode_texture_image_wic(std::move(encoded_source), limits);
        case TextureSourceFormat::dds:
        case TextureSourceFormat::exr:
            return core::Result<DecodedRgba8Texture>::failure(decode_error(
                "texture-image pixel decoding currently supports PNG, JPEG, and TGA"));
        }
        return core::Result<DecodedRgba8Texture>::failure(decode_error(
            "texture-image source format is unsupported"));
    }();
    if (!decoded) return decoded;
    if (decoded.value().admission.source.digest != expected_digest ||
        decoded.value().admission.width != expected_width ||
        decoded.value().admission.height != expected_height) {
        return core::Result<DecodedRgba8Texture>::failure(stale(
            "decoded texture-image pixels no longer match the source binding"));
    }
    return decoded;
}

} // namespace carto::assets
#else

#include <carto/assets/texture_image_decoder.hpp>

namespace carto::assets {

core::Result<DecodedRgba8Texture> decode_texture_image_rgba8(
    std::vector<std::uint8_t> encoded_source,
    const Sha256Digest& expected_digest,
    std::uint32_t expected_width,
    std::uint32_t expected_height,
    const TextureSourceLimits& limits) {
    auto inspected = inspect_texture_source(encoded_source, {}, limits);
    if (!inspected) return core::Result<DecodedRgba8Texture>::failure(inspected.error());
    if (inspected.value().source.digest != expected_digest ||
        inspected.value().width != expected_width ||
        inspected.value().height != expected_height) {
        return core::Result<DecodedRgba8Texture>::failure({core::ErrorCode::stale_data,
            "image bytes do not match their current source binding"});
    }
    if (inspected.value().has_color_encoding_metadata) {
        return core::Result<DecodedRgba8Texture>::failure({core::ErrorCode::unsupported,
            "texture-image color metadata is unsupported by the RGBA8 decode path"});
    }
    if (inspected.value().format == TextureSourceFormat::tga) {
        return decode_tga_rgba8(encoded_source, limits);
    }
    return core::Result<DecodedRgba8Texture>::failure({core::ErrorCode::unsupported,
        "PNG/JPEG pixel decoding requires Windows Imaging Component"});
}

} // namespace carto::assets
#endif
