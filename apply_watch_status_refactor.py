#!/usr/bin/env python3
"""Local WatchStatus refactor for meshcoreWatchUltra (baseline 5729ae0).
Run from repository root: python apply_watch_status_refactor.py [--check|--diff|--revert]
No commits, pushes or builds. Keeps original files in a local backup.
"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path

BACKUP = '.watchstatus-refactor-backup'
MAIN = 'main/main.cpp'
HEADER = 'watch/WatchStatus/include/WatchStatus.h'
SOURCE = 'watch/WatchStatus/WatchStatus.cpp'
CMAKE = 'watch/WatchStatus/CMakeLists.txt'
UI_HEADER = 'watch/WatchDisplay/include/WatchDisplay.h'
FLOW = 'watch/WatchDisplay/FlowBridge.cpp'
UI_CMAKE = 'watch/WatchDisplay/CMakeLists.txt'


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Expected exactly one occurrence of {old[:100]!r}')
    return text.replace(old, new, 1)


def plan(root):
    originals = {}
    changes = {}
    def read(path):
        data = (root / path).read_bytes()
        originals[path] = data
        return data.decode('utf-8').replace('\r\n', '\n')
    def save(path, text):
        old = originals.get(path)
        if old is not None and b'\r\n' in old:
            text = text.replace('\n', '\r\n')
        changes[path] = text.encode('utf-8')
    if (root / SOURCE).exists():
        raise ValueError(f'{SOURCE} already exists; refusing to overwrite it')
    main = read(MAIN)
    start = '// GPS and MeshCore are polled by app_main. Copy their state here rather\n// than reading non-atomic firmware structures from the LVGL task.\nstatic void publish_watchface_status(bool meshcore_enabled) {'
    end = '\n}\n\nextern "C" void app_main()'
    if main.count(start) != 1 or main.count(end) != 1:
        raise ValueError('Main status function does not match expected baseline')
    a = main.index(start)
    b = main.index(end, a) + 2
    old = main[a:b]
    body = old[old.index('static void'):]
    body = replace_once(body, 'static void publish_watchface_status', 'void update_status')
    body = replace_once(body, '    ui::watch_display::publish_watch_status(snapshot);', '    portENTER_CRITICAL(&s_status_mux);\n    s_status = snapshot;\n    portEXIT_CRITICAL(&s_status_mux);')
    save(MAIN, replace_once(replace_once(main, old + '\n\n', ''), '        publish_watchface_status(meshcore_enabled);', '        watch::update_status(meshcore_enabled);'))
    save(SOURCE, '''#include "WatchStatus.h"
#include "HardwareSystem.h"
#include "GPS.h"
#include "SDStorage.h"
#include "MeshCorePort.h"
#include "BluetoothTransport.h"
#include "PowerManagement.h"
#include "freertos/FreeRTOS.h"

namespace watch {
namespace {
WatchStatus s_status{};
portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
}

// Collect on app_main only. Never call drivers or services inside the lock.
''' + body + '''

WatchStatus get_status() {
    portENTER_CRITICAL(&s_status_mux);
    const WatchStatus snapshot = s_status;
    portEXIT_CRITICAL(&s_status_mux);
    return snapshot;
}
} // namespace watch
''')
    h = read(HEADER)
    h = replace_once(h, '// A tiny thread-safe view of application state for the watch face.\n// app_main publishes measurements; LVGL consumes them without calling GPS,\n// MeshCore, or BLE code from the rendering task.', '// Watch status is collected by app_main and read as a thread-safe snapshot.')
    h = replace_once(h, '} // namespace watch', '// Call only from the main application task. Samples once per second.\nvoid update_status(bool meshcore_enabled);\n// RAM-only copy; safe to call from the LVGL task.\nWatchStatus get_status();\n\n} // namespace watch')
    save(HEADER, h)
    c = read(CMAKE)
    save(CMAKE, replace_once(c, 'idf_component_register(INCLUDE_DIRS "include")', '''idf_component_register(
    SRCS "WatchStatus.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES System MIA_M10Q SD MeshCorePort Bluetooth PowerManagement freertos
)'''))
    h = read(UI_HEADER)
    h = replace_once(h, '#include "WatchStatus.h"\n', '')
    h = replace_once(h, '    void publish_watch_status(const watch::WatchStatus &status);\n', '')
    save(UI_HEADER, h)
    f = read(FLOW)
    f = replace_once(f, '#include "WatchDisplay.h"', '#include "WatchDisplay.h"\n#include "WatchStatus.h"')
    f = replace_once(f, 'std::atomic<uint32_t> s_status{0};\nstd::atomic<uint32_t> s_rx{0};\n', '')
    f = replace_once(f, 'constexpr uint32_t GPS_ENABLED=1U, GPS_FIX=2U, BLE=4U, MESH=8U;\n', '')
    for old, new in [
        ('(s_status.load() & GPS_ENABLED) != 0', 'watch::get_status().gps_enabled'),
        ('(s_status.load() & GPS_FIX) != 0', 'watch::get_status().gps_has_fix'),
        ('(s_status.load() >> 8) & 255U', 'watch::get_status().gps_satellites'),
        ('(s_status.load() & BLE) != 0', 'watch::get_status().ble_connected'),
        ('(s_status.load() & MESH) != 0', 'watch::get_status().meshcore_active'),
        ('s_rx.load()', 'watch::get_status().meshcore_rx_packets'),
        ('(s_status.load() >> 16) & 255U', 'watch::get_status().sleep_percent'),
    ]:
        f = replace_once(f, old, new)
    start = 'void publish_watch_status(const watch::WatchStatus &status) {'
    end = '\n}\n\nvoid update_bluetooth_pairing()'
    if f.count(start) != 1 or f.count(end) != 1:
        raise ValueError('FlowBridge status publisher does not match expected baseline')
    a = f.index(start)
    b = f.index(end, a) + 4
    f = f[:a] + f[b:]
    save(FLOW, f)
    c = read(UI_CMAKE)
    c = replace_once(c, 'REQUIRES esp_common WatchStatus\n    PRIV_REQUIRES CO5300', 'REQUIRES esp_common\n    PRIV_REQUIRES WatchStatus CO5300')
    save(UI_CMAKE, c)
    return originals, changes


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    m = p.add_mutually_exclusive_group()
    m.add_argument('--check', action='store_true')
    m.add_argument('--diff', action='store_true')
    m.add_argument('--revert', action='store_true')
    p.add_argument('--root', type=Path, default=Path.cwd())
    args = p.parse_args()
    root = args.root.resolve()
    backup = root / BACKUP
    if not (root / 'main/main.cpp').is_file():
        p.error('Run from repository root or specify --root')
    try:
        if args.revert:
            manifest = json.loads((backup / 'manifest.json').read_text())
            for path, entry in manifest.items():
                if digest((root / path).read_bytes()) != entry['after']:
                    raise ValueError(f'{path} changed after applying; refusing to overwrite')
                if entry['before'] is not None and digest((backup / path).read_bytes()) != entry['before']:
                    raise ValueError(f'Backup for {path} is damaged')
            for path, entry in manifest.items():
                if entry['before'] is None:
                    (root / path).unlink()
                else:
                    (root / path).write_bytes((backup / path).read_bytes())
            import shutil
            shutil.rmtree(backup)
            print('Reverted all refactor changes.')
            return
        if backup.exists():
            raise ValueError(f'{BACKUP} exists; revert or remove after reviewing your changes')
        originals, changes = plan(root)
        if args.diff:
            for path, after in changes.items():
                before = originals.get(path, b'')
                print(''.join(difflib.unified_diff(before.decode().splitlines(True), after.decode().splitlines(True), fromfile='a/'+path if path in originals else '/dev/null', tofile='b/'+path)), end='')
            return
        if args.check:
            print(f'Preflight passed: {len(changes)} files (including one new source file).')
            return
        backup.mkdir()
        manifest = {}
        for path, after in changes.items():
            before = originals.get(path)
            if before is not None:
                dest = backup / path
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(before)
            manifest[path] = {'before': digest(before) if before is not None else None, 'after': digest(after)}
        (backup / 'manifest.json').write_text(json.dumps(manifest, indent=2))
        # Validate all originals immediately before editing.
        for path, before in originals.items():
            if (root / path).read_bytes() != before:
                raise ValueError(f'{path} changed during preflight')
        for path, after in changes.items():
            (root / path).write_bytes(after)
        print(f'Applied {len(changes)} file changes. Backup: {backup}')
        print('Run idf.py reconfigure then idf.py build. No Git operations performed.')
    except (OSError, ValueError, UnicodeError) as exc:
        p.exit(1, f'Error: {exc}\n')


if __name__ == '__main__':
    main()
