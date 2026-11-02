#include <carto/render/visual_baseline.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <locale>
#include <stdexcept>
#include <string>
#include <vector>

namespace visual = carto::render::visual;
static void require(bool value) {
    if (!value) throw std::runtime_error("visual baseline regression failed");
}

class Grouping final : public std::numpunct<char> {
    char do_thousands_sep() const override { return '_'; }
    std::string do_grouping() const override { return "\3"; }
};

int main() {
    try {
        visual::BaselineSpec spec{"web-geometry-overlay-clusters", "d3d12",
            "NVIDIA Quadro P5200", 4318U, 7093U, "D3D_FEATURE_LEVEL_11_0",
            "d3d12-debug-layer", 96U, 96U, 1U, 24U};
        const std::string expected_metadata =
            "format=carto-visual-baseline-v1\n"
            "fixture=web-geometry-overlay-clusters\n"
            "backend=d3d12\n"
            "adapter_name=NVIDIA Quadro P5200\n"
            "vendor_id=4318\n"
            "device_id=7093\n"
            "feature_level=D3D_FEATURE_LEVEL_11_0\n"
            "driver_label=d3d12-debug-layer\n"
            "dimensions=96x96\n"
            "pixel_encoding=rgb8-channel-round-nearest\n"
            "max_channel_delta=1\n"
            "max_changed_pixels=24\n";
        require(visual::metadata_v1(spec) == expected_metadata);
        const auto metadata_path = std::filesystem::path{CARTO_VISUAL_BASELINE_DIR} /
            "web_geometry_overlay_96x96.meta";
        std::ifstream tracked{metadata_path, std::ios::binary};
        require(tracked.good());
        const std::string tracked_metadata{std::istreambuf_iterator<char>{tracked},
            std::istreambuf_iterator<char>{}};
        require(visual::metadata_matches_v1(tracked_metadata, expected_metadata));
        const std::locale old = std::locale();
        std::locale::global(std::locale(old, new Grouping));
        const std::string locale_metadata = visual::metadata_v1(spec);
        std::locale::global(old);
        require(locale_metadata == expected_metadata);

        spec.width = 5U; spec.height = 5U; spec.max_changed_pixels = 2U;
        const auto metadata = visual::metadata_v1(spec);
        std::vector<std::uint8_t> expected(75U, 0U), actual = expected;
        const auto compare = [&] {
            return visual::compare_baseline(spec, metadata, actual, expected, 25U);
        };
        require(compare().status == visual::ComparisonStatus::match);
        std::string windows_metadata;
        for (const char ch : metadata) {
            if (ch == '\n') windows_metadata += '\r';
            windows_metadata += ch;
        }
        require(visual::compare_baseline(spec, windows_metadata, actual, expected, 25U).status ==
            visual::ComparisonStatus::match);
        auto malformed_newline = metadata;
        malformed_newline[malformed_newline.find('\n')] = '\r';
        require(visual::compare_baseline(spec, malformed_newline, actual, expected, 25U).status ==
            visual::ComparisonStatus::metadata_mismatch);
        actual[0] = 1U; // At the threshold is not a changed pixel.
        require(compare().pixels.changed_pixels == 0U);
        actual[0] = 2U; actual[1] = 200U; // Two bad channels, one pixel.
        auto result = compare();
        require(result.status == visual::ComparisonStatus::match);
        require(result.pixels.changed_pixels == 1U && result.pixels.max_observed_channel_delta == 200U);
        actual[3] = 2U;
        require(compare().status == visual::ComparisonStatus::match); // Inclusive pixel budget.
        actual[6] = 2U;
        require(compare().status == visual::ComparisonStatus::pixel_difference);
        require(compare().pixels.changed_pixels == 3U);

        auto changed_identity = spec;
        ++changed_identity.device_id;
        require(visual::compare_baseline(changed_identity, metadata, actual, expected, 25U).status ==
            visual::ComparisonStatus::metadata_mismatch);
        changed_identity = spec; changed_identity.driver_label += "-different";
        require(visual::compare_baseline(changed_identity, metadata, actual, expected, 25U).status ==
            visual::ComparisonStatus::metadata_mismatch);
        changed_identity = spec; changed_identity.max_channel_delta = 2U;
        require(visual::compare_baseline(changed_identity, metadata, actual, expected, 25U).status ==
            visual::ComparisonStatus::metadata_mismatch);
        changed_identity = spec; changed_identity.fixture = "forged\nbackend=other";
        require(visual::compare_baseline(changed_identity, metadata, actual, expected, 25U).status ==
            visual::ComparisonStatus::invalid_spec);
        changed_identity = spec; changed_identity.adapter_name = std::string{"bad\0adapter", 11};
        require(visual::compare_baseline(changed_identity, metadata, actual, expected, 25U).status ==
            visual::ComparisonStatus::invalid_spec);
        require(visual::compare_baseline(spec, metadata, actual, expected, 24U).status ==
            visual::ComparisonStatus::invalid_pixels);
        actual.pop_back();
        require(compare().status == visual::ComparisonStatus::invalid_pixels);
        actual = expected;
        expected[0] = 255U;
        result = compare();
        require(result.status == visual::ComparisonStatus::match);
        require(result.pixels.changed_pixels == 1U && result.pixels.max_observed_channel_delta == 255U);
        require(!visual::compare_rgb8({}, {}, 0U, 1U, 0U, 1U));
        require(!visual::compare_rgb8({}, {}, 0xffffffffU, 0xffffffffU, 0U, 25U));
        std::cout << "Visual baseline contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
