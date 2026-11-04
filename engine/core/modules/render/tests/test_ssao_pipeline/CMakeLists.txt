cmake_minimum_required(VERSION 3.24)
project(SsaoPipelineTests LANGUAGES CXX)
enable_testing()
get_filename_component(NAU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
add_subdirectory("${NAU_ROOT}/engine/3rdparty_libs/googletest" googletest)
add_executable(test_ssao_pipeline
    test_ssao_pipeline.cpp
    "${NAU_ROOT}/engine/core/modules/render/src/drv3d_DX12/device_resource.cpp"
    "${NAU_ROOT}/engine/core/modules/render/src/drv3d_DX12/device_ssao_pass.cpp"
    "${NAU_ROOT}/engine/3rdparty_libs/EASTL/source/allocator_eastl.cpp"
    "${NAU_ROOT}/engine/3rdparty_libs/EASTL/source/assert.cpp"
)
target_compile_features(test_ssao_pipeline PRIVATE cxx_std_20)
target_compile_definitions(test_ssao_pipeline PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN EASTL_DEBUGPARAMS_LEVEL=0)
target_include_directories(test_ssao_pipeline PRIVATE
    "${NAU_ROOT}/engine/3rdparty_libs/EASTL/include"
    "${NAU_ROOT}/engine/3rdparty_libs/EABase/include/Common"
    "${NAU_ROOT}/engine/core/modules/render/include"
    "${NAU_ROOT}/engine/core/modules/render/src/drv3d_DX12"
)
target_link_libraries(test_ssao_pipeline PRIVATE gtest_main d3d12 dxgi d3dcompiler dxguid)
include(GoogleTest)
gtest_discover_tests(test_ssao_pipeline DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)
