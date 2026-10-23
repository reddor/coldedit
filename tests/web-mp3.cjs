const {chromium}=require('playwright');
const assert=require('node:assert/strict'),fs=require('node:fs');
const diag=p=>p.evaluate(()=>coldeditWeb.diagnostics());
const tick=p=>p.evaluate(()=>new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r))));
async function key(p,k){await p.keyboard.press(k);await tick(p);}
async function click(p,x,y){await p.mouse.click(x,y,{delay:40});await tick(p);}
async function format(p,index){const d=await diag(p);await key(p,'F10');await key(p,'Home');
  for(let i=0;i<(d.selection_end>d.selection_start?8:7);i++)await key(p,'ArrowDown');
  await key(p,'ArrowRight');await key(p,'Home');for(let i=0;i<index;i++)await key(p,'ArrowDown');await key(p,'Enter');
  assert.equal((await diag(p)).export_format,index);}
async function download(p,name,selection=false){const pending=p.waitForEvent('download');await key(p,selection?'Control+Shift+e':'Control+e');
  const d=await pending;await d.saveAs('artifacts/'+name);await p.waitForFunction(()=>!coldeditWeb.diagnostics().busy);
  const mime=await p.evaluate(async()=> (await fetch(document.querySelector('#download-export').href)).headers.get('Content-Type'));
  await p.locator('#dismiss').click();await tick(p);return {bytes:fs.readFileSync('artifacts/'+name),name:d.suggestedFilename(),mime};}
function fixture(rate=44100,channels=2){const frames=rate+137,b=Buffer.alloc(44+frames*channels*4);
  b.write('RIFF');b.writeUInt32LE(b.length-8,4);b.write('WAVEfmt ',8);b.writeUInt32LE(16,16);b.writeUInt16LE(3,20);
  b.writeUInt16LE(channels,22);b.writeUInt32LE(rate,24);b.writeUInt32LE(rate*channels*4,28);b.writeUInt16LE(channels*4,32);b.writeUInt16LE(32,34);
  b.write('data',36);b.writeUInt32LE(b.length-44,40);for(let f=0;f<frames;f++)for(let c=0;c<channels;c++)b.writeFloatLE(.35*Math.sin(f*2*Math.PI*(c?680:440)/rate),44+(f*channels+c)*4);return b;}
async function open(p,name,bytes){await p.locator('#file-input').setInputFiles({name,mimeType:name.endsWith('.mp3')?'audio/mpeg':'audio/wav',buffer:bytes});
  await p.waitForFunction(n=>coldeditWeb.diagnostics().title===n&&!coldeditWeb.diagnostics().busy,name);}
async function insert(p,frames){const before=await diag(p);await key(p,'Insert');await click(p,620,521);await key(p,'Control+a');await p.keyboard.type(String(frames));await key(p,'Enter');
  await p.waitForFunction(count=>coldeditWeb.diagnostics().frames===count,before.frames+frames);}
(async()=>{
 const browser=await chromium.launch({channel:process.env.COLDEDIT_BROWSER||'msedge',headless:true,args:['--mute-audio']});const reports=[];
 try {
  for(const index of [5,6,7]) {
   const context=await browser.newContext({viewport:{width:1440,height:944},deviceScaleFactor:1.25,acceptDownloads:true});const p=await context.newPage(),errors=[];p.setDefaultTimeout(30000);p.on('pageerror',e=>errors.push(e.message));
   try {
    await p.goto(process.env.COLDEDIT_WEB_URL||'http://localhost:8080');await p.waitForFunction(()=>document.querySelector('#loading').hidden);
    await open(p,'mp3-source.wav',fixture());await key(p,'Control+a');await key(p,'i');
    await p.keyboard.down('Alt');await p.mouse.move(300,350);await tick(p);await p.mouse.down();await tick(p);await p.mouse.move(650,350,{steps:12});await p.mouse.up();await p.keyboard.up('Alt');await tick(p);
    const before=await diag(p),expected=await download(p,'mp3-reference-'+index+'.wav',true);
    await format(p,index);const mp3=await download(p,'mp3-selection-'+index+'.mp3',true);
    assert.equal(mp3.name,'mp3-source-selection.mp3');assert.equal(mp3.mime,'audio/mpeg');assert.equal(mp3.bytes[0],255);assert(mp3.bytes.includes(Buffer.from('LAME')));
    let d=await diag(p);for(const field of ['frames','dirty','history_states','selection_start','selection_end','audio_bytes'])assert.equal(d[field],before[field],field);
    await key(p,'Control+z');await open(p,'reimport.mp3',mp3.bytes);d=await diag(p);
    assert.equal(d.frames,before.selection_end-before.selection_start);assert.equal(d.sample_rate,44100);assert.equal(d.channels,2);
    await format(p,1);const decoded=await download(p,'mp3-decoded-'+index+'.wav');assert.equal(decoded.bytes.length,expected.bytes.length);
    let energy=0,error=0;for(let f=1000;f<d.frames-1000;f++)for(let c=0;c<2;c++){const at=44+(f*2+c)*3,a=expected.bytes.readIntLE(at,3)/8388608,b=decoded.bytes.readIntLE(at,3)/8388608;energy+=a*a;error+=(a-b)*(a-b);}
    assert(error/energy<.025,'decoded selection retains timing, fades and stereo signal');
    await key(p,'r');await format(p,index);await download(p,'mp3-full-'+index+'.mp3');assert.equal((await diag(p)).dirty,false);
    if(index===5){await open(p,'high-rate.wav',fixture(96000,1));await format(p,5);const high=await download(p,'mp3-resampled.mp3');
      await open(p,'resampled.mp3',high.bytes);d=await diag(p);assert.equal(d.sample_rate,48000);assert.equal(d.channels,1);assert(Math.abs(d.frames-(96000+137)/2)<=1);}
    if(index===7){
      await key(p,'Home');await insert(p,50000000);const state=await diag(p);await key(p,'Control+e');await p.waitForFunction(()=>coldeditWeb.diagnostics().busy);
      await click(p,370,923);await p.waitForFunction(()=>!coldeditWeb.diagnostics().busy);d=await diag(p);assert(d.error.toLowerCase().includes('cancel'));assert.equal(d.dirty,true);assert.equal(d.audio_bytes,state.audio_bytes);assert.equal(d.history_states,state.history_states);
      assert.equal(await p.evaluate(()=>Module.FS.readdir('/exports').length),2);await key(p,'Tab');await key(p,'Enter');
      await key(p,'Control+z');await insert(p,200000000);const large=await diag(p);await key(p,'Control+e');await p.waitForFunction(()=>coldeditWeb.diagnostics().error.includes('128 MiB'));
      d=await diag(p);assert.equal(d.busy,false);assert.equal(d.audio_bytes,large.audio_bytes);assert.equal(d.history_states,large.history_states);assert.equal(await p.evaluate(()=>Module.FS.readdir('/exports').length),2);
    }
    assert.deepEqual(errors,[]);reports.push({format:index,selectionFrames:before.selection_end-before.selection_start,bytes:mp3.bytes.length,relativeError:error/energy});
    console.log('PASS browser MP3 preset '+index+': selection/full export, audio/mpeg download, gapless reimport, signal and history'+(index===5?', 96 -> 48 kHz mono':index===7?', cancellation and web output cap':''));
   }catch(e){console.error(await p.evaluate(()=>({diagnostics:coldeditWeb.diagnostics(),exports:Module.FS.readdir('/exports').map(name=>({name,size:Module.FS.stat('/exports/'+name).size}))})));await p.screenshot({path:'artifacts/web-mp3-failure-'+index+'.png'});throw e;}finally{await context.close();}
  }
 }finally{await browser.close();fs.writeFileSync('artifacts/web-mp3.json',JSON.stringify(reports,null,2));}
})().catch(e=>{console.error(e);process.exitCode=1;});
