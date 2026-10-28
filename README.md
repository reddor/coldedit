# ColdEdit

A small native wave editor built around a smooth timeline. C++20, Dear ImGui,
GLFW, OpenGL 3.3, and miniaudio. Available for Windows x64, Linux x64, and the web.

**[Open the web editor](https://sqrctl.de/ce/)** ·
**[Download releases](https://github.com/reddor/coldedit/releases)** ·
[Build status](https://github.com/reddor/coldedit/actions/workflows/build.yml)

## Run

Open `dist/ColdEdit/coldedit.exe` after packaging, or `build/coldedit.exe` after
building. Download `ColdEdit-win64.zip` from GitHub Releases and extract it to
run the packaged Windows app. There is no installer. The Windows release statically links its C/C++
runtime and its libraries; no separate runtime DLLs are needed.

On Linux, extract `ColdEdit-linux-x64.tar.gz` and run `./ColdEdit-linux/coldedit`.
The GitHub build targets Ubuntu 22.04 or newer on x86-64, with OpenGL 3.3 and
X11 (or XWayland). It uses the system's C/C++, graphics and audio runtime
libraries. Open/export uses a path-entry dialog; drag-and-drop also works.

Drop a WAV, FLAC or MP3 onto the window, or click **ColdEdit > Open audio**.
The editor starts empty until you open or record audio.

A WebAssembly build is also available in `dist/Web` and `dist/ColdEdit-web.zip`.
It runs locally in your browser, with a **64 MiB input limit** and a
**128 MiB default shared audio budget** (optionally 256 or 512 MiB) and **128 MiB export limit**, and links to GitHub Releases for native downloads for
larger recordings. See [WEB.md](WEB.md) for running, building, hosting, and
browser-specific memory behavior. Smaller screens use a compact toolbar and a
**Tools** panel, with touch selection and zoom buttons.

```powershell
.\build\coldedit.exe 'C:\Audio\recording.wav'
```

## What works

- Microphone recording on desktop, plus microphone or screen-sharing audio in
  the browser. Takes open as new unsaved documents for editing and export.
- Mono and stereo RIFF/WAVE: 8/16/24/32-bit PCM, 32-bit float, standard PCM/float
  extensible headers with matching valid and container bit depths.
- Mono/stereo FLAC import and 16/24-bit FLAC export using libFLAC 1.5.0, plus
  MP3 import using dr_mp3. Original sample rates are preserved; MP3 gapless
  delay/padding metadata is honored when present. LAME 3.100 exports MP3 using
  high-quality VBR (V2), 192 kbps CBR, or 320 kbps CBR, including gapless metadata.
  Compressed imports run on a cancellable worker. Native decoding streams to a
  temporary mapped float PCM file (including recordings over 4 GiB decoded).
  The browser enforces its shared audio budget against both declared duration
  and actual decoded output.
- Pointer-anchored animated zoom, continuous wheel/trackpad scrolling,
  middle-button panning, full-file navigator, selection autoscroll, exact
  sample-range entry, and connected samples/dots at close zoom.
- Automatic high-DPI layout and crisp font rendering, including live scale
  changes when moving between monitors. Waveforms use physical-pixel detail.
- Adjustable selection boundaries: drag either edge or its ruler handle.
  The opposite edge stays fixed; crossing it and autoscrolling are supported.
- Drag inside a selection to move its range without moving audio. Cut and Delete
  retain the exact range, including any portion beyond the new audio end.
- Paste into an active selection adds the clipboard signal at its start, stopping
  at the shorter of the clipboard or selection. It neither repeats nor stretches.
  The remaining selection stays unchanged; mixing beyond EOF extends the audio.
  Paste replace is separate; paste without a selection inserts at the cursor.
- **Edit > Paste/Insert from file...** mixes a WAV, FLAC or MP3 into the selection,
  or inserts it at the cursor when nothing is selected, in one undo step.
  The file must match the document's sample rate and channel count; the clipboard is preserved.
- Play, pause, seek, loop the selection or file, follow playback, output meters,
  monitor gain, and independent vertical waveform scale.
- Cut, copy, paste, delete, trim, silence, reverse, gain, normalize to -1 dBFS,
  and 128 undo steps. Clipboard audio can cross documents when the channel count
  and sample rate match.
- Fade in/out over the selection, with Linear or Smooth curves and linked
  channels. Fades remain intact through copy, trim, reverse, and undo/redo.
- Insert silence at the cursor using seconds or an exact sample count. Audio
  after the cursor moves right; the inserted interval becomes the selection.
- Save selection as WAV, FLAC or MP3 directly, without trimming the document or changing history.
- Optional zero-crossing snapping (5 ms search) and 1–10 ms Smooth edge fades.
  Automatic fades cover cut/delete/insertion or replacement paste/trim/insert
  silence in one undo step. Additive paste keeps the original signal intact.
- Extract left/right as mono, mix stereo to mono using `(L+R)/2`, swap channels,
  and invert both or either channel's polarity. Channel conversion applies to
  the whole document; polarity uses the selection, or the whole file.
- Cleanup dialog with Preview, Bypass, Apply, and cancellation: per-channel DC
  removal, linked peak normalization from -60 to 0 dBFS, and 12 dB/octave
  Butterworth high-pass/low-pass filters with adjustable cutoff.
- **Time-stretch mode** (`T`, or **Process > Time stretch / tempo**) adds a beat
  ruler below the time ruler. Click to anchor the audio at that position, drag
  an anchor to stretch its neighbouring intervals, and right-click to remove it.
  Add at least two anchors to stretch. Audio before the first and after the last
  keeps its length when an anchor moves; moving an outer anchor changes the file
  duration. The file starts at time zero, with the beat ruler following its leading
  edge. Drags snap to quarter beats; hold Alt for free timing. Audio and waveform
  previews update immediately with neutral pitch/formants, and releasing creates one undo step. The editor
  stays usable while a cancellable worker finishes the pitch-preserving audio.
- Click **BPM** on the beat ruler for source tempo (20–400 BPM), **Detect BPM**,
  and **Find transient anchors**. Before adding anchors, editing pitch/formants
  or detecting notes, BPM calibrates the grid.
  Afterwards **Apply BPM** changes the whole file's speed while retaining each
  anchor's beat assignment. Automatic discovery adds anchors at waveform attacks
  without moving audio or replacing manual anchors; it also estimates source BPM
  when starting a new map. Tempo estimates can have half/double-time ambiguity;
  silence or unsteady audio may need manual BPM. Detection uses linked-channel
  energy and keeps up to 2048 of the strongest transients.
- The ColdEdit logo opens a popup menu for file, editing, selection, processing,
  playback, and view commands. One toolbar combines playback controls with
  Undo/Redo, Cut/Copy/Paste, Delete, Trim, Fit, and Zoom to selection. Open and
  Export live in the menu. Playhead, duration, monitor gain, and output meters
  stay visible at the top of the right sidebar while the inspector scrolls.
- **Pitch / formants** (`P`, or **Process > Pitch / formants**) transposes the
  whole file by -24 to +24 semitones without changing duration. Decimal values
  allow fine tuning (0.01 semitone is one cent). **Preserve formants** is on by
  default; **Formant shift** adjusts tonal character independently by -12 to +12
  semitones. The optional voice/instrument pitch in Hz guides formant processing;
  leave it at 0 for automatic estimation. **Apply** renders on a cancellable
  worker before changing the audio, with one undo step. **Reset pitch / formants**
  keeps timing edits, and **Reset stretch** keeps pitch/formant settings.
- **Pitch editor** (`N`, or **Process > Pitch editor**) detects notes in a single
  voice or melodic instrument. Controls share one toolbar row on desktop and
  expand into rows in compact views. Choose **Detect**, then drag a note up or
  down. **Snap** targets semitones; hold **Alt** for fine tuning and **Escape**
  to cancel a drag. **Correct note / all** moves note centres to the nearest
  semitone while retaining vibrato and pitch variation. Click empty space or
  press **Escape** to clear selection; correction and vibrato then target all
  notes. Ctrl-click toggles individual notes, Shift-click selects a range, and
  dragging empty space draws a selection box. **All notes** / Ctrl+A selects
  every note. Dragging a selected group transposes it together, preserving
  intervals and creating one undo step. Escape cancels drags and box selection.
  **Vibrato** scales measured pitch variation around each note's centre: 0%
  flattens it, 100% retains the original, and 200% doubles it. This includes
  measured bends/drift; estimated regions are left unchanged. Ctrl-click the
  slider to type a percentage. The curve previews while dragging; release to
  render one undo step, or Escape to cancel. Multiple notes can share a setting.
  **Split** (`S`) followed by a click, or a double-click on a note, splits at
  that time. Each half must be at least 20 ms long. Splits preserve the existing
  pitch, vibrato and sound, and remain aligned through tempo changes. The halves
  can then be edited independently, with a short transition at their shared
  boundary. Splitting is undoable and does not render or modify audio by itself.
  **Reset note / all** restores pitch and 100% vibrato while retaining splits;
  **Re-detect** runs analysis again and resets note edits, splits and harmony voices.
  **Add voice** duplicates selected notes into an independently editable harmony
  (with no selection, it copies all original notes). Set the interval first:
  +3 semitones for a minor third, +4 for a major third, +7 for a fifth, +12 for
  an octave; negative intervals go lower. Select notes from one voice when
  duplicating. Add as many harmony voices as needed within the shared 8192-note
  limit; splitting a note keeps both halves in the same voice.
  Purple notes labelled H1, H2, and so on support the same dragging, multi-selection,
  vibrato and splitting. Right-click overlapping notes to choose a voice.
  Select notes in one harmony and use **Lvl** to change
  that entire voice's level (35% initially, 0% to mute, 100% full source level).
  Ctrl-click to enter a level; Escape cancels. **Remove** / Delete / Backspace
  removes selected harmony notes while retaining original notes. All voice
  changes are undoable and included in playback and export. Copies follow
  shared timing and formant settings and always render from the original source.
  Audible regions with uncertain pitch borrow estimates from nearby sustained
  notes. Amber dashed contour sections show these estimates; mostly estimated
  notes have a muted amber fill. Silence stays empty. Short tracking outliers
  are replaced with context, and the contour breaks across note boundaries
  instead of drawing tall connecting spikes. Estimates remain editable and
  participate in correction and export. With no reliable notes anywhere in the
  file, there is no pitch reference to borrow.
  The pitch contour, piano lanes and **Fit notes** help navigate; Alt+wheel
  zooms the pitch range. `T` shows the shared timing ruler in either editor.
  Detection and each completed pitch edit are undoable. Note edits render on a
  cancellable worker and use the same formant settings, source and export path
  as whole-file transposition. Global transposition adds to individual shifts.
- Cancellable background export to 16-bit PCM, 24-bit PCM, or 32-bit float WAV,
  16/24-bit FLAC (compression level 5), and MP3.
  WAV/FLAC preserve the original sample rate. Export always writes a **new** file. Existing
  destinations are rejected. Unsaved edits prompt before close/open; exporting
  from that prompt completes the pending close/open after success.

Fades require a selection; its length sets the fade duration. Linear is the
default; Smooth uses an S-shaped gain curve. A one-sample fade produces silence.
Other effects apply to the selection, or the whole file if nothing is selected.
Choose the format in **ColdEdit > Export format** or the inspector's format
selector, then **Export audio** or **Save selection**. FLAC stores integer PCM:
samples are rounded to the selected bit depth and clipped at full scale, just
like integer WAV export. Float WAV preserves headroom. Metadata tags/artwork
are not copied to exported files. MP3 is lossy; it clips input to full scale and
preserves supported sample rates. Other rates are resampled by LAME to a supported
rate shown in the inspector (for example 96 -> 48 kHz, 88.2 -> 44.1 kHz).
192/320 kbps require MPEG-1, so sources below 32 kHz are upsampled to 32 kHz for
those presets. VBR preserves all standard MP3 rates from 8 to 48 kHz. Inputs below
8 kHz and empty audio cannot be exported as MP3. The general sample-rate conversion
workflow is still not included.

Edits pause playback. Monitor gain does not affect export. Export audio includes
the whole edited timeline; Save selection includes only the selected range,
with all edits applied, and leaves the document's unsaved state unchanged.
Save selection includes only the range that overlaps existing audio.
Range dragging preserves length even when
zero-crossing snapping is enabled. Mixed audio keeps float headroom; signals
above full scale can clip in playback or integer WAV export. Normalize if needed.

Pitch and time processing share Signalsmith Stretch 1.3.2 and retain the original
source for subsequent pitch, formant, anchor and BPM changes. They render together
in one streaming pass, avoiding repeated processing of rendered audio. Harmony
voices run independent processors and mix into the same output, with short
fades at their edges. This creates chords from monophonic material; it does not
separate existing chords into voices. Harmony levels add to the original and
retain floating-point headroom. With neutral pitch
and formants, stretch playback uses fast linear resampling while rendering,
temporarily changing pitch; finished audio replaces it automatically without
changing edit history. New anchor edits supersede pending renders. With pitch or
formant changes active, anchor drags preview the waveform and audio renders on
release; playback pauses for the render and resumes after success if it was playing.
Cancellation or failure leaves the document and history unchanged. Every export
and destructive audio process uses the finished result. Undo/redo restores audio
and all pitch/time controls. Hiding either editor or the stretch ruler keeps all edits. Other audio
edits bake pitch and timing and clear their controls; undo restores them.
Pitch/time controls and note analysis are session-only, like edit history.
Monophonic detection uses YIN over 50-1200 Hz with roughly 10 ms contour spacing
and a 60 ms minimum note length. Chords, mixes, breathy sounds and strong pitch
slides can produce missing notes or octave errors; polyphonic detection is not
included. Analysis is bounded to about 2.7 hours
and 8192 notes including harmony copies; trim a shorter passage if needed. Note regions use source
coordinates, so corrections follow tempo and anchor changes. Individual note
shifts allow +/-24 semitones, with short ramps at their boundaries.
**Reset pitch / formants** retains note edits, and **Reset note / all** retains
global transposition and timing. Formant quality depends on the material and shift amount.
Individual segments support 1/8–8 times their source duration; moderate changes
give better sound quality. Native renders stream to temporary mapped audio;
browser renders share the existing audio memory budget, including retained sources.

## Recording

Click the red record button, use **ColdEdit > Record audio**, or press **Shift+R**.
The native app records the default microphone as 48 kHz mono float PCM. Choose
your default input in the operating system's audio settings. In the browser,
choose **Microphone** or **System / tab audio**; enable **Share audio** in the
screen-sharing picker. Browser and OS support determine which shared audio
sources are available. See [WEB.md](WEB.md#record-audio).

The recorder shows elapsed time and an input meter. **Stop and keep** opens a new
take for editing; **Discard take** preserves the previous document. Existing
unsaved changes prompt for export before starting. The take stays unsaved until
you export it, including after undo. Input is not played through the speakers.
Desktop capture uses a bounded ring and a disk-writing worker; temporary audio
is removed when no document, clipboard or history needs it. Browser capture
shares the audio memory budget and keeps the captured prefix when the budget
is reached. Ending screen sharing also stops and keeps the take.

## Controls

| Input | Action |
| --- | --- |
| Wheel | Zoom at pointer in the main waveform |
| Ctrl + wheel / horizontal wheel | Pan the main waveform |
| Wheel over navigator | Zoom around the current view's center |
| Middle-button drag | Direct pan |
| Alt + wheel | Vertical waveform scale |
| Drag / Shift-click | Select / extend selection |
| Drag inside selection | Move range, preserving its length and the audio |
| Click top ruler | Place the play cursor without changing the selection |
| Drag top ruler | Move the selection when inside it; otherwise pan the view without clearing it |
| Alt-drag | Draw a new range, even inside a selection |
| Drag selection edge or ruler handle | Adjust start or end; drag beyond view to autoscroll |
| Double-click waveform | Select all / clear full selection |
| Ctrl+A | Select all |
| Space | Play / pause |
| L | Toggle loop |
| F / Z | Fit file / zoom to selection |
| + / - | Zoom in / out |
| Home / End | Jump to start / end |
| Left / Right | Pan by 8% of the view |
| Ctrl+X / C / V | Cut / copy / paste |
| Ctrl+V with selection | Add clipboard signal into selection |
| Ctrl+Shift+V | Replace selection with clipboard audio |
| Delete / Ctrl+T | Delete / trim selection |
| R | Reverse selected range or file |
| Shift+R | Record a new audio file |
| I / O | Fade in / fade out selection |
| J | Snap selection/cursor to nearby zero crossings |
| T | Show / hide the time-stretch beat ruler |
| P | Open whole-file pitch and formant controls |
| N | Switch waveform / monophonic pitch editor |
| Insert | Insert silence at cursor |
| Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y | Undo / redo |
| Escape | Clear selection |
| Ctrl+O / Ctrl+E / Ctrl+S | Open / export / export |
| Ctrl+Shift+E | Save selected audio in the chosen WAV/FLAC/MP3 format |
| F1 | Shortcut reference |
| F10 | Open main menu; arrows and Enter navigate, Escape dismisses |

**ColdEdit > View > Wheel zooms by default** swaps Wheel and Ctrl+wheel in the
main waveform. The preference is saved across launches (per browser origin on
the web). Alt+wheel still adjusts vertical scale; the navigator always zooms
around the current view. Sample rate, channels, and source bit depth/format
appear in the bottom status bar.

**Follow playback** is off by default. When enabled, the view stays put until
the playhead reaches about two-thirds across the waveform, then scrolls forward
to keep it there, stopping at the file's boundary. If a loop or seek takes the
playhead off-screen to the left, the view brings it back immediately. Zooming,
panning, and selecting preserve this setting; toggle it in the inspector or
**ColdEdit > Playback > Follow playback**.

## Build and verify

On Windows, install Visual Studio with **Desktop development with C++** and its
CMake tools. The script discovers the toolchain using `vswhere`; a developer
shell is optional.

```powershell
.\build.ps1 -Test -Smoke -Package
.\build.ps1 -Run
```

The first configure fetches pinned, SHA-256-verified source archives. If present,
`third_party/{imgui,glfw,miniaudio,flac,dr_libs,lame,signalsmith_stretch,signalsmith_linear}.tar.gz` are used as offline caches. This
workspace may include these caches. They are ignored by Git; GitHub Actions
downloads the same verified archives on clean runners.

On Ubuntu/Debian (including WSL Ubuntu), install the build dependencies and run:

```sh
sudo apt-get install build-essential cmake ninja-build libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev xvfb xauth
bash build-linux.sh --test --smoke --package
```

This uses a separate `build-linux` directory, so WSL and Windows builds can
share a checkout. The Linux smoke test runs under Xvfb with software OpenGL.
Launching the normal UI in WSL requires WSLg or another graphical display.
The locally built binary requires the runtime versions of that build system;
use the GitHub release for the Ubuntu 22.04 compatibility baseline.

### GitHub builds and releases

[GitHub Actions](https://github.com/reddor/coldedit/actions/workflows/build.yml)
builds Windows, Linux and WebAssembly on pushes to `main`, pull requests, and
manual runs. Documentation-only changes skip CI. Successful jobs retain
downloadable package artifacts. CI runs the native tests, Linux OpenGL smoke test,
packaged relink check, and all three platform builds. Run the browser suite locally
before releasing (see [WEB.md](WEB.md)); browser timing on hosted runners is not a
release gate. **Run workflow → full_tests** remains available for optional browser
diagnostics at multiple display scales. Superseded branch runs are
canceled automatically. Windows UI smoke tests remain
available locally with `-Smoke` because hosted runners do not provide the
required desktop OpenGL session.

To publish a release, update the version in `CMakeLists.txt`, commit and push,
then push a matching version tag (for example `v0.1.0`):

```sh
git tag v0.1.0
git push origin v0.1.0
```

Only after all three builds and the native checks pass, the workflow creates a
GitHub Release with `ColdEdit-win64.zip`, `ColdEdit-linux-x64.tar.gz`,
`ColdEdit-web.zip`, and `SHA256SUMS.txt`. No extra repository secrets are needed.
The web ZIP can be deployed to [sqrctl.de/ce](https://sqrctl.de/ce/); the workflow
does not automatically deploy to that server.

From an initialized C++ toolchain shell:

```text
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release
```

`cmake --preset core` builds the storage/editing tests and benchmark without
fetching UI/audio libraries. `COLDEDIT_SANITIZE=ON` enables address and undefined
behavior sanitizers on Clang/GCC. A 64-bit process is required for large mappings.

The smoke suite creates hidden, real OpenGL windows at the monitor's native
scale, 100%, 150%, and 200%, then cycles through scale changes in one window.
Each case generates disposable test audio, tests undo/redo/paste, plays silently,
and injects selection and wheel input. Four boundary-drag assertions cover moving
either edge, crossing, and autoscroll. Font sizing and atlas rebuild counts
are checked too. Each case writes a framebuffer BMP and JSON timing report
under `artifacts/`. The suite requires a graphical session and OpenGL 3.3.

```powershell
.\tests\smoke.ps1
.\build\coldedit.exe --smoke-test --screenshot artifacts\smoke.bmp --report artifacts\smoke.json
.\build\coldedit_benchmark.exe
```

The optional `build/coldedit_codec_tests.exe --large-file` integration test
decodes over 4 GiB of float PCM with both known and unknown FLAC lengths and
checks samples and peaks across the boundary. It needs at least 5 GiB free on
the system temporary drive; its temporary audio is removed after the test.

The standalone benchmark creates and removes a 256 MiB WAV in its working
directory. It measures mapping, peak construction, 128 edits, and 250 stereo
waveform queries at 1,600 columns across changing zoom levels. See
[PERFORMANCE.md](PERFORMANCE.md) for the measured results and their scope.

## How it stays small

Native WAV audio is read through a read-only file mapping. FLAC/MP3 decoding
streams through small blocks into a temporary raw float PCM file, then maps it read-only.
The waveform area shows import progress and cancellation while decoding, followed
by waveform-building progress. Wide views wait for the completed peak cache;
close zoom can display actual samples while it builds, and playback is available.
Native temporary audio has no 4 GiB container limit: it requires sufficient space
on the system temporary drive and a 64-bit address space. Float PCM needs
`frames × channels × 4` bytes (about 1.29 GiB per hour at 48 kHz stereo).
Standard RIFF/WAV import/export still uses 32-bit chunk sizes; RF64 is not supported.
Use FLAC export or a shorter selection when the WAV output would exceed 4 GiB.
There is no whole-file decoded float heap buffer on native. The OS manages mapped pages; they may appear in the working set
and can be reclaimed under memory pressure. Mapping a file does **not** mean
that its full size is private heap memory.

Each source builds a min/max pyramid on a cancellable worker: 256 samples per
base block, then 16:1 aggregation. Stereo sources also cache sum/difference
peaks so channel cancellation is represented correctly after mono conversion.
Float extrema preserve float-WAV headroom.
The base stride increases for large sources to keep the cache below 32 MiB per
source. Typical 16-bit stereo audio uses about 3.33% of its file size for peaks
(mono about 1.67%). Queries combine cache blocks wholly inside each interval
and scan the partial base blocks at its edges, keeping neighboring transients
out of the result. No full-file scan runs on the drawing thread.
Visible waveform columns are cached until the view, edit state, resolution,
or peak availability changes, so dragging a selection reuses their extrema.
An antialiased connected envelope replaces hard pixel columns, including in the
navigator. Between one and four samples per physical pixel it blends into the
sample line. Extrema retain their actual sign instead of always extending to zero.

The timeline is a piece table. A piece references a source range, direction,
gain, and compact fade envelopes; silence has no sample storage. Fade envelopes
are evaluated by playback/export and conservatively bound cached waveform peaks.
Clipboard, history, export, and playback
share source mappings. Each edit stores only timeline metadata. Adjacent
compatible pieces merge. Many small edits still increase metadata and query
cost; multiple retained sources each retain their own peak cache. Channel tools
use small mixing matrices and normalization uses gain metadata.

DC removal and filters render only the affected range on a worker. Native
processing streams through a 32 KiB block into a temporary raw float PCM file, then maps
it read-only; it does not allocate a full decoded recording in RAM. Temporary
files are released when no preview, clipboard, playback, or undo state needs
them. Undo retains the original samples; a process crash may leave temporary
files in the system temporary directory. The browser uses an enforced shared
128 MiB default budget for imported PCM and rendered effects, including previews
and history. **ColdEdit > Web audio budget** offers persistent 256 and 512 MiB options.
Its **Clear undo/redo and clipboard** action releases their audio buffers while
keeping the current audio, selection, and saved/unsaved status. Freed memory is
reused by the editor; the browser's allocated heap does not shrink.
When a filter, DC removal, or additive paste reaches the budget, a recovery dialog
shows how much clearing undo/redo can release and offers **Clear undo and retry**
when that would fit. Current audio, clipboard, selection and saved/unsaved status
are preserved; the retried edit starts a new undo history. See WEB.md for details
and allocations outside this audio-storage budget.

Zero-crossing snapping is optional and defaults off. Mono picks the nearest
crossing within 5 ms; stereo picks the quietest nearby crossing across both
channels, since their zero crossings need not coincide. Exact sample entry is
never snapped. Edge fades also default off (3 ms when enabled); they attenuate
both sides of an edit without moving sample boundaries or changing duration.
Filters preserve range length, initialize from the range's first sample, and
do not add a tail outside it. Use edge fades when joining processed regions.
Filter coefficients follow the [W3C Audio EQ Cookbook](https://www.w3.org/TR/audio-eq-cookbook/).

A worker reads and converts samples into a fixed 16,384-frame stereo ring.
The audio callback only copies prepared floats, updates atomic state, and
applies monitor gain: no file reads, allocation, locks, or device commands.
Generation tags invalidate pre-seek audio. Playback state and generation share
an atomic so an old end-of-file cannot stop a newly started transport. The
worker sleeps on a condition variable when idle. The UI uses vsync while
active and waits for OS events while idle.

## Deliberate limits

This is a single-document waveform editor. It does not yet include
multitrack mixing, effects plugins, spectrum tools,
project persistence, or RF64. RIFF exports are limited to 4 GiB. History is
session-only; export before quitting to keep edits. WAV metadata chunks are
not copied to exports.

Monitoring uses linear interpolation into a 48 kHz stereo stream; it is not a
mastering-quality sample-rate converter. Export does not use the monitoring
resampler; MP3 uses LAME's resampler only when required. Integer export rounds and clips,
without dither; float export preserves finite headroom. Peak normalization
measures sample peaks, not inter-sample true peaks. Automated playback checks
verify sample delivery; subjective audio quality has not been listening-tested.

## License

ColdEdit is made available under the [MIT License](LICENSE). It was developed
with AI coding agents, guided by human instructions and feedback. Third-party
libraries, fonts, and other bundled components retain their own licenses.
Release packages include the MIT license and the dependency license notices
described below.

## Source map and dependencies

- `src/audio_file.*`: mapping, RIFF validation, sample access, peak pyramid.
- `src/codecs.*`, `src/dr_mp3.c`: streaming FLAC/MP3 decoding and encoding.
- `src/document.*`: piece table, editing/history, normalization scan, export.
- `src/processing.*`: cleanup analysis and streaming filter processing.
- `src/stretch.*`: beat/source timing maps, transient discovery, and tempo estimation.
- `src/pitch_analysis.*`: streaming monophonic detection, voiced contour and note regions.
- `src/pitch_editor.cpp`: piano lanes, note dragging and chromatic correction.
- `src/pitch_time.*`: shared pitch/time state, streaming transposition and formant
  processing, deferred render resolution, and background render queue.
- `src/playback.*`: streaming worker, SPSC ring, miniaudio callback.
- `src/recording.hpp`, `src/playback.cpp`: microphone capture and disk writer.
- `src/viewport.hpp`: double-precision camera and exponential animation.
- `src/interaction.hpp`: DPI metrics and selection-boundary dragging.
- `src/scaled_draw.hpp`: logical-coordinate drawing at the current DPI scale.
- `src/editor.*`: native UI, selection, input, jobs and smoke scenarios.
- `src/platform.*`: dialogs, saved wheel preference, memory stats, framebuffer capture.
- `src/main_web.cpp`, `src/web_limits.hpp`, `web/`: browser lifecycle, limits,
  file import/download, hosting and the native-download link.
- `tests/`: parser/edit/export tests, transport tests and large-file benchmark.

Pinned dependencies: [Dear ImGui 1.91.9b](https://github.com/ocornut/imgui/tree/v1.91.9b),
[GLFW 3.4](https://github.com/glfw/glfw/tree/3.4),
[miniaudio 0.11.23](https://github.com/mackron/miniaudio/tree/0.11.23),
[libFLAC 1.5.0](https://github.com/xiph/flac/tree/1.5.0) (BSD-style),
[dr_mp3 0.7.4](https://github.com/mackron/dr_libs/tree/8159355353a9baeea04416df9626ac2b9afbf97b)
(public domain or MIT No Attribution), and [LAME 3.100](https://lame.sourceforge.io/)
(LGPL v2 or later; encoder only).
Time stretching uses [Signalsmith Stretch 1.3.2](https://github.com/Signalsmith-Audio/signalsmith-stretch/tree/88061fd4eda56a2353c5d65c74aa3f4ccf76f9da)
with [Signalsmith Linear](https://github.com/Signalsmith-Audio/linear/tree/7bb1c8f36794d6980080ce817c91f8da8fe56a66)
(both MIT; pinned, SHA-256-verified archives). Both license notices are packaged.
Integration follows the [ImGui GLFW/OpenGL example](https://github.com/ocornut/imgui/tree/v1.91.9b/examples/example_glfw_opengl3)
and [miniaudio device callback documentation](https://miniaud.io/docs/manual/index.html).
Their license texts are included in packaged builds. System fonts are loaded
from the local Windows installation and are not redistributed.

LAME is linked directly into all builds. Each release includes `relink/` with
its source, application objects, and build instructions for replacing the library
under the LGPL. Keep that directory with distributed copies. These files add
archive size but are not loaded by the application or fetched at web startup.
