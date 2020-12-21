#ifndef RENDERER_H_
#define RENDERER_H_

#define LOG_RENDERER_ERROR(...) fprintf(stderr, "[flutter-pi renderer] " __VA_ARGS__)

typedef void (*frame_start_callback_t)(void *userdata);

struct renderer;


#endif RENDERER_H