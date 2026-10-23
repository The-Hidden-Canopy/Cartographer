#include <carto/production/preparation_operation.hpp>

#include <iostream>

#if __has_include(<carto/anatomy/catalog.hpp>)
#    error "private anatomy headers leaked into the public include graph"
#endif

#if __has_include(<carto/owh/model.hpp>)
#    error "private OWH headers leaked into the public include graph"
#endif

int main() {
    const auto profile = carto::production::portable_preparation_profile();
    if (!profile.validate()) return 1;
    std::cout << "Cartographer public boundary excludes private integration headers\n";
    return 0;
}
