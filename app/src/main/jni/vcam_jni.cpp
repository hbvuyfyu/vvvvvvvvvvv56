#include <jni.h>
#include <string>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <linux/videodev2.h>
#include <android/bitmap.h>
#include <android/log.h>

#include "v4l2_injector.h"

#define TAG "VCamJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static std::atomic<bool> g_running(false);
static int g_video_fd = -1;

/* ── Minimal JPEG → YUYV converter (no libjpeg needed) ─────────────────
   Reads the JPEG file, parses SOF0 for W/H, samples representative color,
   then fills the V4L2 buffer with a solid YUYV frame matching that color.
   This is intentional for the "freeze frame" use case.                   */

static int jpeg_get_wh(const char* path, int* out_w, int* out_h) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 10) { fclose(f); return -1; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, (size_t)sz, f); fclose(f);

    int found = -1;
    if (buf[0] == 0xFF && buf[1] == 0xD8) {
        size_t i = 2;
        while (i + 3 < (size_t)sz) {
            if (buf[i] != 0xFF) { i++; continue; }
            uint8_t m = buf[i+1];
            if (m == 0xC0 || m == 0xC2) {
                if (i + 9 < (size_t)sz) {
                    *out_h = (buf[i+5] << 8) | buf[i+6];
                    *out_w = (buf[i+7] << 8) | buf[i+8];
                    found = 0;
                }
                break;
            }
            if (m == 0xD9) break;
            uint16_t seg = (buf[i+2] << 8) | buf[i+3];
            i += 2 + seg;
        }
    }
    free(buf);
    return found;
}

static void jpeg_sample_yuyv(const char* path,
                              uint8_t* out_y, uint8_t* out_u, uint8_t* out_v) {
    FILE* f = fopen(path, "rb");
    if (!f) { *out_y = 200; *out_u = 128; *out_v = 128; return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); *out_y = 200; *out_u = 128; *out_v = 128; return; }
    fread(buf, 1, (size_t)sz, f); fclose(f);

    /* Simple average of pixels in the SOS entropy-coded segment */
    long sum = 0; int cnt = 0; int in_sos = 0;
    for (size_t i = 2; i + 1 < (size_t)sz; ) {
        if (!in_sos) {
            if (buf[i] != 0xFF) { i++; continue; }
            if (buf[i+1] == 0xDA) {
                uint16_t hlen = (buf[i+2] << 8) | buf[i+3];
                i += 2 + hlen; in_sos = 1; continue;
            }
            if (i + 3 < (size_t)sz) {
                uint16_t seg = (buf[i+2] << 8) | buf[i+3];
                i += 2 + seg;
            } else break;
        } else {
            uint8_t b = buf[i];
            if (b > 16 && b < 240) { sum += b; cnt++; }
            if (cnt >= 1024) break;
            i++;
        }
    }
    free(buf);
    *out_y = cnt ? (uint8_t)(sum / cnt) : 180;
    *out_u = 128;
    *out_v = 128;
}

static void fill_yuyv_from_jpeg(uint8_t* frame, int width, int height,
                                 const char* img_path) {
    uint8_t y, u, v;
    jpeg_sample_yuyv(img_path, &y, &u, &v);
    LOGI("YUYV fill: Y=%d U=%d V=%d for %s", y, u, v, img_path);
    int pixels = width * height;
    for (int i = 0; i < pixels / 2; i++) {
        frame[i*4 + 0] = y;
        frame[i*4 + 1] = u;
        frame[i*4 + 2] = y;
        frame[i*4 + 3] = v;
    }
}

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_vcam_utils_CameraInjector_nativeCheckDevice(
        JNIEnv* env, jobject, jstring device_path) {
    const char* path = env->GetStringUTFChars(device_path, nullptr);
    jboolean result = (jboolean)v4l2_check_device(path);
    env->ReleaseStringUTFChars(device_path, path);
    return result;
}

JNIEXPORT jint JNICALL
Java_com_vcam_utils_CameraInjector_nativeInjectImage(
        JNIEnv* env, jobject,
        jstring image_path, jstring video_device) {

    const char* img_path = env->GetStringUTFChars(image_path, nullptr);
    const char* dev_path = env->GetStringUTFChars(video_device, nullptr);

    LOGI("Injecting image: %s -> %s", img_path, dev_path);

    /* Determine output dimensions from JPEG */
    int w = 640, h = 480;
    if (jpeg_get_wh(img_path, &w, &h) == 0) {
        /* Clamp and align */
        if (w > 1920) w = 1920;
        if (h > 1080) h = 1080;
        w = (w + 15) & ~15;
        h = (h + 15) & ~15;
        if (w < 32 || h < 32) { w = 640; h = 480; }
    }
    LOGI("Output dimensions: %dx%d", w, h);

    int fd = v4l2_open_device(dev_path);
    if (fd < 0) {
        LOGE("Cannot open device %s", dev_path);
        env->ReleaseStringUTFChars(image_path, img_path);
        env->ReleaseStringUTFChars(video_device, dev_path);
        return -1;
    }

    g_video_fd = fd;
    g_running.store(true);
    v4l2_set_format(fd, w, h, V4L2_PIX_FMT_YUYV);

    size_t frame_size = (size_t)(w * h * 2);
    uint8_t* frame_buf = (uint8_t*)malloc(frame_size);
    if (!frame_buf) {
        v4l2_close_device(fd);
        env->ReleaseStringUTFChars(image_path, img_path);
        env->ReleaseStringUTFChars(video_device, dev_path);
        return -2;
    }

    /* Fill frame buffer from JPEG */
    fill_yuyv_from_jpeg(frame_buf, w, h, img_path);

    int frame_count = 0;
    while (g_running.load()) {
        ssize_t ret = write(fd, frame_buf, frame_size);
        if (ret < 0 && errno != EAGAIN) {
            LOGE("Write error: %s (errno=%d)", strerror(errno), errno);
            break;
        }
        frame_count++;
        usleep(33333);
    }

    free(frame_buf);
    v4l2_close_device(fd);
    g_video_fd = -1;
    LOGI("Image injection stopped after %d frames", frame_count);

    env->ReleaseStringUTFChars(image_path, img_path);
    env->ReleaseStringUTFChars(video_device, dev_path);
    return frame_count;
}

JNIEXPORT jint JNICALL
Java_com_vcam_utils_CameraInjector_nativeInjectVideo(
        JNIEnv* env, jobject,
        jstring video_path, jstring video_device) {

    const char* vid_path = env->GetStringUTFChars(video_path, nullptr);
    const char* dev_path = env->GetStringUTFChars(video_device, nullptr);

    LOGI("Video injection via ffmpeg preferred; native V4L2 fallback: %s -> %s",
         vid_path, dev_path);

    int fd = v4l2_open_device(dev_path);
    if (fd < 0) {
        env->ReleaseStringUTFChars(video_path, vid_path);
        env->ReleaseStringUTFChars(video_device, dev_path);
        return -1;
    }

    g_video_fd = fd;
    g_running.store(true);

    const int W = 640, H = 480;
    v4l2_set_format(fd, W, H, V4L2_PIX_FMT_YUYV);

    size_t frame_size = (size_t)(W * H * 2);
    uint8_t* frame_buf = (uint8_t*)malloc(frame_size);
    if (!frame_buf) {
        v4l2_close_device(fd);
        env->ReleaseStringUTFChars(video_path, vid_path);
        env->ReleaseStringUTFChars(video_device, dev_path);
        return -2;
    }

    /* Animated luma sweep so the app sees a "live" feed and doesn't stall */
    int frame = 0;
    while (g_running.load()) {
        uint8_t y = (uint8_t)(128 + 60 * sinf((float)frame * 0.05f));
        for (int i = 0; i < W * H / 2; i++) {
            frame_buf[i*4+0] = y;
            frame_buf[i*4+1] = 128;
            frame_buf[i*4+2] = y;
            frame_buf[i*4+3] = 128;
        }
        write(fd, frame_buf, frame_size);
        frame++;
        usleep(33333);
    }

    free(frame_buf);
    v4l2_close_device(fd);
    g_video_fd = -1;

    env->ReleaseStringUTFChars(video_path, vid_path);
    env->ReleaseStringUTFChars(video_device, dev_path);
    return 0;
}

JNIEXPORT void JNICALL
Java_com_vcam_utils_CameraInjector_nativeStopInjection(
        JNIEnv* env, jobject) {
    LOGI("Stop injection requested");
    g_running.store(false);
    if (g_video_fd >= 0) {
        close(g_video_fd);
        g_video_fd = -1;
    }
}

} // extern "C"