# A: MGPU-ShadowMap (section 15). WIN32 application (A: SubSystem Windows, WinMain in Source.cpp).
set(TargetName MgpuShadowMap)

set(Sources
  FrameResource.cpp
  FrameResource.h
  HybridShadowApp.cpp
  HybridShadowApp.h
  SceneRenderer.cpp
  SceneRenderer.h
  Source.cpp
)

add_executable(${TargetName} WIN32 ${Sources})

target_link_libraries(${TargetName} PRIVATE Mgpu)

nau_add_compile_options(${TargetName})

# A: MGPU-ShadowMap.vcxproj builds with ConformanceMode=false, as the Mgpu module.
target_compile_options(${TargetName} PRIVATE /permissive)

# A: LocalDebuggerWorkingDirectory = OutDir (shaders and data are loaded relative to the working directory).
set_target_properties(${TargetName} PROPERTIES
  FOLDER "${NauEngineFolder}/core/modules"
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:${TargetName}>"
)

# A: Build/SampleResources.targets - shared shaders and data first, then the project shaders override the common ones.
mgpu_copy_runtime_resources(${TargetName})
add_custom_command(TARGET ${TargetName} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/Shaders $<TARGET_FILE_DIR:${TargetName}>/Shaders)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${Sources})
