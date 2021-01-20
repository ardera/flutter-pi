#ifndef _VIDEO_OUTPUT_H_
#define _VIDEO_OUTPUT_H_

#include <stdbool.h>

struct video_output;

bool video_output_is_kms(struct video_output *vout);



#endif