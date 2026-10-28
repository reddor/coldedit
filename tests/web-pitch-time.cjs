const {chromium} = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {settle, captureFailure} = require('./web-test-helpers.cjs');
const diag = p => p.evaluate(() => coldeditWeb.diagnostics());
async function key(p, k) { await p.keyboard.press(k); await settle(p); }
async function control(p, i, kind = 'pitch_controls') {
  const pos = (await diag(p))[kind][i], canvas = await p.locator('#canvas').boundingBox();
  await p.mouse.click(canvas.x + pos.x, canvas.y + pos.y, {delay: 40}); await settle(p);
}
async function input(p, i, value, kind) {
  await control(p, i, kind); await key(p, 'Control+a'); await p.keyboard.type(String(value)); await key(p, 'Tab');
}
async function idle(p) {
  await settle(p); await p.waitForFunction(() => !coldeditWeb.diagnostics().busy); await settle(p);
  assert.equal((await diag(p)).error, '');
}
function fixture(seconds = 3) {
  const frames = seconds * 48000, b = Buffer.alloc(44 + frames * 4);
  b.write('RIFF'); b.writeUInt32LE(b.length - 8, 4); b.write('WAVEfmt ', 8); b.writeUInt32LE(16, 16);
  b.writeUInt16LE(1, 20); b.writeUInt16LE(2, 22); b.writeUInt32LE(48000, 24); b.writeUInt32LE(192000, 28);
  b.writeUInt16LE(4, 32); b.writeUInt16LE(16, 34); b.write('data', 36); b.writeUInt32LE(frames * 4, 40);
  for (let f = 0; f < frames; f++) {
    const v = Math.round(8000 * Math.sin(2 * Math.PI * 440 * f / 48000));
    b.writeInt16LE(v, 44 + f * 4); b.writeInt16LE(-v, 46 + f * 4);
  }
  return b;
}
async function exportWav(p, path, expectedFrames, expectedHz) {
  const event = p.waitForEvent('download'); await key(p, 'Control+e');
  const download = await event; await download.saveAs(path); await idle(p);
  const b = fs.readFileSync(path), stride = 6;
  assert.equal(b.readUInt16LE(34), 24); assert.equal(b.readUInt32LE(40) / stride, expectedFrames);
  let crossings = 0, difference = 0, energy = 0;
  for (let f = 19201; f < 67200; f++) {
    const v = b.readIntLE(44 + f * stride, 3), previous = b.readIntLE(44 + (f - 1) * stride, 3);
    if (previous <= 0 && v > 0) crossings++;
    difference += Math.abs(v + b.readIntLE(47 + f * stride, 3)); energy += Math.abs(v);
  }
  assert(Math.abs(crossings - expectedHz) < 4, `exported pitch ${crossings} instead of ${expectedHz}`);
  assert(energy > 0 && difference / energy < .003, 'stereo polarity remains linked');
  await p.locator('#dismiss').click(); await settle(p);
}
(async () => {
  fs.mkdirSync('artifacts', {recursive: true});
  const browser = await chromium.launch({channel: process.env.COLDEDIT_BROWSER || 'msedge', headless: true, args: ['--mute-audio']});
  try {
    for (const scale of (process.env.COLDEDIT_DPI || '1,1.5,2').split(',').map(Number)) {
      const context = await browser.newContext({viewport: {width: 1440, height: 944}, deviceScaleFactor: scale, acceptDownloads: true});
      const p = await context.newPage(); p.setDefaultTimeout(60000);
      const errors = []; p.on('pageerror', e => errors.push(e.message));
      try {
        await p.goto(process.env.COLDEDIT_WEB_URL || 'http://localhost:8080/');
        await p.waitForFunction(() => document.querySelector('#loading').hidden);
        await p.locator('#file-input').setInputFiles({name: 'pitch.wav', mimeType: 'audio/wav', buffer: fixture()});
        await p.waitForFunction(() => coldeditWeb.diagnostics().frames === 144000); await idle(p);
        await key(p, 'p'); assert.equal((await diag(p)).popup_open, true);
        const original = await diag(p);
        // Invalid settings cannot start a job or change history.
        await input(p, 0, 25); await control(p, 4); assert.equal((await diag(p)).history_states, original.history_states);
        await input(p, 0, 12); await input(p, 2, 3); await input(p, 3, 440);
        await control(p, 4); await idle(p);
        let d = await diag(p);
        assert.equal(d.pitch_semitones, 12); assert.equal(d.formant_semitones, 3); assert.equal(d.formant_base_hz, 440);
        assert.equal(d.preserve_formants, true); assert.equal(d.frames, 144000);
        assert.equal(d.history_states, original.history_states + 1);
        await p.screenshot({path: `artifacts/pitch-controls-${scale}.png`});
        await key(p, 'Escape'); await key(p, 'Control+z');
        assert.equal((await diag(p)).pitch_semitones, 0); assert.equal((await diag(p)).formant_semitones, 0);
        await key(p, 'Control+Shift+z'); assert.equal((await diag(p)).pitch_semitones, 12);
        await exportWav(p, `artifacts/pitch-export-${scale}.wav`, 144000, 880);
        // Applying tempo with pitch active preserves all pitch settings and exact duration.
        await key(p, 't');
        const canvas = await p.locator('#canvas').boundingBox();
        await p.mouse.click(canvas.x + 45, canvas.y + 135, {delay: 40}); await settle(p);
        await input(p, 0, 150, 'stretch_controls'); await control(p, 1, 'stretch_controls'); await idle(p);
        d = await diag(p); assert.equal(d.frames, 115200); assert.equal(d.pitch_semitones, 12); assert.equal(d.formant_semitones, 3);
        await key(p, 'Escape');
        await exportWav(p, `artifacts/pitch-tempo-export-${scale}.wav`, 115200, 880);
        // With pitch active, dragging keeps the last completed audio playing;
        // releasing renders the new timing and commits exactly once.
        const xAt = (state, frame) => canvas.x + 70 + (frame - state.view_start) / state.view_span * 1100;
        for (const fraction of [.25, .5, .75]) {
          d = await diag(p);
          await p.mouse.click(xAt(d, d.frames * fraction), canvas.y + 135, {delay: 40}); await settle(p);
        }
        d = await diag(p); assert.equal(d.stretch_anchors.length, 3);
        const anchors = d.stretch_anchors, history = d.history_states;
        const middle = (anchors[1].beat - d.stretch_start_beat) * 19200;
        await key(p, 'Space');
        await p.mouse.move(xAt(d, middle), canvas.y + 135); await settle(p); await p.mouse.down(); await settle(p);
        await p.mouse.move(xAt(d, middle + 4800), canvas.y + 135, {steps: 8}); await settle(p);
        assert.equal((await diag(p)).playing, true); assert.equal((await diag(p)).history_states, history);
        await p.mouse.up(); await idle(p);
        d = await diag(p); assert.equal(d.history_states, history + 1); assert.equal(d.pitch_semitones, 12);
        assert.equal(d.frames, 115200); assert.equal(d.playing, true); await key(p, 'Space');
        await exportWav(p, `artifacts/pitch-anchor-export-${scale}.wav`, 115200, 880);
        await key(p, 'Control+z'); assert.deepEqual((await diag(p)).stretch_anchors, anchors);
        await key(p, 'p'); await control(p, 5); await idle(p); await key(p, 'Escape');
        d = await diag(p); assert.equal(d.pitch_semitones, 0); assert.equal(d.formant_semitones, 0); assert.equal(d.frames, 115200);
        await key(p, 'Control+z'); assert.equal((await diag(p)).pitch_semitones, 12);
        await p.mouse.click(canvas.x + 45, canvas.y + 135, {delay: 40}); await settle(p);
        await control(p, 4, 'stretch_controls'); await idle(p); await key(p, 'Escape');
        d = await diag(p); assert.equal(d.frames, 144000); assert.equal(d.pitch_semitones, 12);
        if (scale === 1) {
          // Cancellation leaves the previous document and undo history intact.
          const saved = p.waitForEvent('download'); await key(p, 'Control+e'); await saved;
          await idle(p); await p.locator('#dismiss').click(); await settle(p);
          await p.locator('#file-input').setInputFiles({name: 'long-pitch.wav', mimeType: 'audio/wav', buffer: fixture(60)});
          await p.waitForFunction(() => coldeditWeb.diagnostics().frames === 2880000); await idle(p);
          const before = await diag(p);
          await key(p, 'p'); await input(p, 0, 7); await control(p, 4);
          assert.equal((await diag(p)).busy, true); await control(p, 6);
          await p.waitForFunction(() => !coldeditWeb.diagnostics().busy); await settle(p);
          d = await diag(p); assert.equal(d.pitch_semitones, 0); assert.equal(d.frames, before.frames);
          assert.equal(d.history_states, before.history_states); assert.match(d.error, /cancelled/i);
          // Dismiss the error before inspecting the compact controls.
          await key(p, 'Tab'); await key(p, 'Enter'); assert.equal((await diag(p)).error, '');
          await key(p, 'Escape');
        }
        await p.setViewportSize({width: 390, height: 844}); await settle(p); await key(p, 'p');
        await p.screenshot({path: `artifacts/pitch-compact-${scale}.png`});
        assert.equal((await diag(p)).popup_open, true);
        assert.deepEqual(errors, []);
        console.log(`PASS pitch/time UI ${scale}x: controls, validation, undo/redo, tempo, independent resets, actual exported pitch`);
      } catch (error) { await captureFailure(p, `web-pitch-time-failure-${scale}`, error); throw error; }
      finally { await context.close(); }
    }
  } finally { await browser.close(); }
})().catch(e => { console.error(e); process.exitCode = 1; });
