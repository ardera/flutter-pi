#include "plugins/printing.h"
#include "flutter-pi.h"
#include "pluginregistry.h"
#include "util/logging.h"
#include <MagickWand/MagickWand.h>

static void on_page_raster_end(int64_t job, char* error) {
    struct std_value response = STDMAP1(STDSTRING("job"), STDINT32(job));
    if (error != NULL) {
        response = STDMAP2(STDSTRING("job"), STDINT32(job), STDSTRING("error"), STDSTRING(error));

        LOG_ERROR("%s\n", error);
    }

    platch_call_std(PRINTING_CHANNEL, "onPageRasterEnd", &response, NULL, NULL);
}

static void on_page_rasterized(int64_t job, const uint8_t* data, size_t size, int width, int height) {
    struct std_value image = (struct std_value){ .type = kStdUInt8Array, .uint8array  = data, .size = size };

    struct std_value response = STDMAP4(
        STDSTRING("image"),
        image,
        STDSTRING("width"),
        STDINT32(width),
        STDSTRING("height"),
        STDINT32(height),
        STDSTRING("job"),
        STDINT32(job)
    );

    platch_call_std(PRINTING_CHANNEL, "onPageRasterized", &response, NULL, NULL);
}

static void raster_pdf(const uint8_t *data, size_t size, const int32_t *pages, size_t pages_count, double scale, int64_t job) {
    MagickWand *wand = NULL;
    PixelWand *color = NULL;

    int width, height;
    
    MagickWandGenesis();
    
    wand = NewMagickWand();

    color = NewPixelWand();
    PixelSetColor(color, "white");

    MagickBooleanType result = MagickReadImageBlob(wand, data, size);
    if(result != MagickTrue) {
        on_page_raster_end(job, "Cannot read images from PDF blob.");
        return;
    }

    MagickResetIterator(wand);

    bool all_pages = false;
    if (pages_count == 0) {
        all_pages = true;
        pages_count = MagickGetNumberImages(wand);
    }

    int current_page = 0;
    while(MagickNextImage(wand) != MagickFalse) {
        if(!all_pages){
            bool shouldRasterize = false;

            //Check if current page is set to be rasterized
            for(size_t pn = 0; pn < pages_count; pn++) {
                if(pages[pn] == current_page) {
                    shouldRasterize = true;
                    break;
                }
            }

            if(!shouldRasterize) {
                current_page++;
                continue;
            }
        }

        // Get the image's width and height
        width = MagickGetImageWidth(wand);
        height = MagickGetImageHeight(wand);

        int32_t bWidth = width * scale;
        int32_t bHeight = height * scale;

        MagickResizeImage(wand, bWidth, bHeight, LanczosFilter);
        MagickSetImageFormat(wand, "bmp");

        size_t page_size;
        uint8_t *page_data = MagickGetImageBlob(wand, &page_size);

        on_page_rasterized(job, page_data, page_size, bWidth, bHeight);

        MagickRelinquishMemory(page_data);

        current_page++;
    }

    /* Clean up */
    if(wand){
        wand = DestroyMagickWand(wand);
    }
    
    if(color){
        color = DestroyPixelWand(color);
    }

    MagickWandTerminus();

    on_page_raster_end(job, NULL);
}

static int on_raster_pdf(struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    struct std_value *args, *tmp;
    const uint8_t *data;
    size_t data_length;
    double scale;
    int64_t job;

    args = &object->std_arg;

    if (args == NULL || !STDVALUE_IS_MAP(*args)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `arg` to be a map.");
    }

    tmp = stdmap_get_str(&object->std_arg, "doc");
    if (tmp == NULL || (*tmp).type != kStdUInt8Array ) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `arg['doc'] to be a uint8_t list.");
    }

    data = tmp->uint8array;
    data_length = tmp->size;

    int32_t* pages;
    size_t pages_count;
    tmp = stdmap_get_str(&object->std_arg, "pages");
    if (tmp != NULL || STDVALUE_IS_LIST(*tmp)) {
        pages_count = tmp->size;
        pages = (int32_t*)malloc(sizeof(int32_t) * pages_count);
        for (size_t n = 0; n < pages_count; n++) {
            struct std_value page = tmp->list[n];

            if(!STDVALUE_IS_INT(page)){
                continue;
            }

            pages[n] = page.int32_value;
        }
    }

    tmp = stdmap_get_str(&object->std_arg, "scale");
    if (tmp == NULL || !STDVALUE_IS_FLOAT(*tmp)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `arg['scale'] to be a double.");
    }

    scale = STDVALUE_AS_FLOAT(*tmp);

    tmp = stdmap_get_str(&object->std_arg, "job");
    if (tmp == NULL || !STDVALUE_IS_INT(*tmp)) {
        return platch_respond_illegal_arg_std(response_handle, "Expected `arg['job'] to be an int.");
    }

    job = STDVALUE_AS_INT(*tmp);

    //Rasterize
    raster_pdf(data, data_length, pages, pages_count, scale, job);

    free(pages);

    return platch_respond(
        response_handle,
        &(struct platch_obj){ .codec = kStandardMethodCallResponse, .success = true, .std_result = { .type = kStdTrue } }
    );
}

static int on_printing_info(struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {  
    (void) object;

    return platch_respond(
        response_handle,
        &PLATCH_OBJ_STD_MSG(STDMAP6(
            STDSTRING("canPrint"),
            STDBOOL(false),
            STDSTRING("canShare"),
            STDBOOL(false),
            STDSTRING("canRaster"),
            STDBOOL(true),
            STDSTRING("canListPrinters"),
            STDBOOL(false),
            STDSTRING("directPrint"),
            STDBOOL(false),
            STDSTRING("dynamicLayout"),
            STDBOOL(false)
        ))
    );
}

static int on_receive(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *response_handle) {
    (void) channel;

    const char *method;
    method = object->method;

    if (streq(method, "printingInfo")) {
        return on_printing_info(object, response_handle);
    } else if (streq(method, "rasterPdf")) {
        return on_raster_pdf(object, response_handle);
    }

    return platch_respond_not_implemented(response_handle);
}

enum plugin_init_result printing_init(struct flutterpi *flutterpi, void **userdata_out) {
    (void) flutterpi;

    int ok;

    ok = plugin_registry_set_receiver_locked(PRINTING_CHANNEL, kStandardMethodCall, on_receive);
    if (ok != 0) {
        return PLUGIN_INIT_RESULT_ERROR;
    }

    *userdata_out = NULL;

    return PLUGIN_INIT_RESULT_INITIALIZED;
}

void printing_deinit(struct flutterpi *flutterpi, void *userdata) {
    (void) userdata;

    plugin_registry_remove_receiver_v2_locked(flutterpi_get_plugin_registry(flutterpi), PRINTING_CHANNEL);
}

FLUTTERPI_PLUGIN("printing plugin", printing_plugin, printing_init, printing_deinit)