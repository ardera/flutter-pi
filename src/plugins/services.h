#ifndef _SERVICES_PLUGIN_H
#define _SERVICES_PLUGIN_H

#include <stdio.h>
#include <string.h>

#define ORIENTATION_FROM_STRING(str)                                    \
    (streq(str, "DeviceOrientation.portraitUp")     ? kPortraitUp :     \
     streq(str, "DeviceOrientation.landscapeLeft")  ? kLandscapeLeft :  \
     streq(str, "DeviceOrientation.portraitDown")   ? kPortraitDown :   \
     streq(str, "DeviceOrientation.landscapeRight") ? kLandscapeRight : \
                                                      -1)

#define FLUTTER_NAVIGATION_CHANNEL "flutter/navigation"
#define FLUTTER_ISOLATE_CHANNEL "flutter/isolate"
#define FLUTTER_PLATFORM_CHANNEL "flutter/platform"
#define FLUTTER_ACCESSIBILITY_CHANNEL "flutter/accessibility"
#define FLUTTER_PLATFORM_VIEWS_CHANNEL "flutter/platform_views"
#define FLUTTER_MOUSECURSOR_CHANNEL "flutter/mousecursor"

#endif
