// ����ͷ�ļ�
#include "vdev.h"

extern "C" {
#include "libavformat/avformat.h"
}

#include <ui/GraphicBufferMapper.h>

// �ڲ���������
#define DEF_VDEV_BUF_NUM        3
#define DEF_WIN_PIX_FMT         HAL_PIXEL_FORMAT_YCrCb_420_SP // HAL_PIXEL_FORMAT_RGBX_8888 or HAL_PIXEL_FORMAT_YCrCb_420_SP
#define VDEV_GRALLOC_USAGE      GRALLOC_USAGE_SW_READ_NEVER \
                                    | GRALLOC_USAGE_SW_WRITE_NEVER \
                                    | GRALLOC_USAGE_HW_TEXTURE

// �ڲ����Ͷ���
typedef struct
{
    // common members
    VDEV_COMMON_MEMBERS
    ANativeWindowBuffer **bufs;
} VDEVCTXT;

// �ڲ�����ʵ��
inline uint64_t get_tick_count(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

inline int android_pixfmt_to_ffmpeg_pixfmt(int srcfmt)
{
    // dst fmt
    int dst_fmt = 0;
    switch (srcfmt) {
    case HAL_PIXEL_FORMAT_RGB_565:      dst_fmt = AV_PIX_FMT_RGB565;  break;
    case HAL_PIXEL_FORMAT_RGBX_8888:    dst_fmt = AV_PIX_FMT_BGR32;   break;
    case HAL_PIXEL_FORMAT_YV12:         dst_fmt = AV_PIX_FMT_YUV420P; break;
    case HAL_PIXEL_FORMAT_YCrCb_420_SP: dst_fmt = AV_PIX_FMT_NV21;    break;
    }
    return dst_fmt;
}

static void* video_render_thread_proc(void *param)
{
    VDEVCTXT *c = (VDEVCTXT*)param;

    while (!(c->status & VDEV_CLOSE))
    {
        sem_wait(&c->semr);

        if (c->refresh_flag) {
            c->refresh_flag = 0;
            vdev_refresh_background(c);
        }

        int64_t apts = c->apts;
        int64_t vpts = c->vpts = c->ppts[c->head];
#if CLEAR_VDEV_WHEN_COMPLETED
        if (vpts != -1 && !(c->status & VDEV_COMPLETED)) {
#else
        if (vpts != -1) {
#endif
            ANativeWindow *win = (ANativeWindow*)c->pwnd;
            if (c->bufs[c->head] && 0 != win->queueBuffer(win, c->bufs[c->head], -1)) {
                av_log(NULL, AV_LOG_WARNING, "Surface::queueBuffer failed !\n");
            }
        }

        av_log(NULL, AV_LOG_DEBUG, "vpts: %lld\n", vpts);
        if (++c->head == c->bufnum) c->head = 0;
        sem_post(&c->semw);

        if (!(c->status & (VDEV_PAUSE|VDEV_COMPLETED))) {
            // send play progress event
            vdev_player_event(c, PLAY_PROGRESS, c->vpts > c->apts ? c->vpts : c->apts);

            //++ play completed ++//
            if (c->completed_apts != c->apts || c->completed_vpts != c->vpts) {
                c->completed_apts = c->apts;
                c->completed_vpts = c->vpts;
                c->completed_counter = 0;
            }
            else if (++c->completed_counter == 50) {
                av_log(NULL, AV_LOG_INFO, "play completed !\n");
                c->status |= VDEV_COMPLETED;
                vdev_player_event(c, PLAY_COMPLETED, 0);

#if CLEAR_VDEV_WHEN_COMPLETED
                // todo...
#endif
            }
            //-- play completed --//

            //++ frame rate & av sync control ++//
            uint64_t tickcur  = get_tick_count();
            int      tickdiff = tickcur - c->ticklast;
            int64_t  avdiff   = apts - vpts - c->tickavdiff;
            c->ticklast = tickcur;
            if (tickdiff - c->tickframe >  2) c->ticksleep--;
            if (tickdiff - c->tickframe < -2) c->ticksleep++;
            if (apts != -1 && vpts != -1) {
                if (avdiff > 5) c->ticksleep-=2;
                if (avdiff <-5) c->ticksleep+=2;
            }
            if (c->ticksleep < 0) c->ticksleep = 0;
            if (c->ticksleep > 0) usleep(c->ticksleep * 1000);
            av_log(NULL, AV_LOG_INFO, "d: %3lld, s: %d\n", avdiff, c->ticksleep);
            //-- frame rate & av sync control --//
        }
        else usleep(c->tickframe * 1000);
    }

    return NULL;
}

// �ӿں���ʵ��
void* vdev_android_create(void *win, int bufnum, int w, int h, int frate)
{
    VDEVCTXT *ctxt = new VDEVCTXT();
    if (!ctxt) {
        av_log(NULL, AV_LOG_ERROR, "failed to allocate vdev context !\n");
        exit(0);
    }

    // init vdev context
    bufnum          = bufnum ? bufnum : DEF_VDEV_BUF_NUM;
    ctxt->pwnd      = win;
    ctxt->bufnum    = bufnum;
    ctxt->pixfmt    = android_pixfmt_to_ffmpeg_pixfmt(DEF_WIN_PIX_FMT);
    ctxt->w         = w;
    ctxt->h         = h;
    ctxt->sw        = w;
    ctxt->sh        = h;
    ctxt->tickframe = 1000 / frate;
    ctxt->ticksleep = ctxt->tickframe;
    ctxt->apts      = -1;
    ctxt->vpts      = -1;

    native_window_set_usage             ((ANativeWindow*)win, VDEV_GRALLOC_USAGE);
    native_window_set_scaling_mode      ((ANativeWindow*)win, NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
    native_window_set_buffer_count      ((ANativeWindow*)win, bufnum);
    native_window_set_buffers_format    ((ANativeWindow*)win, DEF_WIN_PIX_FMT);
    native_window_set_buffers_dimensions((ANativeWindow*)win, w, h);

    // alloc buffer & semaphore
    ctxt->ppts = (int64_t*)calloc(bufnum, sizeof(int64_t));
    ctxt->bufs = (ANativeWindowBuffer**)calloc(bufnum, sizeof(ANativeWindowBuffer*));

    // create semaphore
    sem_init(&ctxt->semr, 0, 0     );
    sem_init(&ctxt->semw, 0, bufnum);

    // create video rendering thread
    pthread_create(&ctxt->thread, NULL, video_render_thread_proc, ctxt);
    return ctxt;
}

void vdev_android_destroy(void *ctxt)
{
    int i;
    VDEVCTXT *c = (VDEVCTXT*)ctxt;

    // make visual effect & rendering thread safely exit
    c->status = VDEV_CLOSE;
    sem_post(&c->semr);
    pthread_join(c->thread, NULL);

    // close semaphore
    sem_destroy(&c->semr);
    sem_destroy(&c->semw);

#if CLEAR_VDEV_WHEN_DESTROYED
    // clear window to background
    // todo...
#endif

    // free memory
    free(c->ppts);
    free(c->bufs);
    delete c;
}

void vdev_android_request(void *ctxt, void **buffer, int *stride)
{
    VDEVCTXT *c = (VDEVCTXT*)ctxt;

    sem_wait(&c->semw);
    ANativeWindowBuffer *buf = NULL;
    void                *dst = NULL;
    c->bufs[c->tail] = NULL;
    if (0 == native_window_dequeue_buffer_and_wait((ANativeWindow*)c->pwnd, &buf)) {
        GraphicBufferMapper &mapper = GraphicBufferMapper::get();
        Rect bounds(buf->width, buf->height);
        if (0 != mapper.lock(buf->handle, GRALLOC_USAGE_SW_WRITE_OFTEN, bounds, &dst)) {
            av_log(NULL, AV_LOG_WARNING, "ANativeWindow failed to lock buffer !\n");
        } else {
            c->bufs[c->tail] = buf;
        }
    } else {
        av_log(NULL, AV_LOG_WARNING, "ANativeWindow failed to dequeue buffer !\n");
    }

    if (buffer) *buffer = dst;
    if (stride) *stride = buf ? buf->width: 0;
    if (c->pixfmt == AV_PIX_FMT_BGR32) *stride *= 4;
}

void vdev_android_post(void *ctxt, int64_t pts)
{
    VDEVCTXT *c = (VDEVCTXT*)ctxt;

    ANativeWindowBuffer *buf    = c->bufs[c->tail];
    GraphicBufferMapper &mapper = GraphicBufferMapper::get();
    if (buf) mapper.unlock(buf->handle);

    c->ppts[c->tail] = pts;
    if (++c->tail == c->bufnum) c->tail = 0;
    sem_post(&c->semr);
}

void vdev_android_setrect(void *ctxt, int x, int y, int w, int h)
{
    VDEV_COMMON_CTXT *c = (VDEV_COMMON_CTXT*)ctxt;
    c->x  = x; c->y  = y;
    c->w  = w; c->h  = h;
    c->sw = w; c->sh = h;
    c->refresh_flag  = 1;
}

