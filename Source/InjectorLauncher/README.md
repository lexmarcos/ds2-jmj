# Injector.exe

Starts the game and loads `Injector.dll` into it.

On Windows the loader is itself a Windows program and injects directly. On Linux
it cannot: the game is a Windows process inside a Proton prefix, and a native
Linux process reaches neither its wineserver session nor its pressure-vessel
container. So the Linux loader points Steam's launch options at a wrapper that
swaps the game executable in Proton's command line for this program, which then
runs inside the prefix and does what the Windows loader does.

```
Injector.exe <game exe> [args passed through to the game]
```

`Injector.dll` and `Injector.config` must sit next to it, because the injector
resolves its config as `<dll directory>/Injector.config`. The Linux loader
copies all three into the game directory.

## Building

This is plain Win32 with no Detours and no MSVC extensions, so it cross-compiles
from Linux:

```bash
sudo apt install mingw-w64
./build.sh
```

`Injector.dll` is a different matter and still needs a Windows toolchain: it
depends on Detours.

## Notes

- Steam waits on this process, so it outlives the game and returns the game's
  exit code.
- The game path arrives as a Linux path, because Wine converts the program it
  launches but not the arguments it passes on. `ToWindowsPath` handles that
  through Wine's own `wine_get_dos_file_name`, falling back to the `Z:` mapping
  every prefix has.
- A failed injection is logged and the game still starts, on the retail servers.
  Losing the private server beats losing the ability to play.
- Everything is written to `DS2OS_Injector.log` next to the executable.

## Verified

The injection chain was exercised end to end under Proton's Wine against a
stand-in target: the Linux path converted to `Z:\...`, the process started, and
the DLL's `DllMain` ran inside it. What has not been tested is the real game,
where Steam's DRM is also unpacking while this runs.
