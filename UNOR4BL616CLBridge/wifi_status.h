#ifndef UNOR4_BL616CL_WIFI_STATUS_H
#define UNOR4_BL616CL_WIFI_STATUS_H

#include <stdint.h>

namespace bridge {

enum class WifiStatus : uint8_t {
    Idle = 0U,
    NoSsidAvailable = 1U,
    ScanCompleted = 2U,
    Connected = 3U,
    ConnectFailed = 4U,
    ConnectionLost = 5U,
    Disconnected = 6U,
    ApListening = 7U,
    ApConnected = 8U,
    ApFailed = 9U,
    NoModule = 255U,
};

/*
 * This is the platform-neutral subset of ESP32-S3 WiFi events that affects
 * the observable GETSTATUS value. The future Bouffalo EV_WIFI adapter owns
 * translating SDK event codes into these events.
 */
enum class WifiStatusEvent : uint8_t {
    InterfaceReady,
    ScanDone,
    StaStarted,
    StaStopped,
    StaConnected,
    StaDisconnected,
    ApStarted,
    ApStopped,
    ApStaConnected,
    ApStaDisconnected,
    Ignored,
};

class WifiStatusTracker {
public:
    WifiStatus status() const;
    void set_status(WifiStatus status);
    void on_event(WifiStatusEvent event);

private:
    WifiStatus status_ = WifiStatus::Idle;
};

const char *wifi_status_text(WifiStatus status);

} // namespace bridge

#endif
