#include "webpage_routes.h"

// File handle for the incoming upload stream
static File uploadFile;

void init_webpage_routes(AsyncWebServer &server, fs::FS &sd_instance, fs::FS &fs_instance) {
  // Local structure definition to encapsulate request tracking state cleanly
  struct UploadState {
    File file;
    bool aborted = false;
  };

  // =========================================================================
  // 1. HOME ROUTE
  // =========================================================================
  server.on("/", HTTP_GET, [&fs_instance](AsyncWebServerRequest *request) {
    const char* path = "/index.html";
    if (!fs_instance.exists(path)) {
      request->send(404, "text/plain", "File Not Found inside LittleFS");
      return;
    }
    request->send(fs_instance, path, "text/html", false, [](const String& var) -> String {
      if (var == "BOARD_HOSTNAME") return String(WiFi.getHostname());
      return String();
    });
  });

  // =========================================================================
  // 2. CHECK ROUTE
  // =========================================================================
  server.on("/check-file", HTTP_GET, [&sd_instance](AsyncWebServerRequest *request) {
    if (!request->hasParam("name")) {
      request->send(400, "text/plain", "Missing name parameter");
      return;
    }
    String filename = request->getParam("name")->value();
    if (!filename.startsWith("/")) filename = "/" + filename;
    
    if (sd_instance.exists(filename)) {
      request->send(200, "text/plain", "true");
    } else {
      request->send(200, "text/plain", "false");
    }
  });

  // Inside your file serving route in webpage_routes.cpp:
  server.on("/download", HTTP_GET, [&sd_instance](AsyncWebServerRequest *request) {
    if (!request->hasParam("file")) {
      request->send(400, "text/plain", "Missing file parameter");
      return;
    }

    String filename = request->getParam("file")->value();
    if (!filename.startsWith("/")) {
      filename = "/" + filename;
    }

    if (!sd_instance.exists(filename)) {
      request->send(404, "text/plain", "File Not Found");
      return;
    }

    // PASSING 'true' AS THE LAST ARGUMENT FORCES THE DOWNLOAD PARAMETER!
    // Syntax: send(File system, Path, Content Type, Download parameter [true = force download filename])
    request->send(sd_instance, filename, "text/plain", true);
  });

  // =========================================================================
  // 3. UPLOAD ROUTE (Callbacks defined cleanly as local auto variables)
  // =========================================================================
  
 // A. Define what happens when the total upload finishes
  auto on_upload_complete = [](AsyncWebServerRequest *request) {
    UploadState *state = (UploadState *)request->_tempObject;
    if (state && state->aborted) {
      request->send(507, "text/plain", "Error: Insufficient Storage on SD Card");
    } else {
      request->send(200, "text/plain", "Upload Complete");
    }
    if (state) {
      delete state;
      request->_tempObject = nullptr;
    }
  };

  // B. Define how raw byte chunks are actively written to hardware
  auto on_incoming_chunk = [&sd_instance](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    UploadState *state = (UploadState *)request->_tempObject;
    if (!state) {
      state = new UploadState();
      request->_tempObject = state;
    }

    if (!index) {
      size_t total_file_size = request->contentLength();
      size_t free_space_on_sd = SD.totalBytes() - SD.usedBytes();

      if (total_file_size > free_space_on_sd) {
        Serial.printf("[WARN] Upload aborted. File (%u B) exceeds Free Space (%u B)\n", total_file_size, free_space_on_sd);
        state->aborted = true;
        return;
      }

      if (!filename.startsWith("/")) filename = "/" + filename;
      Serial.printf("[Web Server] Starting Upload: %s\n", filename.c_str());
      
      state->file = sd_instance.open(filename, FILE_WRITE);
      if (!state->file) {
        Serial.println("[ERROR] Failed to open target file on SD Card for writing.");
        state->aborted = true;
      }
    }

    if (!state->aborted && state->file && len) {
      state->file.write(data, len);
    }

    if (final && state->file) {
      state->file.close();
      if (!state->aborted) {
        Serial.printf("[Web Server] File Upload complete. Bytes written: %u\n", index + len);
      }
    }
  };

  // C. Register routes with zero visual inline nesting
  server.on("/upload", HTTP_POST, on_upload_complete, on_incoming_chunk);
}

void handle_sd_files(AsyncWebServerRequest *request) {
  File root = SD.open("/");
  if (!root) {
    request->send(500, "text/plain", "Failed to open SD card root directory.");
    return;
  }
  
  // 1. Change content type to text/html to let the browser parse links
  AsyncResponseStream *response = request->beginResponseStream("text/html");
  // Add a basic monospaced body wrapper for clean alignment
  response->print("<html><body style='font-family: monospace; line-height: 1.5;'>");
  response->print("<h2>SD Card files in /</h2>");
  
  File entry = root.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      const char* name = entry.name();
      size_t bytes = entry.size();
      
      // Handle optional leading slash formatting depending on core versions
      String filename_str = String(name);
      String url_path = filename_str.startsWith("/") ? filename_str : "/" + filename_str;
      String display_name = filename_str.startsWith("/") ? filename_str.substring(1) : filename_str;
      
      // Format file size into a human-readable string (KB or MB)
      String size_str;
      if (bytes >= 1024 * 1024) {
        size_str = String((float)bytes / (1024.0 * 1024.0), 2) + " MB";
      } else {
        size_str = String((float)bytes / 1024.0, 1) + " KB";
      }
      
      // 2. Format as a clickable link pointing to a viewing route with size appended
      response->printf("<a href='/view?file=%s' target='_blank'>%s</a> (%s)<br>\n", 
        url_path.c_str(), 
        display_name.c_str(),
        size_str.c_str()
      );
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
  response->print("</body></html>");
  
  // Ship remaining data packets to the browser
  request->send(response);
}

void say_hello(void) {
    Serial.printf("Hello, World!\n");
}

void mcu_dir(fs::FS &fs, const char * dir_name, uint8_t levels) {
  Serial.printf("Files in: %s\n", dir_name);

  File root = fs.open(dir_name, "r");
  if (!root) {
    Serial.printf("Failed to open directory\n");
    return;
  }
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      Serial.printf(" * %s (%d blocks)\n", file.name(), file.size());
    }
    file = root.openNextFile();
  }
  root.close(); // Clean up the root file system handle
}

void webpage_serve_html(AsyncWebServerRequest *request, fs::LittleFSFS &local_filesystem) { 
    const char* path = "/index.html"; 
    
    if (!local_filesystem.exists(path)) { 
        request->send(404, "text/plain", "Index HTML Missing from Flash"); 
        return; 
    } 
    
    File file = local_filesystem.open(path, "r"); 
    if (!file) { 
        request->send(500, "text/plain", "Failed to open HTML template."); 
        return; 
    } 
    
    String htmlContent = file.readString(); 
    file.close(); 
    
    htmlContent.replace("%BOARD_HOSTNAME%", ESP32_HOSTNAME); 
    request->send(200, "text/html", htmlContent); 
}

void webpage_led(AsyncWebServer &server, const int led_pin) {
  pinMode(led_pin, OUTPUT);

  server.on("/led", HTTP_GET, [led_pin](AsyncWebServerRequest *request) {
    if (request -> hasArg("state")) {
      String state = request ->arg("state");
      if (state == "on") {
        digitalWrite(led_pin, HIGH);
        request -> send(200, "text/plain", "LED ON");
      } else if (state == "off") {
        digitalWrite(led_pin, LOW);
        request -> send(200, "text/plain", "LED OFF");
      } else {
        request -> send(400, "text/plain", "Invalid State Value");
      }
    } else {
      request -> send(400, "text/plain", "Missing State Parameter");
    }
  });
}

void webpage_file_list(AsyncWebServerRequest *request, fs::FS &fs) { 
    File root = fs.open("/", "r"); 
    if (!root || !root.isDirectory()) { 
        request->send(500, "text/plain", "Failed to open directory"); 
        return; 
    } 

    String json = "["; 
    File file = root.openNextFile(); 
    while (file) { 
      // Serial.printf("File: %s %d\n", file.name(), file.isDirectory());
      if (!file.isDirectory()) {
        String file_name = String(file.name());
        // Check if the file matches any excluded extensions
        if (!file_name.endsWith(".html") && !file_name.endsWith(".foo")) {
            if (json != "[") { 
                json += ","; 
            } 
            json += "\"" + file_name + "\""; 
        }
      }
      file = root.openNextFile(); 
    } 
    json += "]"; 

    request->send(200, "application/json", json); 
}

String add_commas_to_string(long value) {
  String original = String(value);
  String formatted = "";
  
  // Handle negative numbers smoothly
  bool is_negative = value < 0;
  if (is_negative) {
    original.remove(0, 1); // Temporarily strip the minus sign
  }

  int len = original.length();
  
  // Loop through characters backwards to place commas every 3 digits
  for (int i = 0; i < len; i++) {
    if (i > 0 && i % 3 == 0) {
      formatted = "," + formatted;
    }
    formatted = original[len - 1 - i] + formatted;
  }
  
  // Re-attach the minus sign if the number was negative
  if (is_negative) {
    formatted = "-" + formatted;
  }
  
  return formatted;
}

bool init_sd(int chip_select_pin) {
  Serial.println("Attempting to initialize SD card...");
  if (SD.begin(chip_select_pin)) {
    Serial.println("✅ SD Card Successfully Initialized / Re-inserted!");
    return true;
    
    // Optional: Print card type or size here to verify it works
    uint8_t cardType = SD.cardType();
    if(cardType == CARD_NONE){
        Serial.println("No SD card type recognized");
    }
  } else {
    Serial.println("❌ No SD card found.");
    return false;
  }
}

String get_unique_id() {
  uint8_t mac[6];
  char macStr[13]; // 12 hex characters + null terminator
  
  // Fetch the factory-programmed base MAC address
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
    snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
  }
  
  return "UNKNOWN_ESP32";
}

String format_with_commas(long value) {
  String original = String(value);
  String formatted = "";
  
  // Handle negative numbers smoothly
  bool is_negative = value < 0;
  if (is_negative) {
    original.remove(0, 1); // Temporarily strip the minus sign
  }

  int len = original.length();
  
  // Loop through characters backwards to place commas every 3 digits
  for (int i = 0; i < len; i++) {
    if (i > 0 && i % 3 == 0) {
      formatted = "," + formatted;
    }
    formatted = original[len - 1 - i] + formatted;
  }
  
  // Re-attach the minus sign if the number was negative
  if (is_negative) {
    formatted = "-" + formatted;
  }
  
  return formatted;
}

