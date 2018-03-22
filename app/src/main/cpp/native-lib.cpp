#include <jni.h>
#include <string>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
#include "libavutil/opt.h"
#include <assert.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
};
#ifdef ANDROID

#include <android/log.h>

#define LOGE(format, ...)  __android_log_print(ANDROID_LOG_ERROR, "(>_<)", format, ##__VA_ARGS__)
#define LOGI(format, ...)  __android_log_print(ANDROID_LOG_INFO,  "(^_^)", format, ##__VA_ARGS__)
#else
#define LOGE(format, ...)  printf("(>_<) " format "\n", ##__VA_ARGS__)
#define LOGI(format, ...)  printf("(^_^) " format "\n", ##__VA_ARGS__)
#endif

static void *nextBuffer;
static int nextSize;
static AVPacket packet;
static AVFrame *pFrame;
static AVCodecContext *pCodecCtx;
static SwrContext *swr;
static AVFormatContext *pFormatCtx;
static int audioindex;
uint8_t *outputBuffer;

// engine interfaces
static SLObjectItf engineObject = NULL;
static SLEngineItf engineEngine;
static SLObjectItf outputMixObject = NULL;
static SLEnvironmentalReverbItf outputMixEnvironmentalReverb = NULL;
static SLObjectItf bqPlayerObject = NULL;
static SLPlayItf bqPlayerPlay = NULL;
static SLAndroidSimpleBufferQueueItf bqPlayerBufferQueue;
static SLEffectSendItf bqPlayerEffectSend;
static SLMuteSoloItf bqPlayerMuteSolo;
static SLVolumeItf bqPlayerVolume;
static SLmilliHertz bqPlayerSampleRate = 0;
static jint bqPlayerBufSize = 0;
static short *resampleBuf = NULL;
static pthread_mutex_t audioEngineLock = PTHREAD_MUTEX_INITIALIZER;
// aux effect on the output mix, used by the buffer queue player
static const SLEnvironmentalReverbSettings reverbSettings =
        SL_I3DL2_ENVIRONMENT_PRESET_STONECORRIDOR;

// 释放相关资源
void releaseFFmpegAudioPlay() {
    av_packet_unref(&packet);
    av_free(outputBuffer);
    av_free(pFrame);
    avcodec_close(pCodecCtx);
    avformat_close_input(&pFormatCtx);
}

// shut down the native audio system
void shutdown() {
    // destroy buffer queue audio player object, and invalidate all associated interfaces
    if (bqPlayerObject != NULL) {
        (*bqPlayerObject)->Destroy(bqPlayerObject);
        bqPlayerObject = NULL;
        bqPlayerPlay = NULL;
        bqPlayerBufferQueue = NULL;
        bqPlayerEffectSend = NULL;
        bqPlayerMuteSolo = NULL;
        bqPlayerVolume = NULL;
    }

    // destroy output mix object, and invalidate all associated interfaces
    if (outputMixObject != NULL) {
        (*outputMixObject)->Destroy(outputMixObject);
        outputMixObject = NULL;
        outputMixEnvironmentalReverb = NULL;
    }

    // destroy engine object, and invalidate all associated interfaces
    if (engineObject != NULL) {
        (*engineObject)->Destroy(engineObject);
        engineObject = NULL;
        engineEngine = NULL;
    }

    // 释放FFmpeg解码器相关资源
    releaseFFmpegAudioPlay();
}


int getPCM() {
    while (av_read_frame(pFormatCtx, &packet) >= 0) {
        if (packet.stream_index == audioindex) {
            int ret = avcodec_send_packet(pCodecCtx, &packet);
            int timestamp = packet.pts * av_q2d(pFormatCtx->streams[audioindex]->time_base);
            int iHour = timestamp / 3600;//小时
            int iMinute = timestamp % 3600 / 60;//分钟
            int iSecond = timestamp % 60;//秒
            LOGI("时间：%02d:%02d:%02d\n", iHour, iMinute, iSecond);

            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                break;
            ret = avcodec_receive_frame(pCodecCtx, pFrame);
            if (ret < 0 && ret != AVERROR_EOF)
                break;
            //处理不同的格式
            if (pCodecCtx->sample_fmt == AV_SAMPLE_FMT_S16P) {
                nextSize = av_samples_get_buffer_size(pFrame->linesize, pCodecCtx->channels,
                                                      pCodecCtx->frame_size, pCodecCtx->sample_fmt,
                                                      1);
            } else {
                av_samples_get_buffer_size(&nextSize, pCodecCtx->channels, pCodecCtx->frame_size,
                                           pCodecCtx->sample_fmt, 1);
            }
            // 音频格式转换
            swr_convert(swr, &outputBuffer, pFrame->nb_samples,
                        (uint8_t const **) (pFrame->extended_data),
                        pFrame->nb_samples);
            nextBuffer = outputBuffer;
            av_packet_unref(&packet);
            return 0;
        }
        av_packet_unref(&packet);
    }
    shutdown();
    return -1;
}

//创建OpenSLES引擎
extern "C"
void createEngine() {
    SLresult result;
    //创建引擎
    result = slCreateEngine(&engineObject, 0, NULL, 0, NULL, NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
    //关联引擎
    result = (*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
    //获取引擎接口, which is needed in order to create other objects
    result = (*engineObject)->GetInterface(engineObject, SL_IID_ENGINE, &engineEngine);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
    //创建输出混音器, with environmental reverb specified as a non-required interface
    const SLInterfaceID ids[1] = {SL_IID_ENVIRONMENTALREVERB};
    const SLboolean req[1] = {SL_BOOLEAN_FALSE};
    result = (*engineEngine)->CreateOutputMix(engineEngine, &outputMixObject, 1, ids, req);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
    //关联输出混音器
    result = (*outputMixObject)->Realize(outputMixObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
    // get the environmental reverb interface
    // this could fail if the environmental reverb effect is not available,
    // either because the feature is not present, excessive CPU load, or
    // the required MODIFY_AUDIO_SETTINGS permission was not requested and granted
    //获取reverb接口
    result = (*outputMixObject)->GetInterface(outputMixObject, SL_IID_ENVIRONMENTALREVERB,
                                              &outputMixEnvironmentalReverb);
    if (SL_RESULT_SUCCESS == result) {
        result = (*outputMixEnvironmentalReverb)->SetEnvironmentalReverbProperties(
                outputMixEnvironmentalReverb, &reverbSettings);
        (void) result;
    }
    // ignore unsuccessful result codes for environmental reverb, as it is optional for this example
}

void releaseResampleBuf(void) {
    if (0 == bqPlayerSampleRate) {
        /*
         * we are not using fast path, so we were not creating buffers, nothing to do
         */
        return;
    }
    free(resampleBuf);
    resampleBuf = NULL;
}

// this callback handler is called every time a buffer finishes playing
extern "C"
void bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context) {
    assert(bq == bqPlayerBufferQueue);
    assert(NULL == context);
    // for streaming playback, replace this test by logic to find and fill the next buffer
    if (getPCM() < 0)//解码音频文件
        return;
    if (NULL != nextBuffer && 0 != nextSize) {
        SLresult result;
        // enqueue another buffer
        result = (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, nextBuffer, nextSize);
        // the most likely other result is SL_RESULT_BUFFER_INSUFFICIENT,
        // which for this code example would indicate a programming error
        if (SL_RESULT_SUCCESS != result) {
            pthread_mutex_unlock(&audioEngineLock);
        }
        (void) result;
    } else {
        releaseResampleBuf();
        pthread_mutex_unlock(&audioEngineLock);
    }
}

// create buffer queue audio player
extern "C"
void createBufferQueueAudioPlayer(int sampleRate, int channel) {
    SLresult result;
    if (sampleRate >= 0) {
        bqPlayerSampleRate = sampleRate * 1000;

    }
    //配置音频源
    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM format_pcm = {SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_8,
                                   SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                                   SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
    if (bqPlayerSampleRate) {
        format_pcm.samplesPerSec = bqPlayerSampleRate;       //sample rate in mili second
    }
    format_pcm.numChannels = (SLuint32) channel;
    if (channel == 2) {
        format_pcm.channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;
    } else {
        format_pcm.channelMask = SL_SPEAKER_FRONT_CENTER;
    }
    SLDataSource audioSrc = {&loc_bufq, &format_pcm};

    //配置音频池
    SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, outputMixObject};
    SLDataSink audioSnk = {&loc_outmix, NULL};
    /*
     * create audio player:
     *     fast audio does not support when SL_IID_EFFECTSEND is required, skip it
     *     for fast audio case
     */
    const SLInterfaceID ids[3] = {SL_IID_BUFFERQUEUE, SL_IID_VOLUME, SL_IID_EFFECTSEND,
            /*SL_IID_MUTESOLO,*/};
    const SLboolean req[3] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE,
            /*SL_BOOLEAN_TRUE,*/ };
    //创建音频播放器
    result = (*engineEngine)->CreateAudioPlayer(engineEngine, &bqPlayerObject, &audioSrc, &audioSnk,
                                                bqPlayerSampleRate ? 2 : 3, ids, req);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
    // 关联播放器
    result = (*bqPlayerObject)->Realize(bqPlayerObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
    // 获取播放接口
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_PLAY, &bqPlayerPlay);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
    // 获取缓冲队列接口
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_BUFFERQUEUE,
                                             &bqPlayerBufferQueue);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
    // 注册缓冲队列回调
    result = (*bqPlayerBufferQueue)->RegisterCallback(bqPlayerBufferQueue, bqPlayerCallback, NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
    // 获取音效接口
    bqPlayerEffectSend = NULL;
    if (0 == bqPlayerSampleRate) {
        result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_EFFECTSEND,
                                                 &bqPlayerEffectSend);
        assert(SL_RESULT_SUCCESS == result);
        (void) result;
    }
#if 0   // mute/solo is not supported for sources that are known to be mono, as this is
    // get the mute/solo interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_MUTESOLO, &bqPlayerMuteSolo);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
#endif
    // 获取音量接口
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_VOLUME, &bqPlayerVolume);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
    // 开始播放音乐
    result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PLAYING);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
}

extern "C"
//int createFFmpegAudioPlay(const char *file_name) {
int Java_com_lake_ndkaudiotest_MainActivity_play(JNIEnv *env, jclass clazz, jstring url) {

    int i;
    AVCodec *pCodec;
    //读取输入的音频文件地址
    const char *file_name = env->GetStringUTFChars(url, NULL);
    LOGI("file_name:%s\n", file_name);
    //初始化
    av_register_all();
    //分配一个AVFormatContext结构
    pFormatCtx = avformat_alloc_context();
    //打开文件
    if (avformat_open_input(&pFormatCtx, file_name, NULL, NULL) != 0) {
        LOGE("Couldn't open input stream.\n");
        return -1;
    }
    //查找文件的流信息
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        LOGE("Couldn't find stream information.\n");
        return -1;
    }
    //在流信息中找到音频流
    audioindex = -1;
    for (i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioindex = i;
            break;
        }
    }
    if (audioindex == -1) {
        LOGE("Couldn't find a video stream.\n");
        return -1;
    }
    //获取相应音频流的解码器
    AVCodecParameters *pCodecPar = pFormatCtx->streams[audioindex]->codecpar;
    pCodec = avcodec_find_decoder(pCodecPar->codec_id);
    assert(pCodec != NULL);
    pCodecCtx = avcodec_alloc_context3(pCodec);
    // Copy context
    if (avcodec_parameters_to_context(pCodecCtx, pCodecPar) != 0) {
        fprintf(stderr, "Couldn't copy codec context");
        return -1; // Error copying codec context
    }
    //打开解码器
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
        LOGE("Couldn't open codec.\n");
        return -1;
    }
    //分配一个帧指针，指向解码后的原始帧
    pFrame = av_frame_alloc();
    //packet = (AVPacket *) av_malloc(sizeof(AVPacket));
    //设置格式转换
    swr = swr_alloc();
    swr = swr_alloc_set_opts(NULL,
                             pCodecCtx->channel_layout,
                             AV_SAMPLE_FMT_S16,
                             pCodecCtx->sample_rate,
                             pCodecCtx->channel_layout,
                             pCodecCtx->sample_fmt,
                             pCodecCtx->sample_rate,
                             0, NULL);
    if (!swr || swr_init(swr) < 0) {
        swr_free(&swr);
        return -1;
    }
    swr_init(swr);
    //分配输入缓存
    int outputBufferSize = 8192;
    outputBuffer = (uint8_t *) malloc(sizeof(uint8_t) * outputBufferSize);
    // 创建播放引擎
    createEngine();
    // 创建缓冲队列音频播放器
    createBufferQueueAudioPlayer(pCodecCtx->sample_rate, pCodecCtx->channels);
    // 启动音频播放
    bqPlayerCallback(bqPlayerBufferQueue, NULL);
    return 0;
}

extern "C"
int Java_com_lake_ndkaudiotest_MainActivity_stop(JNIEnv *env, jclass clazz) {
    shutdown();
    return 0;
}

extern "C"
int Java_com_lake_ndkaudiotest_MainActivity_pause(JNIEnv *env, jclass clazz, jboolean isPlaying) {
    SLresult result;
    // make sure the asset audio player was created
    // 暂停音乐
    if (NULL != bqPlayerPlay) {
        result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, isPlaying ? SL_PLAYSTATE_PAUSED
                                                                       : SL_PLAYSTATE_PLAYING);
        assert(SL_RESULT_SUCCESS == result);
        (void) result;
    }
    return 0;
}





