
set(TargetName Graphics)
#set(ApiTargetName ${TargetName}Api)

cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH moduleRoot)

nau_collect_files(Sources
    RELATIVE ${moduleRoot}/src
    DIRECTORIES ${moduleRoot}/src
    MASK "*.cpp" "*.h" "*.hlsli"
    EXCLUDE
    "/platform/.*"
)

nau_collect_files(Headers
    RELATIVE ${moduleRoot}
    DIRECTORIES ${moduleRoot}/include
    MASK "*.h"
    PREPEND "../"
)

# if (${Platform_Windows})
#   nau_collect_files(SOURCES
#     RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
#     DIRECTORIES
#       ${CMAKE_CURRENT_SOURCE_DIR}/include/runtime/platform/windows
#       ${CMAKE_CURRENT_SOURCE_DIR}/platform/windows
#     MASK "*.cpp" "*.h"
#   )
# endif()

nau_add_module(${TargetName}
    SOURCES ${Sources} ${Headers}
    FOLDER "${NauEngineFolder}/core/modules"
    EXPORT_AS_LIBRARY
    #  INTERFACE_TARGET ${ApiTargetName}
)

target_precompile_headers(${TargetName} PRIVATE pch.h)
target_include_directories(${TargetName} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/internal
)

target_include_directories(${TargetName} PUBLIC
    $<BUILD_INTERFACE:${moduleRoot}/include>  
    $<INSTALL_INTERFACE:$<INSTALL_PREFIX>/include/core/modules/graphics/include>
)

target_link_libraries(${TargetName} PRIVATE
    Render
    PlatformAppApi
    CoreScene
    CoreInput
    NauKernel
    Animation
    D3DCompiler.lib
    d3d12.lib
    dxgi.lib
    imgui
    gainput
    DebugRenderer
    ui
    VFX
)

target_link_libraries(${TargetName} PUBLIC
    Render
    CoreInput
    GraphicsAssets
    CoreAssets
)

target_compile_definitions(${TargetName} PRIVATE
    NAU_GRAPHICS_BUILD=1
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX Sources FILES ${Sources})
source_group(TREE ${moduleRoot}/include PREFIX Headers FILES ${Headers})

## Module API
# nau_collect_files(ApiHeaders
#   RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
#   DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/include
#   MASK "*.h"
# )

# target_sources(${ApiTargetName} INTERFACE ${ApiHeaders})
# target_include_directories(${ApiTargetName} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# set_target_properties (${ApiTargetName} PROPERTIES
#     FOLDER "core/modules"
# )

install(DIRECTORY ${moduleRoot}/include
    DESTINATION ${CMAKE_INSTALL_PREFIX}/include/core/modules/graphics
    FILES_MATCHING 
      PATTERN "*.h"
      PATTERN "*.hpp"
      PATTERN "*.ipp"
)

nau_install(${TargetName} core)