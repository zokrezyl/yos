/*
 * Linux audio backend — ALSA via libasound.
 *
 * Both directions use snd_pcm_*. The poll fd returned by
 * snd_pcm_poll_descriptors is itself what we expose as the public
 * ydev_audio_*_fd, so no vfd ring or self-pipe is needed on this
 * backend — ALSA already gives us pollable readiness.
 *
 * Build requires libasound (-lasound). The meson backend list pulls in
 * the alsa pkg-config dependency on linux.
 */

#include "../../impl/ydev/internal.h"
#include <yos/ydev/audio.h>

#include <alsa/asoundlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct ydev_audio_in  {
    snd_pcm_t          *pcm;
    int                 fd;          /* dup of ALSA's poll fd            */
    ydev_audio_config_t cfg;
    size_t              frame_bytes;
};

struct ydev_audio_out {
    snd_pcm_t          *pcm;
    int                 fd;
    ydev_audio_config_t cfg;
    size_t              frame_bytes;
};

static snd_pcm_format_t to_alsa_fmt(ydev_sample_format_t f)
{
    switch (f) {
    case YDEV_SAMPLE_S16: return SND_PCM_FORMAT_S16_LE;
    case YDEV_SAMPLE_F32: return SND_PCM_FORMAT_FLOAT_LE;
    }
    return SND_PCM_FORMAT_S16_LE;
}

static int open_pcm(snd_pcm_t **out, snd_pcm_stream_t stream,
                    const ydev_audio_config_t *cfg)
{
    snd_pcm_t *p = NULL;
    int err = snd_pcm_open(&p, "default", stream, SND_PCM_NONBLOCK);
    if (err < 0) {
        ydev_set_error("snd_pcm_open: %s", snd_strerror(err));
        return err;
    }
    snd_pcm_uframes_t period = cfg->frames_per_chunk
        ? cfg->frames_per_chunk : (cfg->rate_hz / 100); /* 10 ms */
    err = snd_pcm_set_params(p, to_alsa_fmt(cfg->format),
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             cfg->channels, cfg->rate_hz,
                             1 /* soft resample */, 100000 /* 100 ms latency */);
    if (err < 0) {
        ydev_set_error("snd_pcm_set_params: %s", snd_strerror(err));
        snd_pcm_close(p);
        return err;
    }
    (void)period;
    *out = p;
    return 0;
}

static int pcm_poll_fd(snd_pcm_t *p)
{
    struct pollfd pfd;
    int n = snd_pcm_poll_descriptors(p, &pfd, 1);
    if (n != 1) return -1;
    return dup(pfd.fd);
}

static size_t bpf(const ydev_audio_config_t *c)
{
    size_t bps = (c->format == YDEV_SAMPLE_F32) ? 4 : 2;
    return bps * c->channels;
}

/* ── input ───────────────────────────────────────────────────────────── */

ydev_audio_in_t *ydev_audio_in_open(const ydev_audio_config_t *cfg)
{
    if (!cfg || !cfg->rate_hz || !cfg->channels) return NULL;
    ydev_audio_in_t *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->cfg = *cfg;
    h->frame_bytes = bpf(cfg);
    if (open_pcm(&h->pcm, SND_PCM_STREAM_CAPTURE, cfg) < 0) { free(h); return NULL; }
    h->fd = pcm_poll_fd(h->pcm);
    return h;
}

ydev_result_t ydev_audio_in_start(ydev_audio_in_t *h)
{ return h && snd_pcm_start(h->pcm) >= 0 ? YDEV_OK : YDEV_IO; }
ydev_result_t ydev_audio_in_stop (ydev_audio_in_t *h)
{ if (h) snd_pcm_drop(h->pcm); return YDEV_OK; }
void          ydev_audio_in_close(ydev_audio_in_t *h)
{ if (!h) return; if (h->fd >= 0) close(h->fd); if (h->pcm) snd_pcm_close(h->pcm); free(h); }
int           ydev_audio_in_fd   (ydev_audio_in_t *h) { return h ? h->fd : -1; }

ssize_t ydev_audio_in_read(ydev_audio_in_t *h, void *buf, size_t bytes,
                           uint64_t *ts_ns, int timeout_ms)
{
    if (!h || !buf) { errno = EINVAL; return -1; }
    (void)timeout_ms;   /* client polls h->fd before calling */
    snd_pcm_uframes_t want = bytes / h->frame_bytes;
    snd_pcm_sframes_t got  = snd_pcm_readi(h->pcm, buf, want);
    if (got == -EAGAIN) { errno = EAGAIN; return -1; }
    if (got < 0) {
        if (snd_pcm_recover(h->pcm, (int)got, 1) < 0) { errno = EIO; return -1; }
        return 0;
    }
    if (ts_ns) *ts_ns = ydev_now_ns();
    return (ssize_t)got * (ssize_t)h->frame_bytes;
}

/* ── output ──────────────────────────────────────────────────────────── */

ydev_audio_out_t *ydev_audio_out_open(const ydev_audio_config_t *cfg)
{
    if (!cfg || !cfg->rate_hz || !cfg->channels) return NULL;
    ydev_audio_out_t *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->cfg = *cfg;
    h->frame_bytes = bpf(cfg);
    if (open_pcm(&h->pcm, SND_PCM_STREAM_PLAYBACK, cfg) < 0) { free(h); return NULL; }
    h->fd = pcm_poll_fd(h->pcm);
    return h;
}

ydev_result_t ydev_audio_out_start(ydev_audio_out_t *h)
{ return h ? YDEV_OK : YDEV_INVALID_ARG; }   /* ALSA starts on first write */
ydev_result_t ydev_audio_out_stop (ydev_audio_out_t *h)
{ if (h) snd_pcm_drain(h->pcm); return YDEV_OK; }
void          ydev_audio_out_close(ydev_audio_out_t *h)
{ if (!h) return; if (h->fd >= 0) close(h->fd); if (h->pcm) snd_pcm_close(h->pcm); free(h); }
int           ydev_audio_out_fd   (ydev_audio_out_t *h) { return h ? h->fd : -1; }

ssize_t ydev_audio_out_write(ydev_audio_out_t *h, const void *buf,
                             size_t bytes, int timeout_ms)
{
    if (!h || !buf) { errno = EINVAL; return -1; }
    (void)timeout_ms;
    snd_pcm_uframes_t want = bytes / h->frame_bytes;
    snd_pcm_sframes_t put  = snd_pcm_writei(h->pcm, buf, want);
    if (put == -EAGAIN) { errno = EAGAIN; return -1; }
    if (put < 0) {
        if (snd_pcm_recover(h->pcm, (int)put, 1) < 0) { errno = EIO; return -1; }
        return 0;
    }
    return (ssize_t)put * (ssize_t)h->frame_bytes;
}
