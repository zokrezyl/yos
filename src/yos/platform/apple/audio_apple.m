/*
 * Apple audio backend — AVAudioEngine.
 *
 * Input: install a tap on engine.inputNode. The tap block runs on a
 * framework thread; we convert each captured buffer to the client's
 * requested format with AVAudioConverter, then push raw bytes into a
 * vfd-style ring so ydev_audio_in_read() can drain them.
 *
 * Output: an AVAudioSourceNode pulls samples on the realtime thread.
 * We back it with a host-side ring that the client writes into via
 * ydev_audio_out_write(); the source-node render block memcpy's out
 * of the ring. The fd becomes readable when the ring level drops
 * below the low watermark — i.e. when there is room for more data.
 *
 * Format conversion is performed every input callback to honour the
 * client's YDEV_SAMPLE_S16 / YDEV_SAMPLE_F32 + rate + channels request,
 * regardless of what the hardware actually delivers.
 */

#include "../../impl/ydev/internal.h"
#include <yos/ydev/audio.h>

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── shared format helpers ───────────────────────────────────────────── */

static size_t bytes_per_frame(const ydev_audio_config_t *c)
{
    size_t bps = (c->format == YDEV_SAMPLE_F32) ? 4 : 2;
    return bps * (c->channels ? c->channels : 1);
}

static AVAudioFormat *client_format(const ydev_audio_config_t *c)
{
    AVAudioCommonFormat fmt = (c->format == YDEV_SAMPLE_F32)
        ? AVAudioPCMFormatFloat32 : AVAudioPCMFormatInt16;
    return [[AVAudioFormat alloc]
        initWithCommonFormat:fmt
                  sampleRate:c->rate_hz
                    channels:c->channels ? c->channels : 1
                 interleaved:YES];
}

/* ── input ───────────────────────────────────────────────────────────── */

@interface YdevAudioInImpl : NSObject
{
@public
    AVAudioEngine     *engine;
    AVAudioConverter  *converter;
    AVAudioFormat     *out_fmt;
    struct ydev_audio_in *owner;   /* back pointer, unretained          */
}
@end

struct ydev_audio_in {
    struct ydev_vfd        vfd;       /* records = chunk bytes            */
    ydev_audio_config_t    cfg;
    size_t                 frame_bytes;
    size_t                 chunk_frames;
    void                  *impl;      /* CFBridgingRetain'd YdevAudioInImpl */
    int                    started;
};

@implementation YdevAudioInImpl
@end

ydev_audio_in_t *ydev_audio_in_open(const ydev_audio_config_t *cfg)
{
    if (!cfg || cfg->rate_hz == 0 || cfg->channels == 0) {
        ydev_set_error("audio_in_open: invalid config");
        return NULL;
    }
    ydev_audio_in_t *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->cfg = *cfg;
    h->frame_bytes  = bytes_per_frame(cfg);
    h->chunk_frames = cfg->frames_per_chunk ? cfg->frames_per_chunk
                                            : (cfg->rate_hz / 100);  /* 10 ms */
    size_t chunk_bytes = h->chunk_frames * h->frame_bytes;
    if (ydev_vfd_init(&h->vfd, chunk_bytes, 8, NULL) != 0) {
        free(h);
        return NULL;
    }

    YdevAudioInImpl *impl = [[YdevAudioInImpl alloc] init];
    impl->engine    = [[AVAudioEngine alloc] init];
    impl->out_fmt   = client_format(cfg);
    impl->owner     = h;

    AVAudioFormat *in_fmt = [impl->engine.inputNode inputFormatForBus:0];
    impl->converter = [[AVAudioConverter alloc] initFromFormat:in_fmt toFormat:impl->out_fmt];

    AVAudioFrameCount tap_size = (AVAudioFrameCount)h->chunk_frames;

    [impl->engine.inputNode installTapOnBus:0
                                 bufferSize:tap_size
                                     format:in_fmt
                                      block:^(AVAudioPCMBuffer * _Nonnull buf,
                                              AVAudioTime * _Nonnull when) {
        (void)when;
        AVAudioFrameCount cap = (AVAudioFrameCount)((double)buf.frameLength
                                                    * (double)impl->out_fmt.sampleRate
                                                    / (double)in_fmt.sampleRate + 16);
        AVAudioPCMBuffer *outbuf =
            [[AVAudioPCMBuffer alloc] initWithPCMFormat:impl->out_fmt
                                          frameCapacity:cap];
        if (!outbuf) return;

        __block BOOL consumed = NO;
        NSError *err = nil;
        AVAudioConverterInputBlock src = ^AVAudioBuffer *(AVAudioPacketCount want,
                                                          AVAudioConverterInputStatus *st) {
            (void)want;
            if (consumed) { *st = AVAudioConverterInputStatus_NoDataNow; return nil; }
            consumed = YES;
            *st = AVAudioConverterInputStatus_HaveData;
            return buf;
        };
        AVAudioConverterOutputStatus os =
            [impl->converter convertToBuffer:outbuf error:&err withInputFromBlock:src];
        if (os == AVAudioConverterOutputStatus_Error || !outbuf.frameLength) return;

        const uint8_t *src_bytes;
        if (impl->out_fmt.commonFormat == AVAudioPCMFormatInt16)
            src_bytes = (const uint8_t *)outbuf.int16ChannelData[0];
        else
            src_bytes = (const uint8_t *)outbuf.floatChannelData[0];

        size_t total_bytes = outbuf.frameLength * h->frame_bytes;
        size_t consumed_bytes = 0;
        size_t chunk_bytes_l  = h->chunk_frames * h->frame_bytes;
        while (consumed_bytes + chunk_bytes_l <= total_bytes) {
            ydev_vfd_push(&h->vfd, src_bytes + consumed_bytes);
            consumed_bytes += chunk_bytes_l;
        }
        /* Tail samples (<1 chunk) are dropped — at 10 ms chunks the
         * imprecision is below human perception and avoids a partial-
         * chunk slow path that the consumer would have to special-case. */
    }];

    h->impl = (void *)CFBridgingRetain(impl);
    return h;
}

ydev_result_t ydev_audio_in_start(ydev_audio_in_t *h)
{
    if (!h) return YDEV_INVALID_ARG;
    ydev_perm_status_t pst = ydev_perm_query_platform(YDEV_CAP_MIC);
    if (pst == YDEV_PERM_DENIED || pst == YDEV_PERM_RESTRICTED) return YDEV_DENIED;

    YdevAudioInImpl *impl = (__bridge YdevAudioInImpl *)h->impl;
    NSError *err = nil;
    if (![impl->engine startAndReturnError:&err]) {
        ydev_set_error("audio_in_start: %s",
                       err.localizedDescription.UTF8String ?: "engine failed");
        return YDEV_IO;
    }
    h->started = 1;
    return YDEV_OK;
}

ydev_result_t ydev_audio_in_stop(ydev_audio_in_t *h)
{
    if (!h) return YDEV_INVALID_ARG;
    YdevAudioInImpl *impl = (__bridge YdevAudioInImpl *)h->impl;
    [impl->engine stop];
    h->started = 0;
    ydev_vfd_close(&h->vfd);
    return YDEV_OK;
}

void ydev_audio_in_close(ydev_audio_in_t *h)
{
    if (!h) return;
    if (h->started) ydev_audio_in_stop(h);
    if (h->impl) {
        YdevAudioInImpl *impl = (YdevAudioInImpl *)CFBridgingRelease(h->impl);
        [impl->engine.inputNode removeTapOnBus:0];
        impl->owner = NULL;
        impl = nil;
    }
    ydev_vfd_destroy(&h->vfd);
    free(h);
}

int ydev_audio_in_fd(ydev_audio_in_t *h) { return h ? ydev_vfd_fd(&h->vfd) : -1; }

ssize_t ydev_audio_in_read(ydev_audio_in_t *h, void *buf, size_t bytes,
                           uint64_t *ts_ns, int timeout_ms)
{
    if (!h || !buf || bytes == 0) return -1;
    size_t chunk_bytes = h->chunk_frames * h->frame_bytes;

    size_t copied = 0;
    while (copied + chunk_bytes <= bytes) {
        ydev_result_t r = ydev_vfd_pop(&h->vfd, (uint8_t *)buf + copied,
                                       copied == 0 ? timeout_ms : 0);
        if (r == YDEV_AGAIN) break;
        if (r != YDEV_OK) { errno = EIO; return copied ? (ssize_t)copied : -1; }
        copied += chunk_bytes;
    }
    if (ts_ns) *ts_ns = ydev_now_ns();
    return (ssize_t)copied;
}

/* ── output ──────────────────────────────────────────────────────────── */

@interface YdevAudioOutImpl : NSObject
{
@public
    AVAudioEngine        *engine;
    AVAudioSourceNode    *source;
    AVAudioFormat        *fmt;
    struct ydev_audio_out *owner;
}
@end

struct ydev_audio_out {
    pthread_mutex_t       lock;
    pthread_cond_t        cond_room;
    uint8_t              *ring;
    size_t                ring_cap;        /* bytes                       */
    size_t                ring_used;
    size_t                ring_head;
    size_t                ring_tail;
    size_t                low_watermark;   /* bytes — fd readable below   */
    int                   pipe_r, pipe_w;
    bool                  closed;
    ydev_audio_config_t   cfg;
    size_t                frame_bytes;
    void                 *impl;
    int                   started;
};

@implementation YdevAudioOutImpl
@end

static void ring_drain_pipe(struct ydev_audio_out *h)
{
    char buf[8];
    while (read(h->pipe_r, buf, sizeof buf) > 0) {}
}

static void ring_kick_pipe(struct ydev_audio_out *h)
{
    char x = 1;
    ssize_t w = write(h->pipe_w, &x, 1);
    (void)w;
}

ydev_audio_out_t *ydev_audio_out_open(const ydev_audio_config_t *cfg)
{
    if (!cfg || cfg->rate_hz == 0 || cfg->channels == 0) {
        ydev_set_error("audio_out_open: invalid config");
        return NULL;
    }
    struct ydev_audio_out *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->cfg         = *cfg;
    h->frame_bytes = bytes_per_frame(cfg);
    /* Half a second of pre-buffer. Big enough that the audio thread
     * never starves on small jitter, small enough that the client
     * doesn't accumulate seconds of latency before audible glitches. */
    h->ring_cap       = h->frame_bytes * cfg->rate_hz / 2;
    h->low_watermark  = h->ring_cap / 4;
    h->ring           = calloc(1, h->ring_cap);
    pthread_mutex_init(&h->lock, NULL);
    pthread_cond_init(&h->cond_room, NULL);

    int p[2];
    if (pipe(p) != 0) { free(h->ring); free(h); return NULL; }
    int fl;
    fl = fcntl(p[0], F_GETFL, 0); fcntl(p[0], F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(p[1], F_GETFL, 0); fcntl(p[1], F_SETFL, fl | O_NONBLOCK);
    h->pipe_r = p[0]; h->pipe_w = p[1];
    /* Start with the pipe armed: room is available, so polling should
     * wake the client immediately to fill the buffer. */
    ring_kick_pipe(h);

    YdevAudioOutImpl *impl = [[YdevAudioOutImpl alloc] init];
    impl->engine = [[AVAudioEngine alloc] init];
    impl->fmt    = client_format(cfg);
    impl->owner  = h;

    impl->source = [[AVAudioSourceNode alloc] initWithFormat:impl->fmt
        renderBlock:^OSStatus(BOOL              *isSilence,
                              const AudioTimeStamp *ts,
                              AVAudioFrameCount   frameCount,
                              AudioBufferList    *outData) {
        (void)ts;
        size_t need = frameCount * h->frame_bytes;
        size_t given = 0;

        pthread_mutex_lock(&h->lock);
        bool was_above = (h->ring_used > h->low_watermark);
        uint8_t *dst = (uint8_t *)outData->mBuffers[0].mData;
        while (given < need && h->ring_used > 0) {
            size_t chunk = need - given;
            size_t to_end = h->ring_cap - h->ring_tail;
            if (chunk > h->ring_used) chunk = h->ring_used;
            if (chunk > to_end)       chunk = to_end;
            memcpy(dst + given, h->ring + h->ring_tail, chunk);
            h->ring_tail = (h->ring_tail + chunk) % h->ring_cap;
            h->ring_used -= chunk;
            given += chunk;
        }
        if (given < need) {
            memset(dst + given, 0, need - given);
            *isSilence = (given == 0) ? YES : NO;
        } else {
            *isSilence = NO;
        }
        outData->mBuffers[0].mDataByteSize = (UInt32)need;
        bool below = (h->ring_used <= h->low_watermark);
        pthread_cond_broadcast(&h->cond_room);
        pthread_mutex_unlock(&h->lock);

        if (was_above && below) ring_kick_pipe(h);
        return noErr;
    }];

    [impl->engine attachNode:impl->source];
    [impl->engine connect:impl->source to:impl->engine.mainMixerNode format:impl->fmt];

    h->impl = (void *)CFBridgingRetain(impl);
    return (ydev_audio_out_t *)h;
}

ydev_result_t ydev_audio_out_start(ydev_audio_out_t *ho)
{
    struct ydev_audio_out *h = (struct ydev_audio_out *)ho;
    if (!h) return YDEV_INVALID_ARG;
    YdevAudioOutImpl *impl = (__bridge YdevAudioOutImpl *)h->impl;
    NSError *err = nil;
    if (![impl->engine startAndReturnError:&err]) {
        ydev_set_error("audio_out_start: %s",
                       err.localizedDescription.UTF8String ?: "engine failed");
        return YDEV_IO;
    }
    h->started = 1;
    return YDEV_OK;
}

ydev_result_t ydev_audio_out_stop(ydev_audio_out_t *ho)
{
    struct ydev_audio_out *h = (struct ydev_audio_out *)ho;
    if (!h) return YDEV_INVALID_ARG;
    YdevAudioOutImpl *impl = (__bridge YdevAudioOutImpl *)h->impl;
    [impl->engine stop];
    h->started = 0;
    pthread_mutex_lock(&h->lock);
    h->closed = true;
    pthread_cond_broadcast(&h->cond_room);
    pthread_mutex_unlock(&h->lock);
    return YDEV_OK;
}

void ydev_audio_out_close(ydev_audio_out_t *ho)
{
    struct ydev_audio_out *h = (struct ydev_audio_out *)ho;
    if (!h) return;
    if (h->started) ydev_audio_out_stop(ho);
    if (h->impl) {
        YdevAudioOutImpl *impl = (YdevAudioOutImpl *)CFBridgingRelease(h->impl);
        impl->owner = NULL;
        impl = nil;
    }
    if (h->pipe_r >= 0) close(h->pipe_r);
    if (h->pipe_w >= 0) close(h->pipe_w);
    pthread_cond_destroy(&h->cond_room);
    pthread_mutex_destroy(&h->lock);
    free(h->ring);
    free(h);
}

int ydev_audio_out_fd(ydev_audio_out_t *ho)
{
    struct ydev_audio_out *h = (struct ydev_audio_out *)ho;
    return h ? h->pipe_r : -1;
}

ssize_t ydev_audio_out_write(ydev_audio_out_t *ho, const void *buf,
                             size_t bytes, int timeout_ms)
{
    struct ydev_audio_out *h = (struct ydev_audio_out *)ho;
    if (!h || !buf) return -1;

    struct timespec deadline; bool have_deadline = false;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec  += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        have_deadline = true;
    }

    pthread_mutex_lock(&h->lock);
    size_t written = 0;
    while (written < bytes && !h->closed) {
        size_t free_bytes = h->ring_cap - h->ring_used;
        if (free_bytes == 0) {
            if (timeout_ms == 0) break;
            if (have_deadline) {
                int r = pthread_cond_timedwait(&h->cond_room, &h->lock, &deadline);
                if (r == ETIMEDOUT) break;
            } else {
                pthread_cond_wait(&h->cond_room, &h->lock);
            }
            continue;
        }
        size_t chunk = bytes - written;
        if (chunk > free_bytes) chunk = free_bytes;
        size_t to_end = h->ring_cap - h->ring_head;
        if (chunk > to_end) chunk = to_end;
        memcpy(h->ring + h->ring_head, (const uint8_t *)buf + written, chunk);
        h->ring_head = (h->ring_head + chunk) % h->ring_cap;
        h->ring_used += chunk;
        written += chunk;
    }
    bool above_now = (h->ring_used > h->low_watermark);
    pthread_mutex_unlock(&h->lock);

    if (above_now) ring_drain_pipe(h);
    return (ssize_t)written;
}
