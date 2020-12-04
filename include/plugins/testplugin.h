#ifndef _TEST_PLUGIN_H
#define _TEST_PLUGIN_H

#include <stdio.h>
#include <string.h>

#define TESTPLUGIN_CHANNEL_JSON "flutter-pi/testjson"
#define TESTPLUGIN_CHANNEL_STD "flutter-pi/teststd"
#define TESTPLUGIN_CHANNEL_PING "flutter-pi/ping"

extern int testp_init(struct flutterpi *flutterpi, void **userdata);
extern int testp_deinit(struct flutterpi *flutterpi, void **userdata);

#endif