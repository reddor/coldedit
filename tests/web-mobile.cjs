const {chromium} = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const url = process.env.COLDEDIT_WEB_URL || 'http://localhost:8080';
const tick = p => p.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));
const diag = p => p.evaluate(() => coldeditWeb.diagnostics());
function fixture() {
  const b = Buffer.alloc(44 + 48000 * 4 * 2);
  b.write('RIFF'); b.writeUInt32LE(b.length - 8, 4); b.write('WAVEfmt ', 8);
  b.writeUInt32LE(16,16); b.writeUInt16LE(1,20); b.writeUInt16LE(1,22);
  b.writeUInt32LE(48000,24); b.writeUInt32LE(96000,28); b.writeUInt16LE(2,32); b.writeUInt16LE(16,34);
  b.write('data',36); b.writeUInt32LE(b.length - 44,40);
  for (let i = 0; i < 192000; ++i) b.writeInt16LE(Math.round(Math.sin(i * .03) * 16000),44 + i * 2);
  return b;
}
async function surface(p, width, height) {
  await p.waitForFunction(() => coldeditWeb.diagnostics().compact_layout);
  await tick(p);
  const s = await p.evaluate(() => ({width:innerWidth,height:innerHeight,scrollWidth:document.documentElement.scrollWidth,scrollHeight:document.documentElement.scrollHeight,canvas:document.querySelector('#canvas').getBoundingClientRect().toJSON()}));
  assert.equal(s.width,width); assert.equal(s.height,height);
  assert.equal(s.scrollWidth,width,'no horizontal page overflow');
  assert.equal(s.scrollHeight,height,'editor fits available screen height');
  assert.equal(s.canvas.width,width); assert.equal(s.canvas.height,height - 44);
  const d = await diag(p);
  assert.equal(d.display_width,Math.round(width * d.surface.dpr));
  assert.equal(d.display_height,Math.round((height - 44) * d.surface.dpr));
}
(async () => {
  fs.mkdirSync('artifacts',{recursive:true});
  const browser = await chromium.launch({channel:process.env.COLDEDIT_BROWSER || 'msedge',headless:true,args:['--mute-audio']});
  try {
    for (const [width,height,dpr] of [[320,568,1],[390,844,3],[844,390,2],[768,1024,2]]) {
      const context = await browser.newContext({viewport:{width,height},deviceScaleFactor:dpr,hasTouch:true,isMobile:true,acceptDownloads:true});
      const p = await context.newPage(), errors = [];
      p.on('pageerror',error => errors.push(error.message));
      await p.goto(url); await p.waitForFunction(() => document.querySelector('#loading').hidden,null,{timeout:45000});
      await surface(p,width,height);
      await p.screenshot({path:`artifacts/mobile-empty-${width}.png`});
      // A real touch opens the menu and browser file picker.
      await p.touchscreen.tap(80,76); await tick(p);
      const chooser = p.waitForEvent('filechooser');
      await p.touchscreen.tap(110,154);
      await (await chooser).setFiles({name:'mobile.wav',mimeType:'audio/wav',buffer:fixture()});
      await p.waitForFunction(() => coldeditWeb.diagnostics().peak_bytes > 0);
      const cdp = await context.newCDPSession(p), y = Math.min(height - 145,230);
      const touch = (type,x) => cdp.send('Input.dispatchTouchEvent',{type,touchPoints:type === 'touchEnd' ? [] : [{x,y,id:1,radiusX:2,radiusY:2,force:1}]});
      await touch('touchStart',100); await tick(p);
      for (let i = 1; i <= 12; ++i) { await touch('touchMove',100 + (width - 160) * i / 12); await tick(p); }
      await touch('touchEnd'); await tick(p);
      let d = await diag(p); assert(d.selection_end > d.selection_start,'touch drag selects audio');
      await p.touchscreen.tap(130,134); await tick(p); assert((await diag(p)).frames < 192000,'touch Cut edits selection');
      await p.touchscreen.tap(34,134); await tick(p); assert.equal((await diag(p)).frames,192000,'touch Undo restores audio');
      await p.touchscreen.tap(274,134); await tick(p); assert((await diag(p)).view_target_span < 192000,'touch zoom button works');
      await p.screenshot({path:`artifacts/mobile-editor-${width}.png`});
      await p.touchscreen.tap(width - 48,76); await tick(p); assert.equal((await diag(p)).tools_open,true);
      await p.screenshot({path:`artifacts/mobile-tools-${width}.png`});
      await p.touchscreen.tap(width - 48,76); await tick(p); assert.equal((await diag(p)).tools_open,false);
      if (width === 390) {
        await p.setViewportSize({width:844,height:390}); await surface(p,844,390);
        assert.equal((await diag(p)).frames,192000,'rotation preserves the document');
        await p.setViewportSize({width,height}); await surface(p,width,height);
        await p.keyboard.press('r'); await tick(p);
        await p.locator('#file-input').setInputFiles({name:'next.wav',mimeType:'audio/wav',buffer:fixture()});
        await p.waitForFunction(() => coldeditWeb.diagnostics().popup_open);
        await p.screenshot({path:'artifacts/mobile-unsaved.png'});
      }
      assert.deepEqual(errors,[]);
      await context.close();
      console.log(`PASS mobile ${width}x${height} @${dpr}: viewport, touch picker, selection, cut/undo, zoom and Tools`);
    }
  } finally { await browser.close(); }
})().catch(error => {console.error(error);process.exitCode = 1;});
