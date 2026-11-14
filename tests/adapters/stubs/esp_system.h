#pragma once
inline void esp_restart(){++fake::restarts;}
inline const char* esp_get_idf_version(){return "5.5.5";}
