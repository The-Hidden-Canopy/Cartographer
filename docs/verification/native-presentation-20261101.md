# Native desktop presentation repair candidate

This candidate changes native presentation and its bundled renderer integration.
Project schemas, authoring authority, import admission and codecs are unchanged.
This note preserves the initial design and subsequent counterexamples below.
CPU tests and compilation now have recorded results; the earlier main-window
Room08/UV workflow passed, while detached-window validation exposed additional
ownership failures. The revised platform renderer has CPU proof and still
requires native acceptance after host-stability review. A build or exit0 alone
does not prove a successful authoring workflow or a repaired host crash.

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
change. Main surface-format rebinding is described in the coordinator follow-up;
native changed-format acceptance remains open. General performance, full-suite, other GPU and
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

## Native teardown counterexample

The b9385b8 native Room08/UV inspection exited normally but emitted two VUID-vkDestroyBuffer-buffer-00922 reports. A zero process exit was therefore not native acceptance. The bundled ImGui DestroyPlatformWindows callback destroys the main viewport upload buffers as well as secondary viewports. It was called before VulkanShell shutdown reached its device-idle wait. Moving that call inside the existing initialized-renderer shutdown path, immediately after that wait, preserves the backend and waits before those buffers are freed.

Required replay: required-validation native Room08 load, UV inspection and normal Close; retain full stderr and require no validation diagnostic in addition to exit0. This ordering repair does not implement maintenance1 presentation fences, change queue policy, remove the existing shutdown idle wait, or prove untested driver/secondary-window retirement cases.

## Detached-window native counterexample and local backend repair

The 2ae19bc explicit-adapter resize/detach run emitted acquired-image and presentation-semaphore reuse diagnostics. It did not produce a terminal receipt; a subsequent unclean host reboot is recorded separately, without claiming causality. Native GPU execution is held. Main-window UV authoring acceptance at2ae19bc remains distinct from this failed platform-window acceptance.

CPU tests include the actual bundled Vulkan backend implementation and inject every exercised Vulkan callback; they create no Vulkan instance/device or native window and link no Vulkan loader. Six baseline cases fail: creation transitions on never-acquired images, irregular-image presentation semaphore reuse, acquisition timeout, deferred image-fence ownership, failed acquisition, and a platform upload ring not bound to its acquired-image fence. Fixture failures are retained separately from the completed baseline.

The local backend patch removes the invalid creation-time transition submission, uses per-image presentation completion semaphores and upload buffers, yields on finite acquisition/fence timeouts while retaining an acquired image, and presents only after successful submission. The application now surfaces unexpected backend Vulkan results instead of silently continuing. No execution/domain authority or project format path changes. Upstream attribution and license remain.

This repair still requires a bounded native detached-window replay after host stability review. CPU success is not GPU acceptance or proof of reboot causality. Maintenance1 presentation retirement, platform format changes, repeated texture replacement, Release performance, dirty close and invalid-adapter native cases remain separate gates.

A seventh actual-helper CPU counterexample recreated a three-image swapchain as two images and exposed a stale acquisition semaphore cursor outside the new array. Resetting both image/semaphore cursors when the arrays are recreated repairs this additional RED; the prior six cases remain required. This is CPU ownership coverage, not a claim of a native surface image-count change.

## Retained acquisition during platform resize — source follow-on

The platform resize callback previously recreated its swapchain and cleared
the acquired-image state immediately, even when a finite image-fence wait had
deferred an already successful acquire. The source follow-on marks recreation
pending instead. A successful outstanding submission is still presented when
resize is pending, and a minimized viewport with no retained image waits for a
nonzero size before recreating or acquiring. The actual-backend injected Vulkan
fixture now includes resize during a deferred acquire, minimize during a
deferred acquire, and minimize/restore without one. These new cases are authored
but **not compiled or run**; the prior seven CPU cases belong to 2f8ac2c.
Device-idle retirement and true presentation-engine lifetime proof, the
host-stability hold, live resize, close/reopen and native performance remain
separate open gates.
