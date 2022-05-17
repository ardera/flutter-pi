// SPDX-License-Identifier: MIT
/*
 * Tracer - simple object for tracing using flutter event tracing interface.
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_TRACER_H
#define _FLUTTERPI_INCLUDE_TRACER_H

#include <flutter_embedder.h>

struct tracer;

struct tracer *tracer_new_with_cbs(
    FlutterEngineTraceEventDurationBeginFnPtr trace_begin,
    FlutterEngineTraceEventDurationEndFnPtr trace_end,
    FlutterEngineTraceEventInstantFnPtr trace_instant
);

struct tracer *tracer_new_with_stubs();

DECLARE_REF_OPS(tracer);

void __tracer_begin(struct tracer *tracer, const char *name);

void __tracer_end(struct tracer *tracer, const char *name);

void __tracer_instant(struct tracer *tracer, const char *name);

void tracer_set_cbs(
    struct tracer *tracer,
    FlutterEngineTraceEventDurationBeginFnPtr trace_begin,
    FlutterEngineTraceEventDurationEndFnPtr trace_end,
    FlutterEngineTraceEventInstantFnPtr trace_instant
);

#ifdef DEBUG
#define TRACER_BEGIN(tracer, name) __tracer_begin(tracer, name)
#define TRACER_END(tracer, name) __tracer_end(tracer, name)
#define TRACER_INSTANT(tracer, name) __tracer_instant(tracer, name)
#else
#define TRACER_BEGIN(tracer, name) do {} while (0)
#define TRACER_END(tracer, name) do {} while (0)
#define TRACER_INSTANT(tracer, name) do {} while (0)
#endif

#define DECLARE_STATIC_TRACING_CALLS(obj_type_name, obj_var_name) \
static void trace_begin(struct obj_type_name *obj_var_name, const char *name); \
static void trace_end(struct obj_type_name *obj_var_name, const char *name); \
static void trace_instant(struct obj_type_name *obj_var_name, const char *name);

#define DEFINE_STATIC_TRACING_CALLS(obj_type_name, obj_var_name, tracer_member_name) \
static void trace_begin(struct obj_type_name *obj_var_name, const char *name) { \
    return tracer_begin(obj_var_name->tracer_member_name, name); \
} \
static void trace_end(struct obj_type_name *obj_var_name, const char *name) { \
    return tracer_end(obj_var_name->tracer_member_name, name); \
} \
static void trace_instant(struct obj_type_name *obj_var_name, const char *name) { \
    return tracer_instant(obj_var_name->tracer_member_name, name); \
}

#endif // _FLUTTERPI_INCLUDE_TRACER_H
