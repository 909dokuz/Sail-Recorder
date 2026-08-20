#define _GNU_SOURCE
#include <wayland-client.h>
#include "wayland-lipstick-recorder-client-protocol.h"

#include <droidmedia/droidmedia.h>
#include <droidmedia/droidmediacodec.h>
#include <droidmedia/droidmediaconstants.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>

#define CAPTURE_BUFFER_COUNT 6
#define CAPTURE_QUEUE_CAP 6
#define DEFAULT_BITRATE 24000000

/*
  sfrec native hw record alpha
  - Capture: Sailfish Lipstick recorder Wayland protocol, same rhythm as the v2 probe.
  - Encode: DroidMedia hardware H.264 encoder, same working loop as encoder-loop-v4.
  - Output: raw H.264 elementary stream. Remux with ffmpeg -framerate <fps> -i file.h264 -c copy file.mp4.
*/

typedef struct CaptureBuffer {
    struct wl_buffer *wl_buffer;
    uint8_t *data;
    size_t size;
    int state; /* 0 free, 1 in flight, 2 queued to worker */
    int32_t transform;
    uint64_t seq;
    uint32_t capture_time_ms;
} CaptureBuffer;

typedef struct CaptureQueue {
    CaptureBuffer *items[CAPTURE_QUEUE_CAP];
    int head;
    int tail;
    int count;
    int stop;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} CaptureQueue;

typedef struct PtsNode {
    int64_t pts_us;
    struct PtsNode *next;
} PtsNode;

typedef struct EncoderState {
    DroidMediaCodec *codec;
    FILE *out;
    AVFormatContext *mux;
    AVStream *stream;
    PtsNode *pts_head;
    PtsNode *pts_tail;
    size_t pts_count;
    int64_t last_pts_us;
    int64_t nominal_frame_us;
    pthread_t loop_thread;
    volatile int loop_running;
    volatile int loop_started;
    long long loop_calls;
    long long encoded_bytes;
    int encoded_packets;
    int codec_config_packets;
    int errors;
    int eos;
    int chosen_format;
    pthread_mutex_t mutex;
} EncoderState;

typedef struct App {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct wl_output *output;
    struct lipstick_recorder_manager *manager;
    struct lipstick_recorder *recorder;

    int width;
    int height;
    int stride;
    int target_fps;
    int seconds;
    int bitrate;
    const char *out_path;

    int running;
    int failed;
    int worker_started;
    pthread_t worker_thread;
    CaptureQueue queue;
    EncoderState enc;
    CaptureBuffer buffers[CAPTURE_BUFFER_COUNT];

    uint64_t frames_captured;
    uint64_t frames_sent_encoder;
    uint64_t frames_dropped_queue;
    uint64_t frames_dropped_no_buffer;
    uint64_t frames_converted;
    uint64_t cancelled;
    uint64_t in_flight;
    uint64_t seq_next;

    uint32_t last_lipstick_time;
    uint32_t first_lipstick_time;
    int have_first_lipstick_time;
    int64_t first_capture_monotonic_us;
    uint32_t min_delta;
    uint32_t max_delta;
    uint64_t sum_delta;
    uint64_t delta_count;

    double start_seconds;
    double last_progress;
} App;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static int64_t now_monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return
        (int64_t)ts.tv_sec * 1000000LL +
        (int64_t)ts.tv_nsec / 1000LL;
}

static int pts_push(EncoderState *enc, int64_t pts_us) {
    PtsNode *node = (PtsNode *)malloc(sizeof(PtsNode));

    if (!node) {
        return 0;
    }

    node->pts_us = pts_us;
    node->next = NULL;

    pthread_mutex_lock(&enc->mutex);

    if (enc->pts_tail) {
        enc->pts_tail->next = node;
    } else {
        enc->pts_head = node;
    }

    enc->pts_tail = node;
    enc->pts_count++;

    pthread_mutex_unlock(&enc->mutex);
    return 1;
}

static int pts_pop_locked(EncoderState *enc, int64_t *pts_us) {
    PtsNode *node = enc->pts_head;

    if (!node) {
        return 0;
    }

    enc->pts_head = node->next;

    if (!enc->pts_head) {
        enc->pts_tail = NULL;
    }

    if (enc->pts_count > 0) {
        enc->pts_count--;
    }

    *pts_us = node->pts_us;
    free(node);
    return 1;
}

static void pts_clear_locked(EncoderState *enc) {
    PtsNode *node = enc->pts_head;

    while (node) {
        PtsNode *next = node->next;
        free(node);
        node = next;
    }

    enc->pts_head = NULL;
    enc->pts_tail = NULL;
    enc->pts_count = 0;
}


static int create_tmp_file(size_t size) {
    char path[] = "/tmp/sfrec-hw-shm-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return -1; }
    unlink(path);
    if (ftruncate(fd, (off_t)size) < 0) { perror("ftruncate"); close(fd); return -1; }
    return fd;
}

static int create_capture_buffer(App *app, CaptureBuffer *buf) {
    memset(buf, 0, sizeof(*buf));
    int stride = app->width * 4;
    size_t size = (size_t)stride * (size_t)app->height;
    int fd = create_tmp_file(size);
    if (fd < 0) return 0;

    uint8_t *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { perror("mmap"); close(fd); return 0; }
    memset(data, 0, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(app->shm, fd, (int)size);
    if (!pool) { fprintf(stderr, "wl_shm_create_pool failed\n"); munmap(data, size); close(fd); return 0; }

    buf->wl_buffer = wl_shm_pool_create_buffer(pool, 0, app->width, app->height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    if (!buf->wl_buffer) { fprintf(stderr, "wl_shm_pool_create_buffer failed\n"); munmap(data, size); return 0; }
    wl_buffer_set_user_data(buf->wl_buffer, buf);
    buf->data = data;
    buf->size = size;
    buf->state = 0;
    return 1;
}

static int queue_push(App *app, CaptureBuffer *buf) {
    CaptureQueue *q = &app->queue;
    pthread_mutex_lock(&q->mutex);
    if (q->count >= CAPTURE_QUEUE_CAP) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }
    q->items[q->tail] = buf;
    q->tail = (q->tail + 1) % CAPTURE_QUEUE_CAP;
    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

static void input_unref(void *data) {
    free(data);
}

static void codec_signal_eos(void *data) {
    EncoderState *enc = (EncoderState *)data;
    pthread_mutex_lock(&enc->mutex);
    enc->eos++;
    pthread_mutex_unlock(&enc->mutex);
}

static void codec_error(void *data, int err) {
    EncoderState *enc = (EncoderState *)data;
    pthread_mutex_lock(&enc->mutex);
    enc->errors++;
    fprintf(stderr, "CODEC_ERROR %d\n", err);
    pthread_mutex_unlock(&enc->mutex);
}

static int codec_size_changed(void *data, int32_t width, int32_t height) {
    (void)data;
    printf("CODEC_SIZE_CHANGED %d %d\n", width, height);
    fflush(stdout);
    return 0;
}

static void encoded_available(void *data, DroidMediaCodecData *encoded) {
    EncoderState *enc = (EncoderState *)data;

    if (!encoded || !encoded->data.data || encoded->data.size <= 0) {
        return;
    }

    pthread_mutex_lock(&enc->mutex);

    enc->encoded_bytes += encoded->data.size;
    enc->encoded_packets++;

    /*
     * Kaynak motor her senkron kareye SPS/PPS basliklarini ekliyor.
     * Ayri codec-config paketi gelirse kare sayilmadan atlanir.
     */
    if (encoded->codec_config) {
        enc->codec_config_packets++;
        pthread_mutex_unlock(&enc->mutex);
        return;
    }

    int64_t pts_us = 0;

    if (!pts_pop_locked(enc, &pts_us)) {
        if (enc->last_pts_us >= 0) {
            pts_us = enc->last_pts_us + enc->nominal_frame_us;
        }

        fprintf(
            stderr,
            "PTS_QUEUE_EMPTY fallback=%lld\n",
            (long long)pts_us
        );
    }

    if (enc->last_pts_us >= 0 && pts_us <= enc->last_pts_us) {
        pts_us = enc->last_pts_us + 1;
    }

    enc->last_pts_us = pts_us;

    if (
        enc->mux &&
        enc->stream &&
        encoded->data.size <= INT_MAX
    ) {
        AVPacket *packet = av_packet_alloc();

        if (!packet) {
            enc->errors++;
            fprintf(stderr, "MUX_PACKET_ALLOC_ERROR\n");
        } else if (
            av_new_packet(packet, (int)encoded->data.size) < 0
        ) {
            enc->errors++;
            fprintf(stderr, "MUX_PACKET_DATA_ALLOC_ERROR\n");
        } else {
            const AVRational input_time_base = { 1, 1000000 };

            memcpy(
                packet->data,
                encoded->data.data,
                encoded->data.size
            );

            packet->stream_index = enc->stream->index;
            packet->pos = -1;

            packet->pts = av_rescale_q(
                pts_us,
                input_time_base,
                enc->stream->time_base
            );

            packet->dts = packet->pts;

            packet->duration = av_rescale_q(
                enc->nominal_frame_us,
                input_time_base,
                enc->stream->time_base
            );

            if (encoded->sync) {
                packet->flags |= AV_PKT_FLAG_KEY;
            }

            int result = av_interleaved_write_frame(
                enc->mux,
                packet
            );

            if (result < 0) {
                enc->errors++;

                fprintf(
                    stderr,
                    "MUX_WRITE_ERROR result=%d pts_us=%lld\n",
                    result,
                    (long long)pts_us
                );
            }
        }

        av_packet_free(&packet);
    }

    pthread_mutex_unlock(&enc->mutex);
}

static void *codec_loop_thread(void *arg) {
    EncoderState *enc = (EncoderState *)arg;
    enc->loop_started = 1;
    while (enc->loop_running) {
        DroidMediaCodecLoopReturn r = droid_media_codec_loop(enc->codec);
        pthread_mutex_lock(&enc->mutex);
        enc->loop_calls++;
        if (r == DROID_MEDIA_CODEC_LOOP_ERROR) {
            enc->errors++;
            enc->loop_running = 0;
        } else if (r == DROID_MEDIA_CODEC_LOOP_EOS) {
            enc->eos++;
            enc->loop_running = 0;
        }
        pthread_mutex_unlock(&enc->mutex);
        if (r != DROID_MEDIA_CODEC_LOOP_OK) break;
        usleep(1000);
    }
    return NULL;
}

static DroidMediaCodec *create_encoder(App *app, int *chosen_format) {
    DroidMediaColourFormatConstants c;
    memset(&c, 0, sizeof(c));
    droid_media_colour_format_constants_init(&c);

    int formats_to_try[] = {
        21,
        c.OMX_COLOR_FormatYUV420SemiPlanar,
        c.OMX_COLOR_FormatYUV420Planar,
        c.OMX_COLOR_FormatYUV420Flexible,
        19,
        20,
        2135033992
    };

    for (unsigned int i = 0; i < sizeof(formats_to_try)/sizeof(formats_to_try[0]); i++) {
        DroidMediaCodecEncoderMetaData meta;
        memset(&meta, 0, sizeof(meta));
        meta.parent.type = "video/avc";
        meta.parent.width = app->width;
        meta.parent.height = app->height;
        meta.parent.fps = app->target_fps;
        meta.parent.flags = DROID_MEDIA_CODEC_HW_ONLY | DROID_MEDIA_CODEC_USE_EXTERNAL_LOOP;
        meta.color_format = formats_to_try[i];
        meta.bitrate = app->bitrate;
        meta.meta_data = 0;
        meta.stride = app->width;
        meta.slice_height = app->height;
        meta.max_input_size = app->width * app->height * 3 / 2;
        meta.bitrate_mode = DROID_MEDIA_CODEC_BITRATE_CONTROL_CBR;
        meta.codec_specific.h264.prepend_header_to_sync_frames = 1;

        printf("TRY_START_FORMAT %d\n", formats_to_try[i]);
        fflush(stdout);
        DroidMediaCodec *codec = droid_media_codec_create_encoder(&meta);
        if (!codec) continue;

        DroidMediaCodecCallbacks cb;
        memset(&cb, 0, sizeof(cb));
        cb.signal_eos = codec_signal_eos;
        cb.error = codec_error;
        cb.size_changed = codec_size_changed;
        droid_media_codec_set_callbacks(codec, &cb, &app->enc);

        DroidMediaCodecDataCallbacks dcb;
        memset(&dcb, 0, sizeof(dcb));
        dcb.data_available = encoded_available;
        droid_media_codec_set_data_callbacks(codec, &dcb, &app->enc);

        if (droid_media_codec_start(codec)) {
            *chosen_format = formats_to_try[i];
            return codec;
        }
        droid_media_codec_destroy(codec);
    }
    return NULL;
}

static int encoder_start(App *app) {
    if (!droid_media_init()) {
        printf("RESULT_DROID_INIT 0\n");
        return 0;
    }
    printf("RESULT_DROID_INIT 1\n");

    pthread_mutex_init(&app->enc.mutex, NULL);
    app->enc.codec = create_encoder(app, &app->enc.chosen_format);
    if (!app->enc.codec) return 0;
    printf("RESULT_ENCODER_STARTED 1\n");
    printf("RESULT_CHOSEN_FORMAT %d\n", app->enc.chosen_format);


    app->enc.last_pts_us = -1;
    app->enc.nominal_frame_us =
        app->target_fps > 0
        ? 1000000LL / app->target_fps
        : 16667LL;

    int mux_result = avformat_alloc_output_context2(
        &app->enc.mux,
        NULL,
        "mpegts",
        app->out_path
    );

    if (mux_result < 0 || !app->enc.mux) {
        fprintf(
            stderr,
            "ERROR mux context result=%d\n",
            mux_result
        );
        return 0;
    }

    app->enc.stream = avformat_new_stream(
        app->enc.mux,
        NULL
    );

    if (!app->enc.stream) {
        fprintf(stderr, "ERROR mux stream create\n");
        avformat_free_context(app->enc.mux);
        app->enc.mux = NULL;
        return 0;
    }

    app->enc.stream->time_base =
        (AVRational){ 1, 1000 };

    app->enc.stream->avg_frame_rate =
        (AVRational){ app->target_fps, 1 };

    app->enc.stream->r_frame_rate =
        (AVRational){ app->target_fps, 1 };

    app->enc.stream->codecpar->codec_type =
        AVMEDIA_TYPE_VIDEO;

    app->enc.stream->codecpar->codec_id =
        AV_CODEC_ID_H264;

    app->enc.stream->codecpar->codec_tag = 0;
    app->enc.stream->codecpar->width = app->width;
    app->enc.stream->codecpar->height = app->height;
    app->enc.stream->codecpar->bit_rate = app->bitrate;

    if (!(app->enc.mux->oformat->flags & AVFMT_NOFILE)) {
        mux_result = avio_open(
            &app->enc.mux->pb,
            app->out_path,
            AVIO_FLAG_WRITE
        );

        if (mux_result < 0) {
            fprintf(
                stderr,
                "ERROR mux file open result=%d\n",
                mux_result
            );

            avformat_free_context(app->enc.mux);
            app->enc.mux = NULL;
            app->enc.stream = NULL;
            return 0;
        }
    }

    mux_result = avformat_write_header(
        app->enc.mux,
        NULL
    );

    if (mux_result < 0) {
        fprintf(
            stderr,
            "ERROR mux header result=%d\n",
            mux_result
        );

        if (!(app->enc.mux->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&app->enc.mux->pb);
        }

        avformat_free_context(app->enc.mux);
        app->enc.mux = NULL;
        app->enc.stream = NULL;
        return 0;
    }

    printf("RESULT_MUX_STARTED MPEGTS\n");
    fflush(stdout);

    app->enc.loop_running = 1;
    if (pthread_create(&app->enc.loop_thread, NULL, codec_loop_thread, &app->enc) != 0) {
        perror("pthread_create codec loop");
        return 0;
    }
    for (int wait = 0; wait < 100 && !app->enc.loop_started; wait++) usleep(10000);
    return 1;
}

static void encoder_stop(App *app) {
    if (app->enc.codec) {
        droid_media_codec_drain(app->enc.codec);
        double start = now_seconds();
        while (now_seconds() - start < 2.0) {
            pthread_mutex_lock(&app->enc.mutex);
            int eos = app->enc.eos;
            int err = app->enc.errors;
            pthread_mutex_unlock(&app->enc.mutex);
            if (eos > 0 || err > 0) break;
            usleep(20000);
        }
        app->enc.loop_running = 0;
        droid_media_codec_drain(app->enc.codec);
        pthread_join(app->enc.loop_thread, NULL);
    }
    pthread_mutex_lock(&app->enc.mutex);

    if (app->enc.mux) {
        int trailer_result = av_write_trailer(
            app->enc.mux
        );

        if (trailer_result < 0) {
            fprintf(
                stderr,
                "MUX_TRAILER_ERROR result=%d\n",
                trailer_result
            );
        }

        if (!(app->enc.mux->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&app->enc.mux->pb);
        }

        avformat_free_context(app->enc.mux);
        app->enc.mux = NULL;
        app->enc.stream = NULL;
    }

    pts_clear_locked(&app->enc);

    if (app->enc.out) {
        fclose(app->enc.out);
    }

    app->enc.out = NULL;

    pthread_mutex_unlock(&app->enc.mutex);
    if (app->enc.codec) {
        droid_media_codec_stop(app->enc.codec);
        droid_media_codec_destroy(app->enc.codec);
        app->enc.codec = NULL;
    }
    droid_media_deinit();
}

static inline uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static inline void rgba_to_yuv(uint8_t r, uint8_t g, uint8_t b, int *y, int *u, int *v) {
    /* BT.601 full-ish range, fast integer approximation. */
    int yy = (( 66 * r + 129 * g +  25 * b + 128) >> 8) + 16;
    int uu = ((-38 * r -  74 * g + 112 * b + 128) >> 8) + 128;
    int vv = ((112 * r -  94 * g -  18 * b + 128) >> 8) + 128;
    *y = yy; *u = uu; *v = vv;
}

static void convert_rgba_to_nv12(uint8_t *dst, const uint8_t *src, int width, int height, int stride, int transform) {
    uint8_t *yplane = dst;
    uint8_t *uvplane = dst + (size_t)width * (size_t)height;

    for (int y = 0; y < height; y += 2) {
        int sy0 = (transform == LIPSTICK_RECORDER_TRANSFORM_Y_INVERTED) ? (height - 1 - y) : y;
        int sy1 = (y + 1 < height) ? ((transform == LIPSTICK_RECORDER_TRANSFORM_Y_INVERTED) ? (height - 2 - y) : (y + 1)) : sy0;
        const uint8_t *row0 = src + (size_t)sy0 * (size_t)stride;
        const uint8_t *row1 = src + (size_t)sy1 * (size_t)stride;

        for (int x = 0; x < width; x += 2) {
            const uint8_t *p00 = row0 + (size_t)x * 4;
            const uint8_t *p01 = row0 + (size_t)(x + 1 < width ? x + 1 : x) * 4;
            const uint8_t *p10 = row1 + (size_t)x * 4;
            const uint8_t *p11 = row1 + (size_t)(x + 1 < width ? x + 1 : x) * 4;

            int y00,u00,v00,y01,u01,v01,y10,u10,v10,y11,u11,v11;
            rgba_to_yuv(p00[0], p00[1], p00[2], &y00, &u00, &v00);
            rgba_to_yuv(p01[0], p01[1], p01[2], &y01, &u01, &v01);
            rgba_to_yuv(p10[0], p10[1], p10[2], &y10, &u10, &v10);
            rgba_to_yuv(p11[0], p11[1], p11[2], &y11, &u11, &v11);

            size_t yi0 = (size_t)y * (size_t)width + x;
            yplane[yi0] = clamp_u8(y00);
            if (x + 1 < width) yplane[yi0 + 1] = clamp_u8(y01);
            if (y + 1 < height) {
                size_t yi1 = (size_t)(y + 1) * (size_t)width + x;
                yplane[yi1] = clamp_u8(y10);
                if (x + 1 < width) yplane[yi1 + 1] = clamp_u8(y11);
            }

            int uavg = (u00 + u01 + u10 + u11) / 4;
            int vavg = (v00 + v01 + v10 + v11) / 4;
            size_t uvi = (size_t)(y / 2) * (size_t)width + x;
            uvplane[uvi] = clamp_u8(uavg);
            if (x + 1 < width) uvplane[uvi + 1] = clamp_u8(vavg);
        }
    }
}

static int encoder_queue_frame(App *app, uint8_t *nv12, int64_t pts_us, int sync) {
    DroidMediaCodecData in;
    memset(&in, 0, sizeof(in));
    in.data.data = nv12;
    in.data.size = app->width * app->height * 3 / 2;
    in.ts = pts_us;
    in.decoding_ts = pts_us;
    in.sync = sync;
    in.codec_config = false;

    DroidMediaBufferCallbacks bcb;
    memset(&bcb, 0, sizeof(bcb));
    bcb.unref = input_unref;
    bcb.data = nv12;

    if (!pts_push(&app->enc, pts_us)) {
        pthread_mutex_lock(&app->enc.mutex);
        app->enc.errors++;
        pthread_mutex_unlock(&app->enc.mutex);

        fprintf(stderr, "PTS_QUEUE_PUSH_ERROR\n");
    }

    droid_media_codec_queue(app->enc.codec, &in, &bcb);
    return 1;
}

static void *worker_thread(void *arg) {
    App *app = (App *)arg;
    CaptureQueue *q = &app->queue;
    int frame_size = app->width * app->height * 3 / 2;

    while (1) {
        pthread_mutex_lock(&q->mutex);
        while (!q->stop && q->count == 0) pthread_cond_wait(&q->cond, &q->mutex);
        if (q->stop && q->count == 0) { pthread_mutex_unlock(&q->mutex); break; }
        CaptureBuffer *buf = q->items[q->head];
        q->head = (q->head + 1) % CAPTURE_QUEUE_CAP;
        q->count--;
        pthread_mutex_unlock(&q->mutex);

        if (buf && buf->data) {
            uint8_t *nv12 = (uint8_t *)malloc((size_t)frame_size);
            if (nv12) {
                convert_rgba_to_nv12(nv12, buf->data, app->width, app->height, app->stride, buf->transform);
                uint32_t elapsed_ms =
                    buf->capture_time_ms -
                    app->first_lipstick_time;

                int64_t pts_us =
                    (int64_t)elapsed_ms * 1000LL;
                encoder_queue_frame(app, nv12, pts_us, app->frames_sent_encoder == 0);
                app->frames_sent_encoder++;
                app->frames_converted++;
            }
            buf->state = 0;
        }
    }
    return NULL;
}

static void request_one(App *app) {
    if (!app->running || !app->recorder) return;
    if (app->in_flight > 0) return;
    for (int i = 0; i < CAPTURE_BUFFER_COUNT; i++) {
        if (app->buffers[i].state == 0 && app->buffers[i].wl_buffer) {
            app->buffers[i].state = 1;
            app->buffers[i].seq = ++app->seq_next;
            app->in_flight++;
            lipstick_recorder_record_frame(app->recorder, app->buffers[i].wl_buffer);
            wl_display_flush(app->display);
            return;
        }
    }
    app->frames_dropped_no_buffer++;
}

static void recorder_setup(void *data, struct lipstick_recorder *recorder, int32_t width, int32_t height, int32_t stride, int32_t format) {
    (void)recorder;
    App *app = (App *)data;
    printf("SETUP width=%d height=%d stride=%d format=%d\n", width, height, stride, format);
    if (width > 0 && height > 0) { app->width = width; app->height = height; app->stride = stride; }
    fflush(stdout);
}

static void recorder_frame(void *data, struct lipstick_recorder *recorder, struct wl_buffer *buffer, uint32_t time, int32_t transform) {
    (void)recorder;
    App *app = (App *)data;
    CaptureBuffer *buf = (CaptureBuffer *)wl_buffer_get_user_data(buffer);
    if (buf && buf->state == 1 && app->in_flight > 0) app->in_flight--;

    app->frames_captured++;
    if (app->last_lipstick_time != 0) {
        uint32_t delta = time - app->last_lipstick_time;
        if (app->delta_count == 0 || delta < app->min_delta) app->min_delta = delta;
        if (app->delta_count == 0 || delta > app->max_delta) app->max_delta = delta;
        app->sum_delta += delta;
        app->delta_count++;
    }
    if (!app->have_first_lipstick_time) {
        app->first_lipstick_time = time;
        app->have_first_lipstick_time = 1;

        app->first_capture_monotonic_us =
            now_monotonic_us();

        printf(
            "RESULT_FIRST_CAPTURE_MONOTONIC_US %lld\n",
            (long long)app->first_capture_monotonic_us
        );

        fflush(stdout);
    }

    app->last_lipstick_time = time;

    if (buf) {
        buf->capture_time_ms = time;
        buf->transform = transform;
        buf->state = 2;
        if (!queue_push(app, buf)) {
            app->frames_dropped_queue++;
            buf->state = 0;
        }
    }

    double elapsed = now_seconds() - app->start_seconds;
    if (elapsed - app->last_progress >= 1.0 || app->frames_captured == 1) {
        pthread_mutex_lock(&app->enc.mutex);
        long long bytes = app->enc.encoded_bytes;
        int packets = app->enc.encoded_packets;
        int errs = app->enc.errors;
        pthread_mutex_unlock(&app->enc.mutex);
        pthread_mutex_lock(&app->queue.mutex);
        int qcount = app->queue.count;
        pthread_mutex_unlock(&app->queue.mutex);
        printf("PROGRESS cap=%llu enc_in=%llu packets=%d bytes=%lld fps=%.2f q=%d dropq=%llu err=%d\n",
               (unsigned long long)app->frames_captured,
               (unsigned long long)app->frames_sent_encoder,
               packets, bytes, elapsed > 0 ? app->frames_captured / elapsed : 0.0,
               qcount, (unsigned long long)app->frames_dropped_queue, errs);
        fflush(stdout);
        app->last_progress = elapsed;
    }

    if (elapsed >= app->seconds) { app->running = 0; return; }
    request_one(app);
}

static void recorder_failed(void *data, struct lipstick_recorder *recorder, int32_t result, struct wl_buffer *buffer) {
    (void)recorder;
    CaptureBuffer *buf = (CaptureBuffer *)wl_buffer_get_user_data(buffer);
    if (buf && buf->state) buf->state = 0;
    App *app = (App *)data;
    if (app->in_flight > 0) app->in_flight--;
    fprintf(stderr, "RECORDER_FAILED result=%d\n", result);
    app->failed = 1;
    app->running = 0;
}

static void recorder_cancelled(void *data, struct lipstick_recorder *recorder, struct wl_buffer *buffer) {
    (void)recorder;
    CaptureBuffer *buf = (CaptureBuffer *)wl_buffer_get_user_data(buffer);
    if (buf && buf->state == 1) buf->state = 0;
    App *app = (App *)data;
    if (app->in_flight > 0) app->in_flight--;
    app->cancelled++;
}

static const struct lipstick_recorder_listener recorder_listener = { recorder_setup, recorder_frame, recorder_failed, recorder_cancelled };

static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char *make, const char *model, int32_t transform) {
    (void)data; (void)output; (void)x; (void)y; (void)physical_width; (void)physical_height; (void)subpixel; (void)make; (void)model; (void)transform;
}
static void output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    (void)output; App *app = (App *)data;
    if (flags & WL_OUTPUT_MODE_CURRENT) { app->width = width; app->height = height; app->stride = width * 4; printf("OUTPUT current width=%d height=%d refresh_mHz=%d\n", width, height, refresh); }
}
static void output_done(void *data, struct wl_output *output) { (void)data; (void)output; }
static void output_scale(void *data, struct wl_output *output, int32_t factor) { (void)data; (void)output; (void)factor; }
static void output_name(void *data, struct wl_output *output, const char *name) { (void)data; (void)output; (void)name; }
static void output_description(void *data, struct wl_output *output, const char *description) { (void)data; (void)output; (void)description; }
static const struct wl_output_listener output_listener = { output_geometry, output_mode, output_done, output_scale, output_name, output_description };

static void registry_global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
    App *app = (App *)data;
    if (strcmp(interface, "wl_shm") == 0) {
        app->shm = (struct wl_shm *)wl_registry_bind(registry, id, &wl_shm_interface, version < 1 ? version : 1); printf("FOUND wl_shm\n");
    } else if (strcmp(interface, "wl_output") == 0 && !app->output) {
        uint32_t bind_ver = version < 4 ? version : 4;
        app->output = (struct wl_output *)wl_registry_bind(registry, id, &wl_output_interface, bind_ver);
        wl_output_add_listener(app->output, &output_listener, app); printf("FOUND wl_output version=%u\n", version);
    } else if (strcmp(interface, "lipstick_recorder_manager") == 0) {
        app->manager = (struct lipstick_recorder_manager *)wl_registry_bind(registry, id, &lipstick_recorder_manager_interface, version < 1 ? version : 1); printf("FOUND lipstick_recorder_manager version=%u\n", version);
    }
    fflush(stdout);
}
static void registry_remove(void *data, struct wl_registry *registry, uint32_t id) { (void)data; (void)registry; (void)id; }
static const struct wl_registry_listener registry_listener = { registry_global, registry_remove };

static void stop_worker(App *app) {
    if (!app->worker_started) return;
    pthread_mutex_lock(&app->queue.mutex);
    app->queue.stop = 1;
    pthread_cond_signal(&app->queue.cond);
    pthread_mutex_unlock(&app->queue.mutex);
    pthread_join(app->worker_thread, NULL);
    app->worker_started = 0;
}

static void cleanup(App *app) {
    stop_worker(app);
    encoder_stop(app);
    for (int i = 0; i < CAPTURE_BUFFER_COUNT; i++) {
        if (app->buffers[i].wl_buffer) wl_buffer_destroy(app->buffers[i].wl_buffer);
        if (app->buffers[i].data && app->buffers[i].size) munmap(app->buffers[i].data, app->buffers[i].size);
    }
    if (app->recorder) lipstick_recorder_destroy(app->recorder);
    if (app->manager) lipstick_recorder_manager_destroy(app->manager);
    if (app->output) wl_output_destroy(app->output);
    if (app->shm) wl_shm_destroy(app->shm);
    if (app->registry) wl_registry_destroy(app->registry);
    if (app->display) wl_display_disconnect(app->display);
    pthread_mutex_destroy(&app->queue.mutex);
    pthread_cond_destroy(&app->queue.cond);
    pthread_mutex_destroy(&app->enc.mutex);
}

static int parse_int(int argc, char **argv, int idx, int def, int min, int max) {
    if (argc <= idx) return def;
    int v = atoi(argv[idx]);
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

static void print_result(App *app) {
    double elapsed = now_seconds() - app->start_seconds;
    pthread_mutex_lock(&app->enc.mutex);
    long long bytes = app->enc.encoded_bytes;
    int packets = app->enc.encoded_packets;
    int cfg = app->enc.codec_config_packets;
    int errs = app->enc.errors;
    int eos = app->enc.eos;
    long long loops = app->enc.loop_calls;
    pthread_mutex_unlock(&app->enc.mutex);
    pthread_mutex_lock(&app->queue.mutex);
    int qleft = app->queue.count;
    pthread_mutex_unlock(&app->queue.mutex);

    printf("RESULT_FILE %s\n", app->out_path);
    printf("RESULT_SECONDS %.3f\n", elapsed);
    printf("RESULT_CAPTURED_FRAMES %llu\n", (unsigned long long)app->frames_captured);
    printf("RESULT_CAPTURE_AVG_FPS %.2f\n", elapsed > 0 ? app->frames_captured / elapsed : 0.0);
    printf("RESULT_SENT_ENCODER %llu\n", (unsigned long long)app->frames_sent_encoder);
    printf("RESULT_CONVERTED %llu\n", (unsigned long long)app->frames_converted);
    printf("RESULT_DROPPED_QUEUE %llu\n", (unsigned long long)app->frames_dropped_queue);
    printf("RESULT_DROPPED_NO_BUFFER %llu\n", (unsigned long long)app->frames_dropped_no_buffer);
    printf("RESULT_QUEUE_LEFT %d\n", qleft);
    printf("RESULT_LOOP_CALLS %lld\n", loops);
    printf("RESULT_ENCODED_PACKETS %d\n", packets);
    printf("RESULT_CODEC_CONFIG_PACKETS %d\n", cfg);
    printf("RESULT_ENCODED_BYTES %lld\n", bytes);
    printf("RESULT_ERRORS %d\n", errs);
    printf("RESULT_EOS %d\n", eos);
    printf("RESULT_CANCELLED %llu\n", (unsigned long long)app->cancelled);
    printf("RESULT_FAILED %d\n", app->failed);
    if (app->delta_count > 0) {
        double avg_delta = (double)app->sum_delta / (double)app->delta_count;
        printf("RESULT_LIPSTICK_DELTA_MS avg=%.2f min=%u max=%u samples=%llu\n", avg_delta, app->min_delta, app->max_delta, (unsigned long long)app->delta_count);
    }
    printf("RESULT_OK %d\n", (bytes > 0 && packets > 0 && errs == 0 && !app->failed) ? 1 : 0);
}

int main(int argc, char **argv) {
    App app;
    memset(&app, 0, sizeof(app));
    app.out_path = argc > 1 ? argv[1] : "/tmp/sail-recorder-video.ts";
    app.seconds = parse_int(argc, argv, 2, 5, 1, 120);
    app.target_fps = parse_int(argc, argv, 3, 45, 1, 60);
    app.bitrate = parse_int(argc, argv, 4, DEFAULT_BITRATE, 1000000, 80000000);
    app.width = 1080;
    app.height = 2520;
    app.stride = app.width * 4;
    pthread_mutex_init(&app.queue.mutex, NULL);
    pthread_cond_init(&app.queue.cond, NULL);

    printf("ARGS out=%s seconds=%d target_fps=%d bitrate=%d\n", app.out_path, app.seconds, app.target_fps, app.bitrate);
    printf("ENV XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=%s\n", getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(null)", getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(null)");

    app.display = wl_display_connect(NULL);
    if (!app.display) { fprintf(stderr, "ERROR wl_display_connect failed\n"); cleanup(&app); return 1; }
    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, &app);
    wl_display_roundtrip(app.display);
    wl_display_roundtrip(app.display);
    if (!app.shm || !app.output || !app.manager) { fprintf(stderr, "ERROR missing globals\n"); cleanup(&app); return 2; }

    printf("USING width=%d height=%d stride=%d\n", app.width, app.height, app.stride);
    app.recorder = lipstick_recorder_manager_create_recorder(app.manager, app.output);
    if (!app.recorder) { fprintf(stderr, "ERROR create_recorder failed\n"); cleanup(&app); return 3; }
    lipstick_recorder_add_listener(app.recorder, &recorder_listener, &app);
    wl_display_roundtrip(app.display);

    for (int i = 0; i < CAPTURE_BUFFER_COUNT; i++) {
        if (!create_capture_buffer(&app, &app.buffers[i])) { fprintf(stderr, "ERROR create_capture_buffer %d failed\n", i); cleanup(&app); return 4; }
    }

    if (!encoder_start(&app)) { fprintf(stderr, "ERROR encoder_start failed\n"); cleanup(&app); return 5; }

    if (pthread_create(&app.worker_thread, NULL, worker_thread, &app) != 0) { perror("pthread_create worker"); cleanup(&app); return 6; }
    app.worker_started = 1;

    app.running = 1;
    app.start_seconds = now_seconds();
    app.last_progress = -1.0;

    int fd = wl_display_get_fd(app.display);
    double next_tick = app.start_seconds;
    double period = 1.0 / (double)app.target_fps;

    while (app.running) {
        double now = now_seconds();
        if (now >= next_tick) {
            lipstick_recorder_repaint(app.recorder);
            request_one(&app);
            next_tick += period;
            if (now - next_tick > 1.0) next_tick = now;
        }
        wl_display_dispatch_pending(app.display);
        wl_display_flush(app.display);
        struct pollfd pfd;
        pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
        int prc = poll(&pfd, 1, 5);
        if (prc > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_dispatch(app.display) == -1) { perror("wl_display_dispatch"); break; }
        }
        if (now_seconds() - app.start_seconds >= (double)app.seconds) app.running = 0;
    }

    stop_worker(&app);
    print_result(&app);
    cleanup(&app);
    return 0;
}
