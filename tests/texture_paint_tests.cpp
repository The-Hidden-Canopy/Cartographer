#include <carto/assets/texture_paint.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace carto;
using namespace carto::assets;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::vector<std::uint8_t> tga(
    std::uint16_t width,
    std::uint16_t height,
    std::span<const std::array<std::uint8_t, 4U>> rgba,
    bool alpha = true) {
    std::vector<std::uint8_t> bytes(18U, 0U);
    bytes[2U] = 2U;
    bytes[12U] = static_cast<std::uint8_t>(width & 0xffU);
    bytes[13U] = static_cast<std::uint8_t>(width >> 8U);
    bytes[14U] = static_cast<std::uint8_t>(height & 0xffU);
    bytes[15U] = static_cast<std::uint8_t>(height >> 8U);
    bytes[16U] = alpha ? 32U : 24U;
    bytes[17U] = static_cast<std::uint8_t>(0x20U | (alpha ? 0x08U : 0U));
    for (const auto& pixel : rgba) {
        bytes.push_back(pixel[2U]);
        bytes.push_back(pixel[1U]);
        bytes.push_back(pixel[0U]);
        if (alpha) bytes.push_back(pixel[3U]);
    }
    return bytes;
}

std::vector<std::array<std::uint8_t, 4U>> base_pixels() {
    return std::vector<std::array<std::uint8_t, 4U>>(12U,
        std::array<std::uint8_t, 4U>{10U, 20U, 30U, 255U});
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto base = std::filesystem::temp_directory_path();
        for (unsigned attempt = 0U; attempt < 32U; ++attempt) {
            path_ = base / ("cartographer-paint-" + std::to_string(stamp) + "-" +
                std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                created_ = true;
                file_ = path_ / "document.paint";
                return;
            }
        }
    }

    ~TemporaryDirectory() {
        if (!created_) return;
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] bool created() const noexcept { return created_; }
    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
    std::filesystem::path file_;
    bool created_ = false;
};

void source_bound_layers_round_trip_and_support_undo_redo() {
    const auto pixels = base_pixels();
    const auto bytes = tga(4U, 3U, pixels);
    const TexturePaintSource source{"texture.base", core::Revision{1U}, bytes};
    auto created = TexturePaintDocument::create("texture.paint", source);
    require(static_cast<bool>(created), "valid source creates a texture paint document");
    auto document = std::move(created).value();

    auto added = document.add_layer(
        document.revision(), "roughness", TexturePaintTarget::red, 0U);
    require(added && added.value() == core::Revision{2U},
        "new paint layer advances the document revision");
    added = document.add_layer(
        document.revision(), "occlusion", TexturePaintTarget::mask, 10U);
    require(added && added.value() == core::Revision{3U},
        "mask layer is revisioned independently from its source");

    auto edited = document.apply_stamp(document.revision(), "roughness",
        TexturePaintStamp{1U, 1U, 1U, 200U, 255U, TexturePaintMode::paint});
    require(edited && edited.value() == core::Revision{4U},
        "channel paint stroke commits at the expected document revision");
    edited = document.apply_stamp(document.revision(), "occlusion",
        TexturePaintStamp{2U, 1U, 1U, 192U, 128U, TexturePaintMode::paint});
    require(edited && edited.value() == core::Revision{5U},
        "mask paint stroke commits after its exact source-bound edit");

    const auto compiled = compile_texture_paint(document, source);
    require(static_cast<bool>(compiled), "valid texture paint history compiles");
    const auto pixel_index = [](std::uint32_t x, std::uint32_t y) {
        return static_cast<std::size_t>(y) * 4U + x;
    };
    require(compiled.value().rgba[pixel_index(1U, 1U) * 4U] == 200U &&
            compiled.value().rgba[pixel_index(0U, 0U) * 4U] == 10U &&
            compiled.value().rgba[pixel_index(1U, 1U) * 4U + 1U] == 20U,
        "hard-edged channel brush changes only its target channel and covered pixels");
    require(compiled.value().masks.size() == 1U &&
            compiled.value().masks[0U].identity == "occlusion" &&
            compiled.value().masks[0U].pixels[pixel_index(2U, 1U)] == 101U &&
            compiled.value().masks[0U].pixels[pixel_index(0U, 0U)] == 10U,
        "mask layer emits a separate deterministic R8 plane with fixed-point opacity");
    require(static_cast<bool>(compiled.value().validate_against(document, source)),
        "compiled pixels validate against the exact document and source");
    auto tampered_product = compiled.value();
    tampered_product.rgba[0U] ^= 1U;
    require(!tampered_product.validate() && !tampered_product.export_tga(),
        "tampered derived pixels cannot validate or export");

    const std::string saved = document.serialize();
    TemporaryDirectory temp;
    require(temp.created(), "unique temporary save directory is created");
    BlobStore source_store(temp.path() / "blobs");
    const auto stored_source = source_store.put(bytes, "image/x-tga");
    require(stored_source && stored_source.value().digest == document.source_digest(),
        "base texture is stored by its content digest before document save");
    {
        std::ofstream output(temp.file(), std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "paint document file opens for save");
        output.write(saved.data(), static_cast<std::streamsize>(saved.size()));
        require(static_cast<bool>(output), "serialized paint document is written completely");
    }
    std::ifstream input(temp.file(), std::ios::binary);
    const std::string reopened_bytes{
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    input.close();
    auto reopened = TexturePaintDocument::deserialize(reopened_bytes);
    auto reopened_source_bytes = source_store.read(stored_source.value().digest);
    require(reopened && reopened.value().serialize() == saved &&
            reopened.value().history_cursor() == 2U,
        "save/reopen preserves source binding, layers, history, and current cursor");
    require(static_cast<bool>(reopened_source_bytes),
        "reopened document source resolves from the content-addressed blob store");
    const TexturePaintSource reopened_source{
        "texture.base", core::Revision{1U}, reopened_source_bytes.value()};
    const auto reopened_product = compile_texture_paint(reopened.value(), reopened_source);
    require(reopened_product &&
            reopened_product.value().artifact_digest == compiled.value().artifact_digest &&
            reopened_product.value().rgba == compiled.value().rgba &&
            reopened_product.value().masks == compiled.value().masks,
        "reopened document deterministically rebuilds the same texture and mask outputs");
    const auto exported = compiled.value().export_tga();
    const auto decoded = exported ? decode_tga_rgba8(exported.value())
                                  : core::Result<DecodedRgba8Texture>::failure(
                                        core::Diagnostic(core::ErrorCode::invalid_argument,
                                            "export unavailable"));
    require(decoded && decoded.value().pixels == compiled.value().rgba,
        "painted texture export round-trips exact RGBA pixels");
    const auto mask_export = compiled.value().export_mask_tga("occlusion");
    const auto decoded_mask = mask_export ? decode_tga_rgba8(mask_export.value())
                                          : core::Result<DecodedRgba8Texture>::failure(
                                                core::Diagnostic(core::ErrorCode::invalid_argument,
                                                    "mask export unavailable"));
    require(decoded_mask &&
            decoded_mask.value().pixels[pixel_index(2U, 1U) * 4U] == 101U &&
            decoded_mask.value().pixels[pixel_index(2U, 1U) * 4U + 1U] == 101U &&
            decoded_mask.value().pixels[pixel_index(2U, 1U) * 4U + 3U] == 255U,
        "mask export preserves the R8 value in a deterministic grayscale TGA");
    require(!compiled.value().export_mask_tga("missing"),
        "mask export cannot silently substitute a missing layer");

    auto history = document.undo(document.revision());
    require(history && document.history_cursor() == 1U,
        "undo moves the cursor without destroying redo history");
    auto mask_undone = compile_texture_paint(document, source);
    require(mask_undone && mask_undone.value().masks[0U].pixels[pixel_index(2U, 1U)] == 10U &&
            mask_undone.value().rgba == compiled.value().rgba,
        "undo removes only the latest mask operation");
    const std::string undone_saved = document.serialize();
    {
        std::ofstream output(temp.file(), std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "undone paint document reopens for save");
        output.write(undone_saved.data(), static_cast<std::streamsize>(undone_saved.size()));
        require(static_cast<bool>(output), "undone history cursor is saved completely");
    }
    std::ifstream undone_input(temp.file(), std::ios::binary);
    const std::string undone_bytes{
        std::istreambuf_iterator<char>{undone_input}, std::istreambuf_iterator<char>{}};
    undone_input.close();
    auto reopened_undo = TexturePaintDocument::deserialize(undone_bytes);
    require(reopened_undo && reopened_undo.value().history_cursor() == 1U,
        "save/reopen preserves an undo cursor and its available redo branch");
    auto reopened_undo_product = compile_texture_paint(reopened_undo.value(), reopened_source);
    require(reopened_undo_product &&
            reopened_undo_product.value().artifact_digest == mask_undone.value().artifact_digest,
        "reopened undone state rebuilds the same source-backed pixels and receipt");
    auto reopened_redo = reopened_undo.value().redo(reopened_undo.value().revision());
    const auto reopened_redo_product = reopened_redo
        ? compile_texture_paint(reopened_undo.value(), reopened_source)
        : core::Result<TexturePaintProduct>::failure(core::Diagnostic(
              core::ErrorCode::invalid_state, "redo unavailable"));
    require(reopened_redo_product && reopened_redo_product.value().rgba == compiled.value().rgba &&
            reopened_redo_product.value().masks == compiled.value().masks,
        "reopened redo branch restores the original texture and mask pixels");
    history = document.undo(document.revision());
    require(history && document.history_cursor() == 0U,
        "second undo reaches the unpainted source state");
    const auto unpainted = compile_texture_paint(document, source);
    const auto source_pixels = decode_tga_rgba8(bytes);
    require(unpainted && source_pixels && unpainted.value().rgba == source_pixels.value().pixels,
        "undo restores the exact source pixels without a fallback texture");
    history = document.redo(document.revision());
    require(history && document.history_cursor() == 1U,
        "redo restores the first channel stroke");
    edited = document.apply_stamp(document.revision(), "roughness",
        TexturePaintStamp{0U, 0U, 1U, 77U, 255U, TexturePaintMode::paint});
    require(edited && document.commands().size() == 2U &&
            document.history_cursor() == 2U,
        "new edit after undo truncates the contested redo branch");
    require(!document.redo(document.revision()),
        "discarded redo history cannot be silently replayed");
    edited = document.apply_stamp(document.revision(), "occlusion",
        TexturePaintStamp{2U, 1U, 1U, 255U, 255U, TexturePaintMode::erase});
    const auto erased = compile_texture_paint(document, source);
    require(edited && erased && erased.value().masks[0U].pixels[pixel_index(2U, 1U)] == 0U &&
            erased.value().masks[0U].pixels[pixel_index(0U, 0U)] == 10U,
        "erase mode clears only covered mask pixels and preserves explicit initial values");
}

void stale_and_adversarial_boundaries_fail_closed() {
    const auto pixels = base_pixels();
    const auto bytes = tga(4U, 3U, pixels);
    const TexturePaintSource source{"texture.base", core::Revision{8U}, bytes};
    auto created = TexturePaintDocument::create("texture.paint", source);
    require(static_cast<bool>(created), "valid source creates boundary fixture");
    auto document = std::move(created).value();
    require(!document.add_layer(core::Revision{0U}, "mask", TexturePaintTarget::mask, 0U),
        "stale layer edit cannot advance document");
    require(!document.add_layer(document.revision(), "bad target",
            static_cast<TexturePaintTarget>(99U), 0U),
        "unknown paint target is refused");
    require(!document.add_layer(document.revision(), "bad initial",
            TexturePaintTarget::red, 1U),
        "channel layer cannot silently override its source channel");
    auto added = document.add_layer(
        document.revision(), "mask", TexturePaintTarget::mask, 0U);
    require(static_cast<bool>(added), "valid layer admitted before invalid stroke cases");
    require(!document.add_layer(document.revision(), "mask", TexturePaintTarget::mask, 0U),
        "duplicate layer identity is refused");

    const auto before_revision = document.revision();
    require(!document.apply_stamp(core::Revision{1U}, "mask",
            TexturePaintStamp{1U, 1U, 1U, 255U, 255U, TexturePaintMode::paint}),
        "stale paint edit is refused");
    require(!document.apply_stamp(before_revision, "missing",
            TexturePaintStamp{1U, 1U, 1U, 255U, 255U, TexturePaintMode::paint}),
        "stroke cannot create an implicit missing layer");
    require(!document.apply_stamp(before_revision, "mask",
            TexturePaintStamp{4U, 1U, 1U, 255U, 255U, TexturePaintMode::paint}),
        "out-of-bounds brush center is refused");
    require(!document.apply_stamp(before_revision, "mask",
            TexturePaintStamp{1U, 1U, 0U, 255U, 255U, TexturePaintMode::paint}),
        "zero brush radius is refused");
    require(!document.apply_stamp(before_revision, "mask",
            TexturePaintStamp{1U, 1U, 513U, 255U, 255U, TexturePaintMode::paint}),
        "radius above the bounded work limit is refused");
    require(!document.apply_stamp(before_revision, "mask",
            TexturePaintStamp{1U, 1U, 1U, 255U, 0U, TexturePaintMode::paint}),
        "zero-opacity no-op edit is refused");
    require(!document.apply_stamp(before_revision, "mask",
            TexturePaintStamp{1U, 1U, 1U, 255U, 255U,
                static_cast<TexturePaintMode>(99U)}),
        "unknown paint mode is refused");
    require(document.revision() == before_revision && document.commands().empty(),
        "every refused edit leaves document revision and history untouched");

    require(!document.undo(before_revision), "undo with empty history is refused");
    require(!document.redo(before_revision), "redo with empty history is refused");
    auto edited = document.apply_stamp(before_revision, "mask",
        TexturePaintStamp{1U, 1U, 1U, 255U, 255U, TexturePaintMode::paint});
    require(static_cast<bool>(edited), "valid stamp succeeds after refused attempts");
    require(!document.undo(before_revision), "stale undo cannot change the history cursor");

    auto stale_revision = source;
    stale_revision.revision = core::Revision{9U};
    require(!compile_texture_paint(document, stale_revision),
        "same bytes at a different source revision are stale");
    auto changed_bytes = bytes;
    changed_bytes.back() ^= 1U;
    const TexturePaintSource changed_source{
        "texture.base", core::Revision{8U}, changed_bytes};
    require(!compile_texture_paint(document, changed_source),
        "changed source digest cannot reuse old paint history");
    const TexturePaintSource other_source{"texture.other", core::Revision{8U}, bytes};
    require(!compile_texture_paint(document, other_source),
        "paint document cannot bind to a similarly shaped fallback source");

    auto bad_header = document.serialize();
    bad_header.replace(0U, std::string("CARTO_TEXTURE_PAINT").size(), "BAD");
    require(!TexturePaintDocument::deserialize(bad_header),
        "unknown serialized document header is refused");
    auto trailing = document.serialize() + "UNREVIEWED\n";
    require(!TexturePaintDocument::deserialize(trailing),
        "trailing serialized records are refused");
    auto bad_cursor = document.serialize();
    const auto command_record = bad_cursor.find("COMMANDS 1 1");
    require(command_record != std::string::npos, "serialized history record is present");
    bad_cursor.replace(command_record, std::string("COMMANDS 1 1").size(), "COMMANDS 1 2");
    require(!TexturePaintDocument::deserialize(bad_cursor),
        "history cursor beyond command count is refused");
    require(!TexturePaintDocument::deserialize(std::string(
                TexturePaintDocument::kMaxSerializedBytes + 1U, 'x')),
        "oversized serialized paint document is refused before parsing");

    auto trailing_source = bytes;
    trailing_source.push_back(0U);
    require(!TexturePaintDocument::create("trailing", TexturePaintSource{
                "texture.trailing", core::Revision{1U}, trailing_source}),
        "source with uninterpreted trailing bytes is not admitted");
    auto rle_source = bytes;
    rle_source[2U] = 10U;
    require(!TexturePaintDocument::create("rle", TexturePaintSource{
                "texture.rle", core::Revision{1U}, rle_source}),
        "unsupported run-length source is refused rather than decoded by fallback");
    auto truncated_source = bytes;
    truncated_source.pop_back();
    require(!TexturePaintDocument::create("truncated", TexturePaintSource{
                "texture.truncated", core::Revision{1U}, truncated_source}),
        "incomplete source payload is refused");

    auto exhausted_text = document.serialize();
    const std::string revision_prefix = "DOCUMENT \"texture.paint\" ";
    const auto revision_start = exhausted_text.find(revision_prefix);
    require(revision_start != std::string::npos, "document revision record is serialized");
    const std::size_t value_start = revision_start + revision_prefix.size();
    const auto value_end = exhausted_text.find('\n', value_start);
    exhausted_text.replace(value_start, value_end - value_start,
        std::to_string(core::Revision::max_value()));
    auto exhausted = TexturePaintDocument::deserialize(exhausted_text);
    require(exhausted && exhausted.value().revision().exhausted(),
        "valid exhausted revision can be reopened without wrapping");
    require(!exhausted.value().add_layer(exhausted.value().revision(), "new-layer",
                TexturePaintTarget::mask, 0U),
        "revision exhaustion blocks new layer authority");
    require(!exhausted.value().undo(exhausted.value().revision()),
        "revision exhaustion blocks undo revision wraparound");
}

void brush_work_budget_is_enforced_before_history_mutation() {
    std::vector<std::array<std::uint8_t, 4U>> pixels(1024U * 1024U,
        std::array<std::uint8_t, 4U>{1U, 2U, 3U, 255U});
    const auto bytes = tga(1024U, 1024U, pixels);
    const TexturePaintSource source{"texture.large", core::Revision{1U}, bytes};
    auto created = TexturePaintDocument::create("texture.budget", source);
    require(static_cast<bool>(created), "bounded large source is admitted");
    auto document = std::move(created).value();
    auto added = document.add_layer(
        document.revision(), "mask", TexturePaintTarget::mask, 0U);
    require(static_cast<bool>(added), "budget fixture mask is admitted");
    const TexturePaintStamp stamp{
        512U, 512U, 512U, 100U, 255U, TexturePaintMode::paint};
    for (std::size_t index = 0U; index < 64U; ++index) {
        added = document.apply_stamp(document.revision(), "mask", stamp);
        require(static_cast<bool>(added), "exact aggregate brush-work budget is accepted");
    }
    const auto before = document.revision();
    require(!document.apply_stamp(before, "mask", stamp),
        "brush-work budget rejects the first operation beyond its bound");
    require(document.revision() == before && document.commands().size() == 64U,
        "over-budget stroke does not mutate revision or retained history");
}

} // namespace

int main() {
    source_bound_layers_round_trip_and_support_undo_redo();
    stale_and_adversarial_boundaries_fail_closed();
    brush_work_budget_is_enforced_before_history_mutation();
    std::cout << "PASS source-bound texture and mask painting boundaries\n";
    return EXIT_SUCCESS;
}
