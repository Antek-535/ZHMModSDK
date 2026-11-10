DramaticAimingShift - ZHMModSDK port of Dramatic Aiming Mod 1.2.1

Behaviour (values extracted from the original entity patch):
  * Aim a gun (EnterAimAt/ExitAimAt) AND hold Shift
  * +65 ms   grey overlay blends in over 0.2 s
  * +150 ms  game time multiplier -> 0.3
  * Ave Maria starts immediately (0.1 s fade), stops on release
  * Release Shift / stop aiming: time -> 1.0, overlay out, music fades out

Audio: put your own files next to the plugin DLL:
  ave_maria.mp3|wav|wma     (required for music)
  slowmo_start.wav          (optional)
  slowmo_end.wav            (optional)
