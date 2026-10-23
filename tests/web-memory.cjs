// Exercise the real three-minute editing workflow and the recovery UI.
const {chromium}=require('playwright');
const assert=require('node:assert/strict'),fs=require('node:fs');
const MiB=1024*1024, frames=180*44100;
const {settle:tick,captureFailure}=require('./web-test-helpers.cjs');
const diag=p=>p.evaluate(()=>coldeditWeb.diagnostics());
async function key(p,k){await p.keyboard.press(k);await tick(p);}
async function click(p,x,y){await p.mouse.click(x,y,{delay:50});await tick(p);}
async function processMenu(p,row){
  await key(p,'Control+a');await key(p,'F10');await key(p,'Home');
  for(let i=0;i<5;i++)await key(p,'ArrowDown');
  await key(p,'ArrowRight');await key(p,'Home');
  for(let i=0;i<row;i++)await key(p,'ArrowDown');await key(p,'Enter');
}
async function filter(p,row=7){await processMenu(p,row);await click(p,565,606);await p.waitForFunction(()=>!coldeditWeb.diagnostics().busy);await tick(p);}
function fixture(rate=44100){
  const n=180*rate,b=Buffer.alloc(44+n*4);
  b.write('RIFF');b.writeUInt32LE(b.length-8,4);b.write('WAVEfmt ',8);b.writeUInt32LE(16,16);
  b.writeUInt16LE(1,20);b.writeUInt16LE(2,22);b.writeUInt32LE(rate,24);b.writeUInt32LE(rate*4,28);
  b.writeUInt16LE(4,32);b.writeUInt16LE(16,34);b.write('data',36);b.writeUInt32LE(n*4,40);
  for(let i=0;i<n;i++){b.writeInt16LE(Math.round(7000*Math.sin(i*.14)+1000),44+i*4);b.writeInt16LE(Math.round(5000*Math.sin(i*.21)-500),46+i*4);}
  return {name:'three-minutes.wav',mimeType:'audio/wav',buffer:b};
}
(async()=>{
  const browser=await chromium.launch({channel:process.env.COLDEDIT_BROWSER||'msedge',headless:true,args:['--mute-audio']});
  const context=await browser.newContext({viewport:{width:1440,height:944},acceptDownloads:true});
  const p=await context.newPage(),errors=[];p.on('pageerror',e=>errors.push(e.message));p.setDefaultTimeout(60000);
  p.on('dialog',d=>d.accept());
  try {
    await p.goto(process.env.COLDEDIT_WEB_URL||'http://localhost:8080');await p.waitForFunction(()=>document.querySelector('#loading').hidden);
    assert.equal((await diag(p)).audio_limit,128*MiB);
    await p.locator('#file-input').setInputFiles(fixture());await p.waitForFunction(()=>coldeditWeb.diagnostics().frames===7938000);
    await processMenu(p,3); // Gain stores metadata and shares the source.
    assert.equal((await diag(p)).audio_bytes,44+frames*4);
    await filter(p);const before=await diag(p);assert.equal(before.history_states,3);assert.equal(before.audio_bytes,44+frames*12);
    await filter(p,6);const blocked=await diag(p);
    assert.equal(blocked.memory_recovery,true);assert.equal(blocked.audio_bytes,before.audio_bytes);assert.equal(blocked.history_states,3);
    assert.equal(blocked.memory_needed,frames*8);assert.equal(blocked.memory_reclaimable,44+frames*4);
    await p.mouse.move(1200,150);await p.screenshot({path:'artifacts/web-memory-recovery.png'});
    await p.setViewportSize({width:390,height:844});await tick(p);await p.screenshot({path:'artifacts/web-memory-mobile.png'});
    assert.equal((await diag(p)).memory_recovery,true);
    await p.setViewportSize({width:1440,height:944});await tick(p);
    // Cancel must leave all edits and history available, and return to cleanup.
    await key(p,'Escape');assert.equal((await diag(p)).memory_recovery,false);
    assert.equal((await diag(p)).history_states,3);assert.equal((await diag(p)).popup_open,true);
    await click(p,565,606);await p.waitForFunction(()=>coldeditWeb.diagnostics().memory_recovery);
    await click(p,720,622);await p.waitForFunction(()=>!coldeditWeb.diagnostics().busy&&!coldeditWeb.diagnostics().popup_open);
    let recovered=await diag(p);
    assert.equal(recovered.history_states,2);assert.equal(recovered.undo_label,'High-pass filter');assert.equal(recovered.dirty,true);
    assert.equal(recovered.audio_bytes,frames*16);assert.equal(recovered.memory_recovery,false);
    assert.equal(recovered.selection_start,0);assert.equal(recovered.selection_end,frames);
    // Export and compare real samples with an independent reference filter chain.
    const download=p.waitForEvent('download');await key(p,'Control+e');
    await(await download).saveAs('artifacts/web-memory-recovered.wav');await p.waitForFunction(()=>!coldeditWeb.diagnostics().busy);
    const output=fs.readFileSync('artifacts/web-memory-recovered.wav');
    function runFilter(data,cutoff,high){
      const w=2*Math.PI*cutoff/44100,c=Math.cos(w),a=Math.sin(w)/Math.sqrt(2),a0=1+a;
      const b0=(1+(high?c:-c))/(2*a0),b1=(high?-2:2)*b0,b2=b0,a1=-2*c/a0,a2=(1-a)/a0;
      let y=high?0:data[0],z1=y-b0*data[0],z2=b2*data[0]-a2*y;
      return data.map(x=>{y=b0*x+z1;z1=b1*x-a1*y+z2;z2=b2*x-a2*y;return Math.fround(y);});
    }
    for(let c=0;c<2;c++){
      const input=Array.from({length:1000},(_,i)=>Math.round((c?5000:7000)*Math.sin(i*(c?.21:.14))+(c?-500:1000))/32768);
      const expected=runFilter(runFilter(input,12000,false),80,true);
      for(let i=0;i<1000;i++)assert(Math.abs(output.readIntLE(44+(i*2+c)*3,3)/8388608-expected[i])<2e-6);
    }
    await p.locator('#dismiss').click();await tick(p);
    assert.equal((await diag(p)).dirty,false);
    await key(p,'Control+z');assert.equal((await diag(p)).can_undo,false);assert.equal((await diag(p)).can_redo,true);assert.equal((await diag(p)).dirty,true);
    await key(p,'Control+Shift+z');assert.equal((await diag(p)).dirty,false);
    // The same recovery path works for additive paste, preserving its clipboard.
    await key(p,'Control+c');await key(p,'Control+v');await p.waitForFunction(()=>coldeditWeb.diagnostics().memory_recovery);
    assert.equal((await diag(p)).memory_reclaimable,frames*8);
    await click(p,720,622);await p.waitForFunction(()=>!coldeditWeb.diagnostics().busy&&!coldeditWeb.diagnostics().popup_open);
    assert.equal((await diag(p)).undo_label,'Paste into selection');assert.equal((await diag(p)).audio_bytes,frames*16);
    // Clipboard now holds the older filter output: clearing history cannot free it.
    await filter(p);assert.equal((await diag(p)).memory_reclaimable,0);
    await p.screenshot({path:'artifacts/web-memory-clipboard.png'});
    // This dialog is taller because it explains why clearing undo cannot help.
    await click(p,501,399);await tick(p); // 256 MiB radio
    assert.equal((await diag(p)).audio_limit,256*MiB);
    await p.screenshot({path:'artifacts/web-memory-256.png'});
    await click(p,720,578);await p.waitForFunction(()=>!coldeditWeb.diagnostics().busy&&!coldeditWeb.diagnostics().popup_open);
    assert.equal((await diag(p)).history_states,3);assert.equal((await diag(p)).audio_bytes,frames*24);
    assert.equal(await p.locator('#audio-budget').textContent(),'256');
    // Preference survives a reload. Lowering is available again in an empty tab.
    await p.reload();await p.waitForFunction(()=>document.querySelector('#loading').hidden);
    assert.equal((await diag(p)).audio_limit,256*MiB);
    await key(p,'F10');await key(p,'End');await key(p,'ArrowUp');await key(p,'ArrowRight');await key(p,'Home');await key(p,'Enter');await key(p,'Escape');await key(p,'Escape');
    assert.equal((await diag(p)).audio_limit,128*MiB);
    await p.reload();await p.waitForFunction(()=>document.querySelector('#loading').hidden);assert.equal((await diag(p)).audio_limit,128*MiB);
    // At 48 kHz the current + destination float buffers alone exceed 128 MiB.
    await p.locator('#file-input').setInputFiles(fixture(48000));await p.waitForFunction(()=>coldeditWeb.diagnostics().frames===8640000);
    await filter(p);await filter(p,6);const tooLarge=await diag(p);
    assert.equal(tooLarge.memory_reclaimable,44+8640000*4);
    assert(tooLarge.audio_bytes-tooLarge.memory_reclaimable+tooLarge.memory_needed>tooLarge.audio_limit);
    await p.screenshot({path:'artifacts/web-memory-48khz.png'});
    await key(p,'Escape');assert.equal((await diag(p)).history_states,2);
    assert.deepEqual(errors,[]);
    fs.writeFileSync('artifacts/web-memory.json',JSON.stringify({before,blocked,recovered,tooLarge},null,2));
    console.log('PASS memory: three-minute filters, exact exported samples, cancel, clear/retry, dirty state, clipboard, additive paste, 128/256 setting, persistence, insufficient recovery');
  } catch(e){await captureFailure(p,'web-memory-failure',e);throw e;}
  finally {await context.close();await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
