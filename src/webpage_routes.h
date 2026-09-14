#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <FS.h>
#include <SD.h>
#include <esp_mac.h>

void say_hello(void);
void mcu_dir(fs::FS &fs, const char * dir_name, uint8_t levels);
void webpage_serve_html(AsyncWebServerRequest *request, fs::LittleFSFS &local_filesystem);
void webpage_led(AsyncWebServer &server, const int led_pin);
void webpage_file_list(AsyncWebServerRequest *request, fs::FS &fs_instance);
String add_commas_to_string(long value);
void handle_sd_files(AsyncWebServerRequest *request);
bool init_sd(int chip_select_pin);
String get_unique_id();
void init_webpage_routes(AsyncWebServer &server, fs::FS &sd_instance, fs::FS &fs_instance);
String format_with_commas(long value);
