cmake_minimum_required(VERSION 3.22)
project(cdin_backend_external_consumer LANGUAGES C)

find_package(cdinBackend 1.2.2 EXACT CONFIG REQUIRED)

add_executable(cdin_backend_external_consumer main.c)
target_compile_features(cdin_backend_external_consumer PRIVATE c_std_11)
target_link_libraries(cdin_backend_external_consumer PRIVATE cdin::backend)

enable_testing()
add_test(NAME installed_backend_consumer COMMAND cdin_backend_external_consumer)
