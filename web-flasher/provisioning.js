import {tr,notify} from './i18n.js';
// Account linking adapted from G5500-ESP32S3-Controller. GPL-3.0.
import {SerialApi,pause} from './serial-api.js';
const $=id=>document.getElementById(id),bytes=s=>new TextEncoder().encode(s).length;
const b64=data=>btoa(String.fromCharCode(...data)).replaceAll('+','-').replaceAll('/','_').replace(/=+$/,'');
export class ServiceLink{
 constructor(origin){const url=new URL(origin);const local=['localhost','127.0.0.1'].includes(url.hostname)&&['localhost','127.0.0.1'].includes(location.hostname);if(url.origin!==origin||(url.protocol!=='https:'&&!(local&&url.protocol==='http:')))throw Error(tr("Invalid service URL."));this.origin=origin;}
 async call(path,body){let response,data;try{response=await fetch(this.origin+'/api/provisioning/public/'+path,{method:'POST',credentials:'omit',cache:'no-store',headers:{'Content-Type':'application/json'},body:JSON.stringify(body),signal:AbortSignal.timeout(15000)});data=await response.json()}catch{throw Error(tr("Cannot reach rotator.r3v.ru. Retry in this tab; your draft remains in memory."))}if(!response.ok){const e=Error(data.detail||tr("Account linking failed"));e.status=response.status;throw e}return data;}
 async start(device,name){const verifier=b64(crypto.getRandomValues(new Uint8Array(48))),challenge=b64(new Uint8Array(await crypto.subtle.digest('SHA-256',new TextEncoder().encode(verifier))));const data=await this.call('sessions',{challenge,device_id:device,name:name||'LDG',family:'tuner'});if(!/^[0-9a-f-]{36}$/.test(data.id)||data.approval_url!==this.origin+'/provisioning/'+data.id)throw Error(tr("Invalid account-link response."));this.session={id:data.id,device,verifier};this.approvalURL=data.approval_url;}
 poll(){return this.call(this.session.id+'/poll',{verifier:this.session.verifier})}
 ack(){return this.call(this.session.id+'/ack',{verifier:this.session.verifier})}
}
export class Preconfiguration{
 constructor(lock){
  this.lock=lock;this.service=new ServiceLink(document.querySelector('meta[name="ldg-service"]').content);
  $('settingsForm').addEventListener('submit',e=>{e.preventDefault();this.apply()});
  $('cancelSettings').onclick=()=>{this.cancelled=true;this.message(tr("Stopping setup. Settings already saved on the board are kept."))};
  $('checkConnection').onclick=()=>this.inspect();
  $('refreshService').onclick=()=>this.refresh();
  $('newLink').onclick=()=>{if(this.pending&&!confirm(tr("The password has not been confirmed on the device. A new link requires explicit password replacement. Continue?")))return;this.resetService();this.apply(this.port)};
  $('otherDevice').onclick=()=>{if(this.pending&&!confirm(tr("The password has not been confirmed on the device. Continue with another board?")))return;this.expected=null;this.port=null;this.resetService();this.invalidateConnection();this.message(tr("You can now select another board."));this.render()};
  $('localLink').onclick=e=>{if($('localLink').getAttribute('aria-disabled')==='true')e.preventDefault()};
  window.addEventListener('beforeunload',e=>{if(this.busy||this.pending)e.preventDefault()});
 }
 select(profile){
  if(this.profile?.id!==profile.id){
   if(this.pending){this.message(tr("Confirm or abandon the pending password with Select another board before changing profiles."),true);return false}
   this.invalidateConnection();this.draft=null;
  }
  this.profile=profile;return true;
 }
 resetService(){this.service=new ServiceLink(this.service.origin);this.pending=null;this.serviceConfig=null;this.serviceResult=null;for(const id of ['deviceLink','consent','refreshService'])$(id).hidden=true;$('serviceStatus').textContent=tr("Not selected.")}
 invalidateConnection(){this.localURL=null;$('postSetup').hidden=true;for(const id of ['deviceLink','consent','refreshService'])$(id).hidden=true;this.render()}
 render(locked=this.busy){
  const available=Boolean(this.localURL&&!locked),link=$('localLink');
  if(available)link.href=this.localURL;else link.removeAttribute('href');
  link.setAttribute('aria-disabled',String(!available));link.tabIndex=available?0:-1;
  link.title=tr(available?'Open the device interface in a new tab.':'Connect over USB and wait for a verified network address.');
  $('newLink').hidden=!this.expected||$('mqttMode').value!=='service';$('otherDevice').hidden=!this.expected;
 }
 message(text,error=false){$('settingsStatus').textContent=text;$('settingsStatus').classList.toggle('error',error);if(error){notify(text);if(text===tr("Cannot reach rotator.r3v.ru. Retry in this tab; your draft remains in memory."))$('serviceTroubleshooting').open=true}this.render()}
 collect(){if(!$('settingsForm').reportValidity())throw Error(tr("Check the settings fields."));const v=id=>$(id).value.trim(),draft={profile:this.profile.id,name:v('deviceName'),mode:v('mqttMode'),settings:{}};
  if(draft.name&&bytes(draft.name)>48)throw Error(tr("Name: at most 48 bytes."));
  if($('ssid').value){if(bytes($('ssid').value)>32)throw Error(tr("SSID: at most 32 bytes."));draft.settings.wifiSSID=$('ssid').value;if($('openWifi').checked)draft.settings.clearWifiPassword=true;else{if(!((bytes($('wifiPassword').value)>=8&&bytes($('wifiPassword').value)<=63)||/^[a-fA-F0-9]{64}$/.test($('wifiPassword').value)))throw Error(tr("Wi-Fi password: 8–63 bytes or 64 hexadecimal digits."));draft.settings.wifiPassword=$('wifiPassword').value}}
  if(v('ipMode')!=='preserve'){draft.settings.useStaticIP=v('ipMode')==='static';if(draft.settings.useStaticIP)for(const [id,key]of [['ip','staticIP'],['mask','staticNetmask'],['gateway','staticGateway'],['dns','staticDNS']]){const value=v(id);if(!/^(?:\d{1,3}\.){3}\d{1,3}$/.test(value)||value.split('.').some(x=>Number(x)>255))throw Error(tr("Check the address, subnet mask, gateway and DNS."));draft.settings[key]=value}}
  if(draft.mode==='manual'){const host=v('mqttHost'),port=Number(v('mqttPort'));if(!host||bytes(host)>63||/[\s/#+@]/.test(host))throw Error(tr("MQTT host: no scheme or path, at most 63 bytes."));if(!Number.isInteger(port)||port<1||port>65535)throw Error(tr("Invalid MQTT port."));if(bytes($('mqttUser').value)>64||bytes($('mqttPassword').value)>128)throw Error(tr("MQTT credentials are too long."));Object.assign(draft.settings,{mqttBroker:host,mqttPort:port,mqttUsername:$('mqttUser').value,mqttDiscoveryEnabled:$('discovery').checked});if(!$('keepMqttPassword').checked){if($('mqttPassword').value)draft.settings.mqttPassword=$('mqttPassword').value;else draft.settings.clearMqttPassword=true}}
  if(draft.mode==='off')draft.settings.mqttBroker='';return draft;
 }
 prepare(){if(!$('preconfigure').checked){this.draft=null;return true}try{this.draft=this.collect();return true}catch(e){this.message(e.message,true);return false}}
 observe(dialog){let handoff=false;dialog.addEventListener('ldg-flash-state',e=>{if(e.target!==dialog||!e.cancelable||e.detail?.state!=='finished'||!this.draft)return;e.preventDefault();handoff=true;this.port=dialog.port;const draft=this.draft;this.lock(true);dialog.addEventListener('closed',event=>event.stopImmediatePropagation(),{capture:true,once:true});setTimeout(()=>this.apply(this.port,draft),400)});dialog.addEventListener('closed',()=>{if(!handoff){this.draft=null;this.lock(false)}})}
 check(){if(this.cancelled)throw Error(tr("Setup stopped. Retry in this tab."))}
 async connect(port){const api=new SerialApi(port||await navigator.serial.requestPort());this.port=api.port;try{await api.open();if(api.identity.profile!==(this.activeDraft?.profile||this.profile.id))throw Error(tr("A different firmware profile is running. Select the matching board."));if(api.identity.role!=='tuner_controller'&&this.activeDraft?.mode==='service')throw Error(tr("Connect the tuner controller rather than its remote panel."));if(this.expected&&api.identity.device_id!==this.expected)throw Error(tr("Another board is connected. Required ID: ")+this.expected);this.expected=api.identity.device_id;return api}catch(e){await api.close();throw e}}
 async linked(api){
  if(!this.service.session)await this.service.start(this.expected,this.activeDraft.name);
  this.check();if(this.service.session.device!==this.expected)throw Error(tr("This session belongs to another board."));
  $('consent').href=this.service.approvalURL;$('consent').hidden=false;this.message(tr("Open the consent link and approve adding LDG to your shack."));
  while(true){
   this.check();await api.settings();let result;
   try{result=await this.service.poll()}catch(e){if(e.status===410)this.service.session=null;throw e}
   this.check();
   if(result.status==='denied'){this.service.session=null;throw Error(tr("Linking declined. You can choose your own broker."))}
   if(['issued','complete'].includes(result.status)){
    this.validateService(result);this.pending=result.status==='issued'?result:this.pending;return result;
   }
   if(!['pending','approved'].includes(result.status))throw Error(tr("Account linking expired. Start a new link or choose your own broker."));
   await pause(2000);
  }
 }
 validateService(result){
  if(result.family!=='tuner'||result.device_id!==this.expected)throw Error(tr("The service returned credentials for another device."));
  if(result.device_url!==this.service.origin+'/?tuner='+result.tuner_id||!/^[a-f0-9-]{36}$/.test(result.tuner_id))throw Error(tr("Invalid device-card URL."));
 }
 showService(result){
  this.validateService(result);this.serviceResult=result;
  $('deviceLink').href=result.device_url;$('deviceLink').hidden=false;$('refreshService').hidden=false;
  $('serviceStatus').textContent=tr(this.pending?'Card created. Apply settings to confirm delivery of the service password.':!result.approved?'Settings saved. MQTT is awaiting administrator approval.':result.blocked||!result.enabled?'Settings saved. MQTT access is disabled or blocked.':result.online?'The service confirms fresh LDG telemetry.':'The LDG card is ready. Waiting for fresh telemetry from the controller.');
 }
 async refresh(fromSetup=false){
  if(!this.service.session||this.busy&&!fromSetup)return;
  if(!fromSetup){this.cancelled=false;this.busy=true;this.lock(true)}
  try{const result=await this.service.poll();this.check();if(!['issued','complete'].includes(result.status))throw Error(tr("Account linking expired. Start a new link or choose your own broker."));this.showService(result)}
  catch(e){$('serviceStatus').textContent=tr("Service status unavailable. Retry Check service status; saved board settings are kept.");this.message(e.message,true)}
  finally{if(!fromSetup){this.busy=false;this.lock(false)}}
 }
 async network(api,wait){
  $('networkStatus').textContent=tr("Checking network connection…");
  const deadline=Date.now()+(wait?25000:0);let state;
  do{this.check();state=await api.settings();this.check();if(state.connected||Date.now()>=deadline)break;await pause(1000)}while(true);
  const valid=typeof state.ip==='string'&&/^(?:\d{1,3}\.){3}\d{1,3}$/.test(state.ip)&&state.ip.split('.').every(n=>Number(n)<=255&&String(Number(n))===n)&&!['0.0.0.0','255.255.255.255'].includes(state.ip)&&Number(state.ip.split('.')[0])>0&&Number(state.ip.split('.')[0])<224&&Number(state.ip.split('.')[0])!==127;
  this.localURL=state.connected&&valid?'http://'+state.ip+'/':null;
  $('networkStatus').textContent=tr(this.localURL?'Connected to Wi-Fi. Open Go to device on the same network.':'Wi-Fi is not confirmed. Correct step 4 and retry, or use the access point in step 5.');
  $('mqttStatus').textContent=tr(!state.settings.mqttBroker?'MQTT is not configured; other functions remain available.':state.mqtt_connected?'The device confirms an MQTT connection.':'MQTT is not connected yet. Check service approval or the TLS, clock and credentials in device Settings.');
  return state;
 }
 async inspect(){
  if(this.busy)return;this.busy=true;this.cancelled=false;this.activeDraft=null;this.invalidateConnection();this.lock(true);$('cancelSettings').hidden=false;$('postSetup').hidden=false;$('serviceStatus').textContent=tr("Not selected.");this.message(tr("Reading connection status over USB. Settings will be kept."));let api;
  try{api=await this.connect();this.check();await this.network(api,false);this.message(tr("Connection status read over USB. No settings were changed."));if(this.service.session)await this.refresh(true)}
  catch(e){this.message(e.name==='NotFoundError'?tr("No USB port selected. Try connecting again."):e.message,true)}
  finally{await api?.close();this.busy=false;this.lock(false);$('cancelSettings').hidden=true}
 }
 verify(saved,desired){for(const [key,value]of Object.entries(desired))if(!['wifiPassword','mqttPassword','clearWifiPassword','clearMqttPassword'].includes(key)&&saved[key]!==value)throw Error(tr("Settings readback did not confirm ")+key);if(desired.wifiPassword&&!saved.hasWifiPassword||desired.mqttPassword&&!saved.hasMqttPassword||desired.clearWifiPassword&&saved.hasWifiPassword||desired.clearMqttPassword&&saved.hasMqttPassword)throw Error(tr("The device did not confirm password storage."))}
 async apply(port=null,frozen=null){
  if(this.busy)return;let draft;try{draft=frozen||this.collect()}catch(e){this.message(e.message,true);return}
  this.busy=true;this.cancelled=false;this.invalidateConnection();this.activeDraft=draft;this.lock(true);$('cancelSettings').hidden=false;this.message(tr("Connecting to the application over USB…"));let api;
  try{
   api=await this.connect(port);this.check();let result;const desired={...draft.settings};
   if(draft.mode==='service'){
    result=await this.linked(api);this.check();
    if(result.status==='issued'){
     const m=result.mqtt;
     if(!m||m.host?.toLowerCase()!==new URL(this.service.origin).hostname||m.tls!==true||m.port!==8883||typeof m.username!=='string'||!m.username||bytes(m.username)>64||m.password!==null&&(typeof m.password!=='string'||!m.password||bytes(m.password)>128))throw Error(tr("The service must use verified TLS on port 8883."));
     this.serviceConfig={mqttBroker:m.host,mqttPort:m.port,mqttUsername:m.username};
     const existing=(await api.settings()).settings;
     if(m.password===null&&(!existing.hasMqttPassword||Object.entries(this.serviceConfig).some(([k,v])=>existing[k]!==v)))throw Error(tr("The board has no matching credentials. Start a new link and select password replacement."));
     Object.assign(desired,this.serviceConfig,{mqttDiscoveryEnabled:false});if(m.password!==null)desired.mqttPassword=m.password;
    }else{
     const existing=(await api.settings()).settings;
     if(!this.serviceConfig||!existing.hasMqttPassword||Object.entries(this.serviceConfig).some(([k,v])=>existing[k]!==v))throw Error(tr("The board has no matching credentials. Start a new link and select password replacement."));
    }
   }
   let restart=false;
   if(Object.keys(desired).length){this.check();this.message(tr("Saving settings and checking USB readback…"));const response=await api.apply(desired);if(!response.saved)throw Error(tr("Storage was not confirmed."));restart=response.restart;this.verify((await api.settings()).settings,desired)}
   if(restart){this.check();this.message(tr("Settings saved. Restarting and verifying the same board…"));await api.exchange('reboot');await api.close();api=null;await pause(2000);this.check();api=await this.connect(this.port);this.verify((await api.settings()).settings,desired)}
   this.check();
   if(result){
    if(result.status==='issued')await this.service.ack();
    this.pending=null;if(result.mqtt)result.mqtt.password=null;$('consent').hidden=true;
   }
   $('postSetup').hidden=false;$('serviceStatus').textContent=tr("Not selected.");
   this.message(tr("Settings saved and verified over USB."));
   await this.network(api,Boolean(desired.wifiSSID||'useStaticIP' in desired));
   if(result){this.showService(result);await this.refresh(true)}
  }catch(e){this.message(e.name==='NotFoundError'?tr("No USB port selected. Try connecting again."):e.message,true)}
  finally{await api?.close();this.busy=false;this.draft=null;this.lock(false);$('cancelSettings').hidden=true}
 }
}
