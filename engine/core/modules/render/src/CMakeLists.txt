
set(TargetName Render)

cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH moduleRoot)

nau_collect_files(Sources
  RELATIVE ${moduleRoot}/src
  DIRECTORIES ${moduleRoot}/src
  MASK "*.cpp" "*.h"
  EXCLUDE
    "/platform/.*"
)


nau_collect_files(Headers
    RELATIVE ${moduleRoot}
    DIRECTORIES ${moduleRoot}/include
    MASK "*.h"
    PREPEND "../"
)

#if (${Platform_Windows})
#  nau_collect_files(Sources
#    RELATIVE ${moduleRoot}/src
#    DIRECTORIES
#      ${moduleRoot}/src/include/runtime/platform/windows
#      ${moduleRoot}/src/platform/windows
#    MASK "*.cpp" "*.h"
#  )
#endif()

nau_add_module(${TargetName}
  SOURCES ${Sources} ${Headers}
  FOLDER "${NauEngineFolder}/core/modules"
  EXPORT_AS_LIBRARY
)

target_precompile_headers(${TargetName} PRIVATE pch.h)
target_include_directories(${TargetName} PRIVATE 
  ${moduleRoot}/src
)

target_compile_definitions(${TargetName} PRIVATE 
  NAU_RENDER_BUILD
  INSIDE_DRIVER=1
)

target_link_libraries(${TargetName} PUBLIC
  binPack2D
  RenderDoc
  adlSdk
  dxc
  ska_hash_map
  xxHash
)

if(WIN32)
  target_link_libraries(${TargetName} PRIVATE d3d12 dxgi)
endif()

nau_target_link_modules(${TargetName}
  NauKernel
  vectormath
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${Sources})
source_group(TREE ${moduleRoot}/include PREFIX Headers FILES ${Headers})

target_include_directories(${TargetName} PUBLIC
    $<BUILD_INTERFACE:${moduleRoot}/include>  
    $<INSTALL_INTERFACE:$<INSTALL_PREFIX>/include/core/modules/render/include>
)

target_include_directories(${TargetName} PRIVATE
    $<BUILD_INTERFACE:${moduleRoot}/src/drv3d_DX12>
)

## Module API

target_compile_definitions(${TargetName} PUBLIC
    _TARGET_PC=1
    _TARGET_PC_WIN=1
    _TARGET_64BIT=1
    _TARGET_SIMD_SSE=1
)

install(DIRECTORY ${moduleRoot}/include
    DESTINATION ${CMAKE_INSTALL_PREFIX}/include/core/modules/render
    FILES_MATCHING 
      PATTERN "*.h"
      PATTERN "*.hpp"
      PATTERN "*.ipp"
)

nau_install(${TargetName} core)
