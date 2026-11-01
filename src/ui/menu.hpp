#pragma once

// The recomp's overlay menu: Select (Back) or Escape opens and closes it at
// any time. While it is open the game receives no input.

union SDL_Event;

namespace winback::menu {
    // Registers the RmlUi drawing with RT64. Call before the renderer starts.
    void set_render_hooks();

    // Window thread: handles one SDL event. True if the menu used it.
    bool handle_event(const SDL_Event& ev);

    // Window thread: held-direction repeats and scripted presses.
    void update();

    bool is_open();
}
