// Whether Roblox is in a game right now, read from Nightcap's own session
// log ("Joining game ..." and "onGameLeaveBegin()" lines).

#ifndef NIGHTCAP_AC_GAMESTATE_H_
#define NIGHTCAP_AC_GAMESTATE_H_

namespace nac {

bool InGame();

}  // namespace nac

#endif  // NIGHTCAP_AC_GAMESTATE_H_
