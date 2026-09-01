

PINGUUS - PLUNDERPHONICS PAINTER
=================================================
<img width="1280" height="720" alt="smoke_model" src="https://github.com/user-attachments/assets/2617763d-2f04-49f8-b273-772beaaaf765" />
Pinguus is a musical "paint" app in C++ (raylib + miniaudio). You paint
colored cells on a grid; little creatures called NOTEYS crawl across it and,
each time they step on a painted cell, they trigger a sound pitch-shifted by
the cell's color. On the right, a live video COLLAGE plays the clip tied to
each note, sped up / reversed by the same color -- the classic YTPMV remix
look. A 4-channel TRACKER, scenes, effects and recording let you build a whole
song and export it as audio or video.

Everything is decoded fully into RAM up front so a notey can start playing a
clip INSTANTLY when it hits a painted cell, with no decode hitch at the worst
moment. Short clips are strongly recommended (see MEMORY NOTES below).

-------------------------------------------------
QUICK START
-------------------------------------------------
<img width="1280" height="720" alt="smoke_startup" src="https://github.com/user-attachments/assets/1b15709c-ed6d-4322-abe2-fd802296b959" />

On start you pick a grid size (or load a project). Then:
  * Click an empty CLIP slot (top-left number bar) to load a video, image
    or GIF from anywhere on your PC. Click an empty SMP slot to load an
    audio sample. You can also drag & drop files onto the window.
  * Pick a color (1-8), left-drag on the grid to paint cells.
  * Right-click the grid to drop a NOTEY. Watch/hear it play.
  * Press SPACE (or the PLAY/STOP button) to start/stop everything.
  * Press RecV to record the video collage + audio to an .mp4.

-------------------------------------------------
CONTENT: CLIPS vs SAMPLES (64 each, in pages of 16)
-------------------------------------------------
<img width="1280" height="720" alt="smoke_shot" src="https://github.com/user-attachments/assets/9ba2f2ef-99d2-4c31-a7f8-8fcda05bd5a7" />


* CLIP bar = up to 64 VISUAL slots, numbered 1..64. A clip can be a VIDEO
  (.mp4/.webm/.mov/.avi/.mkv/.mpg...), a still IMAGE (.png/.jpg/.bmp) or an
  animated GIF (.gif). Green number = loaded video/image.
* REPLACE THE VISUAL, KEEP THE SOUND (classic YTPMV move): load a video
  first, then right-click it and press "Replace visual (image/GIF)" -- the
  clip now SHOWS the image/GIF but still PLAYS the original video's audio.
  Same for samples: load the audio in a SMP slot, then right-click it and
  press "Set visual (image/GIF)" to give it a picture that appears in the
  collage when a notey plays it on the grid.
  A "Remove visual" button (top-right of the editor) undoes the override: a
  clip goes back to its original video, a sample back to audio-only.
* SMP bar = up to 64 AUDIO slots, labeled A1..D16. Samples are .wav/.mp3/
  .ogg/.flac.
* Use the "<" ">" buttons next to each bar to flip through pages of 16.
* HOVER any loaded slot to see a PREVIEW popup: a frame for clips/images/
  GIFs, a waveform for samples -- so you know what a slot holds at a glance.
* Click an EMPTY slot to choose: LOAD A FILE (from any folder), RECORD
  from your CAMERA + MIC (for a CLIP) / the MICROPHONE (for a SMP), or
  RECORD FROM PHONE (see PHONE AS CAMERA below). The recorder captures a few
  seconds (2/4/8/15 s, pickable) via ffmpeg and loads the result into the
  slot, saved so the project can reload it.
  PICK which camera and microphone to use in the DEVICES panel ("DEV" in the
  top strip -> PHONE / CAMERA / MIC). On Linux it lists the cameras
  (/dev/video*) and the PulseAudio sources; on Windows it lists the DirectShow
  device names, which differ on every PC, so it asks ffmpeg for them. Click
  the ones you want (saved in controls.cfg).
  Drag & drop also works: videos/images go to CLIP, audio goes to SMP.

<img width="1280" height="720" alt="smoke_editor" src="https://github.com/user-attachments/assets/563c5e24-88e5-4694-a8f6-c77421af2afa" />

-------------------------------------------------
KEYBOARD SUMMARY
-------------------------------------------------
  SPACE        play / stop everything
  Up / Down    tempo +/- 5 BPM
  1-9, 0       pick note (C..G#, A)
  [ / ]        octave down / up
  A (R rot)    arrow tool
  G            arpeggiator (cycles the chord shape)
  S (T time)   hold tool
  F (cycle)    fx tool
  P (id)       teleporter tool
  M            mute tool
  E            erase tool
  TAB          next slot in the active bar
  C / B        clear paint / clear noteys
  Ctrl+Z       undo      Ctrl+Shift+Z  redo
  NUMPAD       the 16 BEATBOX pads (works in every view, not just that tab)

-------------------------------------------------
MEMORY NOTES (read before loading long clips)
-------------------------------------------------
Video is pre-decoded to RAM as uncompressed RGB. A 10 s clip at 480x270 is
~116 MB. Clips longer than 30 s are automatically cut to their first 30 s on
load. Keep clips SHORT (a few seconds) and use the RES button and the trim
editor to stay within your PC's memory. GIFs are usually small; big still
images are downscaled automatically.
