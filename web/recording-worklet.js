'use strict';
// Bounded transferable buffers keep a backgrounded/blocked UI from accumulating
// unbounded PCM messages. No sound is connected to the user's speakers.
class ColdEditCapture extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.channels = options.processorOptions.channels;
    this.pool = Array.from({length: 8}, () => new Float32Array(2048 * this.channels));
    this.block = this.pool.pop(); this.frames = 0; this.active = true;
    this.port.onmessage = ({data}) => {
      if (data.type === 'recycle') this.pool.push(new Float32Array(data.buffer));
      if (data.type === 'stop' && this.active) {
        this.flush(); this.active = false; this.port.postMessage({type: 'stopped'});
      }
    };
  }
  flush() {
    if (!this.frames) return;
    const buffer = this.block.buffer;
    this.port.postMessage({type: 'samples', frames: this.frames, buffer}, [buffer]);
    this.frames = 0; this.block = null;
  }
  process(inputs) {
    if (!this.active) return false;
    const input = inputs[0];
    if (!input?.length) return true;
    for (let f = 0; f < input[0].length; ++f) {
      if (!this.block) this.block = this.pool.pop();
      if (!this.block) {
        this.active = false;
        this.port.postMessage({type: 'overflow'});
        return false;
      }
      for (let c = 0; c < this.channels; ++c)
        this.block[this.frames * this.channels + c] = (input[c] || input[0])[f];
      if (++this.frames === 2048) this.flush();
    }
    return true;
  }
}
registerProcessor('coldedit-capture', ColdEditCapture);
