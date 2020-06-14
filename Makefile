CC = cc
LD = cc
REAL_CFLAGS = -I./include $(shell pkg-config --cflags gbm libdrm glesv2 egl libsystemd) \
	-DBUILD_TEXT_INPUT_PLUGIN \
	-DBUILD_ELM327_PLUGIN \
	-DBUILD_GPIOD_PLUGIN \
	-DBUILD_SPIDEV_PLUGIN \
	-DBUILD_TEST_PLUGIN \
	-DBUILD_OMXPLAYER_VIDEO_PLAYER_PLUGIN \
	-ggdb -fsanitize=undefined \
	$(CFLAGS)

REAL_LDFLAGS = $(shell pkg-config --libs gbm libdrm glesv2 egl) \
	-lrt \
	-lflutter_engine \
	-lpthread \
	-ldl \
	-lm \
	$(LDFLAGS)

SOURCES = src/flutter-pi.c \
	src/platformchannel.c \
	src/pluginregistry.c \
	src/console_keyboard.c \
	src/texture_registry.c \
	src/compositor.c \
	src/modesetting.c \
	src/plugins/elm327plugin.c src/plugins/services.c src/plugins/testplugin.c src/plugins/text_input.c \
	src/plugins/raw_keyboard.c src/plugins/gpiod.c src/plugins/spidev.c src/plugins/video_player.c
OBJECTS = $(patsubst src/%.c,out/obj/%.o,$(SOURCES))

all: out/flutter-pi

out/obj/%.o: src/%.c 
	@mkdir -p $(@D)
	$(CC) -c $(REAL_CFLAGS) $(REAL_LDFLAGS) $< -o $@

out/flutter-pi: $(OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(REAL_CFLAGS) $(REAL_LDFLAGS) $(OBJECTS) -o out/flutter-pi

clean:
	@mkdir -p out
	rm -rf $(OBJECTS) out/flutter-pi out/obj/*
