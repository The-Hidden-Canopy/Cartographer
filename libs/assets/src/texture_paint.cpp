#include <carto/assets/texture_paint.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::assets {
namespace {

constexpr std::string_view kMagic = "CARTO_TEXTURE_PAINT";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

core::Diagnostic io_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

bool valid_identity(std::string_view value) noexcept {
    return !value.empty() && value.size() <= TexturePaintDocument::kMaxIdentityBytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x21U && byte <= 0x7eU;
        });
}

bool valid_target(TexturePaintTarget target) noexcept {
    switch (target) {
    case TexturePaintTarget::red:
    case TexturePaintTarget::green:
    case TexturePaintTarget::blue:
    case TexturePaintTarget::alpha:
    case TexturePaintTarget::mask:
        return true;
    }
    return false;
}

bool valid_mode(TexturePaintMode mode) noexcept {
    return mode == TexturePaintMode::paint || mode == TexturePaintMode::erase;
}

core::Result<TextureSourceLimits> bounded_limits(
    const TextureSourceLimits& requested) {
    if (auto result = requested.validate(); !result) {
        return core::Result<TextureSourceLimits>::failure(result.error());
    }
    auto limits = requested;
    limits.max_source_bytes = std::min(
        limits.max_source_bytes, TexturePaintDocument::kMaxSourceBytes);
    limits.max_dimension = std::min(
        limits.max_dimension, TexturePaintDocument::kMaxDimension);
    limits.max_pixels = std::min(limits.max_pixels, TexturePaintDocument::kMaxPixels);
    limits.max_mip_levels = std::min(limits.max_mip_levels, 1U);
    if (auto result = limits.validate(); !result) {
        return core::Result<TextureSourceLimits>::failure(result.error());
    }
    return core::Result<TextureSourceLimits>::success(limits);
}

core::Result<TextureSourceAdmission> inspect_paint_source(
    const TexturePaintSource& source,
    const TextureSourceLimits& requested_limits) {
    auto bounded = bounded_limits(requested_limits);
    if (!bounded) return core::Result<TextureSourceAdmission>::failure(bounded.error());
    if (!valid_identity(source.identity) || source.revision.value() == 0U) {
        return core::Result<TextureSourceAdmission>::failure(invalid(
            "texture paint source identity or revision is invalid"));
    }
    auto inspected = inspect_texture_source(
        source.encoded_tga, "image/x-tga", bounded.value());
    if (!inspected) return core::Result<TextureSourceAdmission>::failure(inspected.error());
    if (source.encoded_tga.size() < 18U) {
        return core::Result<TextureSourceAdmission>::failure(invalid(
            "texture paint TGA source is shorter than its fixed header"));
    }
    if (source.encoded_tga[2U] != 2U) {
        return core::Result<TextureSourceAdmission>::failure(unsupported(
            "texture painting currently accepts uncompressed true-color TGA image type 2"));
    }
    if (inspected.value().format != TextureSourceFormat::tga ||
        (inspected.value().bit_depth != 24U && inspected.value().bit_depth != 32U)) {
        return core::Result<TextureSourceAdmission>::failure(unsupported(
            "texture painting requires 24/32-bit true-color TGA"));
    }
    const std::uint64_t pixel_bytes = inspected.value().bit_depth / 8U;
    const std::uint64_t exact_bytes = 18U + source.encoded_tga[0U] +
        static_cast<std::uint64_t>(inspected.value().width) *
            inspected.value().height * pixel_bytes;
    if (exact_bytes != source.encoded_tga.size()) {
        return core::Result<TextureSourceAdmission>::failure(invalid(
            "texture paint TGA contains incomplete or trailing pixel data"));
    }
    auto decoded = decode_tga_rgba8(source.encoded_tga, bounded.value());
    if (!decoded) return core::Result<TextureSourceAdmission>::failure(decoded.error());
    if (decoded.value().admission.width != inspected.value().width ||
        decoded.value().admission.height != inspected.value().height) {
        return core::Result<TextureSourceAdmission>::failure(stale(
            "decoded texture extent differs from its inspected source extent"));
    }
    return core::Result<TextureSourceAdmission>::success(inspected.value());
}

std::uint64_t stamp_work(
    const TexturePaintStamp& stamp,
    std::uint32_t width,
    std::uint32_t height) noexcept {
    const std::uint64_t radius = stamp.radius;
    const std::uint64_t x0 = stamp.x > radius ? stamp.x - radius : 0U;
    const std::uint64_t y0 = stamp.y > radius ? stamp.y - radius : 0U;
    const std::uint64_t x1 = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(width) - 1U,
        static_cast<std::uint64_t>(stamp.x) + radius);
    const std::uint64_t y1 = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(height) - 1U,
        static_cast<std::uint64_t>(stamp.y) + radius);
    return (x1 - x0 + 1U) * (y1 - y0 + 1U);
}

core::Result<void> validate_stamp(
    const TexturePaintStamp& stamp,
    std::uint32_t width,
    std::uint32_t height) {
    if (stamp.x >= width || stamp.y >= height || stamp.radius == 0U ||
        stamp.radius > TexturePaintDocument::kMaxBrushRadius ||
        stamp.opacity == 0U || !valid_mode(stamp.mode)) {
        return core::Result<void>::failure(invalid(
            "texture paint stamp has an invalid center, radius, opacity, or mode"));
    }
    return core::Result<void>::success();
}

std::uint64_t total_history_work(
    std::span<const TexturePaintCommand> commands,
    std::uint32_t width,
    std::uint32_t height) noexcept {
    std::uint64_t total = 0U;
    for (const auto& command : commands) {
        if (width == 0U || height == 0U || command.stamp.x >= width ||
            command.stamp.y >= height || command.stamp.radius == 0U ||
            command.stamp.radius > TexturePaintDocument::kMaxBrushRadius) {
            return TexturePaintDocument::kMaxBrushWorkPixels + 1U;
        }
        const std::uint64_t work = stamp_work(command.stamp, width, height);
        if (work > TexturePaintDocument::kMaxBrushWorkPixels - total) {
            return TexturePaintDocument::kMaxBrushWorkPixels + 1U;
        }
        total += work;
    }
    return total;
}

std::uint64_t mask_layer_count(
    std::span<const TexturePaintLayer> layers) noexcept {
    return static_cast<std::uint64_t>(std::count_if(
        layers.begin(), layers.end(), [](const TexturePaintLayer& layer) {
            return layer.target == TexturePaintTarget::mask;
        }));
}

std::vector<std::uint8_t> digest_text_bytes(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_text(std::vector<std::uint8_t>& bytes, std::string_view value) {
    append_u32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void append_digest(std::vector<std::uint8_t>& bytes, const Sha256Digest& digest) {
    bytes.insert(bytes.end(), digest.bytes.begin(), digest.bytes.end());
}

std::vector<std::uint8_t> canonical_product(const TexturePaintProduct& product) {
    std::vector<std::uint8_t> bytes;
    std::size_t reserve = product.rgba.size() + product.document_identity.size() +
        product.source_identity.size() + 2U * 32U + 64U;
    for (const auto& mask : product.masks) reserve += mask.identity.size() + mask.pixels.size() + 4U;
    bytes.reserve(reserve);
    constexpr std::string_view magic = "CARTO_TEXTURE_PAINT_PRODUCT_V1";
    bytes.insert(bytes.end(), magic.begin(), magic.end());
    bytes.push_back(0U);
    append_text(bytes, product.document_identity);
    append_u64(bytes, product.document_revision.value());
    append_text(bytes, product.source_identity);
    append_u64(bytes, product.source_revision.value());
    append_digest(bytes, product.source_digest);
    append_digest(bytes, product.document_digest);
    append_u32(bytes, product.width);
    append_u32(bytes, product.height);
    append_u32(bytes, static_cast<std::uint32_t>(product.masks.size()));
    for (const auto& mask : product.masks) {
        append_text(bytes, mask.identity);
        bytes.insert(bytes.end(), mask.pixels.begin(), mask.pixels.end());
    }
    bytes.insert(bytes.end(), product.rgba.begin(), product.rgba.end());
    return bytes;
}

} // namespace

core::Result<TexturePaintDocument> TexturePaintDocument::create(
    std::string identity,
    const TexturePaintSource& source,
    const TextureSourceLimits& limits) {
    if (!valid_identity(identity)) {
        return core::Result<TexturePaintDocument>::failure(invalid(
            "texture paint document identity is invalid"));
    }
    auto admission = inspect_paint_source(source, limits);
    if (!admission) return core::Result<TexturePaintDocument>::failure(admission.error());
    TexturePaintDocument document;
    document.identity_ = std::move(identity);
    document.revision_ = core::Revision{1U};
    document.source_identity_ = source.identity;
    document.source_revision_ = source.revision;
    document.source_digest_ = admission.value().source.digest;
    document.source_bytes_ = admission.value().source.bytes;
    document.width_ = admission.value().width;
    document.height_ = admission.value().height;
    if (auto result = document.validate(); !result) {
        return core::Result<TexturePaintDocument>::failure(result.error());
    }
    return core::Result<TexturePaintDocument>::success(std::move(document));
}

core::Result<core::Revision> TexturePaintDocument::add_layer(
    core::Revision expected_revision,
    std::string layer_identity,
    TexturePaintTarget target,
    std::uint8_t initial_value) {
    if (expected_revision != revision_) {
        return core::Result<core::Revision>::failure(stale(
            "texture paint layer edit was based on a stale document revision"));
    }
    if (revision_.exhausted()) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "texture paint document revision is exhausted"));
    }
    if (!valid_identity(layer_identity) || !valid_target(target) ||
        (target != TexturePaintTarget::mask && initial_value != 0U) ||
        layers_.size() >= kMaxLayers ||
        std::any_of(layers_.begin(), layers_.end(), [&](const auto& layer) {
            return layer.identity == layer_identity;
        })) {
        return core::Result<core::Revision>::failure(invalid(
            "texture paint layer identity, target, or bounded layer count is invalid"));
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width_) * height_;
    const std::uint64_t masks_after = mask_layer_count(layers_) +
        (target == TexturePaintTarget::mask ? 1U : 0U);
    if (pixels * 4U + masks_after * pixels > kMaxAggregateOutputBytes) {
        return core::Result<core::Revision>::failure(invalid(
            "texture paint layers exceed the aggregate output byte budget"));
    }
    TexturePaintLayer layer{std::move(layer_identity), target, initial_value};
    try {
        layers_.reserve(layers_.size() + 1U);
    } catch (const std::exception& exception) {
        return core::Result<core::Revision>::failure(io_error(
            std::string("unable to reserve texture paint layer storage: ") + exception.what()));
    } catch (...) {
        return core::Result<core::Revision>::failure(io_error(
            "unable to reserve texture paint layer storage"));
    }
    if (history_cursor_ < commands_.size()) {
        history_work_pixels_ = total_history_work(
            std::span<const TexturePaintCommand>(commands_.data(), history_cursor_),
            width_, height_);
        commands_.resize(history_cursor_);
    }
    layers_.push_back(std::move(layer));
    revision_ = revision_.next();
    return core::Result<core::Revision>::success(revision_);
}

core::Result<core::Revision> TexturePaintDocument::apply_stamp(
    core::Revision expected_revision,
    std::string_view layer_identity,
    TexturePaintStamp stamp) {
    if (expected_revision != revision_) {
        return core::Result<core::Revision>::failure(stale(
            "texture paint stroke was based on a stale document revision"));
    }
    if (revision_.exhausted()) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "texture paint document revision is exhausted"));
    }
    if (std::none_of(layers_.begin(), layers_.end(), [&](const auto& layer) {
            return layer.identity == layer_identity;
        })) {
        return core::Result<core::Revision>::failure(invalid(
            "texture paint command references an unknown layer"));
    }
    if (auto result = validate_stamp(stamp, width_, height_); !result) {
        return core::Result<core::Revision>::failure(result.error());
    }
    const std::uint64_t work = stamp_work(stamp, width_, height_);
    const auto previous_work = history_cursor_ < commands_.size()
        ? total_history_work(
            std::span<const TexturePaintCommand>(commands_.data(), history_cursor_),
            width_, height_)
        : history_work_pixels_;
    if (work > kMaxBrushWorkPixels - std::min(previous_work, kMaxBrushWorkPixels) ||
        history_cursor_ >= kMaxCommands) {
        return core::Result<core::Revision>::failure(invalid(
            "texture paint history exceeds its command or brush-work budget"));
    }
    TexturePaintCommand command;
    try {
        command = TexturePaintCommand{std::string(layer_identity), stamp};
        commands_.reserve(history_cursor_ + 1U);
    } catch (const std::exception& exception) {
        return core::Result<core::Revision>::failure(io_error(
            std::string("unable to reserve texture paint history storage: ") + exception.what()));
    } catch (...) {
        return core::Result<core::Revision>::failure(io_error(
            "unable to reserve texture paint history storage"));
    }
    commands_.resize(history_cursor_);
    commands_.push_back(std::move(command));
    history_work_pixels_ = previous_work + work;
    history_cursor_ = commands_.size();
    revision_ = revision_.next();
    return core::Result<core::Revision>::success(revision_);
}

core::Result<core::Revision> TexturePaintDocument::undo(core::Revision expected_revision) {
    if (expected_revision != revision_) {
        return core::Result<core::Revision>::failure(stale(
            "texture paint undo was based on a stale document revision"));
    }
    if (revision_.exhausted()) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "texture paint document revision is exhausted"));
    }
    if (history_cursor_ == 0U) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "texture paint history has nothing to undo"));
    }
    --history_cursor_;
    revision_ = revision_.next();
    return core::Result<core::Revision>::success(revision_);
}

core::Result<core::Revision> TexturePaintDocument::redo(core::Revision expected_revision) {
    if (expected_revision != revision_) {
        return core::Result<core::Revision>::failure(stale(
            "texture paint redo was based on a stale document revision"));
    }
    if (revision_.exhausted()) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "texture paint document revision is exhausted"));
    }
    if (history_cursor_ >= commands_.size()) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "texture paint history has nothing to redo"));
    }
    ++history_cursor_;
    revision_ = revision_.next();
    return core::Result<core::Revision>::success(revision_);
}

core::Result<void> TexturePaintDocument::validate() const {
    const std::uint64_t pixels = static_cast<std::uint64_t>(width_) * height_;
    if (!valid_identity(identity_) || !valid_identity(source_identity_) ||
        revision_.value() == 0U || source_revision_.value() == 0U ||
        source_digest_.is_zero() || source_bytes_ == 0U ||
        source_bytes_ > kMaxSourceBytes || width_ == 0U || height_ == 0U ||
        width_ > kMaxDimension || height_ > kMaxDimension ||
        pixels > kMaxPixels || layers_.size() > kMaxLayers ||
        commands_.size() > kMaxCommands ||
        history_cursor_ > commands_.size()) {
        return core::Result<void>::failure(invalid(
            "texture paint document identity, source binding, extent, or bounded counts are invalid"));
    }
    const std::uint64_t aggregate_output_bytes = pixels * 4U +
        mask_layer_count(layers_) * pixels;
    if (aggregate_output_bytes > kMaxAggregateOutputBytes) {
        return core::Result<void>::failure(invalid(
            "texture paint document exceeds its aggregate output byte budget"));
    }
    std::set<std::string, std::less<>> layer_ids;
    for (const auto& layer : layers_) {
        if (!valid_identity(layer.identity) || !valid_target(layer.target) ||
            (layer.target != TexturePaintTarget::mask && layer.initial_value != 0U) ||
            !layer_ids.insert(layer.identity).second) {
            return core::Result<void>::failure(invalid(
                "texture paint document contains an invalid or duplicate layer"));
        }
    }
    for (const auto& command : commands_) {
        if (!layer_ids.contains(command.layer_identity)) {
            return core::Result<void>::failure(invalid(
                "texture paint history references a missing layer"));
        }
        if (auto result = validate_stamp(command.stamp, width_, height_); !result) {
            return result;
        }
    }
    const std::uint64_t total_work = total_history_work(commands_, width_, height_);
    if (total_work > kMaxBrushWorkPixels || total_work != history_work_pixels_) {
        return core::Result<void>::failure(invalid(
            "texture paint history work budget or retained work receipt is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> TexturePaintDocument::validate_source(
    const TexturePaintSource& source,
    const TextureSourceLimits& limits) const {
    if (auto result = validate(); !result) return result;
    auto admission = inspect_paint_source(source, limits);
    if (!admission) return core::Result<void>::failure(admission.error());
    if (source.identity != source_identity_ || source.revision != source_revision_ ||
        admission.value().source.digest != source_digest_ ||
        admission.value().source.bytes != source_bytes_ ||
        admission.value().width != width_ || admission.value().height != height_) {
        return core::Result<void>::failure(stale(
            "texture paint source identity, revision, bytes, or extent changed"));
    }
    return core::Result<void>::success();
}

std::string TexturePaintDocument::serialize() const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << kMagic << ' ' << kSchemaVersion << '\n'
           << "DOCUMENT " << std::quoted(identity_) << ' ' << revision_.value() << '\n'
           << "SOURCE " << std::quoted(source_identity_) << ' '
           << source_revision_.value() << ' ' << source_bytes_ << ' '
           << width_ << ' ' << height_ << ' ' << source_digest_.hex() << '\n'
           << "LAYERS " << layers_.size() << '\n';
    for (const auto& layer : layers_) {
        output << "LAYER " << std::quoted(layer.identity) << ' '
               << static_cast<unsigned>(layer.target) << ' '
               << static_cast<unsigned>(layer.initial_value) << '\n';
    }
    output << "COMMANDS " << commands_.size() << ' ' << history_cursor_ << '\n';
    for (const auto& command : commands_) {
        output << "COMMAND " << std::quoted(command.layer_identity) << ' '
               << command.stamp.x << ' ' << command.stamp.y << ' '
               << command.stamp.radius << ' '
               << static_cast<unsigned>(command.stamp.value) << ' '
               << static_cast<unsigned>(command.stamp.opacity) << ' '
               << static_cast<unsigned>(command.stamp.mode) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<TexturePaintDocument> TexturePaintDocument::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<TexturePaintDocument>::failure(invalid(
            "texture paint document serialization is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic >> version) || magic != kMagic || version != kSchemaVersion) {
        return core::Result<TexturePaintDocument>::failure(invalid(
            "texture paint document header or schema version is invalid"));
    }
    TexturePaintDocument document;
    std::string token;
    std::uint64_t document_revision = 0U;
    if (!(input >> token) || token != "DOCUMENT" ||
        !(input >> std::quoted(document.identity_) >> document_revision)) {
        return core::Result<TexturePaintDocument>::failure(invalid(
            "texture paint document identity record is malformed"));
    }
    document.revision_ = core::Revision{document_revision};
    std::uint64_t source_revision = 0U;
    std::string source_digest;
    if (!(input >> token) || token != "SOURCE" ||
        !(input >> std::quoted(document.source_identity_) >> source_revision >>
            document.source_bytes_ >> document.width_ >> document.height_ >> source_digest)) {
        return core::Result<TexturePaintDocument>::failure(invalid(
            "texture paint source record is malformed"));
    }
    document.source_revision_ = core::Revision{source_revision};
    auto digest = Sha256Digest::from_hex(source_digest);
    if (!digest) return core::Result<TexturePaintDocument>::failure(digest.error());
    document.source_digest_ = digest.value();

    std::uint64_t layer_count = 0U;
    if (!(input >> token) || token != "LAYERS" || !(input >> layer_count) ||
        layer_count > kMaxLayers) {
        return core::Result<TexturePaintDocument>::failure(invalid(
            "texture paint layer count record is malformed or over budget"));
    }
    document.layers_.reserve(static_cast<std::size_t>(layer_count));
    for (std::uint64_t index = 0U; index < layer_count; ++index) {
        std::string identity;
        std::uint32_t target = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t initial_value = 256U;
        if (!(input >> token) || token != "LAYER" ||
            !(input >> std::quoted(identity) >> target >> initial_value) ||
            target > static_cast<std::uint32_t>(TexturePaintTarget::mask) ||
            initial_value > 255U) {
            return core::Result<TexturePaintDocument>::failure(invalid(
                "texture paint layer record is malformed"));
        }
        document.layers_.push_back(TexturePaintLayer{
            std::move(identity), static_cast<TexturePaintTarget>(target),
            static_cast<std::uint8_t>(initial_value)});
    }

    std::uint64_t command_count = 0U;
    std::uint64_t history_cursor = 0U;
    if (!(input >> token) || token != "COMMANDS" ||
        !(input >> command_count >> history_cursor) || command_count > kMaxCommands ||
        history_cursor > command_count) {
        return core::Result<TexturePaintDocument>::failure(invalid(
            "texture paint command count or history cursor is malformed or over budget"));
    }
    document.commands_.reserve(static_cast<std::size_t>(command_count));
    for (std::uint64_t index = 0U; index < command_count; ++index) {
        TexturePaintCommand command;
        std::uint32_t radius = 0U;
        std::uint32_t value = 0U;
        std::uint32_t opacity = 0U;
        std::uint32_t mode = std::numeric_limits<std::uint32_t>::max();
        if (!(input >> token) || token != "COMMAND" ||
            !(input >> std::quoted(command.layer_identity) >> command.stamp.x >>
                command.stamp.y >> radius >> value >> opacity >> mode) ||
            radius > std::numeric_limits<std::uint16_t>::max() || value > 255U ||
            opacity > 255U || mode > static_cast<std::uint32_t>(TexturePaintMode::erase)) {
            return core::Result<TexturePaintDocument>::failure(invalid(
                "texture paint command record is malformed"));
        }
        command.stamp.radius = static_cast<std::uint16_t>(radius);
        command.stamp.value = static_cast<std::uint8_t>(value);
        command.stamp.opacity = static_cast<std::uint8_t>(opacity);
        command.stamp.mode = static_cast<TexturePaintMode>(mode);
        document.commands_.push_back(std::move(command));
    }
    if (!(input >> token) || token != "END") {
        return core::Result<TexturePaintDocument>::failure(invalid(
            "texture paint document terminator is missing"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<TexturePaintDocument>::failure(invalid(
            "texture paint document contains trailing records"));
    }
    document.history_cursor_ = static_cast<std::size_t>(history_cursor);
    document.history_work_pixels_ = total_history_work(
        document.commands_, document.width_, document.height_);
    if (auto result = document.validate(); !result) {
        return core::Result<TexturePaintDocument>::failure(result.error());
    }
    return core::Result<TexturePaintDocument>::success(std::move(document));
}

core::Result<TexturePaintProduct> compile_texture_paint(
    const TexturePaintDocument& document,
    const TexturePaintSource& source,
    const TextureSourceLimits& requested_limits) {
    if (auto result = document.validate_source(source, requested_limits); !result) {
        return core::Result<TexturePaintProduct>::failure(result.error());
    }
    auto limits = bounded_limits(requested_limits);
    if (!limits) return core::Result<TexturePaintProduct>::failure(limits.error());
    auto decoded = decode_tga_rgba8(source.encoded_tga, limits.value());
    if (!decoded) return core::Result<TexturePaintProduct>::failure(decoded.error());

    TexturePaintProduct product{
        .document_identity = document.identity(),
        .document_revision = document.revision(),
        .source_identity = document.source_identity(),
        .source_revision = document.source_revision(),
        .source_digest = document.source_digest(),
        .document_digest = {},
        .width = document.width(),
        .height = document.height(),
        .rgba = std::move(decoded.value().pixels),
        .masks = {},
        .artifact_digest = {},
    };
    const auto serialized_document = document.serialize();
    const auto serialized_document_bytes = digest_text_bytes(serialized_document);
    product.document_digest = sha256(serialized_document_bytes);

    std::map<std::string, std::size_t, std::less<>> mask_index;
    std::map<std::string, TexturePaintTarget, std::less<>> target_by_layer;
    for (const auto& layer : document.layers()) {
        target_by_layer.emplace(layer.identity, layer.target);
        if (layer.target == TexturePaintTarget::mask) {
            mask_index.emplace(layer.identity, product.masks.size());
            product.masks.push_back(TextureMaskLayerProduct{
                .identity = layer.identity,
                .pixels = std::vector<std::uint8_t>(
                    static_cast<std::size_t>(product.width) * product.height,
                    layer.initial_value),
            });
        }
    }

    for (std::size_t command_index = 0U;
         command_index < document.history_cursor(); ++command_index) {
        const auto& command = document.commands()[command_index];
        const auto& stamp = command.stamp;
        const TexturePaintTarget target = target_by_layer.at(command.layer_identity);
        std::vector<std::uint8_t>* destination = nullptr;
        std::size_t channel = 0U;
        if (target == TexturePaintTarget::mask) {
            destination = &product.masks[mask_index.at(command.layer_identity)].pixels;
        } else {
            destination = &product.rgba;
            channel = static_cast<std::size_t>(target);
        }
        const std::uint64_t radius64 = stamp.radius;
        const std::uint32_t min_x = static_cast<std::uint32_t>(
            stamp.x > radius64 ? stamp.x - radius64 : 0U);
        const std::uint32_t min_y = static_cast<std::uint32_t>(
            stamp.y > radius64 ? stamp.y - radius64 : 0U);
        const std::uint32_t max_x = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            product.width - 1U, static_cast<std::uint64_t>(stamp.x) + radius64));
        const std::uint32_t max_y = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            product.height - 1U, static_cast<std::uint64_t>(stamp.y) + radius64));
        const std::int64_t radius_squared = static_cast<std::int64_t>(stamp.radius) *
            static_cast<std::int64_t>(stamp.radius);
        const std::uint8_t target_value = stamp.mode == TexturePaintMode::erase
            ? 0U : stamp.value;
        for (std::uint32_t y = min_y; y <= max_y; ++y) {
            for (std::uint32_t x = min_x; x <= max_x; ++x) {
                const std::int64_t dx = static_cast<std::int64_t>(x) - stamp.x;
                const std::int64_t dy = static_cast<std::int64_t>(y) - stamp.y;
                if (dx * dx + dy * dy > radius_squared) continue;
                const std::size_t pixel_index =
                    static_cast<std::size_t>(y) * product.width + x;
                const std::size_t offset = target == TexturePaintTarget::mask
                    ? pixel_index : pixel_index * 4U + channel;
                const std::uint32_t old_value = (*destination)[offset];
                const std::uint32_t opacity_value = stamp.opacity;
                const std::uint32_t blended = old_value * (255U - opacity_value) +
                    static_cast<std::uint32_t>(target_value) * opacity_value + 127U;
                (*destination)[offset] = static_cast<std::uint8_t>(blended / 255U);
            }
        }
    }
    product.artifact_digest = sha256(canonical_product(product));
    if (auto result = product.validate(); !result) {
        return core::Result<TexturePaintProduct>::failure(result.error());
    }
    return core::Result<TexturePaintProduct>::success(std::move(product));
}

core::Result<void> TexturePaintProduct::validate() const {
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (!valid_identity(document_identity) || !valid_identity(source_identity) ||
        document_revision.value() == 0U || source_revision.value() == 0U ||
        source_digest.is_zero() || document_digest.is_zero() || width == 0U ||
        height == 0U || width > TexturePaintDocument::kMaxDimension ||
        height > TexturePaintDocument::kMaxDimension || pixels > TexturePaintDocument::kMaxPixels ||
        rgba.size() != static_cast<std::size_t>(pixels) * 4U ||
        masks.size() > TexturePaintDocument::kMaxLayers) {
        return core::Result<void>::failure(invalid(
            "texture paint product identity, revision, extent, or output lengths are invalid"));
    }
    const std::uint64_t aggregate_output_bytes = pixels * 4U +
        static_cast<std::uint64_t>(masks.size()) * pixels;
    if (aggregate_output_bytes > TexturePaintDocument::kMaxAggregateOutputBytes) {
        return core::Result<void>::failure(invalid(
            "texture paint product exceeds its aggregate output byte budget"));
    }
    std::set<std::string, std::less<>> ids;
    for (const auto& mask : masks) {
        if (!valid_identity(mask.identity) || !ids.insert(mask.identity).second ||
            mask.pixels.size() != static_cast<std::size_t>(pixels)) {
            return core::Result<void>::failure(invalid(
                "texture paint mask output has an invalid identity or extent"));
        }
    }
    if (artifact_digest.is_zero() || sha256(canonical_product(*this)) != artifact_digest) {
        return core::Result<void>::failure(stale(
            "texture paint artifact digest does not match its source receipt and output"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> TexturePaintProduct::export_tga() const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    std::vector<std::uint8_t> bytes(18U, 0U);
    bytes[2U] = 2U;
    bytes[12U] = static_cast<std::uint8_t>(width & 0xffU);
    bytes[13U] = static_cast<std::uint8_t>((width >> 8U) & 0xffU);
    bytes[14U] = static_cast<std::uint8_t>(height & 0xffU);
    bytes[15U] = static_cast<std::uint8_t>((height >> 8U) & 0xffU);
    bytes[16U] = 32U;
    bytes[17U] = 0x28U;
    bytes.reserve(18U + rgba.size());
    for (std::size_t pixel = 0U; pixel < rgba.size(); pixel += 4U) {
        bytes.push_back(rgba[pixel + 2U]);
        bytes.push_back(rgba[pixel + 1U]);
        bytes.push_back(rgba[pixel]);
        bytes.push_back(rgba[pixel + 3U]);
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<std::vector<std::uint8_t>> TexturePaintProduct::export_mask_tga(
    std::string_view layer_identity) const {
    if (auto result = validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    const auto found = std::find_if(masks.begin(), masks.end(), [&](const auto& mask) {
        return mask.identity == layer_identity;
    });
    if (found == masks.end()) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "texture paint mask layer does not exist"));
    }
    std::vector<std::uint8_t> bytes(18U, 0U);
    bytes[2U] = 2U;
    bytes[12U] = static_cast<std::uint8_t>(width & 0xffU);
    bytes[13U] = static_cast<std::uint8_t>((width >> 8U) & 0xffU);
    bytes[14U] = static_cast<std::uint8_t>(height & 0xffU);
    bytes[15U] = static_cast<std::uint8_t>((height >> 8U) & 0xffU);
    bytes[16U] = 32U;
    bytes[17U] = 0x28U;
    bytes.reserve(18U + found->pixels.size() * 4U);
    for (const std::uint8_t value : found->pixels) {
        bytes.push_back(value);
        bytes.push_back(value);
        bytes.push_back(value);
        bytes.push_back(255U);
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<void> TexturePaintProduct::validate_against(
    const TexturePaintDocument& document,
    const TexturePaintSource& source,
    const TextureSourceLimits& limits) const {
    const auto expected = compile_texture_paint(document, source, limits);
    if (!expected) return core::Result<void>::failure(expected.error());
    if (document_identity != expected.value().document_identity ||
        document_revision != expected.value().document_revision ||
        source_identity != expected.value().source_identity ||
        source_revision != expected.value().source_revision ||
        source_digest != expected.value().source_digest ||
        document_digest != expected.value().document_digest || width != expected.value().width ||
        height != expected.value().height || rgba != expected.value().rgba ||
        masks != expected.value().masks || artifact_digest != expected.value().artifact_digest) {
        return core::Result<void>::failure(stale(
            "texture paint product differs from its exact document, history, or source"));
    }
    return core::Result<void>::success();
}

} // namespace carto::assets
