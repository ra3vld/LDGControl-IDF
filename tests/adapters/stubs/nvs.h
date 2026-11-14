#pragma once
#include <cstdint>
#include <cstddef>
#include <map>
#include <string>
#include <vector>
#include <cstring>
using nvs_handle_t = int;using esp_err_t=int;
static constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NVS_NOT_FOUND=1,NVS_READONLY=0,NVS_READWRITE=1;
inline const char* esp_err_to_name(int){return "NVS failure";}
namespace fake {
inline std::map<int,std::map<std::string,std::vector<uint8_t>>> stored,pending;
inline bool failSet=false,failCommit=false,failRead=false,corruptRead=false,failErase=false;
inline unsigned writes=0,commits=0;
inline void reset(){stored.clear();pending.clear();failSet=failCommit=failRead=corruptRead=failErase=false;writes=commits=0;}
}
inline int nvs_open(const char* name,int mode,nvs_handle_t* h){*h=std::strcmp(name,"ldg-idf")?2:1;if(mode==NVS_READONLY&&!fake::stored.count(*h))return ESP_ERR_NVS_NOT_FOUND;return ESP_OK;}
inline void nvs_close(nvs_handle_t h){fake::pending.erase(h);}
inline int nvs_get_blob(nvs_handle_t h,const char* key,void* data,size_t* size){
 if(fake::failRead||!fake::stored[h].count(key))return ESP_FAIL;
 const auto& v=fake::stored[h][key];if(!data){*size=v.size();return ESP_OK;}if(*size<v.size())return ESP_FAIL;
 memcpy(data,v.data(),v.size());*size=v.size();if(fake::corruptRead&&*size)static_cast<uint8_t*>(data)[*size-1]^=1;return ESP_OK;
}
inline int nvs_set_blob(nvs_handle_t h,const char* key,const void* data,size_t size){++fake::writes;if(fake::failSet)return ESP_FAIL;if(!fake::pending.count(h))fake::pending[h]=fake::stored[h];const auto* p=static_cast<const uint8_t*>(data);fake::pending[h][key]={p,p+size};return ESP_OK;}
inline int nvs_commit(nvs_handle_t h){++fake::commits;if(fake::failCommit)return ESP_FAIL;if(fake::pending.count(h)){fake::stored[h]=fake::pending[h];fake::pending.erase(h);}return ESP_OK;}
inline int nvs_erase_all(nvs_handle_t h){if(fake::failErase)return ESP_FAIL;fake::pending[h].clear();return ESP_OK;}
inline int nvs_get_str(nvs_handle_t,const char*,char*,size_t*){return ESP_ERR_NVS_NOT_FOUND;}
inline int nvs_get_u8(nvs_handle_t,const char*,uint8_t*){return ESP_ERR_NVS_NOT_FOUND;}
inline int nvs_get_u16(nvs_handle_t,const char*,uint16_t*){return ESP_ERR_NVS_NOT_FOUND;}
