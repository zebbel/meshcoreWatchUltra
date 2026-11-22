#!/usr/bin/env python3
"""Double GPS, MeshCore RX, and BLE watch-face status fonts (20 -> 40 px).

Local-only safe migration. Validates both files before changing anything, makes
backups, and never runs git. Existing clock/battery typography is untouched.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
from difflib import unified_diff
from pathlib import Path
import shutil
import sys


def replace_once(source: str, old: str, new: str, name: str) -> str:
    if old == new:
        return source
    if new in source and old not in source:
        return source  # Idempotent.
    if source.count(old) != 1:
        raise ValueError(f'{name}: expected one known source block, found {source.count(old)}; refusing to modify')
    return source.replace(old, new, 1)


def proposed(repo: Path) -> dict[Path, str]:
    if not (repo / 'CMakeLists.txt').is_file():
        raise ValueError('Expected a meshcoreWatchUltra checkout; CMakeLists.txt missing')
    clock = repo / 'ui/WatchDisplay/ClockPage.cpp'
    cfg = repo / 'ui/WatchDisplay/Kconfig'
    if not clock.is_file() or not cfg.is_file():
        raise ValueError('Missing ClockPage.cpp or Kconfig: install the GPS/MeshCore watch-face update first')

    page = clock.read_text(encoding='utf-8')
    if 'void update_status_labels()' not in page or 's_gps_label' not in page or 's_ble_label' not in page:
        raise ValueError('ClockPage.cpp is missing GPS / MeshCore / BLE status labels')
    for kind in ('gps', 'meshcore', 'ble'):
        page = replace_once(
            page,
            f'#if LV_FONT_MONTSERRAT_20\n    lv_obj_set_style_text_font(s_{kind}_label, &lv_font_montserrat_20, 0);\n#endif',
            f'#if LV_FONT_MONTSERRAT_40\n    lv_obj_set_style_text_font(s_{kind}_label, &lv_font_montserrat_40, 0);\n#else\n    // If a custom LVGL config disables the 40px font, scale the existing font.\n    lv_obj_set_style_transform_pivot_x(s_{kind}_label, LV_PCT(50), 0);\n    lv_obj_set_style_transform_pivot_y(s_{kind}_label, LV_PCT(50), 0);\n    lv_obj_set_style_transform_scale_x(s_{kind}_label, 512, 0);\n    lv_obj_set_style_transform_scale_y(s_{kind}_label, 512, 0);\n#endif',
            f'{kind} font size')

    # Compact RX formatting first, before altering the old MESHCORE OFF literal.
    page = replace_once(page,
        '''    if (state & kMeshActive)
        std::snprintf(text, sizeof(text), "MESHCORE  |  RX %lu", static_cast<unsigned long>(rx));
    else
        std::snprintf(text, sizeof(text), "MESHCORE OFF");''',
        '''    if (state & kMeshActive) {
        // Compact 40px text near the curved screen edge. Counts are abbreviated.
        if (rx >= 1000000U)
            std::snprintf(text, sizeof(text), "MC RX %luM", static_cast<unsigned long>(rx / 1000000U));
        else if (rx >= 1000U)
            std::snprintf(text, sizeof(text), "MC RX %lu.%luK",
                          static_cast<unsigned long>(rx / 1000U),
                          static_cast<unsigned long>((rx % 1000U) / 100U));
        else
            std::snprintf(text, sizeof(text), "MC RX %lu", static_cast<unsigned long>(rx));
    } else {
        std::snprintf(text, sizeof(text), "MC OFF");
    }''', 'MeshCore RX compact formatting')

    # Labels are intentionally shortened to prevent edge clipping on the watch.
    replacements = [
        ('"GPS FIX  |  %u SAT"', '"GPS FIX %u SAT"', 'GPS fix text'),
        ('"GPS SEARCHING"', '"GPS SEARCH"', 'GPS searching text'),
        ('"GPS WAITING"', '"GPS WAIT"', 'GPS waiting text'),
        ('"MESHCORE OFF"', '"MC OFF"', 'MeshCore off text'),
        ('"BLE CONNECTED"', '"BLE LINK"', 'BLE link text'),
        ('"BLE DISCONNECTED"', '"BLE DOWN"', 'BLE disconnected text'),
        ('lv_obj_align(s_gps_label, LV_ALIGN_CENTER, 0, 111);',
         'lv_obj_align(s_gps_label, LV_ALIGN_CENTER, 0, 96);', 'GPS status y-offset'),
        ('lv_obj_align(s_meshcore_label, LV_ALIGN_CENTER, 0, 155);',
         'lv_obj_align(s_meshcore_label, LV_ALIGN_CENTER, 0, 145);', 'MeshCore status y-offset'),
        ('lv_obj_align(s_ble_label, LV_ALIGN_CENTER, 0, 194);',
         'lv_obj_align(s_ble_label, LV_ALIGN_CENTER, 0, 194);', 'BLE status y-offset'),
    ]
    for old, new, name in replacements:
        if old == new:
            continue
        occurrences = page.count(old)
        if occurrences not in (1, 2) and not (occurrences == 0 and new in page):
            raise ValueError(f'{name}: unexpected occurrences ({occurrences}); refusing to modify')
        page = page.replace(old, new)

    kconf = cfg.read_text(encoding='utf-8')
    kconf = replace_once(kconf,
        '''    config MCW_WATCH_CLOCK_STATUS_FONT
        bool "Use 20px font for GPS / MeshCore status"
        default y
        select LV_FONT_MONTSERRAT_20
        help
            Use a readable 20px font for the status indicators below the clock.''',
        '''    config MCW_WATCH_CLOCK_STATUS_FONT
        bool "Use 40px font for GPS / MeshCore / BLE status"
        default y
        select LV_FONT_MONTSERRAT_40
        help
            Double status text from 20px to a native 40px font without
            scaling an intermediate rendering layer.''',
        '40px Kconfig option')

    return {clock: page, cfg: kconf}


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('repo', nargs='?', default='.', help='Path to local firmware checkout')
    p.add_argument('--check', action='store_true', help='Show proposed diff without writing')
    args = p.parse_args()
    repo = Path(args.repo).expanduser().resolve()
    try:
        values = proposed(repo)  # Full preflight before edits.
        changes = {p: s for p, s in values.items() if p.read_text(encoding='utf-8') != s}
        if not changes:
            print('40px watch statuses already installed; nothing changed.')
            return 0
        for path, new in changes.items():
            rel = path.relative_to(repo)
            print('UPDATE', rel)
            if args.check:
                print('\n'.join(unified_diff(
                    path.read_text(encoding='utf-8').splitlines(), new.splitlines(),
                    fromfile=f'a/{rel}', tofile=f'b/{rel}', lineterm='')))
        if args.check:
            print('Preflight passed. No files modified.')
            return 0
        backup = repo / '.local-backups' / (
            'status-font-2x-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'))
        for path in changes:
            target = backup / path.relative_to(repo)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
        for path, new in changes.items():
            path.write_text(new, encoding='utf-8')
        print('Saved backups to', backup)
        print('40px status labels installed locally. Run idf.py reconfigure build flash monitor.')
        return 0
    except (ValueError, OSError) as err:
        print('ERROR:', err, file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
