# Codex Usage Widget Liquid Glass Redesign

> Status: Design approved in conversation; pending written-spec review before implementation planning.

## Goal

Turn the existing Win32/C++ Codex usage widget into a draggable, translucent liquid-glass floating widget that is normally a transparent blue Codex icon and expands into the currently selected full, simple, or taskbar presentation on hover or click. Replace all Simplified Chinese player-facing copy with Traditional Chinese Taiwan wording, remove every action that consumes a rate-limit reset credit, add richer full-mode usage information, replace the text refresh button with a borderless reset-loop icon, and add a persisted horizontal transparency slider in settings.

## Current Project Seams

- `src/AppBarWindow.h` and `src/AppBarWindow.cpp` own the Win32 window, settings, display modes, drag handling, context menu, Direct2D painting, localization, timers, and refresh flow.
- `src/CodexUsageFetcher.h` and `src/CodexUsageFetcher.cpp` parse the usage snapshot and perform the read-only usage and reset-credit inventory requests.
- The current reset-credit consumption path is split between `AppBarWindow` confirmation state/messages and `CodexUsageFetcher`'s POST request. It must be removed as one feature, not merely hidden.
- Position and layout settings are stored in `%APPDATA%\\CodexUsageMonitor\\settings.ini`.
- The project has no Git metadata in the current workspace, so this specification cannot be committed here.

## Scope

### In scope

1. One morphing layered Win32 widget window with bubble, hover-expanded, and pinned-expanded states.
2. Per-pixel transparent liquid-glass rendering for the bubble and all three content modes.
3. Traditional Chinese Taiwan wording for the existing Chinese UI and Chinese README copy, while preserving English mode.
4. Read-only full-mode usage details derived from the existing `UsageSnapshot`.
5. Complete removal of rate-limit reset-credit consumption UI, state, request code, and POST endpoint.
6. Borderless circular-arrow refresh control with tooltip and loading feedback.
7. A temporary glass settings panel containing a live horizontal glass-transparency slider.
8. Automated tests for presentation state/geometry and static verification for removed paths and copy.

### Out of scope

- Changing the Codex API contract or inventing new remote usage fields.
- Implementing automatic token refresh for `auth.json`.
- Adding a second persistent usage panel or a web UI.
- Adding a user-triggered reset-credit consumption fallback.

## Presentation State and Interaction

The existing `AppBarWindow` remains the only persistent usage widget. Add an explicit presentation state rather than inferring behavior from several booleans:

- `Bubble`: only the transparent blue Codex icon is visible.
- `HoverExpanded`: entering the icon expands the same window into the selected display mode.
- `PinnedExpanded`: clicking while expanded locks the content open until the user clicks the floating icon again or clicks the upper-right `×` control.

Behavior rules:

- The application starts in `Bubble`, even if the previous session ended expanded.
- Hover expansion is temporary. When the pointer leaves the expanded shape and the state is not pinned, the window collapses immediately.
- Clicking the bubble while expanded changes `HoverExpanded` to `PinnedExpanded`. Clicking the icon again from `PinnedExpanded` collapses to `Bubble`.
- Clicking the upper-right `×` always collapses to `Bubble`; it never exits the application.
- The existing context-menu Exit command remains the application exit path.
- Refresh, mode selection, language selection, startup, always-on-top, lock-position, and reset-position behavior remain available unless explicitly changed below.

### Position and dragging

- Preserve the existing saved expanded rectangle as the source of truth for position and user-selected size.
- The bubble occupies the expanded rectangle's upper-right anchor so the widget does not jump when it changes shape.
- The bubble and expanded surface use the existing drag and lock-position rules. Dragging the bubble updates the saved expanded rectangle relative to the anchor.
- The selected taskbar mode keeps its narrow presentation and default taskbar-edge placement. Reset position restores that placement. A manually moved bubble is allowed when the position is unlocked so the requested drag behavior remains usable.
- Persist the expanded rectangle and transparency value; do not persist `HoverExpanded` or `PinnedExpanded`.

## Liquid Glass Rendering

Replace the opaque rectangular client paint with a per-pixel alpha rendering path:

- Use a `WS_EX_LAYERED | WS_EX_TOOLWINDOW` popup and update it from a 32-bit premultiplied BGRA surface.
- Render the surface with Direct2D into a top-down DIB or equivalent premultiplied bitmap, then publish it with `UpdateLayeredWindow`.
- Return transparent hit-test results outside the actual bubble/panel geometry so the unused rectangular bounds do not intercept the desktop.
- Keep the existing DPI-aware geometry model and recalculate the bitmap whenever the mode, size, DPI, state, or transparency value changes.

Material rules:

- The bubble is an organic cloud-shaped blue Codex mark with a translucent blue gradient, white inner edge highlight, and soft blue glow. No square bitmap background or square hit area is visible.
- The expanded surface uses a light blue/lavender translucent base, rounded corners, a thin pale border, an inner highlight, and a soft offset shadow. Dark system theme changes text and contrast colors but retains the glass material language.
- Cards, tracks, controls, and taskbar capsules use translucent layers rather than opaque colored blocks.
- Status colors are limited to small status dots, progress fills, and restrained glows: blue-green for healthy, amber for tight, coral for exhausted or failed.
- Text and critical status indicators remain more opaque than the material background for readability.

The icon should be rendered as a DPI-safe vector-like Direct2D shape matching the provided glossy Codex reference. If a raster fallback is needed, it must be a transparent PNG containing only the icon silhouette; the fallback must never include the reference image's square background.

## Display Content

### Full mode

The full glass panel includes:

- Header with Codex identity, account email, plan name, and current overall status.
- Separate 5-hour and weekly usage summaries showing used percentage, remaining percentage, reset countdown, reset timestamp, and the visual progress bar.
- Pace summary showing the current weekly cycle day, expected used percentage, actual used percentage, and the difference from the cycle budget as "more used than budget" or "less used than budget".
- Plan start and plan expiry information when available.
- Read-only reset inventory section titled exactly:

  `使用量限制重設 (Full reset Weekly + 5 hr)`

  It shows the number of available reset credits and the earliest expiry or no-expiry state. It has no action button.
- Last successful refresh time and countdown to the next automatic refresh.
- Borderless circular-arrow refresh control.
- Upper-right `×` collapse control.

### Simple mode

Keep the compact two-card layout, but apply the same glass material. Show 5-hour remaining, weekly remaining, current status, and the nearest reset countdown. The reset inventory remains read-only if it fits without making the compact surface unstable; otherwise the full mode remains the authoritative location for the inventory detail.

### Taskbar mode

Keep the narrow two-column remaining display and status strip, but use translucent glass capsules and the same status-color rules. It remains suitable for taskbar-adjacent placement.

## Traditional Chinese Taiwan Copy

The Chinese localization branch is Traditional Chinese, not Simplified Chinese. Keep the English branch intact. Use these terms consistently:

| Existing concept | Traditional Chinese Taiwan wording |
| --- | --- |
| Chinese language menu item | 繁體中文 |
| Codex usage widget | Codex 用量小工具 |
| Refresh | 重新整理 |
| Refresh usage | 重新整理額度 |
| Refresh interval | 重新整理間隔 |
| Weekly limit | 週限額 |
| 5-hour limit | 5 小時限額 |
| Remaining | 剩餘額度 |
| Plan | 方案 |
| Launch at startup | 開機自動啟動 |
| Always on top | 始終置頂 |
| Lock position | 鎖定位置 |
| Reset widget position | 重設小工具位置 |
| Exit | 離開 |
| Loading | 載入中 |
| Failed | 載入失敗 |
| Tight | 吃緊 |
| Exhausted | 已用罄 |
| Normal | 正常 |

Do not normalize API fields, account IDs, percentages, or parser tokens. Normalize only player-visible copy after parsing.

## Reset-Credit Removal

Keep the read-only inventory GET request because it supplies the required full-mode information. Remove all consuming behavior:

- Remove the consume POST path from `CodexUsageFetcher`.
- Remove `ConsumeResetCreditResult`, the consume method declaration/definition, the HTTP POST helper, and its endpoint string.
- Remove the AppBar reset-credit consumed message, timer, in-flight flag, confirmation step, generated redeem request ID, action message, button rectangle, click branch, confirmation dialog, and result handler.
- Remove the reset action button from full and simple layouts.
- Keep the reset inventory count, expiry calculations, and read-only error display.
- Verify with source search that no consume endpoint or action symbol remains.

## Refresh Control

The full-mode text button becomes a borderless circular-arrow glyph:

- Keep an invisible, accessible click target sized for pointer use.
- Draw only the loop arrow with Direct2D arcs and a triangular arrowhead.
- Add a subtle hover glow, but no button rectangle, border, or filled square.
- While a refresh is in flight, animate the arrow with a low-amplitude rotation or pulse and disable duplicate requests.
- Provide a tooltip: `重新整理額度` in Traditional Chinese and `Refresh usage` in English.
- Keep the right-click `Refresh now` command.

## Transparency Settings

Add a `設定…` context-menu command that opens a small transient glass settings panel. This is not a second persistent usage widget and does not change the single-widget architecture.

- Add a horizontal slider labeled `玻璃透明度`.
- Use a 20%–80% range, with a 42% default.
- Show the current percentage and the hint `數值越高，玻璃越透明`.
- Apply changes live to the bubble, full, simple, and taskbar glass surfaces.
- Keep text, status markers, and pointer hit regions readable and functional at all supported values.
- Persist the value as `glass_transparency_percent` in the existing settings file.
- Bump the layout/settings version so older settings receive the default safely.

## Persistence and Compatibility

- Preserve existing position, size, language, mode, refresh interval, startup, topmost, and lock-position settings.
- Add only the transparency value and any presentation-independent state needed to preserve the expanded geometry.
- Do not persist the bubble/pinned state.
- Keep the existing settings location and reset-position command.
- When a saved rectangle is too small for the new full-mode content, clamp it to the new minimum height and preserve its usable position.

## Error Handling

- If usage loading fails, full mode shows the localized load failure and detail text; simple and taskbar modes show a compact failure status without overflowing their surfaces.
- If reset-credit inventory loading fails, usage content remains usable and the read-only section displays an unavailable state.
- If layered rendering resources cannot be created, discard/recreate device resources using the existing Direct2D recovery pattern and avoid leaking the worker result or window surface.
- Avoid starting a second refresh while one is already in flight.
- Transparent areas must not block underlying windows or create an accidental full-rectangle click target.

## Testing and Acceptance

Extract state transitions, anchor geometry, transparency clamping, and full-mode summary derivation into testable helpers where practical. Add a lightweight C++ test target or equivalent repository-native test harness without introducing a third-party framework solely for this change.

Test-first behaviors:

1. Bubble/hover/pinned transitions, including immediate unpinned collapse on mouse leave.
2. Repeated bubble and `×` clicks collapse a pinned panel.
3. Expanded and bubble rectangles share the same saved anchor.
4. Transparency values clamp to the supported range and persist/load correctly.
5. Full-mode summary includes the exact reset-inventory title and all requested derived metrics.
6. Reset-credit consumption symbols, endpoint, POST helper, action button, and confirmation path are absent.
7. Traditional Chinese copy uses the specified Taiwan terms and does not modify numeric/API data.

Build and smoke verification:

- Configure and build the x64 Release target with the repository's CMake workflow.
- Run the unit/state tests and inspect the complete output.
- Search `src` and `README-zh.md` for Simplified Chinese remnants and for reset-credit consumption strings.
- Launch the executable and verify that startup shows only the icon; hover expands; moving away collapses when unpinned; clicking pins; moving away preserves pinned content; bubble and `×` collapse; dragging and lock position work; the three modes render; the refresh glyph works; settings change transparency live; and the value persists after restart.
- Verify the full panel has no reset-credit action button and retains only read-only reset inventory information.

