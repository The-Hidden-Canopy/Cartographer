set(CARTO_IMGUI_SOURCES
    ${CARTO_IMGUI_ROOT}/imgui.cpp
    ${CARTO_IMGUI_ROOT}/imgui_draw.cpp
    ${CARTO_IMGUI_ROOT}/imgui_tables.cpp
    ${CARTO_IMGUI_ROOT}/imgui_widgets.cpp
    ${CARTO_IMGUI_ROOT}/backends/imgui_impl_win32.cpp
)
if(CARTO_DESKTOP_D3D12_UI)
    list(APPEND CARTO_IMGUI_SOURCES ${CARTO_IMGUI_ROOT}/backends/imgui_impl_dx12.cpp)
else()
    list(APPEND CARTO_IMGUI_SOURCES ${CARTO_IMGUI_ROOT}/backends/imgui_impl_vulkan.cpp)
endif()

add_library(carto_imgui STATIC ${CARTO_IMGUI_SOURCES})
target_include_directories(carto_imgui PUBLIC
    ${CARTO_IMGUI_ROOT}
    ${CARTO_IMGUI_ROOT}/backends
)
target_link_libraries(carto_imgui PUBLIC user32 gdi32 shell32 dwmapi)
if(CARTO_DESKTOP_D3D12_UI)
    target_link_libraries(carto_imgui PUBLIC d3d12 dxgi d3dcompiler)
    target_include_directories(carto_imgui PRIVATE "${PROJECT_SOURCE_DIR}/libs/d3d12/include")
else()
    target_link_libraries(carto_imgui PUBLIC Vulkan::Vulkan)
endif()

add_executable(cartographer_desktop WIN32
    main.cpp
    native_presentation.hpp
    native_upload_lifetime.hpp
    bake_workflow.hpp
    branding.hpp
    reference_image_decoder.cpp
    reference_image_decoder.hpp
    reference_image_projection.hpp
    carto_studio_dock.hpp
    studio_style.hpp
    studio_draft.hpp
    studio_chrome.hpp
    studio_file_picker.cpp
    studio_file_picker.hpp
    studio_file_picker_rules.hpp
    studio_file_picker_paths.hpp
    studio_native_chrome.hpp
    studio_browser.hpp
    studio_scene_camera.hpp
    studio_viewshed.hpp
    studio_view_settings.hpp
    studio_viewport_capture.hpp
    studio_sweep_source.hpp
    uv_inspector.hpp
)

target_link_libraries(cartographer_desktop
    PRIVATE
        carto_application
        carto_assets
        carto_ui
        carto_io
        carto_production
        carto_imgui
        user32
        gdi32
        shell32
        shlwapi
        dwmapi
        winmm
        windowscodecs
        ole32
)
if(CARTO_DESKTOP_D3D12_UI)
    target_sources(cartographer_desktop PRIVATE carto_d3d12_shell.hpp)
    target_link_libraries(cartographer_desktop PRIVATE carto_d3d12_viewport)
    target_compile_definitions(cartographer_desktop PRIVATE CARTO_DESKTOP_D3D12_UI=1)
    target_compile_definitions(cartographer_desktop PRIVATE CARTO_DESKTOP_D3D12_COMPILE_CHECK=1 CARTO_DESKTOP_RESIDENT_WORKER=1)
    if(CARTO_DESKTOP_D3D12_DEBUG)
        target_compile_definitions(cartographer_desktop PRIVATE CARTO_DESKTOP_D3D12_DEBUG=1)
    endif()
else()
    target_link_libraries(cartographer_desktop PRIVATE carto_vulkan Vulkan::Vulkan)
endif()

if(CARTO_ENABLE_D3D12)
    target_link_libraries(cartographer_desktop PRIVATE carto_d3d12_scene)
    target_compile_definitions(cartographer_desktop PRIVATE
        CARTO_DESKTOP_HAS_D3D12_SCENE=1)
endif()

if(TARGET carto_d3d12_bake)
    target_link_libraries(cartographer_desktop PRIVATE carto_d3d12_bake)
    target_compile_definitions(cartographer_desktop PRIVATE
        CARTO_DESKTOP_HAS_D3D12_BAKE=1)
endif()

option(CARTO_DESKTOP_ASYNC_READBACK "Enable the separate-device authoring readback preview" OFF)
if(CARTO_DESKTOP_ASYNC_READBACK)
    if(NOT CARTO_ENABLE_D3D12 OR CARTO_DESKTOP_D3D12_UI)
        message(FATAL_ERROR "The asynchronous readback preview requires D3D12 with the Vulkan desktop UI")
    endif()
    target_link_libraries(cartographer_desktop PRIVATE carto_d3d12_viewport)
    target_compile_definitions(cartographer_desktop PRIVATE CARTO_DESKTOP_ASYNC_READBACK=1)
endif()

target_include_directories(cartographer_desktop PRIVATE
    ${CARTO_IMGUI_ROOT}
    ${CARTO_IMGUI_ROOT}/backends
)

# Explicit opt-in private linked-scene provider (CARTO_LINKED_SCENE_PROVIDER_ROOT).
if(TARGET carto_linked_scene_provider)
    target_compile_definitions(cartographer_desktop PRIVATE CARTO_HAS_LINKED_SCENE_PROVIDER=1)
    target_link_libraries(cartographer_desktop PRIVATE carto_linked_scene_provider)
endif()

carto_target(cartographer_desktop)

if(NOT CARTO_DESKTOP_D3D12_COMPILE_CHECK)
    install(TARGETS cartographer_desktop RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

option(CARTO_DESKTOP_STARTUP_TRACE "Record local desktop startup phases beside the executable" OFF)
if(CARTO_DESKTOP_STARTUP_TRACE)
    target_compile_definitions(cartographer_desktop PRIVATE CARTO_DESKTOP_STARTUP_TRACE=1)
endif()
