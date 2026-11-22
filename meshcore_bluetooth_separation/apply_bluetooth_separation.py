#!/usr/bin/env python3
"""Separate BLE lifecycle/pairing from MeshCore in one Phase-3 MeshCoreWatchUltra checkout.

Does not modify the upstream MeshCore library, hardware PIN security, GATT UUIDs,
BLE bond storage, or settings NVS format. All edits are backed up; --check is read-only.
No Git or flashing operations. Python >= 3.10.
"""
from __future__ import annotations

import argparse
import datetime as dt
import difflib
import pathlib
import re
import tarfile
import tempfile
import os
import sys

class Mismatch(RuntimeError):
    pass


def require(ok: bool, why: str) -> None:
    if not ok:
        raise Mismatch(why)


def sub_one(text: str, pattern: str, replacement: str, label: str, *, flags=0) -> str:
    out, count = re.subn(pattern, replacement, text, count=1, flags=flags)
    require(count == 1, f'{label}: expected exactly one matching expression: {pattern}')
    return out


def remove_exact(text: str, fragment: str, label: str) -> str:
    count = text.count(fragment)
    require(count == 1, f'{label}: expected one {fragment!r}, found {count}')
    return text.replace(fragment, '', 1)


def change_bluetooth_header(text: str) -> str:
    require('namespace hardware::bluetooth' in text and 'class Transport' in text, 'Expected Phase 3 Hardware Bluetooth header')
    require('take_clear_pairing_display' in text, 'BLE clear-event method already renamed or missing')
    text = text.replace('take_clear_pairing_display', 'take_pairing_clear_event')
    text = sub_one(text, r'(\n\s*class Transport\s*\{)', '\n    // One process-wide BLE transport. Main owns lifecycle; MeshCore only uses frames.\n    class Transport;\n    Transport &transport();\n\\1', 'BLE singleton declaration')
    return text


def change_bluetooth_cpp(text: str) -> str:
    require('namespace hardware::bluetooth' in text, 'Expected Phase 3 Hardware Bluetooth implementation')
    require('Transport::Transport()' in text, 'Transport constructor missing')
    text = text.replace('take_clear_pairing_display', 'take_pairing_clear_event')
    text = sub_one(text, r'(\n\s*Transport::Transport\(\)\s*\{\s*\})', '\n    // Owned by Hardware; the application starts/polls it, clients borrow it.\n    Transport &transport(){\n        static Transport singleton;\n        return singleton;\n    }\n\\1', 'BLE singleton definition')
    return text


def extract_uuids(meshcore_cpp: str) -> tuple[str, str]:
    pattern = r'(?m)^\s*// Original MeshCore NUS/GATT UUIDs[^\n]*\n\s*static const hardware::bluetooth::Uuids COMPANION_UUIDS\s*=\s*\{.*?^\s*\};\s*\n'
    match = re.search(pattern, meshcore_cpp, flags=re.M | re.S)
    require(match is not None, 'Missing expected Phase 2/3 COMPANION_UUIDS block. Cannot safely preserve GATT UUIDs.')
    uuids = match.group(0).strip('\n')
    for suffix in ('01', '02', '03'):
        require(f'0x{suffix}, 0x00, 0x40, 0x6E' in uuids, f'Missing MeshCore BLE UUID suffix {suffix}')
    return meshcore_cpp[:match.start()] + meshcore_cpp[match.end():], uuids


def change_meshcore_cpp(text: str) -> tuple[str, str]:
    require('namespace meshcore' in text and 'static bool initialized' in text, 'Expected Phase 3 MeshCore runtime')
    text, uuid_block = extract_uuids(text)
    text = sub_one(text, r'(?m)^\s*static hardware::bluetooth::Transport\s*\*bluetooth\s*=\s*nullptr;\s*\n', '', 'Remove BLE pointer from MeshCore')
    text = sub_one(text, r'(?m)^(\s*)static hardware::bluetooth::Transport ble\s*;', r'\1hardware::bluetooth::Transport &ble = hardware::bluetooth::transport();', 'Use Hardware-owned BLE transport')
    text = sub_one(text, r'(?m)^\s*ble\.set_pairing_pin\(settings\.get\(\)\.ble_pin\);\s*\n', '', 'Remove BLE pairing configuration from MeshCore startup')
    text = sub_one(text, r'(?m)^\s*err\s*=\s*ble\.begin\("MeshCore-Watch",\s*COMPANION_UUIDS\);\s*\n(?:\s*\n)?\s*if\(err\s*!=\s*ESP_OK\)\s*\{\s*\n\s*ESP_LOGE\(TAG,\s*"BLE start failed: %s",\s*esp_err_to_name\(err\)\);\s*\n\s*return err;\s*\n\s*\}\s*\n', '', 'Move BLE initialization to Main')
    text = sub_one(text, r'(?m)^\s*bluetooth\s*=\s*&ble;\s*\n', '', 'Remove MeshCore Bluetooth ownership')
    text = sub_one(text, r'(?m)^\s*PairingEvent take_pairing_event\(\)\s*\{.*?^\s*\}\s*\n', '\n', 'Move pairing events out of MeshCore', flags=re.M | re.S)
    # A legacy BLE PIN is still part of the persisted MeshCore Settings structure and
    # companion settings command. Main bridges it to Hardware; no NVS format change.
    getter = '''    // Compatibility only: persisted Settings (VERSION=1) still stores ble_pin.
    // Main passes this value into Bluetooth before BLE advertising starts.
    uint32_t legacy_ble_pin(){
        return settings_store == nullptr ? 0 : settings_store->get().ble_pin;
    }

'''
    marker = '    bool get_position('
    require(text.count(marker) == 1, 'Expected single MeshCore get_position definition')
    text = text.replace(marker, getter + marker, 1)
    text = text.replace('MeshCore node running; BLE companion and telemetry initialized', 'MeshCore node, companion protocol and telemetry initialized; Bluetooth managed by Main')
    require('ble.begin(' not in text and 'take_pairing_event(' not in text, 'MeshCore still owns BLE startup or pairing events')
    return text, uuid_block


def change_meshcore_h(text: str) -> str:
    require('namespace meshcore' in text, 'Expected meshcore namespace in public runtime header')
    text = sub_one(text, r'(?m)^\s*struct PairingEvent\s*\{.*?^\s*\};\s*\n', '', 'Remove MeshCore pairing event type', flags=re.M | re.S)
    text = sub_one(text, r'(?m)^\s*PairingEvent take_pairing_event\(\);\s*\n', '', 'Remove MeshCore pairing event API')
    require(text.count('void loop();') == 1, 'Unexpected MeshCore lifecycle API')
    text = text.replace('    void loop();', '    void loop();\n\n    // Read-only compatibility access for persisted BLE PIN; lifecycle belongs to Main.\n    uint32_t legacy_ble_pin();', 1)
    return text


def change_companion_cpp(text: str) -> str:
    require('namespace meshcore' in text, 'Expected Phase 3 Companion namespace')
    text = sub_one(text, r'(?m)^\s*ble_\.loop\(\);\s*\n', '', 'Move Bluetooth transport polling from Companion to Main')
    return sub_one(text, r'(?m)^\s*ble_\.set_pairing_pin\(next\.ble_pin\);\s*\n', '', 'Move BLE pairing PIN configuration from Companion to Main')


def change_main_cpp(text: str, uuid_block: str) -> str:
    require('meshcore::init()' in text and 'meshcore::loop()' in text, 'Expected Phase 3 Main')
    require('#include "DeviceUI.h"' in text, 'Expected DeviceUI include for Phase 3')
    text = text.replace('#include "DeviceUI.h"', '#include "BluetoothTransport.h"', 1)
    # UUIDs are GATT profile configuration: Main supplies them to the generic driver.
    require('static const char *TAG' in text and 'COMPANION_UUIDS' not in text, 'Main TAG/UUID location unexpected')
    text = sub_one(text, r'(?m)^(static const char \*TAG\s*=\s*[^;]+;)', r'\1\n\n' + uuid_block, 'Move GATT profile to Main')
    text = sub_one(text, r'(?m)^static void update_pairing_display\(\)\s*\{.*?^\}\s*\n', '''static void log_bluetooth_pairing(){
    auto &bluetooth = hardware::bluetooth::transport();
    const uint32_t pin = bluetooth.take_pairing_pin();
    const bool clear = bluetooth.take_pairing_clear_event();
    if(pin != 0) ESP_LOGI(TAG, "Bluetooth pairing PIN: %06lu", static_cast<unsigned long>(pin));
    if(clear) ESP_LOGI(TAG, "Bluetooth pairing PIN cleared");
}
''', 'Replace display pairing with Bluetooth log', flags=re.M | re.S)
    text = sub_one(text, r'(?m)^\s*err\s*=\s*app::ui::init\(\);\s*\n\s*if\(err\s*!=\s*ESP_OK\)\s*\{\s*\n\s*ESP_LOGE\(TAG,\s*"Cannot display secure BLE pairing code: %s",\s*esp_err_to_name\(err\)\);\s*\n\s*return;\s*\n\s*\}\s*\n', '', 'Remove pairing-only display initialization')
    new_ble_init = '''    // Hardware owns the BLE stack and its pairing state. MeshCore uses its frames.
    // Preserve the legacy fixed-PIN setting until Settings storage is migrated.
    auto &bluetooth = hardware::bluetooth::transport();
    uint32_t applied_ble_pin = meshcore::legacy_ble_pin();
    bluetooth.set_pairing_pin(applied_ble_pin);
    err = bluetooth.begin("MeshCore-Watch", COMPANION_UUIDS);
    if(err != ESP_OK){
        ESP_LOGE(TAG, "Bluetooth initialization failed: %s", esp_err_to_name(err));
        return;
    }

'''
    text = sub_one(text, r'(?m)^(\s*err\s*=\s*meshcore::init\(\);\s*\n\s*if\(err\s*!=\s*ESP_OK\)\s*\{\s*\n\s*ESP_LOGE\(TAG,\s*"MeshCore initialization failed: %s",\s*esp_err_to_name\(err\)\);\s*\n\s*return;\s*\n\s*\}\s*\n)', r'\1\n' + new_ble_init, 'Initialize BLE in Main')
    text = sub_one(text, r'\bupdate_pairing_display\(\);', 'log_bluetooth_pairing();', 'Update Main loop pairing handling')
    text = sub_one(text, r'\bmeshcore::loop\(\);', '''meshcore::loop();
        // Android may update the legacy PIN in Settings. Main applies it to Hardware.
        const uint32_t requested_ble_pin = meshcore::legacy_ble_pin();
        if(requested_ble_pin != applied_ble_pin){
            bluetooth.set_pairing_pin(requested_ble_pin);
            applied_ble_pin = requested_ble_pin;
            ESP_LOGI(TAG, "Bluetooth pairing configuration updated");
        }
        bluetooth.loop();''', 'Main owns BLE polling and legacy config updates')
    require('app::ui::' not in text and 'take_pairing_event(' not in text, 'Main still routes pairing through MeshCore/UI')
    return text


def change_main_cmake(text: str) -> str:
    require('REQUIRES' in text, 'Expected ESP-IDF main CMake REQUIRES')
    if re.search(r'(?m)^\s*Bluetooth\s*$', text):
        return text
    return sub_one(text, r'(\bREQUIRES\s*\n)', r'\1        Bluetooth\n', 'Add direct Main dependency on Hardware Bluetooth')


def validate(changed: dict[pathlib.Path, str], root: pathlib.Path) -> None:
    byname = {str(path.relative_to(root)): txt for path, txt in changed.items()}
    main = byname['main/main.cpp']
    mesh = byname['meshcore/MeshCorePort/MeshCorePort.cpp']
    header = byname['meshcore/MeshCorePort/include/MeshCorePort.h']
    ble_h = byname['hardware/Bluetooth/include/BluetoothTransport.h']
    ble_cpp = byname['hardware/Bluetooth/BluetoothTransport.cpp']
    companion = byname['meshcore/MeshCorePort/companion/Companion.cpp']
    require('transport().loop' not in mesh, 'MeshCore still polls a Bluetooth transport')
    require('meshcore::take_pairing_event' not in main, 'Main still obtains pairing events via MeshCore')
    require('PairingEvent' not in header and 'PairingEvent' not in mesh, 'MeshCore pairing event API remains')
    require('ble.begin(' not in mesh and 'ble_.loop()' not in companion and 'ble_.set_pairing_pin' not in companion, 'MeshCore still owns Bluetooth lifecycle or PIN configuration')
    require('bluetooth.begin("MeshCore-Watch", COMPANION_UUIDS)' in main, 'Main does not initialize the expected companion GATT profile')
    require('bluetooth.set_pairing_pin(applied_ble_pin)' in main and 'bluetooth.set_pairing_pin(requested_ble_pin)' in main, 'Legacy BLE PIN is not synchronized from Main')
    require('bluetooth.loop()' in main and 'meshcore::legacy_ble_pin()' in main, 'Main lacks BLE polling or legacy PIN access')
    require('Bluetooth pairing PIN: %06lu' in main, 'Bluetooth pairing PIN serial log is missing')
    require('app::ui::' not in main, 'Main still uses the pairing display')
    require('Transport &transport();' in ble_h and 'Transport &transport()' in ble_cpp, 'Hardware-owned BLE transport accessor is missing')
    require('take_pairing_clear_event' in ble_h and 'take_pairing_clear_event' in ble_cpp, 'Bluetooth pairing clear-event API is missing')
    # Correct NimBLE constant (the previous validator mistakenly used BLE_SM_IO_DISPLAY_ONLY).
    require('BLE_HS_IO_DISPLAY_ONLY' in ble_cpp, 'Bluetooth DISPLAY_ONLY pairing configuration is missing')
    for setting in ('sm_bonding = 1', 'sm_mitm = 1', 'sm_sc = 1'):
        require(setting in ble_cpp, f'Missing required Bluetooth security setting: {setting}')
    for suffix in ('01', '02', '03'):
        require(f'0x{suffix}, 0x00, 0x40, 0x6E' in main, f'GATT characteristic UUID suffix {suffix} is missing')
    require('bluetooth' not in mesh or 'hardware::bluetooth::Transport' in mesh, 'MeshCore has unexpected Bluetooth integration')


def plan(root: pathlib.Path) -> dict[pathlib.Path, str]:
    paths = {
        'ble_h': 'hardware/Bluetooth/include/BluetoothTransport.h',
        'ble_cpp': 'hardware/Bluetooth/BluetoothTransport.cpp',
        'mesh_cpp': 'meshcore/MeshCorePort/MeshCorePort.cpp',
        'mesh_h': 'meshcore/MeshCorePort/include/MeshCorePort.h',
        'companion_cpp': 'meshcore/MeshCorePort/companion/Companion.cpp',
        'main_cpp': 'main/main.cpp',
        'main_cmake': 'main/CMakeLists.txt',
    }
    source = {}
    for key, relative in paths.items():
        path = root / relative
        require(path.is_file(), f'Missing Phase 3 source: {path}')
        source[key] = path.read_text(encoding='utf-8')
    changed = {}
    changed[root / paths['ble_h']] = change_bluetooth_header(source['ble_h'])
    changed[root / paths['ble_cpp']] = change_bluetooth_cpp(source['ble_cpp'])
    mc, uuids = change_meshcore_cpp(source['mesh_cpp'])
    changed[root / paths['mesh_cpp']] = mc
    changed[root / paths['mesh_h']] = change_meshcore_h(source['mesh_h'])
    changed[root / paths['companion_cpp']] = change_companion_cpp(source['companion_cpp'])
    changed[root / paths['main_cpp']] = change_main_cpp(source['main_cpp'], uuids)
    changed[root / paths['main_cmake']] = change_main_cmake(source['main_cmake'])
    validate(changed, root)
    return changed


def apply(root: pathlib.Path, changed: dict[pathlib.Path, str]) -> pathlib.Path:
    name = 'bluetooth_separation_backup_' + dt.datetime.now().strftime('%Y%m%d_%H%M%S_%f') + '.tar.gz'
    backup = root / name
    with tarfile.open(backup, 'w:gz') as archive:
        for path in changed:
            archive.add(path, arcname=str(path.relative_to(root)))
    print(f'Source backup: {backup}', flush=True)
    written = []
    try:
        for path, value in changed.items():
            temp = path.with_suffix(path.suffix + '.bluetooth_new')
            require(not temp.exists(), f'Refusing to overwrite: {temp}')
            temp.write_text(value, encoding='utf-8')
            os.replace(temp, path)
            written.append(path)
    except Exception:
        with tarfile.open(backup, 'r:gz') as archive:
            for member in archive.getmembers():
                original = root / member.name
                if original in written:
                    with archive.extractfile(member) as src:
                        original.write_bytes(src.read())
        raise
    return backup



def verify_installed(root: pathlib.Path) -> None:
    required = {
        'main/main.cpp': ['hardware::bluetooth::transport()', 'bluetooth.begin("MeshCore-Watch", COMPANION_UUIDS)', 'bluetooth.loop()', 'Bluetooth pairing PIN: %06lu', 'requested_ble_pin'],
        'meshcore/MeshCorePort/MeshCorePort.cpp': ['hardware::bluetooth::transport()', 'uint32_t legacy_ble_pin()'],
        'meshcore/MeshCorePort/include/MeshCorePort.h': ['uint32_t legacy_ble_pin();'],
        'hardware/Bluetooth/include/BluetoothTransport.h': ['Transport &transport();', 'take_pairing_clear_event()'],
        'hardware/Bluetooth/BluetoothTransport.cpp': ['Transport &transport()', 'take_pairing_clear_event()', 'sm_bonding = 1', 'sm_mitm = 1', 'sm_sc = 1'],
    }
    for relative, tokens in required.items():
        content = (root / relative).read_text(encoding='utf-8')
        for token in tokens:
            require(token in content, f'After-patch check: missing {token!r} in {relative}')
    mesh = (root / 'meshcore/MeshCorePort/MeshCorePort.cpp').read_text(encoding='utf-8')
    companion = (root / 'meshcore/MeshCorePort/companion/Companion.cpp').read_text(encoding='utf-8')
    main = (root / 'main/main.cpp').read_text(encoding='utf-8')
    header = (root / 'meshcore/MeshCorePort/include/MeshCorePort.h').read_text(encoding='utf-8')
    require('ble.begin(' not in mesh and 'ble_.loop()' not in companion and 'ble_.set_pairing_pin' not in companion, 'BLE lifecycle or pairing control still in MeshCore')
    require('PairingEvent' not in header and 'take_pairing_event' not in main, 'Pairing events still exposed by MeshCore')
    require('app::ui::init()' not in main, 'Pairing screen still initialized by Main')
    require('bluetooth.set_pairing_pin(requested_ble_pin)' in main, 'Main no longer syncs companion PIN updates')
    for suffix in ('01', '02', '03'):
        require(f'0x{suffix}, 0x00, 0x40, 0x6E' in main, f'Missing original GATT UUID {suffix}')
    print('PASS: Main owns Bluetooth start, PIN events/logging and TX polling')
    print('PASS: MeshCore has no BLE start/loop/pairing-event/PIN configuration ownership')
    print('PASS: GATT UUIDs, MITM, Secure Connections and bonding configuration retained')
    print('Note: these are static checks, not a firmware build or hardware test.')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('project', type=pathlib.Path, help='Single current firmware project root (Phase 3)')
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--apply', action='store_true', help='Back up and modify seven source files in-place')
    group.add_argument('--check', action='store_true', help='Validate that migration is applicable without modifying sources')
    group.add_argument('--verify', action='store_true', help='Check the already migrated code without modifying sources')
    args = parser.parse_args()
    root = args.project.expanduser().resolve()
    require(root.is_dir(), f'Project path missing: {root}')
    require((root / 'hardware/Bluetooth/BluetoothTransport.cpp').is_file(), 'Project must contain the Phase 3 Hardware Bluetooth layout')
    require((root / 'meshcore/MeshCorePort/companion/Companion.cpp').is_file(), 'Project must contain Phase 3 Companion layout')
    if args.verify:
        verify_installed(root)
        return
    changed = plan(root)
    if args.check:
        print('PASS: BLE separation changes apply cleanly to your Phase 3 tree; no files changed.')
        return
    if not args.apply:
        for path, value in changed.items():
            before = path.read_text(encoding='utf-8')
            print(''.join(difflib.unified_diff(before.splitlines(keepends=True), value.splitlines(keepends=True), fromfile=str(path.relative_to(root)), tofile=str(path.relative_to(root)) + ' (proposed)')))
        print('\nPreview only. Use --apply to back up and update your project.')
        return
    backup = apply(root, changed)
    print('PASS: BLE lifecycle, pairing and polling belong to Main/Hardware; MeshCore uses BLE frames.')
    print('UNCHANGED: MeshCore protocol, NVS format, stored identity/bonds, UUID bytes, MITM/Secure Connections.')
    print('No Git operations performed. No ESP-IDF or hardware test performed here.')
    print(f'Rollback: tar -xzf "{backup}" -C "{root}"')
    print(f'Next: cd "{root}" && idf.py build')


if __name__ == '__main__':
    try:
        main()
    except (Mismatch, OSError, AssertionError) as error:
        print(f'ERROR: {error or type(error).__name__}', file=sys.stderr)
        raise SystemExit(1)
