// Check per-pixel envelopes, detail and intermediate frames through real zoom input.
const {chromium}=require('playwright');
const assert=require('node:assert/strict'),fs=require('node:fs');
const {PNG}=require('playwright-core/lib/utilsBundle'); // PNG reader bundled with the pinned test runtime.
const {settle:tick,captureFailure}=require('./web-test-helpers.cjs');
const diag=p=>p.evaluate(()=>coldeditWeb.diagnostics());
async function settled(p){await tick(p);await p.waitForFunction(()=>{const d=coldeditWeb.diagnostics();return Math.abs(d.view_span-d.view_target_span)<.01&&Math.abs(d.view_start-d.view_target_start)<.01;});}
function fixture(dc=false){
  const n=dc?16384:48000*8,b=Buffer.alloc(44+n*8);b.write('RIFF');b.writeUInt32LE(b.length-8,4);b.write('WAVEfmt ',8);b.writeUInt32LE(16,16);
  b.writeUInt16LE(3,20);b.writeUInt16LE(2,22);b.writeUInt32LE(48000,24);b.writeUInt32LE(384000,28);b.writeUInt16LE(8,32);b.writeUInt16LE(32,34);b.write('data',36);b.writeUInt32LE(n*8,40);
  for(let i=0;i<n;i++)for(let c=0;c<2;c++){
    const envelope=.35+.23*Math.sin(i*.00008+c),wave=Math.sin(i*.017+c)*.8+Math.sin(i*.153)*.2;
    b.writeFloatLE(dc?(c?-.23:.37):i%6151===0?.95:envelope*wave,44+(i*2+c)*4);
  }
  return {name:dc?'waveform-dc.wav':'waveform-detail.wav',mimeType:'audio/wav',buffer:b};
}
async function checkAnimation(p,scale){
  await p.locator('#file-input').setInputFiles(fixture(true));
  await p.waitForFunction(()=>coldeditWeb.diagnostics().frames===16384&&coldeditWeb.diagnostics().peak_bytes>0);
  await settled(p);
  while((await diag(p)).view_span/(1100*scale)>.65){await p.keyboard.press('+');await settled(p);}
  const bytes=(await diag(p)).audio_bytes;
  // Read back after the app's animation callback, before browser presentation.
  // A constant signal must retain stable coverage in every column, including
  // the faint side fringes, while crossing the old 1..4 samples/pixel crossfade.
  await p.evaluate(()=>{
    const canvas=document.querySelector('#canvas'),gl=canvas.getContext('webgl2');
    const capture=window.waveformAnimation={running:true,frames:[]};
    function sample(){
      if(!capture.running)return;
      const d=coldeditWeb.diagnostics(),scale=d.font_raster_scale;
      const height=d.display_height/d.ui_scale,lane=(height-220-34)/2;
      const x=Math.ceil(80*scale),width=Math.floor(1160*scale)-x;
      const frame={span:d.view_span,target:d.view_target_span,min:Infinity,max:-Infinity,mean:[]};
      for(let c=0;c<2;c++){
        const y=(115+lane*(c+.5)-lane*.38*(c?-.23:.37))*scale;
        const top=Math.floor(y)-3,rows=7,data=new Uint8Array(width*rows*4);
        gl.readPixels(x,canvas.height-top-rows,width,rows,gl.RGBA,gl.UNSIGNED_BYTE,data);
        let total=0;
        for(let col=0;col<width;col++){
          const difference=row=>data[(row*width+col)*4+(c?2:1)]-data[(row*width+col)*4];
          const background=difference(0),full=c?115:124;
          let ink=0;for(let row=1;row<rows-1;row++)ink+=(difference(row)-background)/(full-background);
          frame.min=Math.min(frame.min,ink);frame.max=Math.max(frame.max,ink);
          total+=ink;
        }
        frame.mean.push(total/width);
      }
      capture.frames.push(frame);requestAnimationFrame(sample);
    }
    requestAnimationFrame(sample);
  });
  for(let i=0;i<8;i++){await p.keyboard.press('-');await tick(p);}
  await settled(p);
  const frames=await p.evaluate(()=>{waveformAnimation.running=false;return waveformAnimation.frames;});
  assert(frames.length>4,'not enough intermediate animation frames');
  assert(frames.some(f=>Math.abs(f.span-f.target)>1),'only captured settled views');
  assert(frames.some(f=>f.span/(1100*scale)<1)&&frames.some(f=>f.span/(1100*scale)>4),'animation did not cross both old thresholds');
  for(const f of frames){
    assert(f.min>.94,`missing/thin column during zoom at ${scale}x: ${JSON.stringify(f)}`);
    assert(f.max<1.28,`excessive stroke thickness at ${scale}x: ${JSON.stringify(f)}`);
    for(let c=0;c<2;c++)assert(Math.abs(f.mean[c]-frames[0].mean[c])<.02,`brightness jump during zoom at ${scale}x: ${JSON.stringify(f)}`);
  }
  await p.screenshot({path:`artifacts/web-waveform-dc-${scale}.png`});
  const d=await diag(p);assert.equal(d.audio_bytes,bytes);assert.equal(d.history_states,1);assert.equal(d.error,'');
  return {scale,name:'animation',frames:frames.length,minCoverage:Math.min(...frames.map(f=>f.min)),maxCoverage:Math.max(...frames.map(f=>f.max))};
}
function checkPixels(buffer,scale){
  const png=PNG.sync.read(buffer);
  // Both channels must span the entire plot. This catches 16-bit draw-index
  // overflow, which previously erased the right half of the second channel.
  for(let channel=0;channel<2;channel++)for(let part=0;part<8;part++){
    let colored=0,soft=0;
    for(let x=Math.ceil((80+part*134)*scale);x<Math.floor((80+(part+1)*134)*scale);x++)
      for(let y=Math.ceil((channel?490:170)*scale);y<Math.floor((channel?800:475)*scale);y++){
        const at=(y*png.width+x)*4,r=png.data[at],g=png.data[at+1],b=png.data[at+2];
        if(channel?b-r>35&&g-r>20:g-r>45&&g>b){colored++;if(r>30&&r<90)soft++;}
      }
    assert(colored>20,`missing waveform channel ${channel}, section ${part}, DPI ${scale}`);
    assert(soft>0,`missing antialiasing channel ${channel}, section ${part}, DPI ${scale}`);
  }
}
(async()=>{
  const browser=await chromium.launch({channel:process.env.COLDEDIT_BROWSER||'msedge',headless:true,args:['--mute-audio']});const reports=[];
  try {for(const scale of (process.env.COLDEDIT_DPI||'1,1.25,2').split(',').map(Number)){
    const context=await browser.newContext({viewport:{width:1440,height:944},deviceScaleFactor:scale}),p=await context.newPage(),errors=[];
    p.on('pageerror',e=>errors.push(e.message));
    try {
      await p.goto(process.env.COLDEDIT_WEB_URL||'http://localhost:8080');await p.waitForFunction(()=>document.querySelector('#loading').hidden);
      await p.locator('#file-input').setInputFiles(fixture());await p.waitForFunction(()=>coldeditWeb.diagnostics().frames===384000&&coldeditWeb.diagnostics().peak_bytes>0);await settled(p);
      const bytes=(await diag(p)).audio_bytes;
      for(const [name,target] of [['overview',Infinity],['envelope',12],['transition',2.5],['samples',.65]]){
        let steps=0;
        while((await diag(p)).view_span/(1100*scale)>target){assert(++steps<40);await p.keyboard.press('+');await settled(p);}
        await p.mouse.move(1200,20);await tick(p);const pixels=await p.screenshot({path:`artifacts/web-waveform-${name}-${scale}.png`});checkPixels(pixels,scale);
        const d=await diag(p);assert.equal(d.audio_bytes,bytes);assert.equal(d.history_states,1);assert.equal(d.error,'');reports.push({scale,name,span:d.view_span});
      }
      reports.push(await checkAnimation(p,scale));
      assert.deepEqual(errors,[]);console.log(`PASS waveform ${scale}x: envelope, sample detail, animation coverage, no audio or history mutation`);
    }catch(e){await captureFailure(p,`web-waveform-failure-${scale}`,e);throw e;}finally{await context.close();}
  }}finally{await browser.close();fs.writeFileSync('artifacts/web-waveform.json',JSON.stringify(reports,null,2));}
})().catch(e=>{console.error(e);process.exitCode=1;});
