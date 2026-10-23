// Requires Playwright and a running web/serve.mjs server. Uses an isolated browser profile.
const {chromium} = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const url = process.env.COLDEDIT_WEB_URL || 'http://localhost:8080';
const frames = 48000 * 4;
function fixture() {
  const data = Buffer.alloc(44 + frames * 4);
  data.write('RIFF'); data.writeUInt32LE(data.length-8,4); data.write('WAVEfmt ',8); data.writeUInt32LE(16,16);
  data.writeUInt16LE(1,20); data.writeUInt16LE(2,22); data.writeUInt32LE(48000,24); data.writeUInt32LE(192000,28);
  data.writeUInt16LE(4,32); data.writeUInt16LE(16,34); data.write('data',36); data.writeUInt32LE(frames*4,40);
  for (let f=0;f<frames;f++) {
    data.writeInt16LE(Math.round(Math.sin(f*.03)*18000),44+f*4);
    data.writeInt16LE(Math.round(Math.sin(f*.021)*14000),46+f*4);
  }
  return data;
}
const original = fixture();
const reports = [];
const diag = page => page.evaluate(() => window.coldeditWeb.diagnostics());
async function settle(page) { await page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)))); }
async function press(page, key) { await page.keyboard.press(key); await settle(page); }
async function drag(page,x1,y1,x2,y2) {
  await page.mouse.move(x1,y1); await settle(page); await page.mouse.down(); await settle(page);
  await page.mouse.move(x2,y2,{steps:12}); await settle(page); await page.mouse.up(); await settle(page);
}
async function load(page, name='browser-test.wav', data=original) {
  await page.locator('#file-input').setInputFiles({name,mimeType:'audio/wav',buffer:data});
  await page.waitForFunction(name => window.coldeditWeb.diagnostics().title===name, name);
  await page.waitForFunction(() => window.coldeditWeb.diagnostics().peak_bytes>0);
}
(async () => {
  const browser = await chromium.launch({channel:process.env.COLDEDIT_BROWSER || 'msedge',headless:true,args:['--mute-audio']});
  try {
    for (const scale of (process.env.COLDEDIT_DPI ? process.env.COLDEDIT_DPI.split(',').map(Number) : [1,1.1,1.25,1.5,1.75,2])) {
      const context = await browser.newContext({viewport:{width:1440,height:944},deviceScaleFactor:scale,acceptDownloads:true});
      const page = await context.newPage();
      const errors = [];
      page.on('pageerror',e => errors.push(e.message));
      page.on('console',msg => { if (msg.type()==='error') console.log('browser:',msg.text().slice(0,600)); });
      await page.goto(url);
      await page.waitForFunction(() => document.getElementById('loading').hidden,null,{timeout:45000});
      await load(page);
      let state = await diag(page);
      assert.equal(state.ui_scale,scale); assert.equal(state.font_raster_scale,scale); assert.equal(state.font_pixel_size,Math.floor(17*scale));
      assert.deepEqual(await page.locator('#canvas').evaluate(c=>[c.width,c.height]),[state.surface.width,state.surface.height]);
      assert.equal(state.display_width,state.surface.width); assert.equal(state.display_height,state.surface.height);
      assert.equal(state.framebuffer_scale,1); assert.equal(state.font_global_scale,1);
      assert(Math.abs(state.surface.width-1440*scale)<=1); assert(Math.abs(state.surface.height-900*scale)<=1);
      assert.equal(state.frames,frames);
      assert.equal(await page.evaluate(() => Module.FS.readdir('/imports').length),2,'import copy released');
      // Actual canvas input, including each selection edge and crossing.
      await drag(page,300,350,650,350);
      state = await diag(page);
      if (!(state.selection_end>state.selection_start)) await page.screenshot({path:`artifacts/web-selection-failure-${scale}.png`});
      assert(state.selection_end>state.selection_start,JSON.stringify(state));
      const fixedEnd = state.selection_end;
      await drag(page,300,350,350,350);
      state = await diag(page); assert.equal(state.selection_end,fixedEnd); assert(state.selection_start>0);
      const fixedStart = state.selection_start;
      await drag(page,650,350,700,350);
      state = await diag(page); assert.equal(state.selection_start,fixedStart); assert(state.selection_end>fixedEnd);
      const crossedEnd = state.selection_end;
      await drag(page,350,350,750,350);
      state = await diag(page); assert.equal(state.selection_start,crossedEnd); assert(state.selection_end>crossedEnd);
      // Pointer anchored zoom and wheel pan must reach the app, not zoom the page.
      await page.mouse.move(650,380); await page.mouse.wheel(0,-240);
      await page.waitForFunction(() => window.coldeditWeb.diagnostics().view_span < 192000*.9);
      await page.keyboard.down('Control'); await page.mouse.wheel(0,100); await settle(page); await page.keyboard.up('Control');
      // AudioWorklet must actually consume the ring and advance the position.
      const beforePlay = (await diag(page)).position;
      await page.mouse.click(262,76,{delay:50});
      await page.waitForFunction(before => window.coldeditWeb.diagnostics().position>before+4800,beforePlay);
      await page.mouse.click(262,76,{delay:50});
      await page.waitForFunction(() => !window.coldeditWeb.diagnostics().playing);
      // Whole-file reverse + undo/redo, then verify downloaded PCM samples.
      await press(page,'Escape'); await press(page,'Control+a'); await press(page,'r');
      assert.equal((await diag(page)).dirty,true);
      await press(page,'Control+z'); assert.equal((await diag(page)).dirty,false);
      await press(page,'Control+Shift+z'); assert.equal((await diag(page)).dirty,true);
      const downloadPromise = page.waitForEvent('download');
      await page.mouse.click(80,76,{delay:50}); await settle(page);
      assert.equal((await diag(page)).popup_open,true,'logo opens export menu');
      await page.mouse.click(115,178,{delay:50}); await settle(page);
      assert.equal((await diag(page)).popup_open,false,'export closes menu');
      const download = await downloadPromise;
      const out = `artifacts/web-export-${scale}.wav`; await download.saveAs(out);
      const exported = fs.readFileSync(out);
      assert.equal(exported.readUInt16LE(34),24); assert.equal(exported.length,44+frames*6);
      for (let i=0;i<frames;i+=997) for (let c=0;c<2;c++) {
        const expected = original.readInt16LE(44+(frames-1-i)*4+c*2)*256;
        assert.equal(exported.readIntLE(44+i*6+c*3,3),expected,'reversed exported sample');
      }
      await page.waitForFunction(() => !window.coldeditWeb.diagnostics().busy);
      assert.equal((await diag(page)).dirty,false);
      assert.equal(await page.evaluate(() => Module.FS.readdir('/exports').length),2,'export buffer released');
      await page.locator('#dismiss').click();
      await page.screenshot({path:`artifacts/web-editor-${scale}.png`});
      // Reject before reading the file, retaining the current waveform.
      const rejected = await page.evaluate(async () => {
        let read=false;
        await window.coldeditWeb.openFile({name:'too-large.wav',size:64*1024*1024+1,arrayBuffer(){read=true;throw Error('must not read');}});
        return {read,notice:document.getElementById('notice-text').textContent,files:Module.FS.readdir('/imports')};
      });
      assert.equal(rejected.read,false); assert.match(rejected.notice,/64 MiB/); assert.equal(rejected.files.length,2);
      assert.equal((await diag(page)).frames,frames);
      await page.locator('#dismiss').click();
      // Oversized export is refused before a background job or temporary file.
      for (let i=0;i<8;i++) { await press(page,'Control+a'); await press(page,'Control+c'); await press(page,'End'); await press(page,'Escape'); await press(page,'Control+v'); }
      await page.mouse.click(80,76,{delay:50}); await settle(page); await page.mouse.click(115,178,{delay:50});
      await page.waitForFunction(() => window.coldeditWeb.diagnostics().error.includes('128 MiB'));
      assert.equal((await diag(page)).busy,false);
      assert.equal(await page.evaluate(() => Module.FS.readdir('/exports').length),2);
      // Close error modal with its focused button; reload avoids accepting an unsaved-change prompt.
      await page.screenshot({path:`artifacts/web-limit-${scale}.png`});
      assert.deepEqual(errors,[]);
      reports.push({scale,passed:true,diagnostics:await diag(page)});
      await context.close();
      console.log(`PASS browser scale ${scale}: file I/O, boundaries, zoom, playback, history, exact export, limits`);
    }
  } finally { await browser.close(); fs.writeFileSync('artifacts/web-smoke.json',JSON.stringify(reports,null,2)); }
})().catch(error => {console.error(error);process.exitCode=1});
