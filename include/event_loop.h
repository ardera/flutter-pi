#ifndef _FLUTTERPI_INCLUDE_EVENT_LOOP_H_
#define _FLUTTERPI_INCLUDE_EVENT_LOOP_H_

#include <collection.h>

struct sd_event;
struct sd_event_source;
struct sd_event_source_generic;
typedef struct sd_event_source_generic sd_event_source_generic;

typedef int (*sd_event_generic_handler_t)(sd_event_source_generic *source, void *argument, void *userdata);

DECLARE_REF_OPS(sd_event_source_generic);

struct sd_event_source_generic *sd_event_add_generic(struct sd_event *event, sd_event_generic_handler_t handler, void *userdata);

void sd_event_source_generic_signal(struct sd_event_source_generic *source, void *argument);

void *sd_event_source_generic_set_userdata(struct sd_event_source_generic *source, void *userdata);

void *sd_event_source_generic_get_userdata(struct sd_event_source_generic *source);


#endif