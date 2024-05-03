#include "config.h"

#ifndef HAVE_BUNDLED_CRASHPAD_HANDLER
#error "This file should only be built when using the bundled crashpad handler"
#endif

#include "handler/handler_main.h"

extern "C" {

int crashpad_handler_main(int argc, char **argv) {
    return crashpad::HandlerMain(argc, argv, nullptr);
}

}
