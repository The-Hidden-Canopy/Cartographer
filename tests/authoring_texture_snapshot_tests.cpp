#include "authoring_surface_fixture.hpp"

#include <carto/production/authoring_material.hpp>
#include <iostream>
#include <memory>

int main() {
    try {
        using namespace carto;
        constexpr auto semantic = assets::TextureSemantic::base_color;
        constexpr auto color_space = assets::ColorSpace::srgb;
        auto original_pixels = pixels(1, 2, 1, {{0, 0, 0, 255}, {255, 255, 255, 255}});
        auto replacement = pixels(1, 2, 1, {{255, 0, 0, 255}, {255, 0, 0, 255}});
        replacement.record.revision = original_pixels.record.revision.next();
        auto missing = pixels(2, 1, 1, {{0, 255, 0, 255}});
        std::shared_ptr<const production::AuthoringTextureLibrary> frozen;

        {
            production::AuthoringTextureLibrary source;
            auto empty = take(source.snapshot());
            check(empty && empty->resident_bytes() == 0, "empty snapshot must be independent");
            check(!empty->prepare(original_pixels.record, semantic, color_space),
                  "empty snapshot must reject a missing source");
            okay(source.admit(original_pixels.record, original_pixels.encoded));
            const auto source_mips = take(source.prepare(original_pixels.record, semantic, color_space));
            check(source.resident_bytes() == 20, "source fixture must include decoded and mip data");

            frozen = take(source.snapshot());
            check(frozen && frozen->resident_bytes() == 8,
                  "snapshot copies decoded pixels but not derived mip cache");
            check(!frozen->prepare(missing.record, semantic, color_space),
                  "snapshot must reject a missing source");
            const auto frozen_mips = take(frozen->prepare(original_pixels.record, semantic, color_space));
            check(frozen_mips.get() != source_mips.get(), "snapshot must build independent mip storage");
            check(frozen_mips->levels.at(0).pixels == source_mips->levels.at(0).pixels,
                  "snapshot preserves decoded source pixels");

            okay(source.admit(replacement.record, replacement.encoded));
            okay(source.admit(missing.record, missing.encoded));
            check(!source.prepare(original_pixels.record, semantic, color_space),
                  "original record must be stale in the replaced source");
            check(!frozen->prepare(replacement.record, semantic, color_space) &&
                  !frozen->prepare(missing.record, semantic, color_space),
                  "snapshot must reject later source mutations");
            check(take(frozen->prepare(original_pixels.record, semantic, color_space))
                      ->levels.at(0).pixels == source_mips->levels.at(0).pixels,
                  "source replacement must not alter snapshot data");
            auto second = take(source.snapshot());
            check(!second->prepare(original_pixels.record, semantic, color_space) &&
                  bool(second->prepare(replacement.record, semantic, color_space)) &&
                  bool(second->prepare(missing.record, semantic, color_space)),
                  "later snapshots reflect their own admission point");
        }
        check(bool(frozen->prepare(original_pixels.record, semantic, color_space)),
              "snapshot must outlive the original library");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
