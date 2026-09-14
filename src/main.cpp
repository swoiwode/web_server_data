#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>

#include "webpage_routes.h"

const char* ssid = "eero_JELP";
const char* password = "slimjim314";

AsyncWebServer server(80);

// 1. Declare the Event Source stream endpoint globally
AsyncEventSource events("/events");
unsigned long last_time = 0;
int global_counter = 0;
char output_buffer[256] = "Hello, World!"; // Buffer for incoming data
// Global variables for thread communication
volatile bool new_value_available = false;
String shared_input_message = "";
int colon_pos = 0;
String input_cmd = "";
String input_data = "";
const char* local_ntp = "10.0.0.1";   // Local gateway/NTP server IP
struct tm timeinfo;
char timestamp[64];

const int led_pin = LED_BUILTIN; // Onboard LED pin

// Explicitly define the standard DevKit I2C pins
#define I2C_SDA 23
#define I2C_SCL 22
#define SD_CS 18
#define SD_SCK 19
#define SD_MOSI 20
#define SD_MISO 21

Adafruit_BME280 bme;

void setup() {
  Serial.begin(115200);
  while(!Serial) {
    delay(10);
  }
  
  // Needs some delay to enable initial Serial.printf, 1000 is not enough
  delay(2000);

  pinMode(led_pin, OUTPUT);
  digitalWrite(led_pin, LOW);
  Wire.begin(I2C_SDA, I2C_SCL);
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  // Address 0x76 is standard for generic modules; Adafruit modules use 0x77
  if (!bme.begin(0x77, &Wire)) {
    Serial.println(F("Could not find a valid BME280 sensor, check your wiring or I2C address!"));
    while (1) delay(10);
  }
  Serial.println(F("BME280 Sensor successfully initialized!"));

  init_sd(SD_CS);
  Serial.printf(" *** SD Card Available Space: %.2f GB\n", 
    ((double)(SD.totalBytes() - SD.usedBytes()) / 1e9)
  );

  if (!LittleFS.begin()) {
    Serial.printf("An error occurred while mounting LittleFS\n");
    return;
  }
  Serial.printf("LittleFS mounted successfully.\n");

  mcu_dir(LittleFS, "/", 3);

  // Connect to Wi-Fi
  Serial.printf("Connecting to %s ", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.printf(".");
  }
  Serial.printf("\nWi-Fi connected, ip: %s\n", WiFi.localIP().toString().c_str());

  // Start mDNS Responder (Must be done AFTER Wi-Fi is connected)
  if (!MDNS.begin(ESP32_HOSTNAME)) {
    Serial.printf("Error setting up mDNS!\n");
    while(1) { delay(1000); }
  }
  Serial.printf("mDNS started, hostname: http://%s.local\n", ESP32_HOSTNAME);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
      webpage_serve_html(request, LittleFS);
  });

  webpage_led(server, led_pin);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send(LittleFS, "/index.html", "text/html");
  });

  server.on("/sd_files", HTTP_GET, handle_sd_files);

  server.on("/files", HTTP_GET, [](AsyncWebServerRequest *request) {
    webpage_file_list(request, LittleFS);
  });

  server.on("/view", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("file")) {
      String file_path = request->getParam("file")->value();
      
      if (SD.exists(file_path)) {
        // 1. Prepare the response stream directly from the SD card.
        // Leaving the 3rd parameter empty lets the server auto-detect text/images/code.
        AsyncWebServerResponse *response = request->beginResponse(SD, file_path, String());
        
        // 2. Extract just the raw file name (removing any leading slash)
        String clean_name = file_path;
        if (clean_name.startsWith("/")) {
          clean_name = clean_name.substring(1);
        }
        
        // 3. THE FIX: Force the browser to read the real filename on right-click save
        // "inline" means it still displays perfectly inside the new browser window
        response->addHeader("Content-Disposition", "inline; filename=\"" + clean_name + "\"");
        
        // 4. Send the configured response payload out
        request->send(response);
        return;
      } else {
        request->send(404, "text/plain", "File Not Found on SD Card");
        return;
      }
    }
    request->send(400, "text/plain", "Bad Request: Missing 'file' parameter");
  });

  init_webpage_routes(server, SD, LittleFS);

  server.serveStatic("/", LittleFS, "/");

  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
    String response_message = "No content data received";
    
    if (request->hasParam("value")) {
      // 1. Save data to global variable
      shared_input_message = request->getParam("value")->value(); 
      // 2. Set flag to true
      new_value_available = true;                                  
      
      response_message = "ESP32 received: " + shared_input_message;
    }
    request->send(200, "text/plain", response_message);
  });

  server.addHandler(&events);
  
  server.begin();
  Serial.printf("HTTP Web Server running.\n");

  Serial.printf("Unique Device ID: 0x%s\n", get_unique_id().c_str());

  // Point directly to your local gateway IP address
  configTime(0, 0, local_ntp); 
  // This string explicitly defines "PST" for standard and "PDT" for daylight savings
  setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1); 
  tzset();

  if (getLocalTime(&timeinfo)) {
    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d", 
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  } else {
    snprintf(timestamp, sizeof(timestamp), "[UNSYNCED]");
  }
  Serial.printf("ESP32 Web Server started on, %s.\n", timestamp);

  /*
  // 1. Create a text buffer string to hold the output
  char formattedTime[64]; 

  // 2. Use strftime to convert %Z and %z into readable text inside the buffer
  // %Z = Abbreviation (e.g. EST) | %z = Numeric offset (e.g. -0500)
  strftime(formattedTime, sizeof(formattedTime), "%Z", &timeinfo);
  
  // 3. Now use standard printf to display the string buffer using %s
  Serial.printf("%s\n", formattedTime);
  */

  say_hello();
}

void loop() {
  if (new_value_available) {
    new_value_available = false; // Reset the flag
    shared_input_message.trim(); // Remove any leading/trailing whitespace

    // Serial.printf("Input command: %s\n", shared_input_message.c_str());
    colon_pos = shared_input_message.indexOf(':');
    input_cmd = shared_input_message.substring(0, colon_pos);
    input_cmd.trim();
    input_cmd.toUpperCase();
    Serial.printf("command: %s\n", input_cmd.c_str());
    input_data = shared_input_message.substring(colon_pos + 1);
    input_data.trim();

    Serial.printf("data: %s\n", input_data.c_str());
    
    if (input_cmd == "COUNTER") {
      global_counter = input_data.toInt();
      Serial.printf("Updated global_counter and added commas: %s\n", 
        add_commas_to_string(global_counter).c_str());
    } else if (input_cmd == "LED") {
      input_data.toUpperCase();
      if (input_data == "ON") {
        // do action
        Serial.printf("Turning LED ON\n");
        digitalWrite(led_pin, HIGH);
      } else if (input_data == "OFF") {
        // do action
        Serial.printf("Turning LED OFF\n");
        digitalWrite(led_pin, LOW);
      }
    } else if (input_cmd == "SD_DELETE") {
      String del_file = "/" + input_data;
      Serial.printf("Deleting SD Card file: %s\n", del_file.c_str());
       SD.remove(del_file);
      Serial.printf("SD Deleted %s\n", del_file.c_str());
    } else if (input_cmd == "SD_MOUNT") {
        init_sd(SD_CS);
        Serial.printf("SD Card mounted\n");
        Serial.printf(" *** SD Card Available Space: %.2f GB\n", 
          ((double)(SD.totalBytes() - SD.usedBytes()) / 1e9)
          );
    } else if (input_cmd == "SD_REMOVE") {
        SD.end(); // Unmount the SD card
        Serial.printf("SD Card can be removed\n");
        Serial.printf(" *** SD Card Available Space: %.2f GB\n", 
          ((double)(SD.totalBytes() - SD.usedBytes()) / 1e9)
          );
    } else if (input_cmd == "CLEAR") {
      Serial.printf("CLEAR\n");
      events.send("[CLEAR_LOG_TRIGGER]", "log_update", millis());
    } else {
      Serial.printf("Unknown command: %s\n", input_cmd.c_str());
    }
  }

  // Send a non-blocking background update to all clients every 1 second
  if ((millis() - last_time) > 1000) {
    last_time = millis();

    if (getLocalTime(&timeinfo)) {
      snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d", 
              timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
              timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
      snprintf(timestamp, sizeof(timestamp), "[UNSYNCED]");
    }

    snprintf(output_buffer, sizeof(output_buffer), 
      "%s, 0x%s, SD: %.2f GB",
      timestamp,
      get_unique_id().c_str(),
      ((double)(SD.totalBytes() - SD.usedBytes()) / 1e9)
    );
    events.send(String(output_buffer).c_str(), "output_update", millis());
    
    snprintf(output_buffer, sizeof(output_buffer), 
      "%s \t\t%.2f °C, \t\t%.2f %%, \t\t%.2f hPa",
      format_with_commas(global_counter).c_str(),
      bme.readTemperature(),
      bme.readHumidity(),
      bme.readPressure() / 100.0F
    );
    events.send(String(output_buffer).c_str(), "log_update", millis());

    // Serial.printf("%s\n", output_buffer);
    global_counter++;
  }
}
