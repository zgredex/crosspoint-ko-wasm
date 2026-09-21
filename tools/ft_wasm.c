/*
 * ft_wasm.c - minimal FreeType shim so the .epdfont converter can run in the BROWSER.
 *
 * Why this exists: converting an OTF/TTF to the device's .epdfont used to require
 * tools/ttf_to_epdfont_fast.py (freetype-py + fontTools), i.e. a local Python install.
 * The hosted site has no backend, so the font feature dead-ended there. This shim gives
 * the page the same rasterizer the local tool uses - FreeType itself - compiled to
 * WebAssembly, and web/epdfont.js does the container packing.
 *
 * Faithful to the Python path in the ways that matter for byte-comparable output:
 *   - same call: FT_Set_Char_Size(size<<6, size<<6, 150, 150)
 *   - same load flags: FT_LOAD_RENDER, or FT_LOAD_NO_BITMAP + FT_Outline_Embolden
 *     then FT_Render_Glyph(FT_RENDER_MODE_NORMAL)
 *   - metrics read raw (26.6), so the JS side applies the tool's norm_floor/norm_ceil
 *   - bitmap rows are copied out CONTIGUOUS (w*h bytes, pitch padding stripped), which
 *     is what the Python packer assumes when it walks the buffer flat
 *   - a face STACK, matching the tool's multi-file behaviour: the first face that has a
 *     codepoint wins
 *   - variable fonts: FT_Set_Var_Design_Coordinates instead of fontTools instancing
 *     (equivalent for rasterization; see docs/font-conversion-in-browser.md)
 *
 * build: scripts/build_ft_wasm.sh  (needs emcc; FreeType comes from -sUSE_FREETYPE=1)
 */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_MULTIPLE_MASTERS_H

#include <emscripten.h>

#define FTW_MAX_FACES 4
#define FTW_MAX_BITMAP_BYTES (4u * 1024u * 1024u)

static FT_Library g_lib = NULL;
static FT_Face g_faces[FTW_MAX_FACES];
static unsigned char* g_face_data[FTW_MAX_FACES]; /* owned copies */
static int g_face_count = 0;
static unsigned char* g_rows = NULL; /* contiguous bitmap rows, grown as needed */
static size_t g_rows_cap = 0;

static int bitmap_within_limit(const FT_Bitmap* bm) {
  if (!bm) return 0;
  if (bm->width == 0 || bm->rows == 0) return 1;
  if ((size_t)bm->width > FTW_MAX_BITMAP_BYTES / (size_t)bm->rows) return 0;
  const size_t tight = (size_t)bm->width * (size_t)bm->rows;
  const size_t pitch = bm->pitch < 0 ? (size_t)(-(int64_t)bm->pitch) : (size_t)bm->pitch;
  if (pitch < bm->width || pitch > FTW_MAX_BITMAP_BYTES / (size_t)bm->rows) return 0;
  return tight <= FTW_MAX_BITMAP_BYTES && pitch * (size_t)bm->rows <= FTW_MAX_BITMAP_BYTES;
}

EMSCRIPTEN_KEEPALIVE int ftw_init(void) {
  if (g_lib) return 1;
  return FT_Init_FreeType(&g_lib) == 0 ? 1 : 0;
}

/* Takes a COPY of the font bytes: emscripten's heap can move underneath a JS buffer,
 * and the faces live across many calls. Returns the face index, or -1. */
EMSCRIPTEN_KEEPALIVE int ftw_add_face(const unsigned char* data, unsigned int len) {
  if (!g_lib && !ftw_init()) return -1;
  if (g_face_count >= FTW_MAX_FACES || !data || !len) return -1;
  unsigned char* copy = (unsigned char*)malloc(len);
  if (!copy) return -1;
  memcpy(copy, data, len);
  FT_Face face = NULL;
  if (FT_New_Memory_Face(g_lib, copy, (FT_Long)len, 0, &face) != 0) {
    free(copy);
    return -1;
  }
  g_face_data[g_face_count] = copy;
  g_faces[g_face_count] = face;
  return g_face_count++;
}

EMSCRIPTEN_KEEPALIVE void ftw_reset(void) {
  for (int i = 0; i < g_face_count; i++) {
    if (g_faces[i]) FT_Done_Face(g_faces[i]);
    free(g_face_data[i]);
    g_faces[i] = NULL;
    g_face_data[i] = NULL;
  }
  g_face_count = 0;
  free(g_rows);
  g_rows = NULL;
  g_rows_cap = 0;
}

EMSCRIPTEN_KEEPALIVE int ftw_set_char_size(int idx, int size26_6, int dpi) {
  if (idx < 0 || idx >= g_face_count) return 0;
  return FT_Set_Char_Size(g_faces[idx], size26_6, size26_6, dpi, dpi) == 0 ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE unsigned int ftw_char_index(int idx, unsigned int cp) {
  if (idx < 0 || idx >= g_face_count) return 0;
  return (unsigned int)FT_Get_Char_Index(g_faces[idx], (FT_ULong)cp);
}

/* wght axis presence + range out through min/max (integers), for the JS weight logic. */
EMSCRIPTEN_KEEPALIVE int ftw_wght_range(int idx, int* out_min, int* out_max) {
  if (idx < 0 || idx >= g_face_count) return 0;
  FT_MM_Var* mm = NULL;
  if (FT_Get_MM_Var(g_faces[idx], &mm) != 0 || !mm) return 0;
  int found = 0;
  for (FT_UInt i = 0; i < mm->num_axis; i++) {
    if (mm->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
      /* 16.16 fixed point -> integer requested weight */
      *out_min = (int)(mm->axis[i].minimum >> 16);
      *out_max = (int)(mm->axis[i].maximum >> 16);
      found = 1;
      break;
    }
  }
  FT_Done_MM_Var(g_lib, mm);
  return found;
}

/* Sets the wght axis, leaving every other axis alone.
 *
 * FT_Set_Var_Design_Coordinates takes one coordinate PER AXIS IN FONT ORDER, so passing a
 * single value sets axis 0 - whatever that is. On a font whose axes are (opsz, wght) that
 * silently sets the optical size and leaves the weight at its default: measured on NewYork
 * at @700, only 2.33% of glyphs matched the reference (96% had different advances - i.e.
 * the output was not at 700 at all), while @400 - the axis default, where nothing needs
 * setting - matched 97.5%. Read the current coordinates, replace the wght entry, write all
 * of them back. */
EMSCRIPTEN_KEEPALIVE int ftw_set_wght(int idx, int wght) {
  if (idx < 0 || idx >= g_face_count) return 0;
  FT_MM_Var* mm = NULL;
  if (FT_Get_MM_Var(g_faces[idx], &mm) != 0 || !mm) return 0;
  const FT_UInt num = mm->num_axis;
  FT_Fixed* coords = (FT_Fixed*)calloc(num ? num : 1, sizeof(FT_Fixed));
  int rc = 1;
  if (coords) {
    if (FT_Get_Var_Design_Coordinates(g_faces[idx], num, coords) == 0) {
      for (FT_UInt i = 0; i < num; i++) {
        if (mm->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
          FT_Fixed lo = mm->axis[i].minimum, hi = mm->axis[i].maximum;
          FT_Fixed v = (FT_Fixed)((long)wght << 16);
          if (v < lo) v = lo;
          if (v > hi) v = hi;
          coords[i] = v;
          break;
        }
      }
      rc = FT_Set_Var_Design_Coordinates(g_faces[idx], num, coords) == 0 ? 1 : 0;
    } else {
      rc = 0;
    }
    free(coords);
  } else {
    rc = 0;
  }
  FT_Done_MM_Var(g_lib, mm);
  return rc;
}

EMSCRIPTEN_KEEPALIVE int ftw_load_render(int idx, unsigned int gi) {
  if (idx < 0 || idx >= g_face_count) return 0;
  if (FT_Load_Glyph(g_faces[idx], (FT_UInt)gi, FT_LOAD_RENDER) != 0) return 0;
  return bitmap_within_limit(&g_faces[idx]->glyph->bitmap);
}

EMSCRIPTEN_KEEPALIVE int ftw_load_outline(int idx, unsigned int gi) {
  if (idx < 0 || idx >= g_face_count) return 0;
  return FT_Load_Glyph(g_faces[idx], (FT_UInt)gi, FT_LOAD_NO_BITMAP) == 0 ? 1 : 0;
}

/* Mirrors embolden_glyph(): x and y get the SAME strength, then a normal re-render.
 * The Python declares FT_Outline_Embolden with a 3-argument prototype and passes the
 * strength twice; in C the 2-argument FT_Outline_Embolden would just ignore the extra
 * argument, so the effective call is (outline, strength) - identical to
 * FT_Outline_EmboldenXY(strength, strength) used here. Same value on both axes either
 * way, which is why the two are interchangeable; do not "simplify" this to a different
 * pair of strengths. */
/* Emboldens the loaded outline and re-renders, mirroring the Python tool - INCLUDING the
 * outline-less case: a space (U+0020, in every font) has n_points == 0 and FreeType answers
 * FT_Err_Invalid_Argument. The Python used to raise there and abort the whole conversion
 * (tools/ttf_to_epdfont_fast.py had the same gap; fixed in both places), so render plainly
 * and report it as handled. Returns 1 = emboldened, 2 = nothing to embolden (rendered),
 * 0 = real failure. The caller must NOT skip the glyph on 2: the interval offsets in the
 * .epdfont assume one glyph record per codepoint in a validated interval. */
EMSCRIPTEN_KEEPALIVE int ftw_embolden(int idx, int strength26_6) {
  if (idx < 0 || idx >= g_face_count) return 0;
  FT_GlyphSlot slot = g_faces[idx]->glyph;
  const int rc = FT_Outline_EmboldenXY(&slot->outline, strength26_6, strength26_6);
  if (rc == 6) { /* FT_Err_Invalid_Argument: outline too degenerate to thicken.
                  * Space has n_points == 0; U+2000-style glyphs have a single point and
                  * no contour. The Python tool used to raise here and abort the whole
                  * conversion; both sides now render plainly instead (same rule in
                  * tools/ttf_to_epdfont_fast.py). */
    if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) return 0;
    return bitmap_within_limit(&slot->bitmap) ? 2 : 0;
  }
  if (rc != 0) return 0;
  if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) return 0;
  return bitmap_within_limit(&slot->bitmap);
}

EMSCRIPTEN_KEEPALIVE unsigned int ftw_bm_width(int idx) {
  return (idx >= 0 && idx < g_face_count) ? g_faces[idx]->glyph->bitmap.width : 0;
}
EMSCRIPTEN_KEEPALIVE unsigned int ftw_bm_rows(int idx) {
  return (idx >= 0 && idx < g_face_count) ? g_faces[idx]->glyph->bitmap.rows : 0;
}
EMSCRIPTEN_KEEPALIVE int ftw_bm_left(int idx) {
  return (idx >= 0 && idx < g_face_count) ? g_faces[idx]->glyph->bitmap_left : 0;
}
EMSCRIPTEN_KEEPALIVE int ftw_bm_top(int idx) {
  return (idx >= 0 && idx < g_face_count) ? g_faces[idx]->glyph->bitmap_top : 0;
}
EMSCRIPTEN_KEEPALIVE int ftw_advance_x(int idx) {
  return (idx >= 0 && idx < g_face_count) ? g_faces[idx]->glyph->advance.x : 0;
}
EMSCRIPTEN_KEEPALIVE int ftw_size_height(int idx) {
  return (idx >= 0 && idx < g_face_count) ? (int)g_faces[idx]->size->metrics.height : 0;
}
EMSCRIPTEN_KEEPALIVE int ftw_size_ascender(int idx) {
  return (idx >= 0 && idx < g_face_count) ? (int)g_faces[idx]->size->metrics.ascender : 0;
}
EMSCRIPTEN_KEEPALIVE int ftw_size_descender(int idx) {
  return (idx >= 0 && idx < g_face_count) ? (int)g_faces[idx]->size->metrics.descender : 0;
}

/* Contiguous copy of the current glyph bitmap: w*h bytes, one byte per pixel. The Python
 * packer walks the raw buffer flat, so pitch padding must not survive. Null when empty. */
EMSCRIPTEN_KEEPALIVE const unsigned char* ftw_bitmap(int idx) {
  if (idx < 0 || idx >= g_face_count) return NULL;
  FT_Bitmap* bm = &g_faces[idx]->glyph->bitmap;
  if (!bm->buffer || !bm->width || !bm->rows) return NULL;
  if (!bitmap_within_limit(bm)) return NULL;
  const size_t need = (size_t)bm->width * bm->rows;
  if (need > g_rows_cap) {
    unsigned char* grown = (unsigned char*)realloc(g_rows, need);
    if (!grown) return NULL;
    g_rows = grown;
    g_rows_cap = need;
  }
  const int pitch = bm->pitch;
  if (pitch == (int)bm->width) {
    memcpy(g_rows, bm->buffer, need); /* already tight */
  } else {
    const int step = pitch < 0 ? -pitch : pitch;
    for (unsigned int y = 0; y < bm->rows; y++) {
      const unsigned char* src = bm->buffer + (pitch < 0 ? (int)(bm->rows - 1 - y) : (int)y) * step;
      memcpy(g_rows + (size_t)y * bm->width, src, bm->width);
    }
  }
  return g_rows;
}

EMSCRIPTEN_KEEPALIVE int ftw_bitmap_is_gray(int idx) {
  if (idx < 0 || idx >= g_face_count) return 0;
  return g_faces[idx]->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE const char* ftw_version(void) {
  static char buf[64];
  FT_Int major = 0, minor = 0, patch = 0;
  ftw_init();  /* the version is a diagnostic: it must be right even if it is the first
                * call (the worker reports it before any face is loaded) */
  if (g_lib) FT_Library_Version(g_lib, &major, &minor, &patch);
  snprintf(buf, sizeof(buf), "FreeType %d.%d.%d", major, minor, patch);
  return buf;
}
