#include "../../main/settings.cpp"
#include "../../main/provisioning.cpp"
#include <cassert>
#include <unistd.h>
namespace app {
static uint32_t fakeNow=100;
uint32_t nowMs(){return fakeNow;}
std::atomic<bool> updating{false},networkUp{false},mqttConnected{false};
SemaphoreHandle_t controlMutex=reinterpret_cast<void*>(1);
ldg::Operations ops;
ldg::Id bootId{};
ldg::Controller owner({},bootId,ops);
ldg::Controller* controller=&owner;
static Snapshot view;
Snapshot snapshot(){return view;}
}
int main(){
 using namespace app;settingsInit();startUsb();
 FILE* serial=tmpfile();assert(serial);const int original=dup(fileno(stdout));assert(original>=0);fflush(stdout);dup2(fileno(serial),fileno(stdout));long consumed=0;
 auto transact=[&](const std::string& request,size_t chunk=128){
  fake::chunk=chunk;fake::input=request+"\r\n";
  while(!fake::input.empty()){serviceUsb();fakeNow+=1;}
  fflush(stdout);const long end=lseek(fileno(stdout),0,SEEK_CUR);assert(end>=consumed);std::string out(size_t(end-consumed),'\0');assert(pread(fileno(serial),out.data(),out.size(),consumed)==long(out.size()));consumed=end;
  JsonDocument reply;if(!out.empty())assert(!deserializeJson(reply,out));return reply;
 };
 const std::string hello=R"({"ldg":1,"cid":1,"op":"hello","client":"11111111111111111111111111111111"})";
 auto reply=transact(hello,1);assert(reply["status"]==200&&reply["profile"]==LDG_BOARD_NAME&&reply["device_id"]=="aabbccddeeff"&&usbMaintenance());const auto sid=reply["sid"].as<unsigned>();const std::string boot=reply["boot"].as<const char*>();
 auto body=[&](unsigned cid,const char* op,const char* extra=""){return std::string("{\"ldg\":1,\"cid\":")+std::to_string(cid)+",\"op\":\""+op+"\",\"boot\":\""+boot+"\",\"sid\":"+std::to_string(sid)+extra+"}";};
 auto save=body(2,"apply",R"(,"settings":{"mqttBroker":"rotator.r3v.ru","mqttPort":8883,"mqttUsername":"device","mqttPassword":"test-secret"})");
 reply=transact(save,7);assert(reply["saved"]==true);const auto writes=fake::writes;reply=transact(save,128);assert(reply["saved"]==true&&fake::writes==writes);
 reply=transact(body(3,"settings"),3);assert(reply["settings"]["hasMqttPassword"]==true&&reply["settings"]["mqttPassword"].isUnbound());
 view.busy=true;reply=transact(body(4,"apply",R"(,"settings":{"clearMqttPassword":true})"),17);assert(reply["status"]==409&&settings().device.mqttPassword[0]);view.busy=false;
 view.tuner.meterAgeMs=0;view.tuner.meter.forward_power_raw=1;reply=transact(body(5,"reboot"));assert(reply["status"]==409&&fake::restarts==0);view.tuner.meter.forward_power_raw=0;
 view.tuner.radio.ageMs=0;view.tuner.radio.transmitting=true;reply=transact(body(6,"reboot"));assert(reply["status"]==409);view.tuner.radio.transmitting=false;
 reply=transact(body(7,"apply",R"(,"settings":{"clearMqttPassword":true})"),31);assert(reply["saved"]==true&&!settings().device.mqttPassword[0]);
 auto overflow=std::string(1600,'a');overflow[0]='{';reply=transact(overflow,17);assert(reply.isNull());reply=transact(body(8,"settings"));assert(reply["status"]==200);
 // The next physical USB session gets a new generation, rejecting stale retries.
 fakeNow+=6000;serviceUsb();assert(!usbMaintenance());reply=transact(hello);assert(reply["sid"].as<unsigned>()!=sid);reply=transact(body(9,"settings"));assert(reply["status"]==409);
 fflush(stdout);dup2(original,fileno(stdout));close(original);fclose(serial);
 puts("Production UART0 adapter: fragmented lines, logs/overflow recovery, exact replay, secret masking, busy/RF guards and lease expiration PASS");
}
