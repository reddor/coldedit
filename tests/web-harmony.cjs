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
    const t = f / rate, v = Math.round(10000 * Math.sin(2 * Math.PI * (t < 2 ? 220 : 330) * t));
    b.writeInt16LE(v, 44 + f * 4); b.writeInt16LE(-v, 46 + f * 4);
  }
  return b;
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

async function number(p, index, value) {
  await p.keyboard.down('Control'); await control(p, index); await p.keyboard.up('Control');
  await key(p, 'Control+a'); await p.keyboard.type(String(value)); await key(p, 'Tab'); await idle(p);
}
function amplitude(b, hz, first, last) {
  const bytes = b.readUInt16LE(34) / 8, a = Math.floor(first * 48000), z = Math.floor(last * 48000);
  let re=0, im=0, weight=0;
  for(let f=a; f<z; f++) {
    const w=.5-.5*Math.cos(2*Math.PI*(f-a)/(z-a)), phase=2*Math.PI*hz*f/48000;
    const sample=b.readIntLE(44+f*bytes*2,bytes)/2**(bytes*8-1);
    re+=sample*w*Math.cos(phase); im+=sample*w*Math.sin(phase); weight+=w;
  }
  return 2*Math.hypot(re,im)/weight;
}
(async () => {
  fs.mkdirSync('artifacts',{recursive:true});
  const browser=await chromium.launch({channel:process.env.COLDEDIT_BROWSER||'msedge',headless:true,args:['--mute-audio']});
  try {
    for(const scale of (process.env.COLDEDIT_DPI||'1,1.5,2').split(',').map(Number)) {
      const context=await browser.newContext({viewport:{width:scale===1.5?1280:1440,height:944},deviceScaleFactor:scale,acceptDownloads:true});
      const p=await context.newPage(); p.setDefaultTimeout(60000); const errors=[]; p.on('pageerror',e=>errors.push(e.message));
      try {
        await p.goto(process.env.COLDEDIT_WEB_URL||'http://localhost:8080/'); await p.waitForFunction(()=>document.querySelector('#loading').hidden);
        await p.locator('#file-input').setInputFiles({name:'harmony.wav',mimeType:'audio/wav',buffer:fixture()});
        await p.waitForFunction(()=>coldeditWeb.diagnostics().frames===192000); await idle(p);
        await key(p,'n'); await control(p,0); await idle(p); assert.equal((await diag(p)).pitch_notes.length,2);
        await note(p,0); await number(p,10,4); let d=await diag(p);
        await control(p,9); await idle(p); let added=await diag(p);
        assert.equal(added.pitch_notes.length,3); assert.equal(added.history_states,d.history_states+1);
        assert.equal(added.pitch_notes[2].voice,1); assert.equal(added.pitch_notes[2].shift,4); assert.equal(added.pitch_notes[2].selected,true);
        assert.equal(added.pitch_notes[0].selected,false);
        await key(p,'Control+z'); assert.equal((await diag(p)).pitch_notes.length,2);
        await key(p,'Control+Shift+z'); assert.equal((await diag(p)).pitch_notes.length,3);
        await note(p,0); await number(p,10,7); await control(p,9); await idle(p);
        d=await diag(p); assert.equal(d.pitch_notes.length,4); assert.equal(d.pitch_notes[3].voice,2); assert.equal(d.pitch_notes[3].shift,7);
        const chord=await exported(p,'harmony-triad-'+scale);
        for(const hz of [220,220*2**(4/12),220*2**(7/12)]) assert(amplitude(chord,hz,.4,1.6)>.06,'missing harmony '+hz);
        assert(amplitude(chord,330,2.4,3.6)>.25); assert(amplitude(chord,660,2.4,3.6)<.01);
        await number(p,12,50); d=await diag(p); assert.equal(d.pitch_notes[3].gain,.5); assert.equal(d.pitch_notes[2].gain,.35);
        const slider=d.note_controls[12], canvas=await p.locator('#canvas').boundingBox();
        await p.mouse.move(canvas.x+slider.x,canvas.y+slider.y); await p.mouse.down(); await settle(p);
        await p.mouse.move(canvas.x+slider.x+30,canvas.y+slider.y,{steps:6}); await settle(p);
        await key(p,'Escape'); await p.mouse.up(); await idle(p);
        assert.equal((await diag(p)).pitch_notes[3].gain,.5); assert.equal((await diag(p)).history_states,d.history_states);
        await number(p,12,0); const muted=await exported(p,'harmony-muted-'+scale);
        assert(amplitude(muted,220*2**(7/12),.4,1.6)<.015); assert(amplitude(muted,220*2**(4/12),.4,1.6)>.06);
        await key(p,'Control+z'); assert.equal((await diag(p)).pitch_notes[3].gain,.5);
        await vibrato(p,50); assert.equal((await diag(p)).pitch_notes[3].vibrato,.5); assert.equal((await diag(p)).pitch_notes[0].vibrato,1);
        await note(p,2); const before=await exported(p,'harmony-before-split-'+scale);
        await key(p,'s'); await note(p,2); await idle(p); await key(p,'Escape'); d=await diag(p);
        assert.equal(d.pitch_notes.length,5); assert.equal(d.pitch_notes[2].last,d.pitch_notes[3].first);
        assert.equal(d.pitch_notes[2].voice,1); assert.equal(d.pitch_notes[3].voice,1);
        assert.deepEqual(await exported(p,'harmony-after-split-'+scale),before);
        await note(p,2); await drag(p,2,2); d=await diag(p);
        assert.equal(d.pitch_notes[0].shift,0); assert.equal(d.pitch_notes[3].shift,4); assert(d.pitch_notes[2].shift>5.9);
        await key(p,'Delete'); await idle(p); assert.equal((await diag(p)).pitch_notes.length,4);
        await key(p,'Control+z'); assert.equal((await diag(p)).pitch_notes.length,5);
        await note(p,0); await note(p,4,'Control'); d=await diag(p); assert.equal(d.notes_selected,2);
        await drag(p,0,2); let moved=await diag(p);
        assert(Math.abs(moved.pitch_notes[4].shift-d.pitch_notes[4].shift-(moved.pitch_notes[0].shift-d.pitch_notes[0].shift))<.0001);
        await key(p,'Control+z'); await key(p,'Escape');
        await key(p,'t'); d=await diag(p); await click(p,d.pitch_plot.left-24,d.pitch_ruler_y+50);
        await control(p,0,'stretch_controls'); await key(p,'Control+a'); await p.keyboard.type('150'); await key(p,'Tab');
        await control(p,1,'stretch_controls'); await idle(p); await key(p,'Escape');
        d=await diag(p); assert.equal(d.frames,153600); assert.equal(d.pitch_notes[4].output_last,d.pitch_notes[0].output_last);
        const warped=await exported(p,'harmony-tempo-'+scale); assert(amplitude(warped,220*2**(7/12),.4,1.2)>.06);
        await p.mouse.move(1400,920); await settle(p); await p.screenshot({path:'artifacts/harmony-'+scale+'.png'});
        await p.setViewportSize({width:390,height:844}); await settle(p);
        await note(p,0); await number(p,10,-12); await control(p,9); await idle(p);
        d=await diag(p); assert.equal(d.pitch_notes.length,6); assert.equal(d.pitch_notes[5].voice,3); assert.equal(d.pitch_notes[5].shift,-12);
        await number(p,12,25); assert.equal((await diag(p)).pitch_notes[5].gain,.25);
        await p.mouse.move(380,830); await settle(p); await p.screenshot({path:'artifacts/harmony-compact-'+scale+'.png'});
        await key(p,'Control+a'); await control(p,11); await idle(p); assert.equal((await diag(p)).pitch_notes.length,2);
        await key(p,'Control+z'); assert.equal((await diag(p)).pitch_notes.length,6);
        await control(p,0); await idle(p); assert.equal((await diag(p)).pitch_notes.length,2);
        await key(p,'Control+z'); assert.equal((await diag(p)).pitch_notes.length,6);
        // Coincident notes remain individually selectable and draggable.
        await key(p,'Control+a'); await control(p,11); await idle(p);
        await note(p,0); await number(p,10,0); await control(p,9); await idle(p);
        d=await diag(p); assert.equal(d.pitch_notes[2].shift,0); assert.equal(d.pitch_notes[2].selected,true);
        await click(p,d.pitch_notes[0].x,d.pitch_notes[0].y,{button:'right'}); await control(p,13); await settle(p);
        assert.equal((await diag(p)).pitch_notes[0].selected,true); assert.equal((await diag(p)).pitch_notes[2].selected,false);
        await drag(p,0,2); d=await diag(p); assert(d.pitch_notes[0].shift>1.9); assert.equal(d.pitch_notes[2].shift,0);
        // Grow past the old three-voice ceiling, then select an overlapping H8.
        await key(p,'Control+z'); await key(p,'Escape'); await note(p,0); await number(p,10,0);
        for(let voice=2; voice<=8; voice++) {
          await control(p,9); await idle(p); d=await diag(p);
          assert.equal(d.pitch_notes.length,voice+2); assert.equal(d.pitch_notes.at(-1).voice,voice);
          assert.equal(d.pitch_notes.at(-1).selected,true);
          await number(p,12,10); // leave headroom for all the unison copies
        }
        await key(p,'Control+z'); await idle(p); assert.equal((await diag(p)).pitch_notes.length,9);
        await key(p,'Control+Shift+z'); await idle(p); assert.equal((await diag(p)).pitch_notes.length,10);
        d=await diag(p); await click(p,d.pitch_notes[0].x,d.pitch_notes[0].y,{button:'right'});
        await control(p,21); await settle(p); assert.equal((await diag(p)).pitch_notes[9].selected,true);
        await number(p,12,15); assert.equal((await diag(p)).pitch_notes[9].gain,.15);
        await drag(p,9,7); d=await diag(p); assert(d.pitch_notes[9].shift>6.9); assert.equal(d.pitch_notes[0].shift,0);
        const many=await exported(p,'harmony-eight-voices-'+scale);
        assert(amplitude(many,220*2**(7/12),.3,1.2)>.02);
        await key(p,'s'); await note(p,9); await idle(p); await key(p,'s');
        d=await diag(p); assert.equal(d.pitch_notes.length,11);
        assert.equal(d.pitch_notes[9].voice,8); assert.equal(d.pitch_notes[10].voice,8); assert.equal(d.notes_selected,2);
        await p.mouse.move(380,830); await settle(p); await p.screenshot({path:'artifacts/harmony-eight-voices-'+scale+'.png'});
        await key(p,'Delete'); await idle(p); assert.equal((await diag(p)).pitch_notes.length,9);
        await key(p,'Control+z'); await idle(p); assert.equal((await diag(p)).pitch_notes.length,11);
        await control(p,0); await idle(p); assert.equal((await diag(p)).pitch_notes.length,2);
        await key(p,'Control+z'); await idle(p); assert.equal((await diag(p)).pitch_notes.length,11);
        assert.deepEqual(errors,[]); console.log('PASS harmony '+scale+'x: triad and eight-voice audio, independent voices, overlap selection, levels/mute/cancel, vibrato, split, drag, selection, removal, tempo, undo, export and compact controls');
      } catch(error) { await captureFailure(p,'web-harmony-failure-'+scale,error); throw error; }
      finally {await context.close();}
    }
  } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
