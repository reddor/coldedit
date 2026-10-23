// Real canvas clicks, keyboard menu navigation, fractional DPI and resize checks.
const {chromium} = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const url = process.env.COLDEDIT_WEB_URL || 'http://localhost:8080';
const diag = page => page.evaluate(() => window.coldeditWeb.diagnostics());
const settle = page => page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));
async function click(page,x,y) { await page.mouse.click(x,y,{delay:35}); await settle(page); }
async function press(page,key) { await page.keyboard.press(key); await settle(page); }
async function drag(page,x1,y1,x2,y2) {
  await page.mouse.move(x1,y1); await settle(page); await page.mouse.down(); await settle(page);
  await page.mouse.move(x2,y2,{steps:12}); await settle(page); await page.mouse.up(); await settle(page);
}
async function surfaceCheck(page,scale) {
  await page.waitForFunction(scale => window.coldeditWeb.diagnostics().ui_scale===scale,scale);
  await settle(page);
  const d=await diag(page);
  const canvas=await page.locator('#canvas').evaluate(c=>{const r=c.getBoundingClientRect(),p=c.parentElement.getBoundingClientRect();
    return {pixels:[c.width,c.height],css:[r.width,r.height],parent:[p.width,p.height]};});
  assert.deepEqual(canvas.pixels,[d.surface.width,d.surface.height]);
  assert.deepEqual(canvas.css,canvas.parent,'GLFW inline styles must not change CSS layout');
  assert.equal(d.display_width,canvas.pixels[0]); assert.equal(d.display_height,canvas.pixels[1]);
  assert.equal(d.framebuffer_scale,1); assert.equal(d.font_global_scale,1);
  assert.equal(d.font_pixel_size,Math.floor(17*scale));
  for(let i=0;i<2;i++) assert(Math.abs(canvas.pixels[i]-canvas.css[i]*scale)<=1,'backing store within one snapped physical pixel');
  return {...d.surface,pixels:canvas.pixels};
}
(async()=>{
  const args=['--mute-audio'];
  if(process.env.COLDEDIT_FORCE_DPI) args.push(`--force-device-scale-factor=${process.env.COLDEDIT_FORCE_DPI}`);
  const browser=await chromium.launch({channel:process.env.COLDEDIT_BROWSER||'msedge',headless:true,args});
  const reports=[];
  try {
    for(const scale of (process.env.COLDEDIT_DPI ? process.env.COLDEDIT_DPI.split(',').map(Number) : [1,1.1,1.25,1.5,1.75,2])) {
      const context=await browser.newContext({viewport:{width:1441,height:945},deviceScaleFactor:scale});
      const page=await context.newPage(), errors=[];
      page.on('pageerror',e=>errors.push(e.message));
      await page.goto(url); await page.waitForFunction(()=>document.querySelector('#loading').hidden);
      const surface=await surfaceCheck(page,scale);
      if(Number(process.env.COLDEDIT_FORCE_DPI)===scale) assert.equal(surface.exact,true,'browser-reported physical pixel box is used at real DPR');
      // Empty-document edit controls must do nothing.
      for(const x of [425,464,526,600,676,736,800]) await click(page,x,76);
      assert.equal((await diag(page)).frames,0); assert.equal((await diag(page)).error,'');
      // Menu launch, outside dismissal, keyboard launch, and Escape dismissal.
      await click(page,80,76); assert.equal((await diag(page)).popup_open,true);
      await click(page,900,170); assert.equal((await diag(page)).popup_open,false);
      await press(page,'F10'); assert.equal((await diag(page)).popup_open,true);
      await press(page,'Escape'); assert.equal((await diag(page)).popup_open,false);
      // Import a real fixture; the empty editor no longer offers demo audio.
      await page.locator('#file-input').setInputFiles('tests/fixtures/tone.mp3');
      await page.waitForFunction(()=>window.coldeditWeb.diagnostics().frames>0);
      const total=(await diag(page)).frames;
      // Home gives a stable starting point even when the popup remembers focus.
      await press(page,'F10'); await press(page,'Home');
      // Export selection is disabled here, so keyboard navigation skips that row.
      for(let row=0;row<3;row++) await press(page,'ArrowDown');
      await press(page,'ArrowRight'); await press(page,'Enter');
      assert.equal((await diag(page)).popup_open,false);
      assert.equal((await diag(page)).selection_end,total); assert.equal((await diag(page)).selection_start,0);
      await press(page,'Escape');
      await drag(page,300,350,650,350);
      const selected=await diag(page), length=selected.selection_end-selected.selection_start;
      assert(length>0);
      await click(page,600,76); // Copy.
      assert.equal((await diag(page)).dirty,false);
      await click(page,526,76); // Cut.
      assert.equal((await diag(page)).frames,total-length);
      assert.equal((await diag(page)).selection_end-(await diag(page)).selection_start,length);
      await press(page,'Escape'); // Clear the retained range for an insertion paste.
      await click(page,676,76); // Paste back at the cut cursor.
      assert.equal((await diag(page)).frames,total);
      assert.equal((await diag(page)).selection_end-(await diag(page)).selection_start,length);
      await click(page,425,76); // Undo paste.
      assert.equal((await diag(page)).frames,total-length);
      await click(page,464,76); // Redo paste.
      assert.equal((await diag(page)).frames,total);
      await click(page,736,76); // Delete selection.
      assert.equal((await diag(page)).frames,total-length);
      await click(page,425,76); // Undo delete, then select a new range.
      await press(page,'Escape');
      await drag(page,300,350,650,350);
      const beforeTrim=await diag(page), trimmed=beforeTrim.selection_end-beforeTrim.selection_start;
      await click(page,800,76);
      assert.equal((await diag(page)).frames,trimmed);
      await click(page,425,76); assert.equal((await diag(page)).frames,total);
      // Menu/submenu input uses the same CSS-to-pixel conversion as the waveform.
      await click(page,80,76); await page.mouse.move(100,237); await settle(page);
      await page.screenshot({path:`artifacts/web-menu-${scale}.png`});
      await click(page,900,170); assert.equal((await diag(page)).popup_open,false);
      // Odd viewport sizes and minimum-width layout catch rounded-CSS errors.
      await page.setViewportSize({width:961,height:705});
      await surfaceCheck(page,scale);
      await page.screenshot({path:`artifacts/web-compact-${scale}.png`});
      await page.setViewportSize({width:1441,height:945}); await surfaceCheck(page,scale);
      const fractional=await page.addStyleTag({content:'#editor-container{width:calc(100% - .375px);height:calc(100dvh - 44.375px)}'});
      const fractionSurface=await surfaceCheck(page,scale);
      assert(!Number.isInteger(fractionSurface.cssWidth));
      await fractional.evaluate(element=>element.remove()); await surfaceCheck(page,scale);
      assert.equal((await diag(page)).dpi_rebuilds,1,'resizing does not rebuild fonts');
      // Leave and re-enter the canvas: backend cached mouse positions stay in CSS space.
      await press(page,'Escape'); assert.equal((await diag(page)).selection_start,(await diag(page)).selection_end);
      await page.mouse.move(500,20); await settle(page); await drag(page,310,350,620,350);
      const afterReentry=await diag(page); assert(afterReentry.selection_end>afterReentry.selection_start);
      await page.screenshot({path:`artifacts/web-toolbar-${scale}.png`});
      assert.deepEqual(errors,[]); reports.push({scale,surface,passed:true});
      await context.close();
      console.log(`PASS toolbar and popup at ${scale}: disabled states, cut/copy/paste, history, delete, trim, resize, pointer re-entry`);
    }
  } finally { await browser.close(); fs.writeFileSync('artifacts/web-toolbar.json',JSON.stringify(reports,null,2)); }
})().catch(e=>{console.error(e);process.exitCode=1;});
