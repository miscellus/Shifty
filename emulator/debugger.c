#ifdef DEBUGGER_C_ALREADY_DEFINED
#error "DEBUGGER_C_ALREADY_DEFINED"
#else
#define DEBUGGER_C_ALREADY_DEFINED
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "dejavu_sans_mono_ttf.h"

#define MAX_FILES 64

typedef struct {
    char *path;
    char *data;         /* Owned file bytes */
    size_t size;
} SourceFileBuffer;

typedef struct {
    int32_t file_index; /* Index into files[0 .. file_count - 1] */
    int32_t start;      /* Byte offset of line start (inclusive) */
    int32_t end;        /* Byte offset of line end (exclusive, before '\r'/'\n') */
    bool has_breakpoint;
} SourceLine;

typedef struct {
    SourceFileBuffer files[MAX_FILES];
    size_t file_count;

    SourceLine *lines;
    size_t line_count;
    size_t line_capacity;
    size_t max_line_len;

    int scroll_line;
    int scroll_col;
} SourceViewModel;

#define ATLAS_FIRST_CHAR 32
#define ATLAS_CHAR_COUNT 95 /* ASCII 32 (' ') through 126 ('~') */

typedef struct {
    GLuint texture;
    int width;
    int height;
    float pixel_height;
    float glyph_w;      /* Monospace cell width in pixels */
    float line_h;       /* Monospace line height in pixels */
    stbtt_bakedchar glyphs[ATLAS_CHAR_COUNT];
} FontAtlas;


typedef struct {
    float x, y, w, h;       /* Screen top-left and size in pixels */
    float u0, v0, u1, v1;   /* Atlas UVs */
    float r, g, b, a;       /* RGBA in [0.0, 1.0] */
} GlyphInstance;

typedef struct {
    GLuint program;
    GLuint vao;
    GLuint quad_vbo;
    GLuint instance_vbo;
    size_t max_instances;

    GLint u_viewport_size;
    GLint u_font_atlas;
} GlyphRenderer;

static bool font_atlas_bake_ascii(
    FontAtlas *atlas,
    const uint8_t *ttf_data,
    float pixel_height,
    int atlas_w,
    int atlas_h)
{
    memset(atlas, 0, sizeof(*atlas));
    atlas->width = atlas_w;
    atlas->height = atlas_h;
    atlas->pixel_height = pixel_height;

    uint8_t *pixels = (uint8_t *)malloc((size_t)atlas_w * (size_t)atlas_h);
    if (!pixels) {
        SDL_Log("Failed to allocate %dx%d font atlas buffer", atlas_w, atlas_h);
        return false;
    }

    /* Bake ASCII 32..126 into single-channel alpha/coverage bitmap */
    int bake_res = stbtt_BakeFontBitmap(
        ttf_data,
        0,                  /* Font index (0 for standard TTF) */
        pixel_height,
        pixels,
        atlas_w,
        atlas_h,
        ATLAS_FIRST_CHAR,
        ATLAS_CHAR_COUNT,
        atlas->glyphs
    );

    if (bake_res <= 0) {
        SDL_Log("stbtt_BakeFontBitmap failed or ran out of space (res=%d)", bake_res);
        free(pixels);
        return false;
    }

    /* Derive monospace cell width and line height from baked glyphs */
    float max_xadvance = 0.0f;
    float max_h = 0.0f;
    for (int i = 0; i < ATLAS_CHAR_COUNT; ++i) {
        const stbtt_bakedchar *g = &atlas->glyphs[i];
        if (g->xadvance > max_xadvance) {
            max_xadvance = g->xadvance;
        }
        float h = (float)(g->y1 - g->y0);
        if (h > max_h) {
            max_h = h;
        }
    }

    atlas->glyph_w = (max_xadvance > 0.0f) ? max_xadvance : (pixel_height * 0.6f);
    atlas->line_h  = (max_h > pixel_height) ? (max_h + 4.0f) : (pixel_height * 1.25f);

    /* Upload single-channel 8-bit coverage to GLES 3.0 GL_R8 texture */
    glGenTextures(1, &atlas->texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, atlas->texture);

    /* Critical for 1-byte-per-pixel widths that aren't multiples of 4 */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_R8,
        atlas_w,
        atlas_h,
        0,
        GL_RED,
        GL_UNSIGNED_BYTE,
        pixels
    );

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4); /* Restore default alignment */

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);
    free(pixels);
    return true;
}

uint8_t *read_entire_file(const char *, size_t *);

static bool debugger_load_font(FontAtlas *atlas, float pixel_height, int atlas_w, int atlas_h) {
    bool ok = font_atlas_bake_ascii(atlas, dejavu_sans_mono_ttf, pixel_height, atlas_w, atlas_h);
    return ok;
}

static float push_text_line(const FontAtlas *atlas,
                            GlyphInstance *out_instances,
                            size_t *out_count,
                            size_t max_instances,
                            const char *text,
                            float start_x,
                            float row_top_y,
                            float max_x,
                            float r, float g, float b, float a) {
    float cursor_x = start_x;
    float baseline_y = row_top_y + atlas->pixel_height;

    for (const char *p = text; *p != '\0'; ++p) {
        /* Stop before clipping past the right edge of the viewer */
        if (cursor_x + atlas->glyph_w > max_x) {
            break;
        }

        unsigned char uc = (unsigned char)*p;
        if (uc >= ATLAS_FIRST_CHAR && uc < ATLAS_FIRST_CHAR + ATLAS_CHAR_COUNT) {
            if (*out_count >= max_instances) {
                break;
            }

            const stbtt_bakedchar *bc = &atlas->glyphs[uc - ATLAS_FIRST_CHAR];
            float gw = (float)(bc->x1 - bc->x0);
            float gh = (float)(bc->y1 - bc->y0);

            if (gw > 0.0f && gh > 0.0f) {
                GlyphInstance *inst = &out_instances[(*out_count)++];
                inst->x  = cursor_x + bc->xoff;
                inst->y  = baseline_y + bc->yoff;
                inst->w  = gw;
                inst->h  = gh;
                inst->u0 = (float)bc->x0 / (float)atlas->width;
                inst->v0 = (float)bc->y0 / (float)atlas->height;
                inst->u1 = (float)bc->x1 / (float)atlas->width;
                inst->v1 = (float)bc->y1 / (float)atlas->height;
                inst->r  = r;
                inst->g  = g;
                inst->b  = b;
                inst->a  = a;
            }
        }

        cursor_x += atlas->glyph_w;
    }

    return cursor_x;
}

static const char *glyph_vs_src =
    "#version 300 es\n"
    "precision highp float;\n"
    "\n"
    "layout(location = 0) in vec2 a_unit_pos;   /* (0,0) to (1,1) */\n"
    "layout(location = 1) in vec4 a_inst_rect;   /* x, y (top-left px), w, h (px) */\n"
    "layout(location = 2) in vec4 a_inst_uv;     /* u0, v0, u1, v1 */\n"
    "layout(location = 3) in vec4 a_inst_color;  /* normalized RGBA */\n"
    "\n"
    "uniform vec2 u_viewport_size;               /* (view_w, view_h) in pixels */\n"
    "\n"
    "out vec2 v_uv;\n"
    "out vec4 v_color;\n"
    "\n"
    "void main() {\n"
    "    vec2 px = a_inst_rect.xy + a_unit_pos * a_inst_rect.zw;\n"
    "    vec2 ndc;\n"
    "    ndc.x = (px.x / u_viewport_size.x) * 2.0 - 1.0;\n"
    "    ndc.y = 1.0 - (px.y / u_viewport_size.y) * 2.0;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "\n"
    "    v_uv = mix(a_inst_uv.xy, a_inst_uv.zw, a_unit_pos);\n"
    "    v_color = a_inst_color;\n"
    "}\n";

static const char *glyph_fs_src =
    "#version 300 es\n"
    "precision mediump float;\n"
    "\n"
    "uniform sampler2D u_font_atlas;\n"
    "\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "\n"
    "out vec4 out_color;\n"
    "\n"
    "void main() {\n"
    "    /* Assumes single-channel GL_R8 atlas; use .a if RGBA atlas */\n"
    "    float coverage = texture(u_font_atlas, v_uv).r;\n"
    "    out_color = vec4(v_color.rgb, v_color.a * coverage);\n"
    "}\n";

static bool glyph_renderer_init(GlyphRenderer *gr, size_t max_instances) {
    memset(gr, 0, sizeof(*gr));
    gr->max_instances = max_instances;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, glyph_vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, glyph_fs_src);
    if (!vs || !fs) return false;

    gr->program = glCreateProgram();
    glAttachShader(gr->program, vs);
    glAttachShader(gr->program, fs);
    glLinkProgram(gr->program);

    GLint linked = 0;
    glGetProgramiv(gr->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        SDL_Log("Glyph shader program link failed");
        return false;
    }

    gr->u_viewport_size = glGetUniformLocation(gr->program, "u_viewport_size");
    gr->u_font_atlas    = glGetUniformLocation(gr->program, "u_font_atlas");

    /* Two triangles forming a [0, 1] x [0, 1] unit quad */
    static const float unit_quad[12] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,

        0.0f, 1.0f,
        1.0f, 0.0f,
        1.0f, 1.0f
    };

    glGenVertexArrays(1, &gr->vao);
    glGenBuffers(1, &gr->quad_vbo);
    glGenBuffers(1, &gr->instance_vbo);

    glBindVertexArray(gr->vao);

    /* Location 0: per-vertex unit quad */
    glBindBuffer(GL_ARRAY_BUFFER, gr->quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(unit_quad), unit_quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);

    /* Dynamic instance buffer */
    glBindBuffer(GL_ARRAY_BUFFER, gr->instance_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                  (GLsizeiptr)(max_instances * sizeof(GlyphInstance)),
                  NULL,
                  GL_DYNAMIC_DRAW);

    /* Location 1: a_inst_rect (vec4 at offset 0) */
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(GlyphInstance),
                           (void *)offsetof(GlyphInstance, x));
    glVertexAttribDivisor(1, 1);

    /* Location 2: a_inst_uv (vec4 at offset 16) */
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(GlyphInstance),
                           (void *)offsetof(GlyphInstance, u0));
    glVertexAttribDivisor(2, 1);

    /* Location 3: a_inst_color (vec4 at offset 32) */
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(GlyphInstance),
                           (void *)offsetof(GlyphInstance, r));
    glVertexAttribDivisor(3, 1);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    return true;
}

static void glyph_renderer_draw(
    GlyphRenderer *gr,
    const GlyphInstance *instances,
    size_t count,
    GLuint atlas_tex,
    float view_w,
    float view_h)
{
    if (count == 0) return;
    if (count > gr->max_instances) count = gr->max_instances;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(gr->program);
    glUniform2f(gr->u_viewport_size, view_w, view_h);

    glActiveTexture(GL_TEXTURE1); /* Keep GL_TEXTURE0 free for your LCD texture */
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glUniform1i(gr->u_font_atlas, 1);

    glBindVertexArray(gr->vao);
    glBindBuffer(GL_ARRAY_BUFFER, gr->instance_vbo);
    /* Orphan old buffer first to avoid driver stalls on dynamic streaming */
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(gr->max_instances * sizeof(GlyphInstance)), NULL, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(count * sizeof(GlyphInstance)), instances);

    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)count);

    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

#define DEBUGGER_MAX_INSTANCES 16384
#define DEBUGGER_MAX_FILES 64

typedef struct {
    uint32_t file_index;
    uint32_t line_start;
    uint32_t line_end;
    bool break_point;
} DbgLine;

typedef struct {
    DbgLine *items;
    size_t line_count;
    size_t line_capacity;
    size_t max_line_len;
} DbgLines;

typedef struct {
    const char *path;
    const uint8_t *data; /* Owned file bytes */
    size_t size;

    DbgLines lines;
} DbgFile;

typedef struct {
    FontAtlas font;
    GlyphRenderer renderer;
    GlyphInstance instances[DEBUGGER_MAX_INSTANCES];
    size_t instance_count;
    bool initialized;

    const char *src_dir; // Base path to search for source files from

    DbgFile files[DEBUGGER_MAX_FILES];
    uint32_t file_count;
    uint32_t file_index; // Currently viewed file

    uint32_t scroll_line;
    uint32_t scroll_col;
} Dbg;

static bool dbg_load_bin_debug_map(Dbg *dbg, Pc8201Machine *machine, const char *bin_path);

static bool dbg_init(Dbg *dbg, Pc8201Machine *mach, const char *debug_map_path) {
    memset(dbg, 0, sizeof(*dbg));

    if (!debugger_load_font(&dbg->font, 24.0f, 512, 512)) {
        return false;
    }
    if (!glyph_renderer_init(&dbg->renderer, DEBUGGER_MAX_INSTANCES)) {
        return false;
    }

    if (!dbg_load_bin_debug_map(dbg, mach, debug_map_path)) {
        return false;
    }

    dbg->initialized = true;
    return true;
}

static void dbg_lines_free(DbgLines *ls) {
    if (!ls) return;
    free(ls->items);
    ls->items = NULL;
    ls->line_count = 0;
    ls->line_capacity = 0;
    ls->max_line_len = 0;
}

static bool dbg_lines_push(DbgLines *ls,
                           uint32_t file_index,
                           uint32_t line_start,
                           uint32_t line_end) {
    if (line_end < line_start) line_end = line_start;

    if (ls->line_count == ls->line_capacity) {
        size_t new_cap = (ls->line_capacity == 0) ? 512 : ls->line_capacity * 2;
        DbgLine *grown = (DbgLine *)realloc(ls->items, new_cap * sizeof(DbgLine));
        if (!grown) return false;
        ls->items = grown;
        ls->line_capacity = new_cap;
    }

    ls->items[ls->line_count++] = (DbgLine){
        .file_index = file_index,
        .line_start = line_start,
        .line_end = line_end,
        .break_point = false,
    };

    size_t len = (size_t)(line_end - line_start);
    if (len > ls->max_line_len) {
        ls->max_line_len = len;
    }
    return true;
}

static int32_t dbg_add_file(Dbg *dbg, const char *path, const uint8_t *raw, uint32_t size) {
    if (!dbg || dbg->file_count >= DEBUGGER_MAX_FILES) {
        SDL_Log("Dbg reached DEBUGGER_MAX_FILES (%d); cannot load '%s'", DEBUGGER_MAX_FILES, path ? path : "(null)");
        return -1;
    }

    uint32_t file_idx = dbg->file_count++;
    DbgFile *f = &dbg->files[file_idx];
    memset(f, 0, sizeof(*f));
    f->path = path;
    f->data = raw;
    f->size = size;

    uint32_t start = 0;
    for (uint32_t i = 0; i < size; ++i) {
        if (f->data[i] != '\n') {
            continue;
        }

        uint32_t end = i;
        if (end > start && f->data[end - 1] == '\r') {
            end--;
        }
        if (!dbg_lines_push(&f->lines, file_idx, start, end)) {
            return -1;
        }

        start = i + 1;
    }

    if (start < size) {
        uint32_t end = size;
        if (end > start && f->data[end - 1] == '\r') {
            end--;
        }
        if (!dbg_lines_push(&f->lines, file_idx, start, end)) {
            return -1;
        }
    }

    return (int32_t)file_idx;
}

static void dbg_free(Dbg *dbg) {
    if (!dbg) return;
    for (uint32_t i = 0; i < dbg->file_count; ++i) {
        free((void *)dbg->files[i].path);
        dbg->files[i].path = NULL;
        dbg->files[i].data = NULL;
        dbg_lines_free(&dbg->files[i].lines);
    }
    dbg->src_dir = NULL;
    dbg->file_count = 0;
    dbg->file_index = 0;
    dbg->scroll_line = 0;
    dbg->scroll_col = 0;
}

static uint32_t dbg_clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void push_source_slice(
    const FontAtlas *atlas,
    GlyphInstance *out_instances,
    size_t *out_count,
    size_t max_instances,
    const char *text,
    size_t len,
    uint32_t scroll_col,
    float start_x,
    float row_top_y,
    float max_x,
    float r, float g, float b, float a) {
    if (!text || (size_t)scroll_col >= len) return;

    float cursor_x = start_x;
    float baseline_y = row_top_y + atlas->pixel_height;

    for (size_t i = (size_t)scroll_col; i < len; ++i) {
        /* No line wrapping: stop before exceeding right edge */
        if (cursor_x + atlas->glyph_w > max_x) {
            break;
        }

        unsigned char uc = (unsigned char)text[i];
        if (uc >= ATLAS_FIRST_CHAR && uc < ATLAS_FIRST_CHAR + ATLAS_CHAR_COUNT) {
            if (*out_count >= max_instances) {
                break;
            }

            const stbtt_bakedchar *bc = &atlas->glyphs[uc - ATLAS_FIRST_CHAR];
            float gw = (float)(bc->x1 - bc->x0);
            float gh = (float)(bc->y1 - bc->y0);

            if (gw > 0.0f && gh > 0.0f) {
                GlyphInstance *inst = &out_instances[(*out_count)++];
                inst->x  = cursor_x + bc->xoff;
                inst->y  = baseline_y + bc->yoff;
                inst->w  = gw;
                inst->h  = gh;
                inst->u0 = (float)bc->x0 / (float)atlas->width;
                inst->v0 = (float)bc->y0 / (float)atlas->height;
                inst->u1 = (float)bc->x1 / (float)atlas->width;
                inst->v1 = (float)bc->y1 / (float)atlas->height;
                inst->r  = r;
                inst->g  = g;
                inst->b  = b;
                inst->a  = a;
            }
        }

        cursor_x += atlas->glyph_w;
    }
}

static void dbg_clamp_scroll_file(Dbg *dbg, const DbgFile *file, float view_w, float view_h) {
    if (!dbg || !file || dbg->font.glyph_w <= 0.0f || dbg->font.line_h <= 0.0f) return;

    float text_x = dbg->font.glyph_w * 4.0f;
    float text_w = view_w - text_x - 8.0f;

    uint32_t visible_cols = (text_w > dbg->font.glyph_w)
        ? (uint32_t)(text_w / dbg->font.glyph_w) : 1u;
    uint32_t visible_rows = (view_h > dbg->font.line_h)
        ? (uint32_t)(view_h / dbg->font.line_h) : 1u;

    uint32_t max_line = (file->lines.line_count > visible_rows)
        ? (uint32_t)(file->lines.line_count - visible_rows) : 0u;
    dbg->scroll_line = dbg_clamp_u32(dbg->scroll_line, 0u, max_line);

    uint32_t max_col = (file->lines.max_line_len > visible_cols)
        ? (uint32_t)(file->lines.max_line_len - visible_cols) : 0u;
    dbg->scroll_col = dbg_clamp_u32(dbg->scroll_col, 0u, max_col);
}

static void dbg_draw_source(Dbg *dbg, float view_w, float view_h) {
    if (!dbg || !dbg->initialized || dbg->file_count == 0) return;

    /* For now, draw the first loaded file (index 0) */
    const DbgFile *file = &dbg->files[0];
    if (!file->data || file->size == 0 || file->lines.line_count == 0) return;

    dbg_clamp_scroll_file(dbg, file, view_w, view_h);
    dbg->instance_count = 0;

    uint32_t visible_rows = (dbg->font.line_h > 0.0f)
        ? (uint32_t)(view_h / dbg->font.line_h) : 1u;
    if (visible_rows < 1u) visible_rows = 1u;

    uint32_t end_line = dbg->scroll_line + visible_rows;
    if (end_line > (uint32_t)file->lines.line_count) {
        end_line = (uint32_t)file->lines.line_count;
    }

    float gutter_x = 8.0f;
    float text_x   = dbg->font.glyph_w * 4.0f;
    float max_x    = view_w - 8.0f;

    for (uint32_t row = 0; row < (end_line - dbg->scroll_line); ++row) {
        uint32_t line_idx = dbg->scroll_line + row;
        const DbgLine *line = &file->lines.items[line_idx];
        float row_y = (float)row * dbg->font.line_h;

        if (line->break_point) {
            push_text_line(&dbg->font, dbg->instances, &dbg->instance_count,
                           DEBUGGER_MAX_INSTANCES, "*",
                           gutter_x, row_y, text_x,
                           1.0f, 0.25f, 0.25f, 1.0f);
        }

        if ((size_t)line->line_start <= (size_t)line->line_end &&
            (size_t)line->line_end <= file->size) {
            size_t len = (size_t)(line->line_end - line->line_start);
            push_source_slice(&dbg->font, dbg->instances, &dbg->instance_count,
                              DEBUGGER_MAX_INSTANCES,
                              (const char *)file->data + line->line_start,
                              len,
                              dbg->scroll_col,
                              text_x, row_y, max_x,
                              0.90f, 0.92f, 0.95f, 1.0f);
        }
    }

    glyph_renderer_draw(&dbg->renderer,
                        dbg->instances,
                        dbg->instance_count,
                        dbg->font.texture,
                        view_w,
                        view_h);
}

static void dbg_handle_event(Dbg *dbg, const SDL_Event *event, FloatRect area) {
    if (!dbg || !dbg->initialized || dbg->file_count == 0) return;

    DbgFile *file = &dbg->files[0];
    if (!file->data || file->lines.line_count == 0) return;

    if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        float mx = event->wheel.mouse_x;
        float my = event->wheel.mouse_y;
        if (!PointInsideFloatRect(mx, my, area)) {
            return;
        }

        SDL_Keymod mods = SDL_GetModState();
        int v_delta = -(int)(event->wheel.y * 3.0f);
        int h_delta = -(int)(event->wheel.x * 6.0f);

        if (mods & SDL_KMOD_SHIFT) {
            h_delta = -(int)(event->wheel.y * 6.0f);
            v_delta = 0;
        }

        int next_line = (int)dbg->scroll_line + v_delta;
        int next_col  = (int)dbg->scroll_col + h_delta;
        dbg->scroll_line = (next_line < 0) ? 0u : (uint32_t)next_line;
        dbg->scroll_col  = (next_col < 0)  ? 0u : (uint32_t)next_col;
        dbg_clamp_scroll_file(dbg, file, area.w, area.h);
    }
    else if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
             event->button.button == SDL_BUTTON_LEFT) {
        float mx = event->button.x;
        float my = event->button.y;
        if (mx < area.x || mx > area.x + area.w ||
            my < area.y || my > area.y + area.h) {
            return;
        }

        if (dbg->font.line_h <= 0.0f) return;
        uint32_t clicked_row = (uint32_t)((my - area.y) / dbg->font.line_h);
        uint32_t line_idx = dbg->scroll_line + clicked_row;

        if (line_idx < (uint32_t)file->lines.line_count) {
            file->lines.items[line_idx].break_point = !file->lines.items[line_idx].break_point;
        }
    }
}



typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    bool ok;
} BinReader;

static uint8_t br_u8(BinReader *r) {
    if (!r->ok || r->p + 1 > r->end) { r->ok = false; return 0; }
    return *r->p++;
}

static uint16_t br_u16_le(BinReader *r) {
    uint8_t b0 = br_u8(r);
    uint8_t b1 = br_u8(r);
    return (uint16_t)b0 | ((uint16_t)b1 << 8);
}

static uint32_t br_u32_le(BinReader *r) {
    uint8_t b0 = br_u8(r);
    uint8_t b1 = br_u8(r);
    uint8_t b2 = br_u8(r);
    uint8_t b3 = br_u8(r);
    return (uint32_t)b0 |
           ((uint32_t)b1 << 8) |
           ((uint32_t)b2 << 16) |
           ((uint32_t)b3 << 24);
}

static const uint8_t *br_bytes(BinReader *r, size_t n) {
    if (!r->ok || n > (size_t)(r->end - r->p)) { r->ok = false; return NULL; }
    const uint8_t *ptr = r->p;
    r->p += n;
    return ptr;
}

static bool dbg_load_bin_debug_map(Dbg *dbg, Pc8201Machine *machine, const char *bin_path) {
    if (!dbg || !machine || !bin_path) return false;

    size_t raw_size = 0;
    uint8_t *raw = read_entire_file(bin_path, &raw_size);
    if (!raw) {
        SDL_Log("Failed to read binary debug map '%s'", bin_path);
        return false;
    }

    BinReader r = { .p = raw, .end = raw + raw_size, .ok = true };

    const uint8_t *magic = br_bytes(&r, 4);
    uint32_t version = br_u32_le(&r);
    uint32_t file_count = br_u32_le(&r);
    uint32_t mapping_count = br_u32_le(&r);
    uint32_t symbol_count = br_u32_le(&r);

    const uint32_t expected_format_version = 1;

    if (!r.ok || strncmp((char *)magic, "8085", 4) != 0 || version != expected_format_version) {
        SDL_Log("Invalid binary debug map header in '%s'", bin_path);
        free(raw);
        return false;
    }

    /* Section 1: Files */
    for (uint32_t i = 0; i < file_count; ++i) {
        uint32_t path_len = br_u32_le(&r);
        const uint8_t *path_bytes = br_bytes(&r, path_len);
        uint32_t content_len = br_u32_le(&r);
        const uint8_t *content_bytes = br_bytes(&r, content_len);
        if (!r.ok) break;

        char *path_copy = (char *)malloc((size_t)path_len + 1u);
        if (path_copy) {
            if (path_len > 0) memcpy(path_copy, path_bytes, path_len);
            path_copy[path_len] = '\0';
        }

        dbg_add_file(dbg, path_copy, content_bytes, content_len);
    }

    /* Section 2: Address-to-line mappings (0-based line_index) */
    for (uint32_t m = 0; m < mapping_count; ++m) {
        uint16_t addr = br_u16_le(&r);
        uint16_t file_id = br_u16_le(&r);
        uint16_t line_index = br_u16_le(&r);
        if (!r.ok) break;

        if ((uint32_t)file_id < dbg->file_count) {
            const DbgFile *f = &dbg->files[file_id];
            if (f->data && (size_t)line_index < f->lines.line_count) {
                DebuggerLoc loc = {0};
                loc.s.file_index = file_id;
                loc.s.line_number = line_index;
                machine->address_to_line[addr] = loc;
            }
        }
    }

    /* Section 3: Symbols (skip or store if needed) */
    for (uint32_t s = 0; s < symbol_count; ++s) {
        uint16_t name_len = br_u16_le(&r);
        (void)br_bytes(&r, name_len);
        (void)br_u16_le(&r);
        if (!r.ok) break;
    }

    if (!r.ok) {
        SDL_Log("Truncated binary debug map '%s'", bin_path);
        return false;
    }

    SDL_Log("Loaded binary debug map '%s' (%u files, %u mappings)",
            bin_path, dbg->file_count, mapping_count);
    return true;
}
