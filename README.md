

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
PHONE AS CAMERA / MICROPHONE
-------------------------------------------------
<img width="1280" height="720" alt="smoke_phone" src="https://github.com/user-attachments/assets/5313e332-5765-4433-8b78-302e13a6b071" />

You can use your PHONE's camera and mic instead of a webcam. Open DEV and
pick the "PHONE / CAMERA / MIC" tab: it offers two routes, ordered by how
little you have to do.

HOW THE TWO DEVICES FIND EACH OTHER: two buttons at the top of the phone
column -- "Tailscale (anywhere)" and "Local Wi-Fi".

TAILSCALE (recommended, and where the panel opens)
  Tailscale (tailscale.com, free for personal use) puts your own devices on a
  small private network of their own. Install it on the PC and on the phone and
  sign in to BOTH with the same account; each gets a fixed address starting
  with 100 that only your other devices can reach, encrypted end to end.
  Then:
    * the phone can be ANYWHERE -- another building, another country -- and its
      camera still lands in a clip. You can film one side of the world from the
      other;
    * the address you show the phone is that private one, never the address
      your router gave you;
    * guest Wi-Fi, client isolation and hotspots that block broadcast stop
      being a problem, because none of that is involved any more.
  The panel checks three things (installed / this PC signed in / another device
  online) and each failing one comes with the exact command or page that fixes
  it, plus a button to copy or open it. Broadcast does not cross a tailnet, so
  in Pinguus Cam use "Type the address instead" once -- the phone remembers it.

  Nothing else changes: the same live preview, the same "Record from PHONE",
  the same MJPEG apps (the scan button becomes "Scan tailnet" and asks your few
  tailnet devices instead of sweeping 254 local addresses).

SHOW / HIDE ADDRESSES: every IP in this panel starts HIDDEN, drawn as
asterisks -- the panel gets opened while sharing a window or recording more
often than you would think. "Show addresses" in the title bar reveals them and
hides them again; it always starts hidden next time you open Pinguus. "Copy
address" and "Copy link" work whether they are visible or not, so you can send
the address to the phone without ever putting it on screen.

OUR OWN APP — "Pinguus Cam" (no ads)  [recommended]
  A tiny Android app that ships with this project. No ads, no trackers, no
  accounts, and the lowest latency of the two. Two steps, and you never
  type an IP address:
    1. Press "Share the app". Pinguus serves the APK over your Wi-Fi and
       shows a short address like http://192.168.1.42:45814 — open that in
       the phone's browser and tap DOWNLOAD THE APP. (Android warns about
       installing outside the Play Store; that happens with any app installed
       this way.) No cable, no cloud, nothing to copy by hand.
    2. Open the app and press SEARCH FOR PINGUUS. It finds this computer by
       itself and connects — if exactly one PC answers, automatically.
    3. Empty CLIP/SMP slot -> "Record from PHONE".
  The panel shows a green CONNECTED with the frame rate and a live preview.
  See android/README.txt (it also explains how to rebuild the APK yourself).

  If your network blocks broadcast (some guest Wi-Fi, some hotspots), the app
  falls back to scanning the local subnet; as a last resort there is a "Type
  the address instead" box.

  OVER TAILSCALE, SEARCH FOR PINGUUS CANNOT WORK — and that is not a bug you
  can configure away. A tailnet carries no broadcast traffic and gives every
  device a /32, so there is no subnet to sweep and nothing to shout at. The
  route is:
    1. Pick "Tailscale (anywhere)" in the panel and read the 100.x address it
       shows for this computer ("Copy address" puts it on the clipboard).
    2. In the app, open "Type the address instead", enter that address and
       press CONNECT TO THIS ADDRESS. The app pings the PC first and tells you
       the computer's name, so a typo says "no answer" instead of streaming
       into nowhere.
  The typed address wins over anything the app remembers, so you can move
  between Wi-Fi and Tailscale by editing that one field.

  RECORDING FROM THE PHONE ITSELF (no one has to touch the PC)
    Scroll down in the app to RECORD INTO PINGUUS. It shows the SAME slot rows
    the PC does — CLIP or SMP, sixteen at a time, with "<" and ">" to change
    page — and the PC keeps that list up to date several times a second, so a
    slot that fills up while you are looking goes green in front of you.

      1. Pick CLIP (camera + mic) or SMP (mic only).
      2. Pick the length: 2, 4, 8 or 15 seconds.
      3. Tap a free slot. That is it — the PC records those seconds from THIS
         phone and loads them into that slot.

    The app shows the countdown, then "Done — saved into <slot>". Slots that
    are already taken are drawn green and cannot be tapped, so you cannot
    overwrite somebody's work by accident.

    The PC can switch this off: DEV -> PHONE / CAMERA / MIC -> the tick box
    "let the phone record into slots by itself". It starts ON. Turn it off if
    you are sharing your screen or simply do not want your slots touched; the
    app then says so instead of failing silently.

  TWO OR MORE PHONES AT ONCE
    Every phone is told apart by where its packets come from, so several can be
    connected at the same time without their video getting mixed up. When there
    is more than one, the panel grows a small list — each phone by NAME, with
    its frame rate — and you click the one you want to see in the preview and
    record with the PC's own button.
    A recording ASKED FOR BY A PHONE always records THAT phone, whichever one
    the PC happens to be previewing. If two ask at once they are served in
    order, and whoever is waiting is told so rather than left staring at a
    button that appears to do nothing.

A THIRD-PARTY APP — IP Webcam (has ads)
  1. On the PHONE install "IP WEBCAM" (free, Play Store; DroidCam and other
     MJPEG apps work too). Open it and press "Start server". The phone must be
     on the same Wi-Fi as the PC (USB tethering also works).
  2. Press "Scan network". Pinguus sweeps the local network, finds the phone
     by itself and lists it with its resolution. CLICK it: you get a green
     "connected" and a LIVE preview. The choice is remembered, so next time
     just press "Connect".
  3. Click an empty CLIP/SMP slot -> "Record from PHONE".

ADVANCED WAY — the UDP script (for a PC webcam, or Termux)
  A small Python script (phone/pinguus_phone.py) streams a camera + mic to
  Pinguus over the same UDP protocol the Android app uses. Run it on any
  computer with a webcam: `pip install opencv-python sounddevice numpy`, then
  `python pinguus_phone.py <PC_IP> 45813`. Pinguus listens from startup, so
  there is nothing to switch on first.

If several are connected, Pinguus records from the MJPEG app first, then the
UDP/app stream.

-------------------------------------------------
PAINTING TOOLS (bottom palette)
-------------------------------------------------
<img width="1280" height="720" alt="smoke_slotchooser" src="https://github.com/user-attachments/assets/0008b598-d3e6-4952-a0b6-59594115feae" />

* PIANO NOTES (the 12 colored cells): the palette is a chromatic octave --
  C, C#, D, D#, E, F, F#, G, G#, A, A#, B -- so pitch works like real
  musical notes on a keyboard instead of a few fixed steps. Painting a cell
  with a note makes a notey play the selected CLIP/SMP at that note.
  Keys 1-9 pick C..G#, 0 picks A; or click a note. Each cell shows its
  note name and the octave it will paint (e.g. C0, C#0...).
* OCTAVE (the "OCT - 0 +" control in the CLIP row, or keys [ and ]):
  shifts the whole keyboard up/down by 12 semitones, so you can reach low
  and high notes. Each painted note keeps its octave even if you change
  the selector afterwards.
* TURN [A] (key R rotates): arrow cells that change a notey's direction
  (right/down/left/up).
* HOLD [S] (tap it again to cycle 0.5 / 1 / 2 / 4s / off): makes the notey
  stand still on that cell. Like FX, it goes ON TOP of whatever is already
  there instead of replacing it -- so a note keeps sounding while the notey
  waits. And it does NOT need a note underneath: HOLD on an EMPTY cell is a
  REST, the notey just waits there in silence for that long. That is how you
  write rhythms with gaps instead of a wall of notes.
* VOL [V] (cycles 100 / 90 / 80 ... 10%): sets the VOLUME of one note.
  Goes on top of a note you already painted, exactly like FX -- on an empty
  cell it does nothing. Shown as a small green bar on the cell's left edge,
  as tall as the level. Pick 100% to remove it.
* TIME [W] (cycles x1 / x1/4 / x1/2 / x3/4 / x1.5 / x2 / x4): SLOW MOTION or
  FAST MOTION for one note. Also goes on top of an existing note. It stretches
  the WHOLE note like tape: the video runs slower/faster AND the audio drops
  or rises in pitch with it. Shown as a small orange triangle pointing left
  (slow) or right (fast). Pick x1 to remove it.
* FX [F] (cycles REVERB / ECHO / REVERSE / CHORUS): drops that audio effect
  ONTO a cell that already has a note (echo = feedback delay; reverb = room;
  reverse = played backwards, which also reverses the video; chorus = a
  modulated short delay that thickens the sound). Pick the FX tool and click a
  note you already painted -- empty cells are left untouched, so you no longer
  have to pick a note color first. Works the same on the grid, the tracker,
  and the LINEAR view.
* ARP [G] (cycles maj / min / oct / up-dn / 5th / 7th): the ARPEGGIATOR.
  One click stamps a little run of note cells to the RIGHT spelling that
  chord, so a notey walking through plays the arpeggio. Pick a note + clip
  first; the octave applies too. A simple way to get melodic runs.
* PORT [P] (key P cycles id): TELEPORTER portals, Portal-style A -> B.
  Single-click to place: the FIRST click of an id drops the ENTRANCE (A,
  a filled disc), the SECOND drops the EXIT (B, a hollow ring). A notey
  that steps on A jumps to B (one way). Want another jump? Press P to pick
  a different id (1..8, each its own color) and place a new A/B pair.
* MUTE [M]: cuts the notey's sound (and its video) dead.
* ERASE [E]: erases cells; right-click erases a notey.

HOLD, VOL and TIME STACK. They are attributes of the cell, not cell types,
so one cell can be a note WITH an echo, at 40% volume, in slow motion, and
still hold the notey for 2 seconds. Hover the cell and the tooltip spells out
everything it carries. ERASE clears the cell and all of its attributes.

Grid mouse:
  * Left-drag ............ paint with the current tool (one stroke = one undo)
  * Right-click .......... drop a notey (hold Shift = it moves leftwards)
  * Middle-click ........ delete the notey under the cursor
  * Hover a cell ........ tooltip with what it does

Each notey is born with its own TEMPO multiplier (SPD selector: 1/4..x4)
over the global BPM, so slow and fast noteys can share one grid.

MUTE / VOLUME / PLAY-STOP PER NOTEY: click the NOTEYS tab (top, next to
CANVAS/TRACKER) to open a list panel on the left. Each notey has a VOLUME
slider (0-150%), a PLAY/STOP button and an ON/MUTED toggle:
  * MUTED (ON/MUTE): a muted notey keeps MOVING but does NOT trigger its
    clip/sample (drawn as a grey slashed ring on the grid).
  * STOP/PLAY: stops that notey completely -- it FREEZES in place (no moving,
    no sound), drawn as a blue square. Press PLAY to release it. Handy to pause
    one notey while the rest keep going.
"Mute all" / "Enable all" toggle every notey's mute at once.

MASTER VOLUME: the MASTER slider in the top strip sets the overall output
level (0-150%). Both the master and the per-notey volumes are applied to the
sound BEFORE recording, so the exported .wav / .mp4 match exactly what you
hear. Volumes are saved with the project.

UNDO / REDO: Ctrl+Z / Ctrl+Shift+Z (or the Undo/Redo buttons). 40 steps.

-------------------------------------------------
MELODY: a palette cell that you sing into
-------------------------------------------------
<img width="1280" height="720" alt="smoke_melody" src="https://github.com/user-attachments/assets/b0d51061-12f3-4d09-8758-449fa9ca6df6" />

MEL is a palette cell like ARP: it stamps SEVERAL notes at once. The
difference is where the notes come from -- ARP has a fixed list of chord
shapes, MEL has notes taken from LISTENING to a sound. Sing, hum or whistle a
melody and stamp it wherever you want, played by whichever clip or sample you
have selected.

There is a BANK OF 8 melodies, exactly like the arpeggio has its chord shapes:
  * Click MEL in the palette to select it; click it again to cycle through the
    eight. The cell shows which one is loaded and how many notes it holds.
  * If the one you land on is EMPTY, the bank editor opens by itself -- that is
    what you need at that moment.
  * RIGHT-CLICK the MEL cell any time to open the bank editor.
  * Keyboard: N selects it and cycles.

Once a melody is loaded, click the canvas and it stamps to the right, one cell
per sixteenth, exactly like the arpeggio. Hovering shows a ghost of the whole
melody first, so you can see where it lands and whether it runs off the edge.
A notey walking through then plays it.

IN THE BANK EDITOR
  1. Pick which of the eight you are editing (the row at the top).
  2. Pick the SOURCE slot -- any CLIP or SMP with audio. The button on the
     right records straight from the microphone into it, which is the short
     path: hum, and it is there.
  3. ANALYSE. The piano roll shows what was detected, brighter the louder you
     sang it. Four controls tune the listening:
       SENSITIVITY  how loud something has to be to count as sound at all.
       TOLERANCE    how forgiving the pitch detector is. RAISE THIS FIRST if a
                    recording comes out with only a handful of notes -- a real
                    microphone take is never as clean as the detector would
                    like, and this is what recovers it.
       MIN NOTE     shorter than this is a click, not a note.
       SPLIT AT     how far the pitch must move to start a NEW note. Low values
                    chop a vibrato into pieces.
  4. CELL sets how much time ONE canvas cell is worth: 1/16, 1/8 or 1/4. This
     is what decides whether a long melody FITS. Six seconds of humming is
     about 45 cells at 1/16 and fits in no grid at all; at 1/4 it is twelve and
     fits easily, sounding the same but stretched over bigger cells. FIT picks
     the finest setting that still fits, and the line underneath tells you how
     many cells it takes against how many the grid has.
  5. SAVE into the chosen slot. Analysing does NOT overwrite by itself, so you
     can keep tweaking until it sounds right without destroying what is there.

Silence before you start singing is trimmed automatically -- a banked melody is
a piece you stamp, so where you happened to come in during the recording means
nothing, and leaving it in wasted half the grid on empty cells. Short gaps
INSIDE the singing (a consonant, a breath) no longer break a note in two.
  * CLEAR empties a slot. "Write to LINEAR" and "Write to TRACKER ch1" are
    still there for a long melody that does not fit across the canvas.

The eight melodies are saved with the project.

It follows ONE melody at a time -- a single line, not chords. That is a real
limit of how it works, not an oversight: it finds the period of the waveform,
and two notes at once do not have one.

The same thing is in the Android app: MEL in the tool column, the same bank of
eight, and the same editor. On a phone it is even more direct, since the
microphone is already in your hand.

-------------------------------------------------
TRACKER (tab, top-left)
-------------------------------------------------
<img width="1280" height="720" alt="smoke_tracker" src="https://github.com/user-attachments/assets/5b70c585-3f76-4de6-9cff-c3a4ffb3c66a" />

A 4-channel x 32-row step sequencer for rhythms with your SAMPLES.
  * Left-click a cell: place a note using the selected SMP and current color
    (pitch). If the FX tool is active, the note carries that effect.
  * Right-click: erase.
  * "Clear" empties the pattern.
  * "MIDI": import a .mid file (or drag a .mid onto the window). Pinguus
    reads the notes, maps the MIDI channels (in order of appearance) to the
    4 tracker channels, snaps each note to the nearest palette pitch, and
    takes the tempo from the file. If you have samples in A1..A4, each
    channel uses its own; otherwise the selected sample.

The tracker plays TOGETHER with the canvas: PLAY/STOP (SPACE) starts and
stops everything at once, looped at the global BPM. Saved with the project.

-------------------------------------------------
LINEAR (tab, top-left) — Mario Paint Composer style
-------------------------------------------------
<img width="1280" height="720" alt="smoke_linear" src="https://github.com/user-attachments/assets/c0fbf2f4-1c73-40c1-9e36-8392fda51c84" />
Works like the CANVAS, but plays back in a straight line. It's a grid of time
COLUMNS (left-to-right, 16th steps) and LANE ROWS you can add or remove. A
yellow PLAYHEAD sweeps across and plays each column as it passes.
  * Left-click a cell: place the currently selected CLIP or SMP at the chosen
    NOTE COLOR -- exactly like painting on the canvas (pick a note color at the
    bottom + a CLIP/SMP, then click). Each cell SHOWS its clip/sample and note
    so you can see what it is. Several cells in the same column (different
    lanes) play together as a CHORD -- you can mix clips and samples freely.
  * Right-click (or the ERASE tool): erase a cell.
  * FX tool: drops an effect (reverb/echo/reverse/chorus) ONTO a cell that
    already has a note -- it does nothing on empty cells.
  * Transport row (top): "Clear" empties it; "LOOP" toggles looping vs a
    single pass; "LEN -/+" sets the loop length in columns (the visible area
    stretches to fit, so shorter loops = bigger, easier cells); "ROWS -/+"
    adds/removes lanes. Tempo is the global BPM.

Each ROW behaves like its own notey: it has ONE video that RE-TRIGGERS (restarts
in place) every time a note plays on that row, instead of stacking up more and
more videos. So a busy row shows one lively clip, not a pile of them.

Like the tracker, LINEAR plays TOGETHER with the canvas and tracker: PLAY/STOP
(SPACE) starts and stops everything at once. A linear cell that uses a video
clip drives the collage on the right too, sped/reversed by its note. Saved with
the project.

-------------------------------------------------
BEATBOX (tab, top-left) — the sampler
-------------------------------------------------
<img width="1280" height="720" alt="smoke_beatbox" src="https://github.com/user-attachments/assets/de622749-dd30-4c4d-98b9-30d85e319ade" />

A pad sampler in the spirit of a Roland SP-555: 16 big pads, played with the
NUMPAD, a gamepad, a MIDI pad controller or the mouse, plus a live effects
section and a pattern recorder. It uses the SAME clips and samples as the rest
of Pinguus -- a pad just points at a CLIP or SMP slot, it does not copy audio.

  * 4 BANKS (A-D) x 16 pads = 64 pads. Switching banks does NOT cut what is
    already sounding, so a loop keeps running while you play another bank.
  * Right-click a pad to assign the CLIP/SMP currently selected in the bars at
    the bottom. That's the quick way to build a kit: pick a sound once, then
    sprinkle it over the pads.
  * Left-click a pad to select AND play it. The inspector on the right sets:
      PITCH   -24..+24 semitones.
      VOL     per-pad level.
      MODE    ONE-SHOT (plays to the end) / GATE (only while held) /
              LOOP (press once to start, again to stop).
      CHOKE   groups 1-4: a pad cuts the others in its group, which is how an
              open hi-hat shuts up the moment the closed one hits.
      FX      a fixed reverb/echo/reverse/chorus for that pad.

HOW YOU PLAY IT
  * NUMPAD -- the numeric keypad already IS a 4x4 grid, so it is the default:
        7 8 9 -        pads  1  2  3  4
        4 5 6 +              5  6  7  8
        1 2 3 Ent            9 10 11 12
        0 . / *             13 14 15 16
    It works in EVERY view, not just this one: you can drum with your right
    hand while painting the canvas with the mouse.
  * GAMEPAD -- in this tab the pad IS the drum kit: D-pad, the four face
    buttons and the four triggers are pads 1-12. The right STICK is the XY
    effects box, R3 toggles LATCH, L3 changes bank, Start plays/stops the
    pattern and Select arms recording.
  * MIDI -- notes 36-51 (C1 upwards) are the 16 pads of the visible bank,
    which is what MPC/Maschine/Launchpad-style controllers send with no setup
    at all. Velocity goes straight into the volume. CC74/CC71 (cutoff and
    resonance on most controllers) drive the two effect knobs, and CC64 (the
    sustain pedal) switches the effect on.

LIVE FX -- the effects section
  One box, two knobs (X and Y), which is exactly what one finger, one thumb
  stick or two controller knobs can move at once. FILTER, DELAY, CRUSH,
  REVERB, FLANGER and SLICER (this one chops in time with the BPM). Without
  LATCH the effect is momentary -- it only lasts while you hold the box, which
  is the "drop the filter for two bars" gesture. With LATCH it stays on.

PATTERN
  A loop of 4 to 64 sixteenth-steps at the global BPM, with its OWN transport:
  it runs whether or not the canvas is playing.
  * REC records what you play. The hit is written by the AUDIO thread with its
    own clock, so what gets recorded is what you heard, not what the drawing
    thread noticed a frame later.
  * QUANT snaps to 1/32, 1/16, 1/8 or 1/4 -- or OFF, which keeps the hit where
    your finger actually landed.
  * Click a step in the lane to write or remove a hit by hand; right-click
    clears it. CLR PAD wipes just the selected pad and leaves the rest.
  * REC OUT records the output (video+audio or audio only) without leaving the
    tab -- playing and recording at the same time is the whole point.

Pads with a video clip drive the collage on the right, re-triggered from the
start on every hit. The kit and the pattern are saved with the project.

-------------------------------------------------
SCENES / SONG MODE
-------------------------------------------------


The SCENE bar (right panel) holds multiple canvases (up to 10). Each scene
keeps its own painting AND its own noteys.
  * Click a scene number to switch to it.
  * "+" adds an empty canvas.
  * TOOLS row: Dup (duplicate), Del (delete), "<" ">" (reorder).
  * Right-click a scene number to type its exact duration in seconds.
  * SONG button: chain all scenes that have a duration, automatically, in a
    loop -- that's how you arrange a full song from several canvases.

-------------------------------------------------
TRIM / EDIT CLIPS AND SAMPLES
-------------------------------------------------
<img width="1280" height="720" alt="smoke_editor" src="https://github.com/user-attachments/assets/d72b800e-2f68-4c44-bbd8-8e72835b79a7" />

Right-click a CLIP slot to open the VIDEO trim editor: a preview plus a
timeline with green (start) and red (end) handles to keep just a piece of the
clip. Trimming is NON-DESTRUCTIVE: the full clip stays loaded and playback
just uses the range, so you can re-open the editor to widen/move the trim, or
press "Reset trim" to restore the whole clip. It also has the per-clip visual
FX (see below).

Right-click a SMP slot to open the SAMPLE trim editor: it shows the sample's
WAVEFORM with the same green/red handles. Also non-destructive, with "Reset
trim".

Both editors have a PLAY / STOP button (top-left of the preview) that plays
the clip's/sample's audio, looped over the selected range, so you can HEAR
what you're trimming. In the video editor the frames follow the sound.

PURE DATA / PLUGDATA EFFECTS (per slot): each editor has a "Pd effect"
button. Load a .pd patch and every note that plays that slot runs through
it (adc~ -> your patch -> dac~) as a real-time insert effect. Design the
patches in Pure Data (puredata.info) or PlugData (plugdata.org) -- both save
the same .pd format, so either works. A few ready-made examples live in
assets/pd/ (lowpass, ringmod, echo). "clear" removes the effect. The patch
choice is saved with the project.

GLSL VIDEO SHADERS (per clip): the CLIP trim editor has a "GLSL fx" button
(top-right of the preview). Load a fragment shader (.fs/.glsl) you wrote
yourself and every time that clip appears in the collage its image runs through
your shader -- the video equivalent of the Pd effects. Your shader gets the
usual raylib inputs (texture0, fragTexCoord, fragColor, colDiffuse) plus two
extras Pinguus updates every frame: "uniform float time" (seconds) and
"uniform vec2 resolution" (the clip size). The effect also shows live in the
editor preview. Ready-made examples live in assets/glsl/ (wave.fs, rgbshift.fs)
-- open them to see the template. "clear" removes it; the choice is saved with
the project. (Use "#version 330" on desktop.)

Both editors also have a "Replace/Set visual (image/GIF)" button (see
CONTENT above) to swap the visual while keeping the audio.

"Apply" cuts the media in RAM permanently -- re-drop the file to undo.

EMPTY SLOT: both editors have an "Empty slot" button (bottom-left) that unloads
everything in that slot -- media, trim, clip-FX, Pd patch and GLSL shader -- so
it goes back to being a blank slot you can load or record something new into.

-------------------------------------------------
CLIP VISUAL FX (in the clip trim editor)
-------------------------------------------------
<img width="1280" height="720" alt="smoke_place" src="https://github.com/user-attachments/assets/deba1d90-6349-4fa5-82ab-d490868be142" />

* FLIP   -- mirror that alternates every note (bounce feel)
* ZOOM   -- grows from small to big on each note
* SPIN   -- keeps spinning while playing
* size   -- base scale 0.25x .. 4x
* layer  -- draw order 1..8 (1 = back, 8 = front): who covers whom

WHERE THE CLIP GOES -- three mutually exclusive choices (turning one on turns
the others off, because they all decide the same thing):
* CENTER -- pinned big in the middle
* MOVE   -- travels from point A to B on each note; drag on the preview to
            draw the A -> B path
* PLACE  -- YOU decide. The preview turns into the actual export frame, with a
            rule-of-thirds grid and the clip drawn exactly as it will come out.
            Drag the clip where you want it; the mouse wheel rotates it (hold
            SHIFT for one-degree steps). The position is stored as a fraction
            of the frame, so it survives switching between 16:9 and 9:16.
* with none of the three, notes scatter the clip around as they always did.

* ROTATION -- a FIXED angle, separate from SPIN. You can leave a clip tilted
              15 degrees and still have it spinning: the two angles add up.

TRANSPARENCY AND BLENDING
* OPACITY -- 0 to 100%. It multiplies with whatever alpha a PNG or GIF already
             had, so a transparent image stays transparent.
* BLEND   -- how the layer combines with what is underneath:
      NORMAL    the layer simply covers what is below
      MULTIPLY  darkens: white vanishes, black stays
      SCREEN    lightens: black vanishes, white stays
      ADD       adds light, good for flashes and glows
      SUBTRACT  removes light from what is below
  There are five and not twenty on purpose: these are the ones that come out
  exact using the blend factors the graphics card already has, with no extra
  render pass per layer -- which is why they also work on the phone. The modes
  that need to READ the background (overlay, soft light, hue...) would cost a
  render pass for every one of the eight layers.

* "Reset look" puts every visual setting of that clip back to default, keeping
  its layer number.

-------------------------------------------------
ANALOG VIDEO (VHS button, top bar)
-------------------------------------------------
<img width="1280" height="720" alt="smoke_vhs" src="https://github.com/user-attachments/assets/644b9df4-0421-44f5-abcf-67141c4634ce" />

An NTSC / VHS effect applied to the FINAL MIX -- the whole collage, after all
the layers are composed and just before the pixels go out to the recording or
to the LIVE window. It runs on the final mix and not per clip on purpose: what
makes tape look like tape is the whole SIGNAL being degraded, not each clip
carrying its own filter. What you see in the preview on the right is exactly
what gets recorded.

Fourteen sliders: composite / luma / chroma noise, snow, chroma delay (colour
smearing off the edges), chroma phase (hue wobbling line to line), ringing,
sharpening, edge wave and its speed, head switching (the torn band at the very
bottom of a tape), chroma loss, scanlines and tape speed blur. Three starting
points: VHS, BROADCAST and RUINED.

* "Load ntsc-rs .json...": ntsc-rs (github.com/valadaptive/ntsc-rs) is the
  reference tool for this look, and its presets are plain JSON, so Pinguus
  reads them. It reports how many settings it translated and how many had no
  equivalent here.

  Be clear about what this is: a TRANSLATION, not an emulation. ntsc-rs is a
  Rust library; linking it would mean everyone who compiles Pinguus needs a
  Rust toolchain plus cross-compilation to Windows and arm64, when right now
  one g++ and a one-click script are enough. So the effect is reimplemented
  here in GLSL, and a loaded preset lands in the same look -- not on the same
  pixels.

The effect is saved with the project. The VHS button turns orange while it is
active, because it changes everything you record and leaving it on by accident
is expensive.

-------------------------------------------------
VIDEO MODE, LIVE OUTPUT, RECORDING
-------------------------------------------------
* VIDEO MODE ("16:9 normal" / "9:16 vertical"): switch the export canvas
  between landscape (1280x720, YouTube) and portrait (720x1280, reels/
  shorts/TikTok). The preview and the RecV file use the chosen format.
* LIVE: opens a SECOND window showing ONLY the video you are making (the
  collage), no UI. Drag it to another monitor or a projector for a live show
  while you keep working in the main window. CLICK the LIVE window to go
  fullscreen; click again to return to a window. The image is shared in real
  time (30 fps).
* RecA / RecV: record & export audio (.wav) / collage video (.mp4, H.264 +
  AAC). When you press either, a dialog asks whether to start from START
  (resets noteys to their spawn points and the tracker to row 0 for a clean
  take) or from HERE (record exactly as it is playing now). RecV needs ffmpeg.
* WHERE THE TAKE GOES. By default it lands in temp/ and no dialog interrupts
  you -- that is what you want while you are hunting for the good take. But
  when the good one arrives you need to get it out of there, so:
    - a SAVE AS button appears next to RecA/RecV as soon as there is a take.
      It copies the last one wherever you choose (the original stays in temp/,
      so a failed copy cannot lose the recording);
    - and the record dialog has a switch to ASK EVERY TIME, if you would
      rather be prompted at the end of each take. It is remembered.
  See "THE temp/ FOLDER" below.
* RES 480/360/240: max resolution of the NEXT videos you load. Lower =
  smaller = less RAM, so you can fit more clips. The MB counter in the
  right-panel header shows how much RAM the bank is using.

-------------------------------------------------
THE temp/ FOLDER AND CLOSING THE PROGRAM
-------------------------------------------------
Everything Pinguus RECORDS goes into a folder called "temp/", next to the
program: the RecA/RecV takes, and anything you capture from a camera, a
microphone or a phone into a slot. Nothing asks you where to put it -- the
point is that you can keep recording takes without a save dialog in the way,
and decide later which ones were any good.

When you CLOSE the window, Pinguus asks what to do with the session. Three
answers, because saving the project and keeping the material are two
different decisions:

  * YES - save project and keep the clips
      You pick where the .smt goes; the clips stay in temp/.
  * Keep only the clips - no project file
      For when you just wanted the material, not the arrangement.
  * NO - delete this session's clips and quit
      Throws away only what THIS session recorded.

Clips from earlier sessions that are still sitting in temp/ are never
touched, whichever answer you pick. If the canvas is empty and you recorded
nothing, there is nothing to decide and the program just closes.

-------------------------------------------------
SAVE / LOAD PROJECTS
-------------------------------------------------
Save / Load buttons write/read a ".smt" project (a readable text file) with
everything: BPM, grid size, video mode, scenes, cells (with their hold,
volume and speed attributes), noteys with their tempo, the tracker pattern,
per-clip trims and FX, and which file feeds each slot. Drag a .smt onto the
window to load it. Clips are re-loaded from their original files (not copied
inside the project), so don't move the files.

The format is version 9. Projects saved by OLDER versions still open: a cell
that used to be a whole "HOLD" cell becomes a note with a hold attribute,
which sounds exactly the same. The ANDROID app writes and reads this very
same format, so a project made on the phone opens on the PC and back.

-------------------------------------------------
MIDI KEYBOARD / CONTROLLER
-------------------------------------------------
Plug in a MIDI keyboard/controller before launching and Pinguus opens it
automatically. It plays LIVE, like an instrument (works even while stopped):
  * Press a key -> plays the currently selected CLIP/SMP at that key's pitch,
    and shows its video in the collage. (If the FX tool is active, the live
    note carries that effect.)
  * Transport: your controller's START / STOP buttons play/stop everything.
  * Mod wheel (CC1) -> master volume.
A small "MIDI" readout appears in the top strip (lights up with the note you
play). No device? Nothing changes -- it's simply ignored.

NOT WORKING? Click "DEV" in the top strip to open the DEVICES panel. It lists
every MIDI input port -- click your keyboard's port to use it (Pinguus skips
the "Midi Through" port automatically, which is the usual reason nothing comes
through). The panel shows the last bytes received, so you can tell whether the
keyboard is actually sending. "Rescan" re-checks after plugging one in. The
same panel also lets you pick the CAMERA / MIC for recording and choose a UI
THEME (see below).

-------------------------------------------------
3D MODELS (.glb / .vrm) triggered by noteys
-------------------------------------------------
<img width="1280" height="720" alt="smoke_modeleditor" src="https://github.com/user-attachments/assets/4b4875ca-2395-4b1f-9ec3-064c9e0d8517" />

You can import 3D models with their textures and animations and have NOTEYS
trigger the animations. The MODELS 3D bar is in the right panel (under the video
preview):
  * Click an empty model slot (up to 5) to load a .glb/.gltf/.vrm. Its
    animations appear in the ANIM row when the model is selected. Click a model
    to select it; right-click to remove it.
  * Click an animation number (0,1,2...) to make it the active "content" (like
    picking a CLIP/SMP). Then paint a note on the CANVAS: a notey that steps on
    it plays THAT animation, rendered in 3D in the collage. Each pass re-triggers
    the animation (like a video). Choose different animations for different notes.
  * RIGHT-CLICK a loaded model to open its EDITOR: a 3D preview plus sliders for
    POSITION (X/Y/Z), ROTATION (X/Y/Z) and SCALE, a preview-animation picker, and
    the global LIGHTING controls (ambient, intensity, light direction and colour
    -- one directional light + ambient illuminate every model). "Reset transform"
    and "Remove model" are there too. The transform is saved with the project.
  * ANIMATION TRIM / LOOP: pick an animation in the preview-anim picker, then use
    the "ANIM TRIM (loop range)" Start/End sliders under it to choose which frames
    play -- works for skeletal .glb clips, humanoid, and imported .vrma. Toggle
    "Loop ON" to have that range loop continuously while the notey triggers it
    (OFF = play the range once per trigger); "Full range" resets it. The preview
    loops exactly the range you set, and it's saved with the project.
  * .GLB / .GLTF is fully supported (mesh, textures, skeletal animation). .VRM
    now imports too: a .vrm IS a binary glTF, so its mesh + textures load (the
    VRM-specific parts -- humanoid retarget, expressions/blend-shapes, spring
    bones, MToon toon-shading -- are not applied, so it renders as a normal lit
    model). Note: very high-poly meshes may show artifacts (the loader uses 16-bit
    indices); VRoid avatars split into many small meshes and usually load fine.
  * HUMANOID VRM ANIMATION: when a .vrm (or any model) has a humanoid rig,
    Pinguus detects its bones (VRoid J_Bip_*, Unity or Mixamo naming) and gives
    it 6 built-in SKELETAL animations that move the actual character -- Idle,
    Wave, Dance, Nod, March, Cheer (marked "h"). So your anime avatar waves,
    dances, marches etc. when a notey triggers it -- no external files needed.
  * TOON SHADING: the model editor has a "Toon shading" toggle for a cel-shaded
    anime look (banded light + rim), great for VRM characters.
  * PROCEDURAL (transform) ANIMATIONS: a model with NO skeleton and NO humanoid
    rig (a static .glb) gets 6 TRANSFORM animations instead -- Spin, Bounce,
    Pulse, Sway, Hop, Wobble (marked "*").
  * IMPORT .VRMA / .BVH: in a humanoid model's editor there's an
    "Import anim (.vrma/.bvh)" button. The file is retargeted onto your avatar's
    bones and added to the ANIM row -- real motion (dances, gestures, etc.) on
    your character, triggerable by noteys. Saved with the project.
      - .VRMA is the standard VRM animation format (e.g. the official VRMA
        MotionPack). These are authored in the VRM 1.0 convention (facing +Z);
        Pinguus auto-corrects them for VRM 0.x avatars (which face -Z, e.g.
        Koikatsu/UniVRM exports like Hexie & Silvia) so the motion no longer
        plays mirrored/backwards on those models.
      - .BVH is what motion capture actually speaks: Mixamo, the free CMU
        library, Rokoko, Perception Neuron, Blender and phone mocap apps all
        export it, so this is where you will find animations in practice. Only
        the rotations are used -- a BVH also records where the hips walk across
        the floor, but the avatar is drawn at a fixed spot in the collage, so
        honouring that would just carry the character out of frame. Bone lengths
        are ignored: your avatar keeps its own proportions.
        Mocap files disagree about which way the character faces, so some arrive
        MIRRORED (the avatar waves with the wrong arm). If that happens, press
        "Flip facing 180" under the import button -- it belongs to that one
        animation and is saved with the project. An example to try right away:
        assets/models/anims/wave.bvh.
  * For other skeletal animation, you can also bake clips into a .GLB in Blender
    (VRM add-on) or Unity (UniVRM) and import that -- its clips show in the ANIM
    row too.
  * A ready example lives in assets/models/ (robot.glb, 14 animations), auto-
    loaded at startup. Models (and their transforms) are saved with the project.

-------------------------------------------------
UI THEME (colors)
-------------------------------------------------
Open the DEVICES panel ("DEV") and pick a theme at the bottom (Midnight, Slate,
Grape, Ember, Forest). It recolors the app's background, panels, buttons and
accents; your choice is saved (controls.cfg) and restored next time. Pick
"Custom" to build your OWN: an editor appears with R/G/B sliders for each part
of the UI (Background, Panels, Buttons, Button hover, Borders, Accent).
"Reset colors" (next to the editor's title) puts the six back to the defaults
-- useful because it is easy to drag a custom theme somewhere unreadable, and
white text on a white background hides the very sliders you would need to fix
it.

-------------------------------------------------
GAMEPAD (any controller)
-------------------------------------------------
Plug in ANY game controller and a "GP:" indicator appears in the top strip. The
START button switches between two modes:
  * NOTEYS mode: left stick / D-pad move a cursor on the grid.
      - RIGHT stick: push left/right to flip through CLIP/SMP slots, up/down to
        switch between the CLIP bar and the SMP bar.
      - LB / RB: change the NOTE (pitch) you'll place.
      - A = place a note   B = drop a notey   X = erase note+notey   Y = play/stop
      - SELECT = "Controllable Notey": your cursor turns into a notey (an orange
        circle) and, as you move it around, it TRIGGERS whatever notes it passes
        over -- like driving a notey by hand. Press SELECT again to turn it off.
  * UI mode: A = play/stop, D-pad up/down = tempo, D-pad left/right = switch
    view (CANVAS/TRACKER/LINEAR), LT/RT = octave down/up.

REMAP: open the DEVICES panel ("DEV"), and next to each action press "Set", then
press the button you want on the pad. Your mapping is saved (controls.cfg) and
works for any controller. "Reset defaults" restores the standard layout.

NOT DETECTED? The DEVICES panel shows whether a controller is seen and lights up
the buttons as you press them. Some pads need X-INPUT mode (e.g. 8BitDo: hold
START+X while turning it on, then reconnect) to be seen as a gamepad.

-------------------------------------------------
MODS (Lua scripts) — "MODS" button, top strip
-------------------------------------------------
You can write little programs in Lua that react to and drive the show --
generative melodies, notey "rain", pattern randomizers, etc. Click "MODS" to
open the panel: "Load .lua" imports a script, "Reload all" re-runs them (edit a
file and reload to see changes live), and the panel lists the loaded mods plus
a cheat-sheet of the API. Any .lua files in a "mods/" folder next to the app are
loaded automatically at startup.

In a downloaded package, mods/ starts EMPTY: a mod can paint and play notes on
its own, so none are switched on for you. Three examples sit in examples/mods/
(rain.lua drops noteys from the top; linear_arp.lua rewrites the LINEAR pattern
each loop; cells.lua). Load one from the MODS panel to try it, or copy it into
mods/ to have it every time. The same applies to assets/: it starts empty, and
examples/ holds a clip, a GIF, a sample, the robot model, shaders and Pd
effects that you import yourself.

A mod registers a function that runs every frame:
    pinguus.on_update(function(dt)
        if pinguus.playing() then
            pinguus.spawn(math.random(0, pinguus.grid_w()-1), 0, 1, -1, 1.0)
        end
    end)
It can spawn/clear noteys, paint/erase cells, play live notes, fill the LINEAR
view, set the BPM, and read the grid size, BPM, playhead and the live noteys.
(slot -1 = the currently selected CLIP/SMP.) Mods run on the main thread, so
they're safe -- a broken script is reported, it won't crash the app. Full API
is listed in the MODS panel and in IMPLEMENTATION.txt.

CUSTOM CELLS: a mod can also add new CELL types you paint on the grid (like the
built-in arrow/hold/portal). A script calls, e.g.:
    pinguus.register_cell{ name="hold3", glyph="H3", behavior="hold", seconds=3 }
    pinguus.register_cell{ name="jump",  glyph="J", behavior="teleport", dx=6 }
behavior is "turn" (dir 0..3), "hold" (seconds), "mute", or "teleport" (dx,dy).
They show up under "Custom cells" in the MODS panel -- click one, then paint it
on the CANVAS. These run with exact per-step timing (they map to the built-in
behaviors). A cell can also CHAIN several behaviors that all fire when a notey
steps on it, by passing an "actions" list:
    pinguus.register_cell{ name="combo", glyph="C*", actions = {
        { behavior="turn", dir=1 }, { behavior="hold", seconds=1 },
        { behavior="fx", effect="echo" }, { behavior="note", note=0 } } }
(behaviors: turn/hold/mute/teleport/fx/note). See mods/cells.lua.

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
