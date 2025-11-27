# ESP32 TCP Client Project

## Overview
This project is designed for an ESP32-based application that connects to a WiFi network and communicates with an MQTT broker. It includes functionality for reading ultrasonic distance measurements and publishing the data to a specified MQTT topic.

## Project Structure
```
esp32-tcp-client
├── src
│   ├── main.cpp          # Main source code for the ESP32 application
│   └── secrets.h        # Contains sensitive information like WiFi credentials
├── include
│   └── mqtt_helpers.h    # Helper functions for MQTT operations
├── lib
│   └── (optional custom libraries)
├── platformio.ini        # Configuration file for PlatformIO
├── .gitignore            # Specifies files to be ignored by Git
└── README.md             # Documentation for the project
```

## Setup Instructions
1. **Clone the Repository**
   ```bash
   git clone <repository-url>
   cd esp32-tcp-client
   ```

2. **Install PlatformIO**
   Ensure you have PlatformIO installed in your development environment.

3. **Configure WiFi Credentials**
   Edit the `src/secrets.h` file to include your WiFi SSID and password:
   ```cpp
   #define WIFI_SSID "your_ssid"
   #define WIFI_PASSWORD "your_password"
   ```

4. **Build the Project**
   Open the project in PlatformIO and build it to ensure all dependencies are resolved.

5. **Upload to ESP32**
   Connect your ESP32 board and upload the code using PlatformIO.

## Usage
- The application connects to the specified WiFi network and MQTT broker.
- It reads distance measurements from an ultrasonic sensor and publishes the data to the MQTT topic `outTopic2`.
- You can subscribe to the topic to receive distance updates.

## Additional Information
- Ensure that the MQTT broker is accessible and configured to accept connections from your ESP32 device.
- Modify the `src/main.cpp` file to customize the behavior of the application as needed.

## License
This project is licensed under the MIT License - see the LICENSE file for details.