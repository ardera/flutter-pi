#ifndef _SERVICES_PLUGIN_H
#define _SERVICES_PLUGIN_H

#include <stdio.h>
#include <string.h>

#define ORIENTATION_FROM_STRING(str) \
    (strcmp(str, "DeviceOrientation.portraitUp") == 0 ? kPortraitUp : \
     strcmp(str, "DeviceOrientation.landscapeLeft") == 0 ? kLandscapeLeft :\
     strcmp(str, "DeviceOrientation.portraitDown") == 0 ? kPortraitDown :\
     strcmp(str, "DeviceOrientation.landscapeRight") == 0 ? kLandscapeRight : -1)

#define FLUTTER_NAVIGATION_CHANNEL "flutter/navigation"
#define FLUTTER_ISOLATE_CHANNEL "flutter/isolate"
#define FLUTTER_PLATFORM_CHANNEL "flutter/platform"
#define FLUTTER_ACCESSIBILITY_CHANNEL "flutter/accessibility"
#define FLUTTER_PLATFORM_VIEWS_CHANNEL "flutter/platform_views"

#endif