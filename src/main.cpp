#include <Arduino.h>
#include "secrets.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <esp_wifi.h>

const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;

// ESP-NOW peer MAC address (replace with your receiver's MAC address)
// You can find MAC address by printing WiFi.macAddress() on the receiver
uint8_t receiverMacAddress[] = {0xCC, 0xBA, 0x97, 0x16, 0x2A, 0xF8}; // Broadcast address - change to specific MAC

// ESP-NOW variables
esp_now_peer_info_t peerInfo;
bool espNowInitialized = false;


// Use values from secrets.h so they can be configured centrally
const char *mqtt_server = MQTT_SERVER;  // Your broker hostname (from secrets.h)
const int mqtt_port = MQTT_PORT;

WiFiClient wifiClient;
PubSubClient client(wifiClient);
void mqttReconnect() ;
void publishCoordinates(const struct ISSData& data);
// Structure to store ISS position data
struct ISSData {
  String message;        // API response status
  float latitude;        // ISS latitude
  float longitude;       // ISS longitude
  unsigned long timestamp; // Unix timestamp
  bool dataValid;        // Whether we have valid data
};

// Global variable to store the latest ISS data
ISSData issData = {"", 0.0, 0.0, 0, false};

// Timing for periodic ISS API polling
unsigned long lastFetchMillis = 0;
const unsigned long fetchIntervalMs = 10000; // fetch every 10s

// Track last published ISS timestamp so we only publish when data changes
unsigned long lastPublishedTimestamp = 0;

// Timing for ESP-NOW sends
unsigned long lastESPNowSendMillis = 0;
const unsigned long espnowSendIntervalMs = 2000; // send every 2 seconds

// ========== ESP-NOW Functions ==========

// Callback when data is sent via ESP-NOW
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.println("\n========== ESP-NOW CALLBACK ==========");
  Serial.print("[ESP-NOW] Packet sent to: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", mac_addr[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
  
  Serial.print("[ESP-NOW] Send Status: ");
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("SUCCESS - Data delivered!");
  } else {
    Serial.println("FAILED - Data not delivered!");
  }
  Serial.println("======================================\n");
}

// Initialize ESP-NOW
bool initESPNow() {
  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Error initializing ESP-NOW");
    return false;
  }
  
  Serial.println("[ESP-NOW] Successfully initialized");
  
  // Register send callback
  esp_now_register_send_cb(OnDataSent);
  
  // Get the current WiFi channel
  uint8_t currentChannel = WiFi.channel();
  Serial.print("[ESP-NOW] Current WiFi Channel: ");
  Serial.println(currentChannel);
  
  // Register peer
  memcpy(peerInfo.peer_addr, receiverMacAddress, 6);
  peerInfo.channel = currentChannel;  // Use actual WiFi channel
  peerInfo.encrypt = false;
  
  // Add peer
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("[ESP-NOW] Failed to add peer");
    return false;
  }
  
  Serial.println("[ESP-NOW] Peer added successfully");
  Serial.print("[ESP-NOW] Peer channel set to: ");
  Serial.println(peerInfo.channel);
  Serial.print("[ESP-NOW] Receiver MAC: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", receiverMacAddress[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
  
  return true;
}

// Send JSON data via ESP-NOW
void sendJsonViaESPNow(String jsonData) {
  if (!espNowInitialized) {
    Serial.println("[ESP-NOW] ESP-NOW not initialized, skipping send");
    return;
  }
  
  // Check WiFi status
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ESP-NOW] WARNING: WiFi not connected!");
  }
  
  // Verify we're on the right channel
  Serial.print("[ESP-NOW] Current WiFi channel: ");
  Serial.println(WiFi.channel());
  
  // ESP-NOW has a limit of 250 bytes per packet
  if (jsonData.length() > 250) {
    Serial.println("[ESP-NOW] Warning: JSON data exceeds 250 bytes, truncating...");
    jsonData = jsonData.substring(0, 250);
  }
  
  Serial.println("\n[ESP-NOW] Sending JSON data...");
  Serial.print("[ESP-NOW] Data length: ");
  Serial.print(jsonData.length());
  Serial.println(" bytes");
  Serial.print("[ESP-NOW] Target MAC: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", receiverMacAddress[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
  Serial.print("[ESP-NOW] Data: ");
  Serial.println(jsonData);
  Serial.flush(); // Ensure all data is sent to serial before ESP-NOW send
  
  // Send data
  esp_err_t result = esp_now_send(receiverMacAddress, (uint8_t *)jsonData.c_str(), jsonData.length());
  
  if (result == ESP_OK) {
    Serial.println("[ESP-NOW] Data sent successfully (queued)");
    Serial.println("[ESP-NOW] Waiting for send callback...");
    Serial.flush(); // Flush before delay
    delay(250); // Give callback time to execute and display
  } else {
    Serial.println("[ESP-NOW] Error sending data");
    Serial.print("[ESP-NOW] Error code: ");
    Serial.println(result);
    
    // Detailed error descriptions
    if (result == ESP_ERR_ESPNOW_NOT_INIT) {
      Serial.println("[ESP-NOW] ESP-NOW not initialized");
    } else if (result == ESP_ERR_ESPNOW_ARG) {
      Serial.println("[ESP-NOW] Invalid argument");
    } else if (result == ESP_ERR_ESPNOW_INTERNAL) {
      Serial.println("[ESP-NOW] Internal error");
    } else if (result == ESP_ERR_ESPNOW_NO_MEM) {
      Serial.println("[ESP-NOW] Out of memory");
    } else if (result == ESP_ERR_ESPNOW_NOT_FOUND) {
      Serial.println("[ESP-NOW] Peer not found");
    }
  }
  Serial.flush(); // Final flush
}

// ========== End ESP-NOW Functions ==========

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Topic: "); Serial.println(topic);
  String msg;
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  Serial.print("Payload: "); Serial.println(msg);
}

/*void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("MQTT msg on [");
  Serial.print(topic);
  Serial.print("]: ");
  for (unsigned int i = 0; i < length; i++) Serial.write(payload[i]);
  Serial.println();

  if (String(topic) == "capter") {
    if (length == 1 && payload[0] == '1') {
      Serial.println("Turning LED ON");
     // digitalWrite(LED_PIN, HIGH);
    } else {
      Serial.println("Turning LED OFF");
     // digitalWrite(LED_PIN, LOW);
    }
  }
}*/
// fonction qui monntre l'etat de la connection mqtt
const char* mqttStateToString(int8_t state) {
  switch (state) {
    case -4: return "MQTT_CONNECTION_TIMEOUT";
    case -3: return "MQTT_CONNECTION_LOST";
    case -2: return "MQTT_CONNECT_FAILED";
    case -1: return "MQTT_DISCONNECTED";
    case 0: return "MQTT_CONNECTED";
    case 1: return "MQTT_CONNECT_BAD_PROTOCOL";
    case 2: return "MQTT_CONNECT_BAD_CLIENT_ID";
    case 3: return "MQTT_CONNECT_UNAVAILABLE";
    case 4: return "MQTT_CONNECT_BAD_CREDENTIALS";
    case 5: return "MQTT_CONNECT_UNAUTHORIZED";
    default: return "MQTT_UNKNOWN";
  }
}

String translateEncryptionType(wifi_auth_mode_t encryptionType) {
  switch (encryptionType) {
    case (WIFI_AUTH_OPEN): return "Open";
    case (WIFI_AUTH_WEP): return "WEP";
    case (WIFI_AUTH_WPA_PSK): return "WPA_PSK";
    case (WIFI_AUTH_WPA2_PSK): return "WPA2_PSK";
    case (WIFI_AUTH_WPA_WPA2_PSK): return "WPA_WPA2_PSK";
    case (WIFI_AUTH_WPA2_ENTERPRISE): return "WPA2_ENTERPRISE";
    default: return "UNKNOWN";
  }
}

/* Function to display WiFi connection errors */
void showWiFiError(wl_status_t status) {
  Serial.print("WiFi Error: ");
  switch (status) {
    case WL_NO_SHIELD:
      Serial.println("NO_SHIELD - WiFi shield not present");
      break;
    case WL_IDLE_STATUS:
      Serial.println("IDLE_STATUS - WiFi is in idle mode");
      break;
    case WL_NO_SSID_AVAIL:
      Serial.println("NO_SSID_AVAIL - Configured SSID cannot be reached");
      Serial.println("  → Check if SSID name is correct");
      Serial.println("  → Check if router is powered on and in range");
      break;
    case WL_SCAN_COMPLETED:
      Serial.println("SCAN_COMPLETED - WiFi scan completed");
      break;
    case WL_CONNECTED:
      Serial.println("CONNECTED - Successfully connected to WiFi");
      break;
    case WL_CONNECT_FAILED:
      Serial.println("CONNECT_FAILED - Connection failed");
      Serial.println("  → Check WiFi password");
      Serial.println("  → Check network security type");
      Serial.println("  → Try restarting the router");
      break;
    case WL_CONNECTION_LOST:
      Serial.println("CONNECTION_LOST - Connection was lost");
      Serial.println("  → Signal may be too weak");
      Serial.println("  → Router may have restarted");
      break;
    case WL_DISCONNECTED:
      Serial.println("DISCONNECTED - Disconnected from network");
      Serial.println("  → Check if credentials are correct");
      Serial.println("  → Check if MAC filtering is enabled on router");
      break;
    default:
      Serial.print("UNKNOWN STATUS - Code: ");
      Serial.println(status);
      break;
  }
}

/* Helper function to extract a value from JSON by key */
String extractJsonValue(String json, String key) {
  String searchKey = "\"" + key + "\"";
  int keyIndex = json.indexOf(searchKey);
  
  if (keyIndex == -1) {
    return "NOT_FOUND";
  }
  
  // Find the colon after the key
  int colonIndex = json.indexOf(":", keyIndex);
  if (colonIndex == -1) return "ERROR";
  
  // Skip whitespace and find the value
  int valueStart = colonIndex + 1;
  while (valueStart < json.length() && (json.charAt(valueStart) == ' ' || json.charAt(valueStart) == '\t')) {
    valueStart++;
  }
  
  // Check if value is a string (starts with ")
  if (json.charAt(valueStart) == '"') {
    valueStart++; // Skip opening quote
    int valueEnd = json.indexOf('"', valueStart);
    if (valueEnd == -1) return "ERROR";
    return json.substring(valueStart, valueEnd);
  }
  // Check if value is an object or array
  else if (json.charAt(valueStart) == '{' || json.charAt(valueStart) == '[') {
    char endChar = (json.charAt(valueStart) == '{') ? '}' : ']';
    int depth = 1;
    int valueEnd = valueStart + 1;
    while (valueEnd < json.length() && depth > 0) {
      if (json.charAt(valueEnd) == json.charAt(valueStart)) depth++;
      if (json.charAt(valueEnd) == endChar) depth--;
      valueEnd++;
    }
    return json.substring(valueStart, valueEnd);
  }
  // Value is a number, boolean, or null
  else {
    int valueEnd = valueStart;
    while (valueEnd < json.length() && 
           json.charAt(valueEnd) != ',' && 
           json.charAt(valueEnd) != '}' && 
           json.charAt(valueEnd) != ']' &&
           json.charAt(valueEnd) != ' ') {
      valueEnd++;
    }
    return json.substring(valueStart, valueEnd);
  }
}

/* Function to parse and display JSON response in readable format */
void parseAndDisplayJson(String json) {
  Serial.println("\n=== Parsed Data (Readable Format) ===");
  
  json.trim();
  
  // Parse top-level keys only, then handle nested objects separately
  int pos = 1; // Start after opening {
  
  while (pos < json.length()) {
    // Find the next key
    int keyStart = json.indexOf('"', pos);
    if (keyStart == -1) break;
    
    int keyEnd = json.indexOf('"', keyStart + 1);
    if (keyEnd == -1) break;
    
    String key = json.substring(keyStart + 1, keyEnd);
    
    //trouve les deux-points après la clé
    int colon = json.indexOf(':', keyEnd);
    if (colon == -1) break;
    
    // saute les espaces après les deux-points
    int valueStart = colon + 1;
    while (valueStart < json.length() && (json.charAt(valueStart) == ' ' || json.charAt(valueStart) == '\t')) {
      valueStart++;
    }
    
    // Check what type of value this is
    if (json.charAt(valueStart) == '{') {
      // Nested object - extract it
      int depth = 1;
      int valueEnd = valueStart + 1;
      while (valueEnd < json.length() && depth > 0) {
        if (json.charAt(valueEnd) == '{') depth++;
        if (json.charAt(valueEnd) == '}') depth--;
        valueEnd++;
      }
      String nestedObj = json.substring(valueStart + 1, valueEnd - 1);
      
      // Print the key
      Serial.print("  ");
      Serial.print(key);
      Serial.println(":");
      
      // Parse nested object
      int nestedPos = 0;
      while (nestedPos < nestedObj.length()) {
        int nKeyStart = nestedObj.indexOf('"', nestedPos);
        if (nKeyStart == -1) break;
        
        int nKeyEnd = nestedObj.indexOf('"', nKeyStart + 1);
        if (nKeyEnd == -1) break;
        
        String nestedKey = nestedObj.substring(nKeyStart + 1, nKeyEnd);
        
        // Find the colon after the key
        int nColon = nestedObj.indexOf(':', nKeyEnd);
        if (nColon == -1) break;
        
        // Skip whitespace after colon
        int nValueStart = nColon + 1;
        while (nValueStart < nestedObj.length() && 
               (nestedObj.charAt(nValueStart) == ' ' || nestedObj.charAt(nValueStart) == '\t')) {
          nValueStart++;
        }
        
        // Extract the value
        String nestedValue;
        if (nestedObj.charAt(nValueStart) == '"') {
          // String value
          int nValueEnd = nestedObj.indexOf('"', nValueStart + 1);
          if (nValueEnd == -1) break;
          nestedValue = nestedObj.substring(nValueStart + 1, nValueEnd);
          nestedPos = nValueEnd + 1;
        } else {
          // Number, boolean, or null
          int nValueEnd = nValueStart;
          while (nValueEnd < nestedObj.length() && 
                 nestedObj.charAt(nValueEnd) != ',' && 
                 nestedObj.charAt(nValueEnd) != '}' && 
                 nestedObj.charAt(nValueEnd) != ' ') {
            nValueEnd++;
          }
          nestedValue = nestedObj.substring(nValueStart, nValueEnd);
          nestedValue.trim();
          nestedPos = nValueEnd + 1;
        }
        
        Serial.print("    - ");
        Serial.print(nestedKey);
        Serial.print(": ");
        Serial.println(nestedValue);
      }
      
      pos = valueEnd;
    } else if (json.charAt(valueStart) == '"') {
      // String value
      int valueEnd = json.indexOf('"', valueStart + 1);
      if (valueEnd == -1) break;
      
      String value = json.substring(valueStart + 1, valueEnd);
      Serial.print("  ");
      Serial.print(key);
      Serial.print(": ");
      Serial.println(value);
      
      pos = valueEnd + 1;
    } else {
      // Number, boolean, or null
      int valueEnd = valueStart;
      while (valueEnd < json.length() && 
             json.charAt(valueEnd) != ',' && 
             json.charAt(valueEnd) != '}' && 
             json.charAt(valueEnd) != ' ') {
        valueEnd++;
      }
      
      String value = json.substring(valueStart, valueEnd);
      value.trim();
      Serial.print("  ");
      Serial.print(key);
      Serial.print(": ");
      Serial.println(value);
      
      pos = valueEnd + 1;
    }
  }
  
  Serial.println("=====================================\n");
}

/* Function to extract and store ISS data from JSON response */
void extractAndStoreISSData(String json) {
  // Extract the values
  String message = extractJsonValue(json, "message");
  String latStr = extractJsonValue(json, "latitude");
  String lonStr = extractJsonValue(json, "longitude");
  String timestampStr = extractJsonValue(json, "timestamp");
  
  // Store in global structure
  issData.message = message;
  issData.latitude = latStr.toFloat();
  issData.longitude = lonStr.toFloat();
  issData.timestamp = timestampStr.toInt();
  issData.dataValid = (message == "success");
  
  // Print confirmation
  Serial.println("\n>>> Data stored in 'issData' structure:");
  Serial.print("    issData.message = ");
  Serial.println(issData.message);
  Serial.print("    issData.latitude = ");
  Serial.println(issData.latitude, 4);
  Serial.print("    issData.longitude = ");
  Serial.println(issData.longitude, 4);
  Serial.print("    issData.timestamp = ");
  Serial.println(issData.timestamp);
  Serial.print("    issData.dataValid = ");
  Serial.println(issData.dataValid ? "true" : "false");
  Serial.println();
}

/* Example: Get specific values from JSON response */
void getSpecificData(const char* url, String key1, String key2 = "", String key3 = "") {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    
    Serial.println("\n--- Getting Specific Data ---");
    http.begin(url);
    int httpResponseCode = http.GET();
    
    if (httpResponseCode == 200) {
      String payload = http.getString();
      
      // Extract and display specific values
      if (key1 != "") {
        String value1 = extractJsonValue(payload, key1);
        Serial.print(key1);
        Serial.print(" = ");
        Serial.println(value1);
      }
      
      if (key2 != "") {
        String value2 = extractJsonValue(payload, key2);
        Serial.print(key2);
        Serial.print(" = ");
        Serial.println(value2);
      }
      
      if (key3 != "") {
        String value3 = extractJsonValue(payload, key3);
        Serial.print(key3);
        Serial.print(" = ");
        Serial.println(value3);
      }
      
    } else {
      Serial.print("Error: ");
      Serial.println(httpResponseCode);
    }
    
    http.end();
  }
}

/* Function to send HTTP GET request */
void sendHTTPGet(const char* url) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    
    Serial.println("\n--- HTTP GET Request ---");
    Serial.print("URL: ");
    Serial.println(url);
    
    http.begin(url);  // Specify the URL
    
    int httpResponseCode = http.GET();  // Send the request
    
    if (httpResponseCode > 0) {
      Serial.print("HTTP Response code: ");
      Serial.println(httpResponseCode);
      
      String payload = http.getString();  // Get the response payload
      Serial.println("Response:");
      Serial.println(payload);
    } else {
      Serial.print("Error code: ");
      Serial.println(httpResponseCode);
      Serial.println("Error: " + http.errorToString(httpResponseCode));
    }
    
    http.end();  // Free resources
  } else {
    Serial.println("WiFi not connected!");
  }
}

/* Function to send HTTP GET request and parse response */
void sendHTTPGetParsed(const char* url) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    
    Serial.println("\n--- HTTP GET Request (Parsed) ---");
    Serial.print("URL: ");
    Serial.println(url);
    Serial.print("WiFi Status: Connected, IP: ");
    Serial.println(WiFi.localIP());
    
    // Set timeout to 10 seconds
    http.setTimeout(10000);
    
    // Try to begin connection
    Serial.println("Starting HTTP connection...");
    bool beginResult = http.begin(url);
    
    if (!beginResult) {
      Serial.println("ERROR: Failed to begin HTTP connection!");
      Serial.println("This could mean:");
      Serial.println("  - Invalid URL format");
      Serial.println("  - DNS lookup failed");
      Serial.println("  - Network issue");
      http.end();
      return;
    }
    
    // Collect response headers
    const char* headerKeys[] = {"Content-Type", "Content-Length", "Server", "Date", "Connection", "Cache-Control"};
    const size_t headerKeysCount = sizeof(headerKeys) / sizeof(headerKeys[0]);
    http.collectHeaders(headerKeys, headerKeysCount);
    
    Serial.println("HTTP connection established, sending GET request...");
    int httpResponseCode = http.GET();
    
    if (httpResponseCode > 0) {
      Serial.print("HTTP Response code: ");
      Serial.println(httpResponseCode);
      
      // Display response headers
      Serial.println("\n--- HTTP Response Headers ---");
      
      // Get common headers
      if (http.hasHeader("Content-Type")) {
        Serial.print("Content-Type: ");
        Serial.println(http.header("Content-Type"));
      }
      
      if (http.hasHeader("Content-Length")) {
        Serial.print("Content-Length: ");
        Serial.println(http.header("Content-Length"));
      }
      
      if (http.hasHeader("Server")) {
        Serial.print("Server: ");
        Serial.println(http.header("Server"));
      }
      
      if (http.hasHeader("Date")) {
        Serial.print("Date: ");
        Serial.println(http.header("Date"));
      }
      
      if (http.hasHeader("Connection")) {
        Serial.print("Connection: ");
        Serial.println(http.header("Connection"));
      }
      
      if (http.hasHeader("Cache-Control")) {
        Serial.print("Cache-Control: ");
        Serial.println(http.header("Cache-Control"));
      }
      
      Serial.println("-----------------------------");
      
      String payload = http.getString();
      
      // Display raw JSON
      Serial.println("\n--- Raw JSON Response ---");
      Serial.println(payload);
      
      // NOTE: ESP-NOW send moved to publishCoordinates() for compact format
      
      // Parse and display in readable format
      parseAndDisplayJson(payload);
      
      // Extract and store the data
      extractAndStoreISSData(payload);

   
      
    } else {
      Serial.print("Error code: ");
      Serial.println(httpResponseCode);
      Serial.print("Error description: ");
      Serial.println(http.errorToString(httpResponseCode));
      
      // Detailed error explanations
      Serial.println("\nPossible causes:");
      if (httpResponseCode == -1) {
        Serial.println("  → Connection refused by server");
        Serial.println("  → Check if URL is correct");
        Serial.println("  → Server might be down");
        Serial.println("  → Try using HTTPS if available");
      } else if (httpResponseCode == -2) {
        Serial.println("  → Failed to send header");
      } else if (httpResponseCode == -3) {
        Serial.println("  → Failed to connect to server");
        Serial.println("  → DNS lookup may have failed");
      } else if (httpResponseCode == -11) {
        Serial.println("  → Request timeout (server didn't respond)");
      }
    }
    
    http.end();
  } else {
    Serial.println("WiFi not connected!");
    Serial.print("WiFi Status: ");
    Serial.println(WiFi.status());
  }
}

/* Function to send HTTP POST request with JSON data */
void sendHTTPPost(const char* url, const char* jsonData) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    
    Serial.println("\n--- HTTP POST Request ---");
    Serial.print("URL: ");
    Serial.println(url);
    Serial.print("Data: ");
    Serial.println(jsonData);
    
    http.begin(url);  // specifier le url
    http.addHeader("Content-Type", "application/json");  // specify le type de contenu
    
    int httpResponseCode = http.POST(jsonData);  // envoi la requete
    
    if (httpResponseCode > 0) {
      Serial.print("HTTP Response code: ");
      Serial.println(httpResponseCode);
      
      String response = http.getString();  //recoit la reponse
      Serial.println("Response:");
      Serial.println(response);
    } else {
      Serial.print("Error code: ");
      Serial.println(httpResponseCode);
      Serial.println("Error: " + http.errorToString(httpResponseCode));
    }
    
    http.end();  // Free resources
  } else {
    Serial.println("WiFi not connected!");
  }
}

/* fonction qui envoie une requête HTTP */
void sendHTTPRequest(const char* url, const char* method, const char* data = nullptr, const char* authToken = nullptr) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    
    Serial.println("\n--- HTTP Request ---");
    Serial.print("Method: ");
    Serial.println(method);
    Serial.print("URL: ");
    Serial.println(url);
    
    http.begin(url);
    
    // Ajout d'une autorisation si le token est fourni
    if (authToken != nullptr) {
      String authHeader = "Bearer ";
      authHeader += authToken;
      http.addHeader("Authorization", authHeader);
    }
    
    int httpResponseCode;
    
    if (strcmp(method, "GET") == 0) {
      httpResponseCode = http.GET();
    } else if (strcmp(method, "POST") == 0) {
      http.addHeader("Content-Type", "application/json");
      httpResponseCode = http.POST(data ? data : "");
    } else if (strcmp(method, "PUT") == 0) {
      http.addHeader("Content-Type", "application/json");
      httpResponseCode = http.PUT(data ? data : "");
    } else if (strcmp(method, "DELETE") == 0) {
      httpResponseCode = http.sendRequest("DELETE");
    } else {
      Serial.println("Unsupported HTTP method");
      http.end();
      return;
    }
    // recoit le code de connection (par exemple 200 pour OK, 404 pour erreur)
    if (httpResponseCode > 0) {
      Serial.print("HTTP Response code: ");
      Serial.println(httpResponseCode);
      
      String response = http.getString();
      Serial.println("Response:");
      Serial.println(response);
    } else if(httpResponseCode == -1) {
      Serial.println("Connection failed");
    } else if(httpResponseCode == -2) {
      Serial.println("Error: Send header failed");
    } else if(httpResponseCode == -3) {
      Serial.println("Error: Send payload failed");
    } else if(httpResponseCode == -4) {
      Serial.println("Error: Not connected");
      while(true);
    } else if(httpResponseCode == -11) {
      Serial.println("Error: Read timeout");
      while(true);
    } else if(httpResponseCode == 400) {
      Serial.println("Error: Bad Request");
      while(true);
    } else if(httpResponseCode == 404) {
      Serial.println("Error: Not Found");
      while(true);
    } else {
      Serial.print("Error code: ");
      Serial.println(httpResponseCode);
      Serial.println("Error: " + http.errorToString(httpResponseCode));
      while(true);// boucle infinie en cas d'erreur non gérée
    }
    
    http.end();
  } else {
    Serial.println("WiFi not connected!");
  }
}

/*fonction qui permet de se connecter au réseau WiFi*/
void connectToNetwork() {
  Serial.println("Connecting to WiFi...");
  Serial.println("Note: Channel will be determined by the router");
  
  // Configure WiFi to station mode first
  WiFi.mode(WIFI_STA);
  
  // Connect without forcing channel (router determines the channel)
  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi network...");
  
  int attempts = 0;
  const int maxAttempts = 20; // 20 seconds timeout
  
  /* Attente de la connexion */
  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    delay(1000);
    attempts++;
    Serial.print(".");
    
    // Check status every 5 seconds
    if (attempts % 5 == 0) {
      Serial.println();
      showWiFiError(WiFi.status());
    }
  }
  
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected to network");
    Serial.print("WiFi Channel: ");
    Serial.println(WiFi.channel());
  } else {
    Serial.println("Failed to connect to WiFi after timeout");
    showWiFiError(WiFi.status());
  }
}

// removed old reconnect() - use mqttReconnect() instead

void setup() {
  Serial.begin(115200);
  while(!Serial); // Attendre que la connexion série soit établie
  pinMode(5, OUTPUT);
  pinMode(6, INPUT);
  digitalWrite(5, LOW);
  delay(100);
 
 // Serial.println("\nStarting ESP32 MQTT Client");
  
  // WiFi mode will be set in connectToNetwork()
  
  connectToNetwork();
  
  Serial.print("Connected to WiFi, IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("Gateway: ");
  Serial.println(WiFi.gatewayIP());
  Serial.print("DNS: ");
  Serial.println(WiFi.dnsIP());
  Serial.print("WiFi Channel: ");
  Serial.println(WiFi.channel());
  
  // Print this ESP32's MAC address
  Serial.print("This ESP32 MAC Address: ");
  Serial.println(WiFi.macAddress());
  
  // Initialize ESP-NOW
  Serial.println("\n[ESP-NOW] Initializing ESP-NOW...");
  espNowInitialized = initESPNow();
  if (espNowInitialized) {
    Serial.println("[ESP-NOW] Ready to send data!");
  } else {
    Serial.println("[ESP-NOW] Initialization failed, will not send data");
  }
  
  // Wait a bit for DNS to be fully ready
  Serial.println("Waiting for network to stabilize...");
  delay(2000);
  

  
  // Request to a public API - ISS location tracker
  sendHTTPGetParsed("http://api.open-notify.org/iss-now.json");
  
  // Example: Using the stored data
  Serial.println("\n=== EXAMPLE: Using stored ISS data ===");
  if (issData.dataValid) {
    Serial.print("The ISS is currently at coordinates: ");
    Serial.print(issData.latitude, 4);
    Serial.print(", ");
    Serial.println(issData.longitude, 4);
    
    // Example calculations
    Serial.print("Distance from equator: ");
    Serial.print(abs(issData.latitude));
    Serial.println(" degrees");
    
    // Determine hemisphere
    Serial.print("Hemisphere: ");
    if (issData.latitude >= 0) {
      Serial.print("Northern (");
    } else {
      Serial.print("Southern (");
    }
    if (issData.longitude >= 0) {
      Serial.println("Eastern)");
    } else {
      Serial.println("Western)");
    }
  } else {
    Serial.println("No valid ISS data available yet.");
  }
  Serial.println("======================================\n");
  

  
  client.setServer(mqtt_server, mqtt_port);
  // enable MQTT message callback
  client.setCallback(callback);
  
  //reconnect();http://api.open-notify.org/iss-now.json
}



void loop() {
  // Ensure WiFi stays connected
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection lost!");
    showWiFiError(WiFi.status()); // afficher les erreurs de connexion WiFi
    Serial.println("Reconnecting...");
    connectToNetwork();
    // Give time to reconnect
    delay(1000);
    return;
  }

  // Ensure MQTT connection
  if (!client.connected()) {
    mqttReconnect();
  }

  // Let the MQTT client process incoming messages and keep the connection alive
  client.loop();

  // Publish coordinates only when connected
  // Periodically fetch the ISS API every 10 seconds
  if (millis() - lastFetchMillis >= fetchIntervalMs) {
    lastFetchMillis = millis();
    sendHTTPGetParsed("http://api.open-notify.org/iss-now.json");
  }

  // Publish only when we have new data
  if (client.connected() && issData.dataValid && issData.timestamp != lastPublishedTimestamp) {
    publishCoordinates(issData);
    lastPublishedTimestamp = issData.timestamp;
  }
  
  // Send ESP-NOW data every 2 seconds (independent of MQTT)
  if (millis() - lastESPNowSendMillis >= espnowSendIntervalMs) {
    lastESPNowSendMillis = millis();
    
    if (espNowInitialized && issData.dataValid) {
      // Create compact JSON payload
      char payload[128];
      snprintf(payload, sizeof(payload), "{\"latitude\":%.6f,\"longitude\":%.6f,\"timestamp\":%lu}", 
               issData.latitude, issData.longitude, issData.timestamp);
      
      Serial.println("\n[ESP-NOW] Periodic send (every 2 seconds)...");
      String espnowPayload = String(payload);
      sendJsonViaESPNow(espnowPayload);
    }
  }
  
  delay(100); // Reduced delay for more responsive timing
}

// MQTT reconnect helper using credentials from secrets.h
void mqttReconnect() {
  if (client.connected()) return;

  Serial.print("Attempting MQTT connection...");
  String clientId = "esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  bool connected;
  // Print which clientId and username we'll try (do NOT print the password)
  // Use a static clientId as requested
  const char* staticClientId = "esp2Ow";
  Serial.print("MQTT clientId: "); Serial.println(staticClientId);
  Serial.print("MQTT username: "); Serial.println(MQTT_USER[0] ? MQTT_USER : "(none)");

  if (MQTT_USER[0] != '\0') {
    connected = client.connect(staticClientId, MQTT_USER, MQTT_PASSWORD);
  } else {
    connected = client.connect(staticClientId);
  }

  if (connected) {
    Serial.println("connected");
    Serial.println("connected (no subscriptions)");
  } else {
    int state = client.state();
    Serial.print("failed, rc="); Serial.print(state);
    Serial.print(" ("); Serial.print(mqttStateToString(state)); Serial.println(")");
  }
}
// fonction qui publie les coordonnées de l'ISS via MQTT
void publishCoordinates(const ISSData& data) {
  if (!client.connected()) {
    mqttReconnect();
  }
  if (!client.connected()) {
    Serial.println("MQTT not connected; cannot publish coordinates");
    return;
  }

  char payload[128];
  snprintf(payload, sizeof(payload), "{\"latitude\":%.6f,\"longitude\":%.6f,\"timestamp\":%lu}", data.latitude, data.longitude, data.timestamp);

  bool res = client.publish(MQTT_TOPIC_COORDS, payload);
  Serial.print("Publish "); Serial.print(MQTT_TOPIC_COORDS); Serial.print(": "); Serial.println(payload);
  Serial.print("Publish result: "); Serial.println(res ? "OK" : "FAIL");
  
  // Note: ESP-NOW sends are now handled in loop() every 2 seconds
}