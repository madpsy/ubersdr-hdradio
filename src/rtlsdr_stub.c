// librtlsdr, as far as nrsc5's library needs it: every call fails. The decoder
// only ever opens nrsc5 with nrsc5_open_pipe(), which never touches a device;
// this exists so the library links. See compat/rtlsdr/rtl-sdr.h.
#include <rtl-sdr.h>

int rtlsdr_open(rtlsdr_dev_t** dev, uint32_t index) { (void)index; *dev = 0; return -1; }
int rtlsdr_close(rtlsdr_dev_t* dev) { (void)dev; return -1; }
int rtlsdr_set_center_freq(rtlsdr_dev_t* dev, uint32_t freq) { (void)dev; (void)freq; return -1; }
uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t* dev) { (void)dev; return 0; }
int rtlsdr_set_freq_correction(rtlsdr_dev_t* dev, int ppm) { (void)dev; (void)ppm; return -1; }
int rtlsdr_get_tuner_gains(rtlsdr_dev_t* dev, int* gains) { (void)dev; (void)gains; return -1; }
int rtlsdr_set_tuner_gain(rtlsdr_dev_t* dev, int gain) { (void)dev; (void)gain; return -1; }
int rtlsdr_get_tuner_gain(rtlsdr_dev_t* dev) { (void)dev; return 0; }
int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t* dev, int manual) { (void)dev; (void)manual; return -1; }
int rtlsdr_set_sample_rate(rtlsdr_dev_t* dev, uint32_t rate) { (void)dev; (void)rate; return -1; }
int rtlsdr_set_direct_sampling(rtlsdr_dev_t* dev, int on) { (void)dev; (void)on; return -1; }
int rtlsdr_set_offset_tuning(rtlsdr_dev_t* dev, int on) { (void)dev; (void)on; return -1; }
int rtlsdr_reset_buffer(rtlsdr_dev_t* dev) { (void)dev; return -1; }
int rtlsdr_read_sync(rtlsdr_dev_t* dev, void* buf, int len, int* n_read) { (void)dev; (void)buf; (void)len; *n_read = 0; return -1; }
int rtlsdr_read_async(rtlsdr_dev_t* dev, rtlsdr_read_async_cb_t cb, void* ctx, uint32_t buf_num, uint32_t buf_len) {
    (void)dev; (void)cb; (void)ctx; (void)buf_num; (void)buf_len;
    return -1;
}
int rtlsdr_cancel_async(rtlsdr_dev_t* dev) { (void)dev; return -1; }
int rtlsdr_set_bias_tee(rtlsdr_dev_t* dev, int on) { (void)dev; (void)on; return -1; }
