# EM4 Debug

An experimental native plugin package for **911 First Responders / Emergency 4** on Windows. It adds an in-game diagnostics menu and a window-mode shortcut without modifying the game's executable or configuration file.

## Features

| Key | Action |
| --- | --- |
| **Ctrl + F6** | Retry the last changed texture or model. If `EM4Plugins\reload_target.txt` names a resource, try that one instead. A manual model target needs a prior file change so the watcher knows its physical path. |
| **Ctrl + F7** | In the command reload build, recompile the last changed `Game/Command` script. In the default build, show the disabled status. |
| **F6** | Save `EM4Plugins\debug_snapshot.txt` with performance, graphics, and recent log information. |
| **F7** | On the Logs page, switch between all lines and likely issues. |
| **F8** | On the Logs page, switch between the game's `logfile.txt` and the plugin loader log. |
| **Ctrl + F10** | Cycle through Overview, Timing, Graphics, Logs, Hot Reload, Unit Inspector, and Map Paths. |
| **F10** | Show or hide the current debug page, including its paths when Map Paths is selected. |
| **F11** | Toggle the game window. From fullscreen, it opens a centered window at 80% of the monitor size. From a normal window, it fills the monitor without borders. Press F11 again to restore the original state. |
| **Ctrl + Shift + F11** | Restore and focus the game if it remains minimized after switching to a texture editor or another app. This shortcut works while another app has focus. |
| **Page Up / Page Down** | Scroll the Logs or Unit Inspector page. **End** returns to the newest log entries or the first inspected command. |

The plugin does not bind F1 through F5, F9, or F12, which the game uses for its own commands.

The menu appears automatically when the game starts. Loader and plugin messages are written to `EM4Plugins\em4_plugins.log` in the game directory.

Some legacy fullscreen sessions remain minimized after switching to another app. Press **Ctrl + Shift + F11** to recover the existing game window without restarting or losing the current session.

The Overview page shows FPS, average and 95th-percentile frame time, the worst recent frame, CPU use, working and private RAM, process handles, viewport size, window style, and uptime. CPU use is normalized against all logical processors. The Timing page includes a 120-frame graph. The Graphics page shows the OpenGL vendor, renderer, version, maximum texture size, depth bits, and pixel format.

The Logs page reads the most recent 512 KiB of each log and retains up to 96 recent lines plus 96 likely issues. The issue filter uses markers and words such as `error`, `failed`, and `not found`; it is a convenience filter, not a severity level reported by the game. The snapshot contains the same bounded log history.

## Inspect a selected vehicle

Select a vehicle in the game and open **Unit Inspector** with **Ctrl + F10**. The page lists the commands attached to the first selected vehicle, their group keys, the command script paths reported by the engine, and each command's latest availability check. **Page Up** and **Page Down** move through the list. If multiple vehicles are selected, the header shows their count and inspects the first one. The hexadecimal vehicle identifier is its current memory address, useful for comparing logs during one session.

To investigate a command that is missing entirely, create `EM4Plugins/inspect_command.txt` in the game folder and put the command name on its first line, for example `VCmdTeste`. The page then reports whether the name is attached to the selected vehicle, registered in the command manager but not attached, or absent from the manager. The file is optional and can be edited while the game runs; the inspector reads it again within one second.

`CheckPossible denied` means the engine's command availability check returned false for that vehicle during the last ten seconds. `Not checked recently` means the inspector has no recent result; it does not prove the command is unavailable. `Different manager instance` identifies an attached command whose name currently resolves to another native object. An attached, available command can still be hidden by other UI logic. The inspector reports the observed layer and does not claim to know every UI filtering rule. Inspection is read-only and works only with the verified Steam `Em4.exe` build.

## View map paths

Press **Ctrl + F10** until the Map Paths page is selected. Cyan lines connect the paths' nodes directly in the 3D scene, yellow dots mark every node, and each visible path has its name drawn over it. This page does not draw the large debug panel. Press **Ctrl + F10** again to return to Overview and hide the paths, or press **F10** to temporarily hide or show them. The path list refreshes when the scene changes.

The viewer reads the active Vision camera matrices to align route lines, nodes, and labels with the 3D scene, even when the game leaves a 2D interface projection active at the end of a frame. The route viewer reads the live scene and does not change map or mod files. It is restricted to the verified Steam executable and Vision DLL builds.

## Reload textures and models while the game runs

Automatic reload is on by default. Save a changed texture or `.v3o` file under the game's `Data` or `Mods` folder while the game runs. The plugin receives file change notifications and waits 750 ms after the last notification. Open the Hot Reload page with **Ctrl + F10** to see the last changed path, result, and queue status. Press **Ctrl + F6** to retry the last changed file.

For textures already loaded by Vision, the plugin reloads the resource. For a changed model, it copies the new `.v3o` beside the source with a `.__em4debug_` suffix and loads that copy as a new resource. It then uses the engine's `SetModel` method on matching entities. Each rendered frame also checks for newly spawned entities that still use the original model and switches them to the latest copy. If there are no matching entities at the time of the edit, the copy remains ready for later spawns. It keeps the old resource loaded, avoiding the direct unload that previously removed the vehicle mesh and crashed the game. The source `.v3o` is never overwritten by the plugin. The generated model copies remain in the mod folder because a game save may reference them. Avoid saving a test session after swapping models unless you intend to keep those copies.

The optional `EM4Plugins\reload_target.txt` file remains available for manual reload. Replace its first line with one loaded resource path and press **Ctrl + F6**. Leave its first line as a comment to make Ctrl + F6 retry the last changed asset.

```text
mod:/Textures/MyTruck.dds
```

Use the resource's loaded engine path, such as `mod:/Textures/MyTruck.dds`. The manual selection accepts `.v3o`, `.dds`, `.tga`, `.png`, `.bmp`, `.jpg`, and `.jpeg`. A `.v3o` selection must match the last changed file so the watcher can supply its physical path. The path must identify a resource already held by the engine. A filesystem path such as `C:\Mods\...` is not an engine path.

The plugin watches loose files in `Data` and `Mods` and handles them on the render thread. Assets inside archives and unloaded assets are not reloaded. A changed file in an inactive mod can match a loaded resource with the same relative path, but the model swap will proceed only if the copied model loads through the game's current virtual file system. The feature is restricted to the verified Steam engine DLL build. Keep a backup of edited assets because an invalid model or texture can make the game fail to render the resource.

## Script changes

The file watcher detects saved `.script` files under `Scripts/Game/Command` and shows their paths on the Hot Reload page. In a build made with `-EnableCommandScriptReload`, save an edit to a command script, wait at least two seconds, return to the game, and press **Ctrl + F7** once. The plugin recompiles the last changed script while keeping the current game session. It rebinds existing native command objects to the new script and restores their name lookup so newly spawned units use them as well. The in-game result and `EM4Plugins/em4_plugins.log` report the outcome. This is a manual action, not an automatic compile on every save.

This implementation was confirmed with `teste.script` and the Sprinter in the Blumenau mod. The command executed changed behavior on both an existing vehicle and one spawned after reload. It supports active scripts in `Game/Command` on the verified Steam `Em4.exe` build. It retains old compiled script copies in memory and stops after 16 updates per game session. Changes to command class names or command lists may require a fresh session. Other command scripts and mods have not been validated yet. Mission scripts are ignored and never recompiled by this feature.

The default build keeps live command reload disabled. Earlier approaches that unloaded scripts or rebuilt every command caused crashes or removed custom vehicle commands. See [the investigation notes](docs/script-reload-investigation.md) for the failure analysis and verified engine addresses.

## Compatibility

This project was built and tested with the 32-bit `Em4.exe` from the Steam release of **911 First Responders**. The overlay hooks the delayed `GDI32.dll!SwapBuffers` import in `vBase71.dll`, so another edition or a modified engine DLL may need a different hook. The loader uses a local `dinput8.dll` proxy. Check for an existing `dinput8.dll` before installing because two proxy DLLs cannot occupy the same filename.

## Build

Requirements: Windows, PowerShell, and [Zig 0.13.0](https://ziglang.org/download/0.13.0/). The build targets x86 Windows and places its output in `build/`.

```powershell
& .\build.ps1 -ZigPath 'C:\path\to\zig.exe'
```

If `zig.exe` is on `PATH`, run `& .\build.ps1` instead. The generated DLLs are ignored by Git. Build locally before installation.

To build the tested command-script reload path, add the opt-in switch:

```powershell
& .\build.ps1 -ZigPath 'C:\path\to\zig.exe' -EnableCommandScriptReload
```

This build remains experimental. Use a test session when trying a different command script. The build targets only the verified Steam executable and does not reload mission scripts.

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

To update only the window and recovery shortcuts, close the game and run:

```powershell
& .\install.ps1 -GamePath 'C:\path\to\911 First Responders' -UpgradeBorderless
```

This checks the recorded hashes, replaces only `em4_borderless.dll`, and keeps the loader and debug menu installed.

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
src/unit_inspector.c      Selected vehicle commands and availability checks
src/path_viewer.c         Live map route inventory and drawing
src/auto_watch.c          Windows file change notifications for Data and Mods
include/hot_reload.h      Hot reload interface for the menu
include/unit_inspector.h  Unit Inspector data interface
include/path_viewer.h     Route viewer interface
examples/reload_target.txt  Editable resource path template
src/borderless.c          Window-mode toggle and F11 handler
src/*.def                Explicit DLL exports
build.ps1                x86 build script
install.ps1              Install and uninstall script
```

The loader calls `BOOL WINAPI EM4PluginInit(const EM4HostAPI *host)` for each DLL in `EM4Plugins/` that exports that function. The API currently provides the game window handle and a log callback. Plugins remain loaded until the game exits.

## Validation and limitations

The x86 build, DirectInput forwarding, window restoration, and OpenGL drawing were tested separately. A windowed OpenGL smoke test verified all five pages, a real Windows file change notification, graphics state restoration, issue retention after routine log spam, snapshot creation, and reading the optional selected reload path. In the game, changing a loaded texture reloaded it successfully. Directly unloading a changed vehicle `.v3o` removed the visible mesh and was followed by a game exit. The copy-and-swap path was tested with `Sprinter.v3o` in the Blumenau mod: existing entities and vehicles spawned after an edit received the new model. A second edit in the same session also updated existing and newly spawned entities. Other model types have not yet been tested. The opt-in command reload build was tested with `teste.script`: the game stayed open, and `VCmdTeste` executed the edited behavior on the original Sprinter and a new one.

The plugin API is experimental and does not expose the game's internal object model. The native `.vplugin` contract used by the Vision engine has not been characterized here. No license has been selected for this source repository.
