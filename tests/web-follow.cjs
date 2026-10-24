// Follow playback stays explicit and tracks every rendered frame, including zoom.
const {chromium} = require('playwright');
const assert = require('node:assert/strict');
const {settle,captureFailure} = require('./web-test-helpers.cjs');
const diag = page => page.evaluate(() => coldeditWeb.diagnostics());
async function key(page, value) { await page.keyboard.press(value); await settle(page); }
function fixture(seconds=120) {
  const data = Buffer.alloc(44 + 48000 * seconds * 2);
  data.write('RIFF'); data.writeUInt32LE(data.length-8,4); data.write('WAVEfmt ',8);
  data.writeUInt32LE(16,16); data.writeUInt16LE(1,20); data.writeUInt16LE(1,22);
  data.writeUInt32LE(48000,24); data.writeUInt32LE(96000,28);
  data.writeUInt16LE(2,32); data.writeUInt16LE(16,34);
  data.write('data',36); data.writeUInt32LE(data.length-44,40);
  return data;
}
async function toggleFollow(page) {
  await key(page,'F10'); await key(page,'Home');
  // No selection: Open, Export, Edit, Selection, Process, Playback.
  for (let i=0;i<5;i++) await key(page,'ArrowDown');
  await key(page,'ArrowRight'); await key(page,'End'); await key(page,'Enter');
  assert.equal((await diag(page)).popup_open,false);
}
async function samples(page, count=24) {
  return page.evaluate(count => new Promise(resolve => {
    const result=[];
    function sample() {
      const d=coldeditWeb.diagnostics();
      if (!result.length || d.frame_count!==result.at(-1).frame_count) result.push(d);
      if (result.length===count) resolve(result); else requestAnimationFrame(sample);
    }
    requestAnimationFrame(sample);
  }), count);
}
async function tracks(page) {
  const frames=await samples(page);
  for (const d of frames) {
    assert.equal(d.playing,true); assert.equal(d.follow_playback,true);
    // Use the rendered cursor; the audio callback may already have advanced position.
    assert(Math.abs(d.view_cursor-d.view_start-d.view_span*2/3)<8,
      `cursor stays at two thirds while the waveform moves: ${JSON.stringify(d)}`);
  }
  assert(frames.at(-1).view_cursor>frames[0].view_cursor,'audio playback advances');
}
async function loopsInView(page) {
  const frames=await samples(page,120);
  let wraps=0;
  for (let i=0;i<frames.length;i++) {
    const d=frames[i];
    assert.equal(d.playing,true); assert.equal(d.follow_playback,true);
    assert(d.view_cursor>=d.view_start-8 && d.view_cursor<=d.view_start+d.view_span+8,
      'looping playhead remains visible on every frame');
    if (i && d.view_cursor<frames[i-1].view_cursor) {
      assert(d.view_cursor<frames[i-1].view_start,'loop goes off the previous view to the left');
      wraps++;
    }
  }
  assert(wraps>0,'observed an actual playback loop');
}
async function zoom(page, x, y, delta) {
  const before=(await diag(page)).view_target_span;
  await page.mouse.move(x,y); await page.mouse.wheel(0,delta); await settle(page);
  const after=(await diag(page)).view_target_span;
  assert(delta<0 ? after<before : after>before,'live wheel input changes zoom');
}
(async () => {
  const browser=await chromium.launch({channel:process.env.COLDEDIT_BROWSER||'msedge',headless:true,args:['--mute-audio']});
  try {
    for (const scale of (process.env.COLDEDIT_DPI||'1,1.25,2').split(',').map(Number)) {
      const context=await browser.newContext({viewport:{width:1440,height:944},deviceScaleFactor:scale});
      const page=await context.newPage(), errors=[];
      page.on('pageerror',e=>errors.push(e.message));
      try {
        await page.goto(process.env.COLDEDIT_WEB_URL||'http://localhost:8080');
        await page.waitForFunction(()=>document.querySelector('#loading').hidden);
        await page.locator('#file-input').setInputFiles({name:'follow.wav',mimeType:'audio/wav',buffer:fixture()});
        await page.waitForFunction(()=>coldeditWeb.diagnostics().peak_bytes>0);
        assert.equal((await diag(page)).follow_playback,false,'follow is off by default');
        for (let i=0;i<8;i++) await key(page,'Equal');
        await page.waitForFunction(()=>{
          const d=coldeditWeb.diagnostics();
          return d.view_span===d.view_target_span && d.view_start===d.view_target_start;
        });
        // Start left of the threshold in a view well away from the file's start.
        await page.mouse.click(345,140); await settle(page);
        const initial=await diag(page);
        await toggleFollow(page); assert.equal((await diag(page)).follow_playback,true);
        await key(page,'Space');
        const waiting=await samples(page,8);
        assert(waiting.at(-1).view_cursor>waiting[0].view_cursor,'cursor advances before scrolling');
        assert(waiting.every(d=>d.view_cursor<initial.view_start+initial.view_span*2/3 && d.view_start===initial.view_start),
          'the view holds until the cursor reaches two thirds');
        await page.waitForFunction(start=>coldeditWeb.diagnostics().view_start>start,initial.view_start);
        await tracks(page);
        // Both waveform and navigator zoom keep the cursor anchored during easing.
        await zoom(page,650,380,-240);
        await tracks(page);
        await zoom(page,40,872,100);
        // Zooming out may leave the cursor before the threshold again.
        await page.waitForFunction(()=>{
          const d=coldeditWeb.diagnostics(); return Math.abs(d.view_cursor-d.view_start-d.view_span*2/3)<8;
        });
        await tracks(page);
        // At a very close zoom, animation lag must never push the cursor off-screen.
        await zoom(page,650,380,-1800);
        for (let i=0;i<8 && (await diag(page)).view_target_span>=4800;i++) await key(page,'Equal');
        assert((await diag(page)).view_target_span<4800,'exercise a view shorter than 100 ms');
        await tracks(page);
        await page.waitForFunction(()=>{
          const d=coldeditWeb.diagnostics(); return d.view_span===d.view_target_span;
        });
        await key(page,'Space');
        const paused=await diag(page); await settle(page);
        assert.equal((await diag(page)).view_start,paused.view_start,'pause holds the viewport');
        // Turning follow off explicitly allows playback to leave the stationary view.
        await toggleFollow(page); assert.equal((await diag(page)).follow_playback,false);
        await key(page,'Space');
        const unfollowed=await samples(page);
        assert(unfollowed.at(-1).position>unfollowed[0].position);
        assert(unfollowed.every(d=>!d.follow_playback && d.view_start===unfollowed[0].view_start));
        await zoom(page,650,380,100);
        assert.equal((await diag(page)).follow_playback,false,'zoom preserves explicit off state');
        await toggleFollow(page); await tracks(page);
        await key(page,'Space');
        // Exercise real selection and whole-file loops with a view shorter than either loop.
        await page.locator('#file-input').setInputFiles({name:'loop.wav',mimeType:'audio/wav',buffer:fixture(1)});
        await page.waitForFunction(()=>coldeditWeb.diagnostics().title==='loop.wav');
        await page.mouse.move(345,350); await page.mouse.down(); await settle(page);
        await page.mouse.move(895,350,{steps:8}); await settle(page);
        await page.mouse.up(); await settle(page);
        const selection=await diag(page);
        assert(selection.selection_end>selection.selection_start,'selection loop has a range');
        for (let i=0;i<4;i++) await key(page,'Equal');
        await key(page,'l'); await key(page,'Space');
        await loopsInView(page);
        await key(page,'Space'); await key(page,'Escape');
        assert.equal((await diag(page)).selection_start,(await diag(page)).selection_end);
        await key(page,'Space'); await loopsInView(page); await key(page,'Space');
        assert.deepEqual(errors,[]);
        console.log(`PASS follow ${scale}x: default off, threshold, live zoom, explicit toggle and loop recovery`);
      } catch (error) { await captureFailure(page,`web-follow-failure-${scale}`,error); throw error; }
      finally { await context.close(); }
    }
  } finally { await browser.close(); }
})().catch(error=>{console.error(error);process.exitCode=1;});
