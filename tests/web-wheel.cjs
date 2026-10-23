// Real wheel events exercise the shared native/web editor at multiple DPI scales.
const {chromium} = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const diag = page => page.evaluate(() => coldeditWeb.diagnostics());
const tick = page => page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));
async function key(page, value) { await page.keyboard.press(value); await tick(page); }
async function settled(page) {
  await tick(page);
  await page.waitForFunction(() => {
    const d = coldeditWeb.diagnostics();
    return Math.abs(d.view_span-d.view_target_span)<.5 && Math.abs(d.view_start-d.view_target_start)<.5;
  });
  return diag(page);
}
async function wheel(page, x, y, modifier) {
  if (modifier) await page.keyboard.down(modifier);
  await page.mouse.wheel(x,y); await tick(page);
  if (modifier) await page.keyboard.up(modifier);
  return settled(page);
}
async function load(page) {
  await page.locator('#file-input').setInputFiles('tests/fixtures/tone.mp3');
  await page.waitForFunction(() => coldeditWeb.diagnostics().frames===44100);
  return settled(page);
}
async function toggleWheelPreference(page) {
  await key(page,'F10'); await key(page,'Home');
  // Open, Export, [disabled Save selection], Edit, Selection, Process, Playback, View.
  for (let i=0;i<6;i++) await key(page,'ArrowDown');
  await key(page,'ArrowRight'); await key(page,'End'); await key(page,'Enter');
  assert.equal((await diag(page)).popup_open,false);
}
function near(a,b,message) { assert(Math.abs(a-b)<2,`${message}: ${a} vs ${b}`); }
(async () => {
  const browser = await chromium.launch({channel:process.env.COLDEDIT_BROWSER||'msedge',headless:true,args:['--mute-audio']});
  const reports=[];
  try {
    for (const scale of (process.env.COLDEDIT_DPI||'1,1.25,2').split(',').map(Number)) {
      const context=await browser.newContext({viewport:{width:1440,height:944},deviceScaleFactor:scale});
      const page=await context.newPage(), errors=[];
      page.on('pageerror',e=>errors.push(e.message));
      try {
        await page.goto(process.env.COLDEDIT_WEB_URL||'http://localhost:8080');
        await page.waitForFunction(()=>document.querySelector('#loading').hidden);
        assert.equal((await diag(page)).frames,0);
        assert.equal((await diag(page)).wheel_zooms,true);
        await page.screenshot({path:`artifacts/web-empty-wheel-${scale}.png`});
        let before=await load(page);
        await page.mouse.move(650,380);
        let after=await wheel(page,0,-240);
        assert(after.view_span<before.view_span,'unmodified wheel zooms');
        const anchor=(650-70)/1100;
        near(before.view_start+anchor*before.view_span,after.view_start+anchor*after.view_span,'main zoom keeps pointer sample');
        before=after; after=await wheel(page,0,100,'Control');
        near(after.view_span,before.view_span,'Ctrl wheel keeps zoom');
        assert(after.view_start>before.view_start,'Ctrl wheel pans');
        before=after; after=await wheel(page,-80,0);
        near(after.view_span,before.view_span,'horizontal wheel keeps zoom');
        assert(after.view_start<before.view_start,'horizontal wheel pans');
        before=after; after=await wheel(page,0,-100,'Alt');
        near(after.view_span,before.view_span,'Alt wheel keeps time zoom');
        near(after.view_start,before.view_start,'Alt wheel keeps position');
        assert(after.vertical_zoom>before.vertical_zoom,'Alt wheel scales amplitude');
        // Opposite ends of the navigator must anchor to the same current view.
        for (const x of [40,1150,40,1150]) {
          before=after;
          await page.mouse.move(x,872);
          after=await wheel(page,0,-100);
          assert(after.view_span<before.view_span);
          near(before.view_start+before.view_span/2,after.view_start+after.view_span/2,'navigator keeps view center');
          assert.equal(after.follow_playback,false);
        }
        // Holding the navigator after a click must not snap zoom back to the pointer.
        await page.mouse.move(800,872); await page.mouse.down(); await tick(page);
        before=await settled(page); after=await wheel(page,0,-100);
        near(before.view_start+before.view_span/2,after.view_start+after.view_span/2,'held navigator keeps view center');
        await page.mouse.up();
        await key(page,'Escape');
        await toggleWheelPreference(page);
        assert.equal((await diag(page)).wheel_zooms,false);
        assert.equal(await page.evaluate(()=>localStorage.getItem('coldedit.wheelZooms')),'false');
        await page.reload(); await page.waitForFunction(()=>document.querySelector('#loading').hidden);
        assert.equal((await diag(page)).wheel_zooms,false,'preference survives reload');
        before=await load(page); await page.mouse.move(650,380);
        after=await wheel(page,0,-240,'Control');
        assert(after.view_span<before.view_span,'alternate setting uses Ctrl to zoom');
        before=after; after=await wheel(page,0,100);
        near(before.view_span,after.view_span,'alternate setting wheel keeps zoom');
        assert(after.view_start>before.view_start,'alternate setting wheel pans');
        before=after; await page.mouse.move(40,872); after=await wheel(page,0,-100);
        near(before.view_start+before.view_span/2,after.view_start+after.view_span/2,'navigator ignores main waveform preference');
        await toggleWheelPreference(page); assert.equal((await diag(page)).wheel_zooms,true);
        await page.mouse.move(500,20);
        await page.screenshot({path:`artifacts/web-wheel-${scale}.png`});
        await page.setViewportSize({width:960,height:704}); await tick(page);
        await page.screenshot({path:`artifacts/web-wheel-compact-${scale}.png`});
        assert.deepEqual(errors,[]);
        reports.push({scale,passed:true,diagnostics:await diag(page)});
        console.log(`PASS wheel ${scale}x: zoom, Ctrl/horizontal pan, Alt scale, navigator anchors, saved preference`);
      } catch (e) { await page.screenshot({path:`artifacts/web-wheel-failure-${scale}.png`}); throw e; }
      finally { await context.close(); }
    }
  } finally { await browser.close(); fs.writeFileSync('artifacts/web-wheel.json',JSON.stringify(reports,null,2)); }
})().catch(e=>{console.error(e);process.exitCode=1;});
