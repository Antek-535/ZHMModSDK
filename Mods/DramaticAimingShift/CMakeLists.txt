# Drop this folder into ZHMModSDK/Mods/DramaticAimingShift and add
#   add_subdirectory(DramaticAimingShift)
# to ZHMModSDK/Mods/CMakeLists.txt
add_library(DramaticAimingShift SHARED src/DramaticAimingShift.cpp src/DramaticAimingShift.h)

target_include_directories(DramaticAimingShift PRIVATE src)
target_link_libraries(DramaticAimingShift ZHMModSDK winmm)

install_mod(DramaticAimingShift)
