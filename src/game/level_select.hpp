#pragma once

// Level Select: the list of stages and checkpoints, jumps to them, and
// snapshots of checkpoints reached in natural play.

#include <cstdint>
#include <string>
#include <vector>

namespace winback::levels {
    struct Checkpoint {
        uint8_t id;   // The game's checkpoint id (script argument).
        uint8_t map;  // Map the checkpoint is in.
    };

    struct Stage {
        int number;                          // 0-30, as on the stage clear screen.
        uint8_t first_map;
        const char* area;                    // The game's area name.
        std::vector<Checkpoint> checkpoints; // In script order.
    };

    // Stages 0-30, read from the ROM's event scripts on first use.
    const std::vector<Stage>& stages();

    // True when a snapshot of this checkpoint was recorded in natural play.
    bool is_recorded(int stage, const Checkpoint& cp);

    // Asks the game to jump to a stage start (checkpoint = -1) or to one of
    // the stage's checkpoints. Taken by the game thread on its next frame.
    void request_jump(int stage, int checkpoint);
}
