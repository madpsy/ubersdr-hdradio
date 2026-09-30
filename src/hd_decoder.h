// HdDecoder: nrsc5's library behind a small interface.
//
// IQ goes in at whatever rate the receiver delivers; it is resampled to the
// rate nrsc5 wants and piped in. nrsc5 decodes on the calling thread and
// reports through a callback, from which the selected program's audio is
// resampled to the output rate and everything else is kept for status().
//
// Audio frames nrsc5 marks unavailable are not passed on, so the output
// carries decoded audio and nothing else, and each program records whether its
// audio is live.
#pragma once
#include "resampler.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <nrsc5.h>
}

class HdDecoder {
public:
    enum Mode { MODE_FM = 0, MODE_AM = 1 };
    static constexpr int MAX_PROGRAMS = 8; // nrsc5 numbers them 0..7; stations show them as HD1..HD8

    struct Comment {
        std::string lang, desc, text;
    };

    // An ID3 commercial frame: the song is for sale.
    struct Commercial {
        bool have = false;
        std::string price, seller, contactUrl, description, validUntil; // validUntil YYYY-MM-DD
        int receivedAs = -1;
    };

    struct Program {
        bool present = false;    // announced by the station, or its audio has been heard
        int type = -1;           // NRSC5_PROGRAM_TYPE_*, -1 until known
        std::string typeName;
        int access = -1;         // NRSC5_ACCESS_*, -1 until the SIS says
        int soundExp = -1;       // 0 none, 2 Dolby Pro Logic II; -1 until the SIS says
        std::string serviceName; // from the SIG, e.g. "MPS", "SPS1"
        std::string title, artist, album, genre;
        std::vector<Comment> comments;
        Commercial commercial;
        // The LOT id of the current song's album art, from the ID3 XHDR, or
        // -1: none named, or the station said to drop the last one.
        int artLot = -1;
        uint64_t audioFrames = 0;
        uint64_t audioErrors = 0;
        bool audio = false;      // the last audio frame carried decoded audio
    };

    struct DataService {
        int access = -1;
        int type = -1;           // NRSC5_SERVICE_DATA_TYPE_*
        std::string typeName;
        uint32_t mime = 0;
    };

    // The exciter or importer in the station's transmitter chain.
    struct Device {
        bool have = false;
        std::string manufacturer;
        int coreVersion[4] = { 0, 0, 0, 0 };
        int coreStatus = -1;     // 0 commercial release, 1 engineering, 2 patch
        int manufacturerVersion[4] = { 0, 0, 0, 0 };
        int manufacturerStatus = -1;
    };

    // A file the station sent that can be shown: album art, a station logo,
    // or a HERE traffic or weather map.
    struct Image {
        std::string kind;        // "art", "logo", "traffic" or "weather"
        int program = -1;        // art and logos: the program it belongs to, if known
        int lot = -1;            // art and logos: LOT id (the ID3 XHDR refers to it)
        std::string mime;        // "image/jpeg" or "image/png"
        std::string name;
        std::vector<uint8_t> data;
        // traffic and weather: the map's edges.
        float north = 0, west = 0, south = 0, east = 0;
    };

    struct Status {
        bool synced = false;
        float freqOffset = 0; // Hz
        int psmi = 0;         // primary service mode
        bool haveMer = false;
        float merLower = 0, merUpper = 0;
        bool haveBer = false;
        float ber = 0;
        std::string country;
        int facilityId = -1;
        std::string name, slogan, message, alert;
        // The alert's categories (NRSC5_ALERT_CATEGORY_*, -1 if none) and
        // the areas it covers, as SAME, FIPS or ZIP codes.
        int alertCategory1 = -1, alertCategory2 = -1;
        std::string alertCategory1Name, alertCategory2Name;
        std::string alertLocationFormat;
        std::vector<int> alertLocations;
        bool haveLocation = false;
        float latitude = 0, longitude = 0;
        int altitude = 0;
        std::vector<DataService> dataServices;
        bool haveLocalTime = false;
        int utcOffset = 0;       // minutes
        int dstRegional = 0, dstLocal = 0, dstSchedule = 0;
        bool haveLeapSecond = false;
        int leapCurrent = 0, leapPending = 0;
        unsigned int leapPendingAlfn = 0;
        Device exciter, importer;
        int importerConnected = -1;
        Program programs[MAX_PROGRAMS];
    };

    // Called with interleaved stereo float samples at the audio rate.
    typedef void (*AudioHandler)(const float* stereo, int frames, void* ctx);

    HdDecoder();
    ~HdDecoder();

    // (Re)opens the decoder. Safe to call again to change mode or rate.
    void open(Mode mode, double inputRate);
    void close();
    bool isOpen();

    // A new VFO rate: only the resampler in front of nrsc5 changes.
    void setInputRate(double inputRate);

    void setAudioHandler(AudioHandler handler, void* ctx);
    void setAudioRate(double rate);
    void setProgram(int program);
    int getProgram() { return _program; }

    // Forgets the station and starts acquiring afresh, e.g. after a retune.
    // Takes effect on the next process() call.
    void requestReset() { _resetRequested = true; }

    // IQ as interleaved float pairs, at the rate given to open().
    void process(const float* iq, int count);

    Status status();

    // Images received since the last call, oldest first.
    std::vector<Image> takeImages();

    // For tests: handle an event exactly as if nrsc5 had reported it, which
    // is the only way to reach the paths a short recording never does (a
    // finished album-art transfer, an alert, the transmitter's details).
    void injectEvent(const nrsc5_event_t* evt) {
        std::lock_guard<std::mutex> lck(_dspMtx);
        handleEvent(evt);
    }

    static double nativeRate(Mode mode) {
        return mode == MODE_FM ? NRSC5_SAMPLE_RATE_NATIVE_FM : NRSC5_SAMPLE_RATE_NATIVE_AM;
    }

private:
    static void callback(const nrsc5_event_t* evt, void* opaque);
    void handleEvent(const nrsc5_event_t* evt);
    void resetState();

    std::mutex _dspMtx;       // nrsc5 and both resamplers
    nrsc5_t* _radio = nullptr;
    Mode _mode = MODE_FM;
    double _inputRate = 0;
    double _audioRate = 48000;
    PairResampler _iqResamp;
    PairResampler _audioResamp;
    std::vector<float> _iqBuf;
    std::vector<float> _audioIn;
    std::vector<float> _audioOut; // resampled, waiting to go out once _dspMtx is released

    std::atomic<int> _program{ 0 };
    int _audioProgram = -1; // program the audio resampler last carried
    std::atomic<bool> _resetRequested{ false };

    AudioHandler _audioHandler = nullptr;
    void* _audioCtx = nullptr;

    std::mutex _stateMtx;
    Status _state;
    std::vector<Image> _images; // guarded by _stateMtx
};
