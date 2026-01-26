module;

#include <karm-core/macros.h>
#include <cstring>

export module Karm.Image:jpeg.decoder;

import Karm.Core;
import Karm.Gfx;
import Karm.Logger;

import :jpeg.base;
import :jpeg.dct;

// JPEG Image decoder
namespace Karm::Image::Jpeg {

// MARK: Decoder ---------------------------------------------------------------
export struct Decoder {
    static usize _nextId;
    usize _id;  
    static bool sniff(Bytes slice) {
        return slice.len() > 2 and slice[0] == 0xFF and slice[1] == SOI;
    }

    static Res<Decoder> init(Bytes slice) {
        if (not sniff(slice)) {
            logError("jpeg: not a JPEG image");
            return Error::invalidData("not a JPEG image");
        }

        Decoder dec{};
        dec._id = _nextId++;
        Io::BScan s{slice};
        
        bool reachedEoi = false;
        bool expectSoi = true;
        
        while (not s.ended()) {
            u8 first = s.nextU8be();

            if (first != 0xFF) {
                logError("jpeg: invalid marker");
                return Error::invalidData("invalid marker");
            }

            u8 marker = s.nextU8be();

            // Skip padding bytes (0xff followed by 0xff)
            while (marker == 0xff and not s.ended()) {
                marker = s.nextU8be();
            }

            if (expectSoi) {
                if (marker != SOI) {
                    logError("jpeg: missing SOI marker");
                    return Error::invalidData("missing SOI marker");
                }
                expectSoi = false;
            } else if (APP0 <= marker and marker <= APP15) {
                logWarn("jpeg: skipping APP{}", marker - APP0);
                dec.skipMarker(s);
            } else if (marker == DQT) {
                try$(dec.defineQuantizationTable(s));
            } else if (marker == SOF0) {
                try$(dec.startOfFrame(s));
            } else if (marker == SOF2) {
                logError("jpeg: Progressive JPEG (SOF2) not supported");
                return Error::invalidData("progressive JPEG not supported");
            } else if (marker == DRI) {
                try$(dec.defineRestartInterval(s));
            } else if (marker == DHT) {
                try$(dec.defineHuffmanTable(s));
            } else if (marker == SOS) {
                try$(dec.startOfScan(s));
                try$(dec.decodeHuffman(s));
                reachedEoi = true;
                break;
            } else if (marker == EOI) {
                reachedEoi = true;
            } else if (marker == TEM) {
                logWarn("jpeg: ignoring TEM marker");
            } else if (marker == COM) {
                dec.skipMarker(s);
            } else {
                logWarn("jpeg: unknown marker: {:02x}", marker);
            }
        }

        if (not reachedEoi) {
            logError("jpeg: missing EOI marker");
            return Error::invalidData("missing EOI marker");
        }

        return Ok(dec);
    }

    void skipMarker(Io::BScan& s) {
        u16 len = s.nextU16be();
        if (len < 2) {
            logError("jpeg: invalid marker length: {}", len);
            return;
        }
        s.skip(len - 2);
    }

    Array<Opt<Quant>, 4> _quant;
    bool _quirkZeroBased = false;

    Res<> defineQuantizationTable(Io::BScan& x) {
        u16 len = x.nextU16be();
        Io::BScan s = x.nextBytes(len - 2);

        while (not s.ended()) {
            u8 infos = s.nextU8be();
            u8 id = infos & 0x0F;

            if (id > 3) {
                logError("jpeg: invalid quantization table id: {}", id);
                return Error::invalidData("invalid quantization table id");
            }

            Quant& quant = _quant[id].emplace();
            bool is16bit = (infos >> 4) == 1;

            for (usize i = 0; i < 64; ++i) {
                quant[ZIGZAG[i]] = is16bit ? s.nextU16be() : s.nextU8be();
            }
        }

        return Ok();
    }

    isize _width = 8;
    isize _height = 8;

    isize width() const { return _width; }

    isize height() const { return _height; }

    // Maximum sampling factors across all components
    u8 _maxHFactor = 1;
    u8 _maxVFactor = 1;

    // MCU dimensions in pixels (based on max sampling factors)
    isize mcuPixelWidth() const { return _maxHFactor * 8; }
    isize mcuPixelHeight() const { return _maxVFactor * 8; }

    // Number of MCUs in each dimension
    isize mcuCountX() const { return (_width + mcuPixelWidth() - 1) / mcuPixelWidth(); }
    isize mcuCountY() const { return (_height + mcuPixelHeight() - 1) / mcuPixelHeight(); }

    struct Component {
        u8 hFactor;
        u8 vFactor;
        u8 quantId;
        
        // Number of blocks per MCU for this component
        usize blocksPerMcu() const { return hFactor * vFactor; }
    };

    Array<Opt<Component>, 4> _components;
    usize _componentCount = 0;

    Res<> startOfFrame(Io::BScan& x) {
        u16 len = x.nextU16be();
        Io::BScan s = x.nextBytes(len - 2);

        u8 precision = s.nextU8be();
        if (precision != 8) {
            logError("jpeg: invalid precision: {}", precision);
            return Error::invalidData("invalid precision");
        }

        _height = s.nextU16be();
        _width = s.nextU16be();

        u8 componentCount = s.nextU8be();
        if (componentCount != 1 and componentCount != 3) {
            logError("jpeg: invalid component count: {}", componentCount);
            return Error::invalidData("invalid component count");
        }

        _maxHFactor = 1;
        _maxVFactor = 1;

        for (u8 i = 0; i < componentCount; ++i) {
            u8 id = s.nextU8be();

            if (id == 0) {
                logWarn("jpeg: zero-based component id");
                _quirkZeroBased = true;
            }

            if (not _quirkZeroBased) {
                id -= 1;
            }

            if (id > 3) {
                logError("jpeg: invalid component id: {}", id);
                return Error::invalidData("invalid component id");
            }

            if (_components[id]) {
                logError("jpeg: duplicate component id: {}", id);
                return Error::invalidData("duplicate component id");
            }

            u8 factors = s.nextU8be();
            u8 quantId = s.nextU8be();

            u8 hFactor = factors >> 4;
            u8 vFactor = factors & 0xF;

            if (hFactor == 0 or hFactor > 4 or vFactor == 0 or vFactor > 4) {
                logError("jpeg: invalid sampling factors: {}x{}", hFactor, vFactor);
                return Error::invalidData("invalid sampling factors");
            }

            _components[id].emplace(Component{
                hFactor,
                vFactor,
                quantId,
            });

            _maxHFactor = max(_maxHFactor, hFactor);
            _maxVFactor = max(_maxVFactor, vFactor);

            _componentCount = max(_componentCount, (usize)id + 1);
        }

        logInfo("jpeg: image {}x{}, max sampling {}x{}", 
            _width, _height, _maxHFactor, _maxVFactor);

        return Ok();
    }

    usize _restartInterval = 0;

    Res<> defineRestartInterval(Io::BScan& x) {
        u16 len = x.nextU16be();
        Io::BScan s = x.nextBytes(len - 2);

        _restartInterval = s.nextU16be();

        if (not s.ended()) {
            logError("jpeg: unexpected data after DRI marker");
            return Error::invalidData("unexpected data after DRI marker");
        }

        return Ok();
    }

    Array<Opt<Huff>, 4> _dcHuff;
    Array<Opt<Huff>, 4> _acHuff;

    Res<> defineHuffmanTable(Io::BScan& x) {
        u16 len = x.nextU16be();
        Io::BScan s = x.nextBytes(len - 2);

        while (not s.ended()) {
            u8 infos = s.nextU8be();
            u8 id = infos & 0x0F;
            bool isAc = (infos >> 4) == 1;

            if (id > 3) {
                logError("jpeg: invalid huffman table id: {}", id);
                return Error::invalidData("invalid huffman table id");
            }

            auto& table = (isAc ? _acHuff : _dcHuff)[id].emplace();

            usize sum = 0;
            for (usize i = 1; i < 17; ++i) {
                sum += s.nextU8be();
                table.offs[i] = sum;
            }

            if (sum > 162) {
                logError("jpeg: invalid huffman table length: {}", sum);
                return Error::invalidData("invalid huffman table length");
            }

            for (usize i = 0; i < sum; ++i) {
                table.syms[i] = s.nextU8be();
            }
        }

        return Ok();
    }

    struct ScanComponent {
        u8 dcHuffId;
        u8 acHuffId;
    };

    Array<Opt<ScanComponent>, 4> _scanComponents;
    u8 _ss = 0;
    u8 _se = 0;
    u8 _ah = 0;
    u8 _al = 0;

    Res<> startOfScan(Io::BScan& x) {
        if (_componentCount == 0) {
            logError("jpeg: start of scan before start of frame");
            return Error::invalidData("start of scan before start of frame");
        }

        u16 len = x.nextU16be();
        Io::BScan s = x.nextBytes(len - 2);

        u8 componentCount = s.nextU8be();
        if (componentCount != _componentCount) {
            logError("jpeg: invalid component count: {}", componentCount);
            return Error::invalidData("invalid component count");
        }

        for (u8 i = 0; i < componentCount; ++i) {
            u8 id = s.nextU8be();

            if (not _quirkZeroBased and id == 0) {
                logError("jpeg: component id is zero-based while SOF0 is not");
                return Error::invalidData("component id is zero-based without SOF0");
            }

            if (not _quirkZeroBased) {
                id -= 1;
            }

            if (id > 3) {
                logError("jpeg: invalid component id: {}", id);
                return Error::invalidData("invalid component id");
            }

            if (not _components[id]) {
                logError("jpeg: undefined component id: {}", id);
                return Error::invalidData("undefined component id");
            }

            u8 huffIds = s.nextU8be();
            u8 dcHuffId = huffIds >> 4;
            u8 acHuffId = huffIds & 0xF;

            if (dcHuffId > 3) {
                logError("jpeg: invalid dc huffman table id: {}", dcHuffId);
                return Error::invalidData("invalid dc huffman table id");
            }

            if (acHuffId > 3) {
                logError("jpeg: invalid ac huffman table id: {}", acHuffId);
                return Error::invalidData("invalid ac huffman table id");
            }

            _scanComponents[id].emplace(ScanComponent{dcHuffId, acHuffId});
        }

        _ss = s.nextU8be();
        _se = s.nextU8be();
        u8 ahAl = s.nextU8be();
        _ah = ahAl >> 4;
        _al = ahAl & 0xF;

        if (_ss != 0 or _se != 63) {
            logError("jpeg: unexpected spectral selection");
            return Error::invalidData("unexpected spectral selection");
        }

        if (_ah != 0 or _al != 0) {
            logError("jpeg: unexpected successive approximation");
            return Error::invalidData("unexpected successive approximation");
        }

        if (not s.ended()) {
            logError("jpeg: unexpected data after SOS marker");
            return Error::invalidData("unexpected data after SOS marker");
        }

        return Ok();
    }

    // Storage for decoded MCU blocks
    // Organized as: _mcuData[componentId][mcuIndex * blocksPerMcu + blockIndex]
    Array<Vec<Mcu>, 4> _mcuData;

    Res<Mcu&> decodeBlock(BitReader& bs, usize componentId, isize& prevDc) {
        if (not _scanComponents[componentId]) {
            logError("jpeg: undefined component id: {}", componentId);
            return Error::invalidData("undefined component id");
        }

        auto& sc = _scanComponents[componentId].unwrap();

        if (not _dcHuff[sc.dcHuffId]) {
            logError("jpeg: undefined dc huffman table id: {}", sc.dcHuffId);
            return Error::invalidData("undefined dc huffman table id");
        }

        auto& dcHuff = _dcHuff[sc.dcHuffId].unwrap();

        if (not _acHuff[sc.acHuffId]) {
            logError("jpeg: undefined ac huffman table id: {}", sc.acHuffId);
            return Error::invalidData("undefined ac huffman table id");
        }

        auto& acHuff = _acHuff[sc.acHuffId].unwrap();

        // Add new block
        Mcu mcu{};

        // Decode DC coefficient
        auto dcLenRes = dcHuff.next(bs);
        if (not dcLenRes) {
            return dcLenRes.none();
        }
        u8 len = dcLenRes.unwrap();

        if (len > 11) {
            logError("jpeg: invalid dc huffman code length: {}", len);
            return Error::invalidData("invalid dc huffman code length");
        }

        auto coeffRes = bs.nextBits(len);
        if (not coeffRes) {
            return coeffRes.none();
        }
        isize coeff = coeffRes.unwrap();

        if (len != 0 and coeff < (1 << (len - 1))) {
            coeff -= (1 << len) - 1;
        }

        mcu[0] = prevDc + coeff;
        prevDc = mcu[0];

        // Decode AC coefficients
        usize k = 1;
        while (k < 64) {
            auto symRes = acHuff.next(bs);
            if (not symRes) {
                return symRes.none();
            }
            u8 sym = symRes.unwrap();

            if (sym == 0) {
                // EOB - rest are zeros
                while (k < 64) {
                    mcu[ZIGZAG[k++]] = 0;
                }
                break;
            }

            u8 numZeroes = sym >> 4;

            if (sym == 0xF0) {
                // ZRL - 16 zeros
                numZeroes = 16;
            }

            if (k + numZeroes >= 64) {
                logError("jpeg: zero run length exceeds block size: {}", k + numZeroes);
                return Error::invalidData("zero run length exceeds block size");
            }

            for (usize z = 0; z < numZeroes; ++z) {
                mcu[ZIGZAG[k++]] = 0;
            }

            u8 acLen = sym & 0xF;

            if (acLen > 10) {
                logError("jpeg: invalid ac huffman code length: {}", acLen);
                return Error::invalidData("invalid ac huffman code length");
            }

            if (acLen) {
                auto acCoeffRes = bs.nextBits(acLen);
                if (not acCoeffRes) {
                    return acCoeffRes.none();
                }
                coeff = acCoeffRes.unwrap();

                if (coeff < (1 << (acLen - 1))) {
                    coeff -= (1 << acLen) - 1;
                }

                mcu[ZIGZAG[k++]] = coeff;
            }
        }

        _mcuData[componentId].pushBack(mcu);
        return Ok(_mcuData[componentId][_mcuData[componentId].len() - 1]);
    }

    Res<> decodeHuffman(Io::BScan& s) {
        usize totalMcus = mcuCountX() * mcuCountY();

        // Storage will grow as needed for each component

        Array<isize, 4> prevDc = {};
        BitReader bs{s};

        for (usize mcuIdx = 0; mcuIdx < totalMcus; ++mcuIdx) {
            // Handle restart markers
            if (_restartInterval > 0 and mcuIdx > 0 and mcuIdx % _restartInterval == 0) {
                prevDc = {};
                bs.reset();
                
                // Skip to next restart marker in stream
                // The BitReader should handle finding the marker
            }

            // For each MCU, decode blocks in component order
            // Within each component, decode hFactor * vFactor blocks
            for (usize c = 0; c < _componentCount; ++c) {
                if (not _components[c]) {
                    logError("jpeg: undefined component: {}", c);
                    return Error::invalidData("undefined component");
                }

                auto& comp = _components[c].unwrap();
                usize blocksPerMcu = comp.blocksPerMcu();

                // Decode all blocks for this component in this MCU
                // Order: row by row within the MCU
                for (usize block = 0; block < blocksPerMcu; ++block) {
                    auto res = decodeBlock(bs, c, prevDc[c]);
                    if (not res) {
                        if (strcmp(res.none().msg(), "end of scan data") == 0) {
                            goto end_decode;
                        }
                        return res.none();
                    }
                }
            }
        }

    end_decode:
        return Ok();
    }

    Res<> decode(Gfx::MutPixels pixels) {
        logInfo("jpeg: DECODE - image {}x{}, sampling {}x{}", 
            _width, _height, _maxHFactor, _maxVFactor);

        // Dequantize and IDCT all blocks
        for (usize c = 0; c < _componentCount; ++c) {
            if (not _components[c]) continue;

            auto& comp = _components[c].unwrap();

            if (not _quant[comp.quantId]) {
                logError("jpeg: undefined quantization table id: {}", comp.quantId);
                return Error::invalidData("undefined quantization table id");
            }

            auto& quant = _quant[comp.quantId].unwrap();

            for (auto& mcu : _mcuData[c]) {
                dequantize(mcu, quant);
                idct(mcu);
            }
        }

        // Now reconstruct pixels
        usize mcuCountXVal = mcuCountX();
        usize mcuCountYVal = mcuCountY();

        for (usize mcuY = 0; mcuY < mcuCountYVal; ++mcuY) {
            for (usize mcuX = 0; mcuX < mcuCountXVal; ++mcuX) {
                usize mcuIdx = mcuY * mcuCountXVal + mcuX;

                // Pixel coordinates of MCU top-left
                isize mcuPixelX = mcuX * mcuPixelWidth();
                isize mcuPixelY = mcuY * mcuPixelHeight();

                // Process each pixel in the MCU
                for (isize py = 0; py < mcuPixelHeight(); ++py) {
                    for (isize px = 0; px < mcuPixelWidth(); ++px) {
                        isize imgX = mcuPixelX + px;
                        isize imgY = mcuPixelY + py;

                        if (imgX >= _width or imgY >= _height) {
                            continue;
                        }

                        // Get Y value
                        isize yVal = getComponentValue(0, mcuIdx, px, py);

                        if (_componentCount >= 3) {
                            // Get Cb and Cr values (with subsampling)
                            isize cbVal = getComponentValue(1, mcuIdx, px, py);
                            isize crVal = getComponentValue(2, mcuIdx, px, py);

                            Gfx::YCbCr ycbcr{(f32)yVal, (f32)cbVal, (f32)crVal};
                            auto color = Gfx::yCbCrToRgb(ycbcr);
                            pixels.storeUnsafe({imgX, imgY}, color);
                        } else {
                            // Grayscale
                            isize grayVal = yVal + 128;
                            u8 gray = (u8)clamp(grayVal, (isize)0, (isize)255);
                            pixels.storeUnsafe({imgX, imgY}, Gfx::Color::fromRgb(gray, gray, gray));
                        }
                    }
                }
            }
        }

        logInfo("jpeg: decode completed");
        return Ok();
    }

    // Get the sample value for a component at a pixel position within an MCU
    // Handles subsampling by mapping pixel coordinates to the correct block and sample
    isize getComponentValue(usize componentId, usize mcuIdx, isize px, isize py) {
        if (not _components[componentId]) {
            return 0;
        }

        auto& comp = _components[componentId].unwrap();

        // Scale pixel coordinates based on subsampling ratio
        // For component with lower sampling factor, fewer blocks cover the same area
        isize scaledX = (px * comp.hFactor) / _maxHFactor;
        isize scaledY = (py * comp.vFactor) / _maxVFactor;

        // Determine which block within the MCU
        usize blockX = scaledX / 8;
        usize blockY = scaledY / 8;
        usize blockIdx = blockY * comp.hFactor + blockX;

        // Position within the block
        usize sampleX = scaledX % 8;
        usize sampleY = scaledY % 8;
        usize sampleIdx = sampleY * 8 + sampleX;

        // Get the block from storage
        usize blocksPerMcu = comp.blocksPerMcu();
        usize dataIdx = mcuIdx * blocksPerMcu + blockIdx;

        if (dataIdx >= _mcuData[componentId].len()) {
            return 0;
        }

        return _mcuData[componentId][dataIdx][sampleIdx];
    }

    void repr(Io::Emit& e) {
        e("JPEG image");
        e.indentNewline();
        e.ln("width: {}", width());
        e.ln("height: {}", height());
        e.ln("sampling: {}x{}", _maxHFactor, _maxVFactor);
        e.deindent();
    }
};
usize Decoder::_nextId = 0;

} // namespace Karm::Image::Jpeg