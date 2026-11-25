#!/usr/bin/env python3
"""EEZ-only AMOLED brightness policy.
Supports original b1ece07, LVGL touch integration and brightness V1. No generated edits.
Run in repository root: --check / --diff / apply / --revert.
"""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

BASE = 'ui/WatchDisplay/'
CLOCK = BASE + 'pages/ClockPage.cpp'
HEADER = BASE + 'pages/ClockPage.h'
DISPLAY = BASE + 'WatchDisplay.cpp'
PANEL = 'hardware/CO5300/CO5300Panel.cpp'
PUBLIC = BASE + 'include/WatchDisplay.h'
FILES = (CLOCK, HEADER, DISPLAY, PANEL, PUBLIC)

def replace(s, old, new):
    if s.count(old) != 1:
        raise ValueError(f'Expected exactly one unchanged anchor: {old[:100]!r}')
    return s.replace(old, new, 1)

def enable_v1(src):
    out = dict(src)
    s = src[CLOCK]
    s = replace(s, '''// EEZ vars.h uses C linkage. Native hardware variables are read-only:
// the generated table requires setters even when the UI only reads them.''', '''// EEZ vars.h uses C linkage. Sensor variables are read-only. Brightness
// is writable through the existing display owner, with the LVGL lock held.''')
    s = replace(s, 'void set_var_brightness(int32_t) {}', '''void set_var_brightness(int32_t value) {
    // Do not narrow an unchecked signed value, or turn the UI black.
    if (value < 1 || value > 100) return;
    if (value == s_brightness.load()) return;
    const esp_err_t err = ui::watch_display::set_ui_brightness_locked(
        static_cast<uint8_t>(value));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "EEZ brightness change failed: %s", esp_err_to_name(err));
    }
}''')
    out[CLOCK] = s
    out[HEADER] = replace(src[HEADER], 'void publish_ui_brightness(uint8_t percent);', '''void publish_ui_brightness(uint8_t percent);
// Native EEZ setter runs during ui_init/ui_tick with the adapter lock held.
// Adjust only the active power mode. No nested lock or recursive Flow tick.
esp_err_t set_ui_brightness_locked(uint8_t percentage);''')
    s = src[DISPLAY]
    s = replace(s, '    constexpr uint8_t kChargingBrightness = 10;', '    uint8_t s_charging_brightness = 10; // EEZ may adjust the active mode')
    if s.count('kChargingBrightness') != 3:
        raise ValueError('Unexpected charging brightness references; refusing ambiguous edit')
    s = s.replace('kChargingBrightness', 's_charging_brightness')
    s = replace(s, 'esp_err_t set_brightness(uint8_t percentage) {', '''esp_err_t set_ui_brightness_locked(uint8_t percentage) {
    if (percentage < 1 || percentage > 100) return ESP_ERR_INVALID_ARG;
    // ui_init() may invoke a setter before s_ready becomes true; the panel
    // and adapter have already been initialized in that case.
    if (!s_adapter_ready) return ESP_ERR_INVALID_STATE;
    uint8_t &selected = s_charging_mode ? s_charging_brightness : s_normal_brightness;
    if (selected == percentage) return ESP_OK;
    const esp_err_t err = hardware::co5300::set_brightness(percentage);
    if (err == ESP_OK) {
        selected = percentage;
        publish_ui_brightness(percentage);
    }
    return err;
}

esp_err_t set_brightness(uint8_t percentage) {''')
    out[DISPLAY] = s
    return out

def transform(src):
    # Normalize both supported baseline variants through the tested V1 anchors.
    if 'void set_var_brightness(int32_t) {}' in src[CLOCK]:
        out = enable_v1(src)
    else:
        out = dict(src)
        if 'void set_var_brightness(int32_t value)' not in out[CLOCK]:
            raise ValueError('Unknown brightness setter; expected V1 or original baseline')
    s = out[CLOCK]
    s = replace(s, 'std::atomic<int32_t> s_brightness{25};',
                'std::atomic<int32_t> s_brightness{0}; // blank until EEZ requests brightness')
    s = replace(s, '    ESP_LOGI(TAG, "EEZ Flow ready (50 ms tick; native hardware variables)");',
                '    if (s_brightness.load() == 0) ESP_LOGW(TAG, "EEZ has not set brightness yet; panel remains blank until a valid request");\n'
                '    ESP_LOGI(TAG, "EEZ Flow ready (50 ms tick; native hardware variables)");')
    out[CLOCK] = s
    out[HEADER] = replace(out[HEADER], '// Adjust only the active power mode. No nested lock or recursive Flow tick.',
                          '// EEZ owns brightness policy. No nested lock or recursive Flow tick.')
    s = out[DISPLAY]
    s = replace(s, '''    uint8_t s_charging_brightness = 10; // EEZ may adjust the active mode
    uint8_t s_normal_brightness = 25;
    bool s_charging_mode = false; // protected by the LVGL lock
''', '')
    s = replace(s, '''        if (s_charging_mode != charging) {
            const esp_err_t err = hardware::co5300::set_brightness(
                charging ? s_charging_brightness : s_normal_brightness);
            if (err != ESP_OK) return err;
            s_charging_mode = charging;
        }
        publish_ui_brightness(charging ? s_charging_brightness : s_normal_brightness);
''', '''        // Publish the charging state only. EEZ Flow owns brightness and navigation.
''')
    s = replace(s, '''    uint8_t &selected = s_charging_mode ? s_charging_brightness : s_normal_brightness;
    if (selected == percentage) return ESP_OK;
''', '')
    s = replace(s, '''        selected = percentage;
        publish_ui_brightness(percentage);''', '''        publish_ui_brightness(percentage);''')
    s = replace(s, '''esp_err_t set_brightness(uint8_t percentage) {
    if (percentage > 100) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_lv_adapter_lock(1000);
    if (err != ESP_OK) return err;
    if (!s_charging_mode) err = hardware::co5300::set_brightness(percentage);
    if (err == ESP_OK) {
        s_normal_brightness = percentage;
        publish_ui_brightness(s_charging_mode ? s_charging_brightness : s_normal_brightness);
    }
    esp_lv_adapter_unlock();
    return err;
}
''', '')
    for forbidden in ('s_normal_brightness', 's_charging_brightness', 's_charging_mode', 'kChargingBrightness'):
        if forbidden in s: raise ValueError(f'Unexpected remaining brightness policy: {forbidden}')
    if s.count('hardware::co5300::set_brightness(') != 1:
        raise ValueError('Expected exactly one EEZ hardware brightness path')
    out[DISPLAY] = s
    out[PUBLIC] = replace(src[PUBLIC], '    esp_err_t set_brightness(uint8_t percentage);\n', '')
    out[PANEL] = replace(src[PANEL], 'constexpr uint8_t kInitialBrightness = 25;',
                         'constexpr uint8_t kInitialBrightness = 0; // blank until EEZ sets brightness')
    return out

def sha(b): return hashlib.sha256(b).hexdigest()

def atomic(p, data):
    p.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.eez-brightness-', dir=p.parent)
    try:
        with os.fdopen(fd, 'wb') as f: f.write(data)
        os.chmod(name, p.stat().st_mode & 0o777 if p.exists() else 0o644)
        os.replace(name, p)
    finally:
        if os.path.exists(name): os.unlink(name)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    g = p.add_mutually_exclusive_group()
    g.add_argument('--check', action='store_true')
    g.add_argument('--diff', action='store_true')
    g.add_argument('--revert', action='store_true')
    p.add_argument('--root', default='.')
    a = p.parse_args()
    root = Path(a.root).resolve()
    gitdir = Path(subprocess.check_output(['git', '-C', str(root), 'rev-parse', '--absolute-git-dir'], text=True).strip())
    backup = gitdir / 'mcw_eez_brightness_only_backup'
    manifest = backup / 'manifest.json'
    if backup.exists():
        if not manifest.is_file(): raise ValueError('Incomplete backup; inspect it before proceeding')
        meta = json.loads(manifest.read_text())
        if set(meta) != set(FILES): raise ValueError('Unexpected backup manifest')
        old, current = {}, {}
        for n in FILES:
            old[n] = (backup/n).read_bytes()
            current[n] = (root/n).read_bytes()
            if sha(old[n]) != meta[n]['old'] or sha(current[n]) != meta[n]['new']:
                raise ValueError(f'{n}: backup or patched file changed; refusing overwrite')
        if not a.revert:
            print('Already applied; files match backup manifest.'); return
        done = []
        try:
            for n in FILES:
                atomic(root/n, old[n]); done.append(n)
        except Exception:
            for n in done: atomic(root/n, current[n])
            raise
        shutil.rmtree(backup)
        print('Original sources restored. Generated EEZ files unchanged.'); return
    if a.revert: raise ValueError('No brightness patch backup found')
    raw = {n:(root/n).read_bytes() for n in FILES}
    src = {n:b.decode('utf-8').replace('\r\n', '\n') for n,b in raw.items()}
    out = transform(src)
    new = {n:(s.replace('\n','\r\n') if b'\r\n' in raw[n] else s).encode('utf-8') for n,s in out.items()}
    if a.diff:
        for n in FILES:
            print(''.join(difflib.unified_diff(src[n].splitlines(True),out[n].splitlines(True),fromfile='a/'+n,tofile='b/'+n)),end='')
    print('All EEZ-only brightness source anchors verified; five source files ready.')
    vh = root/BASE/'generated/vars.h'
    if not vh.is_file() or 'get_var_brightness' not in vh.read_text() or 'set_var_brightness' not in vh.read_text():
        raise ValueError('Add native Integer brightness and an explicit startup SetVariable action in EEZ, then export before applying this patch. The display starts blank.')
    print('WARNING: EEZ must explicitly set brightness at startup. There is no firmware brightness fallback.')
    if a.check or a.diff: return
    backup.mkdir(parents=True, exist_ok=False)
    try:
        for n in FILES: atomic(backup/n, raw[n])
        atomic(manifest, json.dumps({n:{'old':sha(raw[n]),'new':sha(new[n])} for n in FILES}, indent=2).encode())
    except Exception:
        shutil.rmtree(backup); raise
    done = []
    try:
        for n in FILES:
            atomic(root/n, new[n]); done.append(n)
    except Exception:
        for n in done: atomic(root/n, raw[n])
        shutil.rmtree(backup); raise
    print('Applied EEZ-only brightness. Run idf.py build flash monitor.')

if __name__ == '__main__':
    try: main()
    except (ValueError, OSError, subprocess.CalledProcessError) as e:
        print('ERROR:', e, file=sys.stderr); sys.exit(1)
