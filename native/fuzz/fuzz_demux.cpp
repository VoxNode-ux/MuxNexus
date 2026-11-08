#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

namespace {

struct MemIo {
    const uint8_t* data;
    size_t size;
    size_t pos;
};

int mem_read(void* opaque, uint8_t* buf, int buf_size) {
    auto* m = static_cast<MemIo*>(opaque);
    if (m->pos >= m->size) return AVERROR_EOF;
    size_t n = m->size - m->pos;
    if (n > (size_t)buf_size) n = buf_size;
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return (int)n;
}

int64_t mem_seek(void* opaque, int64_t offset, int whence) {
    auto* m = static_cast<MemIo*>(opaque);
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return (int64_t)m->size;
    int64_t base = 0;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = (int64_t)m->pos; break;
        case SEEK_END: base = (int64_t)m->size; break;
        default: return AVERROR(EINVAL);
    }
    int64_t target = base + offset;
    if (target < 0 || target > (int64_t)m->size) return AVERROR(EINVAL);
    m->pos = (size_t)target;
    return target;
}

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 16) return 0;

    MemIo state{data, size, 0};
    uint8_t* iobuf = static_cast<uint8_t*>(av_malloc(4096));
    if (!iobuf) return 0;

    AVIOContext* avio = avio_alloc_context(iobuf, 4096, 0, &state, mem_read, nullptr, mem_seek);
    if (!avio) { av_free(iobuf); return 0; }

    AVFormatContext* ctx = avformat_alloc_context();
    if (!ctx) { av_free(avio->buffer); avio_context_free(&avio); return 0; }
    ctx->pb = avio;
    ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&ctx, nullptr, nullptr, nullptr) >= 0) {
        avformat_find_stream_info(ctx, nullptr);
        AVPacket* pkt = av_packet_alloc();
        if (pkt) {
            for (int i = 0; i < 64; ++i) { if (av_read_frame(ctx, pkt) < 0) break; av_packet_unref(pkt); }
            av_packet_free(&pkt);
        }
        avformat_close_input(&ctx);
    }
    if (ctx) avformat_free_context(ctx);
    av_freep(&avio->buffer);
    avio_context_free(&avio);
    return 0;
}