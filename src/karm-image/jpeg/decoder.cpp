module;

#include <karm-core/macros.h>
#include <cstdio>
#include <jpeglib.h>
#include <jerror.h>
#include <setjmp.h>
#include <cstring>

export module Karm.Image:jpeg.decoder;

import Karm.Core;
import Karm.Gfx;
import Karm.Logger;

namespace Karm::Image::Jpeg {

struct ErrorManager {
    jpeg_error_mgr pub;
    jmp_buf jumpBuffer;
    char message[JMSG_LENGTH_MAX];
};

static void errorExit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<ErrorManager*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->message);
    longjmp(err->jumpBuffer, 1);
}

struct MemorySourceManager {
    jpeg_source_mgr pub;
    const u8* data;
    usize size;
};

static void initSource(j_decompress_ptr) {}

static boolean fillInputBuffer(j_decompress_ptr cinfo) {
    auto* src = reinterpret_cast<MemorySourceManager*>(cinfo->src);
    WARNMS(cinfo, JWRN_JPEG_EOF);
    static const JOCTET eoi_buffer[2] = { 0xFF, JPEG_EOI };
    src->pub.next_input_byte = eoi_buffer;
    src->pub.bytes_in_buffer = 2;
    return TRUE;
}

static void skipInputData(j_decompress_ptr cinfo, long numBytes) {
    auto* src = reinterpret_cast<MemorySourceManager*>(cinfo->src);
    if (numBytes > 0) {
        while (numBytes > (long)src->pub.bytes_in_buffer) {
            numBytes -= (long)src->pub.bytes_in_buffer;
            src->pub.bytes_in_buffer = 0;
            (*src->pub.fill_input_buffer)(cinfo);
        }
        src->pub.next_input_byte += numBytes;
        src->pub.bytes_in_buffer -= numBytes;
    }
}

static void termSource(j_decompress_ptr) {}

static void setupMemorySource(j_decompress_ptr cinfo, const u8* data, usize size) {
    if (cinfo->src == nullptr) {
        cinfo->src = (jpeg_source_mgr*)(*cinfo->mem->alloc_small)(
            (j_common_ptr)cinfo, JPOOL_PERMANENT, sizeof(MemorySourceManager)
        );
    }

    auto* src = reinterpret_cast<MemorySourceManager*>(cinfo->src);
    src->pub.init_source = initSource;
    src->pub.fill_input_buffer = fillInputBuffer;
    src->pub.skip_input_data = skipInputData;
    src->pub.resync_to_restart = jpeg_resync_to_restart;
    src->pub.term_source = termSource;
    src->pub.bytes_in_buffer = size;
    src->pub.next_input_byte = data;
    src->data = data;
    src->size = size;
}

export struct Decoder {
    isize _width = 0;
    isize _height = 0;
    Opt<Rc<Gfx::Surface>> _surface = NONE;

    static bool sniff(Bytes slice) {
        return slice.len() > 2 and slice[0] == 0xFF and slice[1] == 0xD8;
    }

    static Res<Decoder> init(Bytes slice) {
        if (not sniff(slice))
            return Error::invalidData("not a JPEG image");

        Decoder dec{};
        jpeg_decompress_struct cinfo;
        ErrorManager jerr;

        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = errorExit;
        std::memset(jerr.message, 0, sizeof(jerr.message));

        if (setjmp(jerr.jumpBuffer)) {
            jpeg_destroy_decompress(&cinfo);
            return Error::invalidData("JPEG decompression failed");
        }

        jpeg_create_decompress(&cinfo);
        setupMemorySource(&cinfo, slice.buf(), slice.len());

        if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
            jpeg_destroy_decompress(&cinfo);
            return Error::invalidData("failed to read JPEG header");
        }

        // Request RGBA output directly — libjpeg-turbo writes straight into
        // the surface buffer, no intermediate rowBuffer, no per-pixel conversion
        cinfo.out_color_space = JCS_EXT_RGBA;

        if (not jpeg_start_decompress(&cinfo)) {
            jpeg_destroy_decompress(&cinfo);
            return Error::invalidData("failed to start JPEG decompression");
        }

        dec._width = cinfo.output_width;
        dec._height = cinfo.output_height;

        dec._surface = Gfx::Surface::alloc({dec._width, dec._height}, Gfx::RGBA8888);

        u8* surfaceBuf = static_cast<u8*>(dec._surface.unwrap()->mutPixels()._buf);
        usize surfaceStride = dec._surface.unwrap()->mutPixels()._stride;

        // Decode each scanline directly into the surface buffer — zero intermediate copies
        while (cinfo.output_scanline < cinfo.output_height) {
            isize y = cinfo.output_scanline;
            JSAMPROW row = surfaceBuf + y * surfaceStride;
            if (jpeg_read_scanlines(&cinfo, &row, 1) == 0)
                break;
        }

        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);

        return Ok(dec);
    }

    isize width() const { return _width; }
    isize height() const { return _height; }

    // decode() not needed — loadJpeg returns _surface directly
};

} // namespace Karm::Image::Jpeg