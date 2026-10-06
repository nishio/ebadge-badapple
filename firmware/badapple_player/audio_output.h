#pragma once

// Hardware callbacks keep this transition testable without producing sound.
struct AudioOutputState {
  bool muted = true;
  const bool locked;
  explicit AudioOutputState(bool forceMute) : locked(forceMute) {}

  template<class CodecMute, class Amplifier>
  bool setMuted(bool requested, bool playing, CodecMute codecMute, Amplifier amplifier) {
    if (locked && !requested) return false;
    amplifier(false); // Silence first; never enable PA before codec success.
    muted = true;
    if (!codecMute(requested)) return false;
    muted = requested;
    if (playing && !muted) amplifier(true);
    return true;
  }
};
