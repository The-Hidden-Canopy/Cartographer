cmake_minimum_required(VERSION 3.25)

project(CartographerPackageConsumer LANGUAGES CXX)

find_package(Cartographer 0.1 CONFIG REQUIRED)

add_executable(cartographer_package_consumer main.cpp)
target_compile_features(cartographer_package_consumer PRIVATE cxx_std_23)
target_link_libraries(cartographer_package_consumer
    PRIVATE
        Cartographer::carto_project
)

enable_testing()
add_test(NAME cartographer_package_consumer COMMAND cartographer_package_consumer)
