#pragma once

namespace services {
    void restore_wakup_pins(); // Before hardware and protocol polling.
    void loop(); // Poll application services.
    void sleep_guard(); // Update the sleep policy and wait for events.
}
