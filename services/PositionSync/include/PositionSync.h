#pragma once

namespace services::position_sync {
    // Persist a fresh position when the change and write interval permit it.
    void synchronize();
}
