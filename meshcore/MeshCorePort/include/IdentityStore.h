
#pragma once

#include "esp_err.h"

#include "Identity.h"
#include "Utils.h"

namespace meshcore {

    // NVS must have been initialized before calling this.
    // Never erases all of NVS to recover an error: loss of identity is fatal.
    esp_err_t load_or_create_identity(mesh::LocalIdentity &identity, mesh::RNG &rng);

}
