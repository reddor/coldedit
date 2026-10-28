const {chromium} = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {settle, captureFailure} = require('./web-test-helpers.cjs');
const width = 1440, height = 944, frames = 384000;
const diag = p => p.evaluate(() => coldeditWeb.diagnostics());
async function key(p, k) { await p.keyboard.press(k); await settle(p); }
async function click(p, x, y, button = 'left') { await p.mouse.click(x, y, {button, delay: 50}); await settle(p); }
async function control(p, index) { const pos = (await diag(p)).stretch_controls[index]; const canvas = await p.locator('#canvas').boundingBox(); await click(p, pos.x + canvas.x, pos.y + canvas.y); }
async function idle(p) { await settle(p); await p.waitForFunction(() => !coldeditWeb.diagnostics().busy); await settle(p); assert.equal((await diag(p)).error, ''); }
function fixture() {
  const b = Buffer.alloc(44 + frames * 4);
  b.write('RIFF'); b.writeUInt32LE(b.length - 8, 4); b.write('WAVEfmt ', 8); b.writeUInt32LE(16, 16);
  b.writeUInt16LE(1, 20); b.writeUInt16LE(2, 22); b.writeUInt32LE(48000, 24); b.writeUInt32LE(192000, 28);
  b.writeUInt16LE(4, 32); b.writeUInt16LE(16, 34); b.write('data', 36); b.writeUInt32LE(frames * 4, 40);
  for (let f = 0; f < frames; f++) {
    const phase = f % 24000;
    const v = Math.round(7000 * Math.sin(2 * Math.PI * 440 * f / 48000) + (phase < 1000 ? 14000 * Math.exp(-phase / 180) : 0));
    b.writeInt16LE(v, 44 + f * 4); b.writeInt16LE(-v, 46 + f * 4);
  }
  return b;
}
const xAt = (d, f) => 70 + (f - d.view_start) / d.view_span * 1100;
(async () => {
  fs.mkdirSync('artifacts', {recursive: true});
  const browser = await chromium.launch({channel: process.env.COLDEDIT_BROWSER || 'msedge', headless: true, args: ['--mute-audio']});
  try {
    for (const scale of (process.env.COLDEDIT_DPI || '1,1.5,2').split(',').map(Number)) {
      const context = await browser.newContext({viewport: {width, height}, deviceScaleFactor: scale, acceptDownloads: true});
      const p = await context.newPage(); p.setDefaultTimeout(60000);
      const errors = []; p.on('pageerror', e => errors.push(e.message));
      try {
        await p.goto(process.env.COLDEDIT_WEB_URL || 'http://localhost:8080/');
        await p.waitForFunction(() => document.querySelector('#loading').hidden);
        await p.locator('#file-input').setInputFiles({name: 'stretch.wav', mimeType: 'audio/wav', buffer: fixture()});
        await p.waitForFunction(() => coldeditWeb.diagnostics().frames === 384000); await idle(p);
        await key(p, 't'); assert.equal((await diag(p)).stretch_mode, true);
        // Browser chrome is 44 px high; beat ruler is canvas y=115..155.
        const y = 178;
        if (scale === 1) {
          await click(p, 45, y); await control(p, 2); await idle(p);
          assert(Math.abs((await diag(p)).stretch_bpm - 120) < 1, 'source tempo detection');
          assert.equal((await diag(p)).stretch_anchors.length, 0);
          await control(p, 0); await key(p, 'Control+a'); await p.keyboard.type('100'); await key(p, 'Tab'); await control(p, 1);
          assert.equal((await diag(p)).frames, frames, 'unanchored BPM only calibrates the grid');
          await control(p, 0); await key(p, 'Control+a'); await p.keyboard.type('120'); await key(p, 'Tab'); await control(p, 1);
          await key(p, 'Escape');
        }
        await click(p, xAt(await diag(p), 96000), y);
        let d = await diag(p); assert.equal(d.stretch_anchors.length, 1); assert.equal(d.frames, frames);
        const loneHistory = d.history_states;
        await p.mouse.move(xAt(d, 96000), y); await settle(p); await p.mouse.down(); await settle(p);
        await p.mouse.move(xAt(d, 120000), y, {steps: 5}); await settle(p);
        assert.equal((await diag(p)).stretch_dragging, -1, 'one anchor cannot stretch');
        await p.mouse.up(); await settle(p); assert.equal((await diag(p)).history_states, loneHistory);
        await click(p, xAt(d, 192000), y); await click(p, xAt(d, 288000), y);
        d = await diag(p); assert.equal(d.stretch_anchors.length, 3);
        const original = d.stretch_anchors[1], history = d.history_states;
        const start = xAt(d, original.beat * 24000), end = xAt(d, original.beat * 24000 + 24000);
        await key(p, 'Space'); assert.equal((await diag(p)).playing, true);
        await p.mouse.move(start, y); await settle(p); await p.mouse.down(); await settle(p);
        await p.mouse.move(end, y, {steps: 12}); await settle(p);
        assert.equal((await diag(p)).stretch_dragging, 1);
        assert.equal((await diag(p)).playing, true, 'dragging keeps playback active');
        await p.screenshot({path: `artifacts/stretch-drag-${scale}.png`});
        await p.mouse.up(); await idle(p);
        d = await diag(p); assert.equal(d.history_states, history + 1); assert.equal(d.frames, frames);
        assert.equal(d.busy, false, 'stretch render must not disable the editor');
        assert.equal(d.playing, true, 'releasing keeps playback active'); await key(p, 'Space');
        assert(Math.abs(d.stretch_anchors[1].beat - original.beat - 1) < .01);
        assert.equal(d.stretch_anchors[1].source, original.source);
        const moved = d.stretch_anchors;
        // A new drag can start immediately, even while spectral audio finishes.
        await p.mouse.move(xAt(d, moved[0].beat * 24000), y); await settle(p); await p.mouse.down(); await settle(p);
        await p.mouse.move(xAt(d, moved[0].beat * 24000 + 24000), y, {steps: 8}); await settle(p);
        assert.equal((await diag(p)).stretch_dragging, 0);
        assert.equal((await diag(p)).busy, false);
        await p.mouse.up(); await idle(p);
        let outer = await diag(p);
        // Fractional DPI rounds pointer positions to device pixels; use the
        // actual source anchors (diagnostics round to six significant digits).
        assert(Math.abs(outer.frames - (frames - (outer.stretch_anchors[0].beat - moved[0].beat) * 24000)) < 2, 'moving first anchor shortens file');
        assert(Math.abs((outer.stretch_anchors[0].beat - outer.stretch_start_beat) * 24000 - moved[0].source) < 2, 'leading audio keeps its length');
        assert(Math.abs(outer.frames - (outer.stretch_anchors[2].beat - outer.stretch_start_beat) * 24000 - (frames - moved[2].source)) < 2, 'trailing audio keeps its length');
        await key(p, 'Control+z'); assert.deepEqual((await diag(p)).stretch_anchors, moved);
        d = await diag(p);
        await p.mouse.move(xAt(d, moved[2].beat * 24000), y); await settle(p); await p.mouse.down(); await settle(p);
        await p.mouse.move(xAt(d, moved[2].beat * 24000 + 24000), y, {steps: 8}); await settle(p);
        assert.equal((await diag(p)).stretch_dragging, 2);
        await p.mouse.up(); await idle(p); outer = await diag(p);
        assert(Math.abs(outer.frames - (frames + (outer.stretch_anchors[2].beat - moved[2].beat) * 24000)) < 2, 'moving last anchor lengthens file');
        assert(Math.abs(outer.frames - outer.stretch_anchors[2].beat * 24000 - (frames - moved[2].source)) < 2, 'last tail keeps its length');
        await key(p, 'Control+z'); assert.deepEqual((await diag(p)).stretch_anchors, moved);
        await key(p, 'Control+z'); assert.equal((await diag(p)).stretch_anchors[1].beat, original.beat);
        await key(p, 'Control+Shift+z'); assert.deepEqual((await diag(p)).stretch_anchors, moved);
        await key(p, 't'); assert.equal((await diag(p)).stretch_mode, false);
        await key(p, 't'); assert.deepEqual((await diag(p)).stretch_anchors, moved);
        // Escape abandons a drag without adding history.
        d = await diag(p);
        await p.mouse.move(xAt(d, moved[1].beat * 24000), y); await settle(p); await p.mouse.down(); await settle(p);
        await p.mouse.move(xAt(d, moved[1].beat * 24000) + 60, y); await settle(p); await key(p, 'Escape'); await p.mouse.up(); await settle(p);
        assert.deepEqual((await diag(p)).stretch_anchors, moved);
        // Open tempo controls for layout inspection and the BPM workflow below.
        await click(p, 45, y);
        await p.screenshot({path: `artifacts/stretch-controls-${scale}.png`});
        assert.equal((await diag(p)).popup_open, true);
        let outputFrames = frames;
        if (scale === 1) {
          await control(p, 0); await key(p, 'Control+a'); await p.keyboard.type('150'); await key(p, 'Tab');
          await control(p, 1); await idle(p);
          d = await diag(p); outputFrames = frames * .8;
          assert.equal(d.stretch_bpm, 150); assert.equal(d.frames, outputFrames);
          assert.deepEqual(d.stretch_anchors, moved, 'BPM must preserve beat assignments and source anchors');
          const beforeDiscovery = d.history_states;
          await control(p, 3); await idle(p);
          d = await diag(p); assert(d.stretch_anchors.length > 3, 'find transient anchors adds markers');
          assert(d.stretch_anchors.some(a => a.source === moved[1].source && a.beat === moved[1].beat));
          assert.equal(d.frames, outputFrames); assert.equal(d.stretch_bpm, 150);
          assert.equal(d.history_states, beforeDiscovery + 1);
          await p.screenshot({path: 'artifacts/stretch-discovery.png'});
        }
        await key(p, 'Escape');
        if (scale === 1) {
          await key(p, 'Control+z'); assert.deepEqual((await diag(p)).stretch_anchors, moved);
          await key(p, 'Control+z'); assert.equal((await diag(p)).frames, frames);
          await key(p, 'Control+Shift+z'); assert.equal((await diag(p)).frames, outputFrames);
        }
        const downloadEvent = p.waitForEvent('download'); await key(p, 'Control+e');
        const download = await downloadEvent;
        const file = `artifacts/stretch-export-${scale}.wav`; await download.saveAs(file); await idle(p);
        const wav = fs.readFileSync(file); assert.equal(wav.readUInt32LE(40) / 6, outputFrames);
        await p.locator('#dismiss').click(); await settle(p);
        await p.screenshot({path: `artifacts/stretch-ruler-${scale}.png`});
        if (scale === 1) {
          await click(p, 45, y); await control(p, 4); await idle(p);
          assert.equal((await diag(p)).frames, frames); assert.equal((await diag(p)).stretch_bpm, 120);
          assert.equal((await diag(p)).stretch_anchors.length, 0);
          await key(p, 'Escape'); await key(p, 'Control+z');
          assert.equal((await diag(p)).frames, outputFrames); assert.deepEqual((await diag(p)).stretch_anchors, moved);
          const anchorX = xAt(await diag(p), moved[1].beat * 19200);
          await click(p, anchorX, y, 'right'); await click(p, anchorX + 65, y + 45); await idle(p);
          assert.equal((await diag(p)).stretch_anchors.length, 2, 'right-click removes anchor');
          await key(p, 'Control+z'); assert.deepEqual((await diag(p)).stretch_anchors, moved);
          await p.setViewportSize({width: 390, height: 844}); await settle(p);
          const canvas = await p.locator('#canvas').boundingBox();
          await click(p, 58 + 320 * .4, canvas.y + 184);
          assert.equal((await diag(p)).stretch_anchors.length, 4, 'compact beat ruler accepts anchors');
          await p.screenshot({path: 'artifacts/stretch-compact.png'});
          await click(p, 30, canvas.y + 184); await p.screenshot({path: 'artifacts/stretch-compact-controls.png'});
          await key(p, 'Escape');
        }
        assert.deepEqual(errors, []);
        console.log(`PASS stretch UI ${scale}x: add, drag, cancel, undo/redo, mode, controls, WAV export`);
      } catch (error) { await captureFailure(p, `web-stretch-failure-${scale}`, error); throw error; }
      finally { await context.close(); }
    }
  } finally { await browser.close(); }
})().catch(e => { console.error(e); process.exitCode = 1; });
