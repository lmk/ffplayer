// ����ͷ�ļ�
#include <windows.h>
#include "pktqueue.h"
#include "coreplayer.h"
#include "corerender.h"
#include "log.h"

extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
}

// �ڲ���������
#define avcodec_decode_video avcodec_decode_video2
#define avcodec_decode_audio avcodec_decode_audio4

// �ڲ����Ͷ���
typedef struct
{
    // audio
    AVFormatContext *pAVFormatContext;
    AVCodecContext  *pAudioCodecContext;
    int              iAudioStreamIndex;
    double           dAudioTimeBase;

    // video
    AVCodecContext  *pVideoCodecContext;
    int              iVideoStreamIndex;
    double           dVideoTimeBase;

    // render
    int              nRenderMode;
    HANDLE           hCoreRender;

    // thread
    #define PS_D_PAUSE  (1 << 0)  // demux pause
    #define PS_A_PAUSE  (1 << 1)  // audio decoding pause
    #define PS_V_PAUSE  (1 << 2)  // video decoding pause
    #define PS_R_PAUSE  (1 << 3)  // rendering pause
    #define PS_CLOSE    (1 << 4)  // close player
    int              nPlayerStatus;
    HANDLE           hAVDemuxThread;
    HANDLE           hADecodeThread;
    HANDLE           hVDecodeThread;

    // packet queue
    PKTQUEUE         PacketQueue;
} PLAYER;

// �ڲ�����ʵ��
static DWORD WINAPI AVDemuxThreadProc(PLAYER *player)
{
    AVPacket *packet = NULL;
    int       retv   = 0;

    while (!(player->nPlayerStatus & PS_CLOSE))
    {
        //++ when demux pause ++//
        if (player->nPlayerStatus & PS_D_PAUSE) {
            Sleep(20);
            continue;
        }
        //-- when demux pause --//

        pktqueue_write_request(&(player->PacketQueue), &packet);
        retv = av_read_frame(player->pAVFormatContext, packet);

        //++ play completed ++//
        if (retv < 0)
        {
            packet->pts = -1; // video packet pts == -1, means completed
            pktqueue_write_done_a(&(player->PacketQueue));
            player->nPlayerStatus |= PS_D_PAUSE;
            continue;
        }
        //-- play completed --//

        // audio
        if (packet->stream_index == player->iAudioStreamIndex)
        {
            pktqueue_write_done_a(&(player->PacketQueue));
        }

        // video
        if (packet->stream_index == player->iVideoStreamIndex)
        {
            pktqueue_write_done_v(&(player->PacketQueue));
        }

        if (  packet->stream_index != player->iAudioStreamIndex
           && packet->stream_index != player->iVideoStreamIndex )
        {
            av_free_packet(packet); // free packet
            pktqueue_write_release(&(player->PacketQueue));
        }
    }

    return 0;
}

static DWORD WINAPI AudioDecodeThreadProc(PLAYER *player)
{
    AVPacket *packet   = NULL;
    AVFrame  *aframe   = NULL;
    int       consumed = 0;
    int       gotaudio = 0;

    aframe = av_frame_alloc();
    if (!aframe) return 0;

    while (!(player->nPlayerStatus & PS_CLOSE))
    {
        //++ when audio decoding pause ++//
        if (player->nPlayerStatus & PS_A_PAUSE) {
            Sleep(20);
            continue;
        }
        //-- when audio decoding pause --//

        // read packet
        pktqueue_read_request_a(&(player->PacketQueue), &packet);

        //++ play completed ++//
        if (packet->pts == -1) {
            renderaudiowrite(player->hCoreRender, (AVFrame*)-1);
            pktqueue_read_done_a(&(player->PacketQueue));
            continue;
        }
        //-- play completed --//

        //++ decode audio packet ++//
        consumed = gotaudio = 0;
        while (packet->size > 0) {
            if (player->iAudioStreamIndex != -1) {
                consumed = avcodec_decode_audio(player->pAudioCodecContext, aframe, &gotaudio, packet);
            }

            if (consumed < 0) {
                log_printf(TEXT("an error occurred during decoding audio.\n"));
                break;
            }

            if (gotaudio) {
                aframe->pts = (int64_t)(packet->pts * player->dAudioTimeBase);
                renderaudiowrite(player->hCoreRender, aframe);
            }
            packet->data += consumed;
            packet->size -= consumed;
        }
        //-- decode audio packet --//

        // free packet
        av_free_packet(packet);

        pktqueue_read_done_a(&(player->PacketQueue));
    }

    av_frame_free(&aframe);
    return 0;
}

static DWORD WINAPI VideoDecodeThreadProc(PLAYER *player)
{
    AVPacket *packet   = NULL;
    AVFrame  *vframe   = NULL;
    int       gotvideo = 0;

    vframe = av_frame_alloc();
    if (!vframe) return 0;

    while (!(player->nPlayerStatus & PS_CLOSE))
    {
        //++ when video decoding pause ++//
        if (player->nPlayerStatus & PS_V_PAUSE) {
            Sleep(20);
            continue;
        }
        //-- when video decoding pause --//

        // read packet
        pktqueue_read_request_v(&(player->PacketQueue), &packet);

        //++ decode video packet ++//
        gotvideo = 0;
        if (player->iVideoStreamIndex != -1) {
            avcodec_decode_video(player->pVideoCodecContext, vframe, &gotvideo, packet);
        }

        if (gotvideo) {
            vframe->pts = (int64_t)(packet->pts * player->dVideoTimeBase);
            rendervideowrite(player->hCoreRender, vframe);
        }
        //-- decode video packet --//

        // free packet
        av_free_packet(packet);

        pktqueue_read_done_v(&(player->PacketQueue));
    }

    av_frame_free(&vframe);
    return 0;
}

// ����ʵ��
HANDLE playeropen(char *file, HWND hwnd)
{
    PLAYER        *player   = NULL;
    AVCodec       *pAVCodec = NULL;
    int            vformat  = 0;
    int            width    = 0;
    int            height   = 0;
    AVRational     vrate    = {1, 1};
    uint64_t       alayout  = 0;
    int            aformat  = 0;
    int            arate    = 0;
    uint32_t       i        = 0;

    // init log
    log_init(TEXT("DEBUGER"));

    // av register all
    av_register_all();

    // alloc player context
    player = (PLAYER*)malloc(sizeof(PLAYER));
    memset(player, 0, sizeof(PLAYER));

    // create packet queue
    pktqueue_create(&(player->PacketQueue));

    // open input file
    if (avformat_open_input(&(player->pAVFormatContext), file, NULL, 0) != 0) {
        goto error_handler;
    }

    // find stream info
    if (avformat_find_stream_info(player->pAVFormatContext, NULL) < 0) {
        goto error_handler;
    }

    // get video & audio codec context
    player->iAudioStreamIndex = -1;
    player->iVideoStreamIndex = -1;
    for (i=0; i<player->pAVFormatContext->nb_streams; i++)
    {
        switch (player->pAVFormatContext->streams[i]->codec->codec_type)
        {
        case AVMEDIA_TYPE_AUDIO:
            player->iAudioStreamIndex  = i;
            player->pAudioCodecContext = player->pAVFormatContext->streams[i]->codec;
            player->dAudioTimeBase     = av_q2d(player->pAVFormatContext->streams[i]->time_base) * 1000;
            break;

        case AVMEDIA_TYPE_VIDEO:
            player->iVideoStreamIndex  = i;
            player->pVideoCodecContext = player->pAVFormatContext->streams[i]->codec;
            player->dVideoTimeBase     = av_q2d(player->pAVFormatContext->streams[i]->time_base) * 1000;
            vrate = player->pAVFormatContext->streams[i]->r_frame_rate;
            break;
        }
    }

    // open audio codec
    if (player->iAudioStreamIndex != -1)
    {
        pAVCodec = avcodec_find_decoder(player->pAudioCodecContext->codec_id);
        if (pAVCodec)
        {
            if (avcodec_open2(player->pAudioCodecContext, pAVCodec, NULL) < 0)
            {
                player->iAudioStreamIndex = -1;
            }
        }
        else player->iAudioStreamIndex = -1;
    }

    // open video codec
    if (player->iVideoStreamIndex != -1)
    {
        pAVCodec = avcodec_find_decoder(player->pVideoCodecContext->codec_id);
        if (pAVCodec)
        {
            if (avcodec_open2(player->pVideoCodecContext, pAVCodec, NULL) < 0)
            {
                player->iVideoStreamIndex = -1;
            }
        }
        else player->iVideoStreamIndex = -1;
    }

    // for video
    if (player->iVideoStreamIndex != -1)
    {
        vformat = player->pVideoCodecContext->pix_fmt;
        width   = player->pVideoCodecContext->width;
        height  = player->pVideoCodecContext->height;
    }

    // for audio
    if (player->iAudioStreamIndex != -1)
    {
        alayout = player->pAudioCodecContext->channel_layout;
        aformat = player->pAudioCodecContext->sample_fmt;
        arate   = player->pAudioCodecContext->sample_rate;
    }

    // open core render
    player->hCoreRender = renderopen(hwnd, vrate, vformat, width, height,
        alayout, (AVSampleFormat)aformat, arate);

    return player;

error_handler:
    playerclose((HANDLE)player);
    return NULL;
}

void playerclose(HANDLE hplayer)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;

    player->nPlayerStatus = PS_CLOSE;
    renderstart(player->hCoreRender);

    //++ make sure packet queue not empty ++//
    if (pktqueue_isempty_a(&(player->PacketQueue))) {
        pktqueue_write_request(&(player->PacketQueue), NULL);
        pktqueue_write_done_a(&(player->PacketQueue));
    }

    if (pktqueue_isempty_v(&(player->PacketQueue))) {
        pktqueue_write_request(&(player->PacketQueue), NULL);
        pktqueue_write_done_v(&(player->PacketQueue));
    }
    //-- make sure packet queue not empty --//

    // wait and close audio/video demuxing thread handle
    if (player->hAVDemuxThread)
    {
        WaitForSingleObject(player->hAVDemuxThread, -1);
        CloseHandle(player->hAVDemuxThread);
        player->hAVDemuxThread = NULL;
    }

    // wait and close audio decoding thread handle
    if (player->hADecodeThread)
    {
        WaitForSingleObject(player->hADecodeThread, -1);
        CloseHandle(player->hADecodeThread);
        player->hADecodeThread = NULL;
    }

    // wait and close video decoding thread handle
    if (player->hVDecodeThread)
    {
        WaitForSingleObject(player->hVDecodeThread, -1);
        CloseHandle(player->hVDecodeThread);
        player->hVDecodeThread = NULL;
    }

    // destroy packet queue
    pktqueue_destroy(&(player->PacketQueue));

    if (player->hCoreRender       ) renderclose(player->hCoreRender);
    if (player->pVideoCodecContext) avcodec_close(player->pVideoCodecContext);
    if (player->pAudioCodecContext) avcodec_close(player->pAudioCodecContext);
    if (player->pAVFormatContext  ) avformat_close_input(&(player->pAVFormatContext));

    free(player);

    // close log
    log_done();
}

void playerplay(HANDLE hplayer)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;
    player->nPlayerStatus = 0;

    // create audio/video demuxing thread
    if (!player->hAVDemuxThread) {
        player->hAVDemuxThread = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)AVDemuxThreadProc, player, 0, 0);
    }

    // create audio decoding thread
    if (!player->hADecodeThread) {
        player->hADecodeThread = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)AudioDecodeThreadProc, player, 0, 0);
    }

    // create video decoding thread
    if (!player->hVDecodeThread) {
        player->hVDecodeThread = CreateThread(0, 0, (LPTHREAD_START_ROUTINE)VideoDecodeThreadProc, player, 0, 0);
    }

    // render start
    renderstart(player->hCoreRender);
}

void playerpause(HANDLE hplayer)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;
    player->nPlayerStatus |= PS_R_PAUSE;
    renderpause(player->hCoreRender);
}

void playersetrect(HANDLE hplayer, int x, int y, int w, int h)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;

    int vw, vh;
    int rw, rh;
    playergetparam(hplayer, PARAM_VIDEO_WIDTH , &vw);
    playergetparam(hplayer, PARAM_VIDEO_HEIGHT, &vh);
    if (!vw || !vh) return;

    switch (player->nRenderMode)
    {
    case RENDER_LETTERBOX:
        if (w * vh < h * vw) { rw = w; rh = rw * vh / vw; }
        else                 { rh = h; rw = rh * vw / vh; }
        break;

    case RENDER_STRETCHED:
        rw = w;
        rh = h;
        break;
    }
    rendersetrect(player->hCoreRender, x + (w - rw) / 2, y + (h - rh) / 2, rw, rh);
}

void playerseek(HANDLE hplayer, DWORD sec)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;

    // start render if paused
    if (player->nPlayerStatus & PS_R_PAUSE) renderstart(player->hCoreRender);

    // render seek start
    player->nPlayerStatus |= PS_D_PAUSE;
    renderseek(player->hCoreRender, sec);

    // wait for packet queue empty
    while (!pktqueue_isempty_a(&(player->PacketQueue))) Sleep(20);
    while (!pktqueue_isempty_v(&(player->PacketQueue))) Sleep(20);

    // seek frame
    av_seek_frame(player->pAVFormatContext, -1, (int64_t)sec * AV_TIME_BASE, 0);
    if (player->iAudioStreamIndex != -1) avcodec_flush_buffers(player->pAudioCodecContext);
    if (player->iVideoStreamIndex != -1) avcodec_flush_buffers(player->pVideoCodecContext);

    // render seek done, -1 means done
    renderseek(player->hCoreRender, -1);
    player->nPlayerStatus &= ~PS_D_PAUSE;

    // wait for video packet queue not empty witch timeout 200ms
    int i = 10; while (i-- && pktqueue_isempty_v(&(player->PacketQueue))) Sleep(20);

    // pause render if needed
    if (player->nPlayerStatus & PS_R_PAUSE) renderpause(player->hCoreRender);
}

void playersetparam(HANDLE hplayer, DWORD id, DWORD param)
{
    if (!hplayer) return;
    PLAYER *player = (PLAYER*)hplayer;

    switch (id)
    {
    case PARAM_RENDER_MODE:
        player->nRenderMode = param;
        break;
    }
}

void playergetparam(HANDLE hplayer, DWORD id, void *param)
{
    if (!hplayer || !param) return;
    PLAYER *player = (PLAYER*)hplayer;

    switch (id)
    {
    case PARAM_VIDEO_WIDTH:
        if (!player->pVideoCodecContext) *(int*)param = 0;
        else *(int*)param = player->pVideoCodecContext->width;
        break;

    case PARAM_VIDEO_HEIGHT:
        if (!player->pVideoCodecContext) *(int*)param = 0;
        else *(int*)param = player->pVideoCodecContext->height;
        break;

    case PARAM_VIDEO_DURATION:
        if (!player->pAVFormatContext) *(DWORD*)param = 0;
        else *(DWORD*)param = (DWORD)(player->pAVFormatContext->duration / AV_TIME_BASE);
        break;

    case PARAM_VIDEO_POSITION:
        rendertime(player->hCoreRender, (DWORD*)param);
        break;

    case PARAM_RENDER_MODE:
        *(int*)param = player->nRenderMode;
        break;
    }
}




