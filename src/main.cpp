/*
 * ubersdr-hdradio — HD Radio (NRSC-5) AM decoder for UberSDR.
 *
 * Reads stereo int16 little-endian zero-IF IQ on stdin (I at even indices, Q
 * at odd), centred on an AM station's carrier, and writes the selected
 * program's decoded audio to stdout as stereo int16 little-endian PCM. Audio is
 * written only while the program is actually decoding; there is nothing on
 * stdout while acquiring or after losing sync.
 *
 * Metadata goes out as JSON Lines on a separate descriptor (fd 3 by default),
 * so stdout stays raw headerless PCM. Commands come in as lines on another
 * (fd 4 by default), because stdin is taken by the IQ.
 *
 * Usage:
 *   ubersdr-hdradio [--input-sample-rate <N>] [--output-sample-rate <N>]
 *                   [--program <N>] [--status-fd <n> | --no-status]
 *                   [--control-fd <n> | --no-control]
 *
 * --input-sample-rate <N>   IQ sample rate in Hz (default 48000). UberSDR's
 *                           iq48 covers a hybrid station's sidebands, which
 *                           reach +/-15 kHz; iq (12 kHz) only an all-digital
 *                           (MA3) one.
 * --output-sample-rate <N>  audio rate in Hz (default 48000). nrsc5 decodes
 *                           at 44100; this resamples to whatever is asked for.
 * --program <N>             program to play, 0 = HD1 .. 7 = HD8 (default 0)
 * --status-fd <n>           write JSON Lines status to this descriptor (default 3)
 * --no-status               write no status even if the descriptor is open
 * --control-fd <n>          read commands from this descriptor (default 4)
 * --no-control              read no commands even if the descriptor is open
 *
 * Commands, one per line on the control descriptor:
 *   program <N>   switch to program N (0-7). Takes effect with the next audio
 *                 frame; nothing is reset, so it is immediate once synced.
 *   reset         forget the station and acquire afresh, as after a retune.
 *
 * Exits 0 when stdin reaches EOF or stdout's reader goes away.
 */
#include "hd_decoder.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

static const char* TAG = "[ubersdr-hdradio]";

// Status goes out when it changes, but no more often than this, and at least
// this often whether or not it has: a reader that attaches late is not left
// waiting for the next change. Both are counted in input time, so a file piped
// in faster than real time gets the same sequence a live stream would.
static const double STATUS_MIN_INTERVAL = 0.25;
static const double STATUS_MAX_INTERVAL = 1.0;

// Input is read in blocks of this long.
static const double READ_BLOCK_SECONDS = 0.1;

static bool g_stdoutGone = false;

static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [--input-sample-rate N] [--output-sample-rate N] [--program N]\n"
            "          [--status-fd n | --no-status] [--control-fd n | --no-control]\n",
            argv0);
}

// ─── output ──────────────────────────────────────────────────────────────────

static bool writeAll(int fd, const void* buf, size_t len) {
    const char* p = (const char*)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) { continue; }
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static void onAudio(const float* stereo, int frames, void* ctx) {
    (void)ctx;
    if (g_stdoutGone) { return; }
    std::vector<int16_t> pcm(2 * (size_t)frames);
    for (size_t i = 0; i < pcm.size(); i++) {
        float v = stereo[i] * 32768.0f;
        if (v > 32767.0f) { v = 32767.0f; }
        if (v < -32768.0f) { v = -32768.0f; }
        pcm[i] = (int16_t)lrintf(v);
    }
    // Native order, which on every target this is built for is little-endian:
    // the order the wrapper reads.
    if (!writeAll(STDOUT_FILENO, pcm.data(), pcm.size() * sizeof(int16_t))) {
        g_stdoutGone = true;
    }
}

// ─── status ──────────────────────────────────────────────────────────────────

// Every string in the status comes from the station, so each is cut to this
// many bytes, and control characters become spaces rather than six-byte \u
// escapes: nothing then grows more than two-fold when escaped. With 45 strings
// at most (the station's 5, and 5 for each of 8 programs) a status line stays
// under 100 KiB however hostile the station; README.md promises readers 256 KiB.
static const size_t MAX_STRING_BYTES = 1024;

static void jsonString(std::string& out, const std::string& in) {
    std::string s = in;
    if (s.size() > MAX_STRING_BYTES) {
        // Back up to the start of a UTF-8 sequence, so the cut never leaves
        // half a character.
        size_t n = MAX_STRING_BYTES;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) { n--; }
        s.resize(n);
    }
    out += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
        } else if (c < 0x20 || c == 0x7f) {
            out += ' ';
        } else {
            out += (char)c;
        }
    }
    out += '"';
}

static void jsonNumber(std::string& out, double v, const char* fmt = "%.2f") {
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), fmt, v);
    out += buf;
}

// One status line, without the trailing newline.
static std::string statusJson(const HdDecoder::Status& s, int program) {
    std::string j = "{\"t\":\"status\",\"sync\":";
    j += s.synced ? "true" : "false";
    j += ",\"freqOffset\":";
    jsonNumber(j, s.freqOffset, "%.1f");
    j += ",\"psmi\":" + std::to_string(s.psmi);
    j += ",\"merLower\":";
    if (s.haveMer) { jsonNumber(j, s.merLower); } else { j += "null"; }
    j += ",\"merUpper\":";
    if (s.haveMer) { jsonNumber(j, s.merUpper); } else { j += "null"; }
    j += ",\"ber\":";
    if (s.haveBer) { jsonNumber(j, s.ber, "%.6f"); } else { j += "null"; }
    j += ",\"country\":";
    jsonString(j, s.country);
    j += ",\"facilityId\":";
    j += s.facilityId >= 0 ? std::to_string(s.facilityId) : "null";
    j += ",\"name\":";
    jsonString(j, s.name);
    j += ",\"slogan\":";
    jsonString(j, s.slogan);
    j += ",\"message\":";
    jsonString(j, s.message);
    j += ",\"alert\":";
    jsonString(j, s.alert);
    j += ",\"location\":";
    if (s.haveLocation) {
        j += "{\"lat\":";
        jsonNumber(j, s.latitude, "%.5f");
        j += ",\"lon\":";
        jsonNumber(j, s.longitude, "%.5f");
        j += ",\"alt\":" + std::to_string(s.altitude) + "}";
    } else {
        j += "null";
    }
    j += ",\"program\":" + std::to_string(program);
    j += ",\"audio\":";
    j += (program >= 0 && program < HdDecoder::MAX_PROGRAMS && s.programs[program].audio) ? "true" : "false";
    j += ",\"programs\":[";
    bool first = true;
    for (int p = 0; p < HdDecoder::MAX_PROGRAMS; p++) {
        const HdDecoder::Program& pr = s.programs[p];
        if (!pr.present) { continue; }
        if (!first) { j += ','; }
        first = false;
        j += "{\"program\":" + std::to_string(p);
        j += ",\"type\":" + std::to_string(pr.type);
        j += ",\"typeName\":";
        jsonString(j, pr.typeName);
        j += ",\"title\":";
        jsonString(j, pr.title);
        j += ",\"artist\":";
        jsonString(j, pr.artist);
        j += ",\"album\":";
        jsonString(j, pr.album);
        j += ",\"genre\":";
        jsonString(j, pr.genre);
        j += ",\"audio\":";
        j += pr.audio ? "true" : "false";
        j += ",\"frames\":" + std::to_string(pr.audioFrames);
        j += ",\"errors\":" + std::to_string(pr.audioErrors);
        j += '}';
    }
    j += "]}";
    return j;
}

// The parts of the status worth a line on stderr when they change: the log is
// the only view of the decoder a server has, and it must not fill with
// once-a-second telemetry.
struct LogState {
    bool synced = false;
    std::string name;
    bool audio = false;
    int program = -1;
};

static void logChanges(LogState& prev, const HdDecoder::Status& s, int program) {
    bool audio = program >= 0 && program < HdDecoder::MAX_PROGRAMS && s.programs[program].audio;
    if (s.synced != prev.synced) {
        if (s.synced) {
            fprintf(stderr, "%s sync (offset %.1f Hz, psmi %d)\n", TAG, s.freqOffset, s.psmi);
        } else {
            fprintf(stderr, "%s lost sync\n", TAG);
        }
    }
    if (s.name != prev.name && !s.name.empty()) { fprintf(stderr, "%s station %s\n", TAG, s.name.c_str()); }
    if (program != prev.program) { fprintf(stderr, "%s program HD%d\n", TAG, program + 1); }
    if (audio != prev.audio) { fprintf(stderr, "%s audio %s\n", TAG, audio ? "decoding" : "stopped"); }
    prev.synced = s.synced;
    prev.name = s.name;
    prev.audio = audio;
    prev.program = program;
}

// ─── control ─────────────────────────────────────────────────────────────────

// Reads commands until the descriptor closes. Runs on its own thread: the
// decoder's program and reset are atomics, safe to set from here while the
// main thread is inside nrsc5.
static void controlLoop(int fd, HdDecoder* decoder) {
    FILE* f = fdopen(fd, "r");
    if (!f) { return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char cmd[32] = { 0 };
        int arg = 0;
        int n = sscanf(line, "%31s %d", cmd, &arg);
        if (n < 1) { continue; }
        if (strcmp(cmd, "program") == 0 && n == 2 && arg >= 0 && arg < HdDecoder::MAX_PROGRAMS) {
            decoder->setProgram(arg);
        } else if (strcmp(cmd, "reset") == 0) {
            fprintf(stderr, "%s reset\n", TAG);
            decoder->requestReset();
        } else {
            // Trim the newline for the log.
            line[strcspn(line, "\r\n")] = 0;
            fprintf(stderr, "%s unknown command: %s\n", TAG, line);
        }
    }
    fclose(f);
}

// ─── main ────────────────────────────────────────────────────────────────────

static bool parseInt(const char* s, int& out) {
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if (!s[0] || *end) { return false; }
    out = (int)v;
    return true;
}

int main(int argc, char** argv) {
    int inputRate = 48000;
    int outputRate = 48000;
    int program = 0;
    int statusFd = 3;
    int controlFd = 4;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        bool hasArg = i + 1 < argc;
        bool ok = true;
        if (strcmp(a, "--input-sample-rate") == 0 && hasArg) {
            ok = parseInt(argv[++i], inputRate) && inputRate > 0;
        } else if (strcmp(a, "--output-sample-rate") == 0 && hasArg) {
            ok = parseInt(argv[++i], outputRate) && outputRate > 0;
        } else if (strcmp(a, "--program") == 0 && hasArg) {
            ok = parseInt(argv[++i], program) && program >= 0 && program < HdDecoder::MAX_PROGRAMS;
        } else if (strcmp(a, "--status-fd") == 0 && hasArg) {
            ok = parseInt(argv[++i], statusFd) && statusFd >= 0;
        } else if (strcmp(a, "--no-status") == 0) {
            statusFd = -1;
        } else if (strcmp(a, "--control-fd") == 0 && hasArg) {
            ok = parseInt(argv[++i], controlFd) && controlFd >= 0;
        } else if (strcmp(a, "--no-control") == 0) {
            controlFd = -1;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            ok = false;
        }
        if (!ok) {
            usage(argv[0]);
            return 2;
        }
    }

    // A write to a reader that has gone away must come back as an error, not
    // kill the process before it can say so.
    signal(SIGPIPE, SIG_IGN);

    // Both side channels are optional and silent when absent. Run from a shell
    // there is no fd 3 or fd 4, and that has to be a no-op rather than an
    // error: stdout alone is a complete, useful output.
    FILE* statusFile = nullptr;
    if (statusFd >= 0 && fcntl(statusFd, F_GETFD) != -1) {
        statusFile = fdopen(statusFd, "w");
        if (!statusFile) {
            fprintf(stderr, "%s could not open status fd %d: %s\n", TAG, statusFd, strerror(errno));
        }
    }
    bool haveControl = controlFd >= 0 && fcntl(controlFd, F_GETFD) != -1;

    HdDecoder decoder;
    decoder.setAudioRate(outputRate);
    decoder.setAudioHandler(onAudio, nullptr);
    decoder.setProgram(program);
    decoder.open(HdDecoder::MODE_AM, inputRate);
    if (!decoder.isOpen()) {
        fprintf(stderr, "%s nrsc5 failed to open\n", TAG);
        return 1;
    }

    fprintf(stderr, "%s AM, input %d Hz, output %d Hz stereo, program HD%d%s%s\n", TAG, inputRate, outputRate,
            program + 1, statusFile ? ", status on fd " : "", statusFile ? std::to_string(statusFd).c_str() : "");

    if (haveControl) {
        // Detached: it blocks in fgets until the writer closes, and nothing
        // needs to wait for it on the way out.
        std::thread(controlLoop, controlFd, &decoder).detach();
    }

    const size_t blockFrames = (size_t)std::max(1.0, inputRate * READ_BLOCK_SECONDS);
    std::vector<uint8_t> raw(blockFrames * 4);
    size_t rawFill = 0;
    std::vector<float> iq(blockFrames * 2);

    uint64_t framesIn = 0;
    double lastStatusAt = -1e9;
    std::string lastStatus;
    LogState logState;

    auto emitStatus = [&](bool force) {
        double now = (double)framesIn / inputRate;
        HdDecoder::Status s = decoder.status();
        int prog = decoder.getProgram();
        logChanges(logState, s, prog);
        if (!statusFile) { return; }
        std::string j = statusJson(s, prog);
        double since = now - lastStatusAt;
        bool changed = j != lastStatus;
        if (!force && !(changed && since >= STATUS_MIN_INTERVAL) && since < STATUS_MAX_INTERVAL) { return; }
        lastStatus = j;
        lastStatusAt = now;
        j += '\n';
        if (fwrite(j.data(), 1, j.size(), statusFile) != j.size() || fflush(statusFile) != 0) {
            // The reader has gone. The audio matters more than the metadata,
            // so carry on without it.
            fprintf(stderr, "%s status channel closed\n", TAG);
            fclose(statusFile);
            statusFile = nullptr;
        }
    };

    for (;;) {
        ssize_t n = read(STDIN_FILENO, raw.data() + rawFill, raw.size() - rawFill);
        if (n < 0) {
            if (errno == EINTR) { continue; }
            fprintf(stderr, "%s stdin: %s\n", TAG, strerror(errno));
            break;
        }
        if (n == 0) { break; }
        rawFill += (size_t)n;

        // Whole IQ pairs only; a partial one waits for the rest.
        size_t frames = rawFill / 4;
        if (frames == 0) { continue; }
        const int16_t* s16 = (const int16_t*)raw.data();
        for (size_t i = 0; i < 2 * frames; i++) { iq[i] = s16[i] / 32768.0f; }
        size_t used = frames * 4;
        memmove(raw.data(), raw.data() + used, rawFill - used);
        rawFill -= used;

        decoder.process(iq.data(), (int)frames);
        framesIn += frames;
        if (g_stdoutGone) {
            fprintf(stderr, "%s stdout closed\n", TAG);
            break;
        }
        emitStatus(false);
    }

    // One last line, so a reader of a finite input sees where it ended.
    emitStatus(true);
    if (statusFile) { fclose(statusFile); }
    decoder.close();
    return 0;
}
