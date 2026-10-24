const {chromium}=require('playwright');
const assert=require('node:assert/strict'),fs=require('node:fs');
const frames=48000,width=1440,height=944;
const diag=p=>p.evaluate(()=>coldeditWeb.diagnostics());
const {settle:tick,captureFailure}=require('./web-test-helpers.cjs');
async function key(p,k){await p.keyboard.press(k);await tick(p);}
async function drag(p,a,b,alt=false,y=350){if(alt)await p.keyboard.down('Alt');await p.mouse.move(a,y);await tick(p);await p.mouse.down();await tick(p);await p.mouse.move(b,y,{steps:12});await tick(p);await p.mouse.up();if(alt)await p.keyboard.up('Alt');await tick(p);}
function sample(f,c){return f<0||f>=frames?0:Math.fround((c?-.1:.2)+.15*Math.sin(f*2*Math.PI/(c?71:48)));}
function fixture(){const b=Buffer.alloc(44+frames*8);b.write('RIFF');b.writeUInt32LE(b.length-8,4);b.write('WAVEfmt ',8);b.writeUInt32LE(16,16);b.writeUInt16LE(3,20);b.writeUInt16LE(2,22);b.writeUInt32LE(48000,24);b.writeUInt32LE(384000,28);b.writeUInt16LE(8,32);b.writeUInt16LE(32,34);b.write('data',36);b.writeUInt32LE(frames*8,40);for(let f=0;f<frames;f++)for(let c=0;c<2;c++)b.writeFloatLE(sample(f,c),44+f*8+c*4);return b;}
const range=d=>[d.selection_start,d.selection_end];
const length=d=>d.selection_end-d.selection_start;
async function slowDoubleClicks(browser){
 const context=await browser.newContext({viewport:{width,height}}),p=await context.newPage();
 try{
  // Two queued presses now take longer than ImGui's 300 ms double-click
  // threshold to render. Browser event timing must still select and clear.
  await p.addInitScript(()=>{
   const raf=window.requestAnimationFrame.bind(window);
   window.requestAnimationFrame=callback=>raf(()=>setTimeout(()=>callback(performance.now()),180));
  });
  await p.goto(process.env.COLDEDIT_WEB_URL||'http://localhost:8080/');await p.waitForFunction(()=>document.querySelector('#loading').hidden);
  await p.locator('#file-input').setInputFiles({name:'slow-selection.wav',mimeType:'audio/wav',buffer:fixture()});
  await p.waitForFunction(()=>coldeditWeb.diagnostics().frames===48000);
  for(const [x,expected] of [[500,frames],[600,0]]){
   await p.mouse.move(x,350);await tick(p);await p.mouse.dblclick(x,350);await tick(p);
   assert.equal(length(await diag(p)),expected,'double-click uses event timing at low frame rates');
  }
  assert.equal((await diag(p)).history_states,1);assert.equal((await diag(p)).dirty,false);
  console.log('PASS selection with deliberately slow rendering: double-click selects and clears');
 }catch(e){await captureFailure(p,'web-selection-slow-failure',e);throw e;}finally{await context.close();}
}
function xAt(d,f){return 70+(f-d.view_start)/d.view_span*1100;}
async function exportAudio(p,path,expected,total,trigger){const before=await diag(p),event=p.waitForEvent('download');if(trigger)await trigger();else await key(p,'Control+e');const download=await event;await download.saveAs(path);await p.waitForFunction(()=>!coldeditWeb.diagnostics().busy);await p.locator('#dismiss').click();await tick(p);const b=fs.readFileSync(path);assert.equal(b.readUInt32LE(40)/6,total);for(let f=0;f<total;f++)for(let c=0;c<2;c++)assert(Math.abs(b.readIntLE(44+(f*2+c)*3,3)/8388608-expected(f,c))<2e-6,`sample ${f}/${c}`);if(trigger){assert.equal(download.suggestedFilename(),'selection-selection.wav');const after=await diag(p);for(const field of ['frames','history_states','dirty','selection_start','selection_end'])assert.equal(after[field],before[field],field);}}
(async()=>{
 const browser=await chromium.launch({channel:process.env.COLDEDIT_BROWSER||'msedge',headless:true,args:['--mute-audio']});const reports=[];
 try{for(const scale of (process.env.COLDEDIT_DPI||'1,1.25,2').split(',').map(Number)){
  const context=await browser.newContext({viewport:{width,height},deviceScaleFactor:scale,acceptDownloads:true}),p=await context.newPage(),errors=[];p.on('pageerror',e=>errors.push(e.message));p.setDefaultTimeout(20000);
  try{
   // Fractional DPI rounds pointer coordinates to physical pixels in GLFW.
   const cursorAt=(d,x,view=d)=>assert(Math.abs(d.position-Math.round(view.view_start+(x-70)/1100*view.view_span))<=view.view_span/(1100*scale)+1,'ruler click places the cursor within one physical pixel');
   await p.goto(process.env.COLDEDIT_WEB_URL||'http://localhost:8080/');await p.waitForFunction(()=>document.querySelector('#loading').hidden);
   await p.locator('#file-input').setInputFiles({name:'selection.wav',mimeType:'audio/wav',buffer:fixture()});await p.waitForFunction(()=>coldeditWeb.diagnostics().frames===48000);
   // Waveform double-click toggles full selection, including after Ctrl+A
   // and at a handle where the first click can already change the range.
   const doubleClick=async(x,y=350)=>{await p.mouse.move(x,y);await tick(p);await p.mouse.dblclick(x,y);await tick(p);};
   const cleared=async()=>{const d=await diag(p);assert.equal(length(d),0,'full selection clears');assert.equal(d.position,d.selection_start,'playhead follows the clearing click');assert.equal(d.history_states,1);assert.equal(d.dirty,false);};
   await doubleClick(500);assert.deepEqual(range(await diag(p)),[0,frames]);
   await doubleClick(600);await cleared();
   await key(p,'Control+a');await doubleClick(700);await cleared();
   await drag(p,650,300);await doubleClick(450);assert.deepEqual(range(await diag(p)),[0,frames]);
   await doubleClick(72);await cleared();
   await key(p,'Control+a');await doubleClick(550,140);
   assert.deepEqual(range(await diag(p)),[0,frames],'even a ruler double-click preserves the selection');
   cursorAt(await diag(p),550);await key(p,'Escape');
   await drag(p,300,650);const original=await diag(p);
   // Ruler clicks seek inside/outside the range and at its handles, including
   // modified clicks; small pointer jitter must leave the exact range intact.
   for(const [x,modifier] of [[850,null],[475,null],[650,null],[800,'Shift'],[425,'Alt'],[300,null]]){
    if(modifier)await p.keyboard.down(modifier);
    await p.mouse.move(x,140);await tick(p);await p.mouse.down();await tick(p);
    await p.mouse.move(x+2,141);await tick(p);await p.mouse.up();await tick(p);
    if(modifier)await p.keyboard.up(modifier);
    const clicked=await diag(p);assert.deepEqual(range(clicked),range(original));
    cursorAt(clicked,x);
    assert.equal(clicked.view_start,original.view_start);assert.equal(clicked.history_states,1);assert.equal(clicked.dirty,false);
   }
   await drag(p,475,575,false,140);const rulerMoved=await diag(p);
   assert.equal(length(rulerMoved),length(original));assert(rulerMoved.selection_start>original.selection_start);
   assert.equal(rulerMoved.position,original.position);assert.equal(rulerMoved.history_states,1);assert.equal(rulerMoved.dirty,false);
   await drag(p,575,475,false,140);assert.deepEqual(range(await diag(p)),range(original));
   // Zoom enough to pan. Starting outside the selection stays a pan even when
   // the pointer crosses the range or leaves the ruler vertically.
   await key(p,'=');await p.waitForFunction(()=>coldeditWeb.diagnostics().view_span===coldeditWeb.diagnostics().view_target_span);
   const beforePan=await diag(p);await drag(p,1000,900,false,140);const panned=await diag(p);
   assert(panned.view_start>beforePan.view_start);assert.equal(panned.view_span,beforePan.view_span);
   assert.deepEqual(range(panned),range(original));assert.equal(panned.position,original.position);
   await p.mouse.move(1100,140);await tick(p);await p.mouse.down();await tick(p);
   await p.mouse.move(1000,350,{steps:8});await tick(p);await p.mouse.up();await tick(p);
   assert((await diag(p)).view_start>panned.view_start);assert.deepEqual(range(await diag(p)),range(original));
   const released=await diag(p);await p.mouse.move(800,140);await tick(p);assert.equal((await diag(p)).view_start,released.view_start);
   await key(p,'Escape');const noRange=await diag(p);await drag(p,900,1000,false,140);
   assert((await diag(p)).view_start<noRange.view_start);assert.equal(length(await diag(p)),0);assert.equal((await diag(p)).position,noRange.position);
   const beforeSeek=await diag(p);await p.mouse.click(500,140,{delay:40});await tick(p);
   assert.equal(length(await diag(p)),0);cursorAt(await diag(p),500,beforeSeek);
   await key(p,'f');await p.waitForFunction(()=>coldeditWeb.diagnostics().view_span===48000);await drag(p,300,650);
   await exportAudio(p,`artifacts/web-save-selection-menu-${scale}.wav`,(f,c)=>sample(original.selection_start+f,c),length(original),async()=>{await key(p,'F10');await key(p,'Home');await key(p,'ArrowDown');await key(p,'ArrowDown');await key(p,'Enter');});
   await drag(p,475,575);let moved=await diag(p);assert.equal(length(moved),length(original));assert(moved.selection_start>original.selection_start);assert.equal(moved.history_states,1);assert.equal(moved.dirty,false);assert.equal(moved.position,original.position);
   await drag(p,400,500,true);assert(length(await diag(p))<length(original),'Alt-drag draws a fresh range');
   await key(p,'Escape');await drag(p,300,650);assert.deepEqual(range(await diag(p)),range(original));
   await key(p,'Control+x');const cut=await diag(p),n=length(original),a=original.selection_start,b=original.selection_end;
   assert.deepEqual(range(cut),range(original));assert.equal(cut.frames,frames-n);assert.equal(cut.history_states,2);
   await drag(p,xAt(cut,(a+b)/2),xAt(cut,(a+b)/2)+65);moved=await diag(p);assert.equal(length(moved),n);assert.equal(moved.history_states,2);
   const start=moved.selection_start,end=Math.min(moved.selection_end,start+n),cutSample=(f,c)=>f<frames-n?sample(f<a?f:f+n,c):0;
   await key(p,'Control+v');await p.waitForFunction(()=>!coldeditWeb.diagnostics().busy);const mixed=await diag(p);
   assert.equal(mixed.undo_label,'Paste into selection');assert.deepEqual(range(mixed),range(moved));assert.equal(mixed.frames,Math.max(frames-n,end));assert.equal(mixed.history_states,3);
   await exportAudio(p,`artifacts/web-save-selection-inspector-${scale}.wav`,(f,c)=>cutSample(start+f,c)+sample(a+f,c),end-start,async()=>{await p.mouse.click(1290,747,{delay:40});await tick(p);});
   await exportAudio(p,`artifacts/web-selection-mix-${scale}.wav`,(f,c)=>cutSample(f,c)+(f>=start&&f<end?sample(a+f-start,c):0),mixed.frames);
   await key(p,'Control+z');assert.equal((await diag(p)).frames,frames-n);await key(p,'Control+Shift+z');assert.equal((await diag(p)).undo_label,'Paste into selection');await key(p,'Control+z');
   await key(p,'Control+Shift+v');assert.equal((await diag(p)).undo_label,'Paste');await key(p,'Control+z');await key(p,'Control+z');assert.equal((await diag(p)).frames,frames);
   // Cut at EOF retains the old end; the range can be moved back over audio.
   await key(p,'Escape');await key(p,'f');await p.waitForFunction(()=>Math.abs(coldeditWeb.diagnostics().view_span-48000)<.01);
   await drag(p,850,1170);const tail=await diag(p);await key(p,'Control+x');const tailCut=await diag(p);
   assert.deepEqual(range(tailCut),range(tail));assert(tailCut.selection_end>tailCut.frames);
   // A retained end can move inward and then back outward during one drag.
   const edge=xAt(tailCut,tailCut.selection_end)-2;await p.mouse.move(edge,350);await tick(p);await p.mouse.down();await tick(p);
   await p.mouse.move(edge-90,350,{steps:6});await tick(p);const inward=(await diag(p)).selection_end;
   await p.mouse.move(edge-30,350,{steps:6});await tick(p);assert((await diag(p)).selection_end>inward);
   await p.mouse.move(edge,350,{steps:6});await tick(p);await p.mouse.up();await tick(p);assert.deepEqual(range(await diag(p)),range(tail));
   await drag(p,xAt(tailCut,(tail.selection_start+tail.selection_end)/2),xAt(tailCut,(tail.selection_start+tail.selection_end)/2)-200);
   assert.equal(length(await diag(p)),length(tail));assert.equal((await diag(p)).history_states,2);await key(p,'Control+z');
   // Empty documents retain the range and can recover the cut signal with Paste into.
   await key(p,'Control+a');await key(p,'Control+x');assert.equal((await diag(p)).frames,0);assert.deepEqual(range(await diag(p)),[0,frames]);
   await key(p,'Control+v');await p.waitForFunction(()=>!coldeditWeb.diagnostics().busy);assert.equal((await diag(p)).frames,frames);
   await exportAudio(p,`artifacts/web-selection-recovered-${scale}.wav`,sample,frames);
   await key(p,'Escape');await key(p,'f');await p.waitForFunction(()=>Math.abs(coldeditWeb.diagnostics().view_span-48000)<.01);await drag(p,300,650);
   const saved=await diag(p);
   await exportAudio(p,`artifacts/web-save-selection-context-${scale}.wav`,(f,c)=>sample(saved.selection_start+f,c),length(saved),async()=>{await p.mouse.click(500,350,{button:'right',delay:40});await tick(p);await p.screenshot({path:`artifacts/web-selection-${scale}.png`});await key(p,'Home');for(let i=0;i<4;i++)await key(p,'ArrowDown');await key(p,'Enter');});
   assert.deepEqual(errors,[]);assert.equal((await diag(p)).error,'');reports.push({scale,passed:true,diagnostics:await diag(p)});console.log(`PASS selection ${scale}x: ruler range drag/pan, double-click toggle, range drag, Alt-drag, Save selection WAV via menu/inspector/context, retained cut, mix samples/history, replacement, EOF and cut-all recovery`);
  }catch(e){await captureFailure(p,`web-selection-failure-${scale}`,e);throw e;}finally{await context.close();}
 }await slowDoubleClicks(browser);}finally{await browser.close();fs.writeFileSync('artifacts/web-selection.json',JSON.stringify(reports,null,2));}
})().catch(e=>{console.error(e);process.exitCode=1;});
