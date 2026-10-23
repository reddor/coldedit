# Verification and performance

Measured on this Windows x64 machine with an optimized MSVC build on
2026-10-23. These are local measurements, not guaranteed frame rates on other
hardware. Raw reports are in `artifacts/`.

## Large-file core benchmark

| Measurement | Observed |
| --- | ---: |
| Input | 256 MiB, stereo 16-bit, 48 kHz, 23.30 minutes |
| Map and parse | 0.43 ms |
| Build all peak levels | 870 ms |
| Peak cache, including stereo sum/difference | 8.53 MiB |
| Process private memory before open | 0.48 MiB |
| Process private memory after peaks and 128 edits | 12.15 MiB |
| 1,600-column stereo waveform query, median | 0.13 ms |
| Same query, p95 | 1.64 ms |

The fixture contains periodic impulses and silence. Timings include no disk
cache flush, so they are not cold-disk throughput measurements. Private memory
excludes shared/file-backed pages. This is the standalone core process, not the
OpenGL application. The normal application also holds font atlases, UI buffers,
audio device resources, and graphics-driver allocations.

This core measurement was rerun after channel tools and cleanup were added;
see `artifacts/benchmark-cleanup.json`. Sum/difference caches account for the
additional peak memory and keep mono conversion accurate without rendering PCM.
The total source peak-cache budget remains 32 MiB.

## Actual UI smoke runs

OpenGL 3.3 windows on the installed Intel graphics driver rendered a 24-second
stereo demo, performed edits and history changes, played silently, and processed
injected selection, boundary dragging, autoscroll, and Ctrl-wheel zoom. The
logical window size was 1440 x 900 for the explicit scale cases.

| Scale | Framebuffer | Body font | CPU frame median / p95 |
| --- | --- | --- | --- |
| 100% | 1440 x 900 | 17 px | 1.59 / 7.03 ms |
| 150% | 2160 x 1350 | 25 px | 1.87 / 8.77 ms |
| 200% | 2880 x 1800 | 34 px | 2.12 / 10.62 ms |

Timings measure CPU frame construction and GL submission, excluding event waits,
vsync, and between-frame font/DPI rebuilds. They do not measure GPU completion
or guarantee a refresh rate. Working sets ranged from approximately 123 to
172 MiB, including mapped pages and driver resources. All three cases reported
zero running-stream underruns; initial buffering after play/seek is excluded.

The native monitor's 200% scale also passed. A live 100% → 150% → 200% → 100%
cycle passed with exactly four font-atlas builds and the original logical
window size restored. These transitions are injected through the application's
DPI update path; physically moving the window between different monitors has
not been tested. Framebuffer captures were inspected at 100%, 150%, and 200%.

## Correctness coverage

- PCM depths, float samples, NaN/infinity handling, extensible headers, unknown
  odd-sized chunks, missing files, truncated headers/chunks, invalid alignment,
  channel limits, empty files and float headroom.
- 2,400 randomized editing/history operations checked against a flat-sample
  reference model; reversed range splitting, silence, gain, trim, coalescing,
  metadata sharing, empty timelines and the history limit.
- PCM16/PCM24/float export round trips, odd data padding, Unicode destinations,
  overwrite refusal, cancellation cleanup and RIFF size overflow.
- Peak extrema across block boundaries and reversed/gain-adjusted pieces;
  double-precision pointer anchoring and extreme viewport bounds.
- DPI metrics for pixel-based and framebuffer-scaled windows, invalid scales,
  selection-edge hit testing, grab offsets, reversed selections, crossing,
  and file-bound clamps.
- Offline callback checks for exact samples, stereo duplication, pause, seek,
  looping, end-of-file, rate conversion, snapshot changes and concurrent rapid
  play commands around end-of-file.
- Five hidden-window smoke cases exercise the actual mouse input and rendering
  path, selection-boundary drags, DPI layout, fonts, and live scale transitions;
  each saves a framebuffer for visual review.

Native open/save dialogs were compiled and integrated but not automated through
the operating-system dialog UI. Export correctness is tested through the same
export function used by the application. Other operating systems and real-time
performance under heavy system load have not been verified.

## WebAssembly verification

Built with Emscripten 4.0.23 and tested in an isolated, headless Microsoft Edge
profile on this machine. The runtime (`.wasm`, JavaScript, font `.data`) is about
1.75 MiB before transport compression, excluding the native Windows download.

Browser tests pass at 100%, 110%, 125%, 150%, 175%, and 200% device pixel ratios. They exercise
actual canvas mouse/keyboard input, selection-boundary dragging and crossing,
zoom/pan, AudioWorklet playback, undo/redo, and export. Downloaded reversed
24-bit WAV samples are checked against the original PCM fixture. The tests
also reject oversized inputs before reading, and oversized exports before a
worker job or temporary export file is created.

Toolbar tests additionally exercise cut/copy/paste, undo/redo, delete, trim,
empty-document disabled controls, popup navigation and dismissal, minimum-width
layouts, fractional CSS sizes, and pointer leave/re-entry. The canvas backing
store and ImGui display dimensions match, with 1:1 framebuffer and font scales;
ordinary resizes do not rebuild the font atlas. The report is
`artifacts/web-toolbar.json`.

The lifecycle run opened three consecutive **64 MiB** stereo WAV files. WASM
heap capacities were **128 MiB, 153.625 MiB, 153.625 MiB**: replacement reused
storage after the first growth. The last source's peak pyramid was **1.067 MiB**.
Temporary import/export files were removed, cancelled opens and malformed files
preserved the current document, and playback worked after memory growth. The
100% → 150% → 200% → 100% live DPI cycle rebuilt fonts exactly four times.
These runs reported zero playback underruns.

WASM capacity is not process working set or total browser memory. JavaScript
file buffers, the latest download Blob, graphics resources, and browser
overhead are additional. The input/export caps bound audio buffers; they do
not guarantee a fixed total browser footprint. No browser frame-rate claim
is made. Firefox, Safari, mobile/touch, and physical monitor moves have not
been verified. Reports are `artifacts/web-smoke.json` and
`artifacts/web-lifecycle.json`; see `WEB.md` for deployment requirements.
