#pragma once

// M5Unified 0.1.16 Speaker.end() stops its task but leaves the I2S driver installed.
// Only release a port known to be owned by the speaker, never an active microphone.
template<class Speaker, class Microphone, class ReleaseDriver>
bool handoverToMicrophone(Speaker& speaker, Microphone& microphone, ReleaseDriver releaseDriver) {
  const bool wasRunning = speaker.isRunning();
  const auto port = speaker.config().i2s_port;
  speaker.end();
  if (wasRunning) releaseDriver(port);
  return microphone.begin();
}
