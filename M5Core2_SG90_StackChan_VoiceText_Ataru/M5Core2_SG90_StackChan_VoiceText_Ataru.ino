// Core2 network voice is the default. Set to 0 to build the original firmware.
#define USE_NETWORK_VOICE 1
#if USE_NETWORK_VOICE
#include "NetworkVoice.h"
#else
#include "LegacyFirmware.h"
#endif
