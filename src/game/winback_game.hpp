#pragma once

#include "librecomp/rsp.hpp"

namespace winback {
    inline constexpr const char8_t* game_id = u8"winback.us";

    // Registers the code section and ROM with the runtime.
    bool register_game();
    const recomp::rsp::callbacks_t& rsp_callbacks();
}
