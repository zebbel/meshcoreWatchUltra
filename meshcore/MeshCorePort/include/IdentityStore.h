#pragma once

#include "esp_err.h"

#include "Identity.h"
#include "Utils.h"

namespace meshcore {

    // Restore the identity from SD; create a new identity only in an empty store.
    // Refuse to replace an unreadable or missing identity in a populated store.
    esp_err_t load_or_create_identity(mesh::LocalIdentity &identity, mesh::RNG &rng);

}
