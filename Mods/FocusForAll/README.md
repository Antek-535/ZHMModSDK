# FocusForAll (ZHMModSDK plugin)

Hold SHIFT + right mouse (aim) with ANY weapon -> time slows down; release -> back to normal.

## Build
1. Clone ZHMModSDK (github.com/OrfeasZ/ZHMModSDK) and its submodules.
2. Copy this folder to ZHMModSDK/Mods/FocusForAll and register it in Mods/CMakeLists.txt
   the same way the other mods are.
3. Build with Visual Studio 2022 / CMake (Release x64).
4. Put FocusForAll.dll in the `mods` folder next to the SDK launcher / game, then enable it in the SDK menu.

## Tweaks (src/FocusForAll.h)
- m_FocusScale: 0.30 = 30% speed. Lower = slower.
- m_BlendSpeed: ease in/out speed.

## Notes
- Aim detection is the right mouse button (keyboard/mouse only). Controllers need an input hook.
- Member names for the time manager may differ per SDK version; see SetTimeScale().
- Slowing global time also slows the player; this is a simple version of the sniper-style focus.
