/* Skin UI sounds: main/UI thread owns set_root/play/stop/release.
 * set_root synchronously reads skin.json assets[{id,file,type:"sound"}].
 * WAV: RIFF WAVE PCM format 1, little-endian signed 16-bit, 44100Hz mono,
 * <=350ms (15435 frames), <=65536 bytes/file. At most 8 sounds,
 * <=196608 bytes cached raw PCM. Manifest <=32768 bytes.
 * IDs: 1..32 ASCII letters/digits/_/- (chosen by the skin).
 * ID names are chosen by the skin (move/click/enter/back are conventions).
 * A new play replaces the voice; an unknown ID leaves the voice unchanged.
 * Paths: assets/<ASCII basename>.wav (letters/digits/_/- only).
 * Invalid declarations skipped. No on-play I/O. Single voice, gain 1/4.
 * mix accepts 44100Hz PCM16 mono/stereo and saturates at signed16 limits.
 * No channel/thread created. Audio output owner handles MV exclusion.
 */
#ifndef MELODY_SKIN_SOUND_H
#define MELODY_SKIN_SOUND_H
void skin_sound_set_root(const char *root);
int skin_sound_play(const char *id); /* 1 started; 0 unavailable/busy */
void skin_sound_stop(void);
void skin_sound_release(void); /* stops and frees cache, safe against mix */
/* Diagnostics reset on set_root/release. mixed_frames counts nonzero SFX
 * frames added before output; peak is SFX amplitude after gain, before music.
 * Scalar snapshots are atomic on PSP; error is a static reason, no paths. */
int skin_sound_loaded_count(void);
unsigned skin_sound_play_count(void);
unsigned skin_sound_mixed_frames(void);
int skin_sound_peak(void);
const char *skin_sound_error(void);
int skin_sound_active(void);
void skin_sound_mix(short *pcm, int frames, int channels, int sample_rate);
#endif
