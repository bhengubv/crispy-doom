# Crispy Doom -> native Android game lib (libmain.so). SDL loads it, SDL_main = crispy main().
# LOCAL_PATH points at the crispy-doom repo root (4 dirs up from android/app/jni/src).
LOCAL_PATH := $(call my-dir)/../../../..

include $(CLEAR_VARS)

LOCAL_MODULE := main

# Gather shared engine + doom sources via wildcard, plus a hand-picked opl/pcsound subset.
CRISPY_SRC := \
  $(wildcard $(LOCAL_PATH)/src/*.c) \
  $(wildcard $(LOCAL_PATH)/src/doom/*.c) \
  $(LOCAL_PATH)/opl/opl.c \
  $(LOCAL_PATH)/opl/opl3.c \
  $(LOCAL_PATH)/opl/opl_queue.c \
  $(LOCAL_PATH)/opl/opl_sdl.c \
  $(LOCAL_PATH)/opl/opl_timer.c \
  $(LOCAL_PATH)/pcsound/pcsound.c \
  $(LOCAL_PATH)/pcsound/pcsound_sdl.c \
  $(LOCAL_PATH)/android/jni_extra/android_stubs.c

# Drop platform-specific / desktop-only files that don't belong on Android.
CRISPY_SRC := $(filter-out \
  $(LOCAL_PATH)/src/i_winmusic.c \
  $(LOCAL_PATH)/src/w_file_win32.c \
  $(LOCAL_PATH)/src/i_endoom.c \
  $(LOCAL_PATH)/src/net_gui.c \
  $(LOCAL_PATH)/src/d_dedicated.c \
  $(LOCAL_PATH)/src/z_native.c \
  ,$(CRISPY_SRC))

LOCAL_SRC_FILES := $(patsubst $(LOCAL_PATH)/%,%,$(CRISPY_SRC))

LOCAL_C_INCLUDES := \
  $(LOCAL_PATH)/src \
  $(LOCAL_PATH)/src/doom \
  $(LOCAL_PATH)/opl \
  $(LOCAL_PATH)/pcsound \
  $(LOCAL_PATH)/android/genconfig \
  $(LOCAL_PATH)/android/app/jni/SDL/include \
  $(LOCAL_PATH)/android/app/jni/SDL2_mixer/include \
  $(LOCAL_PATH)/android/app/jni/SDL2_net

LOCAL_CFLAGS := -DHAVE_CONFIG_H -D_DEFAULT_SOURCE \
  -Wno-error=implicit-function-declaration \
  -Wno-implicit-function-declaration

LOCAL_SHARED_LIBRARIES := SDL2 SDL2_mixer SDL2_net

LOCAL_LDLIBS := -lGLESv1_CM -lGLESv2 -lOpenSLES -llog -landroid

include $(BUILD_SHARED_LIBRARY)
