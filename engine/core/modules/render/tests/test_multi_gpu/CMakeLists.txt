cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(NauMultiGpuTests LANGUAGES CXX)
    enable_testing()
    if(WIN32)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        add_subdirectory("../../../../../3rdparty_libs/googletest" "googletest")
    endif()
endif()

add_executable(test_multi_gpu_status test_multi_gpu_status.cpp)
target_compile_features(test_multi_gpu_status PRIVATE cxx_std_20)
target_include_directories(test_multi_gpu_status PRIVATE "../../src/drv3d_DX12")
add_test(NAME MultiGpuFallbackStatus COMMAND test_multi_gpu_status)

if(WIN32)
    add_executable(test_multi_gpu test_multi_gpu.cpp test_multi_gpu_fence.cpp test_multi_gpu_transfer.cpp test_device_resource.cpp test_device_fullscreen_pass.cpp test_device_ssao_pass.cpp test_main.cpp)
    if(TARGET Render AND NOT BUILD_SHARED_LIBS)
        target_link_libraries(test_multi_gpu PRIVATE Render)
        target_compile_definitions(test_multi_gpu PRIVATE NAU_MGPU_TEST_ENGINE_ALLOCATOR)
    else()
        target_sources(test_multi_gpu PRIVATE
            ../../src/drv3d_DX12/device_resource.cpp ../../src/drv3d_DX12/device_fullscreen_pass.cpp ../../src/drv3d_DX12/device_ssao_pass.cpp
            ../../../../../3rdparty_libs/EASTL/source/assert.cpp)
    endif()
    target_compile_features(test_multi_gpu PRIVATE cxx_std_20)
    target_compile_definitions(test_multi_gpu PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    get_filename_component(NAU_SSAO_SHADER_DIR "../../../../../../samples/sceneBase/content/shaders" ABSOLUTE)
    target_compile_definitions(test_multi_gpu PRIVATE NAU_SSAO_SHADER_DIR="${NAU_SSAO_SHADER_DIR}")
    target_include_directories(test_multi_gpu PRIVATE "../../include" "../../src/drv3d_DX12")
    # EASTL containers and shared lifetime support remain independent of NauKernel in standalone builds.
    target_include_directories(test_multi_gpu PRIVATE
        "../../../../../3rdparty_libs/EASTL/include"
        "../../../../../3rdparty_libs/EABase/include/Common")
    # Bundled allocator.h has an undefined pName in its debug realloc branch. These tests
    # do not use that allocator; omit allocation-name arguments without disabling assertions.
    if(NOT TARGET Render OR BUILD_SHARED_LIBS)
        target_compile_definitions(test_multi_gpu PRIVATE EASTL_DEBUGPARAMS_LEVEL=0)
    endif()
    # d3dcompiler compiles the HLSL of the test pass at run time; the engine itself receives ready bytecode.
    target_link_libraries(test_multi_gpu PRIVATE gtest d3d12 dxgi dxguid d3dcompiler)
    if(COMMAND nau_add_compile_options)
        nau_add_compile_options(test_multi_gpu)
    endif()
    include(GoogleTest)
    gtest_discover_tests(test_multi_gpu DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 10
        NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)
endif()
