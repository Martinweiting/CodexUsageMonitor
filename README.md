# Codex Usage Monitor

Codex Usage Monitor is a native Windows desktop widget for viewing the current Codex account's usage limits.

It uses Win32, Direct2D, DirectWrite, and WinHTTP. The application does not require C#, WebView, or a separate background service.

Traditional Chinese documentation: [README-zh.md](README-zh.md)

## Features

- Displays weekly usage and, when available, the five-hour usage window.
- Shows used percentage, remaining percentage, expected pace, actual pace, and reset countdowns.
- Supports full, simple, and taskbar display modes.
- Uses a compact transparent Codex bubble in full and simple modes.
- Supports dragging, resizing, position locking, always-on-top, and startup launch.
- Provides a glass-transparency setting from 20% to 80%.
- Refreshes usage data automatically every 60 seconds and supports manual refresh.
- Provides English and Traditional Chinese interfaces.
- Uses read-only rate-limit reset information; it does not consume or reset credits.
- Stores widget settings in `%APPDATA%\CodexUsageMonitor\settings.ini`.

## Screenshots

### Full mode

![Full mode](IMG/1.png)

### Simple mode

![Simple mode](IMG/2.png)

## Data and privacy

The monitor reads the Codex authentication file already present on the local computer:

- `%USERPROFILE%\.codex\auth.json`
- `%CODEX_HOME%\auth.json`, when `CODEX_HOME` is configured

It uses the existing access token to request usage data from the Codex backend. The project does not include an authentication token, password, or personal account data. Do not commit your `auth.json`, environment files, or generated build output.

## Requirements

- Windows 10 or later
- Visual Studio with the MSVC C++ toolchain
- CMake, if using the CMake workflow

## Build

### Direct build

```cmd
build.cmd
```

The generated executable is `CodexUsageMonitor.exe`.

### CMake build

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Use a Visual Studio generator installed on your computer if the example generator is unavailable.

### Build and run tests

```powershell
cmake -S . -B build-tests -G "Visual Studio 18 2026" -A x64 -DCODEX_USAGE_MONITOR_BUILD_TESTS=ON
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

## Usage

- Hover over the bubble to expand it in full or simple mode.
- Click the bubble or expanded panel to keep it open.
- Drag the bubble or panel to move it.
- Drag the right, bottom, or bottom-right edge of the full panel to resize it.
- Use the right-click menu to refresh, change display mode, adjust transparency, toggle startup launch, lock the position, or exit.

Taskbar mode remains docked near the taskbar and does not expand on hover.

## Project layout

```text
src/       Application and usage-data code
tests/     Native and source-contract tests
assets/    Fonts, icons, and runtime assets
IMG/       Documentation screenshots
docs/      Public design and technical documentation
```

## Known limitations

- Automatic access-token refresh is not implemented; the existing `access_token` must remain valid.
- The usage parser depends on the Codex backend response format.
- If the backend does not provide a five-hour window, that value is shown as unavailable instead of being inferred.
- This is a desktop overlay, not the legacy Windows Gadget platform.

## License

No license file is currently included. Add a license before distributing the project if reuse terms are required.
