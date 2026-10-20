// Control socket for the launcher (see control.cc).

#ifndef NIGHTCAP_AC_CONTROL_H_
#define NIGHTCAP_AC_CONTROL_H_

namespace nac {

// Starts listening (once). Called from the first event pump.
void StartControl();

}  // namespace nac

#endif  // NIGHTCAP_AC_CONTROL_H_
