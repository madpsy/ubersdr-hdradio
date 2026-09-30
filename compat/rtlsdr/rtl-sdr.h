// Stand-in for librtlsdr's header.
//
// nrsc5's library can drive an RTL-SDR itself, so nrsc5.c calls librtlsdr
// directly. Here the samples arrive on stdin and are piped
// in, so those calls are never reached; src/rtlsdr_stub.c satisfies them
// without making the binary depend on librtlsdr.
#pragma once
#include <stdint.h>

typedef struct rtlsdr_dev rtlsdr_dev_t;
// rtltcp.c maps rtl_tcp's tuner codes through these.
enum rtlsdr_tuner {
    RTLSDR_TUNER_UNKNOWN = 0,
    RTLSDR_TUNER_E4000,
    RTLSDR_TUNER_FC0012,
    RTLSDR_TUNER_FC0013,
    RTLSDR_TUNER_FC2580,
    RTLSDR_TUNER_R820T,
    RTLSDR_TUNER_R828D
};

typedef void (*rtlsdr_read_async_cb_t)(unsigned char* buf, uint32_t len, void* ctx);

int rtlsdr_open(rtlsdr_dev_t** dev, uint32_t index);
int rtlsdr_close(rtlsdr_dev_t* dev);
int rtlsdr_set_center_freq(rtlsdr_dev_t* dev, uint32_t freq);
uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t* dev);
int rtlsdr_set_freq_correction(rtlsdr_dev_t* dev, int ppm);
int rtlsdr_get_tuner_gains(rtlsdr_dev_t* dev, int* gains);
int rtlsdr_set_tuner_gain(rtlsdr_dev_t* dev, int gain);
int rtlsdr_get_tuner_gain(rtlsdr_dev_t* dev);
int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t* dev, int manual);
int rtlsdr_set_sample_rate(rtlsdr_dev_t* dev, uint32_t rate);
int rtlsdr_set_direct_sampling(rtlsdr_dev_t* dev, int on);
int rtlsdr_set_offset_tuning(rtlsdr_dev_t* dev, int on);
int rtlsdr_reset_buffer(rtlsdr_dev_t* dev);
int rtlsdr_read_sync(rtlsdr_dev_t* dev, void* buf, int len, int* n_read);
int rtlsdr_read_async(rtlsdr_dev_t* dev, rtlsdr_read_async_cb_t cb, void* ctx, uint32_t buf_num, uint32_t buf_len);
int rtlsdr_cancel_async(rtlsdr_dev_t* dev);
int rtlsdr_set_bias_tee(rtlsdr_dev_t* dev, int on);
