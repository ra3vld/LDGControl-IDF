#include "mqtt_health.hpp"
#include "mqtt_state.h"
#include "usb_provisioning.h"
#include <unity.h>
#include <string>
using namespace ldg;
void setUp(){}void tearDown(){}
static unsigned applies;
static void adapter(const char* op,JsonVariantConst,JsonDocument& out){if(!strcmp(op,"apply"))++applies;out["saved"]=true;}
void usb_retries_own_exact_body_and_session(){
 UsbProvisioning usb;usb.begin("1234567890abcdef",adapter,40);char response[1538];
 TEST_ASSERT_TRUE(usb.handle(R"({"ldg":1,"cid":1,"op":"hello","client":"11111111111111111111111111111111"})",response,sizeof(response)));
 JsonDocument hello;deserializeJson(hello,response);TEST_ASSERT_EQUAL(41,hello["sid"].as<unsigned>());
 const char* body=R"({"ldg":1,"cid":2,"op":"apply","boot":"1234567890abcdef","sid":41,"settings":{"mqttPassword":"secret"}})";
 applies=0;TEST_ASSERT_TRUE(usb.handle(body,response,sizeof(response)));const std::string saved=response;
 TEST_ASSERT_TRUE(usb.handle(body,response,sizeof(response)));TEST_ASSERT_EQUAL_STRING(saved.c_str(),response);TEST_ASSERT_EQUAL(1,applies);
 TEST_ASSERT_TRUE(usb.handle(R"({"ldg":1,"cid":2,"op":"apply","boot":"1234567890abcdef","sid":41,"settings":{}})",response,sizeof(response)));JsonDocument error;deserializeJson(error,response);TEST_ASSERT_EQUAL(409,error["status"].as<int>());
 TEST_ASSERT_TRUE(usb.handle(body,response,sizeof(response)));TEST_ASSERT_EQUAL_STRING(saved.c_str(),response);TEST_ASSERT_EQUAL(1,applies);
 TEST_ASSERT_TRUE(usb.handle(R"({"ldg":1,"cid":3,"op":"apply","boot":"1234567890abcdef","sid":40})",response,sizeof(response)));deserializeJson(error,response);TEST_ASSERT_EQUAL(409,error["status"].as<int>());TEST_ASSERT_EQUAL(1,applies);
 TEST_ASSERT_FALSE(usb.handle("not JSON",response,sizeof(response)));
 std::string huge(UsbProvisioning::Limit+1,'a');TEST_ASSERT_FALSE(usb.handle(huge.c_str(),response,sizeof(response)));
 TEST_ASSERT_FALSE(usb.handle(R"({"ldg":1,"cid":4,"op":"apply","x":{"x":{"x":{"x":{}}}}})",response,sizeof(response)));TEST_ASSERT_EQUAL(1,applies);
}
void mqtt_guards_cover_managed_host_and_fragmentation(){
 TEST_ASSERT_TRUE(managedMqttBroker("ROTATOR.R3V.RU"));
 for(const char* host:{"mqtt.rotator.r3v.ru","rotator.r3v.ru.attacker","rotator.r3v.ru.",""})TEST_ASSERT_FALSE(managedMqttBroker(host));
 const auto gate=mqttMemoryGate(true,true);TEST_ASSERT_EQUAL(52*1024,gate.free);TEST_ASSERT_TRUE(gate.allowed(gate.free,gate.largest));TEST_ASSERT_FALSE(gate.allowed(gate.free-1,gate.largest));TEST_ASSERT_FALSE(gate.allowed(gate.free,gate.largest-1));
 TEST_ASSERT_EQUAL(64*1024,mqttMemoryGate(true,false).free);TEST_ASSERT_EQUAL(16*1024,mqttMemoryGate(false,false).free);
 MqttHealth health;health.attemptAt=0xfffffff0;health.failed("certificate",0xfffffff8);health.connected(0x10);TEST_ASSERT_EQUAL(32,health.connectMs);TEST_ASSERT_FALSE(health.errorActive);TEST_ASSERT_EQUAL_STRING("certificate",health.error);TEST_ASSERT_EQUAL_STRING("connected",health.state);
}
void stable_and_zero_meter_get_one_second_heartbeat(){
 MqttStateSchedule schedule;tuner_state_t state{};state.meterValid=true;state.meter.forward_power_raw=100;
 TEST_ASSERT_TRUE(schedule.due(100,state));schedule.published(100,state);
 TEST_ASSERT_FALSE(schedule.due(1099,state));TEST_ASSERT_TRUE(schedule.due(1100,state));schedule.published(1100,state);
 state.meter.forward_power_raw=0;TEST_ASSERT_TRUE(schedule.due(2100,state));schedule.published(2100,state);TEST_ASSERT_TRUE(schedule.due(3100,state));schedule.published(3100,state);
 state.meterValid=false;TEST_ASSERT_TRUE(schedule.due(4100,state));schedule.published(4100,state);TEST_ASSERT_FALSE(schedule.due(5100,state));TEST_ASSERT_TRUE(schedule.due(34100,state));
}
int main(){UNITY_BEGIN();RUN_TEST(usb_retries_own_exact_body_and_session);RUN_TEST(mqtt_guards_cover_managed_host_and_fragmentation);RUN_TEST(stable_and_zero_meter_get_one_second_heartbeat);return UNITY_END();}
