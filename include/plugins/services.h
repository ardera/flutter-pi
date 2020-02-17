#ifndef _SERVICES_PLUGIN_H
#define _SERVICES_PLUGIN_H

#include <stdio.h>
#include <string.h>

#define ORIENTATION_FROM_STRING(str) \
    (strcmp(str, "DeviceOrientation.portraitUp") == 0 ? kPortraitUp : \
     strcmp(str, "DeviceOrientation.landscapeLeft") == 0 ? kLandscapeLeft :\
     strcmp(str, "DeviceOrientation.portraitDown") == 0 ? kPortraitDown :\
     strcmp(str, "DeviceOrientation.landscapeRight") == 0 ? kLandscapeRight : -1)

int services_init(void);
int services_deinit(void);

#endif