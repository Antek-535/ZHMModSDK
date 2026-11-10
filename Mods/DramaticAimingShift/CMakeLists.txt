add_library(DramaticAimingShift SHARED
    src/DramaticAimingShift.cpp
    src/DramaticAimingShift.h
)

target_include_directories(DramaticAimingShift PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(DramaticAimingShift ZHMModSDK winmm)
set_target_properties(DramaticAimingShift PROPERTIES CXX_STANDARD 23)
