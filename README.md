# EM4 Debug

An experimental native plugin package for **911 First Responders / Emergency 4** on Windows. It adds an in-game performance overlay and a window-mode shortcut without modifying the game's executable or configuration file.

## Features

| Key | Action |
| --- | --- |
| **F10** | Show or hide the `EM4 DEBUG` overlay. The overlay displays FPS, process RAM usage, render viewport size, and window mode. |
| **F11** | Toggle the game window. From fullscreen, it opens a centered window at 80% of the monitor size. From a normal window, it fills the monitor without borders. Press F11 again to restore the original state. |

The overlay appears automatically when the game starts. Loader and plugin messages are written to `EM4Plugins\em4_plugins.log` in the game directory.

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

The installer places `dinput8.dll` beside `Em4.exe` and `em4_debug.dll` plus `em4_borderless.dll` in `EM4Plugins/`. It records SHA-256 hashes in `EM4Plugins/install-manifest.json`. It stops if any destination DLL or manifest already exists. It does not edit `em4.cfg`.

To remove the files installed by this script:

```powershell
& .\install.ps1 -GamePath 'C:\path\to\911 First Responders' -Uninstall
```

Uninstall checks the recorded hashes before removing files. If a file has changed, the script stops so it cannot remove an unrelated mod. The manifest is specific to the installed build.

## Project layout

```text
include/em4_plugin.h       Small host API shared by the loader and plugins
src/dinput8_proxy.c        DirectInput proxy and plugin loader
src/debug_overlay.c       OpenGL overlay and F10 handler
src/borderless.c          Window-mode toggle and F11 handler
src/*.def                Explicit DLL exports
build.ps1                x86 build script
install.ps1              Install and uninstall script
```

The loader calls `BOOL WINAPI EM4PluginInit(const EM4HostAPI *host)` for each DLL in `EM4Plugins/` that exports that function. The API currently provides the game window handle and a log callback. Plugins remain loaded until the game exits.

## Validation and limitations

The x86 build, DirectInput forwarding, window restoration, and OpenGL drawing were tested separately. In the game, the loader recorded both plugins, the overlay hook received frames, F10 hid and showed the panel, and F11 changed and restored the window. The player confirmed that the FPS and RAM display was visible.

The plugin API is experimental and does not expose the game's internal object model. The native `.vplugin` contract used by the Vision engine has not been characterized here. No license has been selected for this source repository.
