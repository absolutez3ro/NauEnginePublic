cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(NauMultiGpuTests LANGUAGES CXX)
    enable_testing()
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory("../../../../../3rdparty_libs/googletest" "googletest")
endif()

if(WIN32)
    add_executable(test_multi_gpu test_multi_gpu.cpp test_device_resource.cpp test_main.cpp
        ../../src/drv3d_DX12/device_resource.cpp)
    target_compile_features(test_multi_gpu PRIVATE cxx_std_20)
    target_compile_definitions(test_multi_gpu PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(test_multi_gpu PRIVATE "../../include" "../../src/drv3d_DX12")
    # The tests only use header-only EASTL array/unique_ptr, keeping standalone builds independent of NauKernel.
    target_include_directories(test_multi_gpu PRIVATE
        "../../../../../3rdparty_libs/EASTL/include"
        "../../../../../3rdparty_libs/EABase/include/Common")
    # Bundled allocator.h has an undefined pName in its debug realloc branch. These tests
    # do not use that allocator; omit allocation-name arguments without disabling assertions.
    target_compile_definitions(test_multi_gpu PRIVATE EASTL_DEBUGPARAMS_LEVEL=0)
    target_link_libraries(test_multi_gpu PRIVATE gtest d3d12 dxgi dxguid)
    if(COMMAND nau_add_compile_options)
        nau_add_compile_options(test_multi_gpu)
    endif()
    include(GoogleTest)
    gtest_discover_tests(test_multi_gpu DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 10
        NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)
endif()
