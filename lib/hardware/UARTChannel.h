#ifndef UARTCHANNEL_H
#define UARTCHANNEL_H

#include "ESP32UARTChannel.h"
#include "ACMChannel.h"
#include "TTYChannel.h"
#include "COMChannel.h"

#if defined(ITS_A_UNIX_SYSTEM_I_KNOW_THIS)
using UARTChannel = TTYChannel;
#elif defined(HELLO_IM_A_PC)
using UARTChannel = COMChannel;
#elif defined(ESP_PLATFORM)
using UARTChannel = ACMChannel;
#else
#error "Unknown serial hardware"
#endif

#endif /* UARTCHANNEL_H */
