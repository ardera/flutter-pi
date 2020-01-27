#ifndef _SPI_PLUGIN_H
#define _SPI_PLUGIN_H

#include <platformchannel.h>
#include <pluginregistry.h>

#define SPI_PLUGIN_METHOD_CHANNEL "flutter-pi/spi"

int SPIPlugin_onReceive(char *channel, struct ChannelObject *object, FlutterPlatformMessageResponseHandle *responsehandle);

int SPIPlugin_init(void);
int SPIPlugin_deinit(void);

#endif