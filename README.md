# EM4 Debug

An experimental native plugin package for **911 First Responders / Emergency 4** on Windows. It adds an in-game diagnostics menu and a window-mode shortcut without modifying the game's executable or configuration file.

## Features

| Key | Action |
| --- | --- |
| **F5** | Retry the last changed asset. If `EM4Plugins\reload_target.txt` names a resource, reload that one instead. |
| **F6** | Save `EM4Plugins\debug_snapshot.txt` with performance, graphics, and recent log information. |
| **F7** | On the Logs page, switch between all lines and likely issues. |
| **F8** | On the Logs page, switch between the game's `logfile.txt` and the plugin loader log. |
| **F9** | Cycle through Overview, Timing, Graphics, Logs, and Hot Reload. |
| **F10** | Show or hide the `EM4 DEBUG` menu. |
| **F11** | Toggle the game window. From fullscreen, it opens a centered window at 80% of the monitor size. From a normal window, it fills the monitor without borders. Press F11 again to restore the original state. |
| **Page Up / Page Down** | Scroll through the selected log. **End** returns to the newest entries. |

The menu appears automatically when the game starts. Loader and plugin messages are written to `EM4Plugins\em4_plugins.log` in the game directory.

The Overview page shows FPS, average and 95th-percentile frame time, the worst recent frame, CPU use, working and private RAM, process handles, viewport size, window style, and uptime. CPU use is normalized against all logical processors. The Timing page includes a 120-frame graph. The Graphics page shows the OpenGL vendor, renderer, version, maximum texture size, depth bits, and pixel format.

The Logs page reads the most recent 512 KiB of each log and retains up to 96 recent lines plus 96 likely issues. The issue filter uses markers and words such as `error`, `failed`, and `not found`; it is a convenience filter, not a severity level reported by the game. The snapshot contains the same bounded log history.

## Reload models and textures while the game runs

Automatic reload is on by default. Save a changed `.v3o` model or texture file under the game's `Data` or `Mods` folder while the game runs. The plugin receives file change notifications, waits 750 ms after the last notification, finds matching resources already loaded by the Vision engine, and reloads them. Open the Hot Reload page with **F9** to see the last changed path, result, and queue status. Press **F5** to retry the last changed asset.

The optional `EM4Plugins\reload_target.txt` file remains available for manual reload. Replace its first line with one loaded engine path and press **F5**. Leave its first line as a comment to make F5 retry the last changed asset.

```text
mod:/Models/Vehicles/MyTruck.v3o
```

For a texture, use its loaded engine path instead, such as `mod:/Textures/MyTruck.dds`. The manual selection accepts `.v3o`, `.dds`, `.tga`, `.png`, `.bmp`, `.jpg`, and `.jpeg`. The path must identify a resource already held by the engine. A filesystem path such as `C:\Mods\...` is not an engine path.

The plugin watches loose files in `Data` and `Mods`, enumerates Vision's loaded model and texture resources after a relevant file changes, and asks the engine to reload matching resources on the render thread. Assets inside archives and unloaded assets are not reloaded. A changed file in an inactive mod can match a loaded resource with the same relative path; the engine still resolves the active resource when it reloads. The feature is restricted to the verified Steam engine DLL build. Keep a backup of edited assets because an invalid model or texture can make the game fail to render the resource.

## Compatibility

This project was built and tested with the 32-bit `Em4.exe` from the Steam release of **911 First Responders**. The overlay hooks the delayed `GDI32.dll!SwapBuffers` import in `vBase71.dll`, so another edition or a modified engine DLL may need a different hook. The loader uses a local `dinput8.dll` proxy. Check for an existing `dinput8.dll` before installing because two proxy DLLs cannot occupy the same filename.

## Build

Requirements: Windows, PowerShell, and [Zig 0.13.0](https://ziglang.org/download/0.13.0/). The build targets x86 Windows and places its output in `build/`.

```powershell
& .\build.ps1 -ZigPath 'C:\path\to\zig.exe'
```

If `zig.exe` is on `PATH`, run `& .\build.ps1` instead. The generated DLLs are ignored by Git. Build locally before installation.

## Install and uninstall

Close the game, then supply the folder containing `Em4.exe`:

```powershell
& .\install.ps1 -GamePath 'C:\path\to\911 First Responders'
```

The installer places `dinput8.dll` beside `Em4.exe` and `em4_debug.dll` plus `em4_borderless.dll` in `EM4Plugins/`. It also creates `EM4Plugins/reload_target.txt` if absent, and preserves your edits on upgrades and uninstall. It records SHA-256 hashes in `EM4Plugins/install-manifest.json`. It stops if any destination DLL or manifest already exists. It does not edit `em4.cfg`.

To update only the debug menu in an existing installation managed by this script, close the game and run:

```powershell
& .\install.ps1 -GamePath 'C:\path\to\911 First Responders' -UpgradeDebug
```

The upgrade verifies the current installation against its manifest, stages the new debug DLL, and updates its recorded hash. It leaves the loader and F11 plugin in place.

To remove the files installed by this script:

```powershell
& .\install.ps1 -GamePath 'C:\path\to\911 First Responders' -Uninstall
```

Uninstall checks the recorded hashes before removing files. If a file has changed, the script stops so it cannot remove an unrelated mod. The manifest is specific to the installed build.

## Project layout

```text
include/em4_plugin.h       Small host API shared by the loader and plugins
src/dinput8_proxy.c        DirectInput proxy and plugin loader
src/debug_overlay.c       OpenGL menu, log viewer, snapshot, and hotkeys
src/hot_reload.c          Resource matching and runtime reload
src/auto_watch.c          Windows file change notifications for Data and Mods
include/hot_reload.h      Hot reload interface for the menu
examples/reload_target.txt  Editable resource path template
src/borderless.c          Window-mode toggle and F11 handler
src/*.def                Explicit DLL exports
build.ps1                x86 build script
install.ps1              Install and uninstall script
```

The loader calls `BOOL WINAPI EM4PluginInit(const EM4HostAPI *host)` for each DLL in `EM4Plugins/` that exports that function. The API currently provides the game window handle and a log callback. Plugins remain loaded until the game exits.

## Validation and limitations

The x86 build, DirectInput forwarding, window restoration, and OpenGL drawing were tested separately. A windowed OpenGL smoke test verified all five pages, a real Windows file change notification, graphics state restoration, issue retention after routine log spam, snapshot creation, and reading the optional selected reload path. In the game, temporarily changing only the timestamp of a loaded `.v3o` and a loaded `.dds` produced `AUTO: reloaded 1/1` for each type. Both original timestamps were restored and the file hashes were unchanged. The player visually confirmed the earlier overlay version. The visual result of editing asset bytes has not yet been checked in the running game.

The plugin API is experimental and does not expose the game's internal object model. The native `.vplugin` contract used by the Vision engine has not been characterized here. No license has been selected for this source repository.
