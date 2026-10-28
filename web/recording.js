'use strict';
(() => {
  const dialog = document.getElementById('recorder');
  const mic = document.getElementById('record-microphone'), system = document.getElementById('record-system');
  const stop = document.getElementById('record-stop'), cancel = document.getElementById('record-cancel');
  const status = document.getElementById('record-status'), time = document.getElementById('record-time');
  const meter = document.getElementById('record-level');
  let session = null;
  const coreError = () => Module.ccall('coldedit_record_error', 'string', [], []);
  const stopTracks = stream => stream?.getTracks().forEach(track => { track.onended = null; track.stop(); });
  function release(s) {
    clearTimeout(s.timeout);
    stopTracks(s.stream);
    if (s.node) { s.node.port.onmessage = null; s.node.port.close(); s.node.disconnect(); }
    s.source?.disconnect();
    if (s.context && s.context.state !== 'closed') void s.context.close().catch(() => {});
  }
  function finish(keep, message = '') {
    const s = session;
    if (s) { s.cancelled = true; release(s); }
    session = null;
    const ok = Module._coldedit_record_end(keep ? 1 : 0);
    dialog.close(); canvas.focus();
    if (!ok) showNotice(coreError() || 'Could not finish the recording. Your previous audio is still open.');
    else if (message) showNotice(message);
  }
  function stopAndKeep(message = '') {
    const s = session;
    if (!s || s.stopping) return;
    s.stopping = true; s.message = message;
    stop.disabled = true; status.textContent = 'Finishing recording…';
    if (!s.node || s.context.state !== 'running') { finish(true, message); return; }
    // The worklet flushes the last partial block before acknowledging Stop.
    s.node.port.postMessage({type: 'stop'});
    s.timeout = setTimeout(() => finish(true, message || 'The audio device stopped responding. The received audio has been kept.'), 2000);
  }
  function describeError(error) {
    if (error.name === 'NotAllowedError') return 'Recording permission was denied or sharing was cancelled. Allow access in your browser and try again.';
    if (error.name === 'NotFoundError') return 'No microphone was found. Connect an input device and try again.';
    if (error.name === 'NotReadableError') return 'The audio source could not be read. Check your device and operating system permissions.';
    return error.message || 'Could not start recording.';
  }
  async function start(kind) {
    if (session || !dialog.open) return;
    const s = {cancelled: false, frames: 0, channels: kind === 'microphone' ? 1 : 2};
    session = s; mic.disabled = system.disabled = true;
    status.textContent = kind === 'microphone' ? 'Waiting for microphone access…' : 'Choose a source and enable Share audio…';
    try {
      // Invoke capture directly from the button event, before any await, to keep
      // the transient user activation required by getDisplayMedia.
      s.context = new AudioContext();
      const resumed = s.context.resume().catch(error => error);
      const stream = await (kind === 'microphone'
        ? navigator.mediaDevices.getUserMedia({audio: {channelCount: 1, echoCancellation: false, noiseSuppression: false, autoGainControl: false}, video: false})
        : navigator.mediaDevices.getDisplayMedia({video: true, audio: true, systemAudio: 'include', selfBrowserSurface: 'exclude', surfaceSwitching: 'exclude'}));
      if (s.cancelled) { stopTracks(stream); return; }
      s.stream = stream;
      if (!stream.getAudioTracks().length) throw new Error('No audio was shared. Choose a source with audio and enable “Share audio” in the browser picker. Try sharing a browser tab if system audio is unavailable.');
      for (const track of stream.getTracks()) track.onended = () => stopAndKeep('Sharing or microphone capture ended. The recorded audio has been kept.');
      const resumeResult = await resumed;
      if (s.cancelled) return;
      if (resumeResult instanceof Error || s.context.state !== 'running') throw new Error('The audio device could not start. Try recording again.');
      await s.context.audioWorklet.addModule('recording-worklet.js');
      if (s.cancelled) return;
      if (stream.getAudioTracks().every(track => track.readyState === 'ended')) throw new Error('The audio source ended before recording could start.');
      if (!Module._coldedit_record_begin(s.context.sampleRate, s.channels)) throw new Error(coreError() || 'The editor could not start recording.');
      s.source = s.context.createMediaStreamSource(new MediaStream(stream.getAudioTracks()));
      s.node = new AudioWorkletNode(s.context, 'coldedit-capture', {
        numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
        channelCount: s.channels, channelCountMode: 'explicit', processorOptions: {channels: s.channels}
      });
      s.node.onprocessorerror = () => stopAndKeep('Audio capture stopped unexpectedly. The received audio has been kept.');
      s.node.port.onmessage = ({data}) => {
        if (s.cancelled) return;
        if (data.type === 'stopped') { finish(true, s.message); return; }
        if (data.type === 'overflow') { finish(true, 'Recording stopped because capture could not keep up. The received audio has been kept.'); return; }
        if (data.type !== 'samples') return;
        try {
          const bytes = new Uint8Array(data.buffer, 0, data.frames * s.channels * 4);
          const ok = Module.ccall('coldedit_record_append', 'number', ['array', 'number'], [bytes, data.frames]);
          if (!ok) { finish(true, (coreError() || 'Recording reached its storage limit.') + ' The received audio has been kept.'); return; }
          let peak = 0;
          for (const sample of new Float32Array(data.buffer, 0, data.frames * s.channels)) peak = Math.max(peak, Math.abs(sample));
          meter.value = peak; s.frames += data.frames;
          const seconds = s.frames / s.context.sampleRate;
          time.textContent = String(Math.floor(seconds / 60)).padStart(2, '0') + ':' + (seconds % 60).toFixed(1).padStart(4, '0');
          s.node.port.postMessage({type: 'recycle', buffer: data.buffer}, [data.buffer]);
        } catch (error) { finish(true, 'Recording stopped: ' + error.message); }
      };
      s.source.connect(s.node); s.node.connect(s.context.destination); // Worklet output is silence.
      status.textContent = kind === 'microphone' ? 'Recording microphone' : 'Recording shared audio';
      stop.hidden = false; stop.disabled = false; cancel.textContent = 'Discard take';
    } catch (error) { if (!s.cancelled) finish(false, describeError(error)); }
  }
  window.coldeditRecorder = {
    show() {
      if (dialog.open) return;
      session = null; time.textContent = '00:00.0'; meter.value = 0;
      const supported = window.isSecureContext && !!window.AudioWorkletNode && !!window.AudioContext;
      mic.disabled = !supported || !navigator.mediaDevices?.getUserMedia;
      system.disabled = !supported || !navigator.mediaDevices?.getDisplayMedia;
      status.textContent = !supported ? 'Recording requires HTTPS or localhost and a browser with AudioWorklet support.'
        : system.disabled ? 'Screen-sharing audio is unavailable in this browser. Use the microphone or try a desktop browser with screen sharing.' : 'Choose an audio source.';
      stop.hidden = true; cancel.textContent = 'Cancel';
      notice.hidden = true; dialog.showModal();
    },
    abort() {
      if (session) { session.cancelled = true; release(session); session = null; }
      dialog.close();
    }
  };
  mic.addEventListener('click', () => { void start('microphone'); });
  system.addEventListener('click', () => { void start('system'); });
  stop.addEventListener('click', () => stopAndKeep());
  cancel.addEventListener('click', () => finish(false));
  dialog.addEventListener('cancel', event => { event.preventDefault(); if (session?.node) stopAndKeep(); else finish(false); });
  window.addEventListener('pagehide', () => { if (session) { session.cancelled = true; release(session); } });
})();
