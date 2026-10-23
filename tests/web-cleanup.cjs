const {chromium}=require('playwright');
const assert=require('node:assert/strict'),fs=require('node:fs');
const width=1440,height=944,cx=width/2,cy=(height+44)/2,frames=48000;
const diag=p=>p.evaluate(()=>coldeditWeb.diagnostics());
const tick=p=>p.evaluate(()=>new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r))));
async function key(p,k){await p.keyboard.press(k);await tick(p);}
async function click(p,x,y){await p.mouse.click(x,y,{delay:50});await tick(p);}
async function drag(p,a,b){await p.mouse.move(a,350);await tick(p);await p.mouse.down();await tick(p);await p.mouse.move(b,350,{steps:12});await tick(p);await p.mouse.up();await tick(p);}
function sample(f,c){return Math.fround((c?-.1:.2)+(c?.15:.3)*Math.sin(f*2*Math.PI/48));}
function fixture(){const b=Buffer.alloc(44+frames*8);b.write('RIFF');b.writeUInt32LE(b.length-8,4);b.write('WAVEfmt ',8);b.writeUInt32LE(16,16);b.writeUInt16LE(3,20);b.writeUInt16LE(2,22);b.writeUInt32LE(48000,24);b.writeUInt32LE(384000,28);b.writeUInt16LE(8,32);b.writeUInt16LE(32,34);b.write('data',36);b.writeUInt32LE(frames*8,40);for(let f=0;f<frames;f++)for(let c=0;c<2;c++)b.writeFloatLE(sample(f,c),44+f*8+c*4);return b;}
async function submenu(p,root,row,child){
  await key(p,'F10');await key(p,'Home');for(let i=0;i<root;i++)await key(p,'ArrowDown');await key(p,'ArrowRight');await key(p,'Home');
  for(let i=0;i<row;i++)await key(p,'ArrowDown');
  if(child!==undefined){await key(p,'ArrowRight');await key(p,'Home');for(let i=0;i<child;i++)await key(p,'ArrowDown');}
  await key(p,'Enter');
}
async function processMenu(p,row){await key(p,'Control+a');await submenu(p,6,row);assert.equal((await diag(p)).popup_open,true);}
async function exportAudio(p,path){const download=p.waitForEvent('download');await key(p,'Control+e');await(await download).saveAs(path);await p.waitForFunction(()=>!coldeditWeb.diagnostics().busy);await p.locator('#dismiss').click();await tick(p);return fs.readFileSync(path);}
function read(b,f,c){const channels=b.readUInt16LE(22);return b.readIntLE(44+(f*channels+c)*3,3)/8388608;}
async function applied(p){await p.waitForFunction(()=>!coldeditWeb.diagnostics().busy&&!coldeditWeb.diagnostics().popup_open);}
(async()=>{
  const browser=await chromium.launch({channel:'msedge',headless:true,args:['--mute-audio']});const reports=[];
  try{for(const scale of (process.env.COLDEDIT_DPI||'1,1.25,2').split(',').map(Number)){
    const context=await browser.newContext({viewport:{width,height},deviceScaleFactor:scale,acceptDownloads:true});const p=await context.newPage(),errors=[];
    p.on('pageerror',e=>errors.push(e.message));p.setDefaultTimeout(20000);
    try{
      await p.goto(process.env.COLDEDIT_WEB_URL||'http://localhost:8080/');await p.waitForFunction(()=>document.querySelector('#loading').hidden);
      await p.locator('#file-input').setInputFiles({name:'cleanup.wav',mimeType:'audio/wav',buffer:fixture()});await p.waitForFunction(()=>coldeditWeb.diagnostics().frames===48000);
      // Preview, bypass, and cancel leave the source and history intact, and release processed PCM.
      await processMenu(p,5);await click(p,cx-155,cy+40);
      await p.waitForFunction(()=>coldeditWeb.diagnostics().cleanup_preview&&coldeditWeb.diagnostics().playing);
      assert.equal((await diag(p)).history_states,1);assert.equal((await diag(p)).dirty,false);assert.equal((await diag(p)).processed_bytes,frames*8);
      await click(p,cx-75,cy+40);assert.equal((await diag(p)).cleanup_bypass,true);
      await click(p,cx-30,cy+92);await p.waitForFunction(()=>coldeditWeb.diagnostics().processed_bytes===0);
      assert.equal((await diag(p)).dirty,false);assert.equal((await diag(p)).playing,false);
      await processMenu(p,5);await click(p,cx-155,cy+92);await applied(p);
      assert.equal((await diag(p)).undo_label,'Remove DC offset');
      const dc=await exportAudio(p,`artifacts/web-dc-${scale}.wav`);
      for(let c=0;c<2;c++){let sum=0;for(let f=0;f<frames;f++){sum+=read(dc,f,c);assert(Math.abs(read(dc,f,c)-(sample(f,c)-(c?-.1:.2)))<2e-6);}assert(Math.abs(sum/frames)<1e-6);}
      await key(p,'Control+z');
      await processMenu(p,4);await p.keyboard.down('Control');await p.mouse.click(cx-100,cy-17,{delay:50});await p.keyboard.up('Control');await tick(p);await key(p,'Control+a');await p.keyboard.type('-6',{delay:40});await key(p,'Enter');
      await click(p,cx-155,cy+112);await applied(p);
      const normalized=await exportAudio(p,`artifacts/web-normalize-${scale}.wav`);let peak=0;
      for(let f=0;f<frames;f++)peak=Math.max(peak,Math.abs(read(normalized,f,0)));
      assert(Math.abs(peak-Math.pow(10,-6/20))<2e-6);await key(p,'Control+z');
      await processMenu(p,6);await click(p,cx-155,cy+60);await p.waitForFunction(()=>coldeditWeb.diagnostics().cleanup_preview);
      await p.mouse.move(950,150);await tick(p);await p.screenshot({path:`artifacts/web-cleanup-${scale}.png`});
      await click(p,cx-155,cy+112);await applied(p);const hp=await exportAudio(p,`artifacts/web-high-pass-${scale}.wav`);
      let mean=0;for(let f=24000;f<frames;f++)mean+=read(hp,f,0);assert(Math.abs(mean/24000)<.0001);await key(p,'Control+z');
      await processMenu(p,7);await p.keyboard.down('Control');await p.mouse.click(cx-100,cy-17,{delay:50});await p.keyboard.up('Control');await tick(p);await key(p,'Control+a');await p.keyboard.type('400',{delay:40});await key(p,'Enter');
      await click(p,cx-155,cy+112);await applied(p);const lp=await exportAudio(p,`artifacts/web-low-pass-${scale}.wav`);
      let maxDeviation=0;for(let f=24000;f<frames;f++)maxDeviation=Math.max(maxDeviation,Math.abs(read(lp,f,0)-.2));assert(maxDeviation<.06);await key(p,'Control+z');
      // Every channel tool is exercised through the menus and checked in the exported WAV.
      for(let operation=0;operation<7;operation++){
        await key(p,'Control+a');await submenu(p,6,8,operation);
        const channels=operation<3?1:2;assert.equal((await diag(p)).channels,channels);
        const b=await exportAudio(p,`artifacts/web-channel-${operation}-${scale}.wav`);assert.equal(b.readUInt16LE(22),channels);
        for(let f=0;f<frames;f+=19)for(let c=0;c<channels;c++){
          let expected=sample(f,c);
          if(operation===1)expected=sample(f,1);if(operation===2)expected=(sample(f,0)+sample(f,1))/2;if(operation===3)expected=sample(f,1-c);
          if(operation===4||(operation===5&&c===0)||(operation===6&&c===1))expected=-expected;
          assert(Math.abs(read(b,f,c)-expected)<2e-6);
        }
        await key(p,'Control+z');assert.equal((await diag(p)).channels,2);
      }
      await key(p,'Escape');await drag(p,300,650);const raw=await diag(p);await key(p,'j');const snapped=await diag(p);
      for(const field of ['selection_start','selection_end']){
        const f=snapped[field];assert(Math.abs(f-raw[field])<=240);
        assert([0,1].some(c=>Math.sign(sample(f-1,c))!==Math.sign(sample(f,c))||sample(f,c)===0));
      }
      await key(p,'Control+a');await submenu(p,5,5);assert.equal((await diag(p)).auto_edge_fades,true);
      await key(p,'Escape');await drag(p,300,650);const selected=await diag(p),start=selected.selection_start,length=selected.selection_end-start;
      await key(p,'Delete');assert.equal((await diag(p)).frames,frames-length);
      const edged=await exportAudio(p,`artifacts/web-edit-edges-${scale}.wav`);
      assert.equal(read(edged,start-1,0),0);assert.equal(read(edged,start,1),0);await key(p,'Control+z');assert.equal((await diag(p)).frames,frames);
      // Reject a filter larger than the processing budget without losing edits or allocating PCM.
      await key(p,'Control+a');await submenu(p,5,5);await key(p,'Home');await key(p,'Insert');
      await click(p,cx-100,cy+27);await key(p,'Control+a');await p.keyboard.type('20000000',{delay:35});await key(p,'Enter');
      const budgetBefore=await diag(p);await processMenu(p,6);await click(p,cx-155,cy+112);
      await p.waitForFunction(()=>coldeditWeb.diagnostics().error.includes('128 MiB budget'));
      assert.equal((await diag(p)).processed_bytes,budgetBefore.processed_bytes);assert.equal((await diag(p)).history_states,budgetBefore.history_states);
      assert.deepEqual(errors,[]);reports.push({scale,passed:true,diagnostics:await diag(p)});
      console.log(`PASS cleanup ${scale}x: preview/bypass/cancel, DC, normalization, filters, seven channel tools, snapping, edge fades, memory budget`);
    }catch(e){await p.screenshot({path:`artifacts/web-cleanup-failure-${scale}.png`});throw e;}finally{await context.close();}
  }}finally{await browser.close();fs.writeFileSync('artifacts/web-cleanup.json',JSON.stringify(reports,null,2));}
})().catch(e=>{console.error(e);process.exitCode=1;});
