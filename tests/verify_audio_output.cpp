#include "../firmware/badapple_player/audio_output.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main() {
  AudioOutputState output(false);
  bool pa=false, codecMuted=true, codecOk=true;
  char events[16]={}; unsigned eventPos=0;
  auto append=[&](char c){events[eventPos++]=c;events[eventPos]=0;};
  auto amp=[&](bool on){pa=on;append(on?'A':'a');};
  auto codec=[&](bool mute){append(mute?'M':'U');if(codecOk)codecMuted=mute;return codecOk;};
  assert(output.muted && !pa);
  assert(output.setMuted(false,true,codec,amp));
  assert(strcmp(events,"aUA") == 0 && !output.muted && !codecMuted && pa);
  eventPos=0;events[0]=0;
  assert(output.setMuted(true,true,codec,amp));
  assert(strcmp(events,"aM") == 0 && output.muted && codecMuted && !pa);
  // A failed codec unmute must not turn the amplifier on.
  eventPos=0;events[0]=0;codecOk=false;
  assert(!output.setMuted(false,true,codec,amp));
  assert(strcmp(events,"aU") == 0 && output.muted && !pa);
  // Forced-silent builds never attempt to unmute the codec or amplifier.
  AudioOutputState locked(true);eventPos=0;events[0]=0;codecOk=true;
  assert(!locked.setMuted(false,true,codec,amp));
  assert(events[0]==0 && locked.muted && !pa);
  assert(locked.setMuted(true,true,codec,amp));
  assert(locked.muted && !pa && codecMuted);
  // Even a requested unmute cannot enable the amplifier outside playback.
  eventPos=0;events[0]=0;
  assert(output.setMuted(false,false,codec,amp));
  assert(strcmp(events,"aU") == 0 && !pa);
  puts("{\"default_muted\":true,\"mute_unmute_order\":\"passed\",\"codec_failure_keeps_pa_off\":true,\"silent_lock_blocks_unmute\":true,\"idle_pa_off\":true}");
}
