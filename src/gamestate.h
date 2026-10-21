// Whether Roblox is in a game right now, read from Nightcap's own session
// log ("Joining game ..." and "onGameLeaveBegin()" lines).

#ifndef VOX_TTS_GAMESTATE_H_
#define VOX_TTS_GAMESTATE_H_

namespace nac {

bool InGame();

}  // namespace nac

#endif  // VOX_TTS_GAMESTATE_H_
