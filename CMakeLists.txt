cmake_minimum_required(VERSION 3.22)
project(cdin_backend VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
include(CTest)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

set(CDIN_KERNEL_MIN_VERSION "1.2.1" CACHE STRING "Minimum supported cdin-kernel package version")
option(CDIN_WITH_LUA "Compile the PUC Lua 5.4 Runtime bridge" OFF)
option(CDIN_BUILD_INTEGRATION_TESTS "Build tests that link the installed Kernel library" ON)
option(CDIN_BUILD_EXAMPLES "Build complete standalone service examples" ON)

# OS and concurrency services have no Kernel dependency. Keeping them in a
# separate library lets other C projects reuse them without starting CDIN.
add_library(cdin_backend_services STATIC
    src/jobs/pool.c
    src/jobs/public.c
    src/os/fs.c
    src/os/path.c
    src/os/process.c
    src/platform/clock.c
    src/platform/native_path.c
    src/platform/thread.c)
add_library(cdin::backend_services ALIAS cdin_backend_services)
target_compile_features(cdin_backend_services PUBLIC c_std_11)
target_include_directories(cdin_backend_services
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cdin_backend_services PUBLIC Threads::Threads)
target_compile_options(cdin_backend_services PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wshadow;-Wconversion;-Wstrict-prototypes;-Wformat=2;-Wpedantic;-Werror>)
if(UNIX AND NOT APPLE)
    target_compile_definitions(cdin_backend_services PRIVATE _GNU_SOURCE _POSIX_C_SOURCE=200809L)
elseif(APPLE)
    target_compile_definitions(cdin_backend_services PRIVATE _DARWIN_C_SOURCE _POSIX_C_SOURCE=200809L)
endif()

# The application-specific Backend library depends on reusable services and
# the installed Kernel package. Kernel ABI details stay inside src/kernel/;
# that directory is the private adapter boundary shared by small operation
# modules and is never exposed through public include/cdin headers.
set(PKG_CONFIG_ARGN "--static")
pkg_check_modules(CDIN_KERNEL REQUIRED IMPORTED_TARGET "cdin-kernel>=${CDIN_KERNEL_MIN_VERSION}")
pkg_get_variable(CDIN_KERNEL_LIBDIR cdin-kernel libdir)
if(NOT CDIN_KERNEL_LIBDIR)
    message(FATAL_ERROR "cdin-kernel.pc did not provide its library directory")
endif()
get_filename_component(CDIN_KERNEL_PREFIX "${CDIN_KERNEL_LIBDIR}" DIRECTORY)
set(CDIN_KERNEL_BINDIR "${CDIN_KERNEL_PREFIX}/bin")
add_library(cdin_backend STATIC
    src/backend.c
    src/core/status.c
    src/diagnostics/log.c
    src/kernel/adapter.c
    src/kernel/adapter_common.c
    src/kernel/adapter_documents.c
    src/kernel/adapter_resources.c
    src/kernel/adapter_search.c
    src/protocol/backend_protocol.c
    src/protocol/server.c
    src/runtime/lua_host.c
    src/runtime/lua_host_common.c
    src/runtime/lua_host_api.c
    src/runtime/lua_host_documents.c
    src/runtime/lua_host_services.c
    src/runtime/lua_host_register.c
    src/session/session.c)
add_library(cdin::backend ALIAS cdin_backend)
target_compile_features(cdin_backend PUBLIC c_std_11)
target_include_directories(cdin_backend
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cdin_backend PUBLIC cdin_backend_services PRIVATE PkgConfig::CDIN_KERNEL)
target_compile_options(cdin_backend PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wshadow;-Wconversion;-Wstrict-prototypes;-Wformat=2;-Wpedantic;-Werror>)
if(UNIX AND NOT APPLE)
    target_compile_definitions(cdin_backend PRIVATE _GNU_SOURCE _POSIX_C_SOURCE=200809L)
elseif(APPLE)
    target_compile_definitions(cdin_backend PRIVATE _DARWIN_C_SOURCE _POSIX_C_SOURCE=200809L)
endif()

if(CDIN_WITH_LUA)
    find_package(Lua 5.4 REQUIRED)
    if(LUA_VERSION_STRING VERSION_LESS "5.4" OR NOT LUA_VERSION_STRING VERSION_LESS "5.5")
        message(FATAL_ERROR "CDIN_WITH_LUA requires PUC Lua 5.4.x; found ${LUA_VERSION_STRING}")
    endif()
    target_compile_definitions(cdin_backend PRIVATE CDIN_WITH_LUA)
    target_include_directories(cdin_backend PRIVATE ${LUA_INCLUDE_DIR})
    target_link_libraries(cdin_backend PRIVATE ${LUA_LIBRARIES})
endif()

install(TARGETS cdin_backend cdin_backend_services
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" FILES_MATCHING PATTERN "*.h")
configure_file(packaging/cdin-backend-services.pc.in
               "${CMAKE_CURRENT_BINARY_DIR}/cdin-backend-services.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cdin-backend-services.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

if(CDIN_BUILD_EXAMPLES)
    add_executable(cdin_backend_atomic_backup examples/atomic-backup/main.c)
    target_link_libraries(cdin_backend_atomic_backup PRIVATE cdin::backend_services)
    target_compile_features(cdin_backend_atomic_backup PRIVATE c_std_11)
    target_compile_options(cdin_backend_atomic_backup PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wshadow;-Wconversion;-Wstrict-prototypes;-Wformat=2;-Wpedantic;-Werror>)
endif()

if(BUILD_TESTING)
    add_executable(cdin_backend_test_child tests/e2e/support/test_child_process.c)
    target_compile_features(cdin_backend_test_child PRIVATE c_std_11)
    target_compile_options(cdin_backend_test_child PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wshadow;-Wconversion;-Wstrict-prototypes;-Wformat=2;-Wpedantic;-Werror>)

    function(cdin_enable_strict_test_warnings target)
        target_compile_features(${target} PRIVATE c_std_11)
        target_compile_options(${target} PRIVATE
            $<$<C_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wshadow;-Wconversion;-Wstrict-prototypes;-Wformat=2;-Wpedantic;-Werror>)
    endfunction()

    add_executable(cdin_backend_e2e_job_pipeline
        tests/e2e/scenarios/job_pipeline.c tests/e2e/support/scenario.c)
    target_include_directories(cdin_backend_e2e_job_pipeline PRIVATE include tests/e2e/support)
    target_link_libraries(cdin_backend_e2e_job_pipeline PRIVATE cdin_backend_services)
    cdin_enable_strict_test_warnings(cdin_backend_e2e_job_pipeline)
    add_test(NAME backend_e2e_job_pipeline COMMAND cdin_backend_e2e_job_pipeline)

    add_executable(cdin_backend_e2e_job_abort_shutdown
        tests/e2e/scenarios/job_abort_shutdown.c tests/e2e/support/scenario.c)
    target_include_directories(cdin_backend_e2e_job_abort_shutdown PRIVATE
        include src tests/e2e/support)
    target_link_libraries(cdin_backend_e2e_job_abort_shutdown PRIVATE cdin_backend_services)
    cdin_enable_strict_test_warnings(cdin_backend_e2e_job_abort_shutdown)
    add_test(NAME backend_e2e_job_abort_shutdown COMMAND cdin_backend_e2e_job_abort_shutdown)

    add_executable(cdin_backend_e2e_platform_file_workflow
        tests/e2e/scenarios/platform_file_workflow.c tests/e2e/support/scenario.c)
    target_include_directories(cdin_backend_e2e_platform_file_workflow PRIVATE include tests/e2e/support)
    target_link_libraries(cdin_backend_e2e_platform_file_workflow PRIVATE cdin_backend_services)
    cdin_enable_strict_test_warnings(cdin_backend_e2e_platform_file_workflow)
    add_test(NAME backend_e2e_platform_file_workflow COMMAND cdin_backend_e2e_platform_file_workflow)

    add_executable(cdin_backend_e2e_process_supervision
        tests/e2e/scenarios/process_supervision.c tests/e2e/support/scenario.c)
    target_include_directories(cdin_backend_e2e_process_supervision PRIVATE include tests/e2e/support)
    target_link_libraries(cdin_backend_e2e_process_supervision PRIVATE cdin_backend_services)
    cdin_enable_strict_test_warnings(cdin_backend_e2e_process_supervision)
    add_test(NAME backend_e2e_process_supervision
             COMMAND cdin_backend_e2e_process_supervision "$<TARGET_FILE:cdin_backend_test_child>")
    add_dependencies(cdin_backend_e2e_process_supervision cdin_backend_test_child)

    if(CDIN_BUILD_INTEGRATION_TESTS)
        add_executable(cdin_backend_integration_session tests/integration/backend_session.c)
        target_include_directories(cdin_backend_integration_session PRIVATE src)
        target_link_libraries(cdin_backend_integration_session PRIVATE cdin::backend)
        target_compile_features(cdin_backend_integration_session PRIVATE c_std_11)
        target_compile_options(cdin_backend_integration_session PRIVATE
            $<$<C_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wshadow;-Wconversion;-Wstrict-prototypes;-Wformat=2;-Wpedantic;-Werror>)
        add_test(NAME backend_integration_session COMMAND cdin_backend_integration_session)

        add_executable(cdin_backend_integration_kernel_document tests/integration/kernel_document_workflow.c)
        target_include_directories(cdin_backend_integration_kernel_document PRIVATE src)
        target_link_libraries(cdin_backend_integration_kernel_document PRIVATE
            cdin::backend PkgConfig::CDIN_KERNEL)
        cdin_enable_strict_test_warnings(cdin_backend_integration_kernel_document)
        add_test(NAME backend_integration_kernel_document
                 COMMAND cdin_backend_integration_kernel_document)

        # Keep integration executables runnable without global loader changes.
        if(UNIX)
            set_target_properties(
                cdin_backend_integration_session
                cdin_backend_integration_kernel_document
                PROPERTIES BUILD_RPATH "${CDIN_KERNEL_LIBDIR}")
        elseif(WIN32)
            file(GLOB CDIN_KERNEL_RUNTIME_DLLS "${CDIN_KERNEL_BINDIR}/*.dll")
            if(CDIN_KERNEL_RUNTIME_DLLS)
                foreach(integration_target IN ITEMS
                    cdin_backend_integration_session
                    cdin_backend_integration_kernel_document)
                    add_custom_command(TARGET ${integration_target} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            ${CDIN_KERNEL_RUNTIME_DLLS}
                            "$<TARGET_FILE_DIR:${integration_target}>"
                        COMMAND_EXPAND_LISTS)
                endforeach()
            endif()
        endif()
    endif()
endif()
