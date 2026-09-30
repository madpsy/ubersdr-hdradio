#include "hd_decoder.h"

#include <cmath>
#include <cstring>

// Taps for the audio resampler: HD Radio audio reaches ~15-20 kHz, so this is
// longer than the IQ one to keep the band edge sharp at 44.1 kHz.
static const int AUDIO_TAPS = 48;
static const int IQ_TAPS = 32;
// Samples piped into nrsc5 per call; it processes them synchronously.
static const int PIPE_CHUNK = 32768;

static std::string str(const char* s) { return s ? std::string(s) : std::string(); }

// Type 0 is "undefined", which nrsc5 names "None": better shown as nothing.
static void setType(HdDecoder::Program& prog, unsigned int type) {
    prog.type = type;
    const char* name = nullptr;
    if (type != NRSC5_PROGRAM_TYPE_UNDEFINED) { nrsc5_program_type_name(type, &name); }
    prog.typeName = str(name);
}

HdDecoder::HdDecoder() {
    _audioResamp.init(NRSC5_SAMPLE_RATE_AUDIO, _audioRate, AUDIO_TAPS);
}

HdDecoder::~HdDecoder() {
    close();
}

void HdDecoder::open(Mode mode, double inputRate) {
    std::lock_guard<std::mutex> lck(_dspMtx);
    if (_radio) {
        nrsc5_close(_radio);
        _radio = nullptr;
    }
    _mode = mode;
    _inputRate = inputRate;
    // The IQ filter's cutoff sits just below nrsc5's Nyquist rate; the VFO in
    // front of it has already limited the signal to the HD sidebands.
    _iqResamp.init(inputRate, nativeRate(mode), IQ_TAPS);
    _audioResamp.reset();
    _audioProgram = -1;

    if (nrsc5_open_pipe(&_radio) != 0) {
        _radio = nullptr;
        return;
    }
    nrsc5_set_mode(_radio, mode == MODE_FM ? NRSC5_MODE_FM : NRSC5_MODE_AM);
    nrsc5_set_callback(_radio, callback, this);
    _resetRequested = false;
    resetState();
}

void HdDecoder::close() {
    std::lock_guard<std::mutex> lck(_dspMtx);
    if (_radio) {
        nrsc5_close(_radio);
        _radio = nullptr;
    }
    resetState();
}

bool HdDecoder::isOpen() {
    std::lock_guard<std::mutex> lck(_dspMtx);
    return _radio != nullptr;
}

void HdDecoder::setInputRate(double inputRate) {
    std::lock_guard<std::mutex> lck(_dspMtx);
    if (inputRate == _inputRate) { return; }
    _inputRate = inputRate;
    _iqResamp.init(inputRate, nativeRate(_mode), IQ_TAPS);
}

void HdDecoder::setAudioHandler(AudioHandler handler, void* ctx) {
    std::lock_guard<std::mutex> lck(_dspMtx);
    _audioHandler = handler;
    _audioCtx = ctx;
}

void HdDecoder::setAudioRate(double rate) {
    std::lock_guard<std::mutex> lck(_dspMtx);
    if (rate == _audioRate) { return; }
    _audioRate = rate;
    _audioResamp.init(NRSC5_SAMPLE_RATE_AUDIO, _audioRate, AUDIO_TAPS);
}

void HdDecoder::setProgram(int program) {
    if (program < 0 || program >= MAX_PROGRAMS) { return; }
    _program = program;
}

void HdDecoder::process(const float* iq, int count) {
    {
        std::lock_guard<std::mutex> lck(_dspMtx);
        if (!_radio) { return; }

        if (_resetRequested.exchange(false)) {
            // nrsc5_reset() only refuses a running RTL-SDR worker; a pipe
            // never has one.
            nrsc5_reset(_radio);
            _iqResamp.reset();
            _audioResamp.reset();
            resetState();
        }

        _iqBuf.clear();
        _iqResamp.process(iq, count, _iqBuf);
        int n = (int)(_iqBuf.size() / 2);
        for (int off = 0; off < n; off += PIPE_CHUNK) {
            int c = std::min(PIPE_CHUNK, n - off);
            nrsc5_pipe_samples_cf32(_radio, &_iqBuf[2 * off], 2 * c);
        }
    }

    // Outside the lock: the handler writes to a pipe, which can block until
    // the reader catches up, and nothing else should wait on that.
    if (!_audioOut.empty()) {
        if (_audioHandler) { _audioHandler(_audioOut.data(), (int)(_audioOut.size() / 2), _audioCtx); }
        _audioOut.clear();
    }
}

HdDecoder::Status HdDecoder::status() {
    std::lock_guard<std::mutex> lck(_stateMtx);
    return _state;
}

void HdDecoder::resetState() {
    std::lock_guard<std::mutex> lck(_stateMtx);
    _state = Status();
}

void HdDecoder::callback(const nrsc5_event_t* evt, void* opaque) {
    ((HdDecoder*)opaque)->handleEvent(evt);
}

// Runs inside nrsc5_pipe_samples_cf32(), so _dspMtx is held.
void HdDecoder::handleEvent(const nrsc5_event_t* evt) {
    if (evt->event == NRSC5_EVENT_AUDIO) {
        unsigned int p = evt->audio.program;
        if (p < MAX_PROGRAMS) {
            std::lock_guard<std::mutex> lck(_stateMtx);
            Program& prog = _state.programs[p];
            prog.audio = !(evt->audio.flags & NRSC5_AUDIO_FLAGS_UNAVAILABLE);
            if (prog.audio) {
                prog.present = true;
                prog.audioFrames++;
            }
            if (evt->audio.flags & NRSC5_AUDIO_FLAGS_DECODING_ERROR) { prog.audioErrors++; }
        }
        if ((int)p != _program) { return; }
        // nrsc5 keeps the audio clock running with silent frames while the
        // program cannot be decoded. The output is meant to carry decoded
        // audio only, so those stop here.
        if (evt->audio.flags & NRSC5_AUDIO_FLAGS_UNAVAILABLE) { return; }
        if (_audioProgram != (int)p) {
            // A different program's audio: don't let the filter smear the
            // last one into it.
            _audioResamp.reset();
            _audioProgram = p;
        }
        // count is in int16 values, two per stereo frame.
        int frames = (int)(evt->audio.count / 2);
        _audioIn.resize(evt->audio.count);
        for (size_t i = 0; i < evt->audio.count; i++) { _audioIn[i] = evt->audio.data[i] / 32768.0f; }
        _audioResamp.process(_audioIn.data(), frames, _audioOut);
        return;
    }

    std::lock_guard<std::mutex> lck(_stateMtx);
    Status& s = _state;
    switch (evt->event) {
    case NRSC5_EVENT_SYNC:
        s.synced = true;
        s.freqOffset = evt->sync.freq_offset;
        s.psmi = evt->sync.psmi;
        break;
    case NRSC5_EVENT_LOST_SYNC:
        s.synced = false;
        // nrsc5 reports audio only while synced, so no frame is coming to say
        // it has stopped.
        for (Program& prog : s.programs) { prog.audio = false; }
        break;
    case NRSC5_EVENT_MER:
        s.haveMer = true;
        s.merLower = evt->mer.lower;
        s.merUpper = evt->mer.upper;
        break;
    case NRSC5_EVENT_BER:
        s.haveBer = true;
        s.ber = evt->ber.cber;
        break;
    case NRSC5_EVENT_ID3: {
        unsigned int p = evt->id3.program;
        if (p >= MAX_PROGRAMS) { break; }
        Program& prog = s.programs[p];
        // Stations resend ID3 often, sometimes with fields left out; keep
        // what was there rather than blanking it.
        if (evt->id3.title) { prog.title = evt->id3.title; }
        if (evt->id3.artist) { prog.artist = evt->id3.artist; }
        if (evt->id3.album) { prog.album = evt->id3.album; }
        if (evt->id3.genre) { prog.genre = evt->id3.genre; }
        break;
    }
    case NRSC5_EVENT_AUDIO_SERVICE: {
        unsigned int p = evt->audio_service.program;
        if (p >= MAX_PROGRAMS) { break; }
        Program& prog = s.programs[p];
        prog.present = true;
        setType(prog, evt->audio_service.type);
        break;
    }
    case NRSC5_EVENT_SIS:
        if (evt->sis.country_code) { s.country = evt->sis.country_code; }
        if (evt->sis.fcc_facility_id > 0) { s.facilityId = evt->sis.fcc_facility_id; }
        if (evt->sis.name) { s.name = evt->sis.name; }
        if (evt->sis.slogan) { s.slogan = evt->sis.slogan; }
        if (evt->sis.message) { s.message = evt->sis.message; }
        if (evt->sis.alert) { s.alert = evt->sis.alert; }
        // NaN until the station's location has been received; some stations
        // send 0, 0 rather than a real one.
        if (!std::isnan(evt->sis.latitude) && !std::isnan(evt->sis.longitude) && (evt->sis.latitude != 0 || evt->sis.longitude != 0)) {
            s.haveLocation = true;
            s.latitude = evt->sis.latitude;
            s.longitude = evt->sis.longitude;
            s.altitude = evt->sis.altitude;
        }
        for (nrsc5_sis_asd_t* a = evt->sis.audio_services; a; a = a->next) {
            if (a->program >= MAX_PROGRAMS) { continue; }
            Program& prog = s.programs[a->program];
            prog.present = true;
            setType(prog, a->type);
        }
        break;
    case NRSC5_EVENT_STATION_ID:
        if (evt->station_id.country_code) { s.country = evt->station_id.country_code; }
        s.facilityId = evt->station_id.fcc_facility_id;
        break;
    case NRSC5_EVENT_STATION_NAME:
        s.name = str(evt->station_name.name);
        break;
    case NRSC5_EVENT_STATION_SLOGAN:
        s.slogan = str(evt->station_slogan.slogan);
        break;
    case NRSC5_EVENT_STATION_MESSAGE:
        s.message = str(evt->station_message.message);
        break;
    case NRSC5_EVENT_STATION_LOCATION:
        if (evt->station_location.latitude == 0 && evt->station_location.longitude == 0) { break; }
        s.haveLocation = true;
        s.latitude = evt->station_location.latitude;
        s.longitude = evt->station_location.longitude;
        s.altitude = evt->station_location.altitude;
        break;
    case NRSC5_EVENT_EMERGENCY_ALERT:
        s.alert = str(evt->emergency_alert.message);
        break;
    default:
        break;
    }
}
