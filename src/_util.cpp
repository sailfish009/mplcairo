#include "_util.h"

#ifdef MPLCAIRO_USE_LIBRAQM
#include <raqm.h>
#endif

#include <stack>

#include "_macros.h"

namespace mplcairo {

namespace {
// Load FreeType error codes.  This approach (modified to use
// std::unordered_map) is documented in fterror.h.
// NOTE that if we require FreeType>=2.6.3 then the macro can be replaced by
// FTERRORS_H_.
#undef __FTERRORS_H__
#define FT_ERRORDEF( e, v, s )  { e, s },
#define FT_ERROR_START_LIST     {
#define FT_ERROR_END_LIST       };

std::unordered_map<FT_Error, std::string> ft_errors =

#include FT_ERRORS_H
}

namespace detail {

surface_create_for_stream_t cairo_pdf_surface_create_for_stream,
                            cairo_ps_surface_create_for_stream,
                            cairo_svg_surface_create_for_stream;
void (*cairo_ps_surface_set_eps)(cairo_surface_t*, cairo_bool_t);

cairo_user_data_key_t const FILE_KEY{},
                            FT_KEY{},
                            MATHTEXT_RECTANGLE{},
                            MATHTEXT_TO_BASELINE_KEY{},
                            STATE_KEY{};
py::object UNIT_CIRCLE{};
}

py::object rc_param(std::string key)
{
  return py::module::import("matplotlib").attr("rcParams")[key.c_str()];
}

rgba_t to_rgba(py::object color, std::optional<double> alpha)
{
  return
    py::module::import("matplotlib.colors")
    .attr("to_rgba")(color, alpha).cast<rgba_t>();
}

cairo_matrix_t matrix_from_transform(py::object transform, double y0)
{
  if (!py::bool_(py::getattr(transform, "is_affine", py::bool_(true)))) {
    throw std::invalid_argument("Only affine transforms are handled");
  }
  auto py_matrix = transform.cast<py::array_t<double>>().unchecked<2>();
  if ((py_matrix.shape(0) != 3) || (py_matrix.shape(1) != 3)) {
    throw std::invalid_argument(
      "Transformation matrix must have shape (3, 3)");
  }
  return cairo_matrix_t{
    py_matrix(0, 0), -py_matrix(1, 0),
    py_matrix(0, 1), -py_matrix(1, 1),
    py_matrix(0, 2), y0 - py_matrix(1, 2)};
}

cairo_matrix_t matrix_from_transform(
  py::object transform, cairo_matrix_t* master_matrix)
{
  if (!py::bool_(py::getattr(transform, "is_affine", py::bool_(true)))) {
    throw std::invalid_argument("Only affine transforms are handled");
  }
  auto py_matrix = transform.cast<py::array_t<double>>().unchecked<2>();
  if ((py_matrix.shape(0) != 3) || (py_matrix.shape(1) != 3)) {
    throw std::invalid_argument(
      "Transformation matrix must have shape (3, 3)");
  }
  // The y flip is already handled by the master matrix.
  auto matrix = cairo_matrix_t{
    py_matrix(0, 0), py_matrix(1, 0),
    py_matrix(0, 1), py_matrix(1, 1),
    py_matrix(0, 2), py_matrix(1, 2)};
  cairo_matrix_multiply(&matrix, &matrix, master_matrix);
  return matrix;
}

bool has_vector_surface(cairo_t* cr)
{
  switch (auto type = cairo_surface_get_type(cairo_get_target(cr))) {
    case CAIRO_SURFACE_TYPE_IMAGE:
    case CAIRO_SURFACE_TYPE_XLIB:
      return false;
    case CAIRO_SURFACE_TYPE_PDF:
    case CAIRO_SURFACE_TYPE_PS:
    case CAIRO_SURFACE_TYPE_SVG:
    case CAIRO_SURFACE_TYPE_RECORDING:
    case CAIRO_SURFACE_TYPE_SCRIPT:
      return true;
    default:
      throw
        std::invalid_argument(
          "Unexpected surface type: " + std::to_string(type));
  }
}

// Same as GraphicsContextRenderer::get_additional_state() but with checking
// for cairo_t*'s that we may not have initialized.
AdditionalState& get_additional_state(cairo_t* cr)
{
  auto data = cairo_get_user_data(cr, &detail::STATE_KEY);
  if (!data) {
    throw std::runtime_error("cairo_t* missing additional state");
  }
  auto& stack = *static_cast<std::stack<AdditionalState>*>(data);
  if (stack.empty()) {
    throw std::runtime_error("cairo_t* missing additional state");
  }
  return stack.top();
}

void copy_for_marker_stamping(cairo_t* orig, cairo_t* dest)
{
  cairo_set_antialias(dest, cairo_get_antialias(orig));
  cairo_set_line_cap(dest, cairo_get_line_cap(orig));
  cairo_set_line_join(dest, cairo_get_line_join(orig));
  cairo_set_line_width(dest, cairo_get_line_width(orig));

  auto dash_count = cairo_get_dash_count(orig);
  auto dashes = std::unique_ptr<double[]>(new double[dash_count]);
  double offset;
  cairo_get_dash(orig, dashes.get(), &offset);
  cairo_set_dash(dest, dashes.get(), dash_count, offset);

  double r, g, b, a;
  CAIRO_CHECK(cairo_pattern_get_rgba, cairo_get_source(orig), &r, &g, &b, &a);
  cairo_set_source_rgba(dest, r, g, b, a);
}

// Set the current path of `cr` to `path`, after transformation by `matrix`,
// ignoring the CTM ("exact").
//
// TODO: Path clipping and snapping in the general case (with codes present).
// NOTE: Matplotlib also *rounds* the linewidth in some cases (see
// RendererAgg::_draw_path), which helps with snappiness.  We do not provide
// this behavior; instead, one should set the default linewidths appropriately
// if desired.
// NOTE: cairo requires coordinates to fit within a 24-bit signed
// integer (https://bugs.freedesktop.org/show_bug.cgi?id=20091 and
// test_simplification.test_overflow).  We simply clamp the values in the
// general case (with codes) -- proper handling would involve clipping
// of polygons and of Beziers -- and use a simple clippling algorithm
// (Cohen-Sutherland) in the simple (codeless) case as we expect most segments
// to be within the clip rectangle -- cairo will run its own clipping later
// anyways.

// A helper to store the CTM without the need to cairo_save() the full state.
// (We can't simply call cairo_transform(cr, matrix) because matrix may be
// degenerate (e.g., for zero-sized markers).  Fortunately, the cost of doing
// the transformation ourselves seems negligible, if any.)
class LoadPathContext {
  cairo_t* cr;
  cairo_matrix_t ctm;

  public:
  LoadPathContext(cairo_t* cr) : cr{cr}
  {
    cairo_get_matrix(cr, &ctm);
    cairo_identity_matrix(cr);
    cairo_new_path(cr);
  }
  ~LoadPathContext()
  {
    cairo_set_matrix(cr, &ctm);
  }
};

// This overload implements the general case.
void load_path_exact(cairo_t* cr, py::object path, cairo_matrix_t* matrix)
{
  auto const min = double(-(1 << 22)), max = double(1 << 22);
  auto lpc = LoadPathContext{cr};

  auto vertices_keepref = path.attr("vertices").cast<py::array_t<double>>();
  auto codes_keepref =
    path.attr("codes").cast<std::optional<py::array_t<int>>>();
  auto n = vertices_keepref.shape(0);
  if (vertices_keepref.shape(1) != 2) {
    throw std::invalid_argument("vertices must have shape (n, 2)");
  }
  if (!codes_keepref) {
    load_path_exact(cr, vertices_keepref, 0, n, matrix);
    return;
  }
  auto vertices = vertices_keepref.unchecked<2>();
  auto codes = codes_keepref->unchecked<1>();
  if (codes.shape(0) != n) {
    throw std::invalid_argument("Lengths of vertices and codes do not match");
  }
  // Snap control.
  auto snap = (!has_vector_surface(cr)) && get_additional_state(cr).snap;
  auto lw = cairo_get_line_width(cr);
  auto snapper =
    snap
    ? ((0 < lw) && ((lw < 1) || (std::lround(lw) % 2 == 1))
       ? [](double x) -> double { return std::floor(x) + .5; }
       : [](double x) -> double { return std::round(x); })
    // Snap between pixels if lw is exactly zero 0 (in which case the edge is
    // defined by the fill) or if lw rounds to an even value other than 0
    // (minimizing the alpha due to antialiasing).
    : [](double x) -> double { return x; };
  // Main loop.
  for (auto i = 0; i < n; ++i) {
    auto x0 = vertices(i, 0), y0 = vertices(i, 1);
    cairo_matrix_transform_point(matrix, &x0, &y0);
    auto is_finite = std::isfinite(x0) && std::isfinite(y0);
    // Better(?) than nothing.
    x0 = std::clamp(x0, min, max);
    y0 = std::clamp(y0, min, max);
    switch (static_cast<PathCode>(codes(i))) {
      case PathCode::STOP:
        break;
      case PathCode::MOVETO:
        if (is_finite) {
          cairo_move_to(cr, snapper(x0), snapper(y0));
        } else {
          cairo_new_sub_path(cr);
        }
        break;
      case PathCode::LINETO:
        if (is_finite) {
          cairo_line_to(cr, snapper(x0), snapper(y0));
        } else {
          cairo_new_sub_path(cr);
        }
        break;
      // NOTE: The semantics of nonfinite control points are tested in
      // test_simplification.test_simplify_curve: if the last point is finite,
      // it sets the current point for the next curve; otherwise, a new
      // sub-path is created.
      case PathCode::CURVE3: {
        auto x1 = vertices(i + 1, 0), y1 = vertices(i + 1, 1);
        cairo_matrix_transform_point(matrix, &x1, &y1);
        i += 1;
        auto last_finite = std::isfinite(x1) && std::isfinite(y1);
        if (last_finite) {
          x1 = std::clamp(x1, min, max);
          y1 = std::clamp(y1, min, max);
          if (is_finite && cairo_has_current_point(cr)) {
            double x_prev, y_prev;
            cairo_get_current_point(cr, &x_prev, &y_prev);
            cairo_curve_to(cr,
              (x_prev + 2 * x0) / 3, (y_prev + 2 * y0) / 3,
              (2 * x0 + x1) / 3, (2 * y0 + y1) / 3,
              snapper(x1), snapper(y1));
          } else {
            cairo_move_to(cr, snapper(x1), snapper(y1));
          }
        } else {
          cairo_new_sub_path(cr);
        }
        break;
      }
      case PathCode::CURVE4: {
        auto x1 = vertices(i + 1, 0), y1 = vertices(i + 1, 1),
             x2 = vertices(i + 2, 0), y2 = vertices(i + 2, 1);
        cairo_matrix_transform_point(matrix, &x1, &y1);
        cairo_matrix_transform_point(matrix, &x2, &y2);
        i += 2;
        auto last_finite = std::isfinite(x2) && std::isfinite(y2);
        if (last_finite) {
          x1 = std::clamp(x1, min, max);
          y1 = std::clamp(y1, min, max);
          x2 = std::clamp(x2, min, max);
          y2 = std::clamp(y2, min, max);
          if (is_finite && std::isfinite(x1) && std::isfinite(y1)
              && cairo_has_current_point(cr)) {
            cairo_curve_to(cr, x0, y0, x1, y1, snapper(x2), snapper(y2));
          } else {
            cairo_move_to(cr, snapper(x2), snapper(y2));
          }
        } else {
          cairo_new_sub_path(cr);
        }
        break;
      }
      case PathCode::CLOSEPOLY:
        cairo_close_path(cr);
        break;
    }
  }
}

// This overload implements the case of a codeless path.  Exposing start and
// stop in the signature helps implementing support for agg.path.chunksize.
void load_path_exact(
  cairo_t* cr, py::array_t<double> vertices_keepref,
  ssize_t start, ssize_t stop, cairo_matrix_t* matrix)
{
  auto const min = double(-(1 << 22)), max = double(1 << 22);
  auto lpc = LoadPathContext{cr};

  auto vertices = vertices_keepref.unchecked<2>();
  auto n = vertices.shape(0);
  if (!((0 <= start) && (start <= stop) && (stop <= n))) {
    throw std::invalid_argument("Invalid bounds for sub-path");
  }

  auto path_data = std::vector<cairo_path_data_t>{};
  path_data.reserve(2 * (stop - start));
  auto const LEFT = 1 << 0, RIGHT = 1 << 1, BOTTOM = 1 << 2, TOP = 1 << 3;
  auto outcode = [&](double x, double y) -> int {
    auto code = 0;
    if (x < min) {
      code |= LEFT;
    } else if (x > max) {
      code |= RIGHT;
    }
    if (y < min) {
      code |= BOTTOM;
    } else if (y > max) {
      code |= TOP;
    }
    return code;
  };
  // Snap control.
  auto snap = (!has_vector_surface(cr)) && get_additional_state(cr).snap;
  auto lw = cairo_get_line_width(cr);
  auto snapper =
    (lw < 1) || (std::lround(lw) % 2 == 1)
    ? [](double x) -> double { return std::floor(x) + .5; }
    : [](double x) -> double { return std::round(x); };
  // The previous point, if any, before clipping and snapping.
  auto prev = std::optional<std::tuple<double, double>>{};
  // Main loop.
  for (auto i = start; i < stop; ++i) {
    auto x = vertices(i, 0), y = vertices(i, 1);
    cairo_matrix_transform_point(matrix, &x, &y);
    if (std::isfinite(x) && std::isfinite(y)) {
      cairo_path_data_t header, point;
      if (prev) {
        header.header = {CAIRO_PATH_LINE_TO, 2};
        auto [x_prev, y_prev] = *prev;
        prev = {x, y};
        // Cohen-Sutherland clipping: we expect most segments to be within
        // the 1 << 22 by 1 << 22 box.
        auto code0 = outcode(x_prev, y_prev);
        auto code1 = outcode(x, y);
        auto accept = false, update_prev = false;
        while (true) {
          if (!(code0 | code1)) {
            accept = true;
            break;
          } else if (code0 & code1) {
            break;
          } else {
            auto xc = 0., yc = 0.;
            auto code = code0 ? code0 : code1;
            if (code & TOP) {
              xc = x_prev + (x - x_prev) * (max - y_prev) / (y - y_prev);
              yc = max;
            } else if (code & BOTTOM) {
              xc = x_prev + (x - x_prev) * (min - y_prev) / (y - y_prev);
              yc = min;
            } else if (code & RIGHT) {
              yc = y_prev + (y - y_prev) * (max - x_prev) / (x - x_prev);
              xc = max;
            } else if (code & LEFT) {
              yc = y_prev + (y - y_prev) * (min - x_prev) / (x - x_prev);
              xc = min;
            }
            if (code == code0) {
              update_prev = true;
              x_prev = xc;
              y_prev = yc;
              code0 = outcode(x_prev, y_prev);
            } else {
              x = xc;
              y = yc;
              code1 = outcode(x, y);
            }
          }
        }
        if (accept) {
          // If we accept the segment, but the previous point moved, record
          // a MOVE_TO the new previous point (which will be followed by a
          // LINE_TO the current point).
          if (update_prev) {
            cairo_path_data_t header_prev, point_prev;
            header_prev.header = {CAIRO_PATH_MOVE_TO, 2};
            point_prev.point = {x_prev, y_prev};
            path_data.push_back(header_prev);
            path_data.push_back(point_prev);
          }
        } else {
          // If we don't accept the segment, still record a MOVE_TO the raw
          // destination, as the next point may involve snapping.
          header.header = {CAIRO_PATH_MOVE_TO, 2};
        }
        // Snapping.
        if (snap) {
          if ((x == x_prev) || (y == y_prev)) {
            // If we have a horizontal or a vertical line, snap both
            // coordinates.  NOTE: While it may make sense to only snap in
            // the direction orthogonal to the displacement, this would cause
            // e.g. axes spines to not line up properly, as they are drawn as
            // independent segments.
            path_data.back().point = {snapper(x_prev), snapper(y_prev)};
            point.point = {snapper(x), snapper(y)};
          } else {
            point.point = {x, y};
          }
        } else {
          point.point = {x, y};
        }
        // Record the point.
        path_data.push_back(header);
        path_data.push_back(point);
      } else {
        prev = {x, y};
        header.header = {CAIRO_PATH_MOVE_TO, 2};
        point.point = {x, y};
        path_data.push_back(header);
        path_data.push_back(point);
      }
    } else {
      prev = {};
    }
  }
  auto path =
    cairo_path_t{
      CAIRO_STATUS_SUCCESS, path_data.data(), int(path_data.size())};
  cairo_append_path(cr, &path);
}

// Fill and/or stroke `path` onto `cr` after transformation by `matrix`,
// ignoring the CTM ("exact").
void fill_and_stroke_exact(
  cairo_t* cr, py::object path, cairo_matrix_t* matrix,
  std::optional<rgba_t> fill, std::optional<rgba_t> stroke)
{
  cairo_save(cr);
  auto path_loaded = false;
  if (fill) {
    auto [r, g, b, a] = *fill;
    cairo_set_source_rgba(cr, r, g, b, a);
    if (path.is(detail::UNIT_CIRCLE)) {
      // Abuse the degenerate-segment handling by cairo to draw circles
      // efficiently.
      cairo_save(cr);
      cairo_new_path(cr);
      cairo_move_to(cr, matrix->x0, matrix->y0);
      cairo_close_path(cr);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      cairo_set_line_width(cr, 2);
      cairo_set_matrix(cr, matrix);
      cairo_stroke(cr);
      cairo_restore(cr);
    } else {
      if (!path_loaded) {
        load_path_exact(cr, path, matrix);
        path_loaded = true;
      }
      cairo_fill_preserve(cr);
    }
  }
  if (stroke) {
    auto [r, g, b, a] = *stroke;
    cairo_set_source_rgba(cr, r, g, b, a);
    if (!path_loaded) {
      load_path_exact(cr, path, matrix);
      path_loaded = true;
    }
    cairo_identity_matrix(cr);  // Dashes are interpreted using the CTM.
    cairo_stroke_preserve(cr);
  }
  cairo_restore(cr);
}

long get_hinting_flag()
{
  // NOTE: Should be moved out of backend_agg.
  return
    py::module::import("matplotlib.backends.backend_agg")
    .attr("get_hinting_flag")().cast<long>();
}

cairo_font_face_t* font_face_from_path(std::string path)
{
  FT_Face ft_face;
  if (auto error = FT_New_Face(_ft2Library, path.c_str(), 0, &ft_face)) {
    throw std::runtime_error(
      "FT_New_Face(_ft2Library, \"" + path + "\", 0, &ft_face) failed with "
      "error: " + ft_errors.at(error));
  }
  auto font_face =
    cairo_ft_font_face_create_for_ft_face(ft_face, get_hinting_flag());
  CAIRO_CLEANUP_CHECK(
    { cairo_font_face_destroy(font_face); FT_Done_Face(ft_face); },
    cairo_font_face_set_user_data,
    font_face, &detail::FT_KEY, ft_face, cairo_destroy_func_t(FT_Done_Face));
  return font_face;
}

cairo_font_face_t* font_face_from_prop(py::object prop)
{
  // It is probably not worth implementing an additional layer of caching here
  // as findfont already has its cache and object equality needs would also
  // need to go through Python anyways.
  auto path =
    py::module::import("matplotlib.font_manager").attr("findfont")(prop)
    .cast<std::string>();
  return font_face_from_path(path);
}

std::tuple<std::unique_ptr<cairo_glyph_t, decltype(&cairo_glyph_free)>, size_t>
  text_to_glyphs(cairo_t* cr, std::string s)
{
  auto scaled_font = cairo_get_scaled_font(cr);
#ifdef MPLCAIRO_USE_LIBRAQM
  auto ft_face = cairo_ft_scaled_font_lock_face(scaled_font);
  auto rq = raqm_create();
  if (!(rq
        && raqm_set_text_utf8(rq, s.c_str(), s.size())
        && raqm_set_freetype_face(rq, ft_face)
        && raqm_layout(rq))) {
    raqm_destroy(rq);
    cairo_ft_scaled_font_unlock_face(scaled_font);
    throw std::runtime_error("Failed to compute text layout");
  }
  auto count = size_t{};
  auto rq_glyphs = raqm_get_glyphs(rq, &count);
  auto glyphs = cairo_glyph_allocate(count);
  auto x = 0., y = 0.;
  for (auto i = 0u; i < count; ++i) {
    auto rq_glyph = rq_glyphs[i];
    glyphs[i].index = rq_glyph.index;
    glyphs[i].x = x + rq_glyph.x_offset / 64.;
    x += rq_glyph.x_advance / 64.;
    glyphs[i].y = y + rq_glyph.y_offset / 64.;
    y += rq_glyph.y_advance / 64.;
  }
  raqm_destroy(rq);
  cairo_ft_scaled_font_unlock_face(scaled_font);
#else
  auto glyphs = (cairo_glyph_t*){};
  auto count = int{};
  CAIRO_CHECK(
    cairo_scaled_font_text_to_glyphs,
    scaled_font, 0, 0, s.c_str(), s.size(),
    &glyphs, &count, nullptr, nullptr, nullptr);
#endif
  auto ptr =
    std::unique_ptr<cairo_glyph_t, decltype(&cairo_glyph_free)>{
      glyphs, cairo_glyph_free};
  return {std::move(ptr), count};
}

}
