#include "score_render.h"
#include "cairo_painter.h"
#include "draw/painter.h"
#include "engraving/dom/score.h"
#include "engraving/dom/mscore.h"
#include "engraving/rendering/iscorerenderer.h"
#include <cairo.h>
#include <cairo-pdf.h>
#include <cairo-svg.h>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace music {
namespace {
cairo_status_t append_bytes(void* context, const unsigned char* bytes, unsigned length) noexcept {
    try { static_cast<std::string*>(context)->append(reinterpret_cast<const char*>(bytes), length); return CAIRO_STATUS_SUCCESS; }
    catch (...) { return CAIRO_STATUS_WRITE_ERROR; }
}
void check(cairo_status_t status) {
    if (status != CAIRO_STATUS_SUCCESS) throw std::runtime_error(cairo_status_to_string(status));
}
}
std::string render_score(mu::engraving::Score* score, const std::string& format, int page) {
    using namespace mu::engraving;
    if (!score || !score->npages()) throw std::runtime_error("Score has no pages");
    if (page < 0 || static_cast<size_t>(page) >= score->npages()) throw std::invalid_argument("Page out of range");
    const auto renderer = muse::modularity::ioc(score->iocContext())->resolve<rendering::IScoreRenderer>("native");
    if (!renderer) throw std::runtime_error("Score renderer unavailable");
    const auto inches = renderer->pageSizeInch(score);
    const int dpi = format == "png" ? 144 : 72;
    const double width = inches.width() * dpi, height = inches.height() * dpi;
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0
        || width > 32767 || height > 32767 || width * height > 64e6)
        throw std::runtime_error("Page dimensions exceed rendering limit");
    std::string output;
    cairo_surface_t* raw = nullptr;
    if (format == "pdf") raw = cairo_pdf_surface_create_for_stream(append_bytes, &output, width, height);
    else if (format == "svg") raw = cairo_svg_surface_create_for_stream(append_bytes, &output, width, height);
    else if (format == "png") raw = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(std::ceil(width)), static_cast<int>(std::ceil(height)));
    else throw std::invalid_argument("Unsupported render format");
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(raw, cairo_surface_destroy);
    check(cairo_surface_status(surface.get()));
    std::unique_ptr<cairo_t, decltype(&cairo_destroy)> context(cairo_create(surface.get()), cairo_destroy);
    check(cairo_status(context.get()));
    // Vector surfaces start with an unbounded clip, which Cairo cannot enumerate.
    cairo_rectangle(context.get(), 0, 0, std::ceil(width), std::ceil(height));
    cairo_clip(context.get());
    auto provider = std::make_shared<muse::draw::CairoPainter>(context.get(), dpi);
    const bool wasPrinting = score->printing(), wasPdf = MScore::pdfPrinting;
    struct Restore {
        Score* score; bool printing, pdf;
        ~Restore() { score->setPrinting(printing); MScore::pdfPrinting = pdf; }
    } restore{score, wasPrinting, wasPdf};
    {
        muse::draw::Painter painter(provider, "native-export");
        rendering::IScoreRenderer::ScorePaintOptions options;
        options.isPrinting = true;
        options.deviceDpi = dpi;
        options.fromPage = format == "pdf" ? 0 : page;
        options.toPage = format == "pdf" ? static_cast<int>(score->npages()) - 1 : page;
        options.onNewPage = [&] { cairo_show_page(context.get()); check(cairo_status(context.get())); };
        renderer->paintScore(&painter, score, options);
        if (!painter.endDraw()) throw std::runtime_error("Painter did not finish");
    }
    check(cairo_status(context.get()));
    if (format == "png") check(cairo_surface_write_to_png_stream(surface.get(), append_bytes, &output));
    else cairo_show_page(context.get());
    cairo_surface_finish(surface.get());
    check(cairo_surface_status(surface.get()));
    return output;
}
}
