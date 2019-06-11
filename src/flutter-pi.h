#ifndef _FLUTTERPI_H
#define _FLUTTERPI_H

#include <stdint.h>
#include <flutter_embedder.h>

struct LinkedTaskListElement {
    struct LinkedTaskListElement* next;
    FlutterTask task;
    uint64_t target_time;
};

FlutterEngine engine;

#endif