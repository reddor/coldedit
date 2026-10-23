// End-to-end sample validation through real canvas input and browser downloads.
const {chromium} = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const frames = 192000, width = 1440, height = 944;
const diag = page => page.evaluate(() => window.coldeditWeb.diagnostics());
const settle = page => page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));
async function press(page, key) { await page.keyboard.press(key); await settle(page); }
async function click(page, x, y) { await page.mouse.click(x,y,{delay:50}); await settle(page); }
async function drag(page,x1,y1,x2,y2) {
  await page.mouse.move(x1,y1); await settle(page); await page.mouse.down(); await settle(page);
  await page.mouse.move(x2,y2,{steps:12}); await settle(page); await page.mouse.up(); await settle(page);
}
function fixture() {
  const b=Buffer.alloc(44+frames*4);
  b.write('RIFF'); b.writeUInt32LE(b.length-8,4); b.write('WAVEfmt ',8); b.writeUInt32LE(16,16);
  b.writeUInt16LE(1,20); b.writeUInt16LE(2,22); b.writeUInt32LE(48000,24); b.writeUInt32LE(192000,28);
  b.writeUInt16LE(4,32); b.writeUInt16LE(16,34); b.write('data',36); b.writeUInt32LE(frames*4,40);
  for(let f=0;f<frames;f++) { b.writeInt16LE(24576,44+f*4); b.writeInt16LE(-8192,46+f*4); }
  return b;
}
async function download(page, key, path, menu=false) {
  const pending=page.waitForEvent('download');
  if(menu) { await click(page,80,76); await click(page,125,203); }
  else await press(page,key);
  const result=await pending; await result.saveAs(path);
  await page.waitForFunction(()=>!window.coldeditWeb.diagnostics().busy);
  await page.locator('#dismiss').click(); await settle(page);
  return {bytes:fs.readFileSync(path),name:result.suggestedFilename()};
}
function verifyWav(bytes, count, sample) {
  assert.equal(bytes.readUInt16LE(34),24); assert.equal(bytes.readUInt16LE(22),2);
  assert.equal(bytes.readUInt32LE(24),48000); assert.equal(bytes.readUInt32LE(40),count*6);
  assert.equal(bytes.length,44+count*6);
  for(let f=0;f<count;f++) for(let c=0;c<2;c++) {
    const expected=Math.round(Math.fround(sample(f,c))*8388608);
    assert(Math.abs(bytes.readIntLE(44+f*6+c*3,3)-expected)<=1,`sample ${f}, channel ${c}`);
  }
}
async function duration(page,value,samples=false) {
  // Modal is centered in the canvas; fields retain their logical positions at every DPR.
  await click(page,width/2-100,(height+44)/2+(samples?27:-13));
  await press(page,'Control+a'); await page.keyboard.type(String(value),{delay:35}); await settle(page);
}
(async()=>{
  const browser=await chromium.launch({channel:process.env.COLDEDIT_BROWSER||'msedge',headless:true,args:['--mute-audio']});
  const reports=[];
  try {
    for(const scale of (process.env.COLDEDIT_DPI||'1,1.25,1.5,2').split(',').map(Number)) {
      const context=await browser.newContext({viewport:{width,height},deviceScaleFactor:scale,acceptDownloads:true});
      const page=await context.newPage(),errors=[];
      page.on('pageerror',e=>errors.push(e.message));
      try {
        await page.goto(process.env.COLDEDIT_WEB_URL||'http://localhost:8080/');
        await page.waitForFunction(()=>document.querySelector('#loading').hidden);
        await page.locator('#file-input').setInputFiles({name:'features.wav',mimeType:'audio/wav',buffer:fixture()});
        await page.waitForFunction(()=>window.coldeditWeb.diagnostics().frames===192000);
        await press(page,'i'); await press(page,'o');
        assert.equal((await diag(page)).history_states,1,'fades require a selection');
        await drag(page,300,350,650,350);
        const selected=await diag(page), a=selected.selection_start,b=selected.selection_end,n=b-a;
        assert(n>1);
        const clean=await download(page,'Control+Shift+e',`artifacts/web-clean-selection-${scale}.wav`);
        verifyWav(clean.bytes,n,(f,c)=>c?-.25:.75); assert.equal((await diag(page)).dirty,false);
        await press(page,'i'); await press(page,'o');
        await press(page,'Control+z'); await press(page,'Control+Shift+z');
        const before=await diag(page);
        const faded=await download(page,'Control+Shift+e',`artifacts/web-faded-selection-${scale}.wav`,true);
        assert.equal(faded.name,'features-selection.wav');
        verifyWav(faded.bytes,n,(f,c)=>(c?-.25:.75)*(f/(n-1))*(1-f/(n-1)));
        const after=await diag(page);
        for(const key of ['frames','history_states','dirty','selection_start','selection_end']) assert.equal(after[key],before[key],key);
        assert.equal(after.dirty,true,'selection export must not mark the document saved');
        // Seconds validation and cancel must not edit the document.
        await press(page,'Home'); await press(page,'Insert'); await duration(page,0); await press(page,'Enter');
        assert.equal((await diag(page)).popup_open,true); assert.equal((await diag(page)).frames,frames);
        await press(page,'Escape'); assert.equal((await diag(page)).history_states,before.history_states);
        await press(page,'Insert'); await duration(page,1/48000); await press(page,'Enter');
        assert.equal((await diag(page)).frames,frames+1,'seconds round to the nearest sample');
        await press(page,'Control+z');
        // A selection is present: insertion must still preserve its audio.
        await press(page,'Control+a'); await press(page,'Insert'); await duration(page,137,true);
        await page.screenshot({path:`artifacts/web-insert-silence-${scale}.png`});
        await press(page,'Enter');
        assert.equal((await diag(page)).frames,frames+137);
        assert.equal((await diag(page)).selection_start,0); assert.equal((await diag(page)).selection_end,137);
        await press(page,'Control+z'); assert.equal((await diag(page)).frames,frames);
        await press(page,'Control+Shift+z'); assert.equal((await diag(page)).frames,frames+137);
        // Undo/redo preserves this selected interval because both timelines contain it.
        assert.equal((await diag(page)).selection_end,137);
        await press(page,'Control+c');
        const silent=await download(page,'Control+Shift+e',`artifacts/web-silence-selection-${scale}.wav`);
        verifyWav(silent.bytes,137,()=>0);
        await press(page,'Control+a');
        // Smooth curve and Fade in through the inspector, then export the entire edited document.
        await click(page,1290,569); await press(page,'ArrowDown'); await press(page,'Enter');
        await click(page,1245,610);
        await page.mouse.move(900,120); await settle(page);
        await page.screenshot({path:`artifacts/web-features-${scale}.png`});
        const full=await download(page,'Control+e',`artifacts/web-features-full-${scale}.wav`);
        verifyWav(full.bytes,frames+137,(f,c)=>{
          if(f<137) return 0;
          const original=f-137, u=f/(frames+136), smooth=u*u*(3-2*u);
          const v=original>=a&&original<b ? (original-a)/(n-1) : null;
          return (c?-.25:.75)*smooth*(v===null?1:v*(1-v));
        });
        assert.equal((await diag(page)).dirty,false);
        // A large silent interval costs metadata, and limits depend on the exported range.
        await press(page,'End'); await press(page,'Escape');
        const heap=(await diag(page)).resident_bytes;
        await press(page,'Insert'); await duration(page,30000000,true); await press(page,'Enter');
        assert.equal((await diag(page)).frames,frames+137+30000000);
        assert.equal((await diag(page)).resident_bytes,heap,'silence must not allocate a PCM buffer');
        await press(page,'End'); await press(page,'Escape'); await press(page,'Control+v');
        const small=await download(page,'Control+Shift+e',`artifacts/web-large-timeline-selection-${scale}.wav`);
        verifyWav(small.bytes,137,()=>0);
        await press(page,'Control+e');
        await page.waitForFunction(()=>window.coldeditWeb.diagnostics().error.includes('128 MiB'));
        assert.equal((await diag(page)).busy,false); assert.deepEqual(errors,[]);
        reports.push({scale,passed:true,selectedFrames:n,diagnostics:await diag(page)});
        console.log(`PASS features ${scale}x: linear/smooth fades, exact selection downloads, silence, history, range limits`);
      } catch(error) { await page.screenshot({path:`artifacts/web-features-failure-${scale}.png`}); throw error; }
      finally { await context.close(); }
    }
  } finally { await browser.close(); fs.writeFileSync('artifacts/web-features.json',JSON.stringify(reports,null,2)); }
})().catch(error=>{console.error(error);process.exitCode=1});
