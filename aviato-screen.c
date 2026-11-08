/* Aviato screen, in C.
 * The monitor scans /dev/fb0 in hardware. This process fills that buffer
 * only when the minute, the weather, or a displayed price changes, then sleeps.
 * Low-power defaults: crypto 60s (ambient 300s), metals 300s, weather 900s, system 60s.
 * Build: gcc -O2 -s -o screen aviato-screen.c -lm /usr/lib/x86_64-linux-gnu/libcurl.so.4
 */
#define _GNU_SOURCE
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "json-reader.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include "portfolio-cache.h"
#include "quotes.h"

typedef void CURL;
#define CURLOPT_WRITEDATA 10001
#define CURLOPT_URL 10002
#define CURLOPT_USERAGENT 10018
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_TIMEOUT 13
#define CURLOPT_FOLLOWLOCATION 52
#define CURLOPT_NOSIGNAL 99
#define CURLOPT_CONNECTTIMEOUT 78
#define CURLOPT_TCP_KEEPALIVE 213
#define CURLOPT_FAILONERROR 45
#define CURLOPT_HEADERFUNCTION 20079
#define CURLOPT_HEADERDATA 10029
#define CURLINFO_RESPONSE_CODE 0x200002
#define CURLOPT_HTTP_VERSION 84
#define CURLOPT_IPRESOLVE 113
#define CURL_HTTP_VERSION_1_1 2L
#define CURL_IPRESOLVE_V4 1L
CURL *curl_easy_init(void);
int curl_easy_setopt(CURL *, int, ...);
int curl_easy_perform(CURL *);
int curl_easy_getinfo(CURL *, int, ...);
void curl_easy_cleanup(CURL *);

#define W 1920
#define H 1080
#define FONT_PATH "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"
#define FONT_BOLD_PATH "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"
#define WEATHER_URL \
  "https://api.open-meteo.com/v1/forecast?latitude=26.4499&longitude=80.3319" \
  "&current=temperature_2m,weather_code" \
  "&daily=weather_code,temperature_2m_max,temperature_2m_min" \
  "&timezone=Asia%2FKolkata&forecast_days=4"

static const char *WDAY[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *MON[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
static const char *WSHORT[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

typedef struct {
  unsigned char r, g, b;
} Color;

typedef struct { int x0, y0, x1, y1; } Rect;

typedef struct {
  unsigned char *pix;
  int w, h, pitch;
  int fd;
  Rect *damage;
} Canvas;

typedef struct {
  int ok;
  int temp;
  int code;
  int n;
  char date[4][11];
  int high[4];
  int low[4];
} Weather;

typedef struct {
  int ok;
  int has_change;
  double value;
  double change;
} Quote;

typedef struct {
  Quote btc, eth, gold, silver;
} Markets;

typedef struct {
  char *data;
  size_t n, cap;
} Body;

typedef struct {
  double interval, next, retry;
} Refresh;

typedef struct {
  int cpu;
  double load, used, total;
} SystemInfo;

#define BODY_LIMIT (256u * 1024u)

static stbtt_fontinfo g_font;
static stbtt_fontinfo g_font_bold;
static unsigned char *g_ttf;
static unsigned char *g_ttf_bold;
static int g_bold_ready;
static int g_face_id;
static int g_text_alpha = 255;
static Markets g_markets;
static CURL *g_coin;
static CURL *g_metal;
static CURL *g_weather;
static Body g_body;
static Refresh g_coin_refresh = {60, 0, 60};
static Refresh g_metal_refresh = {300, 0, 300};
static Refresh g_weather_refresh = {900, 0, 900};
static Refresh g_system_refresh = {60, 0, 60};
static SystemInfo g_system;
static double g_fail_log;
static Portfolio g_portfolio;
static int g_shared_frame;
static int g_ambient;
static int g_minimal;
enum { PROFILE_LEGACY, PROFILE_FOCUS, PROFILE_ALBUM, PROFILE_AMBIENT, PROFILE_ADAPTIVE };
static int g_profile = PROFILE_LEGACY;
static volatile sig_atomic_t g_agent_dirty = 1;
static int g_face_blink;
static int g_face_dirty;
static double g_face_deadline;
static void paint_minimal(Canvas *, const struct tm *, const Weather *);
static void paint_ambient(Canvas *c, const struct tm *tm);
static volatile sig_atomic_t g_wake;
static volatile sig_atomic_t g_redraw;

static void wake_display(int signal_number) {
  (void)signal_number;
  g_wake = 1;
  g_redraw = 1;
  g_agent_dirty = 1;
}

static void wake_agent_status(int signal_number) {
  (void)signal_number;
  g_wake = 1;
  g_agent_dirty = 1;
}

static void sample_portfolio(void) {
  const char *visible=getenv("SCREEN_SHOW_PORTFOLIO");
  if (visible && !strcmp(visible,"0")) { g_portfolio=(Portfolio){0}; return; }
  const char *path=getenv("SCREEN_PORTFOLIO_PATH"), *home=getenv("HOME");
  char buffer[4096];
  if (!path || !*path) {
    if (!home) return;
    int n=snprintf(buffer,sizeof buffer,"%s/.local/state/aviato-screen/portfolio.json",home);
    if (n<0 || n>=(int)sizeof buffer) return;
    path=buffer;
  }
  portfolio_load(path,&g_portfolio,time(NULL));
}

static int curl_open(CURL **out);
static int curl_fetch(CURL *c, const char *url);

static double env_interval(const char *name, long fallback, long minimum) {
  const char *s = getenv(name);
  if (!s || !*s) return fallback;
  errno = 0;
  char *end;
  long value = strtol(s, &end, 10);
  if (errno || end == s || *end || value < minimum || value > 86400) return fallback;
  return value;
}

static void configure_refresh(void) {
  g_coin_refresh.interval = env_interval("SCREEN_CRYPTO_INTERVAL", 60, 60);
  g_metal_refresh.interval = env_interval("SCREEN_METALS_INTERVAL", 300, 300);
  g_weather_refresh.interval = env_interval("SCREEN_WEATHER_INTERVAL", 900, 900);
  g_system_refresh.interval = env_interval("SCREEN_SYSTEM_INTERVAL", 60, 60);
  if (g_minimal) {
    g_coin_refresh.interval = 60;
    g_weather_refresh.interval = 900;
    g_system_refresh.interval = 60;
  }
  g_coin_refresh.retry = g_coin_refresh.interval;
  g_metal_refresh.retry = g_metal_refresh.interval;
  g_weather_refresh.retry = g_weather_refresh.interval;
  g_system_refresh.retry = g_system_refresh.interval;
}

static void refresh_done(Refresh *r, double now, int success) {
  double cap = fmax(r->interval, 3600);
  r->retry = success ? r->interval : fmin(r->retry * 2, cap);
  r->next = now + r->retry;
}

static void logmsg(const char *s) { fputs(s, stdout); fputc('\n', stdout); }

static double mono(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static struct tm now_ist(void) {
  time_t t = time(NULL) + 5 * 3600 + 30 * 60;
  struct tm tm;
  gmtime_r(&t, &tm);
  const char *forced = getenv("SCREEN_FORCE_HOUR");
  if (forced && *forced) tm.tm_hour = atoi(forced);
  return tm;
}

static int theme_of(const struct tm *tm) {
  if (tm->tm_hour < 7) return 0;  /* night clock */
  if (tm->tm_hour < 22) return 1; /* day */
  return 2;                       /* night dashboard */
}

static Color bg_of(int theme) {
  if (theme == 2) return (Color){11, 11, 12};
  return (Color){0, 0, 0};
}
static Color fg_of(int theme) {
  if (theme == 0) return (Color){122, 42, 18};
  if (theme == 1) return (Color){236, 235, 231};
  return (Color){138, 138, 138};
}
static Color dim_of(int theme) {
  if (theme == 0) return (Color){92, 32, 14};
  if (theme == 1) return (Color){111, 116, 123};
  return (Color){138, 138, 138};
}
static Color down_of(int theme) {
  if (theme == 0) return (Color){122, 42, 18};
  return (Color){217, 138, 126};
}
static Color up_of(int theme) {
  (void)theme;
  return (Color){127, 191, 152};
}

static void fill(Canvas *c, Color col) {
  for (int y = 0; y < c->h; y++) {
    unsigned char *p = c->pix + (size_t)y * c->pitch;
    for (int x = 0; x < c->w; x++) {
      p[0] = col.b;
      p[1] = col.g;
      p[2] = col.r;
      p[3] = 0;
      p += 4;
    }
  }
}

static void put(Canvas *c, int x, int y, Color col, int a) {
  if ((unsigned)x >= (unsigned)c->w || (unsigned)y >= (unsigned)c->h || a <= 0) return;
  if (c->damage) {
    if (x < c->damage->x0) c->damage->x0 = x;
    if (y < c->damage->y0) c->damage->y0 = y;
    if (x + 1 > c->damage->x1) c->damage->x1 = x + 1;
    if (y + 1 > c->damage->y1) c->damage->y1 = y + 1;
  }
  unsigned char *p = c->pix + (size_t)y * c->pitch + x * 4;
  if (a >= 255) {
    p[0] = col.b;
    p[1] = col.g;
    p[2] = col.r;
    return;
  }
  p[0] = (unsigned char)((col.b * a + p[0] * (255 - a)) / 255);
  p[1] = (unsigned char)((col.g * a + p[1] * (255 - a)) / 255);
  p[2] = (unsigned char)((col.r * a + p[2] * (255 - a)) / 255);
}

static int utf8_next(const char **s) {
  const unsigned char *p = (const unsigned char *)*s;
  if (*p == 0) return 0;
  if (*p < 0x80) {
    *s += 1;
    return *p;
  }
  if ((*p & 0xE0) == 0xC0 && p[1]) {
    *s += 2;
    return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
  }
  if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
    *s += 3;
    return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
  }
  *s += 1;
  return '?';
}

static stbtt_fontinfo *active_font(void) {
  return (g_face_id && g_bold_ready) ? &g_font_bold : &g_font;
}

static int active_face(void) {
  return (g_face_id && g_bold_ready) ? 1 : 0;
}

static int font_ascent(int size) {
  int asc, desc, gap;
  stbtt_fontinfo *font = active_font();
  stbtt_GetFontVMetrics(font, &asc, &desc, &gap);
  return (int)(asc * stbtt_ScaleForPixelHeight(font, (float)size) + 0.5f);
}

static int advance_of(int cp, float scale) {
  int adv = 0, lsb = 0;
  stbtt_GetCodepointHMetrics(active_font(), cp, &adv, &lsb);
  return (int)(adv * scale + 0.5f);
}

static int text_width(const char *s, int size, int tracking) {
  float scale = stbtt_ScaleForPixelHeight(active_font(), (float)size);
  int width = 0, n = 0;
  const char *p = s;
  int cp;
  while ((cp = utf8_next(&p)) != 0) {
    if (n) width += tracking;
    width += advance_of(cp, scale);
    n++;
  }
  return width;
}

/* Clock digits are redrawn every minute. Keep their rasters in static storage
 * so a steady display does not allocate or re-rasterize them. */
#define GLYPH_SLOTS 256
#define GLYPH_STORE (2 * 1024 * 1024)
typedef struct {
  int cp, size, face, gw, gh, xoff, yoff;
  unsigned char *bmp;
} Glyph;
static Glyph g_glyphs[GLYPH_SLOTS];
static int g_glyph_count;
static unsigned char g_glyph_store[GLYPH_STORE];
static size_t g_glyph_used;

static void blit_glyph(Canvas *c, int x, int baseline, const unsigned char *bmp,
                       int gw, int gh, int xoff, int yoff, Color col) {
  for (int row = 0; row < gh; row++) {
    const unsigned char *src = bmp + row * gw;
    for (int colx = 0; colx < gw; colx++) {
      int a = src[colx];
      if (g_text_alpha < 255) a = a * g_text_alpha / 255;
      if (a) put(c, x + xoff + colx, baseline + yoff + row, col, a);
    }
  }
}

static void draw_text(Canvas *c, int x, int y, const char *s, int size, Color col, int tracking) {
  stbtt_fontinfo *font = active_font();
  int face = active_face();
  float scale = stbtt_ScaleForPixelHeight(font, (float)size);
  int baseline = y + font_ascent(size);
  const char *p = s;
  int cp, n = 0;
  while ((cp = utf8_next(&p)) != 0) {
    if (n) x += tracking;
    n++;
    Glyph *cached = NULL;
    for (int i = 0; i < g_glyph_count; i++) {
      if (g_glyphs[i].cp == cp && g_glyphs[i].size == size && g_glyphs[i].face == face) {
        cached = &g_glyphs[i];
        break;
      }
    }
    if (cached) {
      if (cached->bmp) blit_glyph(c, x, baseline, cached->bmp, cached->gw, cached->gh, cached->xoff, cached->yoff, col);
    } else {
      int gw = 0, gh = 0, xoff = 0, yoff = 0;
      unsigned char *bmp = stbtt_GetCodepointBitmap(font, scale, scale, cp, &gw, &gh, &xoff, &yoff);
      if (bmp) {
        blit_glyph(c, x, baseline, bmp, gw, gh, xoff, yoff, col);
        size_t bytes = (size_t)gw * (size_t)gh;
        if (gw > 0 && gh > 0 && g_glyph_count < GLYPH_SLOTS && g_glyph_used + bytes <= GLYPH_STORE) {
          unsigned char *slot = g_glyph_store + g_glyph_used;
          memcpy(slot, bmp, bytes);
          g_glyph_used += bytes;
          g_glyphs[g_glyph_count++] = (Glyph){cp, size, face, gw, gh, xoff, yoff, slot};
        }
        stbtt_FreeBitmap(bmp, NULL);
      }
    }
    x += advance_of(cp, scale);
  }
}

static void fill_disc(Canvas *c, int cx, int cy, int r, Color col) {
  int r2 = r * r;
  for (int dy = -r; dy <= r; dy++) {
    for (int dx = -r; dx <= r; dx++) {
      if (dx * dx + dy * dy <= r2) put(c, cx + dx, cy + dy, col, 255);
    }
  }
}

static void ring(Canvas *c, int cx, int cy, int r, Color col) {
  int x = r, y = 0;
  int err = 1 - x;
  while (x >= y) {
    put(c, cx + x, cy + y, col, 255);
    put(c, cx + y, cy + x, col, 255);
    put(c, cx - y, cy + x, col, 255);
    put(c, cx - x, cy + y, col, 255);
    put(c, cx - x, cy - y, col, 255);
    put(c, cx - y, cy - x, col, 255);
    put(c, cx + y, cy - x, col, 255);
    put(c, cx + x, cy - y, col, 255);
    y++;
    if (err < 0) err += 2 * y + 1;
    else {
      x--;
      err += 2 * (y - x) + 1;
    }
  }
}

static void hair(Canvas *c, int x0, int x1, int y, Color col) {
  if (x1 < x0) {
    int t = x0;
    x0 = x1;
    x1 = t;
  }
  for (int x = x0; x <= x1; x++) put(c, x, y, col, 255);
}

static void vhair(Canvas *c, int x, int y0, int y1, Color col) {
  if (y1 < y0) {
    int t = y0;
    y0 = y1;
    y1 = t;
  }
  for (int y = y0; y <= y1; y++) put(c, x, y, col, 255);
}

static void line(Canvas *c, int x0, int y0, int x1, int y1, Color col) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    put(c, x0, y0, col, 255);
    put(c, x0 + 1, y0, col, 180);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

static void icon(Canvas *c, int x, int y, int s, const char *kind, Color col, Color bg) {
  int cx = x + s / 2;
  int cy = y + s / 2;
  if (strcmp(kind, "clear") == 0) {
    int r = s * 22 / 100;
    if (r < 3) r = 3;
    fill_disc(c, cx, cy, r, col);
    int dirs[8][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}, {1, -1}, {1, 1}, {-1, -1}, {-1, 1}};
    for (int i = 0; i < 8; i++) {
      int dx = dirs[i][0], dy = dirs[i][1];
      int norm = (dx && dy) ? 7 : 10;
      line(c, cx + dx * (r + s / 10), cy + dy * (r + s / 10) * norm / 10, cx + dx * (r + s / 4),
           cy + dy * (r + s / 4) * norm / 10, col);
    }
  } else if (strcmp(kind, "moon") == 0) {
    fill_disc(c, cx - s / 12, cy, s * 32 / 100, col);
    fill_disc(c, cx + s / 8, cy - s / 12, s * 28 / 100, bg);
  } else if (strcmp(kind, "cloud") == 0 || strcmp(kind, "rain") == 0) {
    int r1 = s * 16 / 100, r2 = s * 22 / 100;
    fill_disc(c, x + s * 35 / 100, y + s * 55 / 100, r1, col);
    fill_disc(c, x + s * 62 / 100, y + s * 48 / 100, r2, col);
    for (int i = x + s / 5; i < x + s * 4 / 5; i++) put(c, i, y + s * 68 / 100, col, 255);
    if (strcmp(kind, "rain") == 0) {
      line(c, cx - s / 8, y + s * 74 / 100, cx - s / 5, y + s * 92 / 100, col);
      line(c, cx + s / 10, y + s * 74 / 100, cx + s / 16, y + s * 92 / 100, col);
    }
  } else if (strcmp(kind, "fog") == 0) {
    for (int i = 0; i < 3; i++) {
      int yy = y + s * (32 + i * 20) / 100;
      int inset = (i % 2) * s / 12;
      line(c, x + s / 6 + inset, yy, x + s * 5 / 6 - inset, yy, col);
    }
  }
}

static const char *condition(int code) {
  if (code < 0) return "";
  if (code == 0) return "Clear";
  if (code <= 2) return "Partly cloudy";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Fog";
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "Rain";
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "Snow";
  if (code >= 95 && code <= 99) return "Thunder";
  return "";
}

static const char *wx_kind(int code, int hour) {
  if (code < 0) return NULL;
  if (code == 0) return (hour < 6 || hour >= 18) ? "moon" : "clear";
  if (code <= 3) return "cloud";
  if (code == 45 || code == 48) return "fog";
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82) || (code >= 95 && code <= 99)) return "rain";
  return NULL;
}

static int parse_weather(const char *buf, Weather *w) {
  if (!json_document(buf)) return -1;
  JsonValue root = {json_space(buf), buf + strlen(buf)};
  JsonValue current = json_member(root, "current");
  JsonValue daily = json_member(root, "daily");
  double temp, code;
  if (!json_number(json_member(current, "temperature_2m"), -100, 100, &temp) ||
      !json_number(json_member(current, "weather_code"), 0, 99, &code) || code != floor(code)) return -1;
  JsonValue dates[4], highs[4], lows[4];
  int count = json_elements(json_member(daily, "time"), dates, 4);
  if (count < 1 || json_elements(json_member(daily, "temperature_2m_max"), highs, 4) != count ||
      json_elements(json_member(daily, "temperature_2m_min"), lows, 4) != count) return -1;
  Weather next = {0};
  next.temp = (int)lround(temp);
  next.code = (int)code;
  next.n = count;
  for (int i = 0; i < count; i++) {
    double high, low;
    int y, m, d, consumed = 0;
    if (dates[i].end - dates[i].start != 12 ||
        sscanf(dates[i].start, "\"%4d-%2d-%2d\"%n", &y, &m, &d, &consumed) != 3 || consumed != 12 ||
        y < 1 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31 ||
        !json_number(highs[i], -100, 100, &high) || !json_number(lows[i], -100, 100, &low)) return -1;
    memcpy(next.date[i], dates[i].start + 1, 10);
    next.date[i][10] = 0;
    next.high[i] = (int)lround(high);
    next.low[i] = (int)lround(low);
  }
  next.ok = 1;
  *w = next;
  return 0;
}

static int load_weather(Weather *w) {
  if (!curl_open(&g_weather) || !curl_fetch(g_weather, WEATHER_URL) || parse_weather(g_body.data, w)) {
    logmsg("weather fetch failed; retaining last good weather");
    return -1;
  }
  return 0;
}

static int cpu_temp(void) {
  DIR *dir = opendir("/sys/class/hwmon");
  if (!dir) return -1;
  int best = -1;
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if (ent->d_name[0] == '.') continue;
    char namep[320], name[64];
    snprintf(namep, sizeof namep, "/sys/class/hwmon/%s/name", ent->d_name);
    FILE *nf = fopen(namep, "r");
    if (!nf) continue;
    if (!fgets(name, sizeof name, nf)) {
      fclose(nf);
      continue;
    }
    fclose(nf);
    if (strncmp(name, "coretemp", 8) != 0) continue;
    for (int i = 1; i <= 8; i++) {
      char labp[340], inp[340], label[64];
      snprintf(labp, sizeof labp, "/sys/class/hwmon/%s/temp%d_label", ent->d_name, i);
      snprintf(inp, sizeof inp, "/sys/class/hwmon/%s/temp%d_input", ent->d_name, i);
      FILE *lf = fopen(labp, "r");
      FILE *vf = fopen(inp, "r");
      if (!lf || !vf) {
        if (lf) fclose(lf);
        if (vf) fclose(vf);
        continue;
      }
      if (!fgets(label, sizeof label, lf)) label[0] = 0;
      int milli = 0;
      if (fscanf(vf, "%d", &milli) == 1) {
        int c = (milli + 500) / 1000;
        if (strstr(label, "Package")) {
          fclose(lf);
          fclose(vf);
          closedir(dir);
          return c;
        }
        if (c > best) best = c;
      }
      fclose(lf);
      fclose(vf);
    }
  }
  closedir(dir);
  return best;
}

static void memory_gb(double *used, double *total) {
  FILE *f = fopen("/proc/meminfo", "r");
  long long total_kb = 0, avail_kb = 0;
  char key[64];
  long long val = 0;
  if (!f) {
    *used = 0;
    *total = 0;
    return;
  }
  while (fscanf(f, "%63s %lld kB\n", key, &val) == 2) {
    if (strcmp(key, "MemTotal:") == 0) total_kb = val;
    else if (strcmp(key, "MemAvailable:") == 0) avail_kb = val;
  }
  fclose(f);
  *total = total_kb / 1024.0 / 1024.0;
  *used = (total_kb - avail_kb) / 1024.0 / 1024.0;
}

static double load_1(void) {
  double v = 0;
  FILE *f = fopen("/proc/loadavg", "r");
  if (f) {
    if (fscanf(f, "%lf", &v) != 1) v = 0;
    fclose(f);
  }
  return v;
}

static void sample_system(void) {
  g_system.cpu = cpu_temp();
  g_system.load = load_1();
  memory_gb(&g_system.used, &g_system.total);
}

static size_t g_body_limit_override;

static size_t curl_write(char *p, size_t s, size_t n, void *u) {
  Body *b = u;
  size_t limit = g_body_limit_override ? g_body_limit_override : g_ambient ? 2048 : g_minimal ? 16384 : BODY_LIMIT;
  if (!s || n > limit / s) return 0;
  size_t bytes = s * n;
  if (b->n > limit || bytes > limit - b->n) return 0;
  if (b->n + bytes + 1 > b->cap) {
    size_t c = b->cap ? b->cap * 2 : 8192;
    while (c < b->n + bytes + 1) c *= 2;
    size_t max_cap = limit + 1;
    if (c > max_cap) c = max_cap;
    char *nd = realloc(b->data, c);
    if (!nd) return 0;
    b->data = nd;
    b->cap = c;
  }
  memcpy(b->data + b->n, p, bytes);
  b->n += bytes;
  b->data[b->n] = 0;
  return bytes;
}

static int curl_open(CURL **out) {
  if (*out) return 1;
  CURL *c = curl_easy_init();
  if (!c) return 0;
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &g_body);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "aviato-screen");
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(c, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
  curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  *out = c;
  return 1;
}

static int curl_fetch(CURL *c, const char *url) {
  g_body.n = 0;
  if (g_body.data) g_body.data[0] = 0;
  curl_easy_setopt(c, CURLOPT_URL, url);
  if (curl_easy_perform(c) != 0 || g_body.n == 0 || !g_body.data) return 0;
  if (!json_document(g_body.data)) return 0;
  return 1;
}

/* Cover images are bytes, not JSON, and larger than a quote response. */
static int curl_fetch_raw(CURL *c, const char *url, size_t limit) {
  g_body_limit_override = limit;
  g_body.n = 0;
  if (g_body.data) g_body.data[0] = 0;
  curl_easy_setopt(c, CURLOPT_URL, url);
  int ok = curl_easy_perform(c) == 0 && g_body.n > 2 && g_body.data;
  g_body_limit_override = 0;
  return ok;
}

static void note_fail(const char *what) {
  double now = mono();
  if (now - g_fail_log < 60) return;
  g_fail_log = now;
  char msg[64];
  snprintf(msg, sizeof msg, "%s fetch failed", what);
  logmsg(msg);
}

static int json_double_span(const char *start, const char *end, const char *key, double *out) {
  size_t klen = strlen(key);
  const char *p = start;
  while (p && p < end) {
    const char *hit = strstr(p, key);
    if (!hit || hit >= end) return 0;
    const char *q = hit + klen;
    if (q < end && *q == '_') {
      p = q;
      continue;
    }
    while (q < end && (*q == ' ' || *q == ':' || *q == '"')) q++;
    char *e = NULL;
    double v = strtod(q, &e);
    if (e != q && e <= end && isfinite(v)) {
      *out = v;
      return 1;
    }
    p = hit + klen;
  }
  return 0;
}

static int take_price(Quote *q, double v, double lo, double hi) {
  if (!(v >= lo && v <= hi)) return 0;
  q->value = v;
  q->ok = 1;
  return 1;
}

static int fetch_coins(void) {
  if (!curl_open(&g_coin)) {
    note_fail("btc");
    return 0;
  }
  const char *url =
      "https://api.binance.com/api/v3/ticker/24hr?symbols=%5B%22BTCUSDT%22%2C%22ETHUSDT%22%5D";
  if (!curl_fetch(g_coin, url)) {
    note_fail("btc");
    return 0;
  }
  const char *btc = strstr(g_body.data, "BTCUSDT");
  const char *eth = strstr(g_body.data, "ETHUSDT");
  const char *end = g_body.data + g_body.n;
  if (!btc || !eth || eth < btc) {
    note_fail("btc");
    return 0;
  }
  double px = 0, pct = 0;
  Markets next = g_markets;
  if (!json_double_span(btc, eth, "\"lastPrice\"", &px) || !take_price(&next.btc, px, 1000, 10000000)) return 0;
  if (json_double_span(btc, eth, "\"priceChangePercent\"", &pct) && pct > -80 && pct < 80) {
    next.btc.change = pct;
    next.btc.has_change = next.btc.ok;
  }
  if (!json_double_span(eth, end, "\"lastPrice\"", &px) || !take_price(&next.eth, px, 10, 100000)) return 0;
  if (json_double_span(eth, end, "\"priceChangePercent\"", &pct) && pct > -80 && pct < 80) {
    next.eth.change = pct;
    next.eth.has_change = next.eth.ok;
  }
  g_markets = next;
  return 1;
}

static int fetch_metal(const char *primary, const char *fallback, Quote *q, double lo, double hi) {
  double px = 0;
  if (curl_fetch(g_metal, primary) && json_double_span(g_body.data, g_body.data + g_body.n, "\"price\"", &px) &&
      take_price(q, px, lo, hi))
    return 1;
  if (curl_fetch(g_metal, fallback) &&
      json_double_span(g_body.data, g_body.data + g_body.n, "\"regularMarketPrice\"", &px) &&
      take_price(q, px, lo, hi))
    return 1;
  return 0;
}

static int fetch_metals(void) {
  if (!curl_open(&g_metal)) {
    note_fail("metal");
    return 0;
  }
  int gold = fetch_metal("https://api.gold-api.com/price/XAU",
                         "https://query1.finance.yahoo.com/v8/finance/chart/GC=F?interval=1d&range=1d",
                         &g_markets.gold, 500, 20000);
  int silver = fetch_metal("https://api.gold-api.com/price/XAG",
                           "https://query1.finance.yahoo.com/v8/finance/chart/SI=F?interval=1d&range=1d",
                           &g_markets.silver, 1, 1000);
  if (!gold || !silver) note_fail("metal");
  return gold && silver;
}

static void refresh_prices(int theme, int force) {
  if (theme == 0) return;
  if (force || mono() >= g_coin_refresh.next) {
    int ok = fetch_coins();
    refresh_done(&g_coin_refresh, mono(), ok);
  }
  if (force || mono() >= g_metal_refresh.next) {
    int ok = fetch_metals();
    refresh_done(&g_metal_refresh, mono(), ok);
  }
}

static void fmt_grouped(char *out, size_t n, long long ip) {
  char raw[32];
  snprintf(raw, sizeof raw, "%lld", ip < 0 ? -ip : ip);
  int len = (int)strlen(raw);
  char tmp[40];
  int ti = 0;
  if (ip < 0 && ti + 1 < (int)sizeof tmp) tmp[ti++] = '-';
  int first = len % 3;
  if (first == 0) first = 3;
  for (int i = 0; i < len && ti + 2 < (int)sizeof tmp; i++) {
    if (i && i >= first && (i - first) % 3 == 0) tmp[ti++] = ',';
    tmp[ti++] = raw[i];
  }
  tmp[ti] = 0;
  snprintf(out, n, "%s", tmp);
}

static void fmt_quote(char *out, size_t n, double v, int decimals) {
  if (decimals <= 0) {
    fmt_grouped(out, n, (long long)llround(v));
    return;
  }
  long long scale = 1;
  for (int i = 0; i < decimals; i++) scale *= 10;
  long long scaled = (long long)llround(v * (double)scale);
  if (scaled < 0) scaled = -scaled;
  long long ip = scaled / scale;
  int frac = (int)(scaled % scale);
  char whole[32];
  fmt_grouped(whole, sizeof whole, ip);
  snprintf(out, n, "%s.%0*d", whole, decimals, frac);
}

static int ymd_of(const struct tm *tm, char *out, size_t n) {
  return (int)strftime(out, n, "%Y-%m-%d", tm);
}

static int weekday_of(const char *ymd) {
  int y, m, d;
  if (sscanf(ymd, "%d-%d-%d", &y, &m, &d) != 3) return -1;
  struct tm tm;
  memset(&tm, 0, sizeof tm);
  tm.tm_year = y - 1900;
  tm.tm_mon = m - 1;
  tm.tm_mday = d;
  time_t sec = timegm(&tm);
  struct tm out;
  gmtime_r(&sec, &out);
  return out.tm_wday;
}

static void draw_clock(Canvas *c, int x, int y, const struct tm *tm, int theme) {
  char stamp[8], utc[16], date[48];
  Color fg = fg_of(theme), dim = dim_of(theme);
  snprintf(stamp, sizeof stamp, "%02d:%02d", tm->tm_hour, tm->tm_min);
  time_t real = time(NULL);
  struct tm ug;
  gmtime_r(&real, &ug);
  snprintf(utc, sizeof utc, "UTC %02d:%02d", ug.tm_hour, ug.tm_min);
  snprintf(date, sizeof date, "%s, %d %s", WDAY[tm->tm_wday], tm->tm_mday, MON[tm->tm_mon]);
  int s_time = H * 10 / 100;
  int s_utc = H * 28 / 1000;
  int s_date = H * 26 / 1000;
  draw_text(c, x, y, stamp, s_time, fg, 0);
  y += font_ascent(s_time) + H * 12 / 1000;
  draw_text(c, x, y, utc, s_utc, dim, 2);
  y += font_ascent(s_utc) + H * 16 / 1000;
  draw_text(c, x, y, date, s_date, dim, 0);
}

static void draw_weather(Canvas *c, int right, int y, const struct tm *tm, const Weather *w, int theme) {
  Color fg = fg_of(theme), dim = dim_of(theme), bg = bg_of(theme);
  int s_temp = H * 45 / 1000;
  int s_body = H * 24 / 1000;
  int s_small = H * 20 / 1000;
  char temp[16];
  if (w->ok) snprintf(temp, sizeof temp, "%d\u00b0", w->temp);
  else snprintf(temp, sizeof temp, "\u2013");
  const char *kind = w->ok ? wx_kind(w->code, tm->tm_hour) : NULL;
  int icon_s = H * 34 / 1000;
  int block = text_width(temp, s_temp, 0) + (kind ? icon_s + 12 : 0);
  int x = right - block;
  if (kind) {
    icon(c, x, y + 6, icon_s, kind, fg, bg);
    x += icon_s + 12;
  }
  draw_text(c, x, y, temp, s_temp, fg, 0);
  y += font_ascent(s_temp) + 8;
  if (w->ok) {
    const char *cond = condition(w->code);
    if (cond[0]) {
      draw_text(c, right - text_width(cond, s_body, 0), y, cond, s_body, dim, 0);
      y += font_ascent(s_body) + 8;
    }
  }
  char today[16];
  ymd_of(tm, today, sizeof today);
  char detail[64] = "Kanpur";
  for (int i = 0; i < w->n; i++) {
    if (strcmp(w->date[i], today) == 0) {
      snprintf(detail, sizeof detail, "Kanpur, today %d\u00b0 to %d\u00b0", w->low[i], w->high[i]);
      break;
    }
  }
  draw_text(c, right - text_width(detail, s_small, 0), y, detail, s_small, dim, 0);
  y += font_ascent(s_small) + 10;
  char forecast[96] = "";
  int count = 0;
  for (int i = 0; i < w->n && count < 3; i++) {
    if (strcmp(w->date[i], today) <= 0) continue;
    int wd = weekday_of(w->date[i]);
    if (wd < 0) continue;
    char bit[24];
    snprintf(bit, sizeof bit, "%s %d\u00b0 %d\u00b0", WSHORT[wd], w->high[i], w->low[i]);
    if (count) strncat(forecast, "   ", sizeof forecast - strlen(forecast) - 1);
    strncat(forecast, bit, sizeof forecast - strlen(forecast) - 1);
    count++;
  }
  if (forecast[0]) draw_text(c, right - text_width(forecast, s_small, 0), y, forecast, s_small, dim, 0);
}

static void system_text(char *line, size_t size) {
  int cpu = g_system.cpu;
  double load = g_system.load, used = g_system.used, total = g_system.total;
  if (cpu >= 0)
    snprintf(line, size, "cpu %d\u00b0      load %.2f      memory %.1f of %.0f GB", cpu, load, used, total);
  else
    snprintf(line, size, "load %.2f      memory %.1f of %.0f GB", load, used, total);
}

static void draw_cpu_line(Canvas *c, int y, int theme) {
  int s_small = H * 22 / 1000;
  char line[96];
  system_text(line, sizeof line);
  draw_text(c, (W - text_width(line, s_small, 1)) / 2, y, line, s_small, dim_of(theme), 1);
}

static int wrap_quote(const char *text, int size, int max_w, char out[][160], int max_lines) {
  int n = 0;
  const char *p = text;
  while (*p && n < max_lines) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char *best = p;
    const char *last_space = NULL;
    const char *q = p;
    while (*q) {
      if (*q == ' ') last_space = q;
      size_t len = (size_t)(q - p + 1);
      if (len >= 160) break;
      char tmp[160];
      memcpy(tmp, p, len);
      tmp[len] = 0;
      if (text_width(tmp, size, 0) > max_w) break;
      best = q;
      q++;
    }
    const char *end;
    if (*q == 0) end = q;
    else if (last_space && last_space > p) end = last_space;
    else end = best + 1;
    size_t len = (size_t)(end - p);
    if (len >= 160) len = 159;
    memcpy(out[n], p, len);
    out[n][len] = 0;
    while (len > 0 && out[n][len - 1] == ' ') out[n][--len] = 0;
    n++;
    p = end;
  }
  return n;
}

static void draw_mark(Canvas *c, int cx, int cy, int r, int theme) {
  Color quiet = theme == 2 ? (Color){28, 28, 30} : (Color){38, 40, 44};
  Color tick = dim_of(theme);
  ring(c, cx, cy, r, quiet);
  ring(c, cx, cy, r - 1, quiet);
  for (int dx = 0; dx <= 1; dx++) {
    vhair(c, cx + dx, cy - r - 18, cy - r + 8, tick);
    vhair(c, cx + dx, cy + r - 8, cy + r + 18, tick);
  }
}

static void draw_quote(Canvas *c, const struct tm *tm, int theme, int below, int above) {
  int slot = saying_slot(tm);
  const char *text = SAYINGS[slot].text;
  const char *who = SAYINGS[slot].author;
  int max_w = W * 72 / 100;
  char lines[4][160];
  int size = H * 56 / 1000;
  int n = 1;
  while (size > 34) {
    n = wrap_quote(text, size, max_w, lines, 4);
    if (n > 0 && n <= 3) break;
    size -= 2;
  }
  if (n < 1) return;
  int s_who = H * 20 / 1000;
  if (s_who < 16) s_who = 16;
  int line_h = font_ascent(size) + H * 14 / 1000;
  int text_h = n * line_h - H * 14 / 1000;
  if (text_h < font_ascent(size)) text_h = font_ascent(size);
  int top_lim = below + H * 3 / 100;
  int bot_lim = above - H * 3 / 100;
  int y = H / 2 - text_h / 2;
  int name_gap = H * 22 / 1000;
  int block = text_h + name_gap + font_ascent(s_who);
  if (y < top_lim) y = top_lim;
  if (y + block > bot_lim) y = bot_lim - block;
  int cy = y + text_h / 2;
  int r = 190 + n * 36;
  if (r > 290) r = 290;
  if (cy - r < below + 10) r = cy - below - 10;
  if (cy + r > above - 12) r = above - 12 - cy;
  if (r > 80) draw_mark(c, W / 2, cy, r, theme);
  Color fg = fg_of(theme), dim = dim_of(theme);
  for (int i = 0; i < n; i++) {
    int tw = text_width(lines[i], size, 0);
    draw_text(c, (W - tw) / 2, y + i * line_h, lines[i], size, fg, 0);
  }
  int name_y = y + n * line_h + H * 22 / 1000;
  int nw = text_width(who, s_who, 3);
  int nx = (W - nw) / 2;
  int rule_y = name_y + font_ascent(s_who) / 2;
  hair(c, nx - 72, nx - 16, rule_y, dim);
  hair(c, nx + nw + 16, nx + nw + 72, rule_y, dim);
  draw_text(c, nx, name_y, who, s_who, dim, 3);
}

static Rect draw_quote_tracked(Canvas *c, const struct tm *tm, int theme, int head, int price_y) {
  Rect bounds = {c->w, c->h, 0, 0};
  Rect *previous = c->damage;
  c->damage = &bounds;
  draw_quote(c, tm, theme, head, price_y);
  c->damage = previous;
  return bounds;
}

static void quote_layout_metrics(int *head, int *price_y) {
  int pad_y = H * 6 / 100;
  int s_cpu = H * 22 / 1000;
  int cpu_y = H - H * 4 / 100 - font_ascent(s_cpu);
  int s_px = H * 26 / 1000;
  if (s_px < 22) s_px = 22;
  *price_y = cpu_y - H * 32 / 1000 - font_ascent(s_px);
  *head = pad_y + font_ascent(H * 10 / 100) + H * 12 / 1000 +
          font_ascent(H * 28 / 1000) + H * 16 / 1000 + font_ascent(H * 26 / 1000);
}

static void draw_index(Canvas *c, int cx, int y, const char *code, const Quote *q, int decimals, int theme) {
  int s_px = H * 26 / 1000;
  int s_code = H * 14 / 1000;
  if (s_px < 22) s_px = 22;
  if (s_code < 13) s_code = 13;
  Color fg = fg_of(theme), dim = dim_of(theme);
  char price[64];
  if (q->ok) fmt_quote(price, sizeof price, q->value, decimals);
  else snprintf(price, sizeof price, "\u2013");
  char chg[24];
  chg[0] = 0;
  Color chg_col = dim;
  if (q->ok && q->has_change) {
    double p = q->change;
    const char *sign = p > 0.005 ? "+" : (p < -0.005 ? "-" : "");
    snprintf(chg, sizeof chg, "%s%.2f%%", sign, fabs(p));
    chg_col = p < -0.005 ? down_of(theme) : (p > 0.005 ? up_of(theme) : dim);
  }
  int gap = 8;
  int pw = text_width(price, s_px, 0);
  int cw = text_width(code, s_code, 2);
  int hw = chg[0] ? text_width(chg, s_code, 0) : 0;
  int total = pw + gap + cw + (hw ? gap + hw : 0);
  int x = cx - total / 2;
  int code_drop = font_ascent(s_px) - font_ascent(s_code);
  if (code_drop < 0) code_drop = 0;
  draw_text(c, x, y, price, s_px, fg, 0);
  draw_text(c, x + pw + gap, y + code_drop, code, s_code, dim, 2);
  if (hw) draw_text(c, x + pw + gap + cw + gap, y + code_drop, chg, s_code, chg_col, 0);
}

static void draw_markets(Canvas *c, int y, int theme) {
  int col = W / 4;
  draw_index(c, col / 2, y, "BTC", &g_markets.btc, 0, theme);
  draw_index(c, col + col / 2, y, "ETH", &g_markets.eth, 0, theme);
  draw_index(c, col * 2 + col / 2, y, "GOLD", &g_markets.gold, 0, theme);
  draw_index(c, col * 3 + col / 2, y, "SILVER", &g_markets.silver, 2, theme);
}

static void draw_night(Canvas *c, const struct tm *tm, const Weather *w, int theme) {
  char stamp[8];
  snprintf(stamp, sizeof stamp, "%02d:%02d", tm->tm_hour, tm->tm_min);
  int size = H * 44 / 100;
  int cap = W * 26 / 100;
  if (size > cap) size = cap;
  while (size > 80 && text_width(stamp, size, 0) > W * 86 / 100) size -= 8;
  int asc = font_ascent(size);
  int x = (W - text_width(stamp, size, 0)) / 2;
  int y = (H - asc) / 2 - H * 3 / 100;
  draw_text(c, x, y, stamp, size, fg_of(theme), 0);
  if (w->ok) {
    char line[48];
    snprintf(line, sizeof line, "Kanpur outside %d\u00b0", w->temp);
    int small = H * 26 / 1000;
    if (small < 18) small = 18;
    int sw = text_width(line, small, 2);
    draw_text(c, (W - sw) / 2, y + asc + H * 4 / 100, line, small, dim_of(theme), 2);
  }
}

static void draw_portfolio(Canvas *c, int theme) {
  char line[384];
  portfolio_text(&g_portfolio,line,sizeof line,time(NULL),!g_shared_frame);
  if (!line[0]) return;
  int size=20;
  while (size>12 && text_width(line,size,0)>W*90/100) size--;
  draw_text(c,(W-text_width(line,size,0))/2,H*83/100,line,size,dim_of(theme),0);
}

static void paint(Canvas *c, const struct tm *tm, const Weather *w) {
  if (g_minimal) { paint_minimal(c, tm, w); return; }
  if (g_ambient) { paint_ambient(c, tm); return; }
  int theme = theme_of(tm);
  fill(c, bg_of(theme));
  if (theme == 0) {
    draw_night(c, tm, w, theme);
    return;
  }
  int pad_x = W * 7 / 100;
  int pad_y = H * 6 / 100;
  draw_clock(c, pad_x, pad_y, tm, theme);
  draw_weather(c, W - pad_x, pad_y, tm, w, theme);
  int head, price_y;
  quote_layout_metrics(&head, &price_y);
  int cpu_y = H - H * 4 / 100 - font_ascent(H * 22 / 1000);
  draw_quote(c, tm, theme, head, price_y);
  draw_markets(c, price_y, theme);
  draw_cpu_line(c, cpu_y, theme);
  draw_portfolio(c, theme);
}

enum { PART_CLOCK, PART_WEATHER, PART_MARKETS, PART_SYSTEM, PART_PORTFOLIO, PART_NIGHT, PART_COUNT };

typedef struct {
  int initialized;
  int adaptive_mode;
  struct tm tm;
  Weather weather;
  char markets[384], system[96];
  char portfolio[384];
  Rect bounds[PART_COUNT];
  Rect quote_bounds;
} RenderState;

static void clear_rect(Canvas *c, Rect r, Color color) {
  if (r.x0 < 0) r.x0 = 0;
  if (r.y0 < 0) r.y0 = 0;
  if (r.x1 > c->w) r.x1 = c->w;
  if (r.y1 > c->h) r.y1 = c->h;
  if (r.x0 >= r.x1 || r.y0 >= r.y1) return;
  for (int y = r.y0; y < r.y1; y++) {
    unsigned char *p = c->pix + (size_t)y * c->pitch + r.x0 * 4;
    for (int x = r.x0; x < r.x1; x++, p += 4) {
      p[0] = color.b; p[1] = color.g; p[2] = color.r; p[3] = 0;
    }
  }
}

#include "ambient-screen.h"
#include "minimal-screen.h"

static void market_text(char *out, size_t size) {
  const Quote *quotes[] = {&g_markets.btc, &g_markets.eth, &g_markets.gold, &g_markets.silver};
  size_t used = 0;
  for (int i = 0; i < 4; i++) {
    char price[64] = "unavailable", change[32] = "";
    if (quotes[i]->ok) {
      fmt_quote(price, sizeof price, quotes[i]->value, i == 3 ? 2 : 0);
      if (quotes[i]->has_change) {
        double p = quotes[i]->change;
        snprintf(change, sizeof change, "%s%.2f%%", p > 0.005 ? "+" : p < -0.005 ? "-" : "", fabs(p));
      }
    }
    int n = snprintf(out + used, size - used, "%s/%s;", price, change);
    if (n < 0 || (size_t)n >= size - used) break;
    used += (size_t)n;
  }
}

static void render_part(Canvas *c, RenderState *state, int part, const struct tm *tm, const Weather *w, int clear) {
  int theme = theme_of(tm);
  if (clear) clear_rect(c, state->bounds[part], bg_of(theme));
  Rect bounds = {c->w, c->h, 0, 0};
  c->damage = &bounds;
  int cpu_y = H - H * 4 / 100 - font_ascent(H * 22 / 1000);
  int price_y = cpu_y - H * 32 / 1000 - font_ascent(H * 26 / 1000);
  switch (part) {
    case PART_CLOCK: draw_clock(c, W * 7 / 100, H * 6 / 100, tm, theme); break;
    case PART_WEATHER: draw_weather(c, W - W * 7 / 100, H * 6 / 100, tm, w, theme); break;
    case PART_MARKETS: draw_markets(c, price_y, theme); break;
    case PART_SYSTEM: draw_cpu_line(c, cpu_y, theme); break;
    case PART_PORTFOLIO: draw_portfolio(c, theme); break;
    case PART_NIGHT: draw_night(c, tm, w, theme); break;
  }
  c->damage = NULL;
  state->bounds[part] = bounds;
}

/* Bounds are measured from actual pixels, so shorter replacement strings clear
 * their old contents without repainting unrelated parts. */
static int render_incremental(Canvas *c, RenderState *state, const struct tm *tm, const Weather *w) {
  if (g_minimal) return render_minimal(c, state, tm, w);
  if (g_ambient) return render_ambient(c, state, tm);
  int theme = theme_of(tm);
  int full = !state->initialized || theme != theme_of(&state->tm);
  int quote_changed = theme != 0 && saying_slot(tm) != saying_slot(&state->tm);
  int clock_changed = full || tm->tm_year != state->tm.tm_year || tm->tm_yday != state->tm.tm_yday ||
                      tm->tm_mon != state->tm.tm_mon || tm->tm_mday != state->tm.tm_mday ||
                      tm->tm_hour != state->tm.tm_hour || tm->tm_min != state->tm.tm_min;
  int weather_changed = full || memcmp(w, &state->weather, sizeof *w) ||
                        tm->tm_mday != state->tm.tm_mday || tm->tm_hour != state->tm.tm_hour;
  char markets[384] = "", system[96] = "", portfolio[384] = "";
  market_text(markets, sizeof markets);
  system_text(system, sizeof system);
  portfolio_text(&g_portfolio,portfolio,sizeof portfolio,time(NULL),!g_shared_frame);
  int markets_changed = full || strcmp(markets, state->markets);
  int system_changed = full || strcmp(system, state->system);
  int portfolio_changed = full || strcmp(portfolio, state->portfolio);
  int changed = 0;
  if (full) fill(c, bg_of(theme));
  if (theme == 0) {
    if (clock_changed || weather_changed) {
      render_part(c, state, PART_NIGHT, tm, w, !full);
      changed = 1;
    }
  } else {
    const int dirty[] = {clock_changed, weather_changed, markets_changed, system_changed, portfolio_changed};
    for (int part = PART_CLOCK; part <= PART_PORTFOLIO; part++) {
      if (dirty[part]) {
        render_part(c, state, part, tm, w, !full);
        changed = 1;
      }
    }
    if (full) {
      int head, price_y;
      quote_layout_metrics(&head, &price_y);
      state->quote_bounds = draw_quote_tracked(c, tm, theme, head, price_y);
    } else if (quote_changed) {
      int head, price_y;
      quote_layout_metrics(&head, &price_y);
      clear_rect(c, state->quote_bounds, bg_of(theme));
      state->quote_bounds = draw_quote_tracked(c, tm, theme, head, price_y);
      changed = 1;
    }
  }
  state->initialized = 1;
  state->tm = *tm;
  state->weather = *w;
  memcpy(state->markets, markets, sizeof markets);
  memcpy(state->system, system, sizeof system);
  memcpy(state->portfolio, portfolio, sizeof portfolio);
  return changed;
}

/* Caller owns the stream, allowing secure atomic runtime publication. */
static int write_bmp_stream(FILE *f, Canvas *c) {
  int row = c->w * 3;
  int pad = (4 - (row % 4)) % 4;
  int stride = row + pad;
  int pixels = stride * c->h;
  unsigned char hdr[54];
  memset(hdr, 0, sizeof hdr);
  hdr[0] = 'B';
  hdr[1] = 'M';
  unsigned size = 54 + pixels;
  memcpy(hdr + 2, &size, 4);
  unsigned off = 54;
  memcpy(hdr + 10, &off, 4);
  unsigned dib = 40;
  memcpy(hdr + 14, &dib, 4);
  memcpy(hdr + 18, &c->w, 4);
  int h = -c->h; /* top-down */
  memcpy(hdr + 22, &h, 4);
  unsigned short planes = 1, bpp = 24;
  memcpy(hdr + 26, &planes, 2);
  memcpy(hdr + 28, &bpp, 2);
  memcpy(hdr + 34, &pixels, 4);
  fwrite(hdr, 1, 54, f);
  unsigned char *scan = calloc((size_t)stride, 1);
  if (!scan) {
    return -1;
  }
  for (int y = 0; y < c->h; y++) {
    unsigned char *src = c->pix + (size_t)y * c->pitch;
    for (int x = 0; x < c->w; x++) {
      scan[x * 3 + 0] = src[x * 4 + 0];
      scan[x * 3 + 1] = src[x * 4 + 1];
      scan[x * 3 + 2] = src[x * 4 + 2];
    }
    fwrite(scan, 1, (size_t)stride, f);
  }
  free(scan);
  return ferror(f) ? -1 : 0;
}

static int write_bmp(const char *path, Canvas *c) {
  FILE *f=fopen(path,"wb");
  if (!f) return -1;
  int result=write_bmp_stream(f,c);
  if (fclose(f)) result=-1;
  return result;
}

#include "display-handover.h"

static int load_face(const char *path, unsigned char **bytes, stbtt_fontinfo *info) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long n = ftell(f);
  if (n < 4 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
  unsigned char *data = malloc((size_t)n);
  if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) {
    free(data);
    fclose(f);
    return -1;
  }
  fclose(f);
  int off = stbtt_GetFontOffsetForIndex(data, 0);
  if (!stbtt_InitFont(info, data, off)) { free(data); return -1; }
  *bytes = data;
  return 0;
}

static int open_font(void) {
  if (load_face(FONT_PATH, &g_ttf, &g_font) != 0) return -1;
  /* Quotes use bold. A missing bold file keeps the regular face rather than blanking the screen. */
  g_bold_ready = load_face(FONT_BOLD_PATH, &g_ttf_bold, &g_font_bold) == 0;
  return 0;
}

static int open_fb(Canvas *c) {
  FILE *vf = fopen("/sys/class/graphics/fb0/virtual_size", "r");
  FILE *sf = fopen("/sys/class/graphics/fb0/stride", "r");
  if (!vf || !sf) return -1;
  if (fscanf(vf, "%d,%d", &c->w, &c->h) != 2) {
    fclose(vf);
    fclose(sf);
    return -1;
  }
  if (fscanf(sf, "%d", &c->pitch) != 1) {
    fclose(vf);
    fclose(sf);
    return -1;
  }
  fclose(vf);
  fclose(sf);
  c->fd = open("/dev/fb0", O_RDWR);
  if (c->fd < 0) return -1;
  size_t bytes = (size_t)c->pitch * (size_t)c->h;
  void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 0);
  if (p == MAP_FAILED) return -1;
  c->pix = p;
  char msg[80];
  snprintf(msg, sizeof msg, "output /dev/fb0 %dx%d pitch %d", c->w, c->h, c->pitch);
  logmsg(msg);
  return 0;
}

static double next_delay(const struct tm *tm, double now) {
  double delay = 60 - tm->tm_sec;
  if (g_minimal) {
    if (!minimal_active(tm)) return delay;
    delay = fmin(delay, g_coin_refresh.next - now);
    delay = fmin(delay, g_weather_refresh.next - now);
    delay = fmin(delay, g_system_refresh.next - now);
    delay = fmin(delay, g_spotify_refresh.next - now);
    /* Move only the progress strip, once a second, while the panel is on and
     * this process owns the framebuffer. A blank panel or a shared screensaver
     * frame must not wake the loop for a bar nobody is watching. */
    if (minimal_active(tm) && g_profile == PROFILE_ADAPTIVE && !g_shared_frame &&
        g_now.playing && g_now.duration_ms > 0 && display_panel_on()) {
      /* The bar moves about once a second. The lyric line wakes on its own
       * timestamp, which is often sooner, so the words are not a second late. */
      delay = fmin(delay, 1.0);
      delay = fmin(delay, spotify_lyric_wake());
    }
    if (g_profile != PROFILE_ADAPTIVE && g_agent.present && g_agent.expires_at > time(NULL))
      delay = fmin(delay, (double)(g_agent.expires_at - time(NULL)));
    if (g_face_deadline > 0 && agent_current_state() == AGENT_RUNNING)
      delay = fmin(delay, g_face_deadline - now);
    if (g_profile == PROFILE_ADAPTIVE)
      delay = fmin(delay, quote_wake(tm, now));
    return fmax(0.1, delay);
  }
  if (g_ambient) {
    /* Wake on the clock minute or the next price fetch. No 10-second poll. */
    double coin = g_coin_refresh.next - now;
    if (coin < delay) delay = coin;
    if (g_agent.present && g_agent.expires_at > time(NULL))
      delay = fmin(delay, (double)(g_agent.expires_at - time(NULL)));
    return fmax(0.1, delay);
  }
  delay = fmin(delay, g_weather_refresh.next - now);
  if (theme_of(tm) != 0) {
    delay = fmin(delay, g_coin_refresh.next - now);
    delay = fmin(delay, g_metal_refresh.next - now);
    delay = fmin(delay, g_system_refresh.next - now);
  }
  return fmax(0.1, delay);
}

static double frame_delay(const struct tm *displayed, const struct tm *current, double now) {
  /* Rendering or a wall-clock correction may cross a minute. Catch up before
   * sleeping to the following minute, even when no network task is due. */
  if (displayed->tm_year != current->tm_year || displayed->tm_yday != current->tm_yday ||
      displayed->tm_hour != current->tm_hour || displayed->tm_min != current->tm_min) return 0;
  return next_delay(current, now);
}

static void sleep_for(double seconds) {
  double minimum = g_quote_fading ? 0.08 :
    g_minimal && g_face_deadline > 0 && agent_current_state() == AGENT_RUNNING ? 0.1 : 0.4;
  if (seconds < minimum) seconds = minimum;
  if (seconds > 60) seconds = 60;
  struct timespec ts;
  ts.tv_sec = (time_t)seconds;
  ts.tv_nsec = (long)((seconds - ts.tv_sec) * 1e9);
  sigset_t blocked, previous;
  sigemptyset(&blocked); sigaddset(&blocked,SIGUSR1); sigaddset(&blocked,SIGUSR2);
  sigprocmask(SIG_BLOCK,&blocked,&previous);
  if (!g_wake) pselect(0,NULL,NULL,NULL,&ts,&previous);
  g_wake=0;
  sigprocmask(SIG_SETMASK,&previous,NULL);
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  logmsg("aviato screen starting");
  configure_ambient();
  configure_refresh();
  if (g_ambient && !getenv("SCREEN_CRYPTO_INTERVAL")) {
    g_coin_refresh.interval = 300;
    g_coin_refresh.retry = 300;
  }
  if (open_font() != 0) {
    logmsg("font missing");
    return 1;
  }
  Weather weather;
  memset(&weather, 0, sizeof weather);
  weather.code = -1;
  const char *preview = getenv("SCREEN_PREVIEW");
  const char *offline = getenv("SCREEN_OFFLINE");
  int offline_preview = preview && *preview && offline && strcmp(offline, "1") == 0;
  struct tm tm = now_ist();
  int weather_ok = 0;
  if (!offline_preview && !g_ambient && !g_minimal) {
    weather_ok = load_weather(&weather) == 0;
    refresh_done(&g_weather_refresh, mono(), weather_ok);
    tm = now_ist();
    refresh_prices(theme_of(&tm), 1);
  }
  if (!offline_preview && !g_minimal) sample_system();
  if (!preview || !*preview) {
    sample_agent();
    g_agent_dirty = 0;
    if (g_minimal) collect_minimal(&tm, &weather);
    else if (g_ambient) refresh_ambient_btc();
    else sample_portfolio();
  }
  refresh_done(&g_system_refresh, mono(), 1);
  tm = now_ist();

  if (preview && *preview) {
    const char *cover = getenv("SCREEN_SPOTIFY_JPEG");
    if (cover && *cover && g_minimal) spotify_load_preview(cover);
    Canvas c = {.w = W, .h = H, .pitch = W * 4, .fd = -1};
    c.pix = calloc((size_t)c.pitch * c.h, 1);
    if (!c.pix) return 1;
    double t0 = mono();
    paint(&c, &tm, &weather);
    char msg[80];
    snprintf(msg, sizeof msg, "paint preview %.0fms", (mono() - t0) * 1000);
    logmsg(msg);
    if (write_bmp(preview, &c) != 0) return 1;
    logmsg(preview);
    return 0;
  }

  Canvas fb = {0};
  if (open_fb(&fb) != 0) {
    logmsg("fb0 open failed");
    return 1;
  }
  RenderState state = {0};
  Display display;
  char runtime[128];
  snprintf(runtime,sizeof runtime,"/run/user/%lu/aviato-screen",(unsigned long)getuid());
  if (display_open(&display,runtime)!=0) {
    logmsg("display handover initialization failed");
    return 1;
  }
  g_agent_dirty = 1;
  for (;;) {
    tm = now_ist();
    int theme = theme_of(&tm);
    if (g_agent_dirty) {
      g_agent_dirty = 0;
      sample_agent();
    }
    update_face_animation(mono());
    if (g_minimal) collect_minimal(&tm, &weather);
    if (!g_minimal && !g_ambient && mono() >= g_weather_refresh.next) {
      weather_ok = load_weather(&weather) == 0;
      refresh_done(&g_weather_refresh, mono(), weather_ok);
    }
    if (!g_minimal && !g_ambient) refresh_prices(theme, 0);
    if (g_ambient) refresh_ambient_btc();
    if (!g_minimal && (g_ambient || theme != 0) && mono() >= g_system_refresh.next) {
      sample_system();
      if (!g_ambient) sample_portfolio();
      refresh_done(&g_system_refresh, mono(), 1);
    }
    tm = now_ist();
    double t0 = mono();
    if (g_redraw) { g_redraw=0; state.initialized=0; }
    Canvas *target=display_begin(&display,&fb,&state);
    if (!target) { logmsg("display ownership unavailable"); sleep_for(5); continue; }
    int changed=render_incremental(target, &state, &tm, &weather);
    if (display_end(&display,target,changed)!=0) {
      state.initialized=0;
      logmsg("shared frame publication failed");
    }
    if (changed) {
      char msg[80];
      const char *name = g_profile == PROFILE_ADAPTIVE ? "adaptive" : g_minimal ? "minimal" : g_ambient ? "ambient" : theme == 0 ? "nc" : theme == 1 ? "day" : "night";
      snprintf(msg, sizeof msg, "paint %s %02d:%02d %.0fms", name, tm.tm_hour, tm.tm_min, (mono() - t0) * 1000);
      logmsg(msg);
    }
    struct tm current = now_ist();
    double delay = frame_delay(&state.tm, &current, mono());
    if (delay > 0) sleep_for(delay);
  }
}
