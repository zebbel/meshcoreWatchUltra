#!/usr/bin/env python3
"""Remove obsolete light-sleep diagnostics; preserve sleep policy and wake handling."""
from pathlib import Path
import re
import subprocess
import sys
import difflib


def main():
    root = Path(subprocess.check_output(['git', 'rev-parse', '--show-toplevel'], text=True).strip())
    changes = {}
    def read(path):
        raw = (root / path).read_bytes()
        return raw, raw.decode('utf-8').replace('\r\n', '\n')
    def edit(path, transform):
        raw, old = read(path)
        new = transform(old)
        if new != old:
            changes[path] = (raw, new.replace('\n', '\r\n' if b'\r\n' in raw else '\n').encode('utf-8'))
    def sub(s, pattern, replacement='', count=1):
        result, n = re.subn(pattern, replacement, s, flags=re.M)
        if n != count:
            raise ValueError(f'Expected {count} match(es), found {n}: {pattern}')
        return result
    def line(s, pattern):
        return sub(s, r'^[ \t]*' + pattern + r'[^\n]*\n')
    def pm(s):
        s = line(s, r'#include "esp_timer\.h"')
        s = sub(s, r'^static std::atomic<uint32_t> s_light_sleep_exits\{0\};\n// Cumulative statistics[^\n]*\n// 32-bit milliseconds[^\n]*\n(?:static std::atomic<uint32_t> s_sleep_\w+\{0\};\n){4}')
        s = sub(s, r'^static esp_err_t IRAM_ATTR on_light_sleep_exit\(int64_t slept_us, void \*\) \{\n[\s\S]*?^\}', 'static esp_err_t IRAM_ATTR on_light_sleep_exit(int64_t, void *) {\n    s_restore_wake_pins.store(true, std::memory_order_release);\n    return ESP_OK;\n}')
        s = line(s, r'// The callback argument is the \*actual\* sleep duration')
        s = sub(s, r'^[ \t]*s_sleep_stats_start_ms\.store\([^\n]*\n[ \t]*std::memory_order_relaxed\);\n')
        s = sub(s, r'^void report_light_sleep_exits\(bool screen_awake, bool gps_powered, bool ble_active\) \{\n[\s\S]*?^\}\n')
        s = line(s, r'void report_light_sleep_exits\(bool, bool, bool\)')
        s = sub(s, r'^SleepStats sleep_stats\(\) \{\n[\s\S]*?^\}\n\n')
        return s
    def pm_header(s):
        s = line(s, r'#include <stdint\.h>')
        s = sub(s, r'^struct SleepStats \{\n[\s\S]*?^\};\n')
        s = line(s, r'void report_light_sleep_exits\(')
        return line(s, r'SleepStats sleep_stats\(')
    def status_header(s):
        s = line(s, r'// on-device diagnostics while USB is absent\.')
        for field in ('bool pm_enabled', 'uint8_t sleep_percent', 'uint32_t sleep_entries'):
            s = line(s, field)
        return s
    def status_source(s):
        s = line(s, r'#include "PowerManagement\.h"')
        s = line(s, r'const auto sleep = services::power_management::sleep_stats\(\);')
        for field in ('pm_enabled', 'sleep_percent', 'sleep_entries'):
            s = line(s, r'snapshot\.' + field + r'\s*=')
        return s
    def bridge(s):
        for name in ('get_var_sleep_percent', 'set_var_sleep_percent'):
            if re.search(r'\b' + name + r'\b', s):
                s = sub(s, r'^[ \t]*(?:int32_t|void)\s+' + name + r'\([^\n]*\)\s*\{[^}]*\}[ \t]*\n')
        return s
    # Do not edit designer or generated artifacts. Require current EEZ export.
    display = root / 'watch/WatchDisplay'
    for p in [display / 'designer/WatchUI.eez-project', *sorted((display / 'generated').rglob('*'))]:
        if p.is_file() and (p.suffix in ('.h', '.c', '.cpp', '.eez-project')):
            if b'sleep_percent' in p.read_bytes().lower():
                raise ValueError(f'EEZ still references sleep_percent in {p.relative_to(root)}. Remove the variable in EEZ Studio and regenerate first.')
    edit('services/PowerManagement/PowerManagement.cpp', pm)
    edit('services/PowerManagement/include/PowerManagement.h', pm_header)
    edit('services/PowerManagement/CMakeLists.txt', lambda s: sub(s, r'\besp_timer[ \t]+'))
    edit('main/main.cpp', lambda s: line(s, r'services::power_management::report_light_sleep_exits\('))
    edit('watch/WatchStatus/include/WatchStatus.h', status_header)
    edit('watch/WatchStatus/WatchStatus.cpp', status_source)
    edit('watch/WatchStatus/CMakeLists.txt', lambda s: sub(s, r'\bPowerManagement[ \t]+'))
    edit('watch/WatchDisplay/FlowBridge.cpp', bridge)
    forbidden = re.compile(rb'\b(?:SleepStats|sleep_stats|report_light_sleep_exits|s_light_sleep_exits|s_sleep_total_ms|s_sleep_entries|s_sleep_longest_ms|s_sleep_stats_start_ms|sleep_percent|pm_enabled|sleep_entries)\b')
    paths = subprocess.check_output(['git','-C',str(root),'ls-files','-z']).decode().split('\0')
    for path in paths:
        if not path.startswith(('hardware/', 'services/', 'meshcore/', 'watch/', 'main/')) or '/generated/' in path or Path(path).suffix not in ('.h','.hpp','.c','.cpp','.cc'):
            continue
        data = changes[path][1] if path in changes else (root/path).read_bytes()
        if forbidden.search(data):
            raise ValueError(f'Unexpected remaining statistics reference: {path}')
    for path,(old,new) in changes.items():
        if (root/path).read_bytes() != old:
            raise ValueError(f'File changed during preflight: {path}')
    # Preserve pre-existing local changes and original line endings.
    written = []
    try:
        for path,(old,new) in changes.items():
            written.append(path)
            (root/path).write_bytes(new)
    except OSError:
        for path in reversed(written):
            (root/path).write_bytes(changes[path][0])
        raise
    for path in changes:
        print('Updated:', path)
    print(f'Applied cleanup to {len(changes)} files. No Git commits or pushes. Run idf.py build.')

if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, UnicodeError, subprocess.CalledProcessError) as e:
        sys.exit(f'Cleanup aborted: {e}')
