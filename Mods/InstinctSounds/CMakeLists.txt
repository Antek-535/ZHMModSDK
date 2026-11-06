# BEST GUESS - compare with Mods/Freecam/CMakeLists.txt in your fork and
# copy its exact pattern if this differs (the SDK changes this between versions).

add_mod(InstinctSounds
    InstinctSounds.cpp
    InstinctSounds.h
)

target_link_libraries(InstinctSounds PRIVATE winmm comdlg32)
