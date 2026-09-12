#pragma once
#include "../api.h"
struct tcpip_api_call_data {int reserved;};
err_t tcpip_api_call(err_t (*fn)(tcpip_api_call_data *),tcpip_api_call_data *);
