const {chromium}=require('playwright');
const assert=require('node:assert/strict'), fs=require('node:fs');
const url=process.env.COLDEDIT_WEB_URL||'http://localhost:8080/';
const diag=p=>p.evaluate(()=>coldeditWeb.diagnostics());
const tick=p=>p.evaluate(()=>new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r))));
async function key(p,k){await p.keyboard.press(k);await tick(p);}
async function open(p,path,name=path.split('/').pop()){
  await p.locator('#file-input').setInputFiles({name,mimeType:name.endsWith('.mp3')?'audio/mpeg':'audio/flac',buffer:fs.readFileSync(path)});
  await p.waitForFunction(n=>!coldeditWeb.diagnostics().busy&&(coldeditWeb.diagnostics().title===n||coldeditWeb.diagnostics().error),name);
}
async function format(p,index){
  const d=await diag(p), steps=d.selection_end>d.selection_start?9:8;
  await key(p,'F10'); await key(p,'Home');
  for(let i=0;i<steps;i++) await key(p,'ArrowDown');
  await key(p,'ArrowRight');await key(p,'Home');for(let i=0;i<index;i++)await key(p,'ArrowDown');await key(p,'Enter');
  assert.equal((await diag(p)).export_format,index);
}
async function download(p,path,selection=false){
  const pending=p.waitForEvent('download');await key(p,selection?'Control+Shift+e':'Control+e');
  const d=await pending;await d.saveAs(path);await p.waitForFunction(()=>!coldeditWeb.diagnostics().busy);
  await p.locator('#dismiss').click();await tick(p);return {bytes:fs.readFileSync(path),name:d.suggestedFilename()};
}
async function dismiss(p){await key(p,'Tab');await key(p,'Enter');await p.waitForFunction(()=>!coldeditWeb.diagnostics().error);}
(async()=>{
  const browser=await chromium.launch({channel:process.env.COLDEDIT_BROWSER||'msedge',headless:true,args:['--mute-audio']});
  const reports=[];
  async function page(scale=1){const context=await browser.newContext({viewport:{width:1440,height:944},deviceScaleFactor:scale,acceptDownloads:true});
    const p=await context.newPage();p.setDefaultTimeout(60000);const errors=[];p.on('pageerror',e=>errors.push(e.message));
    await p.goto(url);await p.waitForFunction(()=>document.querySelector('#loading').hidden);
    return {context,p,errors};}
  try {
    const {context,p,errors}=await page(1.25);
    try {
      assert((await p.locator('#file-input').getAttribute('accept')).includes('.mp3'));
      await open(p,'tests/fixtures/tone.mp3');let d=await diag(p);
      assert.equal(d.frames,44100);assert.equal(d.sample_rate,44100);assert.equal(d.channels,2);
      assert.equal(d.audio_bytes,44100*8);
      await key(p,'Control+a');await key(p,'i');
      await p.keyboard.down('Alt');await p.mouse.move(300,350);await tick(p);await p.mouse.down();await tick(p);
      await p.mouse.move(650,350,{steps:12});await p.mouse.up();await p.keyboard.up('Alt');await tick(p);
      const selected=await diag(p);assert(selected.selection_end>selected.selection_start);
      const expected=await download(p,'artifacts/codec-selection.wav',true);
      await format(p,4);const flac=await download(p,'artifacts/codec-selection.flac',true);
      assert.equal(flac.bytes.toString('ascii',0,4),'fLaC');assert(flac.name.endsWith('-selection.flac'));
      assert.equal((await diag(p)).dirty,true);assert.equal((await diag(p)).history_states,selected.history_states);
      await key(p,'Control+z'); // Import without discarding edits through the prompt.
      await open(p,'artifacts/codec-selection.flac','roundtrip.flac');
      assert.equal((await diag(p)).frames,selected.selection_end-selected.selection_start);
      await format(p,1);const actual=await download(p,'artifacts/codec-roundtrip.wav');
      assert.deepEqual(actual.bytes,expected.bytes,'FLAC preserves edited selection PCM exactly');
      await format(p,3);await download(p,'artifacts/codec-16.flac');
      await open(p,'artifacts/codec-16.flac');await format(p,0);
      const sixteen=await download(p,'artifacts/codec-16.wav');assert.equal(sixteen.bytes.readUInt16LE(34),16);
      for(let i=0;i<(expected.bytes.length-44)/3;i++) {
        const value=expected.bytes.readIntLE(44+i*3,3);assert.equal(sixteen.bytes.readInt16LE(44+i*2),Math.sign(value)*Math.round(Math.abs(value)/256)||0);
      }
      // A damaged file must leave the current document and budget intact.
      const before=await diag(p);let bad=Buffer.from(flac.bytes);bad[26]^=255;
      await p.locator('#file-input').setInputFiles({name:'bad.flac',mimeType:'audio/flac',buffer:bad});
      await p.waitForFunction(()=>!coldeditWeb.diagnostics().busy&&coldeditWeb.diagnostics().error.length>0);
      d=await diag(p);assert.equal(d.frames,before.frames);assert.equal(d.audio_bytes,before.audio_bytes);await dismiss(p);
      // Fast rejection using STREAMINFO, then actual-output enforcement with total_samples=0.
      for(const name of ['over-budget.flac','unknown-over-budget.flac']) {
        await open(p,'artifacts/codec-fixtures/'+name);d=await diag(p);
        assert(d.error.includes('128 MiB'));assert(d.error.includes('Windows'));
        assert.equal(d.title,before.title);assert.equal(d.audio_bytes,before.audio_bytes);
        assert.equal(await p.evaluate(()=>Module.FS.readdir('/imports').length),2);
        await p.screenshot({path:'artifacts/codec-'+name+'.png'});await dismiss(p);
      }
      // Cancellation releases partial storage and leaves the old document intact.
      await p.locator('#file-input').setInputFiles('artifacts/codec-fixtures/unknown-over-budget.flac');
      await p.waitForFunction(()=>coldeditWeb.diagnostics().busy);
      await p.mouse.click(370,923,{delay:30});await p.waitForFunction(()=>!coldeditWeb.diagnostics().busy);
      d=await diag(p);assert(d.error.toLowerCase().includes('cancel'));assert.equal(d.audio_bytes,before.audio_bytes);await dismiss(p);
      await open(p,'tests/fixtures/tone.mp3','recovered.mp3');assert.equal((await diag(p)).frames,44100);
      assert.deepEqual(errors,[]);reports.push({case:'conversion, selection, failure, cancellation, recovery',diagnostics:await diag(p)});
      console.log('PASS web codecs: MP3 gapless import, FLAC 16/24-bit selection round trips, corruption, known/unknown size limits, cancellation, recovery');
    } catch(e){await p.screenshot({path:'artifacts/web-codecs-failure.png'});throw e;} finally{await context.close();}
    // At the exact limit, import succeeds; another source cannot evade aggregate accounting.
    const limit=await page(2);
    try {
      await open(limit.p,'artifacts/codec-fixtures/at-budget.flac');const before=await diag(limit.p);
      assert.equal(before.error,'');assert.equal(before.frames,128*1024*1024/8);assert.equal(before.audio_bytes,128*1024*1024);
      await open(limit.p,'artifacts/codec-fixtures/small.flac');const after=await diag(limit.p);
      assert(after.error.includes('128 MiB'));assert.equal(after.frames,before.frames);assert.equal(after.audio_bytes,before.audio_bytes);
      assert.deepEqual(limit.errors,[]);reports.push({case:'exact and aggregate memory boundary',diagnostics:after});
      console.log('PASS web codecs: exact 128 MiB boundary and aggregate source reservations');
    } finally{await limit.context.close();}
  } finally{await browser.close();fs.writeFileSync('artifacts/web-codecs.json',JSON.stringify(reports,null,2));}
})().catch(e=>{console.error(e);process.exitCode=1;});
