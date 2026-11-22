# meshcoreWatchUltra — 2× GPS / MeshCore / BLE status fonts

**Local-only update.** Requires the previous `GPS + MeshCore Watch Face Update` and the already-enlarged clock and battery page. Does not touch GitHub, `third_party/`, hardware drivers, main loop, MeshCore behavior, RTC, battery, or DisplayPower.

## Changes

| Label | Previous | New |
| --- | --- | --- |
| GPS fix/search/waiting | Montserrat 20 px | Montserrat **40 px** |
| MeshCore RX count | Montserrat 20 px | Montserrat **40 px** |
| BLE companion status | Montserrat 20 px | Montserrat **40 px** |

To fit three larger rows under the visually 96 px clock on the 410×502 display, the labels are shortened:

- `GPS FIX 7 SAT`, `GPS SEARCH`, `GPS WAIT`, or `GPS OFF`. GPS is green for a fresh fix, amber for waiting/search, and gray when off. Satellite count remains visible for a fix.
- `MC RX 42` (MeshCore packet receive counter), `MC RX 1.2K` for counts over 999, `MC RX 1M` for millions, or `MC OFF`.
- `BLE LINK` for an authenticated companion connection, or `BLE DOWN` for disconnected.

The rows are spaced roughly 49 px apart to prevent collision. No changes to the clock/battery fonts (96 px visually / 28 px), periodic updates, or screen sleep and touch wake. The default Kconfig option selects native LVGL Montserrat 40 px to avoid doubling a rasterized layer; if the font is deliberately disabled by a custom LVGL config, the code falls back to LVGL 2× scaling.

## Install

Extract the ZIP, then from your local project directory run:

```bash
cd /home/zebbel/meshcore/meshcoreWatchUltra
python3 /path/to/meshcoreWatchUltra-status-font-2x-local/install_local.py --check .
python3 /path/to/meshcoreWatchUltra-status-font-2x-local/install_local.py .

idf.py reconfigure
idf.py build flash monitor
```

The script previews its edits with `--check`, validates both files before writing, saves backups under `.local-backups/status-font-2x-*`, supports repeat runs, and rejects unexpected source versions rather than overwriting custom code.

## Verification

- Installer dry run / application / idempotence / unexpected-source refusal: **Passed** in a reconstructed fixture assembled from the immediately preceding packages.
- C++17 syntax check of the resulting ClockPage.cpp against LVGL/hardware API stubs: **Passed**.
- Real ESP-IDF build, on-watch text geometry, DMA impact, and touch-wake regression: **Not run here; verify on your device**.

No commit, push, or publication to GitHub.
