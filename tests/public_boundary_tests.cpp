#include <iostream>

#if defined(CARTO_BUILD_PRIVATE_INTEGRATIONS)
#    error "the public boundary test must never compile in private mode"
#endif

#if __has_include(<carto/anatomy/catalog.hpp>)
#    error "private anatomy headers leaked into the public include graph"
#endif

#if __has_include(<carto/owh/model.hpp>)
#    error "private OWH headers leaked into the public include graph"
#endif

#if __has_include(<carto/ai/ai.hpp>)
#    error "private authoring planner headers leaked into the public include graph"
#endif

#if __has_include(<carto/device/kernel_evidence.hpp>)
#    error "private execution-contract headers leaked into the public include graph"
#endif

#if __has_include(<carto/render/gpu_plan.hpp>)
#    error "private render-planning headers leaked into the public include graph"
#endif

int main() {
    std::cout << "Cartographer public boundary excludes private integration headers\n";
    return 0;
}
