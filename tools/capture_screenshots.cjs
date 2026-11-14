// Capture the built web interface using a local preview and example telemetry.
// Run tools/preview.py first. PLAYWRIGHT_MODULE can select an installed Playwright.
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const fs=require('node:fs/promises');
const path=require('node:path');
const base=process.env.LDG_PREVIEW_URL||'http://127.0.0.1:8776';
const output=path.join(__dirname,'../docs/screenshots');
(async()=>{
  await fs.mkdir(output,{recursive:true});
  const browser=await chromium.launch({executablePath:process.env.CHROMIUM_PATH||'/usr/bin/chromium',headless:true,args:['--no-sandbox']});
  try{
    for(const language of ['en','ru']){
      const context=await browser.newContext({viewport:{width:1280,height:1100},locale:language==='en'?'en-US':'ru-RU',deviceScaleFactor:1});
      const page=await context.newPage();const errors=[];page.on('pageerror',e=>errors.push(e.message));
      // Documentation examples never send hardware commands or settings writes.
      await page.route('**/api/v2/**', async route=>{
        const request=route.request(),endpoint=new URL(request.url()).pathname;
        if(request.method()==='POST'){
          if(endpoint==='/api/v2/commands')return route.fulfill({status:202,contentType:'application/json',body:JSON.stringify({...request.postDataJSON(),state:'done',result:'success'})});
          return route.fulfill({status:400,contentType:'application/json',body:'{"error":"invalid_request"}'});
        }
        if(endpoint.startsWith('/api/v2/commands/'))return route.fulfill({contentType:'application/json',body:'{"state":"done","result":"success"}'});
        return route.continue();
      });
      await page.route('**/api/v2/ws-token',route=>route.fulfill({contentType:'application/json',body:'{"token":"documentation"}'}));
      await page.addInitScript(()=>{
        window.WebSocket=class {
          constructor(){this.readyState=1;window.__ldgDemoSocket=this;}
          close(){this.readyState=3;}
        };
      });
      await page.goto(base);await page.locator('#chart').waitFor({state:'visible'});
      if(language==='ru')await page.locator('#language').click();
      await page.evaluate(()=>{
        const socket=window.__ldgDemoSocket;
        const realNow=Date.now;let timestamp=realNow()-1800000;let latest;let sequence=10000000;
        try{
          Date.now=()=>timestamp;
          for(let i=0;i<=7200;i++){
            const tx=i%240<168,band=i<2400?'40m':i<4800?'20m':'17m';
            const fwd=tx?45+18*Math.sin(i/33)+8*Math.sin(i/7):0;
            const swr=tx?1.2+0.8*(1+Math.sin(i/95))/2:null;
            latest={api_version:2,boot_id:'1'.repeat(32),source_token:'2'.repeat(32),sequence:sequence++,uptime_ms:10000000+i*250,connected:true,busy:false,source:'local',fwd_power:fwd,ref_power:tx?fwd*Math.pow((swr-1)/(swr+1),2):0,swr,band,mode:i<800?1:i<1100?2:4,antenna:i<4000?1:2,meter_age_ms:0,scale_watts:100,logarithmic:false,radio:{state:'ready',tx_frequency_hz:band==='40m'?7074000:band==='20m'?14074000:18100000,band,rx_tx:tx?'tx':'rx'},external_antenna:{requested:3,confirmed:3,result:'success',evidence:2},route:{stage:'complete',partial:false,result:'success'}};
            socket.onmessage({data:JSON.stringify(latest)});timestamp+=250;
          }
        }finally{Date.now=realNow;}
        latest={...latest,fwd_power:42.5,ref_power:.8,swr:1.32,band:'20m',antenna:1,radio:{...latest.radio,tx_frequency_hz:14074000,band:'20m'}};
        window.setInterval(()=>socket.onmessage({data:JSON.stringify({...latest,sequence:sequence++,uptime_ms:latest.uptime_ms+=250})}),250);
      });
      await page.locator('[data-command="memory_tune"]').click();
      await page.waitForFunction(expected=>document.querySelector('#command-result').textContent===expected,language==='en'?'Completed':'Выполнено');
      await page.mouse.move(0,0);
      await page.waitForTimeout(600);
      await page.evaluate(()=>scrollTo(0,0));
      const work=await page.locator('#work').boundingBox();
      await page.screenshot({path:path.join(output,`web-control-${language}.png`),clip:{x:0,y:0,width:1280,height:Math.ceil(work.y+work.height+16)}});
      const display=page.locator('#settings-sections>details').nth(6);
      await display.locator('summary').click();await display.locator('form').waitFor({state:'visible'});
      if(await display.locator('input[name=view]').count())throw Error('Removed display view selector is still exposed');
      await display.screenshot({path:path.join(output,`web-display-${language}.png`)});
      await page.setViewportSize({width:390,height:844});await page.evaluate(()=>scrollTo(0,0));
      const commandResult=await page.locator('#command-result').boundingBox();
      await page.screenshot({path:path.join(output,`web-mobile-${language}.png`),clip:{x:0,y:0,width:390,height:Math.ceil(commandResult.y+commandResult.height+16)}});
      const device=page.locator('#settings-sections>details').last();
      await device.locator('summary').click();await device.locator('form').waitFor({state:'visible'});
      await device.locator('input[name=username]').fill('Field station');
      await device.locator('button[type=submit]').click();await page.locator('#toast:not([hidden])').waitFor();
      // Pause the notification timer while capturing the error, without hovering a button.
      await page.locator('#toast span').hover();
      await device.evaluate(element=>scrollTo(0,scrollY+element.getBoundingClientRect().top-16));
      await page.waitForFunction(()=>{const r=document.querySelector('#toast').getBoundingClientRect();return !document.querySelector('#toast').hidden&&r.top>=0&&r.bottom<=innerHeight;});
      await page.screenshot({path:path.join(output,`web-error-${language}.png`)});
      if(errors.length)throw Error(errors.join('\n'));
      await context.close();console.log('Captured actual web UI:',language);
    }
  }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
