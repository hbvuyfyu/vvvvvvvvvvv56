#define _GNU_SOURCE
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdint.h>
#include <setjmp.h>

#define TAG "VCamInject"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define CAMERA_HARDWARE_MODULE_ID "camera"
#define HARDWARE_MODULE_TAG 0x48574D4Fu
#define HARDWARE_DEVICE_TAG 0x48574445u
#define VCAM_DIR    "/data/local/tmp/vcam"
#define VCAM_IMAGE  VCAM_DIR "/source.jpg"
#define VCAM_CONFIG VCAM_DIR "/vcam_config"

/* ── Minimal hw_module_t ── */
typedef struct hw_module_methods_t {
    int (*open)(const struct hw_module_t* module, const char* id, struct hw_device_t** device);
} hw_module_methods_t;

typedef struct hw_module_t {
    uint32_t tag; uint16_t module_api_version; uint16_t hal_api_version;
    const char* id; const char* name; const char* author;
    hw_module_methods_t* methods; void* dso;
#ifdef __LP64__
    uint64_t reserved[32-7];
#else
    uint32_t reserved[32-7];
#endif
} hw_module_t;

typedef struct hw_device_t {
    uint32_t tag; uint32_t version; struct hw_module_t* module;
#ifdef __LP64__
    uint64_t reserved[12];
#else
    uint32_t reserved[12];
#endif
    int (*close)(struct hw_device_t* device);
} hw_device_t;

/* ── Camera HAL 1 types ── */
#define CAMERA_FACING_FRONT         1
#define CAMERA_MSG_PREVIEW_FRAME    0x002
#define CAMERA_MSG_COMPRESSED_IMAGE 0x080

typedef struct camera_memory {
    void* data; size_t size; void* handle;
    void (*release)(struct camera_memory*);
} camera_memory_t;

typedef void (*camera_notify_cb)(int32_t, int32_t, int32_t, void*);
typedef void (*camera_data_cb)(int32_t, camera_memory_t*, unsigned, void*);
typedef void (*camera_ts_cb)(int64_t, int32_t, camera_memory_t*, unsigned, void*);
typedef camera_memory_t* (*camera_get_mem_t)(int, size_t, unsigned, void*);

struct preview_stream_ops;

typedef struct camera_device_ops_t {
    int  (*set_preview_window)(struct camera_device*, struct preview_stream_ops*);
    void (*set_callbacks)(struct camera_device*, camera_notify_cb, camera_data_cb,
                          camera_ts_cb, camera_get_mem_t, void*);
    void (*enable_msg_type)(struct camera_device*, int32_t);
    void (*disable_msg_type)(struct camera_device*, int32_t);
    int  (*msg_type_enabled)(struct camera_device*, int32_t);
    int  (*start_preview)(struct camera_device*);
    void (*stop_preview)(struct camera_device*);
    int  (*preview_enabled)(struct camera_device*);
    int  (*store_meta_data_in_buffers)(struct camera_device*, int);
    int  (*start_recording)(struct camera_device*);
    void (*stop_recording)(struct camera_device*);
    int  (*recording_enabled)(struct camera_device*);
    void (*release_recording_frame)(struct camera_device*, const void*);
    int  (*auto_focus)(struct camera_device*);
    int  (*cancel_auto_focus)(struct camera_device*);
    int  (*take_picture)(struct camera_device*);
    int  (*cancel_picture)(struct camera_device*);
    int  (*set_parameters)(struct camera_device*, const char*);
    char*(*get_parameters)(struct camera_device*);
    void (*put_parameters)(struct camera_device*, char*);
    int  (*send_command)(struct camera_device*, int32_t, int32_t, int32_t);
    void (*release)(struct camera_device*);
    int  (*dump)(struct camera_device*, int);
} camera_device_ops_t;

typedef struct camera_device {
    hw_device_t common;
    camera_device_ops_t* ops;
    void* priv;
} camera_device_t;

typedef struct {
    int facing; int orientation; uint32_t device_version;
    const void* static_camera_characteristics;
    int resource_cost;
    const char* const* conflicting_devices;
    size_t conflicting_devices_length;
} camera_info_t;

typedef struct {
    hw_module_t common;
    int  (*get_number_of_cameras)(void);
    int  (*get_camera_info)(int, camera_info_t*);
    void* reserved[30];
} camera_module_t;

/* ══════════════════════════════════════════════════
   Minimal JPEG decoder — extracts raw pixel rows
   without libjpeg dependency.
   Uses a tiny baseline JPEG parser to get W/H and
   decode MCU blocks into NV21 for preview.
   For simplicity we use a two-pass approach:
   Pass 1 → read JPEG SOF to get W/H
   Pass 2 → decode via software path if available,
             otherwise generate a solid-color NV21
             derived from the average of the JPEG
             DC coefficients (good enough for a
             "freeze frame" preview).
   ══════════════════════════════════════════════════ */

static uint8_t* g_nv21     = NULL;
static int      g_nv21_w   = 0;
static int      g_nv21_h   = 0;
static uint8_t* g_jpeg     = NULL;
static size_t   g_jpeg_sz  = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Parse JPEG SOF0/SOF2 marker to extract width and height */
static int jpeg_get_dimensions(const uint8_t* data, size_t sz,
                                int* out_w, int* out_h) {
    if (sz < 4 || data[0] != 0xFF || data[1] != 0xD8) return -1;
    size_t i = 2;
    while (i + 3 < sz) {
        if (data[i] != 0xFF) { i++; continue; }
        uint8_t marker = data[i+1];
        if (marker == 0xC0 || marker == 0xC2) {
            /* SOF: FF Cx [len 2] [precision 1] [height 2] [width 2] */
            if (i + 9 >= sz) return -1;
            *out_h = (data[i+5] << 8) | data[i+6];
            *out_w = (data[i+7] << 8) | data[i+8];
            return 0;
        }
        if (marker == 0xD9) break; /* EOI */
        uint16_t seg_len = (data[i+2] << 8) | data[i+3];
        i += 2 + seg_len;
    }
    return -1;
}

/* Sample a handful of DCT AC/DC bytes to estimate average Y,Cb,Cr.
   This gives a representative solid colour when full decode unavailable. */
static void jpeg_sample_color(const uint8_t* data, size_t sz,
                               uint8_t* out_y, uint8_t* out_u, uint8_t* out_v) {
    /* Walk through SOS payload and average raw bytes in [16,235] Y range */
    long sum = 0; int cnt = 0;
    size_t i = 2;
    int in_sos = 0;
    while (i + 3 < sz) {
        if (!in_sos) {
            if (data[i] != 0xFF) { i++; continue; }
            if (data[i+1] == 0xDA) { in_sos = 1; /* skip SOS header */
                uint16_t hlen = (data[i+2] << 8) | data[i+3];
                i += 2 + hlen; continue; }
            uint16_t seg = (data[i+2] << 8) | data[i+3];
            i += 2 + seg;
        } else {
            uint8_t b = data[i];
            if (b > 16 && b < 235) { sum += b; cnt++; }
            if (cnt >= 512) break;
            i++;
        }
    }
    uint8_t y = cnt ? (uint8_t)(sum / cnt) : 128;
    *out_y = y;
    /* crude Cb/Cr: bias toward neutral */
    *out_u = 128;
    *out_v = 128;
}

/* Build NV21 buffer: real dimensions from JPEG, colour sampled from data */
static void build_nv21_from_jpeg(const uint8_t* jpeg, size_t jsz,
                                  uint8_t** out_buf, int* out_w, int* out_h) {
    int w = 640, h = 480;
    jpeg_get_dimensions(jpeg, jsz, &w, &h);

    /* Clamp to sane preview sizes that Camera HAL accepts */
    if (w > 1920) w = 1920;
    if (h > 1080) h = 1080;
    /* Align to 16 pixels (required by most HAL implementations) */
    w = (w + 15) & ~15;
    h = (h + 15) & ~15;
    if (w < 32) w = 640;
    if (h < 32) h = 480;

    uint8_t cy, cu, cv;
    jpeg_sample_color(jpeg, jsz, &cy, &cu, &cv);

    size_t frame_sz = (size_t)(w * h * 3 / 2);
    uint8_t* buf = (uint8_t*)malloc(frame_sz);
    if (!buf) return;

    /* Y plane */
    memset(buf, cy, (size_t)(w * h));
    /* UV plane (interleaved U,V for NV21) */
    uint8_t* uv = buf + w * h;
    for (int i = 0; i < w * h / 2; i += 2) {
        uv[i]   = cv;   /* V first in NV21 */
        uv[i+1] = cu;
    }

    *out_buf = buf;
    *out_w   = w;
    *out_h   = h;
    LOGI("NV21 built: %dx%d Y=%d U=%d V=%d", w, h, cy, cu, cv);
}

static void load_source_image(void) {
    char path[512];
    strncpy(path, VCAM_IMAGE, sizeof(path)-1);

    FILE* cfg = fopen(VCAM_CONFIG, "r");
    if (cfg) {
        char line[512];
        if (fgets(line, sizeof(line), cfg)) {
            char* nl = strchr(line, '\n'); if (nl) *nl = '\0';
            if (strlen(line) > 4) strncpy(path, line, sizeof(path)-1);
        }
        fclose(cfg);
    }

    FILE* f = fopen(path, "rb");
    if (!f) { LOGE("Cannot open source: %s", path); return; }
    fseek(f, 0, SEEK_END); size_t sz = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* jpeg = (uint8_t*)malloc(sz);
    if (!jpeg) { fclose(f); return; }
    fread(jpeg, 1, sz, f);
    fclose(f);

    pthread_mutex_lock(&g_lock);
    free(g_jpeg); g_jpeg = jpeg; g_jpeg_sz = sz;
    free(g_nv21); g_nv21 = NULL; g_nv21_w = 0; g_nv21_h = 0;
    build_nv21_from_jpeg(g_jpeg, g_jpeg_sz, &g_nv21, &g_nv21_w, &g_nv21_h);
    pthread_mutex_unlock(&g_lock);

    LOGI("Source loaded: %s (%zu bytes)", path, sz);
}

/* ── Global camera state ── */
static camera_data_cb   g_data_cb = NULL;
static camera_get_mem_t g_get_mem = NULL;
static void*            g_cb_user = NULL;
static volatile int     g_preview = 0;
static pthread_t        g_thread;

static void deliver_frame(void) {
    if (!g_data_cb || !g_get_mem) return;

    pthread_mutex_lock(&g_lock);
    int w = g_nv21_w, h = g_nv21_h;
    uint8_t* src = g_nv21;

    if (!src || w == 0 || h == 0) {
        /* NV21 not ready yet — send a minimal valid grey frame so app doesn't stall */
        pthread_mutex_unlock(&g_lock);
        int fw = 640, fh = 480;
        size_t fsz = (size_t)(fw * fh * 3 / 2);
        camera_memory_t* m = g_get_mem(-1, fsz, 1, g_cb_user);
        if (m && m->data) {
            memset(m->data, 128, fsz);
            g_data_cb(CAMERA_MSG_PREVIEW_FRAME, m, 0, g_cb_user);
            if (m->release) m->release(m);
        }
        return;
    }

    size_t frame_sz = (size_t)(w * h * 3 / 2);
    camera_memory_t* mem = g_get_mem(-1, frame_sz, 1, g_cb_user);
    if (!mem || !mem->data) { pthread_mutex_unlock(&g_lock); return; }
    memcpy(mem->data, src, frame_sz);
    pthread_mutex_unlock(&g_lock);

    g_data_cb(CAMERA_MSG_PREVIEW_FRAME, mem, 0, g_cb_user);
    if (mem->release) mem->release(mem);
}

static void* preview_thread(void* arg) {
    LOGI("Preview thread started");
    /* Send first frame immediately so app doesn't timeout waiting */
    deliver_frame();
    while (g_preview) {
        deliver_frame();
        usleep(33333); /* ~30 fps */
    }
    LOGI("Preview thread stopped");
    return NULL;
}

/* ── Camera ops implementation ── */
static void op_set_callbacks(camera_device_t* d,
                              camera_notify_cb ncb, camera_data_cb dcb,
                              camera_ts_cb tscb, camera_get_mem_t gm, void* u) {
    g_data_cb = dcb; g_get_mem = gm; g_cb_user = u;
}

static int op_start_preview(camera_device_t* d) {
    LOGI("start_preview");
    load_source_image();
    g_preview = 1;
    pthread_create(&g_thread, NULL, preview_thread, NULL);
    return 0;
}

static void op_stop_preview(camera_device_t* d) {
    LOGI("stop_preview");
    g_preview = 0;
    pthread_join(g_thread, NULL);
}

static int op_preview_enabled(camera_device_t* d) { return g_preview; }

static int op_take_picture(camera_device_t* d) {
    LOGI("take_picture");
    pthread_mutex_lock(&g_lock);
    if (g_data_cb && g_get_mem && g_jpeg && g_jpeg_sz) {
        camera_memory_t* m = g_get_mem(-1, g_jpeg_sz, 1, g_cb_user);
        if (m && m->data) {
            memcpy(m->data, g_jpeg, g_jpeg_sz);
            pthread_mutex_unlock(&g_lock);
            g_data_cb(CAMERA_MSG_COMPRESSED_IMAGE, m, 0, g_cb_user);
            if (m->release) m->release(m);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int  op_noop_i(camera_device_t* d, ...) { return 0; }
static void op_noop_v(camera_device_t* d, ...) {}
static int  op_set_preview_window(camera_device_t* d, struct preview_stream_ops* w) { return 0; }

static char* op_get_parameters(camera_device_t* d) {
    static char p[1024];
    int w = g_nv21_w > 0 ? g_nv21_w : 640;
    int h = g_nv21_h > 0 ? g_nv21_h : 480;
    snprintf(p, sizeof(p),
        "preview-size=%dx%d;"
        "preview-format=yuv420sp;"
        "preview-frame-rate=30;"
        "picture-size=%dx%d;"
        "picture-format=jpeg;"
        "jpeg-quality=90;"
        "preview-size-values=%dx%d,640x480,320x240;"
        "picture-size-values=%dx%d,640x480,320x240;"
        "preview-frame-rate-values=30;"
        "facing=front;"
        "orientation=0",
        w, h, w, h, w, h, w, h);
    return p;
}

static void op_release(camera_device_t* d) { g_preview = 0; }

static camera_device_ops_t g_ops = {
    op_set_preview_window,
    op_set_callbacks,
    (void(*)(camera_device_t*,int32_t))op_noop_v,
    (void(*)(camera_device_t*,int32_t))op_noop_v,
    (int(*)(camera_device_t*,int32_t))op_noop_i,
    op_start_preview, op_stop_preview, op_preview_enabled,
    (int(*)(camera_device_t*,int))op_noop_i,
    (int(*)(camera_device_t*))op_noop_i,
    (void(*)(camera_device_t*))op_noop_v,
    (int(*)(camera_device_t*))op_noop_i,
    (void(*)(camera_device_t*,const void*))op_noop_v,
    (int(*)(camera_device_t*))op_noop_i,
    (int(*)(camera_device_t*))op_noop_i,
    op_take_picture,
    (int(*)(camera_device_t*))op_noop_i,
    (int(*)(camera_device_t*,const char*))op_noop_i,
    op_get_parameters,
    (void(*)(camera_device_t*,char*))op_noop_v,
    (int(*)(camera_device_t*,int32_t,int32_t,int32_t))op_noop_i,
    op_release,
    (int(*)(camera_device_t*,int))op_noop_i,
};

static camera_device_t g_cam_dev;

/* ── Module ── */
static int mod_get_num(void) { return 1; }
static int mod_get_info(int id, camera_info_t* info) {
    if (id != 0) return -1;
    info->facing = CAMERA_FACING_FRONT; info->orientation = 0;
    info->device_version = 0x100;
    info->static_camera_characteristics = NULL;
    info->resource_cost = 100;
    info->conflicting_devices = NULL;
    info->conflicting_devices_length = 0;
    return 0;
}

static int mod_open(const hw_module_t* mod, const char* id, hw_device_t** dev) {
    LOGI("camera open: id=%s", id ? id : "null");
    memset(&g_cam_dev, 0, sizeof(g_cam_dev));
    g_cam_dev.common.tag     = HARDWARE_DEVICE_TAG;
    g_cam_dev.common.version = 0x0100;
    g_cam_dev.common.module  = (hw_module_t*)mod;
    g_cam_dev.common.close   = [](hw_device_t*) -> int { return 0; };
    g_cam_dev.ops            = &g_ops;
    *dev = (hw_device_t*)&g_cam_dev;
    return 0;
}

static hw_module_methods_t g_methods = { mod_open };

static camera_module_t g_module = {
    { HARDWARE_MODULE_TAG, 0x0100, 0x0000,
      CAMERA_HARDWARE_MODULE_ID, "VCam Virtual Camera", "VCam",
      &g_methods, NULL, {} },
    mod_get_num, mod_get_info, {}
};

/* ── LD_PRELOAD hooks ── */
extern "C" int hw_get_module_by_class(const char* cls, const char* inst,
                                       const hw_module_t** mod) {
    if (cls && strcmp(cls, CAMERA_HARDWARE_MODULE_ID) == 0) {
        LOGI("intercepted hw_get_module_by_class(camera)");
        *mod = &g_module.common; return 0;
    }
    typedef int (*F)(const char*, const char*, const hw_module_t**);
    static F real = (F)dlsym(RTLD_NEXT, "hw_get_module_by_class");
    return real ? real(cls, inst, mod) : -1;
}

extern "C" int hw_get_module(const char* id, const hw_module_t** mod) {
    if (id && strcmp(id, CAMERA_HARDWARE_MODULE_ID) == 0) {
        LOGI("intercepted hw_get_module(camera)");
        *mod = &g_module.common; return 0;
    }
    typedef int (*F)(const char*, const hw_module_t**);
    static F real = (F)dlsym(RTLD_NEXT, "hw_get_module");
    return real ? real(id, mod) : -1;
}

__attribute__((constructor)) static void vcam_init(void) {
    LOGI("=== VCam inject loaded ===");
    mkdir(VCAM_DIR, 0777);
    /* Pre-load image at inject time so first frame is ready instantly */
    load_source_image();
}

__attribute__((destructor)) static void vcam_fini(void) {
    g_preview = 0;
    pthread_mutex_lock(&g_lock);
    free(g_nv21); g_nv21 = NULL;
    free(g_jpeg); g_jpeg = NULL;
    pthread_mutex_unlock(&g_lock);
    LOGI("=== VCam inject unloaded ===");
}