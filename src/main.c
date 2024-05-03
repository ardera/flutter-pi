
#include <stdbool.h>
#include <string.h>

#include "config.h"

int flutterpi_app_main(int argc, char **argv);
int crashpad_handler_main(int argc, char **argv);

#ifdef HAVE_BUNDLED_CRASHPAD_HANDLER
static bool running_in_crashpad_mode(int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--attachment=FlutterpiCrashpadHandlerMode") == 0) {
            return true;
        }
    }

    return false;
}
#endif

int main(int argc, char **argv) {
#ifdef HAVE_BUNDLED_CRASHPAD_HANDLER
    if (running_in_crashpad_mode(argc, argv)) {
        return crashpad_handler_main(argc, argv);
    }
#endif

    return flutterpi_app_main(argc, argv);
}
