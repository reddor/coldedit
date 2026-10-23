// Isolated server and browser tests; no user browser profile or existing server.
const {spawn}=require('node:child_process');
require('node:fs').mkdirSync('artifacts',{recursive:true});
const port=process.env.COLDEDIT_TEST_PORT||'8091';
const server=spawn(process.execPath,['web/serve.mjs','build-web/site',port],{windowsHide:true,stdio:['ignore','pipe','inherit']});
(async()=>{
  await new Promise((resolve,reject)=>{server.stdout.once('data',resolve);server.once('error',reject);server.once('exit',code=>reject(new Error('Server exited: '+code)));});
  for(const test of process.argv.slice(2)) {
    const code=await new Promise((resolve,reject)=>{
      const child=spawn(process.execPath,['tests/'+test],{windowsHide:true,stdio:'inherit',env:{...process.env,COLDEDIT_WEB_URL:'http://127.0.0.1:'+port} });
      child.once('error',reject);child.once('exit',resolve);
    });
    if(code!==0) throw new Error(test+' failed ('+code+')');
  }
})().catch(error=>{console.error(error);process.exitCode=1;}).finally(()=>server.kill());
