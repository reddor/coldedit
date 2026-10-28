const {chromium} = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {settle, captureFailure} = require('./web-test-helpers.cjs');
const diag = p => p.evaluate(() => coldeditWeb.diagnostics());
async function key(p, k) { await p.keyboard.press(k); await settle(p); }
async function idle(p) { await settle(p); await p.waitForFunction(() => !coldeditWeb.diagnostics().busy); await settle(p); assert.equal((await diag(p)).error, ''); }
async function control(p, i, type = 'note_controls') {
  const at = (await diag(p))[type][i], box = await p.locator('#canvas').boundingBox();
  await p.mouse.click(box.x + at.x, box.y + at.y, {delay: 40}); await settle(p);
}
function fixture(uncertain = false) {
  const rate = 48000, frames = rate * 3, b = Buffer.alloc(44 + frames * 4);
  b.write('RIFF'); b.writeUInt32LE(b.length - 8, 4); b.write('WAVEfmt ', 8); b.writeUInt32LE(16, 16);
  b.writeUInt16LE(1, 20); b.writeUInt16LE(2, 22); b.writeUInt32LE(rate, 24); b.writeUInt32LE(rate * 4, 28);
  b.writeUInt16LE(4, 32); b.writeUInt16LE(16, 34); b.write('data', 36); b.writeUInt32LE(frames * 4, 40);
  let random = 23;
  for (let f = 0; f < frames; f++) {
    const t = f / rate, hz = 440 * 2 ** (((t < 1.5 ? 57.25 : 64.2) - 69) / 12);
    let v = Math.round(9000 * Math.sin(2 * Math.PI * hz * t));
    if (uncertain && t >= .55 && t < .95) {
      random ^= random << 13; random ^= random >>> 17; random ^= random << 5;
      v = Math.round(8000 * ((random >>> 0) / 0xffffffff - .5));
    }
    if (uncertain && t >= 1.9 && t < 2.1) v = 0;
    b.writeInt16LE(v, 44 + f * 4); b.writeInt16LE(-v, 46 + f * 4);
  }
  return b;
}
async function drag(p, i, semitones, {alt = false, cancel = false} = {}) {
  const d = await diag(p), note = d.pitch_notes[i], box = await p.locator('#canvas').boundingBox();
  if (alt) await p.keyboard.down('Alt');
  await p.mouse.move(box.x + note.x, box.y + note.y); await settle(p); await p.mouse.down(); await settle(p);
  await p.mouse.move(box.x + note.x, box.y + note.y - semitones * d.pitch_step, {steps: 8}); await settle(p);
  assert.equal((await diag(p)).note_dragging, i); assert.equal((await diag(p)).history_states, d.history_states);
  if (cancel) await key(p, 'Escape');
  await p.mouse.up(); if (alt) await p.keyboard.up('Alt'); await idle(p);
  const after = await diag(p);
  if (cancel || !d.can_redo) assert.equal(after.history_states, d.history_states + (cancel ? 0 : 1));
  if (!cancel) {
    assert.notEqual(after.pitch_notes[i].shift, note.shift);
    await key(p, 'Control+z'); assert.equal((await diag(p)).pitch_notes[i].shift, note.shift);
    await key(p, 'Control+Shift+z'); assert.equal((await diag(p)).pitch_notes[i].shift, after.pitch_notes[i].shift);
  }
}
async function exported(p, filename, expectedFrames, notes) {
  const event = p.waitForEvent('download'); await key(p, 'Control+e'); const download = await event;
  await download.saveAs(filename); await idle(p);
  const b = fs.readFileSync(filename); assert.equal(b.readUInt16LE(34), 24);
  assert.equal(b.readUInt32LE(40) / 6, expectedFrames);
  for (const [first, last, hz] of notes) {
    let crossings = 0, diff = 0, energy = 0;
    for (let f = Math.round(first * 48000) + 1; f < last * 48000; f++) {
      const v = b.readIntLE(44 + f * 6, 3), prev = b.readIntLE(44 + (f - 1) * 6, 3);
      if (prev <= 0 && v > 0) crossings++;
      diff += Math.abs(v + b.readIntLE(47 + f * 6, 3)); energy += Math.abs(v);
    }
    assert(Math.abs(crossings / (last - first) - hz) < 4, `pitch ${crossings / (last - first)} vs ${hz}`);
    assert(diff / energy < .004, 'linked stereo');
  }
  await p.locator('#dismiss').click(); await settle(p);
}
(async () => {
  fs.mkdirSync('artifacts', {recursive: true});
  const browser = await chromium.launch({channel: process.env.COLDEDIT_BROWSER || 'msedge', headless: true, args: ['--mute-audio']});
  try {
    for (const scale of (process.env.COLDEDIT_DPI || '1,1.5,2').split(',').map(Number)) {
      const context = await browser.newContext({viewport: {width: 1440, height: 944}, deviceScaleFactor: scale, acceptDownloads: true});
      const p = await context.newPage(); p.setDefaultTimeout(60000); const errors = [];
      p.on('pageerror', e => errors.push(e.message));
      try {
        await p.goto(process.env.COLDEDIT_WEB_URL || 'http://localhost:8080/');
        await p.waitForFunction(() => document.querySelector('#loading').hidden);
        await p.locator('#file-input').setInputFiles({name: 'melody.wav', mimeType: 'audio/wav', buffer: fixture()});
        await p.waitForFunction(() => coldeditWeb.diagnostics().frames === 144000); await idle(p);
        await key(p, 'n'); assert.equal((await diag(p)).pitch_view, true);
        await control(p, 0); await idle(p);
        let d = await diag(p); assert.equal(d.pitch_notes.length, 2); assert.equal(d.pitch_analyzed, true);
        assert(Math.abs(d.pitch_notes[0].midi - 57.25) < .12); assert(Math.abs(d.pitch_notes[1].midi - 64.2) < .12);
        await key(p, 'Control+z'); assert.equal((await diag(p)).pitch_analyzed, false);
        await key(p, 'Control+Shift+z'); assert.equal((await diag(p)).pitch_notes.length, 2);
        await p.mouse.move(1400, 920); await settle(p);
        await p.screenshot({path: `artifacts/pitch-editor-detected-${scale}.png`});
        await drag(p, 0, 2); d = await diag(p);
        assert(Math.abs(d.pitch_notes[0].midi + d.pitch_notes[0].shift - 59) < .01);
        assert.equal(d.pitch_notes[1].shift, 0);
        await exported(p, `artifacts/note-drag-${scale}.wav`, 144000, [[.3, 1.3, 440 * 2 ** (-10 / 12)], [1.8, 2.8, 440 * 2 ** ((64.2 - 69) / 12)]]);
        await key(p, 'Control+z'); assert.equal((await diag(p)).pitch_notes[0].shift, 0);
        await key(p, 'Control+Shift+z'); assert(Math.abs((await diag(p)).pitch_notes[0].shift - d.pitch_notes[0].shift) < .001);
        await drag(p, 0, -3, {cancel: true}); assert.deepEqual((await diag(p)).pitch_notes, d.pitch_notes);
        await drag(p, 0, .3, {alt: true}); const fine = await diag(p);
        assert(Math.abs(fine.pitch_notes[0].shift - d.pitch_notes[0].shift - .3) < .06);
        await control(p, 2); await idle(p); assert.equal((await diag(p)).pitch_notes[0].shift, 0);
        await key(p, 'Escape'); await control(p, 1); await idle(p);
        d = await diag(p);
        assert(Math.abs(d.pitch_notes[0].midi + d.pitch_notes[0].shift - 57) < .01);
        assert(Math.abs(d.pitch_notes[1].midi + d.pitch_notes[1].shift - 64) < .01);
        await exported(p, `artifacts/note-correct-${scale}.wav`, 144000, [[.3, 1.3, 220], [1.8, 2.8, 440 * 2 ** (-5 / 12)]]);
        // Timing is still edited through the shared ruler, retaining source notes.
        await key(p, 't'); const box = await p.locator('#canvas').boundingBox();
        await p.mouse.click(box.x + 45, box.y + (await diag(p)).pitch_ruler_y + 50, {delay: 40}); await settle(p);
        await control(p, 0, 'stretch_controls'); await key(p, 'Control+a'); await p.keyboard.type('150'); await key(p, 'Tab');
        await control(p, 1, 'stretch_controls'); await idle(p); await key(p, 'Escape');
        let timed = await diag(p); assert.equal(timed.frames, 115200);
        assert.equal(timed.pitch_notes[1].first, d.pitch_notes[1].first);
        assert(Math.abs(timed.pitch_notes[1].output_first - d.pitch_notes[1].first * .8) < 1);
        await exported(p, `artifacts/note-tempo-${scale}.wav`, 115200, [[.25, 1.05, 220], [1.45, 2.25, 440 * 2 ** (-5 / 12)]]);
        // Global transposition remains additive to the corrected notes.
        await key(p, 'p'); await control(p, 0, 'pitch_controls'); await key(p, 'Control+a'); await p.keyboard.type('12'); await key(p, 'Tab');
        await control(p, 4, 'pitch_controls'); await idle(p); await key(p, 'Escape');
        assert.equal((await diag(p)).pitch_notes[1].shift, d.pitch_notes[1].shift);
        await exported(p, `artifacts/note-global-${scale}.wav`, 115200, [[.25, 1.05, 440], [1.45, 2.25, 440 * 2 ** (7 / 12)]]);
        await key(p, 'Control+z');
        await key(p, 'n'); assert.equal((await diag(p)).pitch_view, false); await key(p, 'n');
        await p.screenshot({path: `artifacts/pitch-editor-corrected-${scale}.png`});
        await p.setViewportSize({width: 390, height: 844}); await settle(p); await control(p, 4);
        await p.mouse.move(380, 830); await settle(p);
        await p.screenshot({path: `artifacts/pitch-editor-compact-${scale}.png`});
        await drag(p, 1, -1); assert.equal((await diag(p)).pitch_view, true);
        const edited = await diag(p);
        await control(p, 0); await idle(p);
        const redetected = await diag(p);
        assert.equal(redetected.frames, edited.frames); assert.equal(redetected.stretch_bpm, 150);
        assert(redetected.pitch_notes.every(note => note.shift === 0));
        assert(Math.abs(redetected.pitch_notes[0].midi - 57.25) < .12);
        await key(p, 'Control+z'); assert.equal((await diag(p)).pitch_notes[1].shift, edited.pitch_notes[1].shift);
        // Infer a noisy audible gap, leaving actual silence empty. Guessed
        // regions remain part of the draggable note and its exported edit.
        await exported(p, `artifacts/note-before-context-${scale}.wav`, 115200, []);
        await p.locator('#file-input').setInputFiles({name: 'uncertain.wav', mimeType: 'audio/wav', buffer: fixture(true)});
        await p.waitForFunction(() => coldeditWeb.diagnostics().frames === 144000); await idle(p);
        await p.setViewportSize({width: 1440, height: 944}); await settle(p);
        await key(p, 'n'); await control(p, 0); await idle(p);
        const guessed = await diag(p); assert.equal(guessed.pitch_notes.length, 3);
        assert(guessed.pitch_notes[0].inferred > .2);
        assert(guessed.pitch_notes[0].first < .55 * 48000 && guessed.pitch_notes[0].last > .95 * 48000);
        assert(guessed.pitch_notes.every(note => note.last <= 1.95 * 48000 || note.first >= 2.05 * 48000));
        await p.mouse.move(1400, 920); await settle(p);
        await p.screenshot({path: `artifacts/pitch-editor-estimates-${scale}.png`});
        await drag(p, 0, 2); await exported(p, `artifacts/note-estimated-${scale}.wav`, 144000,
          [[.1, .45, 440 * 2 ** (-10 / 12)], [1.02, 1.42, 440 * 2 ** (-10 / 12)]]);
        assert.equal((await diag(p)).pitch_notes[0].inferred, guessed.pitch_notes[0].inferred);
        assert.deepEqual(errors, []);
        console.log(`PASS pitch editor ${scale}x: detection, estimates, silence, drag, fine tuning, correction, cancellation, undo/redo, tempo, transposition, exports, compact layout`);
      } catch (error) { await captureFailure(p, `web-pitch-editor-failure-${scale}`, error); throw error; }
      finally { await context.close(); }
    }
  } finally { await browser.close(); }
})().catch(e => { console.error(e); process.exitCode = 1; });
