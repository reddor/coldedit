// Capture transient loading states in the same animation frame as the editor;
// a later Playwright screenshot could miss a completed background operation.
const {chromium}=require('playwright');
const {PNG}=require('playwright-core/lib/utilsBundle');
const assert=require('node:assert/strict'),fs=require('node:fs');
const {captureFailure}=require('./web-test-helpers.cjs');
(async()=>{
  const browser=await chromium.launch({channel:process.env.COLDEDIT_BROWSER||'msedge',headless:true,args:['--mute-audio']});
  const context=await browser.newContext({viewport:{width:1440,height:944},deviceScaleFactor:1});
  const page=await context.newPage();page.setDefaultTimeout(60000);
  const errors=[];page.on('pageerror',e=>errors.push(e.message));
  try {
    await page.goto(process.env.COLDEDIT_WEB_URL||'http://localhost:8080');
    await page.waitForFunction(()=>document.querySelector('#loading').hidden);
    await page.evaluate(()=>{
      window.loadingCaptures={};
      function capture(){
        const d=coldeditWeb.diagnostics();
        const phase=d.opening?'import':d.waveform_building?'peaks':null;
        if(phase&&!loadingCaptures[phase]){
          const canvas=document.querySelector('#canvas'),gl=canvas.getContext('webgl2');
          const bytes=new Uint8Array(canvas.width*canvas.height*4);
          gl.readPixels(0,0,canvas.width,canvas.height,gl.RGBA,gl.UNSIGNED_BYTE,bytes);
          const copy=document.createElement('canvas');copy.width=canvas.width;copy.height=canvas.height;
          const ctx=copy.getContext('2d'),pixels=ctx.createImageData(copy.width,copy.height),row=copy.width*4;
          for(let y=0;y<copy.height;y++)pixels.data.set(bytes.subarray((copy.height-1-y)*row,(copy.height-y)*row),y*row);
          ctx.putImageData(pixels,0,0);loadingCaptures[phase]={image:copy.toDataURL(),diagnostics:d};
        }
        if(!window.loadingCaptureDone)requestAnimationFrame(capture);
      }
      requestAnimationFrame(capture);
    });
    await page.locator('#file-input').setInputFiles('artifacts/codec-fixtures/at-budget.flac');
    await page.waitForFunction(()=>coldeditWeb.diagnostics().opening&&window.loadingCaptures.import);
    // Use the new cancel button in the waveform area, not the status bar.
    const canvas=await page.locator('#canvas').boundingBox();
    await page.mouse.click(canvas.x+450,canvas.y+81+(canvas.height-220-150)/2+106,{delay:30});
    await page.waitForFunction(()=>!coldeditWeb.diagnostics().busy);
    let d=await page.evaluate(()=>coldeditWeb.diagnostics());
    assert(d.error.toLowerCase().includes('cancel'));assert.equal(d.frames,0);assert.equal(d.audio_bytes,0);
    await page.keyboard.press('Tab');await page.keyboard.press('Enter');
    await page.waitForFunction(()=>!coldeditWeb.diagnostics().error);
    await page.locator('#file-input').setInputFiles('artifacts/codec-fixtures/at-budget.flac');
    await page.waitForFunction(()=>{const d=coldeditWeb.diagnostics();return d.frames===128*1024*1024/8&&!d.busy&&d.waveform_ready;});
    const captures=await page.evaluate(()=>{window.loadingCaptureDone=true;return window.loadingCaptures;});
    for(const phase of ['import','peaks']){
      assert(captures[phase],`missing ${phase} indicator`);
      const bytes=Buffer.from(captures[phase].image.split(',')[1],'base64');
      fs.writeFileSync(`artifacts/web-loading-${phase}.png`,bytes);
      const png=PNG.sync.read(bytes);let teal=0;
      for(let y=250;y<550;y++)for(let x=330;x<820;x++){
        const i=(y*png.width+x)*4;
        if(png.data[i+1]-png.data[i]>45&&png.data[i+1]>png.data[i+2])teal++;
      }
      assert(teal>150,`${phase} has no visible progress text/bar in the waveform area`);
      delete captures[phase].image;
    }
    d=await page.evaluate(()=>coldeditWeb.diagnostics());
    assert.equal(d.waveform_progress,1);assert.equal(d.error,'');assert.deepEqual(errors,[]);
    fs.writeFileSync('artifacts/web-loading.json',JSON.stringify({captures,completed:d},null,2));
    console.log('PASS loading: visible import and peak progress, canvas cancellation, recovery, ready waveform');
  }catch(e){await captureFailure(page,'web-loading-failure',e);throw e;}
  finally{await context.close();await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
