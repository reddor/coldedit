const {chromium} = require('playwright');
const assert = require('node:assert/strict'), fs = require('node:fs');
const {settle, captureFailure} = require('./web-test-helpers.cjs');
const frames = 48000, clipFrames = 4800;
const diag = p => p.evaluate(() => coldeditWeb.diagnostics());
const range = d => [d.selection_start, d.selection_end];
async function key(p, k) { await p.keyboard.press(k); await settle(p); }
async function drag(p, a, b) {
  await p.mouse.move(a, 350); await settle(p); await p.mouse.down(); await settle(p);
  await p.mouse.move(b, 350, {steps: 12}); await settle(p); await p.mouse.up(); await settle(p);
}
const base = (f, c) => c ? -.125 : .25;
const clip = (f, c) => c ? .0625 : -.125;
function fixture(count, sample, rate = 48000, channels = 2) {
  const b = Buffer.alloc(44 + count * channels * 4);
  b.write('RIFF'); b.writeUInt32LE(b.length - 8, 4); b.write('WAVEfmt ', 8); b.writeUInt32LE(16, 16);
  b.writeUInt16LE(3, 20); b.writeUInt16LE(channels, 22); b.writeUInt32LE(rate, 24);
  b.writeUInt32LE(rate * channels * 4, 28); b.writeUInt16LE(channels * 4, 32); b.writeUInt16LE(32, 34);
  b.write('data', 36); b.writeUInt32LE(count * channels * 4, 40);
  for (let f = 0; f < count; ++f) for (let c = 0; c < channels; ++c) b.writeFloatLE(sample(f, c), 44 + (f * channels + c) * 4);
  return b;
}
async function chooser(p, mainMenu = false) {
  if (mainMenu) {
    const d = await diag(p);
    await key(p, 'F10'); await key(p, 'Home');
    for (let i = 0; i < (d.selection_end > d.selection_start ? 3 : 2); ++i) await key(p, 'ArrowDown');
    await key(p, 'ArrowRight'); await key(p, 'End'); await key(p, 'ArrowUp');
  } else {
    await p.mouse.click(500, 350, {button: 'right', delay: 40}); await settle(p); await key(p, 'End');
  }
  const pending = p.waitForEvent('filechooser'); await key(p, 'Enter');
  return pending;
}
async function pasteFile(p, buffer, name = 'clip.wav', mainMenu = false, fails = false) {
  const picker = await chooser(p, mainMenu);
  await picker.setFiles({name, mimeType: 'application/octet-stream', buffer});
  await p.waitForFunction(fails => {
    const d = coldeditWeb.diagnostics();
    return !d.busy && (fails ? !!d.error : d.undo_label === 'Paste from file' || d.undo_label === 'Insert from file');
  }, fails);
  assert.equal(await p.evaluate(() => Module.FS.readdir('/imports').length), 2, 'staging is released');
}
async function download(p, path) {
  const pending = p.waitForEvent('download'); await key(p, 'Control+e');
  const result = await pending; await result.saveAs(path);
  await p.waitForFunction(() => !coldeditWeb.diagnostics().busy);
  await p.locator('#dismiss').click(); await settle(p);
  return {bytes: fs.readFileSync(path), name: result.suggestedFilename()};
}
async function verify(p, path, count, sample) {
  const {bytes: b, name} = await download(p, path);
  assert.equal(name, 'original-edited.wav', 'file paste keeps the document name');
  assert.equal(b.readUInt32LE(40), count * 6);
  for (let f = 0; f < count; ++f) for (let c = 0; c < 2; ++c)
    assert(Math.abs(b.readIntLE(44 + (f * 2 + c) * 3, 3) / 8388608 - sample(f, c)) < 2e-6, `sample ${f}/${c}`);
}
async function format(p, index) {
  const d = await diag(p); await key(p, 'F10'); await key(p, 'Home');
  for (let i = 0; i < (d.selection_end > d.selection_start ? 8 : 7); ++i) await key(p, 'ArrowDown');
  await key(p, 'ArrowRight'); await key(p, 'Home');
  for (let i = 0; i < index; ++i) await key(p, 'ArrowDown'); await key(p, 'Enter');
  assert.equal((await diag(p)).export_format, index);
}
(async () => {
  const browser = await chromium.launch({channel: process.env.COLDEDIT_BROWSER || 'msedge', headless: true, args: ['--mute-audio']});
  const reports = [];
  try {
    for (const scale of (process.env.COLDEDIT_DPI || '1,1.25,2').split(',').map(Number)) {
      const context = await browser.newContext({viewport: {width: 1440, height: 944}, deviceScaleFactor: scale, acceptDownloads: true});
      const p = await context.newPage(), errors = []; p.setDefaultTimeout(30000); p.on('pageerror', e => errors.push(e.message));
      try {
        await p.goto(process.env.COLDEDIT_WEB_URL || 'http://localhost:8080/');
        await p.waitForFunction(() => document.querySelector('#loading').hidden);
        await p.locator('#file-input').setInputFiles({name: 'original.wav', mimeType: 'audio/wav', buffer: fixture(frames, base)});
        await p.waitForFunction(() => coldeditWeb.diagnostics().frames === 48000);
        await drag(p, 300, 650); await key(p, 'Control+c');
        const selected = await diag(p), a = selected.selection_start, b = selected.selection_end, n = b - a;
        await pasteFile(p, fixture(clipFrames, clip), 'short.wav', true);
        let d = await diag(p); assert.deepEqual(range(d), range(selected)); assert.equal(d.frames, frames);
        assert.equal(d.history_states, 2); assert.equal(d.dirty, true); assert.equal(d.title, 'original.wav');
        await verify(p, `artifacts/web-file-mix-${scale}.wav`, frames, (f, c) => base(f, c) + (f >= a && f < a + clipFrames ? clip(f - a, c) : 0));
        await key(p, 'Control+z'); await key(p, 'Control+Shift+z'); assert.equal((await diag(p)).undo_label, 'Paste from file');
        await key(p, 'Control+z');
        // A longer file stops at the selection end; it neither repeats nor replaces the tail.
        await pasteFile(p, fixture(frames, clip));
        await verify(p, `artifacts/web-file-long-mix-${scale}.wav`, frames, (f, c) => base(f, c) + (f >= a && f < b ? clip(f - a, c) : 0));
        await key(p, 'Control+z'); await key(p, 'Escape');
        await p.mouse.click(450, 350, {delay: 40}); await settle(p); const at = (await diag(p)).position;
        await pasteFile(p, fixture(clipFrames, clip)); d = await diag(p);
        assert.equal(d.frames, frames + clipFrames); assert.deepEqual(range(d), [at, at + clipFrames]);
        assert.equal(d.history_states, 2); assert.equal(d.undo_label, 'Insert from file');
        await verify(p, `artifacts/web-file-insert-${scale}.wav`, frames + clipFrames, (f, c) => f < at ? base(f, c) : f < at + clipFrames ? clip(f - at, c) : base(f - clipFrames, c));
        await key(p, 'Control+z'); assert.equal((await diag(p)).frames, frames);
        await key(p, 'Control+Shift+z'); assert.equal((await diag(p)).frames, frames + clipFrames);
        await key(p, 'Control+z'); await key(p, 'Escape'); await key(p, 'End'); await key(p, 'Control+v');
        assert.equal((await diag(p)).frames, frames + n, 'file paste preserves the earlier clipboard');
        await key(p, 'Control+z');
        // Failed imports leave audio, history, selection and clipboard intact.
        for (const bad of [fixture(100, clip, 44100), fixture(100, clip, 48000, 1), fixture(0, clip), Buffer.alloc(64)]) {
          const before = await diag(p); await pasteFile(p, bad, 'bad.wav', false, true); d = await diag(p);
          for (const field of ['title', 'frames', 'history_states', 'selection_start', 'selection_end', 'dirty']) assert.equal(d[field], before[field], field);
          await key(p, 'Tab'); await key(p, 'Enter'); await p.waitForFunction(() => !coldeditWeb.diagnostics().error);
        }
        // The same picker supports compressed audio and never opens a second document.
        await key(p, 'Escape'); await format(p, 4);
        const flac = await download(p, `artifacts/web-file-source-${scale}.flac`); await format(p, 1);
        await key(p, 'End'); await pasteFile(p, flac.bytes, 'clip.flac', true);
        assert.equal((await diag(p)).frames, frames * 2); await key(p, 'Control+z');
        // Cancellation resets picker intent so the next normal Open replaces the document.
        await chooser(p); await p.locator('#file-input').dispatchEvent('cancel'); await settle(p);
        await p.locator('#file-input').setInputFiles('tests/fixtures/tone.mp3');
        await p.waitForFunction(() => coldeditWeb.diagnostics().title === 'tone.mp3');
        await key(p, 'End'); await pasteFile(p, fs.readFileSync('tests/fixtures/tone.mp3'), 'second.mp3');
        d = await diag(p); assert.equal(d.frames, 88200); assert.equal(d.title, 'tone.mp3'); assert.equal(d.history_states, 2);
        await p.screenshot({path: `artifacts/web-file-paste-${scale}.png`});
        assert.deepEqual(errors, []); reports.push({scale, passed: true});
        console.log(`PASS file paste ${scale}x: menu/context pickers, WAV samples, mix/insert, clipboard, undo/redo, rejected files, FLAC/MP3, picker cancellation`);
      } catch (e) { await captureFailure(p, `web-file-paste-failure-${scale}`, e); throw e; }
      finally { await context.close(); }
    }
  } finally { await browser.close(); fs.writeFileSync('artifacts/web-file-paste.json', JSON.stringify(reports, null, 2)); }
})().catch(e => { console.error(e); process.exitCode = 1; });
