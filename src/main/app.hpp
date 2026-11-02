#pragma once

namespace winback {
    // Window thread: applies pending mod changes and ends the process, then
    // starts the game again if asked. The runtime's own shutdown frees game
    // memory while the game's threads still run, so it isn't used.
    [[noreturn]] void exit_app(bool restart);
}
