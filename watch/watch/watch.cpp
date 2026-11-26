#include "watch.h"

#include "WatchDisplay.h"
#include "WatchStatus.h"
#include "MeshCorePort.h"

namespace watch {
void loop(){
    display::update_bluetooth_pairing();
    status::update(meshcore::is_initialized());
}
}
