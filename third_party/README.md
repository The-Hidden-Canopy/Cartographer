# Optional desktop dependencies

The headless Cartographer build has no third-party runtime dependency.

The optional Windows desktop target uses Dear ImGui. Configure it with
`-DCARTO_IMGUI_ROOT=<path-to-a-pinned-imgui-tree>` and retain the upstream
license and attribution alongside the dependency. The repository intentionally
does not fetch source code during a normal configure or build.

The adapter targets the current Vulkan backend initialization shape: the
application supplies `ImGui_ImplVulkan_InitInfo::ApiVersion` and the main
render-pass/sample-count values through `PipelineInfoMain`. Pin a Dear ImGui
revision with that interface before enabling `CARTO_BUILD_DESKTOP`; an
arbitrary older checkout is not a supported compatibility promise. The shell
also relies on the current backend-owned font-atlas upload path and does not
call removed manual font-upload helpers.
