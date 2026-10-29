#include "wifi_status.h"

namespace bridge {

WifiStatus WifiStatusTracker::status() const
{
    return status_;
}

void WifiStatusTracker::set_status(WifiStatus status)
{
    status_ = status;
}

void WifiStatusTracker::on_event(WifiStatusEvent event)
{
    switch (event) {
    case WifiStatusEvent::InterfaceReady:
    case WifiStatusEvent::StaStarted:
    case WifiStatusEvent::StaDisconnected:
    case WifiStatusEvent::ApStopped:
        status_ = WifiStatus::Idle;
        break;
    case WifiStatusEvent::ScanDone:
        status_ = WifiStatus::ScanCompleted;
        break;
    case WifiStatusEvent::StaStopped:
        status_ = WifiStatus::NoModule;
        break;
    case WifiStatusEvent::StaConnected:
        status_ = WifiStatus::Connected;
        break;
    case WifiStatusEvent::ApStarted:
        status_ = WifiStatus::ApListening;
        break;
    case WifiStatusEvent::ApStaConnected:
        status_ = WifiStatus::ApConnected;
        break;
    case WifiStatusEvent::ApStaDisconnected:
    case WifiStatusEvent::Ignored:
        break;
    }
}

const char *wifi_status_text(WifiStatus status)
{
    switch (status) {
    case WifiStatus::Idle:
        return "0";
    case WifiStatus::NoSsidAvailable:
        return "1";
    case WifiStatus::ScanCompleted:
        return "2";
    case WifiStatus::Connected:
        return "3";
    case WifiStatus::ConnectFailed:
        return "4";
    case WifiStatus::ConnectionLost:
        return "5";
    case WifiStatus::Disconnected:
        return "6";
    case WifiStatus::ApListening:
        return "7";
    case WifiStatus::ApConnected:
        return "8";
    case WifiStatus::ApFailed:
        return "9";
    case WifiStatus::NoModule:
        return "255";
    }

    return "255";
}

} // namespace bridge
