REAL_CFLAGS = -I./include \
	-I./third_party/ \
	$(shell pkg-config --cflags gbm libdrm glesv2 egl libsystemd libinput libudev xkbcommon libusb-1.0 libssl libcrypto) \
	-DBUILD_TEXT_INPUT_PLUGIN \
	-DBUILD_TEST_PLUGIN \
	-DBUILD_OMXPLAYER_VIDEO_PLAYER_PLUGIN \
	-DBUILD_ANDROID_AUTO_PLUGIN \
	-O0 -ggdb -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function \
	$(CFLAGS)

REAL_LDFLAGS = \
	$(shell pkg-config --libs gbm libdrm glesv2 egl libsystemd libinput libudev xkbcommon libusb-1.0 libssl libcrypto) \
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
	src/plugins/omxplayer_video_player.c \
	src/plugins/android_auto/android_auto.c \
	src/plugins/android_auto/aa_xfer.c \
	src/plugins/android_auto/aa_device.c

OBJECTS := $(patsubst src/%.c,out/obj/%.o,$(SOURCES))

AASDK_PROTO_FILES := $(shell find third_party/aasdk -name '*.proto')
AASDK_C_FILES := $(patsubst %.proto,%.pb-c.c,$(AASDK_PROTO_FILES))
AASDK_H_FILES := $(patsubst %.proto,%.pb-c.h,$(AASDK_PROTO_FILES))
AASDK_O_FILES := $(patsubst %.proto,%.pb-c.o,$(AASDK_PROTO_FILES))

all: out/flutter-pi

$(AASDK_C_FILES) $(AASDK_H_FILES) &: $(AASDK_PROTO_FILES)
	protoc-c --c_out=third_party/aasdk -Ithird_party/aasdk $(AASDK_PROTO_FILES)

third_party/aasdk/%.o: third_party/aasdk/%.c
	$(CC) -c -O3 $(shell pkg-config --cflags 'libprotobuf-c >= 1.0.0') $< -o $@

third_party/aasdk/libaaproto_unbundled.a: $(AASDK_O_FILES)
	$(AR) -rcs third_party/aasdk/libaaproto_unbundled.a $(AASDK_O_FILES)

third_party/aasdk/libaaproto.a: third_party/aasdk/libaaproto_unbundled.a
	@printf "CREATE third_party/aasdk/libaaproto.a\nADDLIB third_party/aasdk/libaaproto_unbundled.a\nADDLIB $(shell locate libprotobuf-c.a)\nSAVE\nEND" | $(AR) -M

out/obj/%.o: src/%.c $(AASDK_H_FILES)
	@mkdir -p $(@D)
	$(CC) -c $(REAL_CFLAGS) $< -o $@

out/flutter-pi: $(OBJECTS) third_party/aasdk/libaaproto.a
	@mkdir -p $(@D)
	$(CC) $(REAL_LDFLAGS) $(OBJECTS) third_party/aasdk/libaaproto.a -o out/flutter-pi

clean:
	@mkdir -p out
	@rm -rf $(OBJECTS) out/flutter-pi out/obj/* $(AASDK_C_FILES) $(AASDK_H_FILES) $(AASDK_O_FILES) third_party/aasdk/libaaproto_unbundled.a third_party/aasdk/libaaproto.a