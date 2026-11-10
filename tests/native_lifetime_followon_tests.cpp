#include "../third_party/imgui/backends/imgui_impl_vulkan_lifetime.h"
#include "../apps/desktop/native_upload_lifetime.hpp"
#include "../apps/desktop/native_presentation.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool value) { if (!value) throw std::runtime_error("native lifetime regression failed"); }

int main()
{
    try {
        ImGui_ImplVulkanLifetime::AcquireCycle first, second;
        require(ImGui_ImplVulkanLifetime::ClampExtent(10, 20, 100) == 20);
        require(ImGui_ImplVulkanLifetime::ClampExtent(80, 20, 100) == 80);
        require(ImGui_ImplVulkanLifetime::ClampExtent(200, 20, 100) == 100);
        using ImGui_ImplVulkanLifetime::SurfaceExtent;
        SurfaceExtent extent{7, 9};
        constexpr SurfaceExtent variable{0xffffffffU, 0xffffffffU};
        constexpr SurfaceExtent minimum{20, 30}, maximum{100, 200};
        require(!ImGui_ImplVulkanLifetime::SelectSurfaceExtent(0, 40, variable, minimum, maximum, extent));
        require(extent.Width == 7 && extent.Height == 9);
        require(!ImGui_ImplVulkanLifetime::SelectSurfaceExtent(50, 40, {0, 0}, minimum, maximum, extent));
        require(!ImGui_ImplVulkanLifetime::SelectSurfaceExtent(50, 40, variable, minimum, {0, 0}, extent));
        require(ImGui_ImplVulkanLifetime::SelectSurfaceExtent(10, 500, variable, minimum, maximum, extent));
        require(extent.Width == 20 && extent.Height == 200);
        require(ImGui_ImplVulkanLifetime::SelectSurfaceExtent(50, 40, {80, 90}, minimum, maximum, extent));
        require(extent.Width == 80 && extent.Height == 90);
        require(!ImGui_ImplVulkanLifetime::SelectSurfaceExtent(50, 40, variable, {101, 30}, maximum, extent));
        ImGui_ImplVulkanLifetime::AcquireCycle many_images;
        require(many_images.Admit(23, 24)); // No fixed 16-image policy.
        require(many_images.MarkSubmitted() && many_images.Presented());
        require(first.MayAcquire() && !first.MayPresent());
        require(!first.MarkSubmitted() && !first.Presented());
        require(!first.Admit(3, 3));
        require(first.Admit(2, 3));
        // Any number of deferred waits preserves this acquisition and forbids
        // a new signal into the same binary acquisition semaphore.
        for (int deferred = 0; deferred < 5; ++deferred) {
            require(!first.MayAcquire() && !first.MayPresent());
            require(!first.Admit(0, 3) && first.Image == 2);
        }
        require(second.Admit(0, 2));
        require(first.MarkSubmitted() && first.MayPresent());
        require(!first.MarkSubmitted() && !first.MayAcquire());
        require(first.Presented()); // Includes enqueued out-of-date presents.
        require(first.MayAcquire() && !first.Presented());
        require(second.Acquired && !second.Submitted && second.Image == 0);
        require(second.MarkSubmitted() && second.Presented());
        require(first.Admit(1, 2));
        require(first.MarkSubmitted() && first.Presented());
        ImGui_ImplVulkanLifetime::PresentFenceCycle present;
        require(present.MayRetire() && !present.Finish(true) && !present.ObserveSignal());
        require(present.Begin() && !present.Begin() && !present.MayRetire());
        require(present.Finish(true)); // Success, suboptimal and enqueued out-of-date.
        require(!present.MayRetire() && !present.Begin());
        for (int rendering_already_done = 0; rendering_already_done < 5; ++rendering_already_done)
            require(present.Pending && !present.MayRetire());
        require(present.ObserveSignal() && present.MayRetire());
        require(present.Begin() && present.Finish(false)); // OOM did not enqueue a present.
        require(present.MayRetire() && !present.ObserveSignal());

        struct DeferredPresentDriver {
            ImGui_ImplVulkanLifetime::PresentFenceCycle prior;
            unsigned acquires = 0, submits = 0;
            carto::studio::PresentationResult wait_frame(std::size_t, std::uint64_t) {
                return carto::studio::PresentationResult::ready;
            }
            carto::studio::AcquiredImage acquire(std::size_t, std::uint64_t) {
                ++acquires; return {carto::studio::PresentationResult::ready, 0};
            }
            carto::studio::PresentationResult wait_image(std::size_t, std::uint32_t, std::uint64_t) {
                return prior.MayRetire() ? carto::studio::PresentationResult::ready : carto::studio::PresentationResult::deferred;
            }
            void submit(std::size_t, std::uint32_t) { ++submits; }
            carto::studio::PresentationResult present(std::uint32_t) {
                require(prior.Begin() && prior.Finish(true));
                return carto::studio::PresentationResult::ready;
            }
        } driver;
        require(driver.prior.Begin() && driver.prior.Finish(true));
        carto::studio::PresentationSequence sequence(2);
        for (int deferred = 0; deferred < 5; ++deferred)
            require(sequence.step(driver, 2) == carto::studio::FrameProgress::deferred);
        require(driver.acquires == 1 && driver.submits == 0 && sequence.has_acquired_image());
        require(driver.prior.ObserveSignal());
        require(sequence.step(driver, 2) == carto::studio::FrameProgress::presented);
        require(driver.acquires == 1 && driver.submits == 1 && driver.prior.Pending);
        require(!sequence.has_acquired_image());
        const carto::studio::TextureUploadIdentity source{"reference", "source-a", 32, 64};
        carto::studio::TextureUploadTicket upload(source);
        require(!upload.observe_completion());
        require(!upload.may_publish(source));
        require(upload.mark_submitted() && !upload.mark_submitted());
        require(!upload.may_release_submission() && !upload.may_publish(source));
        auto changed = source;
        changed.digest = "source-b";
        require(upload.observe_completion() && upload.may_release_submission());
        require(upload.may_publish(source) && !upload.may_publish(changed));
        changed = source; changed.width = 64;
        require(!upload.may_publish(changed));
        changed = source; changed.role = "other-window";
        require(!upload.may_publish(changed));
        carto::studio::TextureUploadTicket cancelled(source);
        require(cancelled.mark_submitted());
        cancelled.cancel();
        require(!cancelled.may_release_submission() && !cancelled.may_publish(source));
        require(cancelled.observe_completion() && cancelled.may_release_submission());
        require(!cancelled.may_publish(source));
        carto::studio::TextureUploadTicket rejected(source);
        require(!rejected.reject_unaccepted_submission());
        require(rejected.mark_submitted() && !rejected.may_release_submission());
        require(rejected.reject_unaccepted_submission() && rejected.cancelled());
        require(rejected.may_release_submission() && !rejected.may_publish(source) &&
                !rejected.observe_completion() && !rejected.reject_unaccepted_submission() &&
                !rejected.mark_submitted());
        carto::studio::TextureUploadTicket completed(source);
        require(completed.mark_submitted() && completed.observe_completion());
        require(!completed.reject_unaccepted_submission() && completed.may_publish(source));
        struct FakeUpload {
            int id;
            carto::studio::TextureUploadTicket ticket;
        };
        std::vector<FakeUpload> jobs;
        for (int id = 0; id < 4; ++id) {
            auto identity = source;
            const char* roles[] = {"reference", "thumbnail 0", "thumbnail 1", "scene capture"};
            identity.role = roles[id];
            identity.digest = std::to_string(id);
            jobs.push_back({id, carto::studio::TextureUploadTicket(std::move(identity))});
            require(jobs.back().ticket.mark_submitted());
        }
        std::vector<int> destroyed;
        const auto destroy = [&](FakeUpload& job) {
            require(job.ticket.may_discard_unclaimed() && job.ticket.cancelled());
            destroyed.push_back(job.id);
        };
        // Saturation with no completed fence cannot free or remove any job.
        require(!carto::studio::reclaim_completed_upload(jobs, destroy));
        require(jobs.size() == 4 && destroyed.empty());
        // Returning to a cached reference withdraws that old publication
        // intent, while retaining its still-pending GPU submission.
        carto::studio::cancel_uploads_for_role(jobs, "reference");
        require(jobs[0].ticket.cancelled() && !jobs[1].ticket.cancelled());
        require(!jobs[2].ticket.cancelled() && !jobs[0].ticket.may_release_submission());
        require(!carto::studio::reclaim_completed_upload(jobs, destroy));
        require(jobs[2].ticket.observe_completion() && jobs[3].ticket.observe_completion());
        // Completed, unclaimed jobs are cache entries: reclaim oldest first,
        // including another role, without touching pending reference jobs.
        require(carto::studio::reclaim_completed_upload(jobs, destroy));
        require(destroyed.size() == 1 && destroyed[0] == 2 && jobs.size() == 3);
        require(jobs[0].id == 0 && jobs[1].id == 1 && jobs[2].id == 3);
        require(!jobs[0].ticket.may_release_submission());
        require(carto::studio::reclaim_completed_upload(jobs, destroy));
        require(destroyed.size() == 2 && destroyed[1] == 3 && jobs.size() == 2);
        require(!carto::studio::reclaim_completed_upload(jobs, destroy));
        std::cout << "Native lifetime follow-on ownership tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
