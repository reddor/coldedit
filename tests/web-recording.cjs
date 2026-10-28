const {chromium} = require('playwright');
const assert = require('node:assert/strict'), fs = require('node:fs');
const {settle, captureFailure} = require('./web-test-helpers.cjs');
const diag = p => p.evaluate(() => coldeditWeb.diagnostics());
function checkWorklet() {
  const vm = require('node:vm');
  let Processor;
  const messages = [];
  vm.runInNewContext(fs.readFileSync('web/recording-worklet.js', 'utf8'), {
    AudioWorkletProcessor: class { constructor() { this.port = {postMessage: (data, transfer = []) => messages.push(structuredClone(data, {transfer}))}; } },
    registerProcessor: (_, type) => { Processor = type; }
  });
  const worklet = new Processor({processorOptions: {channels: 2}});
  const block = [[new Float32Array(128).fill(.25), new Float32Array(128).fill(-.125)]];
  for (let i = 0; i < 17; ++i) assert.equal(worklet.process(block), true);
  worklet.port.onmessage({data: {type: 'stop'}});
  assert.deepEqual(messages.map(m => [m.type, m.frames]), [['samples', 2048], ['samples', 128], ['stopped', undefined]]);
  for (const m of messages.filter(m => m.buffer)) {
    const values = new Float32Array(m.buffer);
    for (let f = 0; f < m.frames; ++f) { assert.equal(values[f * 2], .25); assert.equal(values[f * 2 + 1], -.125); }
  }
  assert.equal(worklet.process(block), false);
  messages.length = 0;
  const blocked = new Processor({processorOptions: {channels: 2}});
  for (let i = 0; i < 128; ++i) assert.equal(blocked.process(block), true);
  assert.equal(blocked.process(block), false);
  assert.equal(messages.length, 9); assert.equal(messages.at(-1).type, 'overflow');
}
async function openRecorder(p) {
  await p.locator('#canvas').focus();
  await p.keyboard.press('Shift+r');
  await p.waitForFunction(() => document.querySelector('#recorder').open);
}
async function waitAudio(p) {
  await p.waitForFunction(() => /Recording (microphone|shared audio)/.test(document.querySelector('#record-status').textContent));
  await p.waitForFunction(() => document.querySelector('#record-time').textContent !== '00:00.0');
  await p.waitForTimeout(250);
}
async function stopped(p) {
  await p.waitForFunction(() => !document.querySelector('#recorder').open && !coldeditWeb.diagnostics().recording);
  await settle(p);
}
async function exportTake(p, name) {
  const pending = p.waitForEvent('download');
  await p.keyboard.press('Control+e');
  const download = await pending;
  const path = `artifacts/recording-${name}.wav`;
  await download.saveAs(path);
  await p.waitForFunction(() => !coldeditWeb.diagnostics().busy);
  await p.locator('#dismiss').click();
  return fs.readFileSync(path);
}
function verifyPCM(b, channels) {
  assert.equal(b.readUInt16LE(22), channels);
  assert.equal(b.readUInt16LE(34), 24);
  const frames = b.readUInt32LE(40) / (channels * 3);
  assert(frames > 4000);
  let signal = 0;
  for (let f = 0; f < frames; ++f) for (let c = 0; c < channels; ++c) {
    const value = b.readIntLE(44 + (f * channels + c) * 3, 3) / 8388608;
    const target = c ? -.125 : .25;
    assert(Math.abs(value - target) < .00001 || value === 0, `unexpected captured sample ${f}, ${c}: ${value}`);
    if (value) ++signal;
  }
  assert(signal > frames * channels / 2, 'real AudioWorklet delivered the known signal');
  return frames;
}
(async () => {
  checkWorklet();
  const browser = await chromium.launch({channel: process.env.COLDEDIT_BROWSER || 'msedge', headless: true,
    args: ['--mute-audio', '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream']});
  const context = await browser.newContext({viewport: {width: 1440, height: 944}, acceptDownloads: true});
  const p = await context.newPage(), errors = [];
  p.on('pageerror', e => errors.push(e.message));
  try {
    await p.goto(process.env.COLDEDIT_WEB_URL || 'http://localhost:8080');
    await p.waitForFunction(() => document.querySelector('#loading').hidden, null, {timeout: 60000});
    await p.evaluate(() => {
      window.captureTest = {mode: 'ok', tracks: [], contexts: [], calls: [], realMic: navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices)};
      const source = async (kind, options) => {
        const t = captureTest;
        t.calls.push({kind, options, activated: navigator.userActivation.isActive});
        if (t.mode === 'denied') throw new DOMException('Denied', 'NotAllowedError');
        if (t.mode === 'missing') throw new DOMException('No device', 'NotFoundError');
        const ctx = new AudioContext({sampleRate: 48000}); t.contexts.push(ctx); await ctx.resume();
        const destination = ctx.createMediaStreamDestination(), merger = ctx.createChannelMerger(2);
        for (const [channel, offset] of [[0, .25], [1, -.125]]) {
          const oscillator = ctx.createConstantSource(); oscillator.offset.value = offset;
          oscillator.connect(merger, 0, channel); oscillator.start();
        }
        merger.connect(destination);
        const stream = new MediaStream(t.mode === 'no-audio' ? [] : destination.stream.getAudioTracks());
        if (kind === 'system') {
          const canvas = document.createElement('canvas'); canvas.width = canvas.height = 16;
          canvas.getContext('2d').fillRect(0, 0, 16, 16);
          stream.addTrack(canvas.captureStream(1).getVideoTracks()[0]);
        }
        t.tracks.push(...stream.getTracks());
        if (t.mode === 'pending') return new Promise(resolve => { t.resolve = () => resolve(stream); });
        return stream;
      };
      navigator.mediaDevices.getUserMedia = options => source('microphone', options);
      navigator.mediaDevices.getDisplayMedia = options => source('system', options);
    });
    // Toolbar entry and native HTML buttons retain the activation for screen sharing.
    await p.mouse.click(213, 76, {delay: 40});
    await p.waitForFunction(() => document.querySelector('#recorder').open);
    await p.locator('#record-system').click(); await waitAudio(p);
    assert.equal(await p.evaluate(() => Module._coldedit_can_import()), 0);
    await p.screenshot({path: 'artifacts/web-recording-active.png'});
    await p.locator('#record-stop').click(); await stopped(p);
    let d = await diag(p);
    assert.equal(d.title, 'Recording.wav'); assert.equal(d.dirty, true); assert.equal(d.channels, 2);
    const capturedFrames = d.frames;
    assert.equal(verifyPCM(await exportTake(p, 'system'), 2), capturedFrames);
    assert.equal(await p.evaluate(() => captureTest.tracks.every(track => track.readyState === 'ended')), true);
    const call = await p.evaluate(() => captureTest.calls[0]);
    assert.equal(call.kind, 'system'); assert.equal(call.activated, true);
    assert.equal(call.options.video, true); assert.equal(call.options.audio, true); assert.equal(call.options.systemAudio, 'include');

    // Permission failures and shares without audio keep the previous document.
    for (const mode of ['denied', 'missing', 'no-audio']) {
      await p.evaluate(mode => { captureTest.mode = mode; }, mode);
      await openRecorder(p);
      await p.locator(mode === 'missing' ? '#record-microphone' : '#record-system').click();
      await stopped(p);
      assert.equal((await diag(p)).frames, capturedFrames);
      assert.equal((await diag(p)).dirty, false);
      assert(await p.locator('#notice').isVisible());
      assert.equal(await p.evaluate(() => captureTest.tracks.every(track => track.readyState === 'ended')), true);
    }

    // Cancelling while the permission request is pending also releases late tracks.
    await p.evaluate(() => { captureTest.mode = 'pending'; });
    await openRecorder(p); await p.locator('#record-system').click();
    await p.waitForFunction(() => !!captureTest.resolve);
    await p.locator('#record-cancel').click(); await stopped(p);
    await p.evaluate(() => captureTest.resolve());
    await p.waitForFunction(() => captureTest.tracks.every(track => track.readyState === 'ended'));
    assert.equal((await diag(p)).frames, capturedFrames);

    // A worklet-load failure releases the already granted capture stream.
    await p.evaluate(() => { captureTest.mode = 'ok'; });
    await p.evaluate(() => {
      const proto = Object.getPrototypeOf(captureTest.contexts[0].audioWorklet);
      captureTest.workletProto = proto; captureTest.addModule = proto.addModule;
      captureTest.ownAddModule = Object.hasOwn(proto, 'addModule');
      proto.addModule = () => Promise.reject(new Error('Simulated worklet load failure'));
    });
    await openRecorder(p); await p.locator('#record-system').click(); await stopped(p);
    await p.evaluate(() => {
      if (captureTest.ownAddModule) captureTest.workletProto.addModule = captureTest.addModule;
      else delete captureTest.workletProto.addModule;
    });
    assert.equal((await diag(p)).frames, capturedFrames);
    assert.equal(await p.evaluate(() => captureTest.tracks.every(track => track.readyState === 'ended')), true);
    await p.evaluate(() => { captureTest.display = navigator.mediaDevices.getDisplayMedia; navigator.mediaDevices.getDisplayMedia = undefined; });
    await openRecorder(p); assert.equal(await p.locator('#record-system').isDisabled(), true);
    await p.locator('#record-cancel').click(); await stopped(p);
    await p.evaluate(() => { navigator.mediaDevices.getDisplayMedia = captureTest.display; });

    // Browser Stop sharing flushes and opens the take; a subsequent discard is inert.
    await p.evaluate(() => { captureTest.mode = 'ok'; });
    await openRecorder(p); await p.locator('#record-system').click(); await waitAudio(p);
    await p.evaluate(() => {
      const track = captureTest.tracks.findLast(track => track.kind === 'video');
      track.stop(); track.dispatchEvent(new Event('ended'));
    });
    await stopped(p);
    verifyPCM(await exportTake(p, 'ended'), 2);
    const saved = await diag(p);
    await openRecorder(p); await p.locator('#record-microphone').click(); await waitAudio(p);
    await p.locator('#record-cancel').click(); await stopped(p);
    assert.equal((await diag(p)).frames, saved.frames); assert.equal((await diag(p)).dirty, false);
    assert.equal((await diag(p)).audio_bytes, saved.audio_bytes);

    // Exercise the browser's real getUserMedia API with Chromium's fake microphone.
    await p.evaluate(() => { navigator.mediaDevices.getUserMedia = captureTest.realMic; });
    await openRecorder(p); await p.locator('#record-microphone').click(); await waitAudio(p);
    await p.locator('#record-stop').click(); await stopped(p);
    d = await diag(p); assert.equal(d.channels, 1); assert(d.frames > 4000); assert.equal(d.dirty, true);
    // Starting another take requires the existing unsaved-changes prompt.
    await p.keyboard.press('Shift+r');
    await p.waitForFunction(() => coldeditWeb.diagnostics().popup_open);
    assert.equal(await p.locator('#recorder').evaluate(e => e.open), false);
    await settle(p);
    await p.keyboard.press('Escape'); await settle(p);
    assert.equal((await diag(p)).popup_open, false);
    assert.equal(await p.locator('#recorder').evaluate(e => e.open), false);
    assert.equal((await diag(p)).dirty, true);
    assert.equal((await diag(p)).frames, d.frames);
    await exportTake(p, 'microphone');
    // A fresh recorder fits the narrow layout and can be cancelled via Escape.
    await p.setViewportSize({width: 320, height: 640}); await settle(p);
    await openRecorder(p);
    const box = await p.locator('#recorder').boundingBox();
    assert(box.x >= 0 && box.width <= 320 && box.y >= 0 && box.height <= 640);
    await p.screenshot({path: 'artifacts/web-recording-mobile.png'});
    await p.keyboard.press('Escape'); await stopped(p);
    // Incremental capture uses the same reservation as retained document audio.
    // Fill it deterministically without waiting minutes for a real-time source.
    const beforeBudget = await diag(p);
    await openRecorder(p);
    const budget = await p.evaluate(() => {
      if (!Module._coldedit_record_begin(48000, 2)) throw new Error('Cannot initialize budget test');
      const bytes = new Uint8Array(4096 * 2 * 4);
      let blocks = 0;
      while (Module.ccall('coldedit_record_append', 'number', ['array', 'number'], [bytes, 4096])) {
        if (++blocks > 4096) throw new Error('Recording exceeded the audio budget');
      }
      const d = coldeditWeb.diagnostics();
      return {blocks, used: d.audio_bytes, limit: d.audio_limit, error: Module.ccall('coldedit_record_error', 'string', [], [])};
    });
    assert(budget.blocks > 0); assert(budget.used <= budget.limit); assert.match(budget.error, /budget/i);
    await p.locator('#record-cancel').click(); await stopped(p);
    assert.equal((await diag(p)).audio_bytes, beforeBudget.audio_bytes);
    assert.equal((await diag(p)).frames, beforeBudget.frames);
    assert.deepEqual(errors, []);
    console.log('PASS recording: worklet tail/backpressure, PCM/export, fake-device microphone, screen options, activation, permissions, missing audio, pending cancellation, track-ended cleanup, discard, dirty guard, mobile layout and memory budget');
  } catch (error) { await captureFailure(p, 'web-recording-failure', error); throw error; }
  finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
