# ubersdr-hdradio

HD Radio (NRSC-5) decoder for UberSDR, AM only. Reads a station's IQ from stdin, writes the chosen program's decoded audio to stdout, and reports the station name, slogan, message, programs and what is playing as JSON Lines on fd 3.

Built for UberSDR as an external decoder binary: UberSDR's Go audio extension spawns it, pipes IQ into stdin and reads the audio and status back. It runs fine on its own from a recording too.

The decoder is [nrsc5](https://github.com/theori-io/nrsc5)'s library, built from a pinned commit with one patch of our own and linked in statically. See [Provenance](#provenance).

## Interface

```
ubersdr-hdradio [--input-sample-rate N] [--output-sample-rate N] [--program N]
                [--status-fd n | --no-status] [--control-fd n | --no-control]
```

| Descriptor | Direction | Carries |
|---|---|---|
| stdin | in | stereo int16 little-endian zero-IF IQ (I at even indices, Q at odd), centred on the carrier, at `--input-sample-rate` (default 48000) |
| stdout | out | the selected program's audio, stereo int16 little-endian at `--output-sample-rate` (default 48000). Written **only while the program is decoding**: nothing while acquiring, after losing sync, or for a program the station does not carry |
| fd 3 | out | JSON Lines status (`--status-fd` moves it, `--no-status` turns it off) |
| fd 4 | in | commands, one per line (`--control-fd` moves it, `--no-control` turns it off) |
| stderr | out | one line per state change: sync, lost sync, station name, program, audio starting and stopping. Nothing periodic |

fd 3 and fd 4 are optional and silent when absent, so it runs from a shell with stdout alone. It exits 0 when stdin reaches EOF or stdout's reader goes away.

### Input rate

A hybrid AM station's digital sidebands reach ±15 kHz from the carrier, so it needs UberSDR's `iq48`. An all-digital (MA3) station fits within ±9.5 kHz and decodes from `iq` (12 kHz) as well — `testdata/` has WSHE both ways — but `iq48` covers both kinds. Any rate is accepted; it is resampled to nrsc5's 46511.72 Hz internally.

The station's carrier must be at 0 Hz. nrsc5 tracks a few Hz of offset, not a mistuning.

### Status (fd 3)

One object per line, written when anything changes (at most every 250 ms) and at least once a second of input:

```bash
tail -c +45 testdata/wshe_na5b_820000Hz_iq48.wav | ./ubersdr-hdradio_amd64 3>&1 >/dev/null
```

(the `3>&1` has to come first: redirections apply left to right)

```json
{"t":"status","sync":true,"freqOffset":0.4,"psmi":2,"merLower":null,"merUpper":null,
 "ber":0.090104,"country":"US","facilityId":47104,"name":"WSHE","slogan":"HD1 ",
 "message":"www.thegamut.fm 820 The Gamut!","alert":"","location":null,"program":0,
 "audio":true,"programs":[{"program":0,"type":7,"typeName":"Adult Hits",
 "title":"Practice Smiling","artist":"V.V. Lightbody","album":"Period Piece [Clear]",
 "genre":"","audio":true,"frames":443,"errors":0}]}
```

(shown wrapped; it is one line)

| Field | Meaning |
|---|---|
| `sync` | nrsc5 has acquired the station |
| `freqOffset` | Hz, the carrier's offset from 0 as nrsc5 measures it |
| `psmi` | primary service mode |
| `merLower` `merUpper` | dB, per sideband. nrsc5 reports these for FM only, so on AM they stay `null` |
| `ber` | bit error rate of the known reference bits; `null` until first measured |
| `country` `facilityId` | from the station's SIS; `facilityId` is the FCC facility ID |
| `name` `slogan` `message` `alert` | station name, slogan, text message and emergency alert, `""` until received |
| `location` | `{lat, lon, alt}` once the station has sent one, else `null` |
| `program` | the selected program, 0 = HD1 |
| `audio` | the selected program is decoding, so stdout is carrying audio. Goes `false` on lost sync as well as when the station stops sending the program |
| `programs` | the programs heard or announced. Per program: `type`/`typeName` (the program type, e.g. 7 = Adult Hits), `title` `artist` `album` `genre` from ID3, `audio` as above, `frames`/`errors` decoded and failed audio frames |

Everything resets to empty on `reset` and at start-up; a field that has been received keeps its last value until then, since stations resend ID3 with fields left out.

Every string comes from the station, so each is cut to 1 KiB (on a UTF-8 character boundary) and control characters are replaced with spaces. A line is therefore under 100 KiB however long what the station sends; a reader should accept lines up to 256 KiB. Treat the text as untrusted when displaying it.

### Commands (fd 4)

| Command | Effect |
|---|---|
| `program N` | play program N (0–7) instead. Nothing is reset: once synced the new program's audio follows at its next frame |
| `reset` | forget the station and acquire afresh, as after a retune. Status goes back to empty, and stderr logs `lost sync` and `audio stopped` if they were set. It takes effect at the next block of input, so IQ already in the pipe from before a retune is decoded as the new station's; nrsc5 needs seconds of signal to sync, so a fraction of a second of stale input only delays it |

## Build

For release binaries — both architectures, built the way they will run:

```bash
./build.sh
```

This builds amd64 and arm64 inside `ubuntu:24.04` (the same image UberSDR's container uses for its runtime stage), refuses a binary that links anything beyond libc and libm, then plays every recording in `testdata/` through each binary as UberSDR would feed it (`test/check_sample.py`, reading `test/samples.txt`). Each must sync, name the station, show what was playing and produce audio; switching to a program the station does not carry, over fd 4, must be acknowledged and produce none; and a `reset` once the station is decoding must clear it and acquire it again. arm64 is built by running an arm64 container under binfmt/qemu, which needs:

```bash
docker run --privileged --rm tonistiigi/binfmt --install all
```

`--arch amd64` for one of them, `--no-check` to skip the checks, `--help` for the rest. For a quick edit-compile loop on this host only:

```bash
./build.sh --native
```

Or with CMake directly (needs CMake ≥ 3.24, a C/C++ compiler, `patch` and `libfftw3-dev`; nrsc5 and FAAD2 are fetched at pinned versions):

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Either way the output is `ubersdr-hdradio_<arch>`. The suffix is Go's `GOARCH` spelling, so it matches Docker's `${TARGETARCH}` and the wrapper's `runtime.GOARCH` lookup without translation.

## Releasing

The UberSDR container downloads the binaries at image build time from the moving `latest` tag:

```
https://github.com/madpsy/ubersdr-hdradio/releases/download/latest/ubersdr-hdradio_${TARGETARCH}
```

```bash
./build.sh --publish          # builds both, checks both, then asks before uploading
./build.sh --publish --yes    # answer that question in advance
```

`--publish --no-check` and `--publish --native` are refused before anything is built. Only the architectures a run built are replaced on the release. `UBERSDR_HDRADIO_REPO` and `UBERSDR_HDRADIO_TAG` override the target.

Then in `ka9q_ubersdr/docker/Dockerfile`, alongside the other decoder binaries:

```dockerfile
    && mkdir -p /opt/ubersdr-hdradio \
    && wget https://github.com/madpsy/ubersdr-hdradio/releases/download/latest/ubersdr-hdradio_${TARGETARCH} \
         -O /opt/ubersdr-hdradio/ubersdr-hdradio_${TARGETARCH} \
    && chmod +x /opt/ubersdr-hdradio/ubersdr-hdradio_${TARGETARCH} \
```

## Test data

| File | Station | IQ |
|---|---|---|
| `testdata/wshe_na5b_820000Hz_iq48.wav` | WSHE 820 kHz: AM all-digital (MA3), via the NA5B UberSDR | 16-bit, 48 kHz, lossless, 30 s |
| `testdata/wshe_na5b_820000Hz_iq12.wav` | the same station, same receiver, straight after | 16-bit, 12 kHz, lossless, 30 s |

## Provenance

- nrsc5 is fetched at a pinned commit (`NRSC5_COMMIT` in `CMakeLists.txt`) and FAAD2 at 2.11.2 with nrsc5's HDC patch applied, both checked against pinned hashes. nrsc5's own build is not used; `CMakeLists.txt` compiles the library sources it needs.
- `patches/nrsc5-ma3-timing-filter.patch` changes nrsc5. nrsc5 finds AM symbol timing through a filter that passes only 10–15 kHz from the carrier, where a hybrid station's primary sidebands are. An all-digital (MA3) station keeps its subcarriers within ±9.5 kHz, so that filter sees mostly noise and neighbouring channels, and a strong MA3 signal can fail to sync (WSHE, 820 kHz, heard strongly at NA5B, never did). Once the station has identified itself as MA3, the patch switches to a filter covering 1.5–14.7 kHz with a null at the carrier. Hybrid stations are unaffected.
- `src/rtlsdr_stub.c`, `src/rtltcp_stub.c` and `compat/rtlsdr/` exist only so nrsc5 links: its library opens RTL-SDR dongles and rtl_tcp servers as well as pipes, with no build option to leave those out. Every stubbed call fails, and none is reached, since the decoder only ever opens a pipe.

## Licence

GPL-3.0-or-later, as nrsc5 is. See `LICENSE`.
