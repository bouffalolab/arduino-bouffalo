// Local UART timings only: no USB output inside the measured sample loops.
#include <WiFiS3.h>
#include <Modem.h>
#include <string.h>
#if defined(AT_TIMING_SCAN_WINDOW)
#include <ArduinoBLE.h>
#endif

static ModemClass timing_modem(&Serial2);
static const char *commands[] = {"AT\r\n", "AT+FWVERSION?\r\n", "AT+GETSTATUS?\r\n"};
static const char *prompts[] = {DO_NOT_CHECK_CMD, PROMPT(_FWVERSION), PROMPT(_GETSTATUS)};
static const unsigned kRounds = 20;
struct Sample { uint32_t write_us, first_us, total_us; uint16_t bytes; bool ok; };
static Sample samples[3][3][kRounds];

static Sample raw_sample(const char *command, unsigned index) {
  Sample sample = {};
  char response[64] = {};
  const uint32_t start = micros();
  const size_t written = Serial2.write((const uint8_t *)command, strlen(command));
  sample.write_us = micros() - start;
  while ((uint32_t)(micros() - start) < 10000000UL) {
    if (!Serial2.available()) continue;
    const int value = Serial2.read();
    if (value < 0) continue;
    if (!sample.bytes) sample.first_us = micros() - start;
    if (sample.bytes >= sizeof(response) - 1) break;
    response[sample.bytes++] = (char)value;
    if ((sample.bytes >= 4 && !memcmp(response + sample.bytes - 4, "OK\r\n", 4)) ||
        (index >= 3 && sample.bytes == 7 && !strcmp(response, "ERROR\r\n"))) {
      sample.total_us = micros() - start;
      sample.ok = written == strlen(command) &&
          (index == 0 ? !strcmp(response, "OK\r\n") :
           index == 1 ? !strcmp(response, "+FWVERSION: 0.6.0\r\nOK\r\n") :
           index == 2 ? (!strcmp(response, "+GETSTATUS: 0\r\nOK\r\n") ||
            !strcmp(response, "+GETSTATUS: 2\r\nOK\r\n")) : !strcmp(response, "ERROR\r\n"));
      return sample;
    }
  }
  sample.total_us = micros() - start;
  return sample;
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200);
  timing_modem.timeout(10000);
  delay(3000);
  Serial.println("RA4_AT_TIMING:READY");
}

void loop() {
  static bool finished = false;
  if (finished || !Serial.available()) return;
  if (Serial.read() != 'S') return;
  finished = true;
  bool passed = true;
#if defined(AT_TIMING_SCAN_WINDOW)
  const unsigned modes = 3;
  passed = BLE.begin();
  for (unsigned mode = 0; mode < modes && passed; ++mode) {
    if (mode == 1) passed = BLE.scan();
    else BLE.stopScan();
    // Let the requested controller transition finish before the measured loop.
    for (unsigned i = 0; i < 20; ++i) { BLE.poll(); delay(5); }
    for (unsigned command = 0; command < 3 && passed; ++command) {
      for (unsigned round = 0; round < kRounds; ++round) {
        const char *request = command == 0 ? "AT\r\n" : command == 1 ?
            "AT+SSLAVAILABLE=2000\r\n" : "AT+SSLPEEK=2000\r\n";
        samples[mode][command][round] = raw_sample(request, command == 0 ? 0 : 3);
        if (!samples[mode][command][round].ok) { passed = false; break; }
      }
    }
  }
  BLE.stopScan();
  BLE.end();
#else
  const unsigned modes = 2;
  for (unsigned mode = 0; mode < 2 && passed; ++mode) {
    for (unsigned command = 0; command < 3 && passed; ++command) {
      for (unsigned round = 0; round < kRounds; ++round) {
        Sample &sample = samples[mode][command][round];
        if (!mode) {
          sample = raw_sample(commands[command], command);
        } else {
          std::string response;
          const uint32_t start = micros();
          sample.ok = timing_modem.write(prompts[command], response, "%s", commands[command]);
          sample.total_us = micros() - start;
          sample.bytes = response.size();
          sample.ok = sample.ok && (command == 0 ? response.empty() :
              command == 1 ? response == "0.6.0" : (response == "0" || response == "2"));
        }
        if (!sample.ok) { passed = false; break; }
      }
    }
  }
#endif
  for (unsigned mode = 0; mode < modes; ++mode)
    for (unsigned command = 0; command < 3; ++command)
      for (unsigned round = 0; round < kRounds; ++round) {
        const Sample &s = samples[mode][command][round];
        Serial.print("RA4_AT_TIMING:SAMPLE="); Serial.print(mode); Serial.print(',');
        Serial.print(command); Serial.print(','); Serial.print(round); Serial.print(',');
        Serial.print(s.write_us); Serial.print(','); Serial.print(s.first_us); Serial.print(',');
        Serial.print(s.total_us); Serial.print(','); Serial.print(s.bytes); Serial.print(',');
        Serial.println(s.ok ? 1 : 0);
      }
  Serial.println(passed ? "RA4_AT_TIMING:RESULT=PASS" : "RA4_AT_TIMING:RESULT=FAIL");
}
