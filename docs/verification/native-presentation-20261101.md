# Native desktop presentation repair candidate

This candidate changes the native shell only. Project schemas, source authoring,
history, import admission and saved content are unchanged. Source review and
whitespace validation are complete; no compiler, CPU regression or native GPU
execution has been performed for this candidate. A passing build is not visual
acceptance and a successful exit is not a successful authoring workflow.

## Changes and evidence

The prior shell selected the first graphics/present-capable adapter. Recorded
failure stacks entered Intel's Vulkan driver, while separately accepted native
renderer runs used a discrete adapter. This is a routing observation, not proof
of a driver defect. Admission now considers compatible surface formats and
present modes, ranks discrete adapters first, and logs compatible and selected
device identity. `CARTOGRAPHER_VULKAN_ADAPTER` selects an exact, case-insensitive
device name. Missing or ambiguous explicit selections refuse; they never fall
back silently. Automatic selection still permits integrated-only machines.

Main frame and acquired-image fences have a 2 ms wait budget; acquisition has an
8 ms budget. Deferred waits return to the event loop without resetting a fence.
A successfully acquired image remains owned across an image-fence timeout, so
the next event-loop iteration cannot acquire again with a signaled binary
semaphore. Suboptimal acquisition is submitted/presented before recreation.
Out-of-date acquisition never resets or submits a frame fence. Fence reset is
immediately before queue submission, after successful command recording.

Presentation completion semaphores now belong to swapchain images. A rendering
submission fence alone does not establish completion of a presentation wait;
image reacquisition supplies that reuse boundary. See the
[Vulkan semaphore lifetime guidance](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).
Two main frame slots fit the backend's guaranteed minimum two-image ImGui
upload-buffer ring. This does not change ImGui's platform-window renderer.

Retired custom textures are collected only after a subsequent main submission
fence covers the current frame and its following platform-window submissions
on the shared queue. Deferred acquisition adds no device-idle retirement wait.
Allocation occurs before moving a live texture into retirement storage.
Shutdown retains explicit idle-before-destruction. Resize waits for main
frame fences before existing swapchain retirement and refuses to recreate
while an acquired image is still owned.

Win32 lifecycle messages precede ImGui input capture, accepted close is observed
before another Vulkan call, and deferred rendering waits for input with a short
deadline rather than busy-spinning. The dirty-project confirmation remains.
`CARTOGRAPHER_VULKAN_VALIDATION=require` requires the Khronos validation layer
and a debug messenger; unavailable prerequisites fail visibly.

## Required verification under the serialized native lease

Build/run `cartographer_native_presentation_tests` first. The same production
sequencer is driven by finite-wait regression cases: frame/acquire timeout,
deferred acquired-image ownership, suboptimal acquire, out-of-date acquire and
present, invalid image index, and irregular image order across frame slots.
Adapter cases cover ordering, explicit selection, typo/ambiguity refusal,
empty admission and memory ranking. These tests do not execute Vulkan.

Compile the exact candidate desktop with the existing offline Vulkan/ImGui
profile, then launch only an owned test copy using a separate preference root.
Retain selected-adapter and validation logs. Verify automatic selection and an
explicit compatible adapter, plus an invalid override refusing visibly. A
particular discrete GPU must not be required on other systems.

Exercise visible project load, geometry and UV tools, edit/undo/redo, save and
cold reopen; resize rapidly, minimize/restore, change activation and close both
clean and dirty owned test projects. Exercise a torn-out window and repeated
reference/capture texture replacement. Inspect actual foreground frames and
validation output, including two-image swapchains where supported. Record
repeatable Release frame intervals, input/close latency, selected adapter,
present mode, resolution and validation setting separately from Debug
correctness evidence. Preserve failures and source inputs.

## Limits still requiring investigation

The driver can still block inside `vkQueuePresentKHR`, swapchain creation or
the existing idle calls used by resize, synchronous texture upload, shutdown
and ImGui's platform-window backend. Finite acquire/fence budgets cannot bound
those APIs. The candidate does not claim to repair a persistent GPU deadlock,
device-loss recovery, or all native stalls. Those paths require native traces;
moving all renderer ownership to a dedicated thread would be a separate design
change. Surface-format changes still use the existing backend pipeline path
and require native acceptance. General performance, full-suite, other GPU and
installed-product acceptance remain open. Rollback is the scoped candidate
diff; do not reset canonical work or downgrade any saved project.

Unextended Vulkan also lacks a formal present-completion guarantee for
swapchain teardown from `vkDeviceWaitIdle` alone. The candidate retains that
existing compatibility path and does not claim to close it. A supported
`VK_EXT_swapchain_maintenance1` presentation-fence path and the corresponding
platform-window ownership need separate native verification before claiming
fully certified resize/shutdown resource retirement.

## Coordinator follow-up boundaries

An additional CPU counterexample refuses an empty swapchain image set before any driver acquisition, submission or presentation. The original helper reached acquisition before its invalid-index check; the added precondition repairs that actual RED failure.

The main ImGui upload ring is now fixed to the two fenced main frame slots, avoiding the pinned backend assertion when a surface changes its image count. A changed main surface format rebuilds the backend main pipeline with the new render pass after existing retirement waits. This preserves the internal source/asset and project codec contracts. Actual format-change coverage requires a suitable native surface; ordinary same-format resize does not prove it.

These changes do not implement presentation-fence maintenance1 retirement or replace the bundled platform-window renderer. Device-idle shutdown/retirement, platform-window teardown, low-level driver blocking and repeated Release performance remain separate acceptance gates. Passing CPU tests or a sampled main-window walkthrough must not be described as complete retirement correctness.
