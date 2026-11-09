#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavcodec/codec_id.h>
}

#include "remux_checks.h"

using namespace muxnexus;

namespace {

class TempFile {
public:
    TempFile() {
        snprintf(path_, sizeof(path_), "/tmp/muxnexus_test_XXXXXX");
        fd_ = mkstemp(path_);
    }
    ~TempFile() {
        if (fd_ >= 0) ::close(fd_);
        ::unlink(path_);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    int write_bytes(const std::vector<uint8_t>& data) {
        return static_cast<int>(::write(fd_, data.data(), data.size()));
    }
    int fd() const { return fd_; }

private:
    char path_[64];
    int fd_ = -1;
};

Fingerprint fingerprint_of(const std::vector<uint8_t>& data) {
    TempFile t;
    t.write_bytes(data);
    lseek(t.fd(), 0, SEEK_SET);
    return FingerprintFd(t.fd());
}

}

TEST_CASE("Fingerprint: MP4") {
    std::vector<uint8_t> data = {
        0x00, 0x00, 0x00, 0x20,
        'f','t','y','p',
        'i','s','o','m',
        0x00, 0x00, 0x02, 0x00
    };
    auto fp = fingerprint_of(data);
    CHECK(fp.kind == ContainerKind::MP4);
    CHECK(fp.label == "MP4");
    CHECK(fp.major_brand == "isom");
    CHECK(fp.confidence >= 90);
}

TEST_CASE("Fingerprint: MOV (qt brand)") {
    std::vector<uint8_t> data = {
        0x00, 0x00, 0x00, 0x14,
        'f','t','y','p',
        'q','t',' ',' ',
        0x00, 0x00, 0x00, 0x00
    };
    auto fp = fingerprint_of(data);
    CHECK(fp.kind == ContainerKind::MOV);
    CHECK(fp.major_brand == "qt  ");
}

TEST_CASE("Fingerprint: Matroska (EBML magic)") {
    std::vector<uint8_t> data = {
        0x1A, 0x45, 0xDF, 0xA3,
        0x93, 0x42, 0x82, 0x88,
        'm','a','t','r','o','s','k','a'
    };
    auto fp = fingerprint_of(data);
    CHECK(fp.kind == ContainerKind::MKV);
    CHECK(fp.confidence >= 90);
}

TEST_CASE("Fingerprint: MPEG-TS (0x47 at stride 188)") {
    std::vector<uint8_t> data(600, 0);
    data[0] = 0x47;
    data[188] = 0x47;
    data[376] = 0x47;
    auto fp = fingerprint_of(data);
    CHECK(fp.kind == ContainerKind::MPEG_TS);
}

TEST_CASE("Fingerprint: FLV") {
    // 9-byte FLV header + 4-byte PreviousTagSize0 (the fingerprinter needs >= 12 bytes).
    std::vector<uint8_t> data = {'F','L','V', 0x01, 0x05, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00};
    auto fp = fingerprint_of(data);
    CHECK(fp.kind == ContainerKind::FLV);
}

TEST_CASE("Fingerprint: Ogg") {
    // Real Ogg page headers are 27+ bytes; the fingerprinter needs >= 12 bytes.
    std::vector<uint8_t> data = {'O','g','g','S', 0x00, 0x02, 0x00, 0x00};
    data.resize(27, 0);
    auto fp = fingerprint_of(data);
    CHECK(fp.kind == ContainerKind::OGG);
}

TEST_CASE("Fingerprint: AVI") {
    std::vector<uint8_t> data = {'R','I','F','F', 0x00,0x00,0x00,0x00, 'A','V','I',' '};
    auto fp = fingerprint_of(data);
    CHECK(fp.kind == ContainerKind::AVI);
}

TEST_CASE("Fingerprint: WAV") {
    std::vector<uint8_t> data = {'R','I','F','F', 0x00,0x00,0x00,0x00, 'W','A','V','E'};
    auto fp = fingerprint_of(data);
    CHECK(fp.kind == ContainerKind::WAV);
}

TEST_CASE("Fingerprint: empty file -> UNKNOWN") {
    std::vector<uint8_t> data;
    auto fp = fingerprint_of(data);
    CHECK(fp.kind == ContainerKind::UNKNOWN);
    CHECK(fp.confidence == 0);
}

TEST_CASE("Fingerprint: random bytes -> UNKNOWN") {
    std::vector<uint8_t> data(64, 0xAB);
    auto fp = fingerprint_of(data);
    CHECK(fp.kind == ContainerKind::UNKNOWN);
}

TEST_CASE("Codec matrix: H.264 in MP4 is allowed") {
    auto r = CheckCodecContainer(AV_CODEC_ID_H264, "mp4");
    CHECK(r.rule == CompatRule::ALLOW);
}

TEST_CASE("Codec matrix: HEVC in MP4 is allowed with note") {
    auto r = CheckCodecContainer(AV_CODEC_ID_HEVC, "mp4");
    CHECK(r.rule == CompatRule::ALLOW_WITH_NOTE);
    CHECK(r.note.find("hvc1") != std::string::npos);
}

TEST_CASE("Codec matrix: HEVC in MKV is allowed") {
    auto r = CheckCodecContainer(AV_CODEC_ID_HEVC, "matroska");
    CHECK(r.rule == CompatRule::ALLOW);
}

TEST_CASE("Codec matrix: Vorbis in MP4 warns and suggests MKV") {
    auto r = CheckCodecContainer(AV_CODEC_ID_VORBIS, "mp4");
    CHECK(r.rule == CompatRule::WARN);
    CHECK(r.suggestion.find("Matroska") != std::string::npos);
}

TEST_CASE("Codec matrix: Vorbis in MKV is allowed") {
    auto r = CheckCodecContainer(AV_CODEC_ID_VORBIS, "matroska");
    CHECK(r.rule == CompatRule::ALLOW);
}

TEST_CASE("Codec matrix: Theora in MP4 fails") {
    auto r = CheckCodecContainer(AV_CODEC_ID_THEORA, "mp4");
    CHECK(r.rule == CompatRule::FAIL);
}

TEST_CASE("Codec matrix: ASS subtitle in MP4 fails") {
    auto r = CheckCodecContainer(AV_CODEC_ID_ASS, "mp4");
    CHECK(r.rule == CompatRule::FAIL);
}

TEST_CASE("Codec matrix: ASS subtitle in MKV allowed with note") {
    auto r = CheckCodecContainer(AV_CODEC_ID_ASS, "matroska");
    CHECK(r.rule == CompatRule::ALLOW_WITH_NOTE);
}

TEST_CASE("Codec matrix: TrueHD in MP4 fails") {
    auto r = CheckCodecContainer(AV_CODEC_ID_TRUEHD, "mp4");
    CHECK(r.rule == CompatRule::FAIL);
}

TEST_CASE("Codec matrix: Opus in MP4 allowed with note") {
    auto r = CheckCodecContainer(AV_CODEC_ID_OPUS, "mp4");
    CHECK(r.rule == CompatRule::ALLOW_WITH_NOTE);
    CHECK(r.note.find("dOps") != std::string::npos);
}

TEST_CASE("Codec matrix: unknown codec in MP4 is allowed (muxer decides)") {
    auto r = CheckCodecContainer(AV_CODEC_ID_NONE, "mp4");
    CHECK(r.rule == CompatRule::ALLOW);
}

TEST_CASE("FNV resume hash: deterministic for identical input") {
    std::vector<uint8_t> data(1024, 0x42);
    TempFile a, b;
    a.write_bytes(data);
    b.write_bytes(data);
    uint64_t ha = Fnv1aFdHeadTail(a.fd(), (int64_t)data.size());
    uint64_t hb = Fnv1aFdHeadTail(b.fd(), (int64_t)data.size());
    CHECK(ha == hb);
}

TEST_CASE("FNV resume hash: changes when a byte differs") {
    std::vector<uint8_t> data_a(1024, 0x42);
    std::vector<uint8_t> data_b = data_a;
    data_b[512] ^= 0xFF;

    TempFile fa, fb;
    fa.write_bytes(data_a);
    fb.write_bytes(data_b);
    CHECK(Fnv1aFdHeadTail(fa.fd(), 1024) != Fnv1aFdHeadTail(fb.fd(), 1024));
}

TEST_CASE("FNV resume hash: changes when size differs") {
    std::vector<uint8_t> data(1024, 0x42);
    TempFile f;
    f.write_bytes(data);
    CHECK(Fnv1aFdHeadTail(f.fd(), 1024) != Fnv1aFdHeadTail(f.fd(), 1025));
}