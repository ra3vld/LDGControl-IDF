file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
idf_component_register(SRCS "app.cpp" "settings.cpp" "http.cpp" "network.cpp" "display.cpp" "display_fonts.cpp" "update.cpp" "sensors.cpp" "recovery.cpp"
    INCLUDE_DIRS "." "${CMAKE_BINARY_DIR}/generated"
    EMBED_TXTFILES "../release-public.pem"
    REQUIRES ldg_core arduinojson esp_wifi esp_netif esp_event nvs_flash esp_timer esp_http_server esp_http_client mqtt esp_lcd driver esp_adc esp_https_ota app_update mbedtls esp_websocket_client lvgl)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LDG_BOARD_${LDG_BOARD}=1 LDG_RELEASE_REPOSITORY="ra3vld/LDGControl-IDF")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
execute_process(COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/tools/build_web.py" "${CMAKE_BINARY_DIR}/generated" COMMAND_ERROR_IS_FATAL ANY)
file(GLOB web_sources "${CMAKE_SOURCE_DIR}/web/src/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${web_sources} "${CMAKE_SOURCE_DIR}/tools/build_web.py" "${CMAKE_SOURCE_DIR}/web/package-lock.json")
