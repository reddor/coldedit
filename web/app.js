'use strict';
const canvas = document.getElementById('canvas');
const input = document.getElementById('file-input');
const loading = document.getElementById('loading');
const loadingText = document.getElementById('loading-text');
const notice = document.getElementById('notice');
const exportLink = document.getElementById('download-export');
let started = false, importing = false, exportUrl = null;
let pasteFile = false;
let inputLimit = 64 * 1024 * 1024, exportLimit = 128 * 1024 * 1024;
// Multiplying rounded CSS dimensions by DPR can miss a physical pixel at
// fractional browser zoom. Use the browser's own snapped content-box size.
let pixelBox = null;
if (typeof ResizeObserver !== 'undefined') {
  const observer = new ResizeObserver(entries => {
    const entry = entries[entries.length - 1], box = entry.devicePixelContentBoxSize?.[0];
    const rect = canvas.getBoundingClientRect();
    pixelBox = box ? {width: box.inlineSize, height: box.blockSize,
      cssWidth: rect.width, cssHeight: rect.height, dpr: window.devicePixelRatio} : null;
  });
  try { observer.observe(canvas, {box: 'device-pixel-content-box'}); }
  catch (_) { observer.observe(canvas); }
}
function showNotice(message) {
  document.getElementById('notice-text').textContent = message;
  notice.hidden = false;
}
function releaseExport() {
  if (exportUrl) URL.revokeObjectURL(exportUrl);
  exportUrl = null; exportLink.hidden = true; exportLink.removeAttribute('href');
}
window.coldeditWeb = {
  dirty: false,
  surface() {
    const rect = canvas.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
    // Chromium's emulated DPR can leave this API reporting the host's pixel
    // box. Reject inconsistent observations (ordinary edge snapping is <= 1px).
    const exact = pixelBox && pixelBox.dpr === dpr && pixelBox.cssWidth === rect.width && pixelBox.cssHeight === rect.height &&
      Math.abs(pixelBox.width - rect.width * dpr) <= 1 && Math.abs(pixelBox.height - rect.height * dpr) <= 1;
    return {width: Math.max(1, exact ? pixelBox.width : Math.round(rect.right * dpr) - Math.round(rect.left * dpr)),
      height: Math.max(1, exact ? pixelBox.height : Math.round(rect.bottom * dpr) - Math.round(rect.top * dpr)),
      cssWidth: rect.width, cssHeight: rect.height, dpr, exact: !!exact};
  },
  ready() {
    started = true; inputLimit = Module._coldedit_input_limit(); exportLimit = Module._coldedit_export_limit();
    document.getElementById('audio-budget').textContent = String(Module._coldedit_audio_limit() / (1024 * 1024));
    loading.hidden = true; canvas.focus();
  },
  fail(message) { started = false; loading.hidden = false; loadingText.textContent = message; },
  pickFile(paste = false) { if (started && !importing) { pasteFile = paste; input.value = ''; input.click(); } },
  clearImport() {
    if (!Module.FS) return;
    for (const name of Module.FS.readdir('/imports')) if (name !== '.' && name !== '..') Module.FS.unlink('/imports/' + name);
  },
  async openFile(file, paste = false) {
    if (!file || !started) return;
    if (importing || !Module._coldedit_can_import()) { showNotice('Finish the current operation or close the dialog before opening another audio file.'); return; }
    if (file.size > inputLimit) {
      showNotice('This audio file is larger than 64 MiB. Use the native app for larger recordings.'); return;
    }
    importing = true;
    try {
      // Validate before arrayBuffer(), avoiding allocation for oversized files.
      const bytes = new Uint8Array(await file.arrayBuffer());
      if (!Module._coldedit_can_import()) { showNotice('Close the current dialog, then open the file again.'); return; }
      this.clearImport(); releaseExport();
      const name = file.name.replace(/[\\/\x00-\x1f]/g, '_').replace(/^\.+$/, 'recording.wav') || 'recording.wav';
      const path = '/imports/' + name;
      Module.FS.writeFile(path, bytes, {canOwn: true});
      Module.ccall(paste ? 'coldedit_paste_file' : 'coldedit_open', null, ['string'], [path]);
      notice.hidden = true; canvas.focus();
    } catch (error) { this.clearImport(); showNotice('Could not open this audio file: ' + error.message); }
    finally { importing = false; input.value = ''; }
  },
  download(path) {
    releaseExport();
    try {
      const size = Module.FS.stat(path).size;
      if (size > exportLimit) throw new Error('Export exceeds 128 MiB. Use the native app for this recording.');
      // Build the download in bounded chunks rather than making another full-size
      // ArrayBuffer copy of the MEMFS export. Blob parts compose without flattening.
      const parts = [], chunk = new Uint8Array(1024 * 1024);
      const stream = Module.FS.open(path, 'r');
      try {
        for (let offset = 0; offset < size;) {
          const count = Module.FS.read(stream, chunk, 0, Math.min(chunk.length, size-offset), offset);
          if (!count) throw new Error('Incomplete export. Please try again.');
          parts.push(new Blob([chunk.subarray(0,count)])); offset += count;
        }
      } finally { Module.FS.close(stream); }
      const flac = path.toLowerCase().endsWith('.flac');
      const mp3 = path.toLowerCase().endsWith('.mp3');
      const blob = new Blob(parts, {type: mp3 ? 'audio/mpeg' : flac ? 'audio/flac' : 'audio/wav'});
      exportUrl = URL.createObjectURL(blob);
      exportLink.href = exportUrl; exportLink.download = path.split('/').pop(); exportLink.hidden = false;
      exportLink.textContent = mp3 ? 'Download MP3' : flac ? 'Download FLAC' : 'Download WAV';
      showNotice('Your audio is ready. Save it before closing this tab.');
      exportLink.click();
      return true;
    } catch (error) { showNotice('Download failed: ' + error.message); return false; }
    finally { Module.FS.unlink(path); }
  },
  diagnostics() { return {...JSON.parse(Module.ccall('coldedit_diagnostics', 'string', [], [])), surface: this.surface()}; }
};
var Module = {
  canvas,
  print: (...args) => console.log(...args),
  printErr: (...args) => console.error(...args),
  onAbort: () => window.coldeditWeb.fail('The editor could not continue. Reload this page or use the native app. Unsaved edits cannot be recovered.'),
  setStatus: message => { if (!started && message) loadingText.textContent = message; }
};
input.addEventListener('change', () => {
  const paste = pasteFile; pasteFile = false;
  window.coldeditWeb.openFile(input.files[0], paste);
});
input.addEventListener('cancel', () => { pasteFile = false; canvas.focus(); });
// Capture the browser's click count before GLFW handles this same event. Clear
// it after dispatch so synthesized touch input cannot reuse an old mouse count.
canvas.addEventListener('mousedown', event => {
  window.coldeditWeb.mouseClickCount = event.detail;
  setTimeout(() => { window.coldeditWeb.mouseClickCount = 0; }, 0);
}, true);
document.getElementById('dismiss').addEventListener('click', () => { notice.hidden = true; canvas.focus(); });
canvas.addEventListener('contextmenu', event => event.preventDefault());
canvas.addEventListener('dragover', event => { event.preventDefault(); canvas.classList.add('dragging'); });
canvas.addEventListener('dragleave', () => canvas.classList.remove('dragging'));
canvas.addEventListener('drop', event => { event.preventDefault(); canvas.classList.remove('dragging'); window.coldeditWeb.openFile(event.dataTransfer.files[0]); });
window.addEventListener('dragover', event => event.preventDefault());
window.addEventListener('drop', event => event.preventDefault());
window.addEventListener('beforeunload', event => { if (window.coldeditWeb.dirty) { event.preventDefault(); event.returnValue = ''; } });
canvas.addEventListener('webglcontextlost', event => { event.preventDefault(); window.coldeditWeb.fail('Graphics context lost. Reload the page to restart the editor.'); });
// Browser shortcuts must not steal editor commands while its canvas has focus.
window.addEventListener('keydown', event => {
  if (document.activeElement !== canvas) return;
  if ((event.ctrlKey || event.metaKey) && ['a','c','e','o','s','t','v','w','x','y','z'].includes(event.key.toLowerCase()) || [' ','F1','F10','Home','End','ArrowLeft','ArrowRight'].includes(event.key)) event.preventDefault();
});
if (!window.crossOriginIsolated || typeof SharedArrayBuffer === 'undefined') {
  window.coldeditWeb.fail('This server needs cross-origin isolation to run the editor. Use the included local server or configure COOP/COEP headers.');
} else if (!window.WebAssembly) {
  window.coldeditWeb.fail('WebAssembly is required. Please use a current browser.');
} else {
  const script = document.createElement('script'); script.src = 'coldedit.js';
  script.onerror = () => window.coldeditWeb.fail('Could not load the editor. Reload the page or try the native app.');
  document.body.appendChild(script);
}
