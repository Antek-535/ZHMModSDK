# Drop this folder into ZHMModSDK/Mods/ and add_subdirectory(FocusForAll) in Mods/CMakeLists.txt
# (mirror whatever the other mods in that folder do, e.g. the install/output macro they use).
add_library(FocusForAll SHARED
    src/FocusForAll.cpp
    src/FocusForAll.h
)

target_include_directories(FocusForAll PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(FocusForAll ZHMModSDK)
set_target_properties(FocusForAll PROPERTIES CXX_STANDARD 23)
target_link_libraries(FocusForAll PRIVATE winmm)
