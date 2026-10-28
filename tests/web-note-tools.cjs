const {chromium} = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {settle, captureFailure} = require('./web-test-helpers.cjs');
const diag = p => p.evaluate(() => coldeditWeb.diagnostics());
async function key(p, k) { await p.keyboard.press(k); await settle(p); }
async function idle(p) { await settle(p); await p.waitForFunction(() => !coldeditWeb.diagnostics().busy); await settle(p); assert.equal((await diag(p)).error, ''); }
async function click(p, x, y, options = {}) { const b = await p.locator('#canvas').boundingBox(); await p.mouse.click(b.x + x, b.y + y, {delay: 40, ...options}); await settle(p); }
async function control(p, index, kind = 'note_controls') { const at = (await diag(p))[kind][index]; await click(p, at.x, at.y); }
async function note(p, i, modifier) {
  if (modifier) await p.keyboard.down(modifier);
  const at = (await diag(p)).pitch_notes[i]; await click(p, at.x, at.y);
  if (modifier) await p.keyboard.up(modifier); await settle(p);
}
async function vibrato(p, percent) {
  await p.keyboard.down('Control'); await control(p, 6); await p.keyboard.up('Control');
  await key(p, 'Control+a'); await p.keyboard.type(String(percent)); await key(p, 'Tab'); await idle(p);
}
function fixture() {
  const rate = 48000, frames = rate * 4, b = Buffer.alloc(44 + frames * 4);
  b.write('RIFF'); b.writeUInt32LE(b.length - 8, 4); b.write('WAVEfmt ', 8); b.writeUInt32LE(16, 16);
  b.writeUInt16LE(1, 20); b.writeUInt16LE(2, 22); b.writeUInt32LE(rate, 24); b.writeUInt32LE(rate * 4, 28);
  b.writeUInt16LE(4, 32); b.writeUInt16LE(16, 34); b.write('data', 36); b.writeUInt32LE(frames * 4, 40);
  for (let f = 0; f < frames; f++) {
    const t = f / rate, v = Math.round(10000 * Math.sin(2 * Math.PI * (t < 2 ? 220 : 330) * t + .88 * Math.sin(2 * Math.PI * 5 * t)));
    b.writeInt16LE(v, 44 + f * 4); b.writeInt16LE(-v, 46 + f * 4);
  }
  return b;
}
function depth(b, first, last) {
  const bytes = b.readUInt16LE(34) / 8, stride = bytes * 2, samples = f => b.readIntLE(44 + f * stride, bytes);
  const crossings = [];
  for (let i = Math.floor(first * 48000) + 1; i < last * 48000; i++) {
    const a = samples(i - 1), v = samples(i); if (a <= 0 && v > 0) crossings.push(i - 1 - a / (v - a));
  }
  const pitches = crossings.slice(1).map((v, i) => 69 + 12 * Math.log2(48000 / (v - crossings[i]) / 440));
  assert(pitches.length > 30); const mean = pitches.reduce((a, b) => a + b, 0) / pitches.length;
  return Math.sqrt(pitches.reduce((a, b) => a + (b - mean) ** 2, 0) / pitches.length);
}
async function exported(p, name) {
  const event = p.waitForEvent('download'); await key(p, 'Control+e'); const download = await event;
  const path = `artifacts/${name}.wav`; await download.saveAs(path); await idle(p);
  await p.locator('#dismiss').click(); await settle(p); return fs.readFileSync(path);
}
async function drag(p, i, amount, cancel = false) {
  const d = await diag(p), at = d.pitch_notes[i], b = await p.locator('#canvas').boundingBox();
  await p.mouse.move(b.x + at.x, b.y + at.y); await settle(p); await p.mouse.down(); await settle(p);
  await p.mouse.move(b.x + at.x, b.y + at.y - amount * d.pitch_step, {steps: 8}); await settle(p);
  assert.equal((await diag(p)).note_dragging, i); assert.equal((await diag(p)).history_states, d.history_states);
  if (cancel) await key(p, 'Escape'); await p.mouse.up(); await idle(p);
}
async function boxSelect(p, cancel = false) {
  const d = await diag(p), r = d.pitch_plot, b = await p.locator('#canvas').boundingBox();
  await p.mouse.move(b.x + r.left + 2, b.y + r.top + 8); await p.mouse.down(); await settle(p);
  await p.mouse.move(b.x + r.right - 2, b.y + r.bottom - 8, {steps: 8}); await settle(p);
  assert.equal((await diag(p)).note_marquee, true);
  if (cancel) await key(p, 'Escape'); await p.mouse.up(); await settle(p);
  assert.equal((await diag(p)).history_states, d.history_states);
}
(async () => {
  fs.mkdirSync('artifacts', {recursive: true});
  const source = fixture(), original = depth(source, .3, 1.7);
  const browser = await chromium.launch({channel: process.env.COLDEDIT_BROWSER || 'msedge', headless: true, args: ['--mute-audio']});
  try {
    for (const scale of (process.env.COLDEDIT_DPI || '1,1.5,2').split(',').map(Number)) {
      const context = await browser.newContext({viewport: {width: 1440, height: 944}, deviceScaleFactor: scale, acceptDownloads: true});
      const p = await context.newPage(); p.setDefaultTimeout(60000); const errors = []; p.on('pageerror', e => errors.push(e.message));
      try {
        await p.goto(process.env.COLDEDIT_WEB_URL || 'http://localhost:8080/'); await p.waitForFunction(() => document.querySelector('#loading').hidden);
        await p.locator('#file-input').setInputFiles({name: 'vibrato.wav', mimeType: 'audio/wav', buffer: source});
        await p.waitForFunction(() => coldeditWeb.diagnostics().frames === 192000); await idle(p);
        await key(p, 'n'); await control(p, 0); await idle(p); assert.equal((await diag(p)).pitch_notes.length, 2);
        await note(p, 0); let d = await diag(p); assert.equal(d.notes_selected, 1);
        await vibrato(p, 0); let flat = await diag(p); assert.equal(flat.pitch_notes[0].vibrato, 0); assert.equal(flat.pitch_notes[1].vibrato, 1);
        assert.equal(flat.history_states, d.history_states + 1);
        assert(depth(await exported(p, `vibrato-flat-${scale}`), .3, 1.7) < original * .6);
        await key(p, 'Control+z'); assert.equal((await diag(p)).pitch_notes[0].vibrato, 1);
        await key(p, 'Control+Shift+z'); assert.equal((await diag(p)).pitch_notes[0].vibrato, 0);
        const beforeCancel = await diag(p), slider = beforeCancel.note_controls[6], canvas = await p.locator('#canvas').boundingBox();
        await p.mouse.move(canvas.x + slider.x, canvas.y + slider.y); await p.mouse.down(); await settle(p);
        await p.mouse.move(canvas.x + slider.x + 40, canvas.y + slider.y, {steps: 6}); await settle(p);
        assert.equal((await diag(p)).history_states, beforeCancel.history_states);
        await key(p, 'Escape'); await p.mouse.up(); await idle(p);
        assert.equal((await diag(p)).pitch_notes[0].vibrato, 0); assert.equal((await diag(p)).history_states, beforeCancel.history_states);
        // A selection click that ends numeric entry must not retarget the edit.
        await p.keyboard.down('Control'); await control(p, 6); await p.keyboard.up('Control');
        await key(p, 'Control+a'); await p.keyboard.type('50'); await control(p, 8); await idle(p);
        assert.equal((await diag(p)).pitch_notes[0].vibrato, .5); assert.equal((await diag(p)).pitch_notes[1].vibrato, 1);
        await key(p, 'Escape'); await note(p, 0);
        await note(p, 1, 'Control'); assert.equal((await diag(p)).notes_selected, 2); assert.equal((await diag(p)).note_vibrato_mixed, true);
        await vibrato(p, 200); d = await diag(p); assert(d.pitch_notes.every(n => n.vibrato === 2));
        assert(depth(await exported(p, `vibrato-double-${scale}`), .3, 1.7) > original * 1.4);
        const shifts = d.pitch_notes.map(n => n.shift); await drag(p, 0, 2); d = await diag(p);
        assert(Math.abs(d.pitch_notes[0].shift - shifts[0] - (d.pitch_notes[1].shift - shifts[1])) < .0001);
        assert(d.pitch_notes[0].shift > 1.9);
        await key(p, 'Control+z'); assert.deepEqual((await diag(p)).pitch_notes.map(n => n.shift), shifts);
        await key(p, 'Control+Shift+z'); const moved = await diag(p);
        await drag(p, 0, -2, true); assert.deepEqual((await diag(p)).pitch_notes.map(n => n.shift), moved.pitch_notes.map(n => n.shift));
        await note(p, 1, 'Control'); assert.equal((await diag(p)).notes_selected, 1);
        await boxSelect(p, true); assert.equal((await diag(p)).notes_selected, 1);
        await boxSelect(p); assert.equal((await diag(p)).notes_selected, 2);
        await key(p, 'Escape'); await note(p, 0); await note(p, 1, 'Shift'); assert.equal((await diag(p)).notes_selected, 2);
        await key(p, 'Escape'); await key(p, 'Control+a'); assert.equal((await diag(p)).notes_selected, 2);
        const beforeSplit = await exported(p, `vibrato-before-split-${scale}`); d = await diag(p);
        await control(p, 7); await note(p, 0); await idle(p); let split = await diag(p);
        assert.equal(split.pitch_notes.length, 3); assert.equal(split.history_states, d.history_states + 1);
        assert.equal(split.pitch_notes[0].last, split.pitch_notes[1].first); assert.equal(split.notes_selected, 2);
        assert(split.pitch_notes.every(n => n.vibrato === 2));
        assert.deepEqual(await exported(p, `vibrato-after-split-${scale}`), beforeSplit);
        await key(p, 'Control+z'); assert.equal((await diag(p)).pitch_notes.length, 2);
        await key(p, 'Control+Shift+z'); assert.equal((await diag(p)).pitch_notes.length, 3);
        await key(p, 'Escape'); await note(p, 0); await vibrato(p, 0);
        d = await diag(p); assert.equal(d.pitch_notes[0].vibrato, 0); assert.equal(d.pitch_notes[1].vibrato, 2);
        const halves = await exported(p, `vibrato-halves-${scale}`);
        assert(depth(halves, .25, .75) < original * .6); assert(depth(halves, 1.25, 1.75) > original * 1.4);
        await key(p, 's'); d = await diag(p);
        await click(p, d.pitch_plot.left + 1, d.pitch_notes[0].y); await idle(p);
        assert.equal((await diag(p)).pitch_notes.length, 3); assert.equal((await diag(p)).history_states, d.history_states);
        await key(p, 'Escape'); await key(p, 't'); d = await diag(p);
        await click(p, d.pitch_plot.left - 24, d.pitch_ruler_y + 50);
        await control(p, 0, 'stretch_controls'); await key(p, 'Control+a'); await p.keyboard.type('150'); await key(p, 'Tab');
        await control(p, 1, 'stretch_controls'); await idle(p); await key(p, 'Escape');
        d = await diag(p); assert.equal(d.frames, 153600); assert.equal(d.pitch_notes[1].first, split.pitch_notes[1].first);
        const expectedCut = (d.pitch_notes[2].first + d.pitch_notes[2].last) / 2;
        await click(p, d.pitch_notes[2].x, d.pitch_notes[2].y, {clickCount: 2}); await idle(p);
        const sourceFramesPerPixel = d.view_span / (d.pitch_plot.right - d.pitch_plot.left) * 1.25;
        assert.equal((await diag(p)).pitch_notes.length, 4);
        assert(Math.abs((await diag(p)).pitch_notes[3].first - expectedCut) <= sourceFramesPerPixel + 1, 'split follows the clicked pixel through the tempo map');
        await p.mouse.move(1400, 920); await settle(p); await p.screenshot({path: `artifacts/note-tools-${scale}.png`});
        await p.setViewportSize({width: 390, height: 844}); await settle(p);
        await key(p, 'Control+a'); await vibrato(p, 50); assert((await diag(p)).pitch_notes.every(n => n.vibrato === .5));
        await p.mouse.move(380, 830); await settle(p); await p.screenshot({path: `artifacts/note-tools-compact-${scale}.png`});
        assert.deepEqual(errors, []);
        console.log(`PASS note tools ${scale}x: vibrato audio, multi-select, group drag, marquee/cancel, split invariance, halves, undo, tempo, exports, compact controls`);
      } catch (error) { await captureFailure(p, `web-note-tools-failure-${scale}`, error); throw error; }
      finally { await context.close(); }
    }
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
