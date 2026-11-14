// Exercise the actual SDK adapter, including its persistent duplicate removal.
#include "../../main/wifi_storage.cpp"
#include <cassert>
#include <cstring>
#include <cstdio>
int main(){
 using namespace fakeWifi;
 wifi_config_t exact{};const char ssid[]="12345678901234567890123456789012";
 const char psk[]="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
 app::fillWifiStation(exact,ssid,psk);assert(!memcmp(exact.sta.ssid,ssid,32)&&!memcmp(exact.sta.password,psk,64));
 app::fillWifiStation(exact,"open","");assert(!memcmp(exact.sta.ssid,"open",5)&&exact.sta.password[0]==0);
 reset();strcpy(reinterpret_cast<char*>(stored.sta.ssid),"old network");strcpy(reinterpret_cast<char*>(stored.sta.password),"old secret");runtime=stored;
 auto result=app::prepareWifiStorage();assert(result.ram&&result.error==0&&storage==WIFI_STORAGE_RAM&&writes==1&&!stored.sta.password[0]&&!stored.sta.ssid[0]);assert((calls==std::vector<int>{1,2,3,4}));
 const auto writesBefore=writes;result=app::prepareWifiStorage();assert(result.ram&&writes==writesBefore);
 wifi_config_t active{};strcpy(reinterpret_cast<char*>(active.sta.password),"new application secret");assert(esp_wifi_set_config(WIFI_IF_STA,&active)==0);assert(!stored.sta.password[0]&&runtime.sta.password[0]&&writes==writesBefore);
 reset();stored.sta.password[20]='s';runtime=stored;result=app::prepareWifiStorage();assert(result.ram&&writes==1&&stored.sta.password[20]==0);
 reset();getError=11;result=app::prepareWifiStorage();assert(result.ram&&result.error==11&&storage==WIFI_STORAGE_RAM);
 reset();runtime.sta.password[0]='s';stored=runtime;clearError=12;result=app::prepareWifiStorage();assert(result.ram&&result.error==12&&stored.sta.password[0]);
 reset();ramError=13;result=app::prepareWifiStorage();assert(!result.ram&&result.error==13); // Caller must not apply private credentials.
 puts("Production Wi-Fi storage adapter: legacy SDK secret cleared, repeat boot avoids writes, runtime credentials stay in RAM, SDK failures reported PASS");
}
