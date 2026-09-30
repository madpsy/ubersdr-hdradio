#include "wire.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <unistd.h>

bool writeAll(int fd, const void* buf, size_t len) {
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

// ─── status ──────────────────────────────────────────────────────────────────

// Every string in the status comes from the station, so each is cut to this
// many bytes, and control characters become spaces rather than six-byte \u
// escapes: nothing then grows more than two-fold when escaped. There are at
// most 226 strings -- the station's 8, 32 data service names, 2 device names,
// and for each of 8 programs 5 ID3 fields, a service name, 4 comments of 3 and
// a commercial frame of 5 -- so a status line stays under 512 KiB however
// hostile the station; README.md promises readers 1 MiB.
static const size_t MAX_STRING_BYTES = 1024;

void jsonString(std::string& out, const std::string& in) {
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

void jsonNumber(std::string& out, double v, const char* fmt) {
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), fmt, v);
    out += buf;
}

static const char* accessName(int access) {
    switch (access) {
    case NRSC5_ACCESS_PUBLIC: return "public";
    case NRSC5_ACCESS_RESTRICTED: return "restricted";
    default: return "";
    }
}

static const char* dstScheduleName(int schedule) {
    switch (schedule) {
    case 0: return "none";
    case 1: return "US/Canada";
    case 2: return "EU";
    default: return "";
    }
}

// What a data service carries, where nrsc5 has a name for its MIME type.
static std::string mimeName(uint32_t mime) {
    switch (mime) {
    case NRSC5_MIME_PRIMARY_IMAGE: return "album art";
    case NRSC5_MIME_STATION_LOGO: return "station logo";
    case NRSC5_MIME_NAVTEQ: return "NAVTEQ traffic";
    case NRSC5_MIME_HERE_TPEG: return "HERE TPEG traffic";
    case NRSC5_MIME_HERE_IMAGE: return "HERE traffic/weather images";
    case NRSC5_MIME_HD_TMC: return "HD TMC traffic";
    case NRSC5_MIME_HDC: return "HDC audio";
    case NRSC5_MIME_TEXT: return "text";
    case NRSC5_MIME_JPEG: return "JPEG";
    case NRSC5_MIME_PNG: return "PNG";
    case NRSC5_MIME_TTN_TPEG_1:
    case NRSC5_MIME_TTN_TPEG_2:
    case NRSC5_MIME_TTN_TPEG_3: return "TomTom TPEG traffic";
    case NRSC5_MIME_TTN_STM_TRAFFIC: return "TomTom traffic";
    case NRSC5_MIME_TTN_STM_WEATHER: return "TomTom weather";
    default: {
        char buf[16];
        snprintf(buf, sizeof(buf), "%08X", mime);
        return buf;
    }
    }
}

static const char* releaseName(int status) {
    switch (status) {
    case 0: return "commercial";
    case 1: return "engineering";
    case 2: return "patch";
    default: return "";
    }
}

static std::string versionString(const int* v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d.%d.%d.%d", v[0], v[1], v[2], v[3]);
    return buf;
}

static void deviceJson(std::string& j, const HdDecoder::Device& d) {
    if (!d.have) {
        j += "null";
        return;
    }
    j += "{\"manufacturer\":";
    jsonString(j, d.manufacturer);
    j += ",\"coreVersion\":";
    jsonString(j, versionString(d.coreVersion));
    j += ",\"coreRelease\":";
    jsonString(j, releaseName(d.coreStatus));
    j += ",\"manufacturerVersion\":";
    jsonString(j, versionString(d.manufacturerVersion));
    j += ",\"manufacturerRelease\":";
    jsonString(j, releaseName(d.manufacturerStatus));
    j += '}';
}

// One status line, without the trailing newline.
std::string statusJson(const HdDecoder::Status& s, int program) {
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
    j += ",\"alertCategories\":[";
    {
        bool firstCat = true;
        for (const std::string* name : { &s.alertCategory1Name, &s.alertCategory2Name }) {
            if (name->empty()) { continue; }
            if (!firstCat) { j += ','; }
            firstCat = false;
            jsonString(j, *name);
        }
    }
    j += "],\"alertLocationFormat\":";
    jsonString(j, s.alertLocationFormat);
    j += ",\"alertLocations\":[";
    for (size_t i = 0; i < s.alertLocations.size(); i++) {
        if (i) { j += ','; }
        j += std::to_string(s.alertLocations[i]);
    }
    j += "],\"location\":";
    if (s.haveLocation) {
        j += "{\"lat\":";
        jsonNumber(j, s.latitude, "%.5f");
        j += ",\"lon\":";
        jsonNumber(j, s.longitude, "%.5f");
        j += ",\"alt\":" + std::to_string(s.altitude) + "}";
    } else {
        j += "null";
    }
    j += ",\"localTime\":";
    if (s.haveLocalTime) {
        j += "{\"utcOffset\":" + std::to_string(s.utcOffset);
        j += ",\"dstRegional\":" + std::string(s.dstRegional ? "true" : "false");
        j += ",\"dstLocal\":" + std::string(s.dstLocal ? "true" : "false");
        j += ",\"dstSchedule\":";
        jsonString(j, dstScheduleName(s.dstSchedule));
        j += '}';
    } else {
        j += "null";
    }
    j += ",\"leapSecond\":";
    if (s.haveLeapSecond) {
        j += "{\"current\":" + std::to_string(s.leapCurrent);
        j += ",\"pending\":" + std::to_string(s.leapPending);
        j += ",\"pendingAlfn\":" + std::to_string(s.leapPendingAlfn) + "}";
    } else {
        j += "null";
    }
    j += ",\"exciter\":";
    deviceJson(j, s.exciter);
    j += ",\"importer\":";
    deviceJson(j, s.importer);
    j += ",\"importerConnected\":";
    j += s.importerConnected < 0 ? "null" : (s.importerConnected ? "true" : "false");
    j += ",\"dataServices\":[";
    for (size_t i = 0; i < s.dataServices.size(); i++) {
        const HdDecoder::DataService& d = s.dataServices[i];
        if (i) { j += ','; }
        j += "{\"type\":" + std::to_string(d.type);
        j += ",\"typeName\":";
        jsonString(j, d.typeName);
        j += ",\"access\":";
        jsonString(j, accessName(d.access));
        j += ",\"mime\":";
        jsonString(j, mimeName(d.mime));
        j += '}';
    }
    j += ']';
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
        j += ",\"serviceName\":";
        jsonString(j, pr.serviceName);
        j += ",\"access\":";
        jsonString(j, accessName(pr.access));
        j += ",\"surround\":";
        j += pr.soundExp < 0 ? "null" : (pr.soundExp == 2 ? "\"Dolby Pro Logic II\"" : "\"\"");
        j += ",\"title\":";
        jsonString(j, pr.title);
        j += ",\"artist\":";
        jsonString(j, pr.artist);
        j += ",\"album\":";
        jsonString(j, pr.album);
        j += ",\"genre\":";
        jsonString(j, pr.genre);
        j += ",\"comments\":[";
        for (size_t i = 0; i < pr.comments.size(); i++) {
            const HdDecoder::Comment& c = pr.comments[i];
            if (i) { j += ','; }
            j += "{\"lang\":";
            jsonString(j, c.lang);
            j += ",\"desc\":";
            jsonString(j, c.desc);
            j += ",\"text\":";
            jsonString(j, c.text);
            j += '}';
        }
        j += "],\"commercial\":";
        if (pr.commercial.have) {
            j += "{\"price\":";
            jsonString(j, pr.commercial.price);
            j += ",\"seller\":";
            jsonString(j, pr.commercial.seller);
            j += ",\"contactUrl\":";
            jsonString(j, pr.commercial.contactUrl);
            j += ",\"description\":";
            jsonString(j, pr.commercial.description);
            j += ",\"validUntil\":";
            jsonString(j, pr.commercial.validUntil);
            j += '}';
        } else {
            j += "null";
        }
        j += ",\"artLot\":";
        j += pr.artLot >= 0 ? std::to_string(pr.artLot) : "null";
        j += ",\"audio\":";
        j += pr.audio ? "true" : "false";
        j += ",\"frames\":" + std::to_string(pr.audioFrames);
        j += ",\"errors\":" + std::to_string(pr.audioErrors);
        j += '}';
    }
    j += "]}";
    return j;
}

// ─── images ──────────────────────────────────────────────────────────────────

// One image frame: [header length u32 LE][header JSON][data length u32 LE][data].
bool writeImage(int fd, const HdDecoder::Image& img) {
    std::string h = "{\"t\":\"image\",\"kind\":";
    jsonString(h, img.kind);
    h += ",\"program\":";
    h += img.program >= 0 ? std::to_string(img.program) : "null";
    h += ",\"lot\":";
    h += img.lot >= 0 ? std::to_string(img.lot) : "null";
    h += ",\"mime\":";
    jsonString(h, img.mime);
    h += ",\"name\":";
    jsonString(h, img.name);
    h += ",\"bounds\":";
    if (img.kind == "traffic" || img.kind == "weather") {
        h += "{\"north\":";
        jsonNumber(h, img.north, "%.5f");
        h += ",\"west\":";
        jsonNumber(h, img.west, "%.5f");
        h += ",\"south\":";
        jsonNumber(h, img.south, "%.5f");
        h += ",\"east\":";
        jsonNumber(h, img.east, "%.5f");
        h += '}';
    } else {
        h += "null";
    }
    h += '}';

    auto u32 = [](uint32_t v, uint8_t* b) {
        b[0] = (uint8_t)v;
        b[1] = (uint8_t)(v >> 8);
        b[2] = (uint8_t)(v >> 16);
        b[3] = (uint8_t)(v >> 24);
    };
    uint8_t len[4];
    u32((uint32_t)h.size(), len);
    if (!writeAll(fd, len, 4) || !writeAll(fd, h.data(), h.size())) { return false; }
    u32((uint32_t)img.data.size(), len);
    return writeAll(fd, len, 4) && writeAll(fd, img.data.data(), img.data.size());
}

