/*
WiFi.cpp - WiFi library for BouffaloLab

Copyright (c) 2023 BH6BAO <qqwang@bouffalolab.com>
Copyright (c) 2023 Bouffalo Lab Intelligent Technology (Nanjing) Co., Ltd. All rights reserved.

*/

#include "WiFi.h"

extern "C" {
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
}

void WiFiClass::enableProv(bool status)
{
    prov_enable = status;
}

bool WiFiClass::isProvEnabled()
{
    return prov_enable;
}

WiFiClass WiFi;
