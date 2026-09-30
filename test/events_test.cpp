// Feeds HdDecoder the nrsc5 events a short recording never produces -- a
// finished album-art or logo transfer, a HERE map, an alert, the station's
// local time and transmitter details -- and checks what reaches the status
// line and the image frames. The recordings in testdata/ cover the rest.
#include "hd_decoder.h"
#include "wire.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

static int failures = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static bool has(const std::string& json, const std::string& part) { return json.find(part) != std::string::npos; }

static const std::vector<uint8_t> JPEG = { 0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 4, 0xFF, 0xD9 };
static const std::vector<uint8_t> PNG = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13 };
static const std::vector<uint8_t> TEXT = { 'h', 'e', 'l', 'l', 'o' };

// A LOT file on a data component of audio service `service` (1 = HD1).
static void lot(HdDecoder& d, int service, uint32_t componentMime, unsigned int lotId, const std::vector<uint8_t>& data,
                unsigned int size = 0) {
    nrsc5_sig_component_t comp {};
    comp.type = NRSC5_SIG_SERVICE_DATA;
    comp.data.mime = componentMime;
    nrsc5_sig_service_t svc {};
    svc.type = NRSC5_SIG_SERVICE_AUDIO;
    svc.number = (uint16_t)service;
    nrsc5_event_t evt {};
    evt.event = NRSC5_EVENT_LOT;
    evt.lot.lot = lotId;
    evt.lot.size = size ? size : (unsigned int)data.size();
    evt.lot.data = data.data();
    evt.lot.name = "cover.jpg";
    evt.lot.service = &svc;
    evt.lot.component = &comp;
    d.injectEvent(&evt);
}

static void testImages() {
    HdDecoder d;
    lot(d, 1, NRSC5_MIME_PRIMARY_IMAGE, 42, JPEG);
    lot(d, 2, NRSC5_MIME_STATION_LOGO, 7, PNG);
    // Not images, or not ones to show: all dropped.
    lot(d, 1, NRSC5_MIME_PRIMARY_IMAGE, 43, TEXT);             // bytes are not an image
    lot(d, 1, NRSC5_MIME_TEXT, 44, JPEG);                      // not an art or logo service
    lot(d, 1, NRSC5_MIME_PRIMARY_IMAGE, 45, JPEG, 600 * 1024); // too big

    std::vector<HdDecoder::Image> imgs = d.takeImages();
    CHECK(imgs.size() == 2, "%zu images, want 2 (art and logo only)", imgs.size());
    if (imgs.size() == 2) {
        CHECK(imgs[0].kind == "art" && imgs[0].program == 0 && imgs[0].lot == 42 && imgs[0].mime == "image/jpeg",
              "art: kind %s program %d lot %d mime %s", imgs[0].kind.c_str(), imgs[0].program, imgs[0].lot, imgs[0].mime.c_str());
        CHECK(imgs[0].data == JPEG, "art bytes changed");
        CHECK(imgs[1].kind == "logo" && imgs[1].program == 1 && imgs[1].mime == "image/png",
              "logo: kind %s program %d mime %s", imgs[1].kind.c_str(), imgs[1].program, imgs[1].mime.c_str());
    }
    CHECK(d.takeImages().empty(), "images handed out twice");

    // Nothing collecting them must not grow without bound.
    for (int i = 0; i < 40; i++) { lot(d, 1, NRSC5_MIME_PRIMARY_IMAGE, 100 + i, JPEG); }
    CHECK(d.takeImages().size() == 16, "pending images not capped at 16");

    nrsc5_event_t here {};
    here.event = NRSC5_EVENT_HERE_IMAGE;
    here.here_image.image_type = NRSC5_HERE_IMAGE_WEATHER;
    here.here_image.latitude1 = 41.5f;
    here.here_image.longitude1 = -88.5f;
    here.here_image.latitude2 = 40.5f;
    here.here_image.longitude2 = -87.0f;
    here.here_image.name = "WeatherImage_0_0.png";
    here.here_image.size = (unsigned int)PNG.size();
    here.here_image.data = PNG.data();
    d.injectEvent(&here);
    imgs = d.takeImages();
    CHECK(imgs.size() == 1 && imgs[0].kind == "weather" && imgs[0].north == 41.5f && imgs[0].east == -87.0f,
          "HERE weather image not passed on with its bounds");
}

static void testImageFrame() {
    HdDecoder::Image img;
    img.kind = "art";
    img.program = 0;
    img.lot = 42;
    img.mime = "image/jpeg";
    img.name = "a\"b";
    img.data = JPEG;
    int fds[2];
    if (pipe(fds) != 0) {
        CHECK(false, "pipe");
        return;
    }
    CHECK(writeImage(fds[1], img), "writeImage failed");
    close(fds[1]);
    std::vector<uint8_t> b(4096);
    ssize_t n = read(fds[0], b.data(), b.size());
    close(fds[0]);
    b.resize(n > 0 ? (size_t)n : 0);
    auto u32 = [&](size_t at) { return (uint32_t)b[at] | (uint32_t)b[at + 1] << 8 | (uint32_t)b[at + 2] << 16 | (uint32_t)b[at + 3] << 24; };
    if (b.size() < 8) {
        CHECK(false, "frame too short (%zu bytes)", b.size());
        return;
    }
    uint32_t hl = u32(0);
    std::string header(b.begin() + 4, b.begin() + 4 + hl);
    uint32_t dl = u32(4 + hl);
    CHECK(has(header, "\"kind\":\"art\"") && has(header, "\"program\":0") && has(header, "\"lot\":42") &&
              has(header, "\"mime\":\"image/jpeg\"") && has(header, "\"name\":\"a\\\"b\"") && has(header, "\"bounds\":null"),
          "header: %s", header.c_str());
    CHECK(dl == JPEG.size() && b.size() == 8 + hl + dl && std::vector<uint8_t>(b.begin() + 8 + hl, b.end()) == JPEG,
          "data length %u, frame %zu bytes", dl, b.size());
}

static void testId3() {
    HdDecoder d;
    nrsc5_id3_comment_t comment {};
    comment.lang = (char*)"eng";
    comment.short_content_desc = (char*)"desc";
    comment.full_text = (char*)"a comment";
    nrsc5_event_t id3 {};
    id3.event = NRSC5_EVENT_ID3;
    id3.id3.program = 0;
    id3.id3.title = "Song";
    id3.id3.artist = "Band";
    id3.id3.xhdr.param = 0;
    id3.id3.xhdr.lot = 42;
    id3.id3.comments = &comment;
    id3.id3.commercial.price = (char*)"USD0.99";
    id3.id3.commercial.seller = (char*)"Shop";
    d.injectEvent(&id3);
    std::string j = statusJson(d.status(), 0);
    CHECK(has(j, "\"artLot\":42"), "art LOT not in status: %s", j.c_str());
    CHECK(has(j, "\"text\":\"a comment\""), "comment missing");
    CHECK(has(j, "\"price\":\"USD0.99\"") && has(j, "\"seller\":\"Shop\""), "commercial frame missing");

    // XHDR param 1: drop the art. Fields not resent are kept.
    nrsc5_event_t flush {};
    flush.event = NRSC5_EVENT_ID3;
    flush.id3.program = 0;
    flush.id3.xhdr.param = 1;
    flush.id3.xhdr.lot = -1;
    d.injectEvent(&flush);
    j = statusJson(d.status(), 0);
    CHECK(has(j, "\"artLot\":null"), "art not dropped on XHDR param 1");
    CHECK(has(j, "\"title\":\"Song\""), "title lost on an ID3 frame without one");

    // No XHDR at all (param -1) leaves the art alone.
    id3.id3.xhdr.param = -1;
    d.injectEvent(&id3);
    id3.id3.xhdr.param = 0;
    d.injectEvent(&id3);
    id3.id3.xhdr.param = -1;
    id3.id3.xhdr.lot = -1;
    d.injectEvent(&id3);
    CHECK(has(statusJson(d.status(), 0), "\"artLot\":42"), "an ID3 frame without XHDR dropped the art");
}

static void testStation() {
    HdDecoder d;
    nrsc5_sis_asd_t asd {};
    asd.program = 1;
    asd.access = NRSC5_ACCESS_RESTRICTED;
    asd.type = 7;
    asd.sound_exp = 2;
    nrsc5_sis_dsd_t dsd {};
    dsd.access = NRSC5_ACCESS_PUBLIC;
    dsd.type = NRSC5_SERVICE_DATA_TYPE_NEWS;
    dsd.mime_type = NRSC5_MIME_TEXT;
    int locations[2] = { 17031, 17043 };
    nrsc5_event_t sis {};
    sis.event = NRSC5_EVENT_SIS;
    sis.sis.name = "WTST";
    sis.sis.latitude = 41.88f;
    sis.sis.longitude = -87.63f;
    sis.sis.altitude = 200;
    sis.sis.alert = "Tornado warning";
    sis.sis.alert_category1 = NRSC5_ALERT_CATEGORY_WEATHER;
    sis.sis.alert_category2 = NRSC5_ALERT_CATEGORY_SAFETY;
    sis.sis.alert_location_format = NRSC5_LOCATION_FORMAT_FIPS;
    sis.sis.alert_num_locations = 2;
    sis.sis.alert_locations = locations;
    sis.sis.audio_services = &asd;
    sis.sis.data_services = &dsd;
    d.injectEvent(&sis);

    nrsc5_event_t lt {};
    lt.event = NRSC5_EVENT_LOCAL_TIME;
    lt.local_time.utc_offset = -360;
    lt.local_time.dst_regional = 1;
    lt.local_time.dst_local = 1;
    lt.local_time.dst_schedule = 1;
    d.injectEvent(&lt);

    nrsc5_event_t ex {};
    ex.event = NRSC5_EVENT_EXCITER_INFO;
    ex.exciter_info.manufacturer_id = "GG";
    int core[4] = { 1, 2, 3, 4 };
    for (int i = 0; i < 4; i++) {
        ex.exciter_info.core_version[i] = core[i];
        ex.exciter_info.manufacturer_version[i] = core[i] + 1;
    }
    ex.exciter_info.importer_connected = 1;
    d.injectEvent(&ex);

    nrsc5_event_t leap {};
    leap.event = NRSC5_EVENT_LEAP_SECOND_OFFSET;
    leap.leap_second_offset.current_offset = 18;
    leap.leap_second_offset.pending_offset = 18;
    d.injectEvent(&leap);

    nrsc5_sig_service_t svc {};
    svc.type = NRSC5_SIG_SERVICE_AUDIO;
    svc.number = 2;
    svc.name = "SPS1";
    nrsc5_event_t sig {};
    sig.event = NRSC5_EVENT_SIG;
    sig.sig.services = &svc;
    d.injectEvent(&sig);

    std::string j = statusJson(d.status(), 1);
    CHECK(has(j, "\"location\":{\"lat\":41.88000,\"lon\":-87.63000,\"alt\":200}"), "location: %s", j.c_str());
    CHECK(has(j, "\"alert\":\"Tornado warning\"") && has(j, "\"alertCategories\":[\"Weather\",\"Safety\"]") &&
              has(j, "\"alertLocationFormat\":\"FIPS\"") && has(j, "\"alertLocations\":[17031,17043]"),
          "alert: %s", j.c_str());
    CHECK(has(j, "\"access\":\"restricted\"") && has(j, "\"surround\":\"Dolby Pro Logic II\"") && has(j, "\"serviceName\":\"SPS1\""),
          "program details: %s", j.c_str());
    CHECK(has(j, "{\"type\":1,\"typeName\":\"News\",\"access\":\"public\",\"mime\":\"text\"}"), "data service: %s", j.c_str());
    CHECK(has(j, "\"localTime\":{\"utcOffset\":-360,\"dstRegional\":true,\"dstLocal\":true,\"dstSchedule\":\"US/Canada\"}"),
          "local time: %s", j.c_str());
    CHECK(has(j, "\"exciter\":{\"manufacturer\":\"GG\",\"coreVersion\":\"1.2.3.4\"") && has(j, "\"importerConnected\":true") &&
              has(j, "\"importer\":null"),
          "devices: %s", j.c_str());
    CHECK(has(j, "\"leapSecond\":{\"current\":18,\"pending\":18,\"pendingAlfn\":0}"), "leap second: %s", j.c_str());

    // A reset forgets all of it.
    d.open(HdDecoder::MODE_AM, 48000);
    d.injectEvent(&sis);
    lot(d, 1, NRSC5_MIME_PRIMARY_IMAGE, 42, JPEG);
    d.requestReset();
    float none[2] = { 0, 0 };
    d.process(none, 1);
    j = statusJson(d.status(), 0);
    CHECK(has(j, "\"location\":null") && has(j, "\"alert\":\"\"") && has(j, "\"dataServices\":[]"), "not reset: %s", j.c_str());
    CHECK(d.takeImages().empty(), "images survived a reset");
}

static void testStrings() {
    std::string j;
    jsonString(j, std::string("tab\there\x01") + "\"q\\");
    CHECK(j == "\"tab here \\\"q\\\\\"", "escaping: %s", j.c_str());
    // 1023 ASCII bytes then a 2-byte character straddling the 1 KiB cut: the
    // whole character goes, not half of it.
    std::string long_(1023, 'a');
    long_ += "\xC3\xA9";
    j.clear();
    jsonString(j, long_);
    CHECK(j.size() == 1023 + 2, "cut at %zu bytes, want 1023 plus quotes", j.size() - 2);
}

int main() {
    testImages();
    testImageFrame();
    testId3();
    testStation();
    testStrings();
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
