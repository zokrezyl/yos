/*
 * Android audio backend — AAudio.
 *
 * Input: data-callback delivers PCM blocks; we copy them into a vfd
 * ring sized for ~10 ms chunks.
 * Output: data-callback pulls from us; we expose a ring the client
 * writes into and signal "room available" via the public fd.
 */

#include "../../impl/ydev/internal.h"
#include <yos/ydev/audio.h>

#include <aaudio/AAudio.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static aaudio_format_t to_aaudio(ydev_sample_format_t f)
{
    return (f == YDEV_SAMPLE_F32) ? AAUDIO_FORMAT_PCM_FLOAT : AAUDIO_FORMAT_PCM_I16;
}
static size_t bpf(const ydev_audio_config_t *c)
{
    size_t bps = (c->format == YDEV_SAMPLE_F32) ? 4 : 2;
    return bps * c->channels;
}

/* ── input ───────────────────────────────────────────────────────────── */

struct ydev_audio_in {
    struct ydev_vfd     vfd;
    AAudioStream       *stream;
    ydev_audio_config_t cfg;
    size_t              frame_bytes;
    size_t              chunk_frames;
};

static aaudio_data_callback_result_t in_cb(AAudioStream *s, void *ud,
                                            void *audioData, int32_t numFrames)
{
    (void)s;
    ydev_audio_in_t *h = ud;
    size_t bytes = (size_t)numFrames * h->frame_bytes;
    size_t chunk = h->chunk_frames * h->frame_bytes;
    size_t off   = 0;
    while (off + chunk <= bytes) {
        ydev_vfd_push(&h->vfd, (const uint8_t *)audioData + off);
        off += chunk;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

ydev_audio_in_t *ydev_audio_in_open(const ydev_audio_config_t *cfg)
{
    if (!cfg || !cfg->rate_hz || !cfg->channels) return NULL;
    ydev_audio_in_t *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->cfg = *cfg;
    h->frame_bytes  = bpf(cfg);
    h->chunk_frames = cfg->frames_per_chunk ? cfg->frames_per_chunk : (cfg->rate_hz / 100);
    if (ydev_vfd_init(&h->vfd, h->chunk_frames * h->frame_bytes, 8, NULL) != 0) {
        free(h); return NULL;
    }

    AAudioStreamBuilder *b = NULL;
    AAudio_createStreamBuilder(&b);
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)cfg->rate_hz);
    AAudioStreamBuilder_setChannelCount(b, (int32_t)cfg->channels);
    AAudioStreamBuilder_setFormat(b, to_aaudio(cfg->format));
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, in_cb, h);

    if (AAudioStreamBuilder_openStream(b, &h->stream) != AAUDIO_OK) {
        AAudioStreamBuilder_delete(b);
        ydev_vfd_destroy(&h->vfd);
        free(h);
        return NULL;
    }
    AAudioStreamBuilder_delete(b);
    return h;
}

ydev_result_t ydev_audio_in_start(ydev_audio_in_t *h)
{ return h && AAudioStream_requestStart(h->stream) == AAUDIO_OK ? YDEV_OK : YDEV_IO; }
ydev_result_t ydev_audio_in_stop (ydev_audio_in_t *h)
{ if (h) AAudioStream_requestStop(h->stream); return YDEV_OK; }
void          ydev_audio_in_close(ydev_audio_in_t *h)
{ if (!h) return; if (h->stream) AAudioStream_close(h->stream); ydev_vfd_destroy(&h->vfd); free(h); }
int           ydev_audio_in_fd   (ydev_audio_in_t *h) { return h ? ydev_vfd_fd(&h->vfd) : -1; }

ssize_t ydev_audio_in_read(ydev_audio_in_t *h, void *buf, size_t bytes,
                           uint64_t *ts_ns, int timeout_ms)
{
    if (!h || !buf) { errno = EINVAL; return -1; }
    size_t chunk = h->chunk_frames * h->frame_bytes;
    size_t copied = 0;
    while (copied + chunk <= bytes) {
        ydev_result_t r = ydev_vfd_pop(&h->vfd, (uint8_t *)buf + copied,
                                       copied == 0 ? timeout_ms : 0);
        if (r == YDEV_AGAIN) break;
        if (r != YDEV_OK) { errno = EIO; return copied ? (ssize_t)copied : -1; }
        copied += chunk;
    }
    if (ts_ns) *ts_ns = ydev_now_ns();
    return (ssize_t)copied;
}

/* ── output ──────────────────────────────────────────────────────────── */

struct ydev_audio_out {
    AAudioStream       *stream;
    pthread_mutex_t     lock;
    pthread_cond_t      cond_room;
    uint8_t            *ring;
    size_t              ring_cap, ring_used, ring_head, ring_tail;
    size_t              low_watermark;
    int                 pipe_r, pipe_w;
    int                 closed;
    ydev_audio_config_t cfg;
    size_t              frame_bytes;
};

static aaudio_data_callback_result_t out_cb(AAudioStream *s, void *ud,
                                             void *audioData, int32_t numFrames)
{
    (void)s;
    ydev_audio_out_t *h = ud;
    size_t need = (size_t)numFrames * h->frame_bytes;
    size_t given = 0;
    pthread_mutex_lock(&h->lock);
    bool was_above = h->ring_used > h->low_watermark;
    uint8_t *dst = audioData;
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
    if (given < need) memset(dst + given, 0, need - given);
    bool below = h->ring_used <= h->low_watermark;
    pthread_cond_broadcast(&h->cond_room);
    pthread_mutex_unlock(&h->lock);
    if (was_above && below) { char x = 1; ssize_t w = write(h->pipe_w, &x, 1); (void)w; }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

ydev_audio_out_t *ydev_audio_out_open(const ydev_audio_config_t *cfg)
{
    if (!cfg || !cfg->rate_hz || !cfg->channels) return NULL;
    ydev_audio_out_t *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->cfg = *cfg;
    h->frame_bytes = bpf(cfg);
    h->ring_cap      = h->frame_bytes * cfg->rate_hz / 2;
    h->low_watermark = h->ring_cap / 4;
    h->ring          = calloc(1, h->ring_cap);
    pthread_mutex_init(&h->lock, NULL);
    pthread_cond_init(&h->cond_room, NULL);
    int p[2]; pipe(p);
    int fl;
    fl = fcntl(p[0], F_GETFL, 0); fcntl(p[0], F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(p[1], F_GETFL, 0); fcntl(p[1], F_SETFL, fl | O_NONBLOCK);
    h->pipe_r = p[0]; h->pipe_w = p[1];
    { char x = 1; ssize_t w = write(h->pipe_w, &x, 1); (void)w; }

    AAudioStreamBuilder *b = NULL;
    AAudio_createStreamBuilder(&b);
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)cfg->rate_hz);
    AAudioStreamBuilder_setChannelCount(b, (int32_t)cfg->channels);
    AAudioStreamBuilder_setFormat(b, to_aaudio(cfg->format));
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, out_cb, h);
    if (AAudioStreamBuilder_openStream(b, &h->stream) != AAUDIO_OK) {
        AAudioStreamBuilder_delete(b);
        free(h->ring); free(h);
        return NULL;
    }
    AAudioStreamBuilder_delete(b);
    return h;
}

ydev_result_t ydev_audio_out_start(ydev_audio_out_t *h)
{ return h && AAudioStream_requestStart(h->stream) == AAUDIO_OK ? YDEV_OK : YDEV_IO; }
ydev_result_t ydev_audio_out_stop (ydev_audio_out_t *h)
{ if (h) AAudioStream_requestStop(h->stream); return YDEV_OK; }
void          ydev_audio_out_close(ydev_audio_out_t *h)
{
    if (!h) return;
    if (h->stream) AAudioStream_close(h->stream);
    if (h->pipe_r >= 0) close(h->pipe_r);
    if (h->pipe_w >= 0) close(h->pipe_w);
    pthread_cond_destroy(&h->cond_room);
    pthread_mutex_destroy(&h->lock);
    free(h->ring); free(h);
}
int ydev_audio_out_fd(ydev_audio_out_t *h) { return h ? h->pipe_r : -1; }

ssize_t ydev_audio_out_write(ydev_audio_out_t *h, const void *buf,
                             size_t bytes, int timeout_ms)
{
    if (!h || !buf) { errno = EINVAL; return -1; }
    struct timespec deadline; bool have_deadline = false;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec  += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
        have_deadline = true;
    }
    pthread_mutex_lock(&h->lock);
    size_t written = 0;
    while (written < bytes && !h->closed) {
        size_t free_bytes = h->ring_cap - h->ring_used;
        if (free_bytes == 0) {
            if (timeout_ms == 0) break;
            int r = have_deadline
                ? pthread_cond_timedwait(&h->cond_room, &h->lock, &deadline)
                : pthread_cond_wait(&h->cond_room, &h->lock);
            if (r == ETIMEDOUT) break;
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
    bool above = h->ring_used > h->low_watermark;
    pthread_mutex_unlock(&h->lock);
    if (above) { char b[8]; while (read(h->pipe_r, b, sizeof b) > 0) {} }
    return (ssize_t)written;
}
