BETTER INSTINCT  (ZHMModSDK mod for HITMAN World of Assassination)
=================================================================

What it does
------------
- Instinct slows time down and drains an Instinct meter.
- When the meter is empty (or too low), Instinct is blocked. It must recharge a bit before it works again.
- Gold, Absolution-style meter on screen, with a spark and sparkle particles while Instinct is in use.
  The meter is only shown in a mission (never in menus).
- Optional looping Instinct sound (.wav / .mp3), with fade in / fade out.
- Point shooting (Absolution style, experimental): hold the point shooting key (default: Mouse 4),
  aim at enemies to tag them (each tag costs meter), release the key and all tagged targets are shot.
  Targets are killed directly (not shot with a real weapon).

Using it
--------
1. Put BetterInstinct.dll in the game's "mods" folder (Retail\mods) and enable it in the ZHMModSDK menu.
2. Open the mod window with the "BETTER INSTINCT" button.
3. Set the Instinct key to your in-game Instinct bind and pick Hold or Toggle (match your in-game setting).
4. Choose a sound (preset, a file from the InstinctSounds folder next to the DLL, or a custom file path).

Settings
--------
All options are saved automatically to:
    %APPDATA%\BetterInstinct\BetterInstinct.cfg

If Instinct still works with an empty meter, open the Debug section in the mod window, find the game's
Instinct action in the list, click it, and tick the test checkbox to check it.

Notes
-----
- Time scales of about 0.30 or lower can stop Agent 47 from walking when the Enhanced First Person mod is used.
- Do not run this together with the old FocusForAll.dll.

Folder layout
-------------
BetterInstinct/
    README.txt
    src/BetterInstinct.cpp
    src/BetterInstinct.h
(The CMakeLists.txt for the mod is not included: use the one from your FocusForAll folder and rename
 FocusForAll to BetterInstinct inside it.)
