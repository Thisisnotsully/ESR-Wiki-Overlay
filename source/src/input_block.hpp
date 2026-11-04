// Stops the game from reacting to the controller while the wiki is open. The wiki still gets its controller
// input, because it reads the pad itself (gamepad.cpp), not through the game.
//
// Two places in the game are redirected to small stubs:
//   * Buttons: the game's key-transition handler (game RVA 0x13EDD0, called from 0x13B3A2). The stub drops key
//     presses while the wiki is visible. Releases always pass, so no key can get stuck down.
//   * Sticks: the game's axis dispatcher (game RVA 0x13F470, arguments kind, x, y). The stub reports the stick as
//     centred (x = y = 0) while the wiki is visible, which clears the movement and aim state it writes.
//
// Each place is checked against the build it was verified on (the call site and the function's first bytes must
// match). Anything that does not match is refused and the game is left alone there.
#pragma once

#include <functional>

namespace esrwiki::inputblock {

// `isWikiOpen` runs on the game thread for each key press; it must be quick and must not throw.
// Returns false, changing nothing, when the game does not match the expected build.
bool Install(std::function<bool()> isWikiOpen);

// Restores the game's original call. Safe to call when not installed.
void Remove();

bool Installed();

}  // namespace esrwiki::inputblock
