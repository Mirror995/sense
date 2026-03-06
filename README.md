# sense

Use DualSense `CREATE` button to trigger Windows capture actions.

## Behavior

- Single click `CREATE` -> `Win + Alt + PrintScreen`
- Double click `CREATE` -> `Win + Alt + G`

Double-click window is `300ms`.

## Tray

- Left click or right click tray icon opens menu.
- Menu items:
  - Pause/Resume
  - Exit

## Build (Windows)

This project is already configured for local folder `SDL3-3.4.2` in the repo root.

1. Keep this structure:
   - `sense/`
   - `SDL3-3.4.2/`
2. Open `sense.slnx` with Visual Studio 2022.
3. Build `x64` Debug/Release.

Project uses:

- Include: `..\SDL3-3.4.2\include`
- Lib: `..\SDL3-3.4.2\lib\x64`
- Dependency: `SDL3.lib`

## Run (Windows)

Post-build step automatically copies `SDL3.dll` to output folder.

Then run `sense.exe`, keep it running in background, and connect DualSense (USB or Bluetooth).
