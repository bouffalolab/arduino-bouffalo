
#include <WiFi.h>

int button_gpio = 33;
int button_state = false;

char *ssid     = "your-ssid";
char *password = "your-password";

void setup() {
    Serial.begin(2000000);
    delay(10);

    // set LED output
    pinMode(LED_BUILTIN, OUTPUT);
    // set input key SW3
    pinMode(button_gpio, INPUT_PULLUP);

    WiFi.begin(ssid, password);
    // Will try for about 10 seconds (20x 500ms)
    int tryDelay = 500;
    int numberOfTries = 20;

    while (true) {
        switch(WiFi.status()) {
            case WL_NO_SSID_AVAIL:
                Serial.println("[WiFi] SSID not found");
                break;
            case WL_CONNECT_FAILED:
                Serial.print("[WiFi] Failed - WiFi not connected! Reason: ");
                return;
                break;
            case WL_CONNECTION_LOST:
                Serial.println("[WiFi] Connection was lost");
                break;
            case WL_SCAN_COMPLETED:
                Serial.println("[WiFi] Scan is completed");
                break;
            case WL_DISCONNECTED:
                Serial.println("[WiFi] WiFi is disconnected");
                break;
            case WL_CONNECTED:
                Serial.println("[WiFi] WiFi is connected!");
                Serial.print("[WiFi] IP address: ");
                // Serial.println(WiFi.localIP());
                return;
                break;
            default:
                Serial.print("[WiFi] WiFi Status: ");
                Serial.println(WiFi.status());
                break;
        }
        delay(tryDelay);

        if(numberOfTries <= 0){
          Serial.print("[WiFi] Failed to connect to WiFi!");
          // Use disconnect function to force stop trying to connect
          WiFi.disconnect();
          return;
        } else {
          numberOfTries--;
        }
    }

}

void loop() {

    button_state = digitalRead(button_gpio);
    if (button_state == LOW)
    {
        // Disconnect from WiFi
        Serial.println("[WiFi] Disconnecting from WiFi!");
        // This function will disconnect and turn off the WiFi (NVS WiFi data is kept)
        WiFi.disconnect();
        Serial.println("[WiFi] Disconnected from WiFi!");
        delay(1000);
    }

    digitalWrite(LED_BUILTIN, HIGH);  // turn the LED on (HIGH is the voltage level)
    delay(1000);
    digitalWrite(LED_BUILTIN, LOW);   // turn the LED off by making the voltage LOW
    delay(1000);

}
