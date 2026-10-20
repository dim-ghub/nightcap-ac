// The in-game menu, drawn with Dear ImGui in the style of Roblox's own
// in-experience menu, and the top-bar button that opens it.

#ifndef NIGHTCAP_AC_UI_H_
#define NIGHTCAP_AC_UI_H_

#include <vector>

#include "overlay.h"

namespace nac::ui {

// Forget the built font atlas (the renderer was recreated).
void ResetFonts();
// (Re)builds fonts for this pixel scale. True when the atlas changed and
// must be uploaded again.
bool PrepareFonts(float px_per_unit);
void FeedInput(const std::vector<overlay::Input>& input, float px_per_unit);
// Builds this frame's UI. False when nothing needs drawing.
bool Draw(float px_per_unit, bool srgb_target);

}  // namespace nac::ui

#endif  // NIGHTCAP_AC_UI_H_
