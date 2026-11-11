#define _GNU_SOURCE

// mpv-compare: two videos side-by-side in one window with a draggable wipe,
// plus a basic playback UI (progress bar, time, filenames, speed, loop,
// screenshots). libmpv keeps VAAPI decoding on the GPU (EGL dmabuf interop);
// a tiny shader composites both frames.

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

// ------------------------------------------------------------------ globals

typedef struct {
  mpv_handle* mpv;
  mpv_render_context* render;
  GLuint fbo;
  GLuint texture;
  int rendered;
  int has_frame;
  int width;
  int height;
  const char* path;
  const char* name;
} Side;

typedef struct {
  float r;
  float g;
  float b;
  float a;
} Color;

static const Color COLOR_WHITE = {0.96f, 0.96f, 0.96f, 1.0f};
static const Color COLOR_DIM = {0.75f, 0.75f, 0.75f, 1.0f};
static const Color COLOR_ACCENT = {0.25f, 0.70f, 1.00f, 1.0f};

static Display* x_display = NULL;
static Window x_window = 0;
static int window_width = 1280;
static int window_height = 720;
static Atom wm_delete_window = 0;
static Atom net_wm_name = 0;
static Atom net_wm_state = 0;
static Atom net_wm_state_fullscreen = 0;

static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLSurface egl_surface = EGL_NO_SURFACE;
static EGLContext egl_context = EGL_NO_CONTEXT;
static int gles3 = 0;

static Side sides[2];

// The render thread (main) only ever calls mpv_render_*; every other libmpv
// call happens on the control thread. See the thread rules in render.h: with
// MPV_RENDER_PARAM_ADVANCED_CONTROL, a render thread must not call other mpv
// APIs or a real deadlock can freeze the core.
typedef struct {
  int type;
  double value;
} Command;

enum {
  CMD_PAUSE = 1,
  CMD_SEEK_ABS,
  CMD_SEEK_REL,
  CMD_STEP_FORWARD,
  CMD_STEP_BACKWARD,
  CMD_SPEED,
  CMD_LOOP,
};

static int cmd_pipe[2] = {-1, -1};
static pthread_t control_thread;
static atomic_int control_running = 1;
static atomic_int renderers_freed = 0;
static atomic_int mpv_shutdown = 0;
static atomic_int update_pending_flag[2];
static atomic_int g_paused = 1;
static atomic_int g_hwdec[2];
static atomic_llong g_drops[2];
static atomic_int g_resync_count = 0;
static _Atomic double g_time[2];
static _Atomic double g_duration = 0.0;
static _Atomic double g_speed = 1.0;

static GLuint video_program = 0;
static GLuint video_vbo = 0;
static GLint loc_tex_left = -1;
static GLint loc_tex_right = -1;
static GLint loc_split = -1;
static GLint loc_line_width = -1;
static GLint attr_video_pos = 0;
static GLint attr_rect_pos = 0;
static GLint attr_rect_color = 1;
static GLint attr_text_pos = 0;
static GLint attr_text_uv = 1;
static GLint attr_text_color = 2;

static GLuint rect_program = 0;
static GLuint rect_vbo = 0;
static GLfloat* rect_vertices = NULL;
static size_t rect_vertex_count = 0;
static size_t rect_capacity = 0;

static GLuint text_program = 0;
static GLuint text_vbo = 0;
static GLint loc_text_atlas = -1;
static GLfloat* text_vertices = NULL;
static size_t text_vertex_count = 0;
static size_t text_capacity = 0;

static GLuint font_texture = 0;
static stbtt_fontinfo font_info;
static unsigned char* font_data = NULL;
static int font_pixel_size = 20;
static float ui_scale = 1.0f;
static float font_scale = 1.0f;
static int font_ascent = 0;
static int font_descent = 0;
static int font_line_gap = 0;

typedef struct {
  uint32_t codepoint;
  int used;
  int x0;
  int y0;
  int x1;
  int y1;
  float advance;
  int atlas_x;
  int atlas_y;
  int atlas_w;
  int atlas_h;
} Glyph;

#define MAX_GLYPHS 2048
static Glyph glyphs[MAX_GLYPHS];
static int glyph_count = 0;
static int atlas_cursor_x = 0;
static int atlas_cursor_y = 0;
static int atlas_row_height = 0;
static const int ATLAS_SIZE = 1024;

static const char* hwdec_mode = "auto-safe";
static float split = 0.5f;
static int dragging_split = 0;
static int dragging_progress = 0;
static int paused = 1;
static volatile int quit = 0;
static int dirty = 1;
static int force_render = 0;
static int wake_pipe[2] = {-1, -1};

static double position = 0.0;
static double duration = 0.0;
static double speed = 1.0;
static int loop_enabled = 0;
static double ui_refresh_next = 0.0;
static double position_override_until = 0.0;

static char toast[256] = {0};
static double toast_until = 0.0;

static int show_help = 0;
static double help_toggled_at = 0.0;

static const char* dump_path = NULL;
static double dump_at = -1.0;
static int dumped = 0;
static int dump_requested = 0;
static int screenshot_requested = 0;
static int screenshot_captured = 0;
static unsigned char* screenshot_pixels = NULL;
static int screenshot_width = 0;
static int screenshot_height = 0;
static double progress_preview = 0.0;
static double last_seek_issue = 0.0;
static int stats_enabled = 0;
static double stats_next = 0.0;
static unsigned long presents = 0;
static unsigned long presents_last = 0;

// ------------------------------------------------------------------ helpers

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void set_toast(const char* message) {
  snprintf(toast, sizeof(toast), "%s", message);
  toast_until = now_seconds() + 1.6;
  dirty = 1;
}

static void wake_main(void) {
  if (wake_pipe[1] >= 0) {
    char byte = 1;
    ssize_t ignored = write(wake_pipe[1], &byte, 1);
    (void)ignored;
  }
}

static void on_mpv_render_update(void* ctx) {
  Side* side = (Side*)ctx;
  const int index = side == &sides[0] ? 0 : 1;
  atomic_store_explicit(&update_pending_flag[index], 1, memory_order_relaxed);
  wake_main();
}

static void* gl_get_proc_address(void* ctx, const char* name) {
  (void)ctx;
  void* symbol = (void*)eglGetProcAddress(name);
  if (symbol == NULL) {
    symbol = dlsym(RTLD_DEFAULT, name);
  }
  return symbol;
}

static void die(const char* message) {
  fprintf(stderr, "mpv-compare: %s\n", message);
  exit(1);
}

static void check_mpv(int status, const char* what) {
  if (status < 0) {
    fprintf(stderr, "mpv-compare: %s failed: %s\n", what, mpv_error_string(status));
    exit(1);
  }
}

static const char* base_name(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash != NULL ? slash + 1 : path;
}

// ---------------------------------------------------------------- X11 / EGL

static void create_window(void) {
  x_display = XOpenDisplay(NULL);
  if (x_display == NULL) {
    die("cannot open X display");
  }

  const int screen = DefaultScreen(x_display);
  x_window = XCreateSimpleWindow(x_display, RootWindow(x_display, screen), 0, 0, (unsigned)window_width, (unsigned)window_height, 0, BlackPixel(x_display, screen),
                                 BlackPixel(x_display, screen));
  XSelectInput(x_display, x_window, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);

  wm_delete_window = XInternAtom(x_display, "WM_DELETE_WINDOW", False);
  net_wm_name = XInternAtom(x_display, "_NET_WM_NAME", False);
  net_wm_state = XInternAtom(x_display, "_NET_WM_STATE", False);
  net_wm_state_fullscreen = XInternAtom(x_display, "_NET_WM_STATE_FULLSCREEN", False);
  XSetWMProtocols(x_display, x_window, &wm_delete_window, 1);

  XMapWindow(x_display, x_window);
  XFlush(x_display);
}

static void set_window_title(const char* title) {
  if (x_display == NULL || x_window == 0) {
    return;
  }
  XStoreName(x_display, x_window, title);
  XChangeProperty(x_display, x_window, net_wm_name, XInternAtom(x_display, "UTF8_STRING", False), 8, PropModeReplace, (const unsigned char*)title, (int)strlen(title));
  XFlush(x_display);
}

static void toggle_fullscreen(void) {
  XEvent event;
  memset(&event, 0, sizeof(event));
  event.type = ClientMessage;
  event.xclient.window = x_window;
  event.xclient.message_type = net_wm_state;
  event.xclient.format = 32;
  event.xclient.data.l[0] = 2;  // _NET_WM_STATE_TOGGLE
  event.xclient.data.l[1] = (long)net_wm_state_fullscreen;
  event.xclient.data.l[2] = 0;
  event.xclient.data.l[3] = 1;
  XSendEvent(x_display, DefaultRootWindow(x_display), False, SubstructureNotifyMask | SubstructureRedirectMask, &event);
  XFlush(x_display);
}

static void create_egl(void) {
  egl_display = eglGetDisplay((EGLNativeDisplayType)x_display);
  if (egl_display == EGL_NO_DISPLAY) {
    die("eglGetDisplay failed");
  }

  EGLint major = 0;
  EGLint minor = 0;
  if (!eglInitialize(egl_display, &major, &minor)) {
    die("eglInitialize failed");
  }
  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    die("eglBindAPI(EGL_OPENGL_ES_API) failed");
  }

  const EGLint config_attribs[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_NONE,
  };

  EGLConfig config = NULL;
  EGLint config_count = 0;
  if (!eglChooseConfig(egl_display, config_attribs, &config, 1, &config_count) || config_count < 1) {
    const EGLint fallback_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_NONE,
    };
    if (!eglChooseConfig(egl_display, fallback_attribs, &config, 1, &config_count) || config_count < 1) {
      die("eglChooseConfig failed");
    }
  }

  egl_surface = eglCreateWindowSurface(egl_display, config, (EGLNativeWindowType)x_window, NULL);
  if (egl_surface == EGL_NO_SURFACE) {
    die("eglCreateWindowSurface failed");
  }

  const EGLint context_attribs3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attribs3);
  gles3 = egl_context != EGL_NO_CONTEXT;
  if (egl_context == EGL_NO_CONTEXT) {
    const EGLint context_attribs2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attribs2);
  }
  if (egl_context == EGL_NO_CONTEXT) {
    die("eglCreateContext failed");
  }
  if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
    die("eglMakeCurrent failed");
  }

  printf("GL: %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
}

// ---------------------------------------------------------------- GL basics

static GLuint compile_shader(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024] = {0};
    glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    fprintf(stderr, "shader compile failed: %s\n", log);
    exit(1);
  }
  return shader;
}

static GLuint link_program(const char* vs_source, const char* fs_source) {
  GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_source);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_source);
  GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[1024] = {0};
    glGetProgramInfoLog(program, sizeof(log), NULL, log);
    fprintf(stderr, "program link failed: %s\n", log);
    exit(1);
  }
  glDeleteShader(vs);
  glDeleteShader(fs);
  return program;
}

static GLuint create_stream_vbo(void) {
  GLuint vbo = 0;
  glGenBuffers(1, &vbo);
  return vbo;
}

static void create_video_pipeline(void) {
  static const char* vs =
      "attribute vec2 a_pos;\n"
      "varying vec2 v_uv;\n"
      "void main() {\n"
      "  v_uv = a_pos * 0.5 + 0.5;\n"
      "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
      "}\n";
  static const char* fs =
      "precision mediump float;\n"
      "varying vec2 v_uv;\n"
      "uniform sampler2D tex_left;\n"
      "uniform sampler2D tex_right;\n"
      "uniform float split;\n"
      "uniform float line_width;\n"
      "void main() {\n"
      "  vec3 color = v_uv.x < split ? texture2D(tex_left, v_uv).rgb : texture2D(tex_right, v_uv).rgb;\n"
      "  if (abs(v_uv.x - split) < line_width) color = vec3(1.0, 1.0, 1.0);\n"
      "  gl_FragColor = vec4(color, 1.0);\n"
      "}\n";

  video_program = link_program(vs, fs);
  loc_tex_left = glGetUniformLocation(video_program, "tex_left");
  loc_tex_right = glGetUniformLocation(video_program, "tex_right");
  loc_split = glGetUniformLocation(video_program, "split");
  loc_line_width = glGetUniformLocation(video_program, "line_width");
  attr_video_pos = glGetAttribLocation(video_program, "a_pos");

  static const GLfloat vertices[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
  video_vbo = create_stream_vbo();
  glBindBuffer(GL_ARRAY_BUFFER, video_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void create_rect_pipeline(void) {
  static const char* vs =
      "attribute vec2 a_pos;\n"
      "attribute vec4 a_color;\n"
      "varying vec4 v_color;\n"
      "void main() {\n"
      "  v_color = a_color;\n"
      "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
      "}\n";
  static const char* fs =
      "precision mediump float;\n"
      "varying vec4 v_color;\n"
      "void main() { gl_FragColor = v_color; }\n";

  rect_program = link_program(vs, fs);
  attr_rect_pos = glGetAttribLocation(rect_program, "a_pos");
  attr_rect_color = glGetAttribLocation(rect_program, "a_color");
  rect_vbo = create_stream_vbo();
}

static void create_text_pipeline(void) {
  static const char* vs =
      "attribute vec2 a_pos;\n"
      "attribute vec2 a_uv;\n"
      "attribute vec4 a_color;\n"
      "varying vec2 v_uv;\n"
      "varying vec4 v_color;\n"
      "void main() {\n"
      "  v_uv = a_uv;\n"
      "  v_color = a_color;\n"
      "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
      "}\n";
  static const char* fs =
      "precision mediump float;\n"
      "varying vec2 v_uv;\n"
      "varying vec4 v_color;\n"
      "uniform sampler2D atlas;\n"
      "void main() {\n"
      "  float coverage = texture2D(atlas, v_uv).a;\n"
      "  gl_FragColor = vec4(v_color.rgb, v_color.a * coverage);\n"
      "}\n";

  text_program = link_program(vs, fs);
  loc_text_atlas = glGetUniformLocation(text_program, "atlas");
  attr_text_pos = glGetAttribLocation(text_program, "a_pos");
  attr_text_uv = glGetAttribLocation(text_program, "a_uv");
  attr_text_color = glGetAttribLocation(text_program, "a_color");
  text_vbo = create_stream_vbo();
}

// ---------------------------------------------------------------- font / text

static void load_font(void) {
  const char* candidates[12];
  int candidate_count = 0;
  const char* env_font = getenv("MPVC_FONT");
  if (env_font != NULL) {
    candidates[candidate_count++] = env_font;
  }
  candidates[candidate_count++] = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
  candidates[candidate_count++] = "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc";
  candidates[candidate_count++] = "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc";
  candidates[candidate_count++] = "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc";
  candidates[candidate_count++] = "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf";
  candidates[candidate_count++] = "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf";
  candidates[candidate_count++] = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
  candidates[candidate_count] = NULL;

  long font_size = 0;
  for (int i = 0; candidates[i] != NULL; i++) {
    if (candidates[i] == NULL) {
      continue;
    }
    FILE* file = fopen(candidates[i], "rb");
    if (file == NULL) {
      fprintf(stderr, "[font] cannot open %s\n", candidates[i]);
      continue;
    }
    fseek(file, 0, SEEK_END);
    font_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (font_size <= 0) {
      fclose(file);
      continue;
    }
    font_data = malloc((size_t)font_size);
    if (font_data == NULL || fread(font_data, 1, (size_t)font_size, file) != (size_t)font_size) {
      free(font_data);
      font_data = NULL;
      fclose(file);
      continue;
    }
    fclose(file);

    int offset = stbtt_GetFontOffsetForIndex(font_data, 0);
    const int init_ok = offset >= 0 ? stbtt_InitFont(&font_info, font_data, offset) : 0;
    fprintf(stderr, "[font] %s size=%ld offset=%d init=%d\n", candidates[i], font_size, offset, init_ok);
    if (init_ok) {
      printf("font: %s\n", candidates[i]);
      break;
    }
    free(font_data);
    font_data = NULL;
  }

  if (font_data == NULL) {
    fprintf(stderr, "mpv-compare: no usable font found; UI text disabled\n");
    return;
  }

  font_scale = stbtt_ScaleForPixelHeight(&font_info, (float)font_pixel_size);
  stbtt_GetFontVMetrics(&font_info, &font_ascent, &font_descent, &font_line_gap);

  glGenTextures(1, &font_texture);
  glBindTexture(GL_TEXTURE_2D, font_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, gles3 ? GL_RGBA8 : GL_RGBA, ATLAS_SIZE, ATLAS_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

  unsigned char zero[4] = {0, 0, 0, 0};
  (void)zero;
  glBindTexture(GL_TEXTURE_2D, 0);
}

static Glyph* glyph_for(uint32_t codepoint) {
  for (int i = 0; i < glyph_count; i++) {
    if (glyphs[i].codepoint == codepoint) {
      return &glyphs[i];
    }
  }
  if (font_data == NULL || glyph_count >= MAX_GLYPHS) {
    return NULL;
  }

  int advance = 0;
  int lsb = 0;
  stbtt_GetCodepointHMetrics(&font_info, (int)codepoint, &advance, &lsb);

  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  stbtt_GetCodepointBitmapBox(&font_info, (int)codepoint, font_scale, font_scale, &x0, &y0, &x1, &y1);
  int width = x1 - x0;
  int height = y1 - y0;

  Glyph* glyph = &glyphs[glyph_count++];
  memset(glyph, 0, sizeof(*glyph));
  glyph->codepoint = codepoint;
  glyph->used = 1;
  glyph->x0 = x0;
  glyph->y0 = y0;
  glyph->x1 = x1;
  glyph->y1 = y1;
  glyph->advance = advance * font_scale;

  if (width <= 0 || height <= 0) {
    return glyph;
  }

  if (atlas_cursor_x + width > ATLAS_SIZE) {
    atlas_cursor_x = 0;
    atlas_cursor_y += atlas_row_height + 1;
    atlas_row_height = 0;
  }
  if (atlas_cursor_y + height > ATLAS_SIZE) {
    fprintf(stderr, "mpv-compare: glyph atlas full\n");
    return glyph;
  }

  unsigned char* bitmap = malloc((size_t)width * (size_t)height);
  if (bitmap == NULL) {
    return glyph;
  }
  stbtt_MakeCodepointBitmap(&font_info, bitmap, width, height, width, font_scale, font_scale, (int)codepoint);

  unsigned char* rgba = malloc((size_t)width * (size_t)height * 4);
  if (rgba == NULL) {
    free(bitmap);
    return glyph;
  }
  for (int i = 0; i < width * height; i++) {
    rgba[i * 4 + 0] = 255;
    rgba[i * 4 + 1] = 255;
    rgba[i * 4 + 2] = 255;
    rgba[i * 4 + 3] = bitmap[i];
  }

  glBindTexture(GL_TEXTURE_2D, font_texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, atlas_cursor_x, atlas_cursor_y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  glBindTexture(GL_TEXTURE_2D, 0);
  free(bitmap);
  free(rgba);

  glyph->atlas_x = atlas_cursor_x;
  glyph->atlas_y = atlas_cursor_y;
  glyph->atlas_w = width;
  glyph->atlas_h = height;

  atlas_cursor_x += width + 1;
  atlas_row_height = MAX(atlas_row_height, height);
  return glyph;
}

static int utf8_next(const char** text) {
  const unsigned char* s = (const unsigned char*)*text;
  if (*s == 0) {
    return -1;
  }
  int codepoint = 0;
  int length = 0;
  if ((s[0] & 0x80) == 0) {
    codepoint = s[0];
    length = 1;
  } else if ((s[0] & 0xE0) == 0xC0) {
    codepoint = s[0] & 0x1F;
    length = 2;
  } else if ((s[0] & 0xF0) == 0xE0) {
    codepoint = s[0] & 0x0F;
    length = 3;
  } else if ((s[0] & 0xF8) == 0xF0) {
    codepoint = s[0] & 0x07;
    length = 4;
  } else {
    codepoint = '?';
    length = 1;
  }
  // Only consume bytes that were actually validated; a truncated sequence must
  // not advance past the terminating NUL.
  int consumed = 1;
  for (int i = 1; i < length; i++) {
    if (s[i] == 0 || (s[i] & 0xC0) != 0x80) {
      break;
    }
    codepoint = (codepoint << 6) | (s[i] & 0x3F);
    consumed++;
  }
  *text += consumed;
  return codepoint;
}

static float text_width(const char* text) {
  float width = 0.0f;
  const char* cursor = text;
  for (;;) {
    int codepoint = utf8_next(&cursor);
    if (codepoint < 0) {
      break;
    }
    Glyph* glyph = glyph_for((uint32_t)codepoint);
    if (glyph != NULL) {
      width += glyph->advance;
    }
  }
  return width;
}

static void text_add_vertex(float x, float y, float u, float v, Color color) {
  if (text_vertex_count + 8 > text_capacity) {
    size_t capacity = text_capacity == 0 ? 4096 : text_capacity * 2;
    GLfloat* grown = realloc(text_vertices, capacity * sizeof(GLfloat));
    if (grown == NULL) {
      return;
    }
    text_vertices = grown;
    text_capacity = capacity;
  }
  GLfloat* out = text_vertices + text_vertex_count;
  out[0] = x / (float)window_width * 2.0f - 1.0f;
  out[1] = 1.0f - y / (float)window_height * 2.0f;
  out[2] = u;
  out[3] = v;
  out[4] = color.r;
  out[5] = color.g;
  out[6] = color.b;
  out[7] = color.a;
  text_vertex_count += 8;
}

static void text_draw(float x, float y, Color color, const char* text) {
  if (font_data == NULL || text == NULL) {
    return;
  }
  const char* cursor = text;
  float pen_x = x;
  const float baseline = y + (float)font_ascent * font_scale;
  for (;;) {
    int codepoint = utf8_next(&cursor);
    if (codepoint < 0) {
      break;
    }
    Glyph* glyph = glyph_for((uint32_t)codepoint);
    if (glyph == NULL) {
      continue;
    }
    if (glyph->atlas_w > 0 && glyph->atlas_h > 0) {
      const float x0 = pen_x + (float)glyph->x0;
      const float y0 = baseline + (float)glyph->y0;
      const float x1 = x0 + (float)glyph->atlas_w;
      const float y1 = y0 + (float)glyph->atlas_h;
      const float u0 = (float)glyph->atlas_x / (float)ATLAS_SIZE;
      const float v0 = (float)glyph->atlas_y / (float)ATLAS_SIZE;
      const float u1 = (float)(glyph->atlas_x + glyph->atlas_w) / (float)ATLAS_SIZE;
      const float v1 = (float)(glyph->atlas_y + glyph->atlas_h) / (float)ATLAS_SIZE;

      // Two triangles (6 vertices).
      text_add_vertex(x0, y0, u0, v0, color);
      text_add_vertex(x1, y0, u1, v0, color);
      text_add_vertex(x0, y1, u0, v1, color);
      text_add_vertex(x1, y0, u1, v0, color);
      text_add_vertex(x1, y1, u1, v1, color);
      text_add_vertex(x0, y1, u0, v1, color);
    }
    pen_x += glyph->advance;
  }
}

static void rect_add(float x, float y, float w, float h, Color color) {
  if (w <= 0.0f || h <= 0.0f) {
    return;
  }
  if (rect_vertex_count + 36 > rect_capacity) {
    size_t capacity = rect_capacity == 0 ? 4096 : rect_capacity * 2;
    GLfloat* grown = realloc(rect_vertices, capacity * sizeof(GLfloat));
    if (grown == NULL) {
      return;
    }
    rect_vertices = grown;
    rect_capacity = capacity;
  }
  const float x0 = x / (float)window_width * 2.0f - 1.0f;
  const float x1 = (x + w) / (float)window_width * 2.0f - 1.0f;
  const float y0 = 1.0f - y / (float)window_height * 2.0f;
  const float y1 = 1.0f - (y + h) / (float)window_height * 2.0f;

  const float vertices[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1}};
  for (int i = 0; i < 6; i++) {
    GLfloat* out = rect_vertices + rect_vertex_count;
    out[0] = vertices[i][0];
    out[1] = vertices[i][1];
    out[2] = color.r;
    out[3] = color.g;
    out[4] = color.b;
    out[5] = color.a;
    rect_vertex_count += 6;
  }
}

// ---------------------------------------------------------------- rendering

static void create_side_target(Side* side, int width, int height) {
  if (side->texture != 0) {
    glDeleteTextures(1, &side->texture);
  }
  if (side->fbo != 0) {
    glDeleteFramebuffers(1, &side->fbo);
  }

  glGenTextures(1, &side->texture);
  glBindTexture(GL_TEXTURE_2D, side->texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, gles3 ? GL_RGBA8 : GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

  glGenFramebuffers(1, &side->fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, side->fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, side->texture, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    die("side framebuffer incomplete");
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);

  side->width = width;
  side->height = height;
}

static void resize_targets(int width, int height) {
  if (width <= 0 || height <= 0) {
    return;
  }
  if (sides[0].width == width && sides[0].height == height && sides[0].texture != 0) {
    return;
  }
  create_side_target(&sides[0], width, height);
  create_side_target(&sides[1], width, height);
  force_render = 1;
  dirty = 1;
}

static void sync_surface_size(void) {
  EGLint width = 0;
  EGLint height = 0;
  if (eglQuerySurface(egl_display, egl_surface, EGL_WIDTH, &width) && eglQuerySurface(egl_display, egl_surface, EGL_HEIGHT, &height)) {
    if (width > 0 && height > 0 && (width != window_width || height != window_height)) {
      window_width = width;
      window_height = height;
      resize_targets(width, height);
    }
  }
}

static void render_side(Side* side) {
  if (side->render == NULL || side->fbo == 0) {
    return;
  }
  mpv_opengl_fbo fbo = {.fbo = (int)side->fbo, .w = side->width, .h = side->height, .internal_format = gles3 ? GL_RGBA8 : GL_RGBA};
  int flip_y = 1;
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
      {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
      {0},
  };
  mpv_render_context_render(side->render, params);
  side->rendered = 1;
  side->has_frame = 1;
}

static void format_time(double seconds, char* out, size_t out_size) {
  if (seconds < 0.0) {
    seconds = 0.0;
  }
  const int total = (int)seconds;
  const int hours = total / 3600;
  const int minutes = (total / 60) % 60;
  const int secs = total % 60;
  if (hours > 0) {
    snprintf(out, out_size, "%d:%02d:%02d", hours, minutes, secs);
  } else {
    snprintf(out, out_size, "%02d:%02d", minutes, secs);
  }
}

static int ui_px(float value) {
  return (int)(value * ui_scale + 0.5f);
}

static int ui_top_bar_height(void) {
  return ui_px(30.0f);
}

static int ui_bottom_bar_height(void) {
  return ui_px(58.0f);
}

static void draw_ui(void) {
  const int w = window_width;
  const int h = window_height;
  const int bar_height = ui_bottom_bar_height();
  const int top_bar_height = ui_top_bar_height();

  rect_vertex_count = 0;
  text_vertex_count = 0;

  // Top bar with file names.
  const Color top_bar = {0.05f, 0.06f, 0.08f, 0.72f};
  rect_add(0, 0, (float)w, (float)top_bar_height, top_bar);

  char left_label[512];
  char right_label[512];
  snprintf(left_label, sizeof(left_label), "L  %s", sides[0].name != NULL ? sides[0].name : "");
  snprintf(right_label, sizeof(right_label), "R  %s", sides[1].name != NULL ? sides[1].name : "");
  text_draw((float)ui_px(10.0f), (float)ui_px(4.0f), COLOR_WHITE, left_label);

  const float right_width = text_width(right_label);
  text_draw((float)w - (float)ui_px(10.0f) - right_width, (float)ui_px(4.0f), COLOR_WHITE, right_label);

  // Bottom bar with progress and times.
  const Color bottom_bar = {0.05f, 0.06f, 0.08f, 0.78f};
  rect_add(0, (float)(h - bar_height), (float)w, (float)bar_height, bottom_bar);

  const float bar_x = (float)ui_px(14.0f);
  const float bar_w = (float)w - 2.0f * bar_x;
  const float bar_h = (float)ui_px(6.0f);
  const float bar_y = (float)h - (float)ui_px(20.0f);

  double progress = 0.0;
  if (duration > 0.001) {
    progress = CLAMP(position / duration, 0.0, 1.0);
  }

  const Color track = {1.0f, 1.0f, 1.0f, 0.22f};
  rect_add(bar_x, bar_y, bar_w, bar_h, track);
  rect_add(bar_x, bar_y, bar_w * (float)progress, bar_h, COLOR_ACCENT);

  const float knob_w = (float)ui_px(10.0f);
  rect_add(bar_x + bar_w * (float)progress - knob_w * 0.5f, bar_y - (float)ui_px(3.0f), knob_w, bar_h + (float)ui_px(6.0f), COLOR_WHITE);

  char position_text[64];
  char duration_text[64];
  format_time(position, position_text, sizeof(position_text));
  format_time(duration, duration_text, sizeof(duration_text));

  char time_label[160];
  snprintf(time_label, sizeof(time_label), "%s / %s", position_text, duration_text);
  text_draw(bar_x, (float)(h - bar_height + ui_px(6.0f)), COLOR_WHITE, time_label);

  char status[160];
  snprintf(status, sizeof(status), "%s  %.2fx%s", paused ? "PAUSED" : "PLAY", speed, loop_enabled ? "  LOOP" : "");
  const float status_width = text_width(status);
  text_draw((float)w - (float)ui_px(14.0f) - status_width, (float)(h - bar_height + ui_px(6.0f)), COLOR_DIM, status);

  // Toast message.
  if (toast[0] != 0 && now_seconds() < toast_until) {
    const float toast_width = text_width(toast);
    const float x = ((float)w - toast_width) * 0.5f;
    const float y = (float)(h - bar_height - ui_px(46.0f));
    const Color toast_bg = {0.0f, 0.0f, 0.0f, 0.72f};
    rect_add(x - (float)ui_px(12.0f), y - (float)ui_px(5.0f), toast_width + (float)ui_px(24.0f), (float)ui_px(30.0f), toast_bg);
    text_draw(x, y, COLOR_WHITE, toast);
    dirty = 1;
  }

  // Help overlay.
  if (show_help) {
    const Color help_bg = {0.02f, 0.02f, 0.03f, 0.86f};
    rect_add(0, 0, (float)w, (float)h, help_bg);

    static const char* lines[] = {
        "空格      播放 / 暂停",
        "← / →     后退 / 前进 5 秒",
        ", / .     上一帧 / 下一帧",
        "[ / ]     减速 / 加速",
        "L         循环播放开关",
        "S         保存截图 (BMP)",
        "F         全屏切换",
        "0         分割线回到中间",
        "H         显示 / 隐藏本帮助",
        "Q / ESC   退出",
        "",
        "鼠标左键拖动分割线，拖动底部进度条跳转",
        NULL,
    };

    const float line_height = (float)ui_px(32.0f);
    int line_count = 0;
    for (int i = 0; lines[i] != NULL; i++) {
      line_count++;
    }
    float y = ((float)h - line_height * (float)(line_count - 1)) * 0.5f;
    for (int i = 0; lines[i] != NULL; i++) {
      const float line_width = text_width(lines[i]);
      text_draw(((float)w - line_width) * 0.5f, y, COLOR_WHITE, lines[i]);
      y += line_height;
    }
  }
}

static void draw_rect_batch(void) {
  if (rect_vertex_count == 0) {
    return;
  }
  if (attr_rect_pos < 0 || attr_rect_color < 0) {
    return;
  }
  glUseProgram(rect_program);
  glBindBuffer(GL_ARRAY_BUFFER, rect_vbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(rect_vertex_count * sizeof(GLfloat)), rect_vertices, GL_STREAM_DRAW);
  glEnableVertexAttribArray((GLuint)attr_rect_pos);
  glEnableVertexAttribArray((GLuint)attr_rect_color);
  glVertexAttribPointer((GLuint)attr_rect_pos, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), (const void*)0);
  glVertexAttribPointer((GLuint)attr_rect_color, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), (const void*)(2 * sizeof(GLfloat)));
  glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(rect_vertex_count / 6));
  glDisableVertexAttribArray((GLuint)attr_rect_pos);
  glDisableVertexAttribArray((GLuint)attr_rect_color);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glUseProgram(0);
}

static void draw_text_batch(void) {
  if (text_vertex_count == 0 || font_data == NULL) {
    return;
  }
  if (attr_text_pos < 0 || attr_text_uv < 0 || attr_text_color < 0) {
    return;
  }
  glUseProgram(text_program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, font_texture);
  glUniform1i(loc_text_atlas, 0);
  glBindBuffer(GL_ARRAY_BUFFER, text_vbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(text_vertex_count * sizeof(GLfloat)), text_vertices, GL_STREAM_DRAW);
  glEnableVertexAttribArray((GLuint)attr_text_pos);
  glEnableVertexAttribArray((GLuint)attr_text_uv);
  glEnableVertexAttribArray((GLuint)attr_text_color);
  glVertexAttribPointer((GLuint)attr_text_pos, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(GLfloat), (const void*)0);
  glVertexAttribPointer((GLuint)attr_text_uv, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(GLfloat), (const void*)(2 * sizeof(GLfloat)));
  glVertexAttribPointer((GLuint)attr_text_color, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(GLfloat), (const void*)(4 * sizeof(GLfloat)));
  glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(text_vertex_count / 8));
  glDisableVertexAttribArray((GLuint)attr_text_pos);
  glDisableVertexAttribArray((GLuint)attr_text_uv);
  glDisableVertexAttribArray((GLuint)attr_text_color);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
}

static void dump_framebuffer(const char* path);
static void write_screenshot_bmp(const unsigned char* pixels, int width, int height);

static void composite_and_present(void) {
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, window_width, window_height);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  // Video.
  glDisable(GL_BLEND);
  glUseProgram(video_program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, sides[0].texture);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, sides[1].texture);
  glUniform1i(loc_tex_left, 0);
  glUniform1i(loc_tex_right, 1);
  glUniform1f(loc_split, split);
  glUniform1f(loc_line_width, 1.5f / (float)MAX(1, window_width));

  if (attr_video_pos >= 0) {
    glBindBuffer(GL_ARRAY_BUFFER, video_vbo);
    glEnableVertexAttribArray((GLuint)attr_video_pos);
    glVertexAttribPointer((GLuint)attr_video_pos, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), (const void*)0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray((GLuint)attr_video_pos);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }
  glUseProgram(0);

  // UI.
  draw_ui();

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  draw_rect_batch();
  draw_text_batch();
  glDisable(GL_BLEND);

  if (dump_requested) {
    dump_framebuffer(dump_path);
    dump_requested = 0;
    dumped = 1;
  }

  if (screenshot_requested) {
    screenshot_requested = 0;
    screenshot_width = window_width;
    screenshot_height = window_height;
    unsigned char* captured = realloc(screenshot_pixels, (size_t)screenshot_width * (size_t)screenshot_height * 4);
    if (captured != NULL) {
      screenshot_pixels = captured;
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(0, 0, screenshot_width, screenshot_height, GL_RGBA, GL_UNSIGNED_BYTE, screenshot_pixels);
      screenshot_captured = 1;
    }
  }

  eglSwapBuffers(egl_display, egl_surface);
  presents++;

  for (int i = 0; i < 2; i++) {
    if (sides[i].rendered) {
      mpv_render_context_report_swap(sides[i].render);
      sides[i].rendered = 0;
    }
  }
  dirty = 0;

  if (screenshot_captured) {
    write_screenshot_bmp(screenshot_pixels, screenshot_width, screenshot_height);
    screenshot_captured = 0;
  }
}

// ---------------------------------------------------------------- mpv setup

// Called from the render thread: writes to a pipe only.
static void send_command(int type, double value) {
  const Command command = {type, value};
  const ssize_t written = write(cmd_pipe[1], &command, sizeof(command));
  (void)written;
}

static void init_side(Side* side, const char* path) {
  side->path = path;
  side->name = base_name(path);
  side->mpv = mpv_create();
  if (side->mpv == NULL) {
    die("mpv_create failed");
  }

  check_mpv(mpv_set_option_string(side->mpv, "vo", "libmpv"), "vo=libmpv");
  check_mpv(mpv_set_option_string(side->mpv, "hwdec", hwdec_mode), "hwdec");
  check_mpv(mpv_set_option_string(side->mpv, "audio", "no"), "audio=no");
  check_mpv(mpv_set_option_string(side->mpv, "pause", "yes"), "pause=yes");
  check_mpv(mpv_set_option_string(side->mpv, "keep-open", "yes"), "keep-open");
  check_mpv(mpv_set_option_string(side->mpv, "idle", "yes"), "idle");
  check_mpv(mpv_set_option_string(side->mpv, "terminal", "no"), "terminal=no");
  check_mpv(mpv_set_option_string(side->mpv, "msg-level", "all=warn"), "msg-level");
  check_mpv(mpv_set_option_string(side->mpv, "video-sync", "display-resample"), "video-sync");
  check_mpv(mpv_set_option_string(side->mpv, "osc", "no"), "osc=no");
  check_mpv(mpv_set_option_string(side->mpv, "config", "no"), "config=no");
  check_mpv(mpv_set_option_string(side->mpv, "input-default-bindings", "no"), "input bindings");
  check_mpv(mpv_set_option_string(side->mpv, "input-vo-keyboard", "no"), "input-vo-keyboard");

  check_mpv(mpv_initialize(side->mpv), "mpv_initialize");
  mpv_request_log_messages(side->mpv, "warn");

  mpv_opengl_init_params gl_init_params = {
      .get_proc_address = gl_get_proc_address,
      .get_proc_address_ctx = NULL,
  };
  int advanced_control = 1;
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL},
      {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
      {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
      {0},
  };
  check_mpv(mpv_render_context_create(&side->render, side->mpv, params), "mpv_render_context_create");

  int block_for_target_time = 0;
  mpv_render_param block_param = {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block_for_target_time};
  mpv_render_context_set_parameter(side->render, block_param);
  mpv_render_context_set_update_callback(side->render, on_mpv_render_update, side);
}

static void request_screenshot(void) {
  screenshot_requested = 1;
  dirty = 1;
}

static void write_screenshot_bmp(const unsigned char* pixels, int width, int height) {
  char base[128];
  char filename[160];
  time_t t = time(NULL);
  struct tm local;
  localtime_r(&t, &local);
  strftime(base, sizeof(base), "mpv-compare-%Y%m%d-%H%M%S", &local);
  snprintf(filename, sizeof(filename), "%s.bmp", base);
  for (int attempt = 1; attempt < 100 && access(filename, F_OK) == 0; attempt++) {
    snprintf(filename, sizeof(filename), "%s-%d.bmp", base, attempt);
  }

  FILE* file = fopen(filename, "wb");
  if (file == NULL) {
    set_toast("截图保存失败");
    return;
  }

  const int row_bytes = width * 3;
  const int padding = (4 - (row_bytes % 4)) % 4;
  const uint32_t data_size = (uint32_t)((row_bytes + padding) * height);
  unsigned char header[54] = {0};
  header[0] = 'B';
  header[1] = 'M';
  const uint32_t file_size = 54 + data_size;
  memcpy(header + 2, &file_size, 4);
  const uint32_t pixel_offset = 54;
  memcpy(header + 10, &pixel_offset, 4);
  const uint32_t dib_size = 40;
  memcpy(header + 14, &dib_size, 4);
  memcpy(header + 18, &width, 4);
  memcpy(header + 22, &height, 4);
  const uint16_t planes = 1;
  memcpy(header + 26, &planes, 2);
  const uint16_t bpp = 24;
  memcpy(header + 28, &bpp, 2);
  memcpy(header + 34, &data_size, 4);
  fwrite(header, 1, sizeof(header), file);

  unsigned char* row = malloc((size_t)row_bytes + (size_t)padding);
  if (row == NULL) {
    fclose(file);
    set_toast("截图保存失败");
    return;
  }
  memset(row, 0, (size_t)row_bytes + (size_t)padding);
  for (int y = 0; y < height; y++) {
    const unsigned char* source = pixels + (size_t)y * (size_t)width * 4;
    for (int x = 0; x < width; x++) {
      row[x * 3 + 0] = source[x * 4 + 2];
      row[x * 3 + 1] = source[x * 4 + 1];
      row[x * 3 + 2] = source[x * 4 + 0];
    }
    fwrite(row, 1, (size_t)row_bytes + (size_t)padding, file);
  }
  free(row);
  fclose(file);

  char message[200];
  snprintf(message, sizeof(message), "截图已保存：%s", filename);
  set_toast(message);
}

// ------------------------------------------------------------ control thread

static const char* hwdec_name(int code) {
  switch (code) {
    case 1:
      return "vaapi";
    case 2:
      return "vaapi-copy";
    case 3:
      return "other";
    default:
      return "?";
  }
}

static int hwdec_code(const char* name) {
  if (name == NULL) {
    return 0;
  }
  if (strcmp(name, "vaapi") == 0) {
    return 1;
  }
  if (strcmp(name, "vaapi-copy") == 0) {
    return 2;
  }
  return 3;
}

static void worker_execute(const Command* command, double* times, double* duration, int* paused_state, double* sync_suppress) {
  const double now = now_seconds();
  switch (command->type) {
    case CMD_PAUSE: {
      int flag = command->value != 0.0;
      for (int i = 0; i < 2; i++) {
        mpv_set_property(sides[i].mpv, "pause", MPV_FORMAT_FLAG, &flag);
      }
      *paused_state = flag;
      atomic_store(&g_paused, flag);
      break;
    }
    case CMD_SEEK_ABS: {
      double target = command->value;
      if (target < 0.0) {
        target = 0.0;
      }
      if (*duration > 0.001 && target > *duration - 0.05) {
        target = *duration - 0.05;
      }
      char value[64];
      snprintf(value, sizeof(value), "%.3f", target);
      const char* cmd[] = {"seek", value, "absolute+exact", NULL};
      for (int i = 0; i < 2; i++) {
        mpv_command(sides[i].mpv, cmd);
      }
      times[0] = target;
      times[1] = target;
      atomic_store(&g_time[0], target);
      atomic_store(&g_time[1], target);
      *sync_suppress = now + 1.5;
      break;
    }
    case CMD_SEEK_REL: {
      char value[64];
      snprintf(value, sizeof(value), "%.3f", command->value);
      const char* cmd[] = {"seek", value, "relative+exact", NULL};
      for (int i = 0; i < 2; i++) {
        mpv_command(sides[i].mpv, cmd);
      }
      *sync_suppress = now + 1.5;
      break;
    }
    case CMD_STEP_FORWARD:
    case CMD_STEP_BACKWARD: {
      const char* cmd[] = {command->type == CMD_STEP_FORWARD ? "frame-step" : "frame-back-step", NULL};
      for (int i = 0; i < 2; i++) {
        mpv_command(sides[i].mpv, cmd);
      }
      *sync_suppress = now + 1.5;
      break;
    }
    case CMD_SPEED: {
      double value = CLAMP(command->value, 0.2, 4.0);
      for (int i = 0; i < 2; i++) {
        mpv_set_property(sides[i].mpv, "speed", MPV_FORMAT_DOUBLE, &value);
      }
      atomic_store(&g_speed, value);
      break;
    }
    case CMD_LOOP: {
      const char* value = command->value != 0.0 ? "inf" : "no";
      for (int i = 0; i < 2; i++) {
        mpv_set_property_string(sides[i].mpv, "loop-file", value);
      }
      break;
    }
    default:
      break;
  }
}

static void* control_thread_main(void* arg) {
  (void)arg;
  double times[2] = {0.0, 0.0};
  double duration = 0.0;
  int paused_state = 1;
  double sync_suppress = 0.0;
  double next_poll = 0.0;
  double next_sync = now_seconds() + 2.0;

  // The control thread owns all non-render libmpv calls, including loadfile;
  // the render context must exist before the libmpv VO opens the file.
  for (int i = 0; i < 2; i++) {
    const char* load_command[] = {"loadfile", sides[i].path, "replace", NULL};
    mpv_command(sides[i].mpv, load_command);
  }

  for (int i = 0; i < 2; i++) {
    mpv_observe_property(sides[i].mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(sides[i].mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(sides[i].mpv, 0, "speed", MPV_FORMAT_DOUBLE);
    mpv_observe_property(sides[i].mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(sides[i].mpv, 0, "hwdec-current", MPV_FORMAT_STRING);
    mpv_observe_property(sides[i].mpv, 0, "frame-drop-count", MPV_FORMAT_INT64);
  }

  for (;;) {
    const int running = atomic_load(&control_running);
    const int freed = atomic_load(&renderers_freed);
    if (!running && freed) {
      break;
    }

    Command command;
    while (read(cmd_pipe[0], &command, sizeof(command)) == (ssize_t)sizeof(command)) {
      worker_execute(&command, times, &duration, &paused_state, &sync_suppress);
    }

    for (int i = 0; i < 2; i++) {
      for (;;) {
        mpv_event* event = mpv_wait_event(sides[i].mpv, 0);
        if (event->event_id == MPV_EVENT_NONE) {
          break;
        }
        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
          mpv_event_property* prop = (mpv_event_property*)event->data;
          if (prop->data == NULL) {
            continue;
          }
          if (strcmp(prop->name, "time-pos") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
            const double value = *(double*)prop->data;
            times[i] = value;
            atomic_store(&g_time[i], value);
          } else if (strcmp(prop->name, "duration") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
            duration = *(double*)prop->data;
            atomic_store(&g_duration, duration);
          } else if (strcmp(prop->name, "speed") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
            atomic_store(&g_speed, *(double*)prop->data);
          } else if (strcmp(prop->name, "pause") == 0 && prop->format == MPV_FORMAT_FLAG) {
            paused_state = *(int*)prop->data;
            atomic_store(&g_paused, paused_state);
          } else if (strcmp(prop->name, "hwdec-current") == 0) {
            const char* value = prop->format == MPV_FORMAT_STRING ? *(char**)prop->data : NULL;
            atomic_store(&g_hwdec[i], hwdec_code(value));
          } else if (strcmp(prop->name, "frame-drop-count") == 0 && prop->format == MPV_FORMAT_INT64) {
            atomic_store(&g_drops[i], (long long)*(int64_t*)prop->data);
          }
        } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
          mpv_event_log_message* message = (mpv_event_log_message*)event->data;
          fprintf(stderr, "[%s/%s] %s", i == 0 ? "L" : "R", message->prefix, message->text);
        } else if (event->event_id == MPV_EVENT_END_FILE) {
          mpv_event_end_file* end = (mpv_event_end_file*)event->data;
          if (end->reason == MPV_END_FILE_REASON_ERROR) {
            fprintf(stderr, "playback error: %s\n", mpv_error_string(end->error));
          }
        } else if (event->event_id == MPV_EVENT_SHUTDOWN) {
          atomic_store(&mpv_shutdown, 1);
        }
      }
    }

    const double now = now_seconds();
    if (now >= next_poll) {
      for (int i = 0; i < 2; i++) {
        char* hwdec = mpv_get_property_string(sides[i].mpv, "hwdec-current");
        atomic_store(&g_hwdec[i], hwdec_code(hwdec));
        mpv_free(hwdec);
        int64_t drops = 0;
        if (mpv_get_property(sides[i].mpv, "frame-drop-count", MPV_FORMAT_INT64, &drops) >= 0) {
          atomic_store(&g_drops[i], (long long)drops);
        }
      }
      next_poll = now + 1.0;
    }

    if (running && !paused_state && now >= next_sync && now >= sync_suppress) {
      next_sync = now + 2.0;
      const double delta = times[0] - times[1];
      if (fabs(delta) >= 0.40) {
        const int lagging = delta > 0.0 ? 1 : 0;
        const double target = lagging == 0 ? times[1] : times[0];
        char value[64];
        snprintf(value, sizeof(value), "%.3f", target);
        const char* cmd[] = {"seek", value, "absolute+exact", NULL};
        mpv_command(sides[lagging].mpv, cmd);
        sync_suppress = now + 1.5;
        atomic_fetch_add(&g_resync_count, 1);
      }
    }

    struct pollfd pfd = {.fd = cmd_pipe[0], .events = POLLIN};
    poll(&pfd, 1, 15);
  }

  for (int i = 0; i < 2; i++) {
    if (sides[i].mpv != NULL) {
      mpv_terminate_destroy(sides[i].mpv);
      sides[i].mpv = NULL;
    }
  }
  return NULL;
}

// ------------------------------------------------------------ UI-side control

static void set_pause(int pause) {
  paused = pause;
  send_command(CMD_PAUSE, pause ? 1.0 : 0.0);
  set_toast(pause ? "已暂停" : "播放");
}

// force=1 issues the seek immediately; otherwise rapid drags are throttled and
// the caller sends a final seek on button release.
static void seek_absolute(double seconds, int force) {
  const double total = atomic_load(&g_duration);
  if (seconds < 0.0 || total <= 0.001) {
    return;
  }
  seconds = MIN(seconds, total - 0.05);
  position = seconds;
  progress_preview = seconds;
  position_override_until = now_seconds() + 0.5;
  dirty = 1;
  if (force || now_seconds() - last_seek_issue >= 0.12) {
    send_command(CMD_SEEK_ABS, seconds);
    last_seek_issue = now_seconds();
  }
}

static void seek_relative(double seconds) {
  send_command(CMD_SEEK_REL, seconds);
  position_override_until = 0.0;
  char message[64];
  snprintf(message, sizeof(message), "%+.0f 秒", seconds);
  set_toast(message);
}

static void step_frame(int forward) {
  if (!paused) {
    set_pause(1);
  }
  send_command(forward ? CMD_STEP_FORWARD : CMD_STEP_BACKWARD, 0.0);
  set_toast(forward ? "下一帧" : "上一帧");
}

static void apply_speed(double new_speed) {
  speed = CLAMP(new_speed, 0.2, 4.0);
  send_command(CMD_SPEED, speed);
  char message[64];
  snprintf(message, sizeof(message), "速度 %.2fx", speed);
  set_toast(message);
}

static void toggle_loop(void) {
  loop_enabled = !loop_enabled;
  send_command(CMD_LOOP, loop_enabled ? 1.0 : 0.0);
  set_toast(loop_enabled ? "循环播放：开" : "循环播放：关");
}

static void print_stats(void) {
  printf("[stats] t=%.3f/%.3f hwdec=%s/%s drops=%lld/%lld present_fps=%lu paused=%d\n", atomic_load(&g_time[0]), atomic_load(&g_time[1]), hwdec_name(atomic_load(&g_hwdec[0])),
         hwdec_name(atomic_load(&g_hwdec[1])), (long long)atomic_load(&g_drops[0]), (long long)atomic_load(&g_drops[1]), presents - presents_last, atomic_load(&g_paused));
  fflush(stdout);
  presents_last = presents;
}

static void update_playback_state(void) {
  int changed = 0;
  const double duration_value = atomic_load(&g_duration);
  const double position_value = atomic_load(&g_time[0]);
  const double speed_value = atomic_load(&g_speed);
  const int paused_value = atomic_load(&g_paused);
  const double now = now_seconds();

  if (duration_value >= 0.0 && fabs(duration_value - duration) > 0.001) {
    duration = duration_value;
    changed = 1;
  }
  if (now >= position_override_until && position_value >= 0.0 && fabs(position_value - position) > 0.02) {
    position = position_value;
    changed = 1;
  }
  if (speed_value > 0.0 && fabs(speed_value - speed) > 0.001) {
    speed = speed_value;
    changed = 1;
  }
  if (paused != paused_value) {
    paused = paused_value;
    changed = 1;
  }

  static int last_resync = 0;
  const int resync = atomic_load(&g_resync_count);
  if (resync != last_resync) {
    last_resync = resync;
    set_toast("自动对齐完成");
    changed = 1;
  }

  if (changed) {
    dirty = 1;
  }
}

// ---------------------------------------------------------------- UI input

static int progress_bar_hit(int x, int y) {
  (void)x;
  const int bar_top = window_height - ui_bottom_bar_height();
  return y >= bar_top && y <= window_height;
}

static void seek_from_progress_x(int x, int force) {
  const float bar_x = (float)ui_px(14.0f);
  const float bar_w = (float)window_width - 2.0f * bar_x;
  const float fraction = CLAMP(((float)x - bar_x) / MAX(1.0f, bar_w), 0.0f, 1.0f);
  if (duration > 0.001) {
    seek_absolute((double)fraction * duration, force);
  }
}

static void handle_x_events(void) {
  while (XPending(x_display) > 0) {
    XEvent event;
    XNextEvent(x_display, &event);

    switch (event.type) {
      case ConfigureNotify:
        if (event.xconfigure.width != window_width || event.xconfigure.height != window_height) {
          window_width = event.xconfigure.width;
          window_height = event.xconfigure.height;
          resize_targets(window_width, window_height);
        }
        break;

      case ButtonPress:
        if (event.xbutton.button == Button1) {
          if (progress_bar_hit(event.xbutton.x, event.xbutton.y)) {
            dragging_progress = 1;
            seek_from_progress_x(event.xbutton.x, 1);
          } else if (event.xbutton.y < window_height - ui_bottom_bar_height()) {
            dragging_split = 1;
            split = CLAMP((float)event.xbutton.x / (float)MAX(1, window_width), 0.0f, 1.0f);
            dirty = 1;
          }
        }
        break;

      case ButtonRelease:
        if (event.xbutton.button == Button1) {
          dragging_split = 0;
          if (dragging_progress) {
            dragging_progress = 0;
            seek_absolute(progress_preview, 1);
          }
        }
        break;

      case MotionNotify:
        if (dragging_progress) {
          seek_from_progress_x(event.xmotion.x, 0);
        } else if (dragging_split) {
          split = CLAMP((float)event.xmotion.x / (float)MAX(1, window_width), 0.0f, 1.0f);
          dirty = 1;
        }
        break;

      case KeyPress: {
        KeySym key = XLookupKeysym(&event.xkey, 0);
        switch (key) {
          case 0xff1b:  // Escape
          case 'q':
            quit = 1;
            break;
          case ' ':
            set_pause(!paused);
            break;
          case 'f':
            toggle_fullscreen();
            break;
          case '0':
            split = 0.5f;
            set_toast("分割线居中");
            break;
          case 'h':
            show_help = !show_help;
            help_toggled_at = now_seconds();
            dirty = 1;
            break;
          case 'l':
            toggle_loop();
            break;
          case 's':
            request_screenshot();
            break;
          case 0xff51:  // Left
            seek_relative(-5.0);
            break;
          case 0xff53:  // Right
            seek_relative(5.0);
            break;
          case ',':
          case 0xff52:  // Up: previous frame
            step_frame(0);
            break;
          case '.':
          case 0xff54:  // Down: next frame
            step_frame(1);
            break;
          case '[':
            apply_speed(speed - 0.1);
            break;
          case ']':
            apply_speed(speed + 0.1);
            break;
          default:
            break;
        }
        break;
      }

      case ClientMessage:
        if ((Atom)event.xclient.data.l[0] == wm_delete_window) {
          quit = 1;
        }
        break;

      default:
        break;
    }
  }
}

static void dump_framebuffer(const char* path) {
  const size_t pixel_count = (size_t)window_width * (size_t)window_height;
  unsigned char* pixels = malloc(pixel_count * 4);
  if (pixels == NULL) {
    return;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, window_width, window_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

  FILE* file = fopen(path, "wb");
  if (file != NULL) {
    fprintf(file, "P6\n%d %d\n255\n", window_width, window_height);
    unsigned char* row = malloc((size_t)window_width * 3);
    if (row != NULL) {
      for (int y = window_height - 1; y >= 0; y--) {
        const unsigned char* source = pixels + (size_t)y * (size_t)window_width * 4;
        for (int x = 0; x < window_width; x++) {
          row[x * 3 + 0] = source[x * 4 + 0];
          row[x * 3 + 1] = source[x * 4 + 1];
          row[x * 3 + 2] = source[x * 4 + 2];
        }
        fwrite(row, 1, (size_t)window_width * 3, file);
      }
      free(row);
    }
    fclose(file);
    printf("dumped %s\n", path);
  }
  free(pixels);
}

// ---------------------------------------------------------------- main

int main(int argc, char** argv) {
  const char* paths[2] = {NULL, NULL};
  int path_count = 0;

  const char* hwdec_env = getenv("MPVC_HWDEC");
  if (hwdec_env != NULL && hwdec_env[0] != '\0') {
    hwdec_mode = hwdec_env;
  }

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--split") == 0 && i + 1 < argc) {
      split = (float)atof(argv[++i]);
    } else if (strcmp(argv[i], "--hwdec") == 0 && i + 1 < argc) {
      hwdec_mode = argv[++i];
    } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
      int w = 0;
      int h = 0;
      if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
        window_width = w;
        window_height = h;
      }
    } else if (path_count < 2) {
      paths[path_count++] = argv[i];
    }
  }
  if (path_count < 2) {
    fprintf(stderr, "usage: %s [--split 0.5] [--size WxH] [--hwdec auto-safe] left.mp4 right.mp4\n", argv[0]);
    return 1;
  }
  split = CLAMP(split, 0.0f, 1.0f);
  stats_enabled = getenv("MPVC_STATS") != NULL;
  dump_path = getenv("MPVC_DUMP");
  if (getenv("MPVC_DUMP_AT") != NULL) {
    dump_at = atof(getenv("MPVC_DUMP_AT"));
  } else if (dump_path != NULL) {
    dump_at = 3.0;
  }
  if (getenv("MPVC_AUTOPLAY") != NULL) {
    paused = 0;
  }

  signal(SIGPIPE, SIG_IGN);
  if (pipe(wake_pipe) != 0) {
    die("pipe failed");
  }
  fcntl(wake_pipe[0], F_SETFL, fcntl(wake_pipe[0], F_GETFL) | O_NONBLOCK);
  fcntl(wake_pipe[1], F_SETFL, fcntl(wake_pipe[1], F_GETFL) | O_NONBLOCK);
  if (pipe(cmd_pipe) != 0) {
    die("command pipe failed");
  }
  fcntl(cmd_pipe[0], F_SETFL, fcntl(cmd_pipe[0], F_GETFL) | O_NONBLOCK);
  fcntl(cmd_pipe[1], F_SETFL, fcntl(cmd_pipe[1], F_GETFL) | O_NONBLOCK);

  create_window();
  {
    const int screen_height = DisplayHeight(x_display, DefaultScreen(x_display));
    font_pixel_size = (int)CLAMP((float)screen_height / 40.0f, 16.0f, 30.0f);
    ui_scale = (float)font_pixel_size / 20.0f;
  }
  {
    char title[1024];
    snprintf(title, sizeof(title), "mpv compare — %s | %s", base_name(paths[0]), base_name(paths[1]));
    set_window_title(title);
  }
  create_egl();
  create_video_pipeline();
  create_rect_pipeline();
  create_text_pipeline();
  load_font();
  resize_targets(window_width, window_height);

  sides[0].path = paths[0];
  sides[1].path = paths[1];
  init_side(&sides[0], paths[0]);
  init_side(&sides[1], paths[1]);

  if (pthread_create(&control_thread, NULL, control_thread_main, NULL) != 0) {
    die("cannot start control thread");
  }
  if (!paused) {
    set_pause(0);
  }
  set_toast(paused ? "按空格播放" : "");

  const int x_fd = ConnectionNumber(x_display);
  const double start_time = now_seconds();
  ui_refresh_next = start_time;
  stats_next = start_time + 1.0;

  while (!quit) {
    struct pollfd fds[2] = {
        {.fd = x_fd, .events = POLLIN},
        {.fd = wake_pipe[0], .events = POLLIN},
    };
    poll(fds, 2, 4);

    if (fds[1].revents & POLLIN) {
      char buffer[64];
      while (read(wake_pipe[0], buffer, sizeof(buffer)) > 0) {
      }
    }

    handle_x_events();

    if (atomic_load(&mpv_shutdown)) {
      quit = 1;
    }

    for (int i = 0; i < 2; i++) {
      if (atomic_exchange(&update_pending_flag[i], 0)) {
        uint64_t flags = mpv_render_context_update(sides[i].render);
        if (flags & MPV_RENDER_UPDATE_FRAME) {
          render_side(&sides[i]);
          dirty = 1;
        }
      }
    }

    if (force_render) {
      for (int i = 0; i < 2; i++) {
        if (sides[i].has_frame) {
          render_side(&sides[i]);
        }
      }
      force_render = 0;
      dirty = 1;
    }

    const double now = now_seconds();
    if (now >= ui_refresh_next) {
      update_playback_state();
      ui_refresh_next = now + 0.25;
    }

    if (dirty) {
      sync_surface_size();
      composite_and_present();
    }

    if (stats_enabled && now >= stats_next) {
      print_stats();
      stats_next = now + 1.0;
    }
    if (!dumped && dump_path != NULL && (now - start_time) >= dump_at) {
      dump_requested = 1;
      dirty = 1;
    }
    if (toast[0] != 0 && now >= toast_until) {
      toast[0] = 0;
      dirty = 1;
    }
  }

  atomic_store(&control_running, 0);
  for (int i = 0; i < 2; i++) {
    if (sides[i].render != NULL) {
      mpv_render_context_set_update_callback(sides[i].render, NULL, NULL);
      mpv_render_context_free(sides[i].render);
      sides[i].render = NULL;
    }
  }
  atomic_store(&renderers_freed, 1);
  pthread_join(control_thread, NULL);
  close(cmd_pipe[0]);
  close(cmd_pipe[1]);

  if (font_texture != 0) {
    glDeleteTextures(1, &font_texture);
  }
  free(font_data);
  free(rect_vertices);
  free(text_vertices);

  eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(egl_display, egl_surface);
  eglDestroyContext(egl_display, egl_context);
  eglTerminate(egl_display);
  XDestroyWindow(x_display, x_window);
  XCloseDisplay(x_display);
  return 0;
}
