set(TargetName SampleCommonLib)

nau_collect_files(Sources
    DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    MASK "*.cpp" "*.h"
)

add_library(${TargetName} STATIC ${Sources})

target_link_libraries(${TargetName} PUBLIC
    PlatformAppApi
    NauFramework
    CoreInput
    Graphics
    imgui
)

target_include_directories(${TargetName} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_precompile_headers(${TargetName} PRIVATE pch.h)

nau_add_compile_options(${TargetName})

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${Sources})
set_target_properties(${TargetName} PROPERTIES
    FOLDER "${NauEngineFolder}/samples"
)
