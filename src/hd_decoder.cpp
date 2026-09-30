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

// Files larger than this are not passed on: album art and logos are a few
// tens of kilobytes, and nothing that needs more belongs in a status panel.
static const size_t MAX_IMAGE_BYTES = 512 * 1024;
// A bound on the images waiting for takeImages(), should nothing collect them.
static const size_t MAX_PENDING_IMAGES = 16;

// What the bytes are, from their signature rather than from what the station
// says they are: only a real JPEG or PNG is passed on, and it is labelled as
// what it is.
static std::string imageMime(const uint8_t* d, size_t n) {
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) { return "image/jpeg"; }
    static const uint8_t png[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (n >= 8 && memcmp(d, png, 8) == 0) { return "image/png"; }
    return "";
}

static std::string alertCategoryName(int category) {
    if (category <= 0) { return ""; }
    const char* name = nullptr;
    nrsc5_alert_category_name(category, &name);
    return str(name);
}

static std::string locationFormatName(int format) {
    switch (format) {
    case NRSC5_LOCATION_FORMAT_SAME: return "SAME";
    case NRSC5_LOCATION_FORMAT_FIPS: return "FIPS";
    case NRSC5_LOCATION_FORMAT_ZIP: return "ZIP";
    default: return "";
    }
}

static void setAlert(HdDecoder::Status& s, int category1, int category2, int locationFormat, int numLocations, const int* locations) {
    s.alertCategory1 = category1 > 0 ? category1 : -1;
    s.alertCategory2 = category2 > 0 ? category2 : -1;
    s.alertCategory1Name = alertCategoryName(category1);
    s.alertCategory2Name = alertCategoryName(category2);
    s.alertLocationFormat = locationFormatName(locationFormat);
    s.alertLocations.clear();
    for (int i = 0; locations && i < numLocations && i < 64; i++) { s.alertLocations.push_back(locations[i]); }
}

static void setDevice(HdDecoder::Device& d, const char* manufacturer, const int* core, int coreStatus, const int* mfr, int mfrStatus) {
    d.have = true;
    d.manufacturer = str(manufacturer);
    for (int i = 0; i < 4; i++) {
        d.coreVersion[i] = core ? core[i] : 0;
        d.manufacturerVersion[i] = mfr ? mfr[i] : 0;
    }
    d.coreStatus = coreStatus;
    d.manufacturerStatus = mfrStatus;
}

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

std::vector<HdDecoder::Image> HdDecoder::takeImages() {
    std::lock_guard<std::mutex> lck(_stateMtx);
    std::vector<Image> out;
    out.swap(_images);
    return out;
}

void HdDecoder::resetState() {
    std::lock_guard<std::mutex> lck(_stateMtx);
    _state = Status();
    _images.clear();
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
        // Metadata for a program is as good as an announcement of it.
        prog.present = true;
        // Stations resend ID3 often, sometimes with fields left out; keep
        // what was there rather than blanking it.
        if (evt->id3.title) { prog.title = evt->id3.title; }
        if (evt->id3.artist) { prog.artist = evt->id3.artist; }
        if (evt->id3.album) { prog.album = evt->id3.album; }
        if (evt->id3.genre) { prog.genre = evt->id3.genre; }
        if (evt->id3.comments) {
            prog.comments.clear();
            for (nrsc5_id3_comment_t* c = evt->id3.comments; c && prog.comments.size() < 4; c = c->next) {
                prog.comments.push_back({ str(c->lang), str(c->short_content_desc), str(c->full_text) });
            }
        }
        if (evt->id3.commercial.price || evt->id3.commercial.seller || evt->id3.commercial.description || evt->id3.commercial.contact_url) {
            Commercial& cm = prog.commercial;
            cm.have = true;
            cm.price = str(evt->id3.commercial.price);
            cm.seller = str(evt->id3.commercial.seller);
            cm.contactUrl = str(evt->id3.commercial.contact_url);
            cm.description = str(evt->id3.commercial.description);
            cm.receivedAs = evt->id3.commercial.received_as;
            cm.validUntil.clear();
            if (const struct tm* t = evt->id3.commercial.valid_until) {
                char buf[48];
                snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
                cm.validUntil = buf;
            }
        }
        // XHDR: param 0 names the LOT file holding this song's art, param 1
        // tells the receiver to drop whatever art it is showing.
        if (evt->id3.xhdr.param == 0 && evt->id3.xhdr.lot >= 0) {
            prog.artLot = evt->id3.xhdr.lot;
        } else if (evt->id3.xhdr.param == 1) {
            prog.artLot = -1;
        }
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
        if (evt->sis.alert) {
            setAlert(s, evt->sis.alert_category1, evt->sis.alert_category2, evt->sis.alert_location_format,
                     evt->sis.alert_num_locations, evt->sis.alert_locations);
        }
        for (nrsc5_sis_asd_t* a = evt->sis.audio_services; a; a = a->next) {
            if (a->program >= MAX_PROGRAMS) { continue; }
            Program& prog = s.programs[a->program];
            prog.present = true;
            setType(prog, a->type);
            prog.access = (int)a->access;
            prog.soundExp = (int)a->sound_exp;
        }
        if (evt->sis.data_services) {
            s.dataServices.clear();
            for (nrsc5_sis_dsd_t* d = evt->sis.data_services; d && s.dataServices.size() < 32; d = d->next) {
                DataService ds;
                ds.access = (int)d->access;
                ds.type = (int)d->type;
                const char* name = nullptr;
                nrsc5_service_data_type_name(d->type, &name);
                ds.typeName = str(name);
                ds.mime = d->mime_type;
                s.dataServices.push_back(ds);
            }
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
        setAlert(s, evt->emergency_alert.category1, evt->emergency_alert.category2, evt->emergency_alert.location_format,
                 evt->emergency_alert.num_locations, evt->emergency_alert.locations);
        break;
    case NRSC5_EVENT_AUDIO_SERVICE_DESCRIPTOR: {
        unsigned int p = evt->asd.program;
        if (p >= MAX_PROGRAMS) { break; }
        Program& prog = s.programs[p];
        prog.present = true;
        setType(prog, evt->asd.type);
        prog.access = (int)evt->asd.access;
        prog.soundExp = (int)evt->asd.sound_exp;
        break;
    }
    case NRSC5_EVENT_DATA_SERVICE_DESCRIPTOR: {
        DataService ds;
        ds.access = (int)evt->dsd.access;
        ds.type = (int)evt->dsd.type;
        const char* name = nullptr;
        nrsc5_service_data_type_name(evt->dsd.type, &name);
        ds.typeName = str(name);
        ds.mime = evt->dsd.mime_type;
        bool known = false;
        for (DataService& d : s.dataServices) {
            if (d.type == ds.type && d.mime == ds.mime) {
                d = ds;
                known = true;
            }
        }
        if (!known && s.dataServices.size() < 32) { s.dataServices.push_back(ds); }
        break;
    }
    case NRSC5_EVENT_SIG:
        for (nrsc5_sig_service_t* sv = evt->sig.services; sv; sv = sv->next) {
            if (sv->type != NRSC5_SIG_SERVICE_AUDIO || sv->number < 1 || sv->number > MAX_PROGRAMS) { continue; }
            s.programs[sv->number - 1].serviceName = str(sv->name);
        }
        break;
    case NRSC5_EVENT_LOT: {
        // Album art and station logos, which arrive as files on a data
        // component of the program's own audio service.
        const nrsc5_sig_component_t* c = evt->lot.component;
        if (!c || c->type != NRSC5_SIG_SERVICE_DATA || !evt->lot.data) { break; }
        const char* kind = nullptr;
        if (c->data.mime == NRSC5_MIME_PRIMARY_IMAGE) {
            kind = "art";
        } else if (c->data.mime == NRSC5_MIME_STATION_LOGO) {
            kind = "logo";
        }
        if (!kind || evt->lot.size == 0 || evt->lot.size > MAX_IMAGE_BYTES) { break; }
        std::string mime = imageMime(evt->lot.data, evt->lot.size);
        if (mime.empty() || _images.size() >= MAX_PENDING_IMAGES) { break; }
        Image img;
        img.kind = kind;
        img.lot = (int)evt->lot.lot;
        const nrsc5_sig_service_t* sv = evt->lot.service;
        if (sv && sv->type == NRSC5_SIG_SERVICE_AUDIO && sv->number >= 1 && sv->number <= MAX_PROGRAMS) {
            img.program = sv->number - 1;
        }
        img.mime = mime;
        img.name = str(evt->lot.name);
        img.data.assign(evt->lot.data, evt->lot.data + evt->lot.size);
        _images.push_back(std::move(img));
        break;
    }
    case NRSC5_EVENT_HERE_IMAGE: {
        // HERE traffic and weather maps, georeferenced by their edges.
        if (!evt->here_image.data || evt->here_image.size == 0 || evt->here_image.size > MAX_IMAGE_BYTES) { break; }
        std::string mime = imageMime(evt->here_image.data, evt->here_image.size);
        if (mime.empty() || _images.size() >= MAX_PENDING_IMAGES) { break; }
        Image img;
        img.kind = evt->here_image.image_type == NRSC5_HERE_IMAGE_TRAFFIC ? "traffic" : "weather";
        img.mime = mime;
        img.name = str(evt->here_image.name);
        img.data.assign(evt->here_image.data, evt->here_image.data + evt->here_image.size);
        img.north = evt->here_image.latitude1;
        img.west = evt->here_image.longitude1;
        img.south = evt->here_image.latitude2;
        img.east = evt->here_image.longitude2;
        _images.push_back(std::move(img));
        break;
    }
    case NRSC5_EVENT_EXCITER_INFO:
        setDevice(s.exciter, evt->exciter_info.manufacturer_id, evt->exciter_info.core_version, evt->exciter_info.core_status,
                  evt->exciter_info.manufacturer_version, evt->exciter_info.manufacturer_status);
        s.importerConnected = evt->exciter_info.importer_connected;
        break;
    case NRSC5_EVENT_IMPORTER_INFO:
        setDevice(s.importer, evt->importer_info.manufacturer_id, evt->importer_info.core_version, evt->importer_info.core_status,
                  evt->importer_info.manufacturer_version, evt->importer_info.manufacturer_status);
        break;
    case NRSC5_EVENT_LEAP_SECOND_OFFSET:
        s.haveLeapSecond = true;
        s.leapCurrent = evt->leap_second_offset.current_offset;
        s.leapPending = evt->leap_second_offset.pending_offset;
        s.leapPendingAlfn = evt->leap_second_offset.pending_alfn;
        break;
    case NRSC5_EVENT_LOCAL_TIME:
        s.haveLocalTime = true;
        s.utcOffset = evt->local_time.utc_offset;
        s.dstRegional = evt->local_time.dst_regional;
        s.dstLocal = evt->local_time.dst_local;
        s.dstSchedule = evt->local_time.dst_schedule;
        break;
    default:
        break;
    }
}
