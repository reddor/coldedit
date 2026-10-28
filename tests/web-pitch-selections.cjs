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


async function field(p,i,value){await control(p,i,'pitch_controls');await key(p,'Control+a');await p.keyboard.type(String(value));await key(p,'Tab');}
function sample(b,f,c=0){const bytes=b.readUInt16LE(34)/8;return b.readIntLE(44+(f*2+c)*bytes,bytes)/2**(bytes*8-1);}
function frequency(b,a,z){let n=0;for(let f=Math.floor(a*48000)+1;f<z*48000;f++)if(sample(b,f-1)<=0&&sample(b,f)>0)n++;return n/(z-a);}
function unchanged(b,original,a,z){for(let f=a;f<z;f++)for(let c=0;c<2;c++)assert(Math.abs(sample(b,f,c)-sample(original,f,c))<1/32768,'unselected sample '+f);}
(async()=>{
 const browser=await chromium.launch({channel:process.env.COLDEDIT_BROWSER||'msedge',headless:true,args:['--mute-audio']});fs.mkdirSync('artifacts',{recursive:true});
 try {for(const scale of (process.env.COLDEDIT_DPI||'1,1.5,2').split(',').map(Number)){
  const context=await browser.newContext({viewport:{width:1440,height:944},deviceScaleFactor:scale,acceptDownloads:true});
  const p=await context.newPage();p.setDefaultTimeout(60000);const errors=[];p.on('pageerror',e=>errors.push(e.message));
  try{
   await p.goto(process.env.COLDEDIT_WEB_URL||'http://localhost:8080/');await p.waitForFunction(()=>document.querySelector('#loading').hidden);
   const original=fixture();await p.locator('#file-input').setInputFiles({name:'selection-pitch.wav',mimeType:'audio/wav',buffer:original});
   await p.waitForFunction(()=>coldeditWeb.diagnostics().frames===192000);await idle(p);
   const canvas=await p.locator('#canvas').boundingBox();
   await p.mouse.move(canvas.x+207.5,canvas.y+300);await p.mouse.down();await settle(p);
   await p.mouse.move(canvas.x+482.5,canvas.y+300,{steps:10});await p.mouse.up();await settle(p);
   let d=await diag(p);const a=d.selection_start,z=d.selection_end;assert(a>0&&z<96000);
   await key(p,'p');assert.equal((await diag(p)).pitch_scope,'range');await field(p,0,12);
   await control(p,4,'pitch_controls');await idle(p);d=await diag(p);
   assert.equal(d.pitch_semitones,0);assert.equal(d.pitch_regions.length,1);assert.equal(d.pitch_regions[0].first,a);assert.equal(d.pitch_regions[0].last,z);
   assert.equal(d.pitch_regions[0].voice,-1);await p.screenshot({path:'artifacts/pitch-range-dialog-'+scale+'.png'});
   await key(p,'Escape');let audio=await exported(p,'pitch-range-'+scale);
   unchanged(audio,original,0,a);unchanged(audio,original,z,192000);assert(Math.abs(frequency(audio,.7,1.2)-440)<5);
   await key(p,'Control+z');assert.equal((await diag(p)).pitch_regions.length,0);await key(p,'Control+Shift+z');
   await key(p,'p');const history=(await diag(p)).history_states;await control(p,4,'pitch_controls');await idle(p);assert.equal((await diag(p)).history_states,history);
   await control(p,7,'pitch_controls');assert.equal((await diag(p)).pitch_scope,'file');await field(p,0,12);await control(p,4,'pitch_controls');await idle(p);
   assert.equal((await diag(p)).pitch_semitones,12);assert.equal((await diag(p)).pitch_regions[0].semitones,12);
   await control(p,5,'pitch_controls');await idle(p);assert.equal((await diag(p)).pitch_semitones,0);assert.equal((await diag(p)).pitch_regions.length,1);
   await control(p,7,'pitch_controls');await control(p,5,'pitch_controls');await idle(p);assert.equal((await diag(p)).pitch_regions.length,0);
   await key(p,'Escape');await key(p,'n');await control(p,0);await idle(p);assert.equal((await diag(p)).pitch_notes.length,2);
   await note(p,0);await key(p,'p');assert.equal((await diag(p)).pitch_scope,'notes');await field(p,0,7);await field(p,2,3);await control(p,4,'pitch_controls');await idle(p);
   d=await diag(p);assert.equal(d.pitch_semitones,0);assert.equal(d.pitch_regions[0].voice,0);assert(Math.abs(d.pitch_notes[0].audible_midi-d.pitch_notes[0].midi-7)<.001);
   await key(p,'Escape');audio=await exported(p,'pitch-note-selection-'+scale);assert(Math.abs(frequency(audio,.4,1.6)-220*2**(7/12))<5);
   unchanged(audio,original,d.pitch_notes[1].first,192000);
   await key(p,'Escape');await key(p,'p');assert.equal((await diag(p)).pitch_scope,'file');await key(p,'Escape');
   await note(p,0);await control(p,9);await idle(p);assert.equal((await diag(p)).pitch_notes.length,3);
   await key(p,'p');assert.equal((await diag(p)).pitch_scope,'notes');await field(p,2,-5);await control(p,4,'pitch_controls');await idle(p);
   d=await diag(p);assert.equal(d.pitch_regions.find(r=>r.voice===0).formants,3);assert.equal(d.pitch_regions.find(r=>r.voice===1).formants,-5);
   await key(p,'Escape');await note(p,0,'Control');await key(p,'p');assert.equal((await diag(p)).pitch_scope_notes,2);assert.equal((await diag(p)).pitch_scope_mixed,true);
   await field(p,2,2);await control(p,4,'pitch_controls');await idle(p);d=await diag(p);assert(d.pitch_regions.every(r=>r.formants===2&&r.semitones===7));
   await p.setViewportSize({width:390,height:844});await settle(p);await p.screenshot({path:'artifacts/pitch-notes-dialog-compact-'+scale+'.png'});
   await control(p,5,'pitch_controls');await idle(p);assert.equal((await diag(p)).pitch_regions.length,0);await key(p,'Escape');
   await key(p,'Control+z');assert.equal((await diag(p)).pitch_regions.length,2);
   // Reset only one half after a split, retaining the other half's settings.
   await key(p,'Escape');await key(p,'s');await note(p,2);await idle(p);
   assert.equal((await diag(p)).pitch_notes.length,4,'harmony note splits after undo');
   await key(p,'s');await note(p,2);await key(p,'p');assert.equal((await diag(p)).pitch_scope,'notes');
   await control(p,5,'pitch_controls');await idle(p);d=await diag(p);
   assert.equal(d.pitch_regions.filter(r=>r.voice===1).length,1);assert.equal(d.pitch_regions.find(r=>r.voice===1).first,d.pitch_notes[3].first);
   await key(p,'Escape');assert.deepEqual(errors,[]);
   console.log('PASS selected pitch/formants '+scale+'x: audio range isolation, note and harmony scope, mixed settings, whole-file override, reset, split, undo/export, compact dialog');
  }catch(e){await captureFailure(p,'web-pitch-selection-failure-'+scale,e);throw e;}finally{await context.close();}
 }}finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
