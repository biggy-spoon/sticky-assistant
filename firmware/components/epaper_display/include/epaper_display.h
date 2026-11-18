#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// The reTerminal Sticky's 800x480 black-and-white e-paper panel (SSD1677
// controller): a 1bpp framebuffer and full or partial refreshes of it.
// Drawing is done by the ui component through SetPixel.
namespace epaper_display {

esp_err_t Init();

void Clear(bool black);
void SetPixel(int x, int y, bool black);

// A full refresh (about two seconds, with a flash) or a partial one (well
// under a second, no flash, only the changed pixels). A partial refresh falls
// back to a full one until a full refresh has set a baseline. Partial refreshes
// leave faint ghosts that build up, so the caller should mix in full ones.
// did_full, if given, reports whether a full refresh actually ran.
esp_err_t Refresh(bool full, bool* did_full = nullptr);

// Puts the panel's controller into deep sleep. The picture stays on the glass
// with no power. The next refresh after this is a full one.
esp_err_t Sleep();

}  // namespace epaper_display
