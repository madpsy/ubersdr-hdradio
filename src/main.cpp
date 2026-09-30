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
 * (fd 4 by default), because stdin is taken by the IQ. Images the station
 * sends -- album art, station logos, HERE traffic and weather maps -- go out
 * as length-prefixed frames on a third (fd 5 by default).
 *
 * Usage:
 *   ubersdr-hdradio [--input-sample-rate <N>] [--output-sample-rate <N>]
 *                   [--program <N>] [--status-fd <n> | --no-status]
 *                   [--control-fd <n> | --no-control]
 *                   [--image-fd <n> | --no-images]
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
 * --image-fd <n>            write images to this descriptor (default 5)
 * --no-images               write no images even if the descriptor is open
 *
 * Image frames, one per image, nothing between them:
 *   [header length: u32 LE][header: JSON][data length: u32 LE][data]
 * The header is {"t":"image","kind":"art"|"logo"|"traffic"|"weather",
 * "program":N|null,"lot":N|null,"mime":"image/jpeg"|"image/png","name":"...",
 * "bounds":{"north","west","south","east"}|null}; see README.md.
 *
 * Commands, one per line on the control descriptor:
 *   program <N>   switch to program N (0-7). Takes effect with the next audio
 *                 frame; nothing is reset, so it is immediate once synced.
 *   reset         forget the station and acquire afresh, as after a retune.
 *
 * Exits 0 when stdin reaches EOF or stdout's reader goes away.
 */
#include "hd_decoder.h"
#include "wire.h"

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
            "          [--status-fd n | --no-status] [--control-fd n | --no-control]\n"
            "          [--image-fd n | --no-images]\n",
            argv0);
}

// ─── output ──────────────────────────────────────────────────────────────────

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
    int imageFd = 5;

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
        } else if (strcmp(a, "--image-fd") == 0 && hasArg) {
            ok = parseInt(argv[++i], imageFd) && imageFd >= 0;
        } else if (strcmp(a, "--no-images") == 0) {
            imageFd = -1;
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
    bool haveImages = imageFd >= 0 && fcntl(imageFd, F_GETFD) != -1;

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
        for (const HdDecoder::Image& img : decoder.takeImages()) {
            if (!haveImages) { break; }
            fprintf(stderr, "%s %s image%s%s, %zu bytes\n", TAG, img.kind.c_str(),
                    img.program >= 0 ? " for HD" : "", img.program >= 0 ? std::to_string(img.program + 1).c_str() : "",
                    img.data.size());
            if (!writeImage(imageFd, img)) {
                // The reader has gone; the audio carries on without images.
                fprintf(stderr, "%s image channel closed\n", TAG);
                haveImages = false;
            }
        }
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
