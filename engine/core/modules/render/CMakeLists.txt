add_subdirectory(src)

if(NAU_CORE_TESTS AND WIN32)
  add_subdirectory(tests/test_multi_gpu)
endif()

# TODO NAU-2089
# if (NAU_CORE_TESTS)
#     nau_collect_cmake_subdirectories(tests ${CMAKE_CURRENT_SOURCE_DIR}/tests)
#     foreach(test ${tests})
#         add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tests/${test})
#     endforeach()
# endif()
