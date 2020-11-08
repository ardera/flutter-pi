REAL_CFLAGS = -I./include $(shell pkg-config --cflags gbm libdrm glesv2 egl libsystemd libinput libudev xkbcommon) \
	-DBUILD_TEXT_INPUT_PLUGIN \
	-DBUILD_TEST_PLUGIN \
	-DBUILD_OMXPLAYER_VIDEO_PLAYER_PLUGIN \
	-O0 -ggdb \
	$(CFLAGS)

REAL_LDFLAGS = \
	$(shell pkg-config --libs gbm libdrm glesv2 egl libsystemd libinput libudev xkbcommon) \
	-lrt \
	-lpthread \
	-ldl \
	-lm \
	-rdynamic \
	$(LDFLAGS)

SOURCES = src/flutter-pi.c \
	src/platformchannel.c \
	src/pluginregistry.c \
	src/texture_registry.c \
	src/compositor.c \
	src/modesetting.c \
	src/collection.c \
	src/cursor.c \
	src/keyboard.c \
	src/plugins/services.c \
	src/plugins/testplugin.c \
	src/plugins/text_input.c \
	src/plugins/raw_keyboard.c \
	src/plugins/omxplayer_video_player.c

OBJECTS = $(patsubst src/%.c,out/obj/%.o,$(SOURCES))

all: out/flutter-pi

out/obj/%.o: src/%.c 
	@mkdir -p $(@D)
	$(CC) -c $(REAL_CFLAGS) $< -o $@

out/flutter-pi: $(OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(REAL_CFLAGS) $(OBJECTS) $(REAL_LDFLAGS) -o out/flutter-pi

clean:
	@mkdir -p out
	rm -rf $(OBJECTS) out/flutter-pi out/obj/*
