# Optional desktop dependencies

The headless Cartographer build has no third-party runtime dependency.

The optional Windows desktop target uses the pinned Dear ImGui source tree in
`third_party/imgui` (v1.92.9, commit
`01380c579715e62fb9a8d6ec0502c4ea83bfde6e`). The upstream MIT license is kept
at `third_party/imgui/LICENSE.txt`. Consumers that carry a separately reviewed
Dear ImGui checkout may override the bundled copy with
`-DCARTO_IMGUI_ROOT=<path-to-a-pinned-imgui-tree>` and can disable the bundled
default with `-DCARTO_USE_BUNDLED_IMGUI=OFF`.

The build does not download source code during configure or build.

The adapter targets the current Vulkan backend initialization shape: the
application supplies `ImGui_ImplVulkan_InitInfo::ApiVersion` and the main
render-pass/sample-count values through `PipelineInfoMain`. Pin a Dear ImGui
revision with that interface before enabling `CARTO_BUILD_DESKTOP`; an
arbitrary older checkout is not a supported compatibility promise. The shell
also relies on the current backend-owned font-atlas upload path and does not
call removed manual font-upload helpers.
