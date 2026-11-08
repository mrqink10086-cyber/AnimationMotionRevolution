cmake_minimum_required(VERSION 3.25)

project(AnimationMotionRevolution VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(COMMONLIBSSE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../CommonLibSSE-NG-ng" CACHE PATH "CommonLibSSE-NG ng source checkout")
set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_PATCH_SAFETY ON CACHE BOOL "" FORCE)
set(COMMONLIB_PREBUILT OFF CACHE STRING "" FORCE)
add_subdirectory("${COMMONLIBSSE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/CommonLibSSE")
include("${COMMONLIBSSE_SOURCE_DIR}/cmake/CommonLibSSE.cmake")
find_path(SIMPLEINI_INCLUDE_DIR SimpleIni.h REQUIRED)
option(AMR_ENABLE_TRUEHUD_DEBUG "Build TrueHUD debug visualizations" ON)

configure_file(cmake/version.rc.in version.rc @ONLY)

add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "AnimationMotionRevolution"
    COMPATIBLE_RUNTIMES 1.6.1170.0
    MINIMUM_SKSE_VERSION 2.2.6
    STRUCT_DEPENDENT
    SOURCES
        source/AnimationMotionRevolutionAPI.cpp
        source/Hooks.cpp
        source/PhysicsHooks.cpp
        source/main.cpp
        source/pch.cpp
        source/Settings.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
)
target_include_directories(${PROJECT_NAME} PRIVATE include "${SIMPLEINI_INCLUDE_DIR}")
if(AMR_ENABLE_TRUEHUD_DEBUG)
    target_sources(${PROJECT_NAME} PRIVATE source/TrueHUDIntegration.cpp)
    target_compile_definitions(${PROJECT_NAME} PRIVATE AMR_ENABLE_TRUEHUD_DEBUG)
endif()
target_precompile_headers(${PROJECT_NAME} PRIVATE include/pch.h)
target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /permissive- /Zc:preprocessor /EHsc /fp:precise /utf-8)
    target_link_options(${PROJECT_NAME} PRIVATE /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF)
endif()

option(AMR_BUILD_TESTS "Build host motion and cache regression tests" OFF)
option(AMR_ENABLE_ASAN_TESTS "Instrument host regression tests with AddressSanitizer" OFF)
if(AMR_BUILD_TESTS)
    enable_testing()
    foreach(test_name IN ITEMS motion warp cache cache_exception)
        add_executable(amr_${test_name}_tests tests/${test_name}_tests.cpp)
        target_include_directories(amr_${test_name}_tests PRIVATE include)
        if(AMR_ENABLE_ASAN_TESTS AND MSVC)
            target_compile_options(amr_${test_name}_tests PRIVATE /fsanitize=address /Zi)
            target_link_options(amr_${test_name}_tests PRIVATE /INCREMENTAL:NO /DEBUG)
        endif()
        add_test(NAME amr_${test_name} COMMAND amr_${test_name}_tests)
    endforeach()
    add_executable(amr_benchmark_motion tests/benchmark_motion.cpp)
    target_include_directories(amr_benchmark_motion PRIVATE include)
    add_executable(amr_benchmark_interpolation tests/benchmark_interpolation.cpp)
    target_include_directories(amr_benchmark_interpolation PRIVATE include)
    add_test(NAME amr_interpolation COMMAND amr_benchmark_interpolation)
    add_executable(amr_benchmark_cache tests/benchmark_cache.cpp)
    target_include_directories(amr_benchmark_cache PRIVATE include)
    find_package(xbyak CONFIG REQUIRED)
    add_executable(amr_stack_tests tests/stack_tests.cpp)
    target_include_directories(amr_stack_tests PRIVATE include)
    target_link_libraries(amr_stack_tests PRIVATE xbyak::xbyak)
    add_test(NAME amr_stack COMMAND amr_stack_tests)
    if(MSVC)
        foreach(host_target IN ITEMS amr_motion_tests amr_warp_tests amr_cache_tests amr_cache_exception_tests amr_benchmark_motion amr_benchmark_interpolation amr_benchmark_cache)
            target_compile_options(${host_target} PRIVATE /fp:precise)
        endforeach()
    endif()
endif()

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION SKSE/Plugins COMPONENT AMR)
install(FILES "$<TARGET_PDB_FILE:${PROJECT_NAME}>" DESTINATION SKSE/Plugins COMPONENT AMR OPTIONAL)
install(FILES config/AnimationMotionRevolution.ini DESTINATION SKSE/Plugins COMPONENT AMR)
install(FILES package/SKSE/Plugins/BehaviorDataInjector/AnimationMotionRevolution_BDI.json DESTINATION SKSE/Plugins/BehaviorDataInjector COMPONENT AMR)
install(FILES LICENSE README.md DESTINATION . COMPONENT AMR)
install(DIRECTORY docs DESTINATION . COMPONENT AMR)
install(FILES tools/verify_1170.py DESTINATION tools COMPONENT AMR)
set(AMR_DEPLOY_DIR "" CACHE PATH "Optional Data directory or mod directory for post-build deployment")
if(AMR_DEPLOY_DIR)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${AMR_DEPLOY_DIR}/SKSE/Plugins"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${AMR_DEPLOY_DIR}/SKSE/Plugins/"
        VERBATIM)
endif()
