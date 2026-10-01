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

int main() {
    std::cout << "Cartographer public boundary excludes private integration headers\n";
    return 0;
}
