#!/usr/bin/env python3
"""Apply BLE Performance V1 to a local zebbel/meshcoreWatchUltra checkout.

Run in the checkout root:
    python3 apply_ble_performance_v1.py --check
    python3 apply_ble_performance_v1.py
    idf.py build
    python3 apply_ble_performance_v1.py --revert

Alternatively: python3 apply_ble_performance_v1.py --repo /path/to/meshcoreWatchUltra

No Git operations or network access. Refuses unexpected source versions/edits.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile

MARKER = "MCW_BLE_PERFORMANCE_V1"
MANIFEST = ".ble_performance_v1_manifest.json"
PATH_CPP = "hardware/Bluetooth/BluetoothTransport.cpp"
PATH_H = "hardware/Bluetooth/include/BluetoothTransport.h"
PATH_MAIN = "main/main.cpp"
FILES = (PATH_CPP, PATH_H, PATH_MAIN)


def replace_once(source: str, old: str, new: str, label: str) -> str:
    count = source.count(old)
    if count != 1:
        raise ValueError(f"{label}: expected one exact match, found {count}; source differs from verified baseline")
    return source.replace(old, new, 1)


def cpp_patch(s: str) -> str:
    s = replace_once(s,
        '    static ble_uuid128_t tx_uuid = {};\n',
        '    static ble_uuid128_t tx_uuid = {};\n'
        '    // MCW_BLE_PERFORMANCE_V1: low-latency BLE with fast initial discovery.\n'
        '    static constexpr uint32_t kFastAdvDurationMs = 30000;\n'
        '    static constexpr uint32_t kNotificationGapMs = 10;\n'
        '    static constexpr uint32_t kNotifyRetryGapMs = 30;\n',
        'Bluetooth constants')

    s = replace_once(s,
        '        advertise();\n    }\n\n    void Transport::advertise(){',
        '        advertise(true);\n    }\n\n    void Transport::advertise(bool fast){',
        'advertising signature')

    s = replace_once(s,
        '        // MCW_ENERGY_STAGE2_V1: 500-640 ms advertising (0.625 ms BLE units).\n'
        '        // Advertising remains continuous; discovery takes slightly longer.\n'
        '        params.itvl_min = 0x0320;\n'
        '        params.itvl_max = 0x0400;\n'
        '        if(rc == 0){\n'
        '            rc = ble_gap_adv_start(own_addr_type_, nullptr, BLE_HS_FOREVER, &params, bluetooth_gap_callback, nullptr);\n'
        '        }\n'
        '        if(rc != 0) ESP_LOGE(TAG, "Advertising failed: %d", rc);',
        '        // 100-150 ms for 30 seconds; then 250-350 ms to save power.\n'
        '        // Units are 0.625 ms. A new disconnect restarts the fast window.\n'
        '        params.itvl_min = fast ? 160 : 400;\n'
        '        params.itvl_max = fast ? 240 : 560;\n'
        '        if(rc == 0){\n'
        '            rc = ble_gap_adv_start(own_addr_type_, nullptr,\n'
        '                                   fast ? kFastAdvDurationMs : BLE_HS_FOREVER,\n'
        '                                   &params, bluetooth_gap_callback, nullptr);\n'
        '        }\n'
        '        if(rc == 0){\n'
        '            advertising_started_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);\n'
        '            ESP_LOGI(TAG, "BLE %s advertising: %u-%u ms", fast ? "fast" : "idle",\n'
        '                     fast ? 100U : 250U, fast ? 150U : 350U);\n'
        '        }else{\n'
        '            ESP_LOGE(TAG, "Advertising failed: %d", rc);\n'
        '        }',
        'advertising policy')

    s = replace_once(s,
        '                    ESP_LOGI(TAG, "BLE connected; starting authenticated pairing");\n',
        '                    const uint32_t connected_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);\n'
        '                    ESP_LOGI(TAG, "BLE connected after %lu ms advertising; starting authenticated pairing",\n'
        '                             static_cast<unsigned long>(connected_ms - advertising_started_ms_));\n'
        '                    wake_main_task();\n',
        'BLE connect diagnostics')

    s = replace_once(s,
        '                }else{\n'
        '                    advertise();\n'
        '                }\n'
        '                break;\n'
        '            case BLE_GAP_EVENT_DISCONNECT:',
        '                }else{\n'
        '                    advertise(true);\n'
        '                }\n'
        '                break;\n'
        '            case BLE_GAP_EVENT_DISCONNECT:',
        'failed connection fallback')

    s = replace_once(s,
        '                xQueueReset(rx_queue_);\n'
        '                xQueueReset(tx_queue_);\n'
        '                advertise();\n'
        '                break;\n'
        '            case BLE_GAP_EVENT_ADV_COMPLETE:\n'
        '                advertise();\n'
        '                break;',
        '                xQueueReset(rx_queue_);\n'
        '                xQueueReset(tx_queue_);\n'
        '                wake_main_task();\n'
        '                advertise(true);\n'
        '                break;\n'
        '            case BLE_GAP_EVENT_ADV_COMPLETE:\n'
        '                // The 30-second fast-advertising window expired.\n'
        '                if (!link_active()) advertise(false);\n'
        '                break;\n'
        '            case BLE_GAP_EVENT_CONN_UPDATE: {\n'
        '                ble_gap_conn_desc desc = {};\n'
        '                if (event->conn_update.status == 0 &&\n'
        '                    ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {\n'
        '                    ESP_LOGI(TAG, "BLE negotiated: interval=%lu us latency=%u supervision=%u ms",\n'
        '                             static_cast<unsigned long>(desc.conn_itvl) * 1250UL,\n'
        '                             static_cast<unsigned>(desc.conn_latency),\n'
        '                             static_cast<unsigned>(desc.supervision_timeout) * 10U);\n'
        '                } else {\n'
        '                    ESP_LOGW(TAG, "BLE connection parameter update status=%d",\n'
        '                             event->conn_update.status);\n'
        '                }\n'
        '                break;\n'
        '            }\n'
        '            case BLE_GAP_EVENT_MTU:\n'
        '                ESP_LOGI(TAG, "BLE negotiated ATT MTU=%u",\n'
        '                         static_cast<unsigned>(event->mtu.value));\n'
        '                break;',
        'disconnect, advertising rollover, negotiated diagnostics')

    s = replace_once(s,
        '                    subscribed_.store(event->subscribe.cur_notify != 0);\n',
        '                    subscribed_.store(event->subscribe.cur_notify != 0);\n'
        '                    wake_main_task();\n',
        'subscribe wake')

    s = replace_once(s,
        '                    // MCW_ENERGY_STAGE2_V1: request 100-150 ms idle-friendly\n'
        '                    // connection interval (1.25 ms units), no slave latency.\n'
        '                    // The phone may reject this. The connection then continues\n'
        '                    // with its original parameters, without protocol changes.\n'
        '                    ble_gap_upd_params params = {};\n'
        '                    params.itvl_min = 80;\n'
        '                    params.itvl_max = 120;\n'
        '                    params.latency = 0;\n'
        '                    params.supervision_timeout = 600; // 6 seconds (10 ms units)',
        '                    // Request 15-30 ms for responsive commands and contact sync.\n'
        '                    // The central (phone) may negotiate a longer interval.\n'
        '                    ble_gap_upd_params params = {};\n'
        '                    params.itvl_min = 12; // 15 ms (1.25 ms units)\n'
        '                    params.itvl_max = 24; // 30 ms\n'
        '                    params.latency = 0;\n'
        '                    params.supervision_timeout = 400; // 4 s (10 ms units)',
        'connection parameters')

    s = replace_once(s,
        '                    ESP_LOGI(TAG, "Authenticated BLE bond established");\n',
        '                    ESP_LOGI(TAG, "Authenticated BLE bond established");\n'
        '                    wake_main_task();\n',
        'auth wake')

    s = replace_once(s,
        '            if(xQueueSend(rx_queue_, &frame, 0) != pdTRUE){\n'
        '                return BLE_ATT_ERR_INSUFFICIENT_RES;\n'
        '            }\n'
        '            return 0;',
        '            if(xQueueSend(rx_queue_, &frame, 0) != pdTRUE){\n'
        '                return BLE_ATT_ERR_INSUFFICIENT_RES;\n'
        '            }\n'
        '            // Wake app_main immediately instead of waiting up to 50 ms.\n'
        '            wake_main_task();\n'
        '            return 0;',
        'BLE receive wake')

    s = replace_once(s,
        '    void Transport::loop(){\n'
        '        if(!authenticated_.load() || !subscribed_.load()) return;\n'
        '        uint32_t now = esp_timer_get_time() / 1000ULL;\n'
        '        if(now - last_notification_ms_ < 60) return;\n'
        '        Frame frame;\n'
        '        if(xQueuePeek(tx_queue_, &frame, 0) != pdTRUE) return;\n'
        '        if(!can_send(frame.length)) return;\n'
        '        auto mbuf = ble_hs_mbuf_from_flat(frame.data, frame.length);\n'
        '        if(mbuf == nullptr) return;\n'
        '        int rc = ble_gatts_notify_custom(connection_handle_.load(), tx_handle_, mbuf);\n'
        '        if(rc == 0){\n'
        '            xQueueReceive(tx_queue_, &frame, 0);\n'
        '            last_notification_ms_ = now;\n'
        '        }else{\n'
        '            ESP_LOGW(TAG, "BLE notify retry: %d", rc);\n'
        '        }\n'
        '    }',
        '    void Transport::set_wakeup_task(TaskHandle_t task){\n'
        '        wakeup_task_.store(task, std::memory_order_release);\n'
        '    }\n'
        '\n'
        '    void Transport::wake_main_task(){\n'
        '        // NimBLE callbacks run in task context, not GPIO ISR context.\n'
        '        const TaskHandle_t task = wakeup_task_.load(std::memory_order_acquire);\n'
        '        if(task != nullptr) xTaskNotifyGive(task);\n'
        '    }\n'
        '\n'
        '    void Transport::loop(){\n'
        '        if(!authenticated_.load() || !subscribed_.load()) return;\n'
        '        const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);\n'
        '        if(last_stats_ms_ == 0) last_stats_ms_ = now;\n'
        '        if(static_cast<uint32_t>(now - last_stats_ms_) >= 30000U){\n'
        '            ESP_LOGI(TAG, "BLE TX/30s: sent=%lu retries=%lu pending=%u/%d MTU=%u",\n'
        '                     static_cast<unsigned long>(tx_sent_),\n'
        '                     static_cast<unsigned long>(tx_retries_),\n'
        '                     static_cast<unsigned>(uxQueueMessagesWaiting(tx_queue_)),\n'
        '                     QUEUE_CAPACITY,\n'
        '                     static_cast<unsigned>(ble_att_mtu(connection_handle_.load())));\n'
        '            tx_sent_ = 0;\n'
        '            tx_retries_ = 0;\n'
        '            last_stats_ms_ = now;\n'
        '        }\n'
        '        // Success pacing targets 10 ms; rejected sends back off to 30 ms.\n'
        '        if(last_notification_ms_ != 0 &&\n'
        '           static_cast<uint32_t>(now - last_notification_ms_) < notification_gap_ms_) return;\n'
        '        Frame frame;\n'
        '        if(xQueuePeek(tx_queue_, &frame, 0) != pdTRUE) return;\n'
        '        if(!can_send(frame.length)) return;\n'
        '        auto mbuf = ble_hs_mbuf_from_flat(frame.data, frame.length);\n'
        '        if(mbuf == nullptr){\n'
        '            notification_gap_ms_ = kNotifyRetryGapMs;\n'
        '            last_notification_ms_ = now;\n'
        '            ++tx_retries_;\n'
        '            return;\n'
        '        }\n'
        '        // NimBLE consumes the mbuf even when notification fails.\n'
        '        const int rc = ble_gatts_notify_custom(connection_handle_.load(), tx_handle_, mbuf);\n'
        '        last_notification_ms_ = now;\n'
        '        if(rc == 0){\n'
        '            xQueueReceive(tx_queue_, &frame, 0);\n'
        '            notification_gap_ms_ = kNotificationGapMs;\n'
        '            ++tx_sent_;\n'
        '        }else{\n'
        '            notification_gap_ms_ = kNotifyRetryGapMs;\n'
        '            ++tx_retries_;\n'
        '            if(tx_retries_ == 1 || (tx_retries_ % 32) == 0)\n'
        '                ESP_LOGW(TAG, "BLE notify backpressure: rc=%d, retries=%lu",\n'
        '                         rc, static_cast<unsigned long>(tx_retries_));\n'
        '        }\n'
        '    }',
        'notification pacing and diagnostics')
    return s


def header_patch(s: str) -> str:
    s = replace_once(s,
        '#include "freertos/queue.h"\n',
        '#include "freertos/queue.h"\n#include "freertos/task.h"\n',
        'header task include')
    s = replace_once(s,
        '        bool send_best_effort(const uint8_t *data, size_t length);\n'
        '        void loop();\n'
        '        bool connected() const { return authenticated_.load(); }',
        '        bool send_best_effort(const uint8_t *data, size_t length);\n'
        '        void loop();\n'
        '        // MCW_BLE_PERFORMANCE_V1: wake app_main when new BLE frames arrive.\n'
        '        void set_wakeup_task(TaskHandle_t task);\n'
        '        bool link_active() const { return connection_handle_.load() != 0xFFFF; }\n'
        '        bool has_pending_tx() const {\n'
        '            return tx_queue_ != nullptr && uxQueueMessagesWaiting(tx_queue_) != 0;\n'
        '        }\n'
        '        bool connected() const { return authenticated_.load(); }',
        'header public API')
    s = replace_once(s,
        '        std::atomic<uint16_t> connection_handle_{0xFFFF};\n'
        '        uint16_t tx_handle_ = 0;\n'
        '        uint8_t own_addr_type_ = 0;\n'
        '        uint32_t last_notification_ms_ = 0;',
        '        std::atomic<uint16_t> connection_handle_{0xFFFF};\n'
        '        std::atomic<TaskHandle_t> wakeup_task_{nullptr};\n'
        '        uint16_t tx_handle_ = 0;\n'
        '        uint8_t own_addr_type_ = 0;\n'
        '        uint32_t last_notification_ms_ = 0;\n'
        '        uint32_t notification_gap_ms_ = 10;\n'
        '        uint32_t last_stats_ms_ = 0;\n'
        '        uint32_t tx_sent_ = 0;\n'
        '        uint32_t tx_retries_ = 0;\n'
        '        uint32_t advertising_started_ms_ = 0;',
        'header members')
    s = replace_once(s,
        '        void advertise();\n        void host_loop();',
        '        void advertise(bool fast);\n        void wake_main_task();\n        void host_loop();',
        'header methods')
    return s


def main_patch(s: str) -> str:
    s = replace_once(s,
        '    const bool must_stay_awake = ui::watch_display::screen_awake() || gps.powered;',
        '    // MCW_BLE_PERFORMANCE_V1: pairing and active BLE links need low latency.\n'
        '    const bool must_stay_awake = ui::watch_display::screen_awake() ||\n'
        '                                 gps.powered || hardware::bluetooth::transport().link_active();',
        'BLE sleep guard')
    s = replace_once(s,
        '    // MCW_CPU_LORA_IDLE_V1: DIO1 IRQ wakes app_main for RX/TX events.\n'
        '    // MeshCore timers, GPS UART and BLE still run at least every 50 ms.\n'
        '    // Registration is optional; the periodic timeout remains the recovery path.\n'
        '    if(meshcore_enabled) hardware::lora::set_wakeup_task(xTaskGetCurrentTaskHandle());\n'
        '    constexpr uint32_t kMainLoopWaitMs = 50;\n'
        '    ESP_LOGI(TAG, "IRQ-assisted MeshCore loop: <=50 ms idle wait, LoRa DIO1 wake");',
        '    // MCW_BLE_PERFORMANCE_V1: shared task notifications from LoRa and NimBLE.\n'
        '    // Connected BLE gets a 10 ms fallback; idle MeshCore keeps 50 ms.\n'
        '    if(meshcore_enabled) hardware::lora::set_wakeup_task(xTaskGetCurrentTaskHandle());\n'
        '    bluetooth.set_wakeup_task(xTaskGetCurrentTaskHandle());\n'
        '    constexpr uint32_t kMainLoopWaitMs = 50;\n'
        '    constexpr uint32_t kActiveBleWaitMs = 10;\n'
        '    ESP_LOGI(TAG, "IRQ-assisted MeshCore loop: BLE event wake and 10/50 ms fallback");',
        'main loop setup')
    s = replace_once(s,
        '        // Incoming LoRa RX/TX IRQ wakes immediately; otherwise sleep briefly.\n'
        '        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kMainLoopWaitMs));',
        '        // Incoming BLE writes and LoRa IRQs wake immediately.\n'
        '        const uint32_t wait_ms = (bluetooth.link_active() || bluetooth.has_pending_tx())\n'
        '            ? kActiveBleWaitMs : kMainLoopWaitMs;\n'
        '        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));',
        'main loop wait')
    return s


PATCH = {PATH_CPP: cpp_patch, PATH_H: header_patch, PATH_MAIN: main_patch}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def atomic_write(path: Path, data: bytes):
    fd, temp = tempfile.mkstemp(prefix=path.name + ".tmp.", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(temp, path)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", type=Path, help="Local firmware checkout root (default: script directory)")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="Validate expected files, without writing")
    mode.add_argument("--revert", action="store_true", help="Restore backups, if patched files are unchanged")
    args = ap.parse_args()
    root = (args.repo or Path(__file__).resolve().parent).resolve()
    manifest_path = root / MANIFEST
    if args.revert:
        if not manifest_path.exists():
            raise ValueError("Missing BLE patch manifest; nothing safe to revert")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        original = {}
        for rel in FILES:
            dst = root / rel
            bak = root / (rel + ".ble-performance-v1.bak")
            if sha(dst.read_bytes()) != manifest[rel]["patched_sha256"]:
                raise ValueError(f"{rel} changed after patch; refusing to overwrite your edits")
            contents = bak.read_bytes()
            if sha(contents) != manifest[rel]["original_sha256"]:
                raise ValueError(f"{bak}: backup checksum mismatch")
            original[rel] = contents
        for rel in FILES:
            atomic_write(root / rel, original[rel])
        for rel in FILES:
            (root / (rel + ".ble-performance-v1.bak")).unlink()
        manifest_path.unlink()
        print("Reverted BLE Performance V1; original sources restored.")
        return

    source = {}
    patched = {}
    for rel in FILES:
        path = root / rel
        if not path.is_file():
            raise ValueError(f"File not found: {path}. Put this script in the repository root or use --repo.")
        raw = path.read_bytes()
        if MARKER in raw.decode("utf-8"):
            raise ValueError(f"{rel} is already patched; use --revert first")
        source[rel] = raw
        patched[rel] = PATCH[rel](raw.decode("utf-8")).encode("utf-8")
        print(f"OK {rel}: {len(raw)} -> {len(patched[rel])} bytes")
    if manifest_path.exists():
        raise ValueError(f"{MANIFEST} exists; refuse overwrite")
    for rel in FILES:
        if (root / (rel + ".ble-performance-v1.bak")).exists():
            raise ValueError(f"Backup already exists for {rel}; refusing overwrite")
    if args.check:
        print("Check passed: all three files match the expected pre-patch code; no changes made.")
        return

    manifest = {}
    for rel in FILES:
        manifest[rel] = {"original_sha256": sha(source[rel]), "patched_sha256": sha(patched[rel])}
    try:
        for rel in FILES:
            atomic_write(root / (rel + ".ble-performance-v1.bak"), source[rel])
        for rel in FILES:
            atomic_write(root / rel, patched[rel])
        atomic_write(manifest_path, (json.dumps(manifest, indent=2) + "\n").encode("utf-8"))
    except Exception:
        for rel in FILES:
            if rel in source:
                atomic_write(root / rel, source[rel])
        raise
    print("Applied BLE Performance V1. Backups and checksums saved; run idf.py build.")
    print("To undo: python3 apply_ble_performance_v1.py --revert")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, UnicodeError, json.JSONDecodeError) as err:
        print(f"ERROR: {err}", file=sys.stderr)
        sys.exit(1)
