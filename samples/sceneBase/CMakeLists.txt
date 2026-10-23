set(TargetName SceneBaseSample)

nau_collect_files(Sources
    DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    MASK "*.cpp" "*.h"
)

add_executable(${TargetName}
    ${Sources}
)

target_link_libraries(${TargetName} PRIVATE
    SampleCommonLib
    Animation
    Physics
    Graphics
    VFX
)

# Mgpu transfer bridge (root option NAU_ENABLE_MGPU, preset win_vs2022_x64_mgpu). The sample only calls
# nau::mgpu::bridge::setEnabled(), see sample_app_delegate.cpp.
if(NAU_ENABLE_MGPU)
  target_compile_definitions(${TargetName} PRIVATE NAU_ENABLE_MGPU=1)
  target_include_directories(${TargetName} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../engine/core/modules/mgpu/include)

  # Mgpu links WinPixEventRuntime (import library of a DLL): the DLL has to sit next to the executable.
  add_custom_command(TARGET ${TargetName} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:WinPixEventRuntime> $<TARGET_FILE_DIR:${TargetName}>)
endif()

target_precompile_headers(${TargetName} PRIVATE pch.h)
target_include_directories(${TargetName} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/content/shaders/include)

if(NOT NauSDKLibraries)
  file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/content/shaders DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/resources)

  nau_add_default_shaders(${TargetName} content shaders
    INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../engine/core/modules/render/include/nau/shaders
  )
  nau_add_default_shaders(${TargetName} resources shaders
    INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../engine/core/modules/render/include/nau/shaders
  )
endif()

nau_collect_files(SourcesMaterials
    DIRECTORIES content/materials
    MASK  "*.json"
    EXCLUDE
    "/platform/.*"
)
source_group(shaders/materials FILES ${SourcesMaterials})
target_sources(${TargetName} PRIVATE ${SourcesMaterials})

nau_target_link_modules(${TargetName}
    PlatformApp
    Animation
    CoreScene
    CoreAssets
    GraphicsAssets
    DebugRenderer
    CoreAssetFormats
    CoreInput
    Graphics
    Physics
    PhysicsJolt
    VFX
)

if(NauSDKLibraries)
  target_link_directories(inputDemo PUBLIC ${NauSDKLibraries})
endif()

nau_add_compile_options(${TargetName})

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${Sources})

set_target_properties(${TargetName} PROPERTIES
    FOLDER "${NauEngineFolder}/samples"
)
