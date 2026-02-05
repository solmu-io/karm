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

// Custom error handler to avoid libjpeg calling exit()
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

// Custom source manager for reading from memory
struct MemorySourceManager {
    jpeg_source_mgr pub;
    const u8* data;
    usize size;
};

static void initSource(j_decompress_ptr) {}

static boolean fillInputBuffer(j_decompress_ptr cinfo) {
    auto* src = reinterpret_cast<MemorySourceManager*>(cinfo->src);
    // Return false to indicate no more data
    WARNMS(cinfo, JWRN_JPEG_EOF);
    // Insert a fake EOI marker
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
        if (not sniff(slice)) {
            return Error::invalidData("not a JPEG image");
        }

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

        // Request RGB output
        cinfo.out_color_space = JCS_RGB;

        if (not jpeg_start_decompress(&cinfo)) {
            jpeg_destroy_decompress(&cinfo);
            return Error::invalidData("failed to start JPEG decompression");
        }

        dec._width = cinfo.output_width;
        dec._height = cinfo.output_height;

        bool isProgressive = cinfo.progressive_mode;

        // Allocate surface
        dec._surface = Gfx::Surface::alloc({dec._width, dec._height}, Gfx::RGBA8888);

        // Read scanlines
        usize rowStride = cinfo.output_width * cinfo.output_components;
        Vec<u8> rowBuffer;
        rowBuffer.resize(rowStride);
        JSAMPROW rowPointer = rowBuffer.buf();

        auto pixels = dec._surface.unwrap()->mutPixels();

        while (cinfo.output_scanline < cinfo.output_height) {
            isize y = cinfo.output_scanline;
            jpeg_read_scanlines(&cinfo, &rowPointer, 1);

            // Convert RGB to RGBA
            for (isize x = 0; x < dec._width; ++x) {
                u8 r = rowBuffer[x * 3 + 0];
                u8 g = rowBuffer[x * 3 + 1];
                u8 b = rowBuffer[x * 3 + 2];
                pixels.store({x, y}, Gfx::Color::fromRgb(r, g, b));
            }
        }

        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);

        return Ok(dec);
    }

    isize width() const { return _width; }
    isize height() const { return _height; }

    Res<> decode(Gfx::MutPixels pixels) {
        if (not _surface) {
            return Error::invalidData("no surface available");
        }
        // Copy from surface to output pixels
        auto srcPixels = _surface.unwrap()->pixels();
        for (isize y = 0; y < _height; ++y) {
            for (isize x = 0; x < _width; ++x) {
                auto color = srcPixels.load({x, y});
                pixels.store({x, y}, color);
            }
        }
        return Ok();
    }

    void repr(Io::Emit& e) const {
        e("JPEG image (libjpeg-turbo)");
        e.indentNewline();
        e.ln("width: {}", _width);
        e.ln("height: {}", _height);
        e.deindent();
    }
};

} // namespace Karm::Image::Jpeg