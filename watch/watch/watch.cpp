#include "watch.h"

#include "WatchDisplay.h"
#include "WatchStatus.h"
#include "MeshCorePort.h"

namespace watch {
    // ------------------------------------------------------------------------
    // Watch layer polling
    // ------------------------------------------------------------------------

    // Process pairing every iteration; status collection is independently rate-limited.
    void loop(){
        display::update_bluetooth_pairing();
        status::update(meshcore::is_initialized());
    }
}
