// Compile the production persistence adapter against transactional NVS fakes.
#include "../../main/settings.cpp"
#include <cassert>
#include <cstdio>
#include <algorithm>
int main(){
 using namespace app;using ldg::Result;
 fake::reset();settingsInit();bool restart=false;JsonDocument patch;
 patch["wifiSSID"]="Radio shack";patch["wifiPassword"]="initial-password";patch["mqttBroker"]="rotator.r3v.ru";patch["mqttPort"]=8883;patch["mqttUsername"]="user";patch["mqttPassword"]="secret";
 assert(saveConnectionSettings(patch.as<JsonVariantConst>(),restart)==Result::Success&&restart);
 auto saved=settings();assert(saved.mqttEnabled&&saved.device.mqttPort==8883&&saved.sectionRevision[0]==2&&saved.sectionRevision[1]==2);
 auto writes=fake::writes;restart=true;assert(saveConnectionSettings(patch.as<JsonVariantConst>(),restart)==Result::Success&&!restart&&fake::writes==writes);
 patch.clear();patch["mqttPassword"]="replacement";fake::failCommit=true;
 assert(saveConnectionSettings(patch.as<JsonVariantConst>(),restart)==Result::Failed);assert(!strcmp(settings().device.mqttPassword,"secret"));
 fake::failCommit=false;fake::failRead=true;
 assert(saveConnectionSettings(patch.as<JsonVariantConst>(),restart)==Result::Failed);assert(!strcmp(settings().device.mqttPassword,"secret"));fake::failRead=false;
 assert(saveConnectionSettings(patch.as<JsonVariantConst>(),restart)==Result::Success);settingsInit();assert(!strcmp(settings().device.mqttPassword,"replacement"));
 patch.clear();patch["password"]="";assert(saveSettings("mqtt",patch.as<JsonVariantConst>(),settings().sectionRevision[1],restart)==Result::Success);assert(!strcmp(settings().device.mqttPassword,"replacement"));
 patch["clear_password"]=true;fake::stored[2]["configV1"]={1,2,3};fake::failErase=true;
 assert(saveSettings("mqtt",patch.as<JsonVariantConst>(),settings().sectionRevision[1],restart)==Result::Failed);assert(settings().device.mqttPassword[0]==0&&fake::stored[2].count("configV1"));
 fake::failErase=false;assert(saveSettings("mqtt",patch.as<JsonVariantConst>(),settings().sectionRevision[1],restart)==Result::Success);assert(fake::stored[2].empty());
 auto erased=settings();assert(std::all_of(std::begin(erased.device.mqttPassword),std::end(erased.device.mqttPassword),[](char x){return x==0;}));
 patch.clear();patch["mqttUsername"]="readback-test";fake::corruptRead=true;
 assert(saveConnectionSettings(patch.as<JsonVariantConst>(),restart)==Result::Failed);assert(!strcmp(settings().device.mqttUsername,"user"));fake::corruptRead=false;
 assert(saveConnectionSettings(patch.as<JsonVariantConst>(),restart)==Result::Success);
 patch.clear();patch["wifiPassword"]=12;assert(saveConnectionSettings(patch.as<JsonVariantConst>(),restart)==Result::Invalid);
 patch.clear();patch["mqttBroker"]="evil/host";assert(saveConnectionSettings(patch.as<JsonVariantConst>(),restart)==Result::Invalid);
 patch.clear();patch["wifiPassword"]="short";assert(saveConnectionSettings(patch.as<JsonVariantConst>(),restart)==Result::Invalid);
 patch.clear();deserializeJson(patch,"{\"mqttUsername\":\"a\\u0000b\"}");assert(saveConnectionSettings(patch.as<JsonVariantConst>(),restart)==Result::Invalid);
 patch.clear();patch["ssid"]="new";assert(saveSettings("network",patch.as<JsonVariantConst>(),1,restart)==Result::TargetChanged);
 patch.clear();patch["clear_password"]=true;assert(saveSettings("network",patch.as<JsonVariantConst>(),settings().sectionRevision[0],restart)==Result::Success);settingsInit();auto emptyWifi=settings();assert(std::all_of(std::begin(emptyWifi.device.wifiPassword),std::end(emptyWifi.device.wifiPassword),[](char x){return x==0;}));
 fake::stored[2]["wifiPassword"]={1,2,3};fake::failSet=true;assert(resetSettings()==Result::Failed);assert(fake::stored[2].count("wifiPassword"));fake::failSet=false;
 assert(resetSettings()==Result::Success);settingsInit();assert(settings().device.wifiSSID[0]==0&&!settings().mqttEnabled&&fake::stored[2].empty());
 // Migration retains old records until an explicit clear/reset.
 fake::reset();auto old=defaultSettings().device;strcpy(old.wifiSSID,"legacy");strcpy(old.wifiPassword,"legacy-pass");ldg::StoredConfig legacy{3,old};const auto* data=reinterpret_cast<const uint8_t*>(&legacy);fake::stored[2]["configV1"]={data,data+sizeof(legacy)};
 settingsInit();assert(!strcmp(settings().device.wifiSSID,"legacy"));patch.clear();patch["brightness"]=73;assert(saveSettings("display",patch.as<JsonVariantConst>(),1,restart)==Result::Success);assert(fake::stored[2].count("configV1"));settingsInit();assert(settings().brightness.manual==73);
 puts("Production NVS adapter: atomic failure/readback, retry, masked secrets, explicit clear, reset and migration PASS");
}
