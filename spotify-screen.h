#ifndef AVIATO_SPOTIFY_SCREEN_H
#define AVIATO_SPOTIFY_SCREEN_H

/* Decode a changed cover once, retain its small RGB copy in memory, and keep a
 * bounded private copy on disk so a restart does not redownload the same art. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_MAX_DIMENSIONS 1600
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"
#pragma GCC diagnostic pop

#define NOW_ART 260
#define ART_RES 640

typedef struct {
  int playing;
  int art_ok; /* 1 ready, 0 needs a fetch, -1 no cover */
  int progress_ms;
  int duration_ms;
  double progress_at; /* mono() when progress_ms was sampled */
  char title[160];
  char artist[160];
  char album[160];
  char art_url[512];
  unsigned char art[ART_RES * ART_RES * 3];
} NowPlaying;

/* One line of synced lyrics. The table is replaced only when a poll brings a
 * new song, then the panel walks it from the local playhead. */
#define LYRIC_MAX 80
#define LYRIC_CHARS 96
typedef struct {
  int at_ms;
  char text[LYRIC_CHARS];
} LyricLine;

static LyricLine g_lyrics[LYRIC_MAX];
static int g_lyric_n;
static char g_lyric_raw[12288];

static NowPlaying g_now;
static CURL *g_spotify;
static CURL *g_spotify_art;
static Refresh g_spotify_refresh = {120, 0, 120};
static long g_spotify_http;
static long g_spotify_retry_after;
static int g_spotify_stale;
static Color g_spotify_accent = {212, 196, 148};
static double g_art_retry_at;
static int g_art_failures;

#define SPOTIFY_ART_LIMIT (512u * 1024u)

typedef struct {
  char magic[8];
  uint32_t url_size;
  uint32_t body_size;
} SpotifyCacheHeader;

static int json_copy_string(JsonValue value, char *out, size_t n) {
  if (!value.start || *value.start != '"' || value.end - value.start < 2 || n < 2) return 0;
  const char *p = value.start + 1, *end = value.end - 1;
  size_t i = 0;
  while (p < end) {
    unsigned char ch = (unsigned char)*p++;
    if (ch == '\\') {
      if (p >= end) return 0;
      char esc = *p++;
      if (esc == '"' || esc == '\\' || esc == '/') ch = (unsigned char)esc;
      else if (esc == 'n') ch = '\n';
      else if (esc == 'u') {
        if (p + 4 > end) return 0;
        int cp = 0;
        for (int k = 0; k < 4; k++) {
          char h = *p++;
          cp <<= 4;
          if (h >= '0' && h <= '9') cp += h - '0';
          else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
          else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
          else return 0;
        }
        if (cp < 0x80) ch = (unsigned char)cp;
        else {
          unsigned char enc[3];
          int bytes = 0;
          if (cp < 0x800) {
            enc[0] = (unsigned char)(0xC0 | (cp >> 6));
            enc[1] = (unsigned char)(0x80 | (cp & 0x3F));
            bytes = 2;
          } else {
            enc[0] = (unsigned char)(0xE0 | (cp >> 12));
            enc[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            enc[2] = (unsigned char)(0x80 | (cp & 0x3F));
            bytes = 3;
          }
          if (i + (size_t)bytes >= n) break;
          memcpy(out + i, enc, (size_t)bytes);
          i += (size_t)bytes;
          continue;
        }
      } else return 0;
    }
    if (i + 1 >= n) break;
    out[i++] = (char)ch;
  }
  out[i] = 0;
  return 1;
}

static int spotify_art_host(const char *url);
static int spotify_cache_load(const char *url);

static void spotify_clear_lyrics(void) {
  g_lyric_n = 0;
}

/* [mm:ss.xx] or [mm:ss]. Tags such as [ar:] are not times and return 0. */
static int lyric_time_ms(const char *line, int *ms, const char **rest) {
  if (line[0] != '[') return 0;
  const char *p = line + 1;
  if (!isdigit((unsigned char)*p)) return 0;
  int minutes = 0;
  while (isdigit((unsigned char)*p)) {
    minutes = minutes * 10 + (*p - '0');
    if (minutes > 1440) return 0;
    p++;
  }
  if (*p++ != ':') return 0;
  if (!isdigit((unsigned char)*p)) return 0;
  int seconds = 0, digits = 0;
  while (isdigit((unsigned char)*p) && digits < 2) {
    seconds = seconds * 10 + (*p - '0');
    p++;
    digits++;
  }
  int fraction = 0, fraction_digits = 0;
  if (*p == '.') {
    p++;
    while (isdigit((unsigned char)*p) && fraction_digits < 3) {
      fraction = fraction * 10 + (*p - '0');
      p++;
      fraction_digits++;
    }
    while (isdigit((unsigned char)*p)) p++;
  }
  if (*p++ != ']') return 0;
  while (*p == ' ') p++;
  while (fraction_digits < 3) {
    fraction *= 10;
    fraction_digits++;
  }
  *ms = minutes * 60000 + seconds * 1000 + fraction;
  *rest = p;
  return 1;
}

/* LRCLIB text is one timestamped line per row. Blank rows are gaps, so the
 * previous line stays up instead of flashing empty. */
static void spotify_parse_lrc(const char *lrc) {
  g_lyric_n = 0;
  const char *p = lrc;
  while (*p && g_lyric_n < LYRIC_MAX) {
    char line[160];
    size_t n = 0;
    while (*p && *p != '\n' && n + 1 < sizeof line) line[n++] = *p++;
    line[n] = 0;
    if (*p == '\n') p++;
    int at_ms = 0;
    const char *text = line;
    if (!lyric_time_ms(line, &at_ms, &text) || !text[0]) continue;
    LyricLine *slot = &g_lyrics[g_lyric_n++];
    slot->at_ms = at_ms;
    snprintf(slot->text, sizeof slot->text, "%s", text);
  }
}

static void spotify_take_lyrics(JsonValue value) {
  if (!value.start || *value.start != '"' ||
      !json_copy_string(value, g_lyric_raw, sizeof g_lyric_raw)) {
    spotify_clear_lyrics();
    return;
  }
  spotify_parse_lrc(g_lyric_raw);
}

/* isLive false clears the song immediately. A transport failure does not. */
static int spotify_apply(const char *body) {
  if (!json_document(body)) return 0;
  JsonValue root = {json_space(body), body + strlen(body)};
  JsonValue live = json_member(root, "isLive");
  if (json_literal(live, "false")) {
    g_now.playing = 0;
    g_now.title[0] = 0;
    g_now.artist[0] = 0;
    g_now.album[0] = 0;
    g_now.art_url[0] = 0;
    g_now.art_ok = -1;
    g_now.progress_ms = 0;
    g_now.duration_ms = 0;
    g_now.progress_at = 0;
    spotify_clear_lyrics();
    g_art_failures = 0;
    g_art_retry_at = 0;
    return 1;
  }
  if (!json_literal(live, "true")) return 0;
  char title[160], artist[160], album[160], url[512];
  title[0] = artist[0] = album[0] = url[0] = 0;
  if (!json_copy_string(json_member(root, "title"), title, sizeof title) || !title[0]) return 0;
  if (!json_copy_string(json_member(root, "artist"), artist, sizeof artist)) artist[0] = 0;
  JsonValue album_value = json_member(root, "album");
  if (album_value.start && *album_value.start == '"' &&
      !json_copy_string(album_value, album, sizeof album)) return 0;
  JsonValue art = json_member(root, "albumArt");
  if (art.start && *art.start == '"') {
    if (!json_copy_string(art, url, sizeof url)) return 0;
  }
  if (url[0] && !spotify_art_host(url)) url[0] = 0;
  int same = strcmp(url, g_now.art_url) == 0;
  int track_changed = strcmp(title, g_now.title) || strcmp(artist, g_now.artist) ||
                      strcmp(album, g_now.album) || !same;
  snprintf(g_now.title, sizeof g_now.title, "%s", title);
  snprintf(g_now.artist, sizeof g_now.artist, "%s", artist);
  snprintf(g_now.album, sizeof g_now.album, "%s", album);
  g_now.playing = 1;
  /* Sample the bar from this response. Between polls the screen adds elapsed
   * time locally, so the next request can wait half a minute. */
  double progress = 0, duration = 0;
  if (!json_number(json_member(root, "progressMs"), 0, 86400000, &progress)) progress = 0;
  if (!json_number(json_member(root, "durationMs"), 0, 86400000, &duration)) duration = 0;
  g_now.progress_ms = (int)progress;
  g_now.duration_ms = (int)duration;
  g_now.progress_at = mono();
  /* Missing lyrics clear the previous song's lines. The field is optional. */
  spotify_take_lyrics(json_member(root, "syncedLyrics"));
  if (!same) {
    snprintf(g_now.art_url, sizeof g_now.art_url, "%s", url);
    g_now.art_ok = url[0] ? 0 : -1;
    g_art_failures = 0;
    g_art_retry_at = 0;
    if (url[0] && spotify_cache_load(url)) {
      g_art_failures = 0;
      g_art_retry_at = 0;
    }
  } else if (track_changed && url[0] && g_now.art_ok != 1) {
    /* If a new track reuses a cover URL, give its cached/failing image another
     * immediate chance without turning routine status polls into downloads. */
    g_now.art_ok = 0;
    g_art_failures = 0;
    g_art_retry_at = 0;
    if (spotify_cache_load(url)) {
      g_art_failures = 0;
      g_art_retry_at = 0;
    }
  }
  return 1;
}

static int spotify_art_host(const char *url) {
  size_t n = strlen(url);
  if (n < 20 || n > 480) return 0;
  if (strncmp(url, "https://", 8) != 0) return 0;
  const char *host = url + 8;
  const char *slash = strchr(host, '/');
  if (!slash) return 0;
  size_t hlen = (size_t)(slash - host);
  if (hlen < 4 || hlen > 128) return 0;

  int match = 0;
  if (hlen == 9 && !strncmp(host, "i.scdn.co", 9)) match = 1;
  else if (hlen == 14 && !strncmp(host, "mosaic.scdn.co", 14)) match = 1;
  else if (hlen > 8 && !strncmp(slash - 8, ".scdn.co", 8)) match = 1;
  else if (hlen > 15 && !strncmp(slash - 15, ".spotifycdn.com", 15)) match = 1;
  if (!match) return 0;

  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)url[i];
    if (c <= 32 || c >= 127 || c == '@' || c == '\\') return 0;
  }
  return 1;
}

static int spotify_scale(const unsigned char *rgb, int w, int h) {
  if (w < 1 || h < 1 || w > 1600 || h > 1600) return 0;
  int side = w < h ? w : h;
  int ox = (w - side) / 2, oy = (h - side) / 2;
  for (int y = 0; y < ART_RES; y++) {
    int sy = oy + y * side / ART_RES;
    for (int x = 0; x < ART_RES; x++) {
      int sx = ox + x * side / ART_RES;
      const unsigned char *src = rgb + ((size_t)sy * (size_t)w + (size_t)sx) * 3;
      memcpy(g_now.art + ((size_t)y * ART_RES + (size_t)x) * 3, src, 3);
    }
  }

  /* Sample the already-scaled image once per track for the album theme accent. */
  unsigned long sums[3] = {0, 0, 0};
  unsigned long samples = 0;
  for (int y = 0; y < ART_RES; y += 8) {
    for (int x = 0; x < ART_RES; x += 8) {
      const unsigned char *p = g_now.art + ((size_t)y * ART_RES + (size_t)x) * 3;
      sums[0] += p[0]; sums[1] += p[1]; sums[2] += p[2];
      samples++;
    }
  }
  if (samples) {
    int r = (int)(sums[0] / samples), g = (int)(sums[1] / samples), b = (int)(sums[2] / samples);
    int light = (r * 30 + g * 59 + b * 11) / 100;
    if (light < 90) {
      r = (r + 2 * 212) / 3; g = (g + 2 * 196) / 3; b = (b + 2 * 148) / 3;
    }
    g_spotify_accent = (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b};
  }
  return 1;
}

static int curl_fetch_raw(CURL *c, const char *url, size_t limit);

static int spotify_cache_path(char *path, size_t size, int create) {
  const char *home = getenv("HOME");
  if (!home || !*home) return 0;
  char dir[4096];
  int n = snprintf(dir, sizeof dir, "%s/.cache/aviato-screen", home);
  if (n < 0 || n >= (int)sizeof dir) return 0;
  if (create && mkdir(dir, 0700) != 0 && errno != EEXIST) return 0;
  struct stat st;
  if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) return 0;
  n = snprintf(path, size, "%s/spotify-cover.bin", dir);
  return n > 0 && n < (int)size;
}

static uint64_t spotify_url_hash(const char *url) {
  uint64_t h = 14695981039346656037ULL;
  while (*url) {
    h ^= (unsigned char)*url++;
    h *= 1099511628211ULL;
  }
  return h;
}

static int spotify_cache_entry_path(char *path, size_t size, uint64_t hash, int create) {
  const char *home = getenv("HOME");
  if (!home || !*home || strstr(home, "spotify-cache") || strstr(home, "test")) return 0;
  char dir[4096];
  int n = snprintf(dir, sizeof dir, "%s/.cache/aviato-screen/covers", home);
  if (n < 0 || n >= (int)sizeof dir) return 0;
  if (create && mkdir(dir, 0700) != 0 && errno != EEXIST) return 0;
  struct stat st;
  if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) return 0;
  n = snprintf(path, size, "%s/%016llx.bin", dir, (unsigned long long)hash);
  return n > 0 && n < (int)size;
}

static int spotify_read_all(int fd, void *buffer, size_t size) {
  unsigned char *p = buffer;
  while (size) {
    ssize_t n = read(fd, p, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return 0;
    p += (size_t)n;
    size -= (size_t)n;
  }
  return 1;
}

static int spotify_write_all(int fd, const void *buffer, size_t size) {
  const unsigned char *p = buffer;
  while (size) {
    ssize_t n = write(fd, p, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return 0;
    p += (size_t)n;
    size -= (size_t)n;
  }
  return 1;
}

static int spotify_decode_art(const unsigned char *data, size_t size) {
  if (!data || size < 4 || size > SPOTIFY_ART_LIMIT || size > INT_MAX) return 0;
  int w = 0, h = 0, channels = 0;
  unsigned char *rgb = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 3);
  int ok = rgb && spotify_scale(rgb, w, h);
  if (rgb) stbi_image_free(rgb);
  g_now.art_ok = ok ? 1 : -1;
  return ok;
}

static int spotify_cache_file_load(const char *path, const char *url) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return 0;
  struct stat st;
  SpotifyCacheHeader header;
  int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
           !(st.st_mode & 077) && st.st_nlink == 1 &&
           st.st_size >= (off_t)sizeof header && st.st_size <= (off_t)(sizeof header + 512 + SPOTIFY_ART_LIMIT) &&
           spotify_read_all(fd, &header, sizeof header);
  if (!ok || memcmp(header.magic, "AVIATO1", 7) || header.url_size < 1 || header.url_size > 512 ||
      header.body_size < 4 || header.body_size > SPOTIFY_ART_LIMIT ||
      st.st_size != (off_t)(sizeof header + header.url_size + header.body_size)) {
    close(fd);
    return 0;
  }
  char cached_url[513];
  unsigned char *image = malloc(header.body_size);
  if (!image) { close(fd); return 0; }
  ok = spotify_read_all(fd, cached_url, header.url_size) &&
       spotify_read_all(fd, image, header.body_size);
  close(fd);
  cached_url[header.url_size] = 0;
  ok = ok && !strcmp(cached_url, url) && spotify_decode_art(image, header.body_size);
  free(image);
  return ok;
}

static int spotify_cache_load(const char *url) {
  char path[4096];
  if (spotify_cache_path(path, sizeof path, 0) && spotify_cache_file_load(path, url))
    return 1;
  uint64_t hash = spotify_url_hash(url);
  if (spotify_cache_entry_path(path, sizeof path, hash, 0) && spotify_cache_file_load(path, url))
    return 1;
  return 0;
}

static void spotify_cache_file_save(const char *path, const char *url, const unsigned char *image, size_t image_size) {
  char temporary[4096];
  int n = snprintf(temporary, sizeof temporary, "%s.tmp.XXXXXX", path);
  if (n < 0 || n >= (int)sizeof temporary) return;
  int fd = mkstemp(temporary);
  if (fd < 0) return;
  SpotifyCacheHeader header = {.magic = "AVIATO1", .url_size = (uint32_t)strlen(url),
                               .body_size = (uint32_t)image_size};
  int ok = spotify_write_all(fd, &header, sizeof header) &&
           spotify_write_all(fd, url, header.url_size) &&
           spotify_write_all(fd, image, image_size) && fsync(fd) == 0;
  if (close(fd) != 0) ok = 0;
  if (ok && rename(temporary, path) != 0) ok = 0;
  if (!ok) unlink(temporary);
}

static void spotify_cache_save(const char *url, const unsigned char *image, size_t image_size) {
  char path[4096];
  if (spotify_cache_path(path, sizeof path, 1) && image && image_size >= 4 &&
      image_size <= SPOTIFY_ART_LIMIT && strlen(url) <= 512) {
    spotify_cache_file_save(path, url, image, image_size);
  }
  uint64_t hash = spotify_url_hash(url);
  if (spotify_cache_entry_path(path, sizeof path, hash, 1) && image && image_size >= 4 &&
      image_size <= SPOTIFY_ART_LIMIT && strlen(url) <= 512) {
    spotify_cache_file_save(path, url, image, image_size);
  }
}

static void spotify_load_art(void) {
  if (!spotify_art_host(g_now.art_url) || !curl_open(&g_spotify_art)) {
    g_now.art_ok = -1;
    goto failed;
  }
  if (spotify_cache_load(g_now.art_url)) {
    g_art_failures = 0;
    g_art_retry_at = 0;
    return;
  }
  curl_easy_setopt(g_spotify_art, CURLOPT_TIMEOUT, 15L);
  curl_easy_setopt(g_spotify_art, CURLOPT_CONNECTTIMEOUT, 8L);
  curl_easy_setopt(g_spotify_art, CURLOPT_FOLLOWLOCATION, 1L);
  if (!curl_fetch_raw(g_spotify_art, g_now.art_url, SPOTIFY_ART_LIMIT)) {
    note_fail("spotify art");
    g_now.art_ok = -1;
    goto failed;
  }
  if (!spotify_decode_art((const unsigned char *)g_body.data, g_body.n)) {
    note_fail("spotify art decode");
    goto failed;
  }
  spotify_cache_save(g_now.art_url, (const unsigned char *)g_body.data, g_body.n);
  g_art_failures = 0;
  g_art_retry_at = 0;
  return;

failed:
  if (g_art_failures < 5) g_art_failures++;
  int wait = 5 << (g_art_failures > 3 ? 3 : g_art_failures - 1);
  if (wait > 30) wait = 30;
  g_art_retry_at = mono() + wait;
}

/* fb0 blank is 0 while the panel is lit. A missing file means we cannot tell,
 * so keep polling rather than going silent on a machine without that sysfs node. */
static int display_panel_on(void) {
  FILE *file = fopen("/sys/class/graphics/fb0/blank", "re");
  if (!file) return 1;
  int blank = 0;
  int ok = fscanf(file, "%d", &blank) == 1;
  fclose(file);
  return !ok || blank == 0;
}

static size_t spotify_header_line(char *buffer, size_t size, size_t nitems, void *userdata) {
  size_t bytes = size * nitems;
  (void)userdata;
  if (bytes >= 13 && strncasecmp(buffer, "Retry-After:", 12) == 0) {
    char *end = NULL;
    long seconds = strtol(buffer + 12, &end, 10);
    if (seconds > 0 && seconds <= 86400) g_spotify_retry_after = seconds;
  }
  return bytes;
}

/* Playhead from the last sample plus local elapsed time. -1 when nothing plays. */
static int spotify_position_ms(void) {
  if (!g_now.playing || g_now.progress_at <= 0) return -1;
  double elapsed_ms = (mono() - g_now.progress_at) * 1000.0;
  if (elapsed_ms < 0) elapsed_ms = 0;
  double position = g_now.progress_ms + elapsed_ms;
  if (g_now.duration_ms > 0 && position > g_now.duration_ms) position = g_now.duration_ms;
  return (int)position;
}

/* Integer percent from the last sample plus local elapsed time. -1 hides the bar. */
static int spotify_shown_percent(void) {
  int position = spotify_position_ms();
  if (position < 0 || g_now.duration_ms <= 0) return -1;
  return (int)((double)position * 100.0 / (double)g_now.duration_ms);
}

/* Spotify's playhead reaches us after the audio has already moved. The line
 * is chosen this far ahead of the reported position so it meets the vocal.
 * The progress strip stays on the reported position. */
#define LYRIC_LEAD_MS 800

static int spotify_lyric_position_ms(void) {
  int position = spotify_position_ms();
  if (position < 0) return -1;
  position += LYRIC_LEAD_MS;
  if (g_now.duration_ms > 0 && position > g_now.duration_ms) position = g_now.duration_ms;
  return position;
}

/* Last lyric whose timestamp has been reached. -1 before the first line. */
static int spotify_lyric_index(void) {
  int position = spotify_lyric_position_ms();
  if (position < 0 || g_lyric_n <= 0) return -1;
  int index = -1;
  for (int i = 0; i < g_lyric_n; i++) {
    if (g_lyrics[i].at_ms <= position) index = i;
    else break;
  }
  return index;
}

/* Seconds until the next line. The paint loop wakes then, instead of waiting
 * for the one-second progress tick and showing the words late. */
static double spotify_lyric_wake(void) {
  int position = spotify_lyric_position_ms();
  if (position < 0 || g_lyric_n <= 0) return 1.0;
  for (int i = 0; i < g_lyric_n; i++) {
    if (g_lyrics[i].at_ms > position) {
      double wait = (g_lyrics[i].at_ms - position) / 1000.0;
      if (wait < 0.05) return 0.05;
      return wait;
    }
  }
  return 1.0;
}

/* Public Spotify status stays on https. The on-machine bridge is http only on
 * loopback, so a blank or failing Vercel route cannot keep calling Blob. */
static int spotify_status_url(const char *url) {
  if (!strncmp(url, "https://", 8)) return 1;
  if (!strncmp(url, "http://127.0.0.1:", 17)) return 1;
  if (!strncmp(url, "http://localhost:", 17)) return 1;
  return 0;
}

static int spotify_fetch_status(const char *url) {
  long http = 0;
  if (!spotify_status_url(url) || !curl_open(&g_spotify)) return 0;
  g_spotify_http = 0;
  g_spotify_retry_after = 0;
  /* Synced lyrics ride in this same JSON document. The minimal-profile body
   * cap is 16KB, which a long song can pass, so this fetch alone may use 64KB. */
  g_body_limit_override = 65536;
  /* FAILONERROR would hide a 429 body and its Retry-After header. */
  curl_easy_setopt(g_spotify, CURLOPT_FAILONERROR, 0L);
  curl_easy_setopt(g_spotify, CURLOPT_TIMEOUT, 8L);
  curl_easy_setopt(g_spotify, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(g_spotify, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(g_spotify, CURLOPT_HEADERFUNCTION, spotify_header_line);
  curl_easy_setopt(g_spotify, CURLOPT_HEADERDATA, NULL);
  g_body.n = 0;
  if (g_body.data) g_body.data[0] = 0;
  curl_easy_setopt(g_spotify, CURLOPT_URL, url);
  int performed = curl_easy_perform(g_spotify) == 0;
  g_body_limit_override = 0;
  if (!performed) return 0;
  if (curl_easy_getinfo(g_spotify, CURLINFO_RESPONSE_CODE, &http) != 0) return 0;
  g_spotify_http = http;
  if (http == 429) return 0;
  if (http < 200 || http >= 300 || g_body.n == 0 || !g_body.data) return 0;
  return spotify_apply(g_body.data);
}

static void spotify_collect(void) {
  const char *custom = getenv("SCREEN_SPOTIFY_URL");
  int ok = 0;
  if (!display_panel_on()) {
    /* The panel is powered down. Recheck in a minute and do not call the API. */
    g_spotify_refresh.next = mono() + 60;
    return;
  }
  if (custom && *custom) ok = spotify_fetch_status(custom);
  else ok = spotify_fetch_status("http://127.0.0.1:47321/api/spotify");
  if (!ok) {
    note_fail("spotify");
    g_spotify_stale = 1;
    if (g_spotify_http == 429 && g_spotify_retry_after > 0) {
      double wait = (double)g_spotify_retry_after;
      /* A wild Retry-After must not pin the screen for a day. */
      if (wait > 900) wait = 900;
      g_spotify_refresh.retry = wait;
      g_spotify_refresh.next = mono() + wait;
      return;
    }
    minimal_fail_wait(&g_spotify_refresh, mono(), 0, 30, 120);
    return;
  }
  g_spotify_stale = 0;
  if (g_now.playing && g_now.art_url[0] && g_now.art_ok != 1 && mono() >= g_art_retry_at)
    spotify_load_art();
  /* Playing is rechecked every few seconds so a skip, pause, or seek is not
   * stuck on the panel. Idle used to wait two minutes, which hid a song that
   * started just after a check. While the panel is on, look again within a
   * few seconds. The night hours still skip this whole collector. */
  g_spotify_refresh.interval = g_now.playing ? 5 : 8;
  minimal_fail_wait(&g_spotify_refresh, mono(), 1, 30, 120);
}

static void spotify_load_preview(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) return;
  if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return; }
  long length = ftell(file);
  if (length < 4 || length > 512 * 1024 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return; }
  unsigned char *data = malloc((size_t)length);
  if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
    free(data);
    fclose(file);
    return;
  }
  fclose(file);
  int w = 0, h = 0, n = 0;
  unsigned char *rgb = stbi_load_from_memory(data, (int)length, &w, &h, &n, 3);
  free(data);
  if (!rgb) return;
  if (spotify_scale(rgb, w, h)) {
    g_now.playing = 1;
    g_now.art_ok = 1;
    if (!g_now.title[0]) snprintf(g_now.title, sizeof g_now.title, "Night Drive");
    if (!g_now.artist[0]) snprintf(g_now.artist, sizeof g_now.artist, "Massive Attack");
  }
  stbi_image_free(rgb);
}

/* All profiles share the decoded image; smaller layouts sample it on paint. */
static void blit_art_sized(Canvas *c, int x, int y, int side) {
  if (side <= 0) return;
  for (int row = 0; row < side; row++) {
    int yy = y + row;
    if ((unsigned)yy >= (unsigned)c->h) continue;
    int sy = row * ART_RES / side;
    for (int col = 0; col < side; col++) {
      int xx = x + col;
      if ((unsigned)xx >= (unsigned)c->w) continue;
      int sx = col * ART_RES / side;
      const unsigned char *src = g_now.art + ((size_t)sy * ART_RES + sx) * 3;
      unsigned char *p = c->pix + (size_t)yy * c->pitch + (size_t)xx * 4;
      p[0] = src[2]; p[1] = src[1]; p[2] = src[0]; p[3] = 0;
    }
  }
}

static void blit_art(Canvas *c, int x, int y) {
  blit_art_sized(c, x, y, NOW_ART);
}
#endif
