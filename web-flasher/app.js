import {tr} from './i18n.js';
import {Preconfiguration} from './provisioning.js';
const $=id=>document.getElementById(id);
let photos,catalog,selected,configBusy=false,dialogBusy=false,ready=false,button;
const preconfig=new Preconfiguration(value=>{configBusy=value;update()});
const labels={usb_provisioning:'USB setup',wifi:'Wi-Fi',web:'Web interface',mqtt:'MQTT',mqtt_tls:'MQTT TLS',home_assistant_discovery:'Home Assistant Discovery',tci:'TCI',display:'Display'};
function update(){
 const locked=configBusy||dialogBusy;
 for(const id of ['profile','preconfigure','confirmProfile'])$(id).disabled=locked;
 $('settingsFields').disabled=locked;
 for(const id of ['applySettings','checkConnection'])$(id).disabled=locked||!selected||!navigator.serial||!isSecureContext;
 for(const id of ['newLink','otherDevice','refreshService'])$(id).disabled=locked;
 if(button)button.disabled=locked||!ready||!selected?.build||!$('confirmProfile').checked||!navigator.serial||!isSecureContext;
 $('installHint').textContent=!navigator.serial||!isSecureContext?tr('Open this HTTPS page in desktop Chrome or Edge.'):!selected?.build?tr('No binary has been published for this profile yet.'):locked?tr('Finish installing or configuring first.'):!$('confirmProfile').checked?tr('Confirm the board and display model.'):tr('Select the USB port in the installation dialog.');
 preconfig.render(locked);
}
function fields(){
 $('mqttFields').hidden=$('mqttMode').value!=='manual';
 for(const id of ['serviceHelp','serviceTroubleshooting'])$(id).hidden=$('mqttMode').value!=='service';
 $('staticFields').hidden=$('ipMode').value!=='static';
 $('wifiPassword').disabled=$('openWifi').checked;
 $('mqttPassword').disabled=$('keepMqttPassword').checked;
 preconfig.render(configBusy||dialogBusy);
}
function describe(){
 $('profileRole').textContent=tr(selected.role);$('selectedName').textContent=tr(selected.title);
 $('buildInfo').textContent=selected.build?selected.build.version+' · '+selected.build.commit.slice(0,12)+' · '+(selected.build.app_bytes/1024).toFixed(1)+' / '+(selected.build.app_limit_bytes/1024).toFixed(0)+tr(' KiB application partition · ')+(selected.build.static_ram_bytes/1024).toFixed(1)+tr(' KiB static RAM'):tr('No prepared build has been published yet.');
 const caps=selected.build?.capabilities||{};
 $('features').replaceChildren(...Object.entries(labels).filter(([key])=>caps[key]).map(([,label])=>{const li=document.createElement('li');li.textContent=tr(label);return li}));
 const russian=document.documentElement.lang==='ru';
 for(const id of ['wiring','boardConnections','postInstallWiring'])$(id).href=selected.documentation.replace(/\.md(?=#|$)/,russian?'.ru.md':'.md');
 $('setupGuide').href=selected.documentation.replace(/docs\/hardware\.md(?:#.*)?$/,'docs/usb-setup'+(russian?'.ru':'')+'.md');
}
window.addEventListener('ldg-language',()=>{if(selected){describe();if(button)button.textContent=tr('Connect and install');update()}});
function select(id){
 if(configBusy||dialogBusy){$('profile').value=selected.id;return}
 const profile=catalog.profiles.find(p=>p.id===id);
 if(!preconfig.select(profile)){$('profile').value=selected.id;return}
 selected=profile;describe();const photo=photos?.[selected.photo];$('boardPhoto').hidden=!photo;
 if(photo){$('boardImage').src='./images/'+photo.file;$('boardImage').alt=tr(photo.alt);$('photoSource').href=photo.source;$('photoSource').textContent=tr(photo.caption)}
 $('download').hidden=!selected.build;if(selected.build)$('download').href='./firmware/'+selected.id+'/usb-firmware.zip';$('postInstall').hidden=true;
 const installer=document.createElement('esp-web-install-button');
 if(selected.build)installer.setAttribute('manifest',new URL('./firmware/'+selected.id+'/manifest.json',location.href).href);
 button=document.createElement('button');button.type='button';button.slot='activate';button.textContent=tr('Connect and install');
 installer.addEventListener('click',e=>{if(button.disabled||!preconfig.prepare()){e.preventDefault();e.stopImmediatePropagation()}else{$('postInstall').hidden=true;preconfig.invalidateConnection()}},true);
 installer.append(button);$('installer').replaceChildren(installer);history.replaceState(null,'','#profile='+id);fields();update();
}
for(const id of ['preconfigure','mqttMode','ipMode','openWifi','keepMqttPassword'])$(id).addEventListener('change',fields);
$('confirmProfile').addEventListener('change',update);$('profile').addEventListener('change',()=>select($('profile').value));
const seen=new WeakSet();
new MutationObserver(()=>{
 const dialog=document.querySelector('ewt-install-dialog');dialogBusy=Boolean(dialog);update();
 if(!dialog||seen.has(dialog))return;seen.add(dialog);const profile=selected;preconfig.observe(dialog);
 dialog.addEventListener('ldg-flash-state',e=>{if(e.target!==dialog)return;const done=e.detail?.state==='finished';$('postInstall').hidden=!done;
  if(done){$('postInstallProfile').textContent=tr(profile.title);$('postInstallNext').textContent=tr(preconfig.draft?'Continue in step 4: USB setup will start automatically.':'Continue in step 4 for optional setup, or open the wiring in step 5.');$('presetPanel').scrollIntoView({block:'start',behavior:'smooth'})}
 });
}).observe(document.body,{childList:true});
try{
 const response=await fetch('./catalog.json',{cache:'no-store'});if(!response.ok)throw Error(tr('Firmware catalog unavailable. Reload the page.'));
 catalog=await response.json();photos=await (await fetch('./photos.json')).json();if(catalog.schema!==1||!catalog.profiles?.length)throw Error(tr('Invalid firmware catalog.'));
 for(const profile of catalog.profiles){const option=document.createElement('option');option.value=profile.id;option.textContent=tr(profile.title);$('profile').append(option)}
 const hash=new URLSearchParams(location.hash.slice(1)).get('profile');select(catalog.profiles.some(p=>p.id===hash)?hash:catalog.profiles[0].id);
 await import('./vendor/install-button.js');await customElements.whenDefined('esp-web-install-button');ready=true;update();
}catch(error){$('catalogError').textContent=error.message;update()}
