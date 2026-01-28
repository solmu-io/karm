module;

#include <karm-font/ttf/fontface.h>

export module Karm.Print:pdfPrinter;

import Karm.Pdf;
import :filePrinter;
import :pdfFonts;

namespace Karm::Print {

struct PdfPage {
    PaperStock paper;
    Io::StringWriter data;
};

export struct PdfPrinter : FilePrinter {
    Vec<PdfPage> _pages;
    Opt<Pdf::Canvas> _canvas;
    Pdf::FontManager fontManager;
    Pdf::ImageManager imageManager;
    Vec<Pdf::GraphicalStateDict> graphicalStates;

    Gfx::Canvas& beginPage(PaperStock paper) override {
        auto& page = _pages.emplaceBack(paper);
        _canvas = Pdf::Canvas{page.data, paper.size(), &fontManager, &imageManager, graphicalStates};

        // Convert from the karm-pdf internal units to PDF units (1/72 inch)
        _canvas->scale(72.0 / DPI);

        // NOTE: PDF has the coordinate system origin at the bottom left corner.
        //       But we want to have it at the top left corner.
        _canvas->transform(
            {1, 0, 0, -1, 0, paper.height}
        );

        return *_canvas;
    }

    Pdf::File pdf() {
        Pdf::Ref alloc;

        Pdf::File file;
        file.header = "PDF-2.0"s;

        Pdf::Array pagesKids;
        Pdf::Ref pagesRef = alloc.alloc();

        // Fonts
        Map<usize, Pdf::Ref> fontManagerId2FontObjRef;
        for (auto& [_, value] : fontManager.mapping._els) {
            auto& [id, fontFace] = value;

            if (not fontFace.is<Font::Ttf::Fontface>()) {
                panic("no support for printing fonts other than TrueType");
            }

            TrueTypeFontAdapter ttfAdapter{
                fontFace.cast<Font::Ttf::Fontface>().unwrap(),
                alloc
            };

            auto fontRef = ttfAdapter.addToFile(file);
            fontManagerId2FontObjRef.put(id, fontRef);
        }

        // Graphical States
        Pdf::Dict graphicalStatesDict;
        for (usize i = 0; i < graphicalStates.len(); ++i) {
            auto stateRef = alloc.alloc();
            file.add(
                stateRef,
                Pdf::Dict{
                    {"Type"s, Pdf::Name{"ExtGState"s}},
                    {"ca"s, graphicalStates[i].opacity},
                }
            );

            graphicalStatesDict.put(
                Pdf::Name{Io::format("GS{}", i)},
                stateRef
            );
        }

        // Images
        Map<usize, Pdf::Ref> imageId2ObjRef;
        for (auto& img : imageManager.images) {
            // Create soft mask (alpha channel) if present
            Opt<Pdf::Ref> softMaskRef = NONE;
            if (img.alphaData) {
                auto maskRef = alloc.alloc();
                file.add(
                    maskRef,
                    Pdf::Stream{
                        .dict = Pdf::Dict{
                            {"Type"s, Pdf::Name{"XObject"s}},
                            {"Subtype"s, Pdf::Name{"Image"s}},
                            {"Width"s, (usize)img.size.x},
                            {"Height"s, (usize)img.size.y},
                            {"ColorSpace"s, Pdf::Name{"DeviceGray"s}},
                            {"BitsPerComponent"s, usize{8}},
                            {"Length"s, img.alphaData->len()},
                        },
                        .data = *img.alphaData,
                    }
                );
                softMaskRef = maskRef;
            }

            // Create the image XObject
            auto imgRef = alloc.alloc();
            Pdf::Dict imgDict{
                {"Type"s, Pdf::Name{"XObject"s}},
                {"Subtype"s, Pdf::Name{"Image"s}},
                {"Width"s, (usize)img.size.x},
                {"Height"s, (usize)img.size.y},
                {"ColorSpace"s, Pdf::Name{"DeviceRGB"s}},
                {"BitsPerComponent"s, usize{8}},
                {"Length"s, img.rgbData.len()},
            };

            if (softMaskRef) {
                imgDict.put("SMask"s, *softMaskRef);
            }

            file.add(
                imgRef,
                Pdf::Stream{
                    .dict = std::move(imgDict),
                    .data = img.rgbData,
                }
            );

            imageId2ObjRef.put(img.id, imgRef);
        }

        // Page
        for (auto& p : _pages) {
            Pdf::Ref pageRef = alloc.alloc();
            Pdf::Ref contentsRef = alloc.alloc();

            // FIXME: adding all fonts for now on each page; later, we will need to filter by page
            Pdf::Dict pageFontsDict;
            for (auto& [managerId, objRef] : fontManagerId2FontObjRef._els) {
                auto formattedName = Io::format("F{}", managerId);
                pageFontsDict.put(formattedName.str(), objRef);
            }

            // Add all images to page resources
            Pdf::Dict pageImagesDict;
            for (auto& [imageId, objRef] : imageId2ObjRef._els) {
                auto formattedName = Io::format("Im{}", imageId);
                pageImagesDict.put(formattedName.str(), objRef);
            }

            Pdf::Dict resourcesDict{
                {"Font"s, pageFontsDict},
                {"ExtGState"s, graphicalStatesDict},
            };

            // Only add XObject if there are images
            if (pageImagesDict.len() > 0) {
                resourcesDict.put("XObject"s, pageImagesDict);
            }

            file.add(
                pageRef,
                Pdf::Dict{
                    {"Type"s, Pdf::Name{"Page"s}},
                    {"Parent"s, pagesRef},
                    {"MediaBox"s,
                     Pdf::Array{
                         usize{0},
                         usize{0},
                         // Convert from the karm-pdf internal units to PDF units (1/72 inch)
                         p.paper.width * (72.0 / DPI),
                         p.paper.height * (72.0 / DPI),
                     }},
                    {
                        "Contents"s,
                        contentsRef,
                    },
                    {
                        "Resources"s,
                        std::move(resourcesDict),
                    }
                }
            );

            file.add(
                contentsRef,
                Pdf::Stream{
                    .dict = Pdf::Dict{
                        {"Length"s, p.data.bytes().len()},
                    },
                    .data = p.data.bytes(),
                }
            );

            pagesKids.pushBack(pageRef);
        }

        // Pages
        file.add(
            pagesRef,
            Pdf::Dict{
                {"Type"s, Pdf::Name{"Pages"s}},
                {"Count"s, _pages.len()},
                {"Kids"s, std::move(pagesKids)},
            }
        );

        // Catalog
        auto catalogRef = file.add(
            alloc.alloc(),
            Pdf::Dict{
                {"Type"s, Pdf::Name{"Catalog"s}},
                {"Pages"s, pagesRef},
            }
        );

        // Trailer
        file.trailer = Pdf::Dict{
            {"Size"s, file.body.len() + 1},
            {"Root"s, catalogRef},
        };

        // Sorting object by their refs, so they are printed in order
        sort(file.body._els, [](auto& a, auto& b) {
            return a.v0.num <=> b.v0.num;
        });

        return file;
    }

    Res<> write(Io::Writer& w) override {
        return pdf().write(w);
    }
};

} // namespace Karm::Print