const {chromium} = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const url = process.env.COLDEDIT_WEB_URL || 'http://localhost:8080';
const diag = page => page.evaluate(() => window.coldeditWeb.diagnostics());
async function tick(page) { await page.evaluate(() => new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r)))); }
(async () => {
  const browser = await chromium.launch({channel:process.env.COLDEDIT_BROWSER || 'msedge',headless:true,args:['--mute-audio']});
  try {
    const context = await browser.newContext({viewport:{width:1440,height:944},acceptDownloads:true});
    const page = await context.newPage();
    page.setDefaultTimeout(15000);
    const errors = []; page.on('pageerror',e=>errors.push(e.message));
    await page.goto(url); await page.waitForFunction(()=>document.getElementById('loading').hidden,null,{timeout:45000});
    // Browser picker is reachable from the ImGui UI, not only from a test bridge.
    const chooser = page.waitForEvent('filechooser');
    await page.mouse.click(80,76,{delay:50}); await tick(page);
    await page.mouse.click(115,154,{delay:50}); await (await chooser).setFiles('artifacts/web-export-1.wav');
    await page.waitForFunction(()=>window.coldeditWeb.diagnostics().frames>0);
    const initial = await diag(page);
    await page.keyboard.press('r'); await tick(page);
    // Stage another file while dirty. Cancel via keyboard navigation preserves edits.
    await page.locator('#file-input').setInputFiles({name:'pending.wav',mimeType:'audio/wav',buffer:fs.readFileSync('artifacts/web-export-1.wav')});
    await page.waitForFunction(()=>!Module._coldedit_can_import());
    await page.screenshot({path:'artifacts/web-confirm.png'});
    await page.mouse.click(846,539,{delay:50}); await tick(page);
    await page.waitForFunction(()=>Module._coldedit_can_import());
    assert.equal((await diag(page)).title,initial.title); assert.equal((await diag(page)).dirty,true);
    assert.equal(await page.evaluate(()=>Module.FS.readdir('/imports').length),2,'cancel releases staged file');
    await page.keyboard.press('Control+z'); await tick(page);
    // Malformed input must leave the previous document intact and release staging.
    await page.locator('#file-input').setInputFiles({name:'broken.wav',mimeType:'audio/wav',buffer:Buffer.alloc(64)});
    await page.waitForFunction(()=>window.coldeditWeb.diagnostics().error.length>0);
    assert.equal((await diag(page)).title,initial.title); assert.equal((await diag(page)).frames,initial.frames);
    assert.equal(await page.evaluate(()=>Module.FS.readdir('/imports').length),2);
    await page.screenshot({path:'artifacts/web-corrupt.png'});
    await page.mouse.click(550,530,{delay:50}); await tick(page);
    await page.waitForFunction(()=>Module._coldedit_can_import());
    // Exercise a real drop event through the production file handler.
    await page.evaluate(async () => {
      const bytes = new Uint8Array(48); const v = new DataView(bytes.buffer);
      bytes.set(new TextEncoder().encode('RIFF'),0); v.setUint32(4,40,true); bytes.set(new TextEncoder().encode('WAVEfmt '),8);
      v.setUint32(16,16,true);v.setUint16(20,1,true);v.setUint16(22,2,true);v.setUint32(24,48000,true);v.setUint32(28,192000,true);v.setUint16(32,4,true);v.setUint16(34,16,true);
      bytes.set(new TextEncoder().encode('data'),36);v.setUint32(40,4,true);
      const transfer=new DataTransfer(); transfer.items.add(new File([bytes],'dropped.wav',{type:'audio/wav'}));
      document.getElementById('canvas').dispatchEvent(new DragEvent('drop',{dataTransfer:transfer,bubbles:true,cancelable:true}));
    });
    await page.waitForFunction(()=>window.coldeditWeb.diagnostics().title==='dropped.wav');
    assert.equal((await diag(page)).frames,1);
    const heaps=[];
    // Exact upper input boundary, repeatedly: includes memory growth and replacement.
    for (let i=0;i<3;i++) {
      await page.evaluate(async i => {
        const bytes = new Uint8Array(64*1024*1024);const v=new DataView(bytes.buffer);
        bytes.set(new TextEncoder().encode('RIFF'),0);v.setUint32(4,bytes.length-8,true);bytes.set(new TextEncoder().encode('WAVEfmt '),8);
        v.setUint32(16,16,true);v.setUint16(20,1,true);v.setUint16(22,2,true);v.setUint32(24,48000,true);v.setUint32(28,192000,true);v.setUint16(32,4,true);v.setUint16(34,16,true);
        bytes.set(new TextEncoder().encode('data'),36);v.setUint32(40,bytes.length-44,true);
        await window.coldeditWeb.openFile(new File([bytes],`boundary-${i}.wav`,{type:'audio/wav'}));
      },i);
      await page.waitForFunction(i=>{const s=window.coldeditWeb.diagnostics();return s.title===`boundary-${i}.wav`&&s.peak_bytes>0;},i,{timeout:30000});
      const state=await diag(page); assert.equal(state.frames,(64*1024*1024-44)/4); heaps.push(state.resident_bytes);
      assert.equal(await page.evaluate(()=>Module.FS.readdir('/imports').length),2);
    }
    assert.equal(heaps[2],heaps[1],'replacing documents reuses WASM storage');
    await page.mouse.click(262,76,{delay:50}); await page.waitForFunction(()=>window.coldeditWeb.diagnostics().position>4800);
    await page.mouse.click(262,76,{delay:50}); await page.waitForFunction(()=>!window.coldeditWeb.diagnostics().playing);
    // Runtime DPR transitions through the same window, without reloading the app.
    const cdp=await context.newCDPSession(page);
    for (const scale of [1.5,2,1]) {
      await cdp.send('Emulation.setDeviceMetricsOverride',{width:1440,height:944,deviceScaleFactor:scale,mobile:false});
      await page.waitForFunction(scale=>window.coldeditWeb.diagnostics().font_raster_scale===scale,scale);
      assert.equal((await diag(page)).ui_scale,scale);
    }
    assert.equal((await diag(page)).dpi_rebuilds,4);
    const native = page.locator('.web-bar a');
    assert.equal(await native.getAttribute('href'),'https://github.com/reddor/coldedit/releases');
    assert.equal(await native.getAttribute('target'),'_blank');
    assert.deepEqual(errors,[]);
    fs.writeFileSync('artifacts/web-lifecycle.json',JSON.stringify({passed:true,heaps,diagnostics:await diag(page)},null,2));
    await page.screenshot({path:'artifacts/web-large-file.png'});
    console.log('PASS browser lifecycle: picker, cancelled open, corrupt input, drop, 64 MiB boundary, repeated opens, audio after memory growth, live DPI, native link');
    await context.close();
    // Missing isolation headers must show an actionable explanation before loading WASM.
    const blocked=await browser.newPage();
    await blocked.route('**/*',async route=>{ const response=await route.fetch();const headers={...response.headers()};delete headers['cross-origin-opener-policy'];delete headers['cross-origin-embedder-policy'];await route.fulfill({response,headers}); });
    await blocked.goto(url);assert.match(await blocked.locator('#loading-text').textContent(),/cross-origin isolation/);
    assert.equal(await blocked.evaluate(()=>document.querySelector('script[src="coldedit.js"]')),null);
    console.log('PASS browser hosting preflight');
  } finally { await browser.close(); }
})().catch(e=>{console.error(e);process.exitCode=1;});
