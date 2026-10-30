#ifndef UNOR4_BL616CL_BOUFFALO_WIFI_BACKEND_H
#define UNOR4_BL616CL_BOUFFALO_WIFI_BACKEND_H

#include "wifi_service.h"

namespace bridge {

const WifiBackend &bouffalo_wifi_backend();
bool bouffalo_wifi_ap_ready();

} // namespace bridge

#endif
