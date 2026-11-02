#include "../apps/desktop/native_presentation.hpp"
#include <array>
#include <iostream>
#include <vector>

using namespace carto::studio;
void require(bool value) { if (!value) throw std::runtime_error("native presentation regression failed"); }
template<class F> void refuses(F&& f) { bool failed = false; try { f(); } catch (const std::runtime_error&) { failed = true; } require(failed); }

struct Driver {
    PresentationResult frame = PresentationResult::ready, image = PresentationResult::ready;
    AcquiredImage next{PresentationResult::ready, 0};
    PresentationResult presentation = PresentationResult::ready;
    unsigned acquires = 0, submits = 0, presents = 0;
    std::vector<std::pair<std::size_t, std::uint32_t>> submitted;
    PresentationResult wait_frame(std::size_t, std::uint64_t budget) { require(budget > 0 && budget <= 2'000'000); return frame; }
    AcquiredImage acquire(std::size_t, std::uint64_t budget) { require(budget > 0 && budget <= 8'000'000); ++acquires; return next; }
    PresentationResult wait_image(std::size_t, std::uint32_t, std::uint64_t budget) { require(budget <= 2'000'000); return image; }
    void submit(std::size_t frame_id, std::uint32_t image_id) { ++submits; submitted.emplace_back(frame_id, image_id); }
    PresentationResult present(std::uint32_t) { ++presents; return presentation; }
};

int main() {
    try {
        const std::array adapters{
            PresentationAdapter{"Intel", AdapterClass::integrated_gpu, 1, 1, 8},
            PresentationAdapter{"Quadro P5200", AdapterClass::discrete_gpu, 2, 2, 4}};
        require(select_presentation_adapter(adapters) == 1);
        require(select_presentation_adapter(adapters, "intel") == 0);
        refuses([&] { select_presentation_adapter(adapters, "Quadro typo"); });
        const std::array duplicates{adapters[1], adapters[1]};
        refuses([&] { select_presentation_adapter(duplicates, "Quadro P5200"); });
        const std::array reversed{adapters[1], adapters[0]};
        require(select_presentation_adapter(reversed) == 0);
        const std::array memory_rank{
            PresentationAdapter{"small", AdapterClass::discrete_gpu, 2, 2, 4},
            PresentationAdapter{"large", AdapterClass::discrete_gpu, 2, 3, 16}};
        require(select_presentation_adapter(memory_rank) == 1);
        refuses([&] { select_presentation_adapter(std::span<const PresentationAdapter>{}); });

        PresentationSequence empty_images;
        Driver invalid_surface;
        refuses([&] { empty_images.step(invalid_surface, 0); });
        if (invalid_surface.acquires != 0 || invalid_surface.submits != 0 ||
            invalid_surface.presents != 0 || empty_images.has_acquired_image())
            throw std::runtime_error("empty swapchain image set reached the driver");

        PresentationSequence sequence;
        Driver driver;
        driver.frame = PresentationResult::deferred;
        require(sequence.step(driver, 4) == FrameProgress::deferred);
        require(driver.acquires == 0 && driver.submits == 0);
        driver.frame = PresentationResult::ready;
        driver.next.result = PresentationResult::deferred;
        require(sequence.step(driver, 4) == FrameProgress::deferred);
        require(!sequence.has_acquired_image() && driver.submits == 0);
        driver.next = {PresentationResult::suboptimal, 3};
        driver.image = PresentationResult::deferred;
        require(sequence.step(driver, 4) == FrameProgress::deferred);
        const auto acquisitions = driver.acquires;
        require(sequence.has_acquired_image());
        require(sequence.step(driver, 4) == FrameProgress::deferred);
        require(driver.acquires == acquisitions && driver.submits == 0);
        driver.image = PresentationResult::ready;
        require(sequence.step(driver, 4) == FrameProgress::recreate);
        require(driver.acquires == acquisitions && driver.submits == 1 && driver.presents == 1);
        require(driver.submitted.back() == std::pair<std::size_t, std::uint32_t>{0, 3});
        require(!sequence.has_acquired_image() && sequence.current_frame() == 1);
        driver.next = {PresentationResult::out_of_date, 0};
        require(sequence.step(driver, 4) == FrameProgress::recreate);
        require(driver.submits == 1 && sequence.current_frame() == 1);
        driver.next = {PresentationResult::ready, 0};
        driver.presentation = PresentationResult::out_of_date;
        require(sequence.step(driver, 4) == FrameProgress::recreate);
        require(driver.submits == 2 && sequence.current_frame() == 2);
        driver.next = {PresentationResult::ready, 4};
        refuses([&] { sequence.step(driver, 4); });
        require(driver.submits == 2);
        // Main upload-buffer reuse is fenced by two frame slots even when
        // WSI returns images in an order unrelated to those slots.
        PresentationSequence two_frames(2);
        Driver irregular;
        for (auto image : {2U, 0U, 1U, 2U, 1U}) {
            irregular.next = {PresentationResult::ready, image};
            require(two_frames.step(irregular, 3) == FrameProgress::presented);
        }
        for (std::size_t i = 0; i < irregular.submitted.size(); ++i)
            require(irregular.submitted[i].first == i % 2);
        require(irregular.submitted[0].second == 2 && irregular.submitted[2].second == 1);
        std::cout << "Native presentation policy and deferred-frame tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
