/*
 * freetype.c
 * Copyright (C) 2016 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "fonts.h"
#include "cleanup.h"
#include "state.h"
#include <math.h>
#include <structmember.h>
#include <ft2build.h>
#include <hb-ft.h>

#if FREETYPE_MAJOR == 2 && FREETYPE_MINOR < 7
#define FT_Bitmap_Init FT_Bitmap_New
#endif

#include FT_BITMAP_H
#include FT_TRUETYPE_TABLES_H
#include FT_MULTIPLE_MASTERS_H
#include FT_SFNT_NAMES_H

typedef union FaceIndex {
    struct {
        FT_Long ttc_index : 16;
        FT_Long variation_index : 16;
    };
    FT_Long val;
} FaceIndex;

typedef struct {
    PyObject_HEAD

    FT_Face face;
    unsigned int units_per_EM;
    int ascender, descender, height, max_advance_width, max_advance_height, underline_position, underline_thickness, strikethrough_position, strikethrough_thickness;
    int hinting, hintstyle;
    bool is_scalable, has_color, is_variable, has_svg;
    float size_in_pts;
    FT_F26Dot6 char_width, char_height;
    FT_UInt xdpi, ydpi;
    PyObject *path;
    hb_font_t *harfbuzz_font;
    hb_codepoint_t space_glyph_id;
    void *extra_data;
    free_extra_data_func free_extra_data;
    float apple_leading;
    PyObject *name_lookup_table;
    FontFeatures font_features;
} Face;
PyTypeObject Face_Type;

static PyObject* FreeType_Exception = NULL;

void
set_freetype_error(const char* prefix, int err_code) {
    int i = 0;
#undef FTERRORS_H_
#undef __FTERRORS_H__
#define FT_ERRORDEF( e, v, s )  { e, s },
#define FT_ERROR_START_LIST     {
#define FT_ERROR_END_LIST       { 0, NULL } };

    static const struct {
        int          err_code;
        const char*  err_msg;
    } ft_errors[] =

#ifdef FT_ERRORS_H
#include FT_ERRORS_H
#else
    FT_ERROR_START_LIST FT_ERROR_END_LIST
#endif

    while(ft_errors[i].err_msg != NULL) {
        if (ft_errors[i].err_code == err_code) {
            PyErr_Format(FreeType_Exception, "%s %s", prefix, ft_errors[i].err_msg);
            return;
        }
        i++;
    }
    PyErr_Format(FreeType_Exception, "%s (error code: %d)", prefix, err_code);
}

static FT_Library  library;

FT_Library
freetype_library(void) { return library; }

static int
font_units_to_pixels_y(Face *self, int x) {
    return (int)ceil((double)FT_MulFix(x, self->face->size->metrics.y_scale) / 64.0);
}

static int
font_units_to_pixels_x(Face *self, int x) {
    return (int)ceil((double)FT_MulFix(x, self->face->size->metrics.x_scale) / 64.0);
}


static int
get_load_flags(int hinting, int hintstyle, int base) {
    int flags = base;
    if (hinting) {
        if (hintstyle >= 3) flags |= FT_LOAD_TARGET_NORMAL;
        else if (0 < hintstyle) flags |= FT_LOAD_TARGET_LIGHT;
    } else flags |= FT_LOAD_NO_HINTING;
    return flags;
}


static bool
load_glyph(Face *self, int glyph_index, int load_type) {
    int flags = get_load_flags(self->hinting, self->hintstyle, load_type);
    int error = FT_Load_Glyph(self->face, glyph_index, flags);
    if (error) {
        char buf[256];
        snprintf(buf, sizeof(buf) - 1, "Failed to load glyph_index=%d load_type=%d, with error:", glyph_index, load_type);
        set_freetype_error(buf, error); return false;
    }
    return true;
}

static unsigned int
get_height_for_char(Face *self, char ch) {
    unsigned int ans = 0;
    int glyph_index = FT_Get_Char_Index(self->face, ch);
    if (load_glyph(self, glyph_index, FT_LOAD_DEFAULT)) {
        unsigned int baseline = font_units_to_pixels_y(self, self->ascender);
        FT_GlyphSlotRec *glyph = self->face->glyph;
        FT_Bitmap *bm = &glyph->bitmap;
        if (glyph->bitmap_top <= 0 || (glyph->bitmap_top > 0 && (unsigned int)glyph->bitmap_top < baseline)) {
            ans = baseline - glyph->bitmap_top + bm->rows;
        }
    }
    return ans;
}

static unsigned int
calc_cell_height(Face *self, bool for_metrics) {
    unsigned int ans = font_units_to_pixels_y(self, self->height);
    if (for_metrics) {
        unsigned int underscore_height = get_height_for_char(self, '_');
        if (underscore_height > ans) {
            if (global_state.debug_font_fallback) printf(
                "Increasing cell height by %u pixels to work around buggy font that renders underscore outside the bounding box\n", underscore_height - ans);
            return underscore_height;
        }
    }
    return ans;
}

static bool
set_font_size(Face *self, FT_F26Dot6 char_width, FT_F26Dot6 char_height, FT_UInt xdpi, FT_UInt ydpi, unsigned int desired_height, unsigned int cell_height) {
    int error = FT_Set_Char_Size(self->face, 0, char_height, xdpi, ydpi);
    if (!error) {
        self->char_width = char_width; self->char_height = char_height; self->xdpi = xdpi; self->ydpi = ydpi;
        if (self->harfbuzz_font != NULL) hb_ft_font_changed(self->harfbuzz_font);
    } else {
        if (!self->is_scalable && self->face->num_fixed_sizes > 0) {
            int32_t min_diff = INT32_MAX;
            if (desired_height == 0) desired_height = cell_height;
            if (desired_height == 0) {
                desired_height = (unsigned int)ceil(((double)char_height / 64.) * (double)ydpi / 72.);
                desired_height += (unsigned int)ceil(0.2 * desired_height);
            }
            FT_Int strike_index = -1;
            for (FT_Int i = 0; i < self->face->num_fixed_sizes; i++) {
                int h = self->face->available_sizes[i].height;
                int32_t diff = h < (int32_t)desired_height ? (int32_t)desired_height - h : h - (int32_t)desired_height;
                if (diff < min_diff) {
                    min_diff = diff;
                    strike_index = i;
                }
            }
            if (strike_index > -1) {
                error = FT_Select_Size(self->face, strike_index);
                if (error) { set_freetype_error("Failed to set char size for non-scalable font, with error:", error); return false; }
                return true;
            }
        }
        set_freetype_error("Failed to set char size, with error:", error);
        return false;
    }
    return !error;
}

bool
set_size_for_face(PyObject *s, unsigned int desired_height, bool force, FONTS_DATA_HANDLE fg) {
    Face *self = (Face*)s;
    FT_F26Dot6 w = (FT_F26Dot6)(ceil(fg->font_sz_in_pts * 64.0));
    FT_UInt xdpi = (FT_UInt)fg->logical_dpi_x, ydpi = (FT_UInt)fg->logical_dpi_y;
    if (!force && (self->char_width == w && self->char_height == w && self->xdpi == xdpi && self->ydpi == ydpi)) return true;
    ((Face*)self)->size_in_pts = (float)fg->font_sz_in_pts;
    return set_font_size(self, w, w, xdpi, ydpi, desired_height, fg->cell_height);
}

static PyObject*
set_size(Face *self, PyObject *args) {
    double font_sz_in_pts, dpi_x, dpi_y;
    if (!PyArg_ParseTuple(args, "ddd", &font_sz_in_pts, &dpi_x, &dpi_y)) return NULL;
    FT_F26Dot6 w = (FT_F26Dot6)(ceil(font_sz_in_pts * 64.0));
    FT_UInt xdpi = (FT_UInt)dpi_x, ydpi = (FT_UInt)dpi_y;
    if (self->char_width == w && self->char_height == w && self->xdpi == xdpi && self->ydpi == ydpi) { Py_RETURN_NONE; }
    self->size_in_pts = (float)font_sz_in_pts;
    if (!set_font_size(self, w, w, xdpi, ydpi, 0, 0)) return NULL;
    Py_RETURN_NONE;
}

static bool
init_ft_face(Face *self, PyObject *path, int hinting, int hintstyle, FONTS_DATA_HANDLE fg) {
#define CPY(n) self->n = self->face->n;
    CPY(units_per_EM); CPY(ascender); CPY(descender); CPY(height); CPY(max_advance_width); CPY(max_advance_height); CPY(underline_position); CPY(underline_thickness);
#undef CPY
    self->is_scalable = FT_IS_SCALABLE(self->face);
    self->has_color = FT_HAS_COLOR(self->face);
    self->is_variable = FT_HAS_MULTIPLE_MASTERS(self->face);
#ifdef FT_HAS_SVG
    self->has_svg = FT_HAS_SVG(self->face);
#else
    self->has_svg = false;
#endif
    self->hinting = hinting; self->hintstyle = hintstyle;
    if (fg && !set_size_for_face((PyObject*)self, 0, false, fg)) return false;
    self->harfbuzz_font = hb_ft_font_create(self->face, NULL);
    if (self->harfbuzz_font == NULL) { PyErr_NoMemory(); return false; }
    hb_ft_font_set_load_flags(self->harfbuzz_font, get_load_flags(self->hinting, self->hintstyle, FT_LOAD_DEFAULT));

    TT_OS2 *os2 = (TT_OS2*)FT_Get_Sfnt_Table(self->face, FT_SFNT_OS2);
    if (os2 != NULL) {
      self->strikethrough_position = os2->yStrikeoutPosition;
      self->strikethrough_thickness = os2->yStrikeoutSize;
    }

    self->path = path; Py_INCREF(self->path);
    self->space_glyph_id = glyph_id_for_codepoint((PyObject*)self, ' ');
    return true;
}

static void*
set_load_error(const char *path, int error) {
    char buf[2048];
    snprintf(buf, sizeof(buf), "Failed to load face from path: %s with error:", path);
    set_freetype_error(buf, error);
    return NULL;
}

bool
face_equals_descriptor(PyObject *face_, PyObject *descriptor) {
    Face *face = (Face*)face_;
    PyObject *t = PyDict_GetItemString(descriptor, "path");
    if (!t) return false;
    if (PyObject_RichCompareBool(face->path, t, Py_EQ) != 1) return false;
    t = PyDict_GetItemString(descriptor, "index");
    if (t && PyLong_AsLong(t) != face->face->face_index) return false;
    return true;
}

PyObject*
face_from_descriptor(PyObject *descriptor, FONTS_DATA_HANDLE fg) {
#define D(key, conv, missing_ok) { \
    PyObject *t = PyDict_GetItemString(descriptor, #key); \
    if (t == NULL) { \
        if (!missing_ok) { PyErr_SetString(PyExc_KeyError, "font descriptor is missing the key: " #key); return NULL; } \
    } else key = conv(t); \
}
    const char *path = NULL;
    long index = 0;
    bool hinting = false;
    long hint_style = 0;
    D(path, PyUnicode_AsUTF8, false);
    D(index, PyLong_AsLong, true);
    D(hinting, PyObject_IsTrue, true);
    D(hint_style, PyLong_AsLong, true);
#undef D
    RAII_PyObject(retval, Face_Type.tp_alloc(&Face_Type, 0));
    if (retval != NULL) {
        Face *self = (Face *)retval;
        int error;
        if ((error = FT_New_Face(library, path, index, &(self->face)))) { self->face = NULL; set_load_error(path, error); }
        if (!init_ft_face(self, PyDict_GetItemString(descriptor, "path"), hinting, hint_style, fg)) return NULL;
        PyObject *ns = PyDict_GetItemString(descriptor, "named_style");
        if (ns) {
            unsigned long index = PyLong_AsUnsignedLong(ns);
            if (PyErr_Occurred()) return NULL;
            if ((error = FT_Set_Named_Instance(self->face, index + 1))) return set_load_error(path, error);
        }
        PyObject *axes = PyDict_GetItemString(descriptor, "axes");
        Py_ssize_t sz;
        if (axes && (sz = PyTuple_GET_SIZE(axes))) {
            RAII_ALLOC(FT_Fixed, coords, malloc(sizeof(FT_Fixed) * sz));
            for (Py_ssize_t i = 0; i < sz; i++) {
                PyObject *t = PyTuple_GET_ITEM(axes, i);
                double val = PyFloat_AsDouble(t);
                if (PyErr_Occurred()) return NULL;
                coords[i] = (FT_Fixed)(val * 65536.0);
            }
            if ((error = FT_Set_Var_Design_Coordinates(self->face, sz, coords))) return set_load_error(path, error);
        }
        if (!create_features_for_face(postscript_name_for_face((PyObject*)self), PyDict_GetItemString(descriptor, "features"), &self->font_features)) return NULL;
    }
    Py_XINCREF(retval);
    return retval;
}

FontFeatures*
features_for_face(PyObject *s) { return &((Face*)s)->font_features; }

static PyObject*
new(PyTypeObject *type UNUSED, PyObject *args, PyObject *kw) {
    const char *path = NULL;
    long index = 0;
    PyObject *descriptor = NULL;

    static char *kwds[] = {"descriptor", "path", "index", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|Osi", kwds, &descriptor, &path, &index)) return NULL;
    if (descriptor) {
        return face_from_descriptor(descriptor, NULL);
    }
    if (path) return face_from_path(path, index, NULL);
    PyErr_SetString(PyExc_TypeError, "Must specify either path or descriptor");
    return NULL;
}

FT_Face
native_face_from_path(const char *path, int index) {
    int error;
    FT_Face ans;
    error = FT_New_Face(library, path, index, &ans);
    if (error) return set_load_error(path, error);
    return ans;
}

PyObject*
face_from_path(const char *path, int index, FONTS_DATA_HANDLE fg) {
    Face *ans = (Face*)Face_Type.tp_alloc(&Face_Type, 0);
    if (ans == NULL) return NULL;
    int error;
    error = FT_New_Face(library, path, index, &ans->face);
    if (error) { ans->face = NULL; return set_load_error(path, error); }
    if (!init_ft_face(ans, Py_None, true, 3, fg)) { Py_CLEAR(ans); return NULL; }
    return (PyObject*)ans;
}

static void
dealloc(Face* self) {
    if (self->harfbuzz_font) hb_font_destroy(self->harfbuzz_font);
    if (self->face) FT_Done_Face(self->face);
    if (self->extra_data && self->free_extra_data) self->free_extra_data(self->extra_data);
    free(self->font_features.features);
    Py_CLEAR(self->path);
    Py_CLEAR(self->name_lookup_table);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *
repr(Face *self) {
    const char *ps_name = FT_Get_Postscript_Name(self->face);
#define B(x) ((x) ? Py_True : Py_False)
    FaceIndex instance;
    instance.val = self->face->face_index;
    return PyUnicode_FromFormat(
        "Face(family=%s style=%s ps_name=%s path=%S ttc_index=%d variant=%S named_instance=%S scalable=%S color=%S)",
        self->face->family_name ? self->face->family_name : "", self->face->style_name ? self->face->style_name : "",
        ps_name ? ps_name: "", self->path, instance.ttc_index,
        B(FT_IS_VARIATION(self->face)), B(FT_IS_NAMED_INSTANCE(self->face)), B(self->is_scalable), B(self->has_color)
    );
#undef B
}

const char*
postscript_name_for_face(const PyObject *face_) {
    const Face *self = (const Face*)face_;
    const char *ps_name = FT_Get_Postscript_Name(self->face);
    return ps_name ? ps_name : "";
}

static unsigned int
calc_cell_width(Face *self) {
    unsigned int ans = 0;
    for (char_type i = 32; i < 128; i++) {
        int glyph_index = FT_Get_Char_Index(self->face, i);
        if (load_glyph(self, glyph_index, FT_LOAD_DEFAULT)) {
            ans = MAX(ans, (unsigned int)ceilf((float)self->face->glyph->metrics.horiAdvance / 64.f));
        }
    }
    if (!ans) ans = MAX(1u, (unsigned int)ceilf(self->face->size->metrics.max_advance / 64.f));
    return ans;
}


void
cell_metrics(PyObject *s, unsigned int* cell_width, unsigned int* cell_height, unsigned int* baseline, unsigned int* underline_position, unsigned int* underline_thickness, unsigned int* strikethrough_position, unsigned int* strikethrough_thickness) {
    Face *self = (Face*)s;
    *cell_width = calc_cell_width(self);
    *cell_height = calc_cell_height(self, true);
    *baseline = font_units_to_pixels_y(self, self->ascender);
    *underline_position = MIN(*cell_height - 1, (unsigned int)font_units_to_pixels_y(self, MAX(0, self->ascender - self->underline_position)));
    *underline_thickness = MAX(1, font_units_to_pixels_y(self, self->underline_thickness));

    if (self->strikethrough_position != 0) {
      *strikethrough_position = MIN(*cell_height - 1, (unsigned int)font_units_to_pixels_y(self, MAX(0, self->ascender - self->strikethrough_position)));
    } else {
      *strikethrough_position = (unsigned int)floor(*baseline * 0.65);
    }
    if (self->strikethrough_thickness > 0) {
      *strikethrough_thickness = MAX(1, font_units_to_pixels_y(self, self->strikethrough_thickness));
    } else {
      *strikethrough_thickness = *underline_thickness;
    }
}

unsigned int
glyph_id_for_codepoint(const PyObject *s, char_type cp) {
    return FT_Get_Char_Index(((Face*)s)->face, cp);
}


bool
is_glyph_empty(PyObject *s, glyph_index g) {
    Face *self = (Face*)s;
    if (!load_glyph(self, g, FT_LOAD_DEFAULT)) { PyErr_Print(); return false; }
#define M self->face->glyph->metrics
    /* printf("glyph: %u horiBearingX: %ld horiBearingY: %ld width: %ld height: %ld\n", g, M.horiBearingX, M.horiBearingY, M.width, M.height); */
    return M.width == 0;
#undef M
}

int
get_glyph_width(PyObject *s, glyph_index g) {
    Face *self = (Face*)s;
    if (!load_glyph(self, g, FT_LOAD_DEFAULT)) { PyErr_Print(); return 0; }
#define M self->face->glyph->metrics
#define B self->face->glyph->bitmap
    /* printf("glyph: %u bitmap.width: %d bitmap.rows: %d horiAdvance: %ld horiBearingX: %ld horiBearingY: %ld vertBearingX: %ld vertBearingY: %ld vertAdvance: %ld width: %ld height: %ld\n", */
    /*         g, B.width, B.rows, M.horiAdvance, M.horiBearingX, M.horiBearingY, M.vertBearingX, M.vertBearingY, M.vertAdvance, M.width, M.height); */
    return B.width ? (int)B.width : (int)(M.width / 64);
#undef M
#undef B
}

hb_font_t*
harfbuzz_font_for_face(PyObject *self) { return ((Face*)self)->harfbuzz_font; }


typedef struct {
    unsigned char* buf;
    size_t start_x, width, stride;
    size_t rows;
    FT_Pixel_Mode pixel_mode;
    bool needs_free;
    unsigned int factor, right_edge;
    int bitmap_left, bitmap_top;
} ProcessedBitmap;

static void
free_processed_bitmap(ProcessedBitmap *bm) {
    if (bm->needs_free) {
        bm->needs_free = false;
        free(bm->buf); bm->buf = NULL;
    }
}

static void
trim_borders(ProcessedBitmap *ans, size_t extra) {
    bool column_has_text = false;

    // Trim empty columns from the right side of the bitmap
    for (ssize_t x = ans->width - 1; !column_has_text && x > -1 && extra > 0; x--) {
        for (size_t y = 0; y < ans->rows && !column_has_text; y++) {
            if (ans->buf[x + y * ans->stride] > 200) column_has_text = true;
        }
        if (!column_has_text) { ans->width--; extra--; }
    }

    // Remove any remaining extra columns from the left edge of the bitmap
    ans->start_x = extra;
    ans->width -= extra;
}

static void
populate_processed_bitmap(FT_GlyphSlotRec *slot, FT_Bitmap *bitmap, ProcessedBitmap *ans, bool copy_buf) {
    ans->stride = bitmap->pitch < 0 ? -bitmap->pitch : bitmap->pitch;
    ans->rows = bitmap->rows;
    if (copy_buf) {
        ans->buf = calloc(ans->rows, ans->stride);
        if (!ans->buf) fatal("Out of memory");
        ans->needs_free = true;
        memcpy(ans->buf, bitmap->buffer, ans->rows * ans->stride);
    } else ans->buf = bitmap->buffer;
    ans->start_x = 0; ans->width = bitmap->width;
    ans->pixel_mode = bitmap->pixel_mode;
    ans->bitmap_top = slot->bitmap_top; ans->bitmap_left = slot->bitmap_left;
}

bool
freetype_convert_mono_bitmap(FT_Bitmap *src, FT_Bitmap *dest) {
    FT_Bitmap_Init(dest);
    // This also sets pixel_mode to FT_PIXEL_MODE_GRAY so we don't have to
    int error = FT_Bitmap_Convert(library, src, dest, 1);
    if (error) { set_freetype_error("Failed to convert bitmap, with error:", error); return false; }
    // Normalize gray levels to the range [0..255]
    dest->num_grays = 256;
    unsigned int stride = dest->pitch < 0 ? -dest->pitch : dest->pitch;
    for (unsigned i = 0; i < (unsigned)dest->rows; ++i) {
        // We only have 2 levels
        for (unsigned j = 0; j < (unsigned)dest->width; ++j) dest->buffer[i * stride + j] *= 255;
    }
    return true;
}

static bool
render_bitmap(Face *self, int glyph_id, ProcessedBitmap *ans, unsigned int cell_width, unsigned int cell_height, unsigned int num_cells, bool bold, bool italic, bool rescale, FONTS_DATA_HANDLE fg) {
    if (!load_glyph(self, glyph_id, FT_LOAD_RENDER)) return false;
    unsigned int max_width = cell_width * num_cells;

    // Embedded bitmap glyph?
    if (self->face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
        FT_Bitmap bitmap;
        freetype_convert_mono_bitmap(&self->face->glyph->bitmap, &bitmap);
        populate_processed_bitmap(self->face->glyph, &bitmap, ans, true);
        FT_Bitmap_Done(library, &bitmap);
    } else {
        populate_processed_bitmap(self->face->glyph, &self->face->glyph->bitmap, ans, false);
    }

    if (ans->width > max_width) {
        size_t extra = ans->width - max_width;
        if (italic && extra < cell_width / 2) {
            trim_borders(ans, extra);
        } else if (extra == 2 && num_cells == 1) {
            // there exist fonts that have bitmaps just a couple of pixels
            // wider than their advances, rather than rescale, which looks
            // bad, we just crop the bitmap on the right. See https://github.com/kovidgoyal/kitty/issues/352
        } else if (rescale && self->is_scalable && extra > 1) {
            FT_F26Dot6 char_width = self->char_width, char_height = self->char_height;
            float ar = (float)max_width / (float)ans->width;
            if (set_font_size(self, (FT_F26Dot6)((float)self->char_width * ar), (FT_F26Dot6)((float)self->char_height * ar), self->xdpi, self->ydpi, 0, fg->cell_height)) {
                free_processed_bitmap(ans);
                if (!render_bitmap(self, glyph_id, ans, cell_width, cell_height, num_cells, bold, italic, false, fg)) return false;
                if (!set_font_size(self, char_width, char_height, self->xdpi, self->ydpi, 0, fg->cell_height)) return false;
            } else return false;
        }
    }
    return true;
}

int
downsample_32bit_image(uint8_t *src, unsigned src_width, unsigned src_height, unsigned src_stride, uint8_t *dest, unsigned dest_width, unsigned dest_height) {
    // Downsample using a simple area averaging algorithm. Could probably do
    // better with bi-cubic or lanczos, but at these small sizes I don't think
    // it matters
    float ratio = MAX((float)src_width / dest_width, (float)src_height / dest_height);
    int factor = (int)ceilf(ratio);
    uint8_t *d = dest;
    for (unsigned int i = 0, sr = 0; i < dest_height; i++, sr += factor) {
        for (unsigned int j = 0, sc = 0; j < dest_width; j++, sc += factor, d += 4) {
            // calculate area average
            unsigned int r=0, g=0, b=0, a=0, count=0;
            for (unsigned int y=sr; y < MIN(sr + factor, src_height); y++) {
                uint8_t *p = src + (y * src_stride) + sc * 4;
                for (unsigned int x=sc; x < MIN(sc + factor, src_width); x++, count++) {
                    b += *(p++); g += *(p++); r += *(p++); a += *(p++);
                }
            }
            if (count) {
                d[0] = b / count; d[1] = g / count; d[2] = r / count; d[3] = a / count;
            }
        }
    }
    return factor;
}

static void
downsample_bitmap(ProcessedBitmap *bm, unsigned int width, unsigned int cell_height) {
    uint8_t *dest = calloc(4, (size_t)width * cell_height);
    if (dest == NULL) fatal("Out of memory");
    bm->factor = downsample_32bit_image(bm->buf, bm->width, bm->rows, bm->stride, dest, width, cell_height);
    bm->buf = dest; bm->needs_free = true; bm->stride = 4 * width; bm->width = width; bm->rows = cell_height;
}

static void
detect_right_edge(ProcessedBitmap *ans) {
    ans->right_edge = 0;
    for (ssize_t x = ans->width - 1; !ans->right_edge && x > -1; x--) {
        for (size_t y = 0; y < ans->rows && !ans->right_edge; y++) {
            uint8_t *p = ans->buf + x * 4 + y * ans->stride;
            if (p[3] > 20) ans->right_edge = x;
        }
    }
}

static bool
render_color_bitmap(Face *self, int glyph_id, ProcessedBitmap *ans, unsigned int cell_width, unsigned int cell_height, unsigned int num_cells, unsigned int baseline UNUSED) {
    unsigned short best = 0, diff = USHRT_MAX;
    if (self->face->num_fixed_sizes > 0) {
        const short limit = self->face->num_fixed_sizes;
        for (short i = 0; i < limit; i++) {
            unsigned short w = self->face->available_sizes[i].width;
            unsigned short d = w > (unsigned short)cell_width ? w - (unsigned short)cell_width : (unsigned short)cell_width - w;
            if (d < diff) {
                diff = d;
                best = i;
            }
        }
        FT_Error error = FT_Select_Size(self->face, best);
        if (error) { set_freetype_error("Failed to set char size for non-scalable font, with error:", error); return false; }
    } else FT_Set_Char_Size(self->face, 0, self->char_height, self->xdpi, self->ydpi);
    if (!load_glyph(self, glyph_id, FT_LOAD_COLOR | FT_LOAD_RENDER)) return false;
    FT_Bitmap *bitmap = &self->face->glyph->bitmap;
    if (bitmap->pixel_mode != FT_PIXEL_MODE_BGRA) return false;
    ans->buf = bitmap->buffer;
    ans->start_x = 0; ans->width = bitmap->width;
    ans->stride = bitmap->pitch < 0 ? -bitmap->pitch : bitmap->pitch;
    ans->rows = bitmap->rows;
    ans->pixel_mode = bitmap->pixel_mode;
    if (ans->width > num_cells * cell_width + 2) downsample_bitmap(ans, num_cells * cell_width, cell_height);
    ans->bitmap_top = (int)((float)self->face->glyph->bitmap_top / ans->factor);
    ans->bitmap_left = (int)((float)self->face->glyph->bitmap_left / ans->factor);
    detect_right_edge(ans);
    return true;
}


static void
copy_color_bitmap(uint8_t *src, pixel* dest, Region *src_rect, Region *dest_rect, size_t src_stride, size_t dest_stride) {
    for (size_t sr = src_rect->top, dr = dest_rect->top; sr < src_rect->bottom && dr < dest_rect->bottom; sr++, dr++) {
        pixel *d = dest + dest_stride * dr;
        uint8_t *s = src + src_stride * sr;
        for(size_t sc = src_rect->left, dc = dest_rect->left; sc < src_rect->right && dc < dest_rect->right; sc++, dc++) {
            uint8_t *bgra = s + 4 * sc;
            if (bgra[3]) {
#define C(idx, shift) ( (uint8_t)(((float)bgra[idx] / (float)bgra[3]) * 255) << shift)
                d[dc] = C(2, 24) | C(1, 16) | C(0, 8) | bgra[3];
#undef C
        } else d[dc] = 0;
        }
    }
}

static const bool debug_placement = false;

static void
place_bitmap_in_canvas(pixel *cell, ProcessedBitmap *bm, size_t cell_width, size_t cell_height, float x_offset, float y_offset, size_t baseline, unsigned int glyph_num, pixel fg_rgb, size_t x_in_canvas, size_t y_in_canvas) {
    // We want the glyph to be positioned inside the cell based on the bearingX
    // and bearingY values, making sure that it does not overflow the cell.

    Region src = { .left = bm->start_x, .bottom = bm->rows, .right = bm->width + bm->start_x }, dest = { .bottom = cell_height, .right = cell_width };

    // Calculate column bounds
    int32_t xoff = (int32_t)(x_offset + bm->bitmap_left);
    if (debug_placement) printf(" bitmap_left: %d xoff: %d", bm->bitmap_left, xoff);
    if (xoff < 0) src.left += -xoff;
    else dest.left = xoff;
    // Move the dest start column back if the width overflows because of it, but only if we are not in a very long/infinite ligature
    if (glyph_num < 4 && dest.left > 0 && dest.left + bm->width > cell_width) {
        uint32_t extra = dest.left + bm->width - cell_width;
        dest.left = extra > dest.left ? 0 : dest.left - extra;
    }
    dest.left += x_in_canvas;

    // Calculate row bounds
    int32_t yoff = (ssize_t)(y_offset + bm->bitmap_top);
    if ((yoff > 0 && (size_t)yoff > baseline)) {
        dest.top = 0;
    } else {
        dest.top = baseline - yoff;
    }
    dest.top += y_in_canvas;

    // printf("x_offset: %d y_offset: %d src_start_row: %u src_start_column: %u dest_start_row: %u dest_start_column: %u bm_width: %lu bitmap_rows: %lu\n", xoff, yoff, src.top, src.left, dest.top, dest.left, bm->width, bm->rows);

    if (bm->pixel_mode == FT_PIXEL_MODE_BGRA) {
        copy_color_bitmap(bm->buf, cell, &src, &dest, bm->stride, cell_width);
    } else render_alpha_mask(bm->buf, cell, &src, &dest, bm->stride, cell_width, fg_rgb);
}

static const ProcessedBitmap EMPTY_PBM = {.factor = 1};

bool
render_glyphs_in_cells(PyObject *f, bool bold, bool italic, hb_glyph_info_t *info, hb_glyph_position_t *positions, unsigned int num_glyphs, pixel *canvas, unsigned int cell_width, unsigned int cell_height, unsigned int num_cells, unsigned int baseline, bool *was_colored, FONTS_DATA_HANDLE fg, bool center_glyph) {
    Face *self = (Face*)f;
    bool is_emoji = *was_colored; *was_colored = is_emoji && self->has_color;
    float x = 0.f, y = 0.f, x_offset = 0.f;
    ProcessedBitmap bm;
    unsigned int canvas_width = cell_width * num_cells;
    for (unsigned int i = 0; i < num_glyphs; i++) {
        bm = EMPTY_PBM;
        // dont load the space glyph since loading it fails for some fonts/sizes and it is anyway to be rendered as a blank
        if (info[i].codepoint != self->space_glyph_id) {
            if (*was_colored) {
                if (!render_color_bitmap(self, info[i].codepoint, &bm, cell_width, cell_height, num_cells, baseline)) {
                    if (PyErr_Occurred()) PyErr_Print();
                    if (!render_bitmap(self, info[i].codepoint, &bm, cell_width, cell_height, num_cells, bold, italic, true, fg)) {
                        free_processed_bitmap(&bm);
                        return false;
                    }
                    *was_colored = false;
                }
            } else {
                if (!render_bitmap(self, info[i].codepoint, &bm, cell_width, cell_height, num_cells, bold, italic, true, fg)) {
                    free_processed_bitmap(&bm);
                    return false;
                }
            }
        }
        x_offset = x + (float)positions[i].x_offset / 64.0f;
        y = (float)positions[i].y_offset / 64.0f;
        if (debug_placement) printf("%d: x=%f canvas: %u", i, x_offset, canvas_width);
        if ((*was_colored || self->face->glyph->metrics.width > 0) && bm.width > 0) {
            place_bitmap_in_canvas(canvas, &bm, canvas_width, cell_height, x_offset, y, baseline, i, 0xffffff, 0, 0);
        }
        if (debug_placement) printf(" adv: %f\n", (float)positions[i].x_advance / 64.0f);
        // the roundf() below is needed for infinite length ligatures, for a test case
        // use: kitty --config None -o 'font_family Fira Code' -o 'font_size 4.5' sh -c
        // "echo '|---|--------|-------|-------------|-------------|HH'; read"
        // if this causes issues with non-infinite ligatures, we could choose this behavior
        // based on num_glyphs and/or num_cells
        x += roundf((float)positions[i].x_advance / 64.0f);
        free_processed_bitmap(&bm);
    }

    if (center_glyph && num_glyphs) {
        unsigned int right_edge = (unsigned int)x, delta;
        // x_advance is wrong for colored bitmaps that have been downsampled
        if (*was_colored) right_edge = num_glyphs == 1 ? bm.right_edge : canvas_width;
        if (num_cells > 1 && right_edge < canvas_width && (delta = (canvas_width - right_edge) / 2) && delta > 1) {
            right_shift_canvas(canvas, canvas_width, cell_height, delta);
        }
    }
    return true;
}

static PyObject*
postscript_name(PyObject *s, PyObject *a UNUSED) {
    Face *self = (Face*)s;
    const char *psname = FT_Get_Postscript_Name(self->face);
    if (psname) return Py_BuildValue("s", psname);
    Py_INCREF(self->path);
    return self->path;
}

static PyObject*
identify_for_debug(PyObject *s, PyObject *a UNUSED) {
    Face *self = (Face*)s;
    FaceIndex instance;
    instance.val = self->face->face_index;
    return PyUnicode_FromFormat("%s: %V:%d", FT_Get_Postscript_Name(self->face), self->path, "[path]", instance.val);
}

static PyObject*
extra_data(PyObject *self, PyObject *a UNUSED) {
    return PyLong_FromVoidPtr(((Face*)self)->extra_data);
}

// NAME table {{{
static bool
ensure_name_table(Face *self) {
    if (self->name_lookup_table) return true;
    RAII_PyObject(ans, PyDict_New());
    if (!ans) return false;
    FT_SfntName temp;
    for (FT_UInt i = 0; i < FT_Get_Sfnt_Name_Count(self->face); i++) {
        FT_Error err = FT_Get_Sfnt_Name(self->face, i, &temp);
        if (err != 0) continue;
        if (!add_font_name_record(ans, temp.platform_id, temp.encoding_id, temp.language_id, temp.name_id, (const char*)temp.string, temp.string_len)) return NULL;
    }
    self->name_lookup_table = ans; Py_INCREF(ans);
    return true;
}

static PyObject*
get_best_name(Face *self, PyObject *nameid) {
    if (!ensure_name_table(self)) return NULL;
    return get_best_name_from_name_table(self->name_lookup_table, nameid);
}

static PyObject*
_get_best_name(Face *self, unsigned long nameid) {
    RAII_PyObject(key, PyLong_FromUnsignedLong(nameid));
    return key ? get_best_name(self, key) : NULL;
}
// }}}

static inline void cleanup_ftmm(FT_MM_Var **p) { if (*p) FT_Done_MM_Var(library, *p); *p = NULL; }

#define RAII_FTMMVar(name) __attribute__((cleanup(cleanup_ftmm))) FT_MM_Var *name = NULL

static const char*
tag_to_string(uint32_t tag, uint8_t bytes[5]) {
    bytes[0] = (tag >> 24) & 0xff;
    bytes[1] = (tag >> 16) & 0xff;
    bytes[2] = (tag >> 8) & 0xff;
    bytes[3] = (tag) & 0xff;
    bytes[4] = 0;
    return (const char*)bytes;
}

static PyObject*
convert_named_style_to_python(Face *face, const FT_Var_Named_Style *src, FT_Var_Axis *axes, unsigned num_of_axes) {
    RAII_PyObject(axis_values, PyDict_New());
    if (!axis_values) return NULL;
    uint8_t tag_buf[5] = {0};
    for (FT_UInt i = 0; i < num_of_axes; i++) {
        double val = src->coords[i] / 65536.0;
        RAII_PyObject(pval, PyFloat_FromDouble(val));
        if (!pval) return NULL;
        if (PyDict_SetItemString(axis_values, tag_to_string(axes[i].tag, tag_buf), pval) != 0) return NULL;
    }
    RAII_PyObject(name, _get_best_name(face, src->strid));
    if (!name) PyErr_Clear();
    RAII_PyObject(psname, src->psid == 0xffff ? NULL : _get_best_name(face, src->psid));
    if (!psname) PyErr_Clear();
    return Py_BuildValue("{sO sO sO}", "axis_values", axis_values, "name", name ? name : PyUnicode_FromString(""), "psname", psname ? psname : PyUnicode_FromString(""));
}

static PyObject*
convert_axis_to_python(Face *face, const FT_Var_Axis *src, FT_UInt flags) {
    PyObject *strid = _get_best_name(face, src->strid);
    if (!strid) { PyErr_Clear(); strid = PyUnicode_FromString(""); }
    uint8_t tag_buf[5] = {0};
    return Py_BuildValue("{sd sd sd sO ss ss sN}",
        "minimum", src->minimum / 65536.0, "maximum", src->maximum / 65536.0, "default", src->def / 65536.0,
        "hidden", flags & FT_VAR_AXIS_FLAG_HIDDEN ? Py_True : Py_False, "name", src->name, "tag", tag_to_string(src->tag, tag_buf),
        "strid", strid
    );
}

static PyObject*
get_variation(Face *self, PyObject *a UNUSED) {
    RAII_FTMMVar(mm);
    FT_Error err;
    if ((err = FT_Get_MM_Var(self->face, &mm))) { Py_RETURN_NONE; }
    RAII_ALLOC(FT_Fixed, coords, malloc(mm->num_axis * sizeof(FT_Fixed)));
    if (!coords) return PyErr_NoMemory();
    if ((err = FT_Get_Var_Design_Coordinates(self->face, mm->num_axis, coords))) {
        set_freetype_error("Failed to load the variation data from font with error:", err); return NULL;
    }
    RAII_PyObject(ans, PyDict_New()); if (!ans) return NULL;
    uint8_t tag[5];
    for (FT_UInt i = 0; i < mm->num_axis; i++) {
        double val = coords[i] / 65536.0;
        tag_to_string(mm->axis[i].tag, tag);
        RAII_PyObject(pval, PyFloat_FromDouble(val));
        if (!pval) return NULL;
        if (PyDict_SetItemString(ans, (const char*)tag, pval) != 0) return NULL;
    }
    Py_INCREF(ans); return ans;
}

static PyObject*
applied_features(Face *self, PyObject *a UNUSED) {
    return font_features_as_dict(&self->font_features);
}

static PyObject*
get_features(Face *self, PyObject *a UNUSED) {
    FT_Error err;
    FT_ULong length = 0;
    if (!ensure_name_table(self)) return NULL;
    RAII_PyObject(output, PyDict_New()); if (!output) return NULL;
    if ((err = FT_Load_Sfnt_Table(self->face, FT_MAKE_TAG('G', 'S', 'U', 'B'), 0, NULL, &length)) == 0) {
        RAII_ALLOC(uint8_t, table, malloc(length));
        if (!table) return PyErr_NoMemory();
        if ((err = FT_Load_Sfnt_Table(self->face, FT_MAKE_TAG('G', 'S', 'U', 'B'), 0, table, &length))) {
            set_freetype_error("Failed to load the GSUB table from font with error:", err); return NULL;
        }
        if (!read_features_from_font_table(table, length, self->name_lookup_table, output)) return NULL;
    }
    length = 0;
    if ((err = FT_Load_Sfnt_Table(self->face, FT_MAKE_TAG('G', 'P', 'O', 'S'), 0, NULL, &length)) == 0) {
        RAII_ALLOC(uint8_t, table, malloc(length));
        if (!table) return PyErr_NoMemory();
        if ((err = FT_Load_Sfnt_Table(self->face, FT_MAKE_TAG('G', 'P', 'O', 'S'), 0, table, &length))) {
            set_freetype_error("Failed to load the GSUB table from font with error:", err); return NULL;
        }
        if (!read_features_from_font_table(table, length, self->name_lookup_table, output)) return NULL;
    }
    Py_INCREF(output); return output;
}

static PyObject*
get_variable_data(Face *self, PyObject *a UNUSED) {
    if (!ensure_name_table(self)) return NULL;
    RAII_PyObject(output, PyDict_New()); if (!output) return NULL;
    RAII_PyObject(axes, PyTuple_New(0));
    RAII_PyObject(named_styles, PyTuple_New(0));
    if (!axes || !named_styles) return NULL;
    FT_Error err;
    FT_ULong length = 0;
    if ((err = FT_Load_Sfnt_Table(self->face, FT_MAKE_TAG('S', 'T', 'A', 'T'), 0, NULL, &length)) == 0) {
        RAII_ALLOC(uint8_t, table, malloc(length));
        if (!table) return PyErr_NoMemory();
        if ((err = FT_Load_Sfnt_Table(self->face, FT_MAKE_TAG('S', 'T', 'A', 'T'), 0, table, &length))) {
            set_freetype_error("Failed to load the STAT table from font with error:", err); return NULL;
        }
        if (!read_STAT_font_table(table, length, self->name_lookup_table, output)) return NULL;
    } else if (!read_STAT_font_table(NULL, 0, self->name_lookup_table, output)) return NULL;
    if (self->is_variable) {
        RAII_FTMMVar(mm);
        if ((err = FT_Get_MM_Var(self->face, &mm))) { set_freetype_error("Failed to get variable axis data from font with error:", err); return NULL; }
        if (_PyTuple_Resize(&axes, mm->num_axis) == -1) return NULL;
        if (_PyTuple_Resize(&named_styles, mm->num_namedstyles) == -1) return NULL;
        for (FT_UInt i = 0; i < mm->num_namedstyles; i++) {
            PyObject *s = convert_named_style_to_python(self, mm->namedstyle + i, mm->axis, mm->num_axis);
            if (!s) return NULL;
            PyTuple_SET_ITEM(named_styles, i, s);
        }

        for (FT_UInt i = 0; i < mm->num_axis; i++) {
            FT_UInt flags;
            FT_Get_Var_Axis_Flags(mm, i, &flags);
            PyObject *s = convert_axis_to_python(self, mm->axis + i, flags);

            if (!s) return NULL;
            PyTuple_SET_ITEM(axes, i, s);
        }
    }
    if (PyDict_SetItemString(output, "variations_postscript_name_prefix", _get_best_name(self, 25)) != 0) return NULL;
    if (PyDict_SetItemString(output, "axes", axes) != 0) return NULL;
    if (PyDict_SetItemString(output, "named_styles", named_styles) != 0) return NULL;
    Py_INCREF(output); return output;
}

StringCanvas
render_simple_text_impl(PyObject *s, const char *text, unsigned int baseline) {
    Face *self = (Face*)s;
    StringCanvas ans = {0};
    size_t num_chars = strnlen(text, 32);
    int max_char_width = font_units_to_pixels_x(self, self->face->max_advance_width);
    size_t canvas_width = max_char_width * (num_chars*2);
    size_t canvas_height = font_units_to_pixels_y(self, self->face->height) + 8;
    pixel *canvas = calloc(canvas_width * canvas_height, sizeof(pixel));
    if (!canvas) return ans;
    size_t pen_x = 0;
    ProcessedBitmap pbm;
    for (size_t n = 0; n < num_chars; n++) {
        FT_UInt glyph_index = FT_Get_Char_Index(self->face, text[n]);
        int error = FT_Load_Glyph(self->face, glyph_index, FT_LOAD_DEFAULT);
        if (error) continue;
        error = FT_Render_Glyph(self->face->glyph, FT_RENDER_MODE_NORMAL);
        if (error) continue;
        FT_Bitmap *bitmap = &self->face->glyph->bitmap;
        pbm = EMPTY_PBM;
        populate_processed_bitmap(self->face->glyph, bitmap, &pbm, false);
        place_bitmap_in_canvas(canvas, &pbm, canvas_width, canvas_height, 0, 0, baseline, n, 0xffffff, pen_x, 0);
        pen_x += self->face->glyph->advance.x >> 6;
    }
    ans.width = pen_x; ans.height = canvas_height;
    ans.canvas = malloc(ans.width * ans.height);
    if (ans.canvas) {
        for (size_t row = 0; row < ans.height; row++) {
            unsigned char *destp = ans.canvas + (ans.width * row);
            pixel *srcp = canvas + (canvas_width * row);
            for (size_t i = 0; i < ans.width; i++) destp[i] = srcp[i] & 0xff;
        }
    }
    free(canvas);
    return ans;
}

static void destroy_hb_buffer(hb_buffer_t **x) { if (*x) hb_buffer_destroy(*x); }

static PyObject*
render_sample_text(Face *self, PyObject *args) {
    unsigned long canvas_width, canvas_height;
    unsigned long fg = 0xffffff;
    PyObject *ptext;
    if (!PyArg_ParseTuple(args, "Ukk|k", &ptext, &canvas_width, &canvas_height, &fg)) return NULL;
    unsigned int cell_width, cell_height, baseline, underline_position, underline_thickness, strikethrough_position, strikethrough_thickness;
    cell_metrics((PyObject*)self, &cell_width, &cell_height, &baseline, &underline_position, &underline_thickness, &strikethrough_position, &strikethrough_thickness);
    RAII_PyObject(pbuf, PyBytes_FromStringAndSize(NULL, sizeof(pixel) * canvas_width * canvas_height));
    if (!pbuf) return NULL;
    memset(PyBytes_AS_STRING(pbuf), 0, PyBytes_GET_SIZE(pbuf));
    if (!cell_width || !cell_height) return Py_BuildValue("OII", pbuf, cell_width, cell_height);
    int num_chars_per_line = canvas_width / cell_width, num_of_lines = (int)ceil((float)PyUnicode_GET_LENGTH(ptext) / (float)num_chars_per_line);
    canvas_height = MIN(canvas_height, num_of_lines * cell_height);

    __attribute__((cleanup(destroy_hb_buffer))) hb_buffer_t *hb_buffer = hb_buffer_create();
    if (!hb_buffer_pre_allocate(hb_buffer, 4*PyUnicode_GET_LENGTH(ptext))) { PyErr_NoMemory(); return NULL; }
    for (ssize_t n = 0; n < PyUnicode_GET_LENGTH(ptext); n++) {
        Py_UCS4 codep = PyUnicode_READ_CHAR(ptext, n);
        hb_buffer_add_utf32(hb_buffer, &codep, 1, 0, 1);
    }
    hb_buffer_guess_segment_properties(hb_buffer);
    if (!HB_DIRECTION_IS_HORIZONTAL(hb_buffer_get_direction(hb_buffer))) goto end;
    hb_shape(harfbuzz_font_for_face((PyObject*)self), hb_buffer, self->font_features.features, self->font_features.count);
    unsigned int len = hb_buffer_get_length(hb_buffer);
    hb_glyph_info_t *info = hb_buffer_get_glyph_infos(hb_buffer, NULL);
    hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(hb_buffer, NULL);

    if (cell_width > canvas_width) goto end;
    pixel *canvas = (pixel*)PyBytes_AS_STRING(pbuf);
    int load_flags = get_load_flags(self->hinting, self->hintstyle, FT_LOAD_RENDER);
    int error;

    float pen_x = 0, pen_y = 0;
    for (unsigned int i = 0; i < len; i++) {
        float advance = (float)positions[i].x_advance / 64.0f;
        if (pen_x + advance > canvas_width) {
            pen_y += cell_height;
            pen_x = 0;
            if (pen_y >= canvas_height) break;
        }
        size_t x = (size_t)round(pen_x + (float)positions[i].x_offset / 64.0f);
        size_t y = (size_t)round(pen_y + (float)positions[i].y_offset / 64.0f);
        pen_x += advance;
        if ((error = FT_Load_Glyph(self->face, info[i].codepoint, load_flags))) continue;
        if ((error = FT_Render_Glyph(self->face->glyph, FT_RENDER_MODE_NORMAL))) continue;
        FT_Bitmap *bitmap = &self->face->glyph->bitmap;
        ProcessedBitmap pbm = EMPTY_PBM;
        populate_processed_bitmap(self->face->glyph, bitmap, &pbm, false);
        place_bitmap_in_canvas(canvas, &pbm, canvas_width, canvas_height, x, 0, baseline, 99999, fg, 0, y);
    }

    const uint8_t *last_pixel = (uint8_t*)PyBytes_AS_STRING(pbuf) + PyBytes_GET_SIZE(pbuf) - sizeof(pixel);
    for (uint8_t *p = (uint8_t*)PyBytes_AS_STRING(pbuf); p <= last_pixel; p += sizeof(pixel)) {
        uint8_t a = p[0], b = p[1], g = p[2], r = p[3];
        p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    }
end:
    return Py_BuildValue("OII", pbuf, cell_width, cell_height);
}


// Boilerplate {{{

static PyMemberDef members[] = {
#define MEM(name, type) {#name, type, offsetof(Face, name), READONLY, #name}
    MEM(units_per_EM, T_UINT),
    MEM(ascender, T_INT),
    MEM(descender, T_INT),
    MEM(height, T_INT),
    MEM(max_advance_width, T_INT),
    MEM(max_advance_height, T_INT),
    MEM(underline_position, T_INT),
    MEM(underline_thickness, T_INT),
    MEM(strikethrough_position, T_INT),
    MEM(strikethrough_thickness, T_INT),
    MEM(is_scalable, T_BOOL),
    MEM(is_variable, T_BOOL),
    MEM(has_svg, T_BOOL),
    MEM(has_color, T_BOOL),
    MEM(path, T_OBJECT_EX),
    {NULL}  /* Sentinel */
};

static PyMethodDef methods[] = {
    METHODB(postscript_name, METH_NOARGS),
    METHODB(identify_for_debug, METH_NOARGS),
    METHODB(extra_data, METH_NOARGS),
    METHODB(get_variable_data, METH_NOARGS),
    METHODB(applied_features, METH_NOARGS),
    METHODB(get_features, METH_NOARGS),
    METHODB(get_variation, METH_NOARGS),
    METHODB(get_best_name, METH_O),
    METHODB(set_size, METH_VARARGS),
    METHODB(render_sample_text, METH_VARARGS),
    {NULL}  /* Sentinel */
};

PyTypeObject Face_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "fast_data_types.Face",
    .tp_new = new,
    .tp_basicsize = sizeof(Face),
    .tp_dealloc = (destructor)dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "FreeType Font face",
    .tp_methods = methods,
    .tp_members = members,
    .tp_repr = (reprfunc)repr,
};

static void
free_freetype(void) {
    FT_Done_FreeType(library);
}

bool
init_freetype_library(PyObject *m) {
    if (PyType_Ready(&Face_Type) < 0) return 0;
    if (PyModule_AddObject(m, "Face", (PyObject *)&Face_Type) != 0) return 0;
    Py_INCREF(&Face_Type);
    FreeType_Exception = PyErr_NewException("fast_data_types.FreeTypeError", NULL, NULL);
    if (FreeType_Exception == NULL) return false;
    if (PyModule_AddObject(m, "FreeTypeError", FreeType_Exception) != 0) return false;
    int error = FT_Init_FreeType(&library);
    if (error) {
        set_freetype_error("Failed to initialize FreeType library, with error:", error);
        return false;
    }
    register_at_exit_cleanup_func(FREETYPE_CLEANUP_FUNC, free_freetype);
    return true;
}

// }}}
