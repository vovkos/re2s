include_directories(${CMAKE_CURRENT_LIST_DIR}/../re2)

add_executable (
  playtest
  main.cpp
)

target_link_libraries(
  playtest
  re2
)

if(UNIX AND NOT APPLE)
  target_link_libraries(
    playtest
    pthread
  )
endif()
