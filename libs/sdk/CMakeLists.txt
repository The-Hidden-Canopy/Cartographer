if(CARTO_BUILD_SDK_SHARED)
    add_library(carto_sdk SHARED
        src/carto.cpp
    )
    target_compile_definitions(carto_sdk
        PRIVATE CARTO_SDK_BUILDING=1
        PUBLIC CARTO_SDK_SHARED=1
    )
else()
    add_library(carto_sdk STATIC
        src/carto.cpp
    )
endif()

if(NOT CARTO_BUILD_SDK_SHARED)
    # Keep the source list explicit in the static path too; this guard makes
    # the target-type decision the only packaging difference.
    set_property(TARGET carto_sdk PROPERTY SOURCES src/carto.cpp)
endif()

set_target_properties(carto_sdk PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

if(CARTO_BUILD_SDK_SHARED)
    # The C header marks the exported ABI symbols; no C++ implementation
    # symbols are promoted accidentally.
    target_compile_definitions(carto_sdk PRIVATE CARTO_SDK_SHARED_BUILD=1)
endif()

target_include_directories(carto_sdk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(carto_sdk PUBLIC carto_application)
carto_target(carto_sdk)

install(TARGETS carto_sdk EXPORT CartographerTargets)
install(DIRECTORY include/ DESTINATION include)
