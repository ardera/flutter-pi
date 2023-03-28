// SPDX-License-Identifier: MIT
/*
 * Tracer - simple event tracing based on flutter event tracing interface
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#include <stdlib.h>

#include <flutter_embedder.h>

#include <collection.h>
#include <tracer.h>

FILE_DESCR("tracing")

struct tracer {
    refcount_t n_refs;
    atomic_bool has_cbs;
    FlutterEngineTraceEventDurationBeginFnPtr trace_begin;
    FlutterEngineTraceEventDurationEndFnPtr trace_end;
    FlutterEngineTraceEventInstantFnPtr trace_instant;

    atomic_bool logged_discarded_events;
};

struct tracer *tracer_new_with_cbs(
    FlutterEngineTraceEventDurationBeginFnPtr trace_begin,
    FlutterEngineTraceEventDurationEndFnPtr trace_end,
    FlutterEngineTraceEventInstantFnPtr trace_instant
) {
    struct tracer *tracer;
    
    tracer = malloc(sizeof *tracer);
    if (tracer == NULL) {
        goto fail_return_null;
    }

    tracer->n_refs = REFCOUNT_INIT_1;
    tracer->has_cbs = true;
    tracer->trace_begin = trace_begin;
    tracer->trace_end = trace_end;
    tracer->trace_instant = trace_instant;
    tracer->logged_discarded_events = false;
    return tracer;


    fail_return_null:
    return NULL;
}

struct tracer *tracer_new_with_stubs() {
    struct tracer *tracer;
    
    tracer = malloc(sizeof *tracer);
    if (tracer == NULL) {
        goto fail_return_null;
    }

    tracer->n_refs = REFCOUNT_INIT_1;
    tracer->has_cbs = false;
    tracer->trace_begin = NULL;
    tracer->trace_end = NULL;
    tracer->trace_instant = NULL;
    tracer->logged_discarded_events = false;
    return tracer;


    fail_return_null:
    return NULL;
}

void tracer_set_cbs(
    struct tracer *tracer,
    FlutterEngineTraceEventDurationBeginFnPtr trace_begin,
    FlutterEngineTraceEventDurationEndFnPtr trace_end,
    FlutterEngineTraceEventInstantFnPtr trace_instant
) {
    tracer->trace_begin = trace_begin;
    tracer->trace_end = trace_end;
    tracer->trace_instant = trace_instant;

    bool already_set_before = atomic_exchange(&tracer->has_cbs, true);
    DEBUG_ASSERT_MSG(!already_set_before, "tracing callbacks can only be set once.");
    (void) already_set_before;
}

void tracer_destroy(struct tracer *tracer) {
    free(tracer);
}

DEFINE_REF_OPS(tracer, n_refs)

static void log_discarded_event(struct tracer *tracer, const char *name) {
    (void) name;
    if (atomic_exchange(&tracer->logged_discarded_events, true) == false) {
        LOG_DEBUG("Tracing event was discarded because tracer not initialized yet: %s. This message will only be logged once.\n", name);
    }
}

void __tracer_begin(struct tracer *tracer, const char *name) {
    DEBUG_ASSERT_NOT_NULL(tracer);
    DEBUG_ASSERT_NOT_NULL(name);
    if (atomic_load(&tracer->has_cbs)) {
        tracer->trace_begin(name);
    } else {
        log_discarded_event(tracer, name);
    }
}

void __tracer_end(struct tracer *tracer, const char *name) {
    DEBUG_ASSERT_NOT_NULL(tracer);
    DEBUG_ASSERT_NOT_NULL(name);
    if (atomic_load(&tracer->has_cbs)) {
        tracer->trace_end(name);
    } else {
        log_discarded_event(tracer, name);
    }
}

void __tracer_instant(struct tracer *tracer, const char *name) {
    DEBUG_ASSERT_NOT_NULL(tracer);
    DEBUG_ASSERT_NOT_NULL(name);
    if (atomic_load(&tracer->has_cbs)) {
        tracer->trace_instant(name);
    } else {
        log_discarded_event(tracer, name);
    }
}
