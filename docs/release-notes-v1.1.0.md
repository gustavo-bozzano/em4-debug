# EM4 Debug 1.1

This release adds selected-unit WASD control and a third-person chase camera to the Windows package for the verified Steam edition of **911 First Responders / Emergency 4**.

Download `EM4-Debug-v1.1.0-win32.zip`, extract it, close the game, and run **Install EM4 Debug.cmd**. The setup wizard can install the plugin or update a managed v1.0 installation. Both `dinput8.dll` and `em4_debug.dll` must be updated for WASD control.

Select one vehicle or person in a loaded scene and press **Ctrl+F8**. Hold WASD to move relative to the chase camera. Press **Ctrl+F8**, **Esc**, or **Enter** to leave control mode and restore the normal view. Vehicle movement uses continuous direct control after a short calibration, while people use the game's native movement action. Camera movement and heading are smoothed. The input proxy prevents WASD from moving the normal camera at the same time.

Vehicle and person control were tested in a single-player Blumenau mod session. Vehicle steering, camera follow, and person precision were confirmed in-game after performance optimization. Multiplayer synchronization has not been tested. The vehicle controller may require `mod:Prototypes/Objects/Misc/empty.e4p`; without it, the plugin falls back to native movement actions that may brake between destinations.

The release retains the diagnostics menu, Unit Inspector, 3D map paths, texture and model reload, and experimental Ctrl+F7 command-script reload. Mission scripts are not recompiled. The package does not change `em4.cfg`, raise the multiplayer player limit, or add graphics effects.
