#pragma once
#include "nvs.h"
#include <string>
#include <algorithm>
#include <cassert>
struct uart_config_t{int baud_rate,data_bits,parity,stop_bits,source_clk;};
static constexpr int UART_NUM_0=0,UART_DATA_8_BITS=8,UART_PARITY_DISABLE=0,UART_STOP_BITS_1=1,UART_SCLK_DEFAULT=0;
namespace fake{inline std::string input;inline size_t chunk=128;inline unsigned restarts=0;}
inline int uart_param_config(int port,const uart_config_t* c){assert(port==0&&c->baud_rate==115200);return ESP_OK;}
inline bool uart_is_driver_installed(int){return false;}
inline int uart_driver_install(int,int,int,int,void*,int){return ESP_OK;}
inline int uart_read_bytes(int port,void* out,size_t size,int){assert(port==0);const auto n=std::min({size,fake::input.size(),fake::chunk});memcpy(out,fake::input.data(),n);fake::input.erase(0,n);return int(n);}
