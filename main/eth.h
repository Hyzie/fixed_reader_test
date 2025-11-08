#ifndef ETH_H
#define ETH_H

#include "uart.h"
#include <stdbool.h>

void eth_init(void);
bool eth_is_connected(void);

#endif // ETH_H
