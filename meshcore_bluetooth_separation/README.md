# MeshCoreWatchUltra — Bluetooth separation (Phase 3 follow-up)

**Target:** your **single** Phase 3 firmware project, `~/meshcore/meshcoreWatchUltra`.
The script edits that project directly **only when `--apply` is supplied**, after creating a timestamped `.tar.gz` backup of the seven affected source files. It does **not** create another `-phaseN` project, commit, push, erase flash or change the upstream MeshCore code. The script checks that the expected Phase 3 sources exist; if they differ from the refactoring baseline, it stops without applying any edits.

## What changes

| Responsibility | Owner after patch |
|---|---|
| NimBLE, GATT service, authentication, PIN generation, bond storage | `hardware::bluetooth::Transport` |
| Bluetooth construction, configuration, startup, polling, PIN logging | `main/main.cpp` |
| Companion command framing over BLE | `meshcore::Companion` |
| BLE service/characteristic UUID constants | Main configures generic Hardware Bluetooth |
| MeshCore radio/contacts/identity/channel/settings | MeshCore (unchanged) |
| Screen presentation | `DeviceUI.cpp` remains in Main but is **not initialized for pairing** |

The Bluetooth transport now has a singleton accessor `hardware::bluetooth::transport()` so Main and MeshCore use the **same** transport. `meshcore::init()` no longer creates/starts the BLE stack, and `meshcore::loop()` no longer pumps notifications. Main receives the pairing events **directly from Hardware**, logging the PIN with `ESP_LOGI`.

**Compatibility:** BLE advertising name `MeshCore-Watch`, UUID bytes, NUS framing, 12-slot frame queues, authentication, bonding, MTU rules, and connection security stay unchanged. A legacy `ble_pin` field still exists in MeshCore's persisted `Settings` (version 1) and in its companion config command. Main reads that saved value using `meshcore::legacy_ble_pin()` before BLE starts. When Android updates the PIN through its existing companion settings command, **Main** detects the changed setting and calls `hardware::bluetooth::Transport::set_pairing_pin()`; Companion no longer sets pairing configuration. All PIN generation/security checks remain in Hardware. Moving PIN storage fully into Hardware requires an **explicit settings migration** and is intentionally deferred to avoid breaking existing NVS data and Android configuration commands. Thus BLE pairing operation is independent, while the *legacy saved PIN setting* remains a read-only compatibility bridge for Main.

## Apply — one project only

Copy `apply_bluetooth_separation.py` into `~/meshcore/meshcoreWatchUltra` or run it from the extracted archive.

```sh
cd ~/meshcore/meshcoreWatchUltra
python3 apply_bluetooth_separation.py "$PWD" --check
python3 apply_bluetooth_separation.py "$PWD"          # optional: inspect unified diff
python3 apply_bluetooth_separation.py "$PWD" --apply
python3 apply_bluetooth_separation.py "$PWD" --verify
idf.py build
idf.py monitor
```

If `--check` fails, **do not force the edits**; the local source is different from the baseline. The error identifies the expected pattern. You can provide the relevant file for adapting the patch safely.

The script uses Python 3.10+. It does not invoke `idf.py`; compilation and device regression must be run on your PC. Do not flash before the firmware compiles successfully. The prior unresolved `ESP_ERR_NVS_NOT_FOUND` / `hardware::storage::is_not_found` issue, if still present, is independent of this change.

## Hardware regression checklist

1. After boot, Bluetooth advertises `MeshCore-Watch` with the same GATT service UUID. The display is **not** initialized merely to show a PIN.
2. Pair from a fresh phone/cleared **phone-side** bond and check `idf.py monitor` for `Bluetooth pairing PIN: NNNNNN`. Enter the PIN on the phone and verify authenticated bonding.
3. Reconnect an already bonded phone; pairing should not unnecessarily restart.
4. Confirm MeshCore companion commands: contacts, channels, telemetry and LoRa send/receive.
5. Disconnect/reconnect: authenticated state and GATT notifications resume; no recurring PIN spam.
6. Change the BLE fixed PIN through the companion settings command if your app supports it; confirm next pairing uses the configured PIN.
7. Check GPS position updates and existing identity, saved contacts and settings.

Printing pairing PINs in logs is for controlled development only. Remove it for public firmware builds; anyone with access to the serial output can see the current pairing secret.

## Restore the backup

On success the script prints a backup path, for example:

```sh
cd ~/meshcore/meshcoreWatchUltra
tar -xzf bluetooth_separation_backup_YYYYMMDD_HHMMSS_ffffff.tar.gz -C "$PWD"
idf.py build
```

Use the exact name printed by your run. This restores only the seven edited source files; it doesn't touch `.git`, SDK configuration, flash or NVS.

## Validation carried out here

Python syntax check and **six isolated migration tests** (rewrite safety, UUID/security preservation, rollback backup completeness, already-applied detection, post-apply verification, forward declaration) pass using a representative Phase 3-shaped fixture derived from the Phase 1 runtime. **No ESP-IDF build or on-device test has been performed here.**

## Revision: NimBLE constant validation fix

The original package incorrectly validated `BLE_SM_IO_DISPLAY_ONLY` rather than the
actual NimBLE configuration value `BLE_HS_IO_DISPLAY_ONLY`, causing a blank `ERROR:`
when running `--check` on genuine Bluetooth sources. This package corrects the
validator and reports the exact failed validation check rather than an empty
assertion. No firmware driver code was changed for this fix.
