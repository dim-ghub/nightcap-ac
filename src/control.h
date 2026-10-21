// Control socket for the launcher (see control.cc).

#ifndef VOX_TTS_CONTROL_H_
#define VOX_TTS_CONTROL_H_

namespace nac {

// Starts listening (once). Called from the first event pump.
void StartControl();

}  // namespace nac

#endif  // VOX_TTS_CONTROL_H_
