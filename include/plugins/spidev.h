#ifndef _SPI_PLUGIN_H
#define _SPI_PLUGIN_H

#include <platformchannel.h>
#include <pluginregistry.h>

#define SPI_PLUGIN_METHOD_CHANNEL "plugins.flutter.io/spidev"

int spidevp_init(void);
int spidevp_deinit(void);

#endif