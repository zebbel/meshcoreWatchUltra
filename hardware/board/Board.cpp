#include "Board.h"
#include "board.h"

namespace hardware::board {
    esp_err_t init(){ return ::board::init(); }
}
