# This source-level boundary check complements the injected presentation
# adapter tests and the native headless admission tests. Foreground startup
# with a real or mock driver is a separate acceptance gate.
file(READ "${CARTO_SOURCE_ROOT}/apps/desktop/main.cpp" desktop)
file(READ "${CARTO_SOURCE_ROOT}/apps/desktop/native_presentation.hpp" policy)
file(READ "${CARTO_SOURCE_ROOT}/libs/vulkan/src/runtime.cpp" headless)

# A desktop must not depend on whether the unrelated headless profile admits
# RGBA16F/D32/UUID, and it must still reach real window surface admission.
string(FIND "${desktop}" "carto::vulkan::Runtime::probe(" forbidden)
string(FIND "${desktop}" "state.renderer.initialize(state.window)" initialize)
string(FIND "${desktop}" "vkGetPhysicalDeviceSurfaceSupportKHR" surface_support)
string(FIND "${desktop}" "select_presentation_adapter(descriptions" selection)
string(FIND "${policy}" "no compatible Vulkan graphics/present adapter" rejection)
string(FIND "${headless}" "headless_adapter_rejections(" headless_guard)
if(NOT forbidden EQUAL -1 OR initialize EQUAL -1 OR surface_support EQUAL -1 OR
   selection EQUAL -1 OR rejection EQUAL -1 OR headless_guard EQUAL -1)
    message(FATAL_ERROR "Desktop/headless admission boundary is broken")
endif()
