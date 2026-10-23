# ColdEdit implementation plan

Build a focused, single-document native wave editor in C++20 with Dear ImGui,
GLFW, OpenGL 3.3, and miniaudio. Prioritize a responsive timeline and bounded
audio memory over a large feature surface.

1. **Storage and edits.** Validate RIFF/WAVE inputs; map PCM audio read-only;
   support 8/16/24/32-bit PCM and 32-bit float, mono/stereo. Represent the edited
   timeline as a piece table, sharing source mappings and peak caches across
   clipboard and undo states. Stream export into a temporary file, then rename.
2. **Waveform.** Build float min/max peak levels on a cancellable worker (preserve
   float-file headroom through gain edits).
   Cap each source's peak cache at 32 MiB. Draw only visible columns; switch to
   connected samples at close zoom. Preserve transient extrema.
3. **Transport.** A streaming worker converts source samples into a bounded
   stereo ring buffer. The device callback only reads prepared samples; no
   file reads, allocations, or locks. Support seek, pause, looping and gain.
4. **Interface.** A restrained dark workspace, clear transport and selection
   readouts, full-file navigator, channel lanes, time ruler, meters, shortcuts,
   native Windows open/export dialogs and drag/drop. Cursor-anchored animated
   zoom, wheel/trackpad pan, middle-button drag, selection autoscroll.
5. **Editing.** Cut, copy, paste, delete, trim, silence, reverse, normalize,
   gain, bounded undo/redo, dirty-document protection and 16/24/float export.
6. **Verification and delivery.** Test parsing, adversarial input, piece-table
   edits, history, exact export, peak extrema, zoom anchoring, and streaming.
   Build Release, execute a scripted OpenGL UI smoke run and capture its actual
   framebuffer. Document build/run commands, controls, memory cost and limits.

Deferred: compressed formats, recording, multitrack, plugins, spectral editing,
project persistence, RF64 and destructive in-place saving. Original inputs are
never overwritten by editing; export creates a new file.

## Completed

All six stages are implemented and verified on Windows x64. The packaged
application and dependency licenses are in `dist/ColdEdit`, with a ZIP alongside.
Core and playback suites pass; current counts are in the CTest reports.
High-DPI layout, font rebuilding, physical-pixel waveform detail, and draggable
selection boundaries are implemented. Five real OpenGL smoke cases pass
selection/zoom checks and four boundary-drag assertions at native, 100%, 150%,
200%, and changing DPI scales. Each captures its framebuffer.
See `PERFORMANCE.md` for measurements and verification limits.

The WebAssembly target is implemented with shared editing/UI code, WebGL 2,
AudioWorklet playback, bundled fonts, browser file import and export, a 64 MiB
input cap and 128 MiB export cap. Browser tests verify actual canvas input,
sample-exact exports, size boundaries, repeated maximum-size imports, cleanup,
playback after memory growth, live DPI changes, and the native download link.
Build and hosting instructions are in `WEB.md`.

The everyday editing additions are implemented in both builds: fades, selection
export, silence insertion, optional zero-crossing snapping and short edit-edge
fades, channel conversion/polarity tools, DC removal, adjustable normalization,
and high-/low-pass filters with Preview/Bypass/Apply. Native cleanup uses streamed
temporary float audio; the web build enforces a retained processing-audio budget.
Codec encoding/decoding remains disabled; format conversion is a separate,
deferred addition whose size should be measured before choosing dependencies.

Selection ranges now move independently of audio and retain their exact position
and length after Cut/Delete. Save selection exports the selected audio as WAV.
Ctrl+V mixes into an active selection; Ctrl+Shift+V replaces it. Mixing uses the
bounded processing path, retains float headroom, and is a single undo step.
