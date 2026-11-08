#pragma once

// Read-only probe for the pinned M5Unified 0.1.16 backend. Its begin() may leave
// isRunning()==true even when task creation failed; playRaw() then returns true.
// A base-member pointer inspects the real object, without downcasting it.
class SpeakerBackendProbe : public m5::Speaker_Class {
 public:
  static bool hasTask(const m5::Speaker_Class& speaker) {
    const auto member = &SpeakerBackendProbe::_task_handle;
    return speaker.*member != nullptr;
  }
};

inline bool speakerBackendReady(const m5::Speaker_Class& speaker) {
  return speaker.isRunning() && SpeakerBackendProbe::hasTask(speaker);
}
