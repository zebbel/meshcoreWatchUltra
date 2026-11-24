# MeshCore Watch Ultra architecture

## Ownership

- `main/`: ESP-IDF entry point and application configuration only.
- `watch/WatchApplication/`: startup order, application loop, pairing events,
  and publication of watch status. Runs in the original app_main task.
- `services/PowerManagement/`: DFS, light-sleep wake sources and restoration,
  PM guards and sleep counters. Takes activity state as input; no UI dependency.
- `services/PositionSync/`: policy for persisting a fresh GPS fix in MeshCore.
- `services/DisplayPower/`: touch/timeout policy and display sleep lock.
- `ui/WatchDisplay/`: LVGL lifecycle, invalidation and flush acknowledgement;
  `pages/` owns watch-face layout and presentation.
- `hardware/`: board pins, bus ownership, device drivers and platform primitives.
  CO5300 owns its single persistent internal DMA stage and all SPI transfers.
- `meshcore/MeshCorePort/`: protocol, radio adapters, storage, routing and companion.

Dependency direction: application -> UI/services/MeshCore -> hardware.
TimeService owns UTC restore/persistence and GNSS synchronization policy. The
GNSS driver emits valid RMC UTC through a callback; MeshCore's RTC adapter uses
TimeService. GNSS acquisition scheduling stays in the GPS driver for now.
The WatchStatus component is a plain data model shared by the application and UI.
No new dependency from hardware to watch/UI/services.

## Display invariants

The validated configuration is 410 x 502 portrait RGB565, 40 MHz QSPI, one
32-row PSRAM draw buffer, a persistent 4096-byte internal DMA staging buffer,
one queued SPI transaction, no direct PSRAM DMA, full-width dirty rectangles,
and exactly one LVGL acknowledgement per completed region. The driver fences
DMA before stage reuse and display reveal. DisplayPower holds its light-sleep
lock through wake/redraw. Yield every two LVGL flushes (same effective cadence
as the previous duplicate callback). Preserve the CO5300 initialization table,
brightness (25%), standby behavior and orientation.

## Logging

Keep initialization summaries, failures and recovery, pairing PIN delivery,
and important connection events at INFO/WARN/ERROR. Repetitive timing, PM
statistics, rail transitions, BLE negotiation details and TX counters are DEBUG.
Enable ESP-IDF maximum/default log level DEBUG when investigating these paths.
The on-screen sleep counters remain available; no new UI feature is introduced.

## Regression checklist

Build using ESP-IDF 6.1.1 with the existing sdkconfig and initialized submodules.
Test cold boot, repeated touch wake/timeout, live clock/status updates with BLE
connected and disconnected, BLE pairing and reconnect, LoRa RX/TX, GPS on/off,
SD persistence, and battery-only light sleep. Confirm no DMA underflow, memory
allocation error, watchdog, tearing, or change in sleep behavior.
