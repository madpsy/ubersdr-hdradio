// nrsc5's rtl_tcp client, as far as its library needs it: every call fails.
// Like src/rtlsdr_stub.c, it exists so nrsc5.c links; the decoder only opens
// nrsc5 with nrsc5_open_pipe(). Leaving out rtltcp.c also leaves out nrsc5's
// only socket code, which does not build under MSVC's headers.
#include <stddef.h>
#include "rtltcp.h"

#define RTLTCP_DEFINE(name, opc) \
    int rtltcp_##name(rtltcp_t* st, unsigned int param) { (void)st; (void)param; return -1; }
RTLTCP_DEFINE(set_center_freq, 0x01)
RTLTCP_DEFINE(set_sample_rate, 0x02)
RTLTCP_DEFINE(set_tuner_gain_mode, 0x03)
RTLTCP_DEFINE(set_tuner_gain, 0x04)
RTLTCP_DEFINE(set_freq_correction, 0x05)
RTLTCP_DEFINE(set_direct_sampling, 0x09)
RTLTCP_DEFINE(set_offset_tuning, 0x0a)
RTLTCP_DEFINE(set_bias_tee, 0x0e)
#undef RTLTCP_DEFINE

rtltcp_t* rtltcp_open(int socket) { (void)socket; return NULL; }
void rtltcp_close(rtltcp_t* st) { (void)st; }
int rtltcp_read(rtltcp_t* st, uint8_t* buf, size_t cnt) { (void)st; (void)buf; (void)cnt; return -1; }
int rtltcp_get_tuner_gains(rtltcp_t* st, int* gains) { (void)st; (void)gains; return -1; }
int rtltcp_reset_buffer(rtltcp_t* st, size_t cnt) { (void)st; (void)cnt; return -1; }
