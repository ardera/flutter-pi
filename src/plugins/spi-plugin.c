#include <sys/select.h>
#include <pthread.h>
#include <linux/spi/spidev.h>
#include <flutter-pi.h>
#include <platformchannel.h>
#include <pluginregistry.h>

#include "spi-plugin.h"

enum spi_task_type {
    kSpiTaskClose,
    kSpiTaskRdMode,
    kSpiTaskWrMode,
    kSpiTaskWrBitsPerWord,
    kSpiTaskRdBitsPerWord,
    kSpiTaskWrMaxSpeedHz,
    kSpiTaskRdMaxSpeedHz,
    kSpiTaskTransmit
};

struct spi_task {
    enum spi_task_type type;
    union {
        uint8_t mode;
        uint8_t bits;
        uint32_t speed;
        struct spi_ioc_transfer transfer;
    };
};

struct spi_task_queue {
    struct spi_task *tasks;
    size_t size_tasks;
    size_t length_tasks;
    pthread_mutex_t queue_mutex;
    pthread_cond_t task_added;
};
#define SPI_TASK_QUEUE_INITIALIZER \
    ((struct spi_task_queue) {.tasks = NULL, .size_tasks = 0, .length_tasks = 0, \
     .queue_mutex = PTHREAD_MUTEX_INITIALIZER, .task_added = PTHREAD_COND_INITIALIZER})

struct {
    fd_set spi_fds;
    struct spi_task_queue queues[FD_SETSIZE];
} spi_plugin = {
    .queues = {SPI_TASK_QUEUE_INITIALIZER}
};

void SPIPlugin_add_task

void *SPIPlugin_spi_loop(void *fd_void) {
    int fd = (int) fd_void;

    while (FD_ISSET(fd, &spi_plugin.spi_fds)) {

    }
}

int SPIPlugin_getFdStrict(struct StdMsgCodecValue *value, int *fd, FlutterPlatformMessageResponseHandle *responsehandle) {
    *fd = -1;

    if (value->type != kInt32) {
        return PlatformChannel_respondError(
            responsehandle,
            kStandardMethodCallResponse,
            "invalidfd",
            "The file-descriptor given to the SPI plugin is not an integer. (int32)",
            NULL
        );
    }

    if ((value->int32_value < 0) || (value->int32_value >= FD_SETSIZE)) {
        return PlatformChannel_respondError(
            responsehandle,
            kStandardMethodCallResponse,
            "invalidfd",
            "The file-descriptor given to the SPI plugin is out of range. Should be 0 <= fd <= " (FD_SETSIZE-1) ".",
            NULL
        );
    }

    if (!FD_ISSET(value->int32_value, &spi_plugin.spi_fds)) {
        return PlatformChannel_respondError(
            responsehandle,
            kStandardMethodCallResponse,
            "invalidfd",
            "The file-descriptor given to the SPI plugin was not opened by the SPI plugin.",
            NULL
        );
    }
    
    *fd = value->int32_value;
    return 0;
}

int SPIPlugin_onReceive(char *channel, struct ChannelObject *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    if STREQ("open", object->method) {
        
    } else if STREQ("setMode", object->method) {
        
    } else if STREQ("setMaxSpeed", object->method) {
        
    } else if STREQ("setWordSize", object->method) {
        
    } else if STREQ("spiFullDuplex", object->method) {
        // we don't yet support full duplex.
        return PlatformChannel_respondNotImplemented(responsehandle);
    } else if STREQ("spiHalfDuplex", object->method) {
        
    } else if STREQ("close", object->method) {
        
    }

    return PlatformChannel_respondNotImplemented(responsehandle);
}

int SPIPlugin_init(void) {
    printf("[spi-plugin] init.\n");
    PluginRegistry_setReceiver(SPI_PLUGIN_METHOD_CHANNEL, kStandardMethodCall, SPIPlugin_onReceive);
}

int SPIPlugin_deinit(void) {
    printf("[spi-plugin] deinit.\n");
}