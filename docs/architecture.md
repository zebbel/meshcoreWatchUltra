# MeshCore Watch Ultra architecture

## Ownership

- `main/`: ESP-IDF entry point and application configuration only.
- `watch/WatchApplication/`: startup order, application loop, pairing events,
  and publication of watch status. Runs in the original app_main task.
- `services/PowerManagement/`: DFS, light-sleep wake sources and restoration,
  PM guards and sleep counters. Takes activity state as input; no UI dependency.
- `services/PositionSync/`: policy for persisting a fresh GPS fix in MeshCore.
- `services/DisplayPower/`: touch/timeout policy and display sleep lock.
- `ui/WatchDisplay/`: LVGL lifecycle, touch pointer, invalidation and flush acknowledgement.
  `FlowBridge.cpp` exposes native variables and schedules EEZ Flow. The EEZ
  project in `designer/` and its `generated/` export own all layout, navigation
  and brightness policy. Never edit generated source by hand.
- `hardware/`: board pins, bus ownership, device drivers and platform primitives.
  CO5300 owns its single persistent internal DMA stage and all SPI transfers.
- `meshcore/MeshCorePort/`: protocol, radio adapters, storage, routing and companion.

Dependency direction: application -> UI/services/MeshCore -> hardware.
TimeService owns UTC restore/persistence and GNSS synchronization policy. The
GNSS driver emits valid RMC UTC through a callback; MeshCore's RTC adapter uses
TimeService. GNSS acquisition scheduling stays in the GPS driver for now.
The WatchStatus component is a plain data model shared by the application and UI.
No new dependency from hardware to watch/UI/services.

## Storage and touch

Contacts own their database, checksum and 30-minute deferred-save policy in
MeshCorePort. The SD driver only owns sessions, shared SPI2, power, and safe
file I/O (temporary/backup files). No separate SD save worker or queue.
The CST9217 driver owns GPIO12, I2C and report ACK. DisplayPower owns wake
suppression and the fixed-size pointer buffer; LVGL consumes samples without
I2C. Keep the 8192-byte display_power stack: transition callbacks still run
Flow and a synchronous redraw under the existing adapter lock.

## Display invariants

The validated configuration is 410 x 502 portrait RGB565, 40 MHz QSPI, one
32-row PSRAM draw buffer, a persistent 4096-byte internal DMA staging buffer,
one queued SPI transaction, no direct PSRAM DMA, full-width dirty rectangles,
and exactly one LVGL acknowledgement per completed region. The driver fences
DMA before stage reuse and display reveal. DisplayPower holds its light-sleep
lock through wake/redraw. Yield every two LVGL flushes (same effective cadence
as the previous duplicate callback). Preserve the CO5300 initialization table,
standby behavior and orientation. The panel starts blank; EEZ must request
a valid brightness (1–100%). The driver retains the last successful request
for hidden wake/reveal; there is no firmware charging brightness policy.

## Logging

Keep initialization summaries, failures and recovery, pairing PIN delivery,
and important connection events at INFO/WARN/ERROR. Repetitive timing, PM
statistics, rail transitions, BLE negotiation details and TX counters are DEBUG.
Enable ESP-IDF maximum/default log level DEBUG when investigating these paths.
Sleep percentage remains available to EEZ through a native variable. Normal touch
and panel transitions do not emit repetitive logs. Keep failure/recovery logs.

## Regression checklist

Build using ESP-IDF 6.1.1 with the existing sdkconfig and initialized submodules.
Test cold boot, repeated touch wake/timeout, live clock/status updates with BLE
connected and disconnected, BLE pairing and reconnect, LoRa RX/TX, GPS on/off,
SD persistence, and battery-only light sleep. Confirm no DMA underflow, memory
allocation error, watchdog, tearing, or change in sleep behavior.
