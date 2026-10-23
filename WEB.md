# ColdEdit for the web

**[Open the hosted editor](https://sqrctl.de/ce/)** ·
[Download releases](https://github.com/reddor/coldedit/releases)

The browser target shares the C++ editor, waveform renderer, effects, history,
and playback ring with the native application. It uses Emscripten 4.0.23,
WebGL 2, the contrib GLFW 3.4 port, and miniaudio's AudioWorklet backend.

WAV/FLAC/MP3 import and export share the native codecs. FLAC export supports
16/24-bit PCM at compression level 5. MP3 export uses LAME 3.100, linked directly
into the main WASM module: no separate encoder download or on-demand loader.
Presets are VBR high (V2), 192 kbps CBR, and 320 kbps CBR. Gapless metadata preserves
duration in compatible players. MP3 preserves supported rates and resamples other
rates as shown in the inspector; see README.md for the rate policy.
Choose **ColdEdit > Export format**, then export the file or selection.

Selection dragging, retained ranges after Cut/Delete, Save selection, and
additive Paste into selection share the native implementation. Mixing uses the
same shared audio budget as imports and filters, including undo history (128 MiB
by default, optionally 256 or 512 MiB).
`tests/web-selection.cjs` checks real pointer/keyboard input and every exported
sample after mixing, plus retained ranges at EOF and recovery after cutting all.

## Run the packaged build

Extract `dist/ColdEdit-web.zip`, then run this command in its folder with Node.js:

```text
node serve.mjs .
```

Open **http://localhost:8080**. From this repository you can instead run:

```text
node web/serve.mjs dist/Web
```

The server binds only to this computer. Opening `index.html` directly through
`file://` will not work. No audio is uploaded; file reads, edits, playback, and
export happen locally in the browser. The native-download links open GitHub
Releases, where Windows and Linux packages are published.

## Limits and behavior

- Maximum input: **64 MiB per WAV, FLAC or MP3**, checked before reading in JavaScript and
  again before allocating audio storage in C++.
- Maximum export: **128 MiB** including headers. WAV uses a conservative 56-byte
  header allowance, checked before starting a job and inside the exporter.
  FLAC and MP3 check actual encoded bytes before each write, so highly compressed
  material can export even when its uncompressed WAV would be too large.
  MP3 encodes in small blocks on the existing export worker and remains cancellable.
  Pasting can grow the timeline without duplicating source audio; exports that
  exceed the limit explain how to export a selection, lower the bit depth, or use the native app.
- Fade in/out (Linear or Smooth), selection export, and exact-duration silence
  insertion use the same code as the native build. Fades and silence add only
  timeline metadata. The export limit applies to the selected range when using
  **Save selection**; that export leaves unsaved edits and history intact.
- The web clipboard is cleared on a successful document open to avoid retaining
  audio from previous files. Undo snapshots share their referenced audio sources.
- All retained source and effect audio (preview, clipboard, playback, undo/redo)
  shares a **128 MiB default audio storage budget**, checked before allocation.
  Choose **ColdEdit > Web audio budget > 256 MiB** or **512 MiB** for more processing room.
  The preference is saved per browser origin; unavailable browser storage leaves
  it active for the session. Lowering the budget is disabled while retained audio
  exceeds the selected lower limit. Input/export limits stay the same.
  **Clear undo/redo and clipboard** in the same submenu manually releases audio
  retained by history and the clipboard. It keeps the current audio, selection,
  saved/unsaved status, and playback state; the cleared history and clipboard
  cannot be restored. The audio-in-use reading drops as obsolete buffers are
  released, making room for new edits or a lower budget. The WASM heap stays
  allocated and reuses that space.
  WAV keeps
  its original PCM byte representation; FLAC and MP3 decode to float32 PCM.
  48 kHz stereo float32 consumes about 22 MiB per minute (roughly 5m50s in 128 MiB).
  Imports check declared duration when available and actual output as it grows;
  missing or forged duration headers cannot bypass the cap. Buffer capacity,
  not just filled samples, is counted. Failed/cancelled imports release their
  reservations and preserve the current document. That document remains counted
  while its replacement is decoding, so opening another large file can exceed
  the shared budget even if either file would fit alone.
- DC removal and filters render float PCM only for the selected range. When these
  operations or additive paste reach the budget, a recovery dialog shows the new
  allocation and the unique audio storage reclaimable from undo/redo. **Clear undo
  and retry** is enabled only if that recovery makes the operation fit. It keeps
  current audio, clipboard, selection, and saved/unsaved status, removes all previous
  undo/redo states, and retries the same operation. The retry itself can be undone.
  Cancel keeps history intact. The dialog also allows raising the budget and retrying
  with history intact. If a later allocation or processing error occurs after clearing,
  current audio survives but the discarded history cannot be restored.
  Clipboard and current audio can pin a whole source even if only a small range is
  used; clearing metadata-only edits may reclaim nothing. A three-minute 44.1 kHz
  stereo float buffer uses 60.6 MiB: current audio plus a new result fits in 128 MiB
  after releasing older sources. At 48 kHz those two buffers require 131.8 MiB and
  need the larger setting or a shorter range. Full-track filters already produce a
  consolidated source; recovery does not allocate another copy to flatten it.
  Normalization, channel tools, fades,
  and silence need no rendered audio buffer. Processing remains cancellable;
  preview/bypass/cancel do not alter the document or its saved state.
- Browser audio uses one owned PCM-byte buffer per source. Known-length FLAC/MP3
  allocates once; unknown-length streams grow bounded storage incrementally.
  Growth can briefly retain both old and new buffers while copying; these
  transient copies, compressed input, codec state, and waveform peaks are outside
  the retained-audio budget. Temporary import files are removed after reading;
  export files are removed after creating a download. Only the latest download
  Blob is retained, and is released on another import/export or page close.
- WASM memory starts at 128 MiB and can grow to 1 GiB, leaving room for the
  512 MiB audio budget plus working buffers and runtime data. This is an address-space
  capacity, **not** a limit on total browser memory: JavaScript file buffers,
  download Blobs, graphics, and browser resources are additional. The WASM heap
  does not shrink, but freed audio storage is reusable.
- Export initiates a download and provides a retry link. Browsers control where
  it is saved; the application cannot confirm that it reached disk. Save it
  before closing the tab. Unsaved edits trigger the browser's unload prompt.
- Smaller screens use a compact toolbar, a full-width waveform, and a **Tools**
  button for the inspector. Tap the ColdEdit menu to open/export files or access
  additional edits; drag on the waveform to select, use **+ / -** to zoom, and
  drag the navigator to pan. The Tools panel has a wide draggable scrollbar.
  The layout fits portrait and landscape screens down to 320 CSS pixels wide;
  dialogs wrap to the available width. At 640 pixels and above, playback and
  edit controls share a single row. Browser memory limits still apply.

## High DPI and browser zoom

The CSS layout and canvas backing store are independent. A ResizeObserver
reads the browser's integer `devicePixelContentBoxSize`, avoiding the rounding
errors from truncating CSS dimensions first. Browsers without that API (or with
an inconsistent emulated DPI report) use rounded physical edges as a fallback.
See the [Resize Observer specification](https://www.w3.org/TR/resize-observer/).

ImGui renders directly in physical pixels, including its glyph positioning;
font size and UI spacing follow DPR. Font atlases use pixel-snapped advances
and are rebuilt only when DPI changes, not on ordinary resizes. The small
`cmake/WebImGui.cmake` adapter scales all GLFW mouse-position events, cached
cursor positions, and cursor warps between CSS and rendering coordinates. It
checks the pinned upstream backend before applying the adaptation.

## Build

Install and activate the pinned SDK in `third_party/emsdk` (or supply `-Sdk`):

```powershell
git clone https://github.com/emscripten-core/emsdk.git third_party/emsdk
.\third_party\emsdk\emsdk install 4.0.23
.\third_party\emsdk\emsdk activate 4.0.23
.\build-web.ps1 -Package
```

CMake and Ninja must be available. On Windows the script can find Visual
Studio's bundled copies. The SDK and dependency archives stay local to this
project. The first web build fetches the SDK-pinned, hash-verified GLFW port.

From an Emscripten environment on other platforms:

```text
emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-web --parallel
cmake --install build-web --prefix dist/Web
```

Serve `build-web/site` while developing. If distributing a manually assembled
build, include the licenses, `relink/`, and all generated
`.js`, `.wasm`, and `.data` files. Do not ship just the HTML file.

## Hosting

Use HTTPS, or localhost for development. The server must send:

```text
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
Cross-Origin-Resource-Policy: same-origin
```

Serve `.wasm` as `application/wasm`, `.js` as JavaScript, and keep all runtime
files on the same origin. `_headers` is included for hosts that support that
format. Hosts that ignore it need equivalent server configuration. Enable
Brotli or gzip for transport. No backend, accounts, or upload endpoint is needed.

## Verification

The Playwright suite uses an isolated Edge profile by default:

```text
node tests/web-smoke.cjs
node tests/web-lifecycle.cjs
node tests/web-toolbar.cjs
node tests/web-wheel.cjs
node tests/web-follow.cjs
node tests/web-features.cjs
node tests/web-cleanup.cjs
node tests/web-selection.cjs
node tests/web-mobile.cjs
```

Install the pinned test dependency with `npm ci --prefix tests` first. Set
`COLDEDIT_WEB_URL` to another local server URL or `COLDEDIT_BROWSER=chrome` to
use an installed Chrome channel. CI uses `COLDEDIT_BROWSER=chromium` and installs
that browser with `cd tests && npx playwright install --with-deps chromium`.
Tests require a running server. Native core
tests also check exact size boundaries and overflow-safe export limits.

Ordinary GitHub pushes and pull requests run only `web-smoke.cjs` at 100% scale.
Release tags and manual runs with `full_tests` enabled run all suites, with
100%, 125% and 200% scales where supported, plus the mobile viewport matrix.
Documentation-only changes skip CI; the full suite also remains available locally.

For codec and decoded-memory verification, generate the small compressed fixtures
once in a new output directory, then run the isolated server/browser harness:

```text
build/coldedit_codec_tests.exe --web-fixtures artifacts/codec-fixtures
node tests/run-web-tests.cjs web-codecs.cjs
node tests/run-web-tests.cjs web-mp3.cjs
node tests/run-web-tests.cjs web-memory.cjs web-waveform.cjs
```

This covers MP3 gapless import, edited-selection FLAC round trips at both bit
depths, corrupt-file recovery, cancellation, exact/aggregate budget boundaries,
and an over-limit FLAC stream with no reported sample count.
The MP3 suite checks all export presets, selection and full-file downloads,
gapless reimport, decoded signal, 96 -> 48 kHz mono resampling, cancellation,
and the web output limit.
The memory suite covers the three-minute filter workflow, exported sample accuracy,
cancel, undo/redo recovery, clipboard retention, additive paste, both budget settings,
persistence, and insufficient recovery at 48 kHz. Waveform captures exercise the
envelope, line transition and sample detail at multiple physical pixel densities.

Browser verification covers 100%, 110%, 125%, 150%, 175%, and 200% DPI, file input, selection
edges and crossing, zoom, pan, AudioWorklet playback, undo/redo, and downloaded
WAV samples against the expected edit. Toolbar tests cover actual button clicks,
disabled states, popup keyboard navigation, narrow layouts, fractional CSS
dimensions, exact framebuffer sizing, and pointer re-entry. Set `COLDEDIT_DPI`
to a comma-separated scale list to run a subset; `COLDEDIT_FORCE_DPI=1.1` with
the toolbar suite also tests Chromium's real (non-emulated) physical pixel box.
Other browser engines require separate
verification. Timing is not a frame-rate guarantee.

The mobile suite checks 320/390-pixel phones, landscape and tablet viewports,
high DPI, touch file picking, selection and editing, zoom, Tools, and rotation.
These are Chromium touch emulation tests; physical iOS/Android devices and
mobile Safari still need separate verification.

Wheel tests cover pointer-anchored default zoom, Ctrl panning, horizontal panning,
Alt vertical scale, navigator center anchoring, and the saved View preference
across reloads. They also capture the empty editor and compact status bar.

The lifecycle suite also tests the real file picker, drag-and-drop, cancelled
unsaved-change prompts, corrupt inputs, three consecutive exact-64-MiB opens,
playback after memory growth, 100% → 150% → 200% → 100% DPI transitions,
the GitHub Releases link, and the explanatory screen when isolation headers are
missing. Raw reports and screenshots are saved in `artifacts/`.
