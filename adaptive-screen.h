#ifndef AVIATO_ADAPTIVE_SCREEN_H
#define AVIATO_ADAPTIVE_SCREEN_H

/* Three compositions, using the same collectors and persistent artwork cache. */
enum { ROOM_NIGHT, ROOM_IDLE, ROOM_MUSIC };
static const Color room_night_bg = {0, 0, 0};
static const Color room_night_fg = {116, 126, 120};
static const Color room_night_dim = {83, 93, 87};
static const Rect room_regions[] = {{0,0,W,160}, {0,160,W,930}, {0,930,W,H}};

static int room_mode(const struct tm *tm) {
  if (!minimal_active(tm)) return ROOM_NIGHT;
  return g_now.playing && g_now.title[0] ? ROOM_MUSIC : ROOM_IDLE;
}

static void room_weather(const Weather *w, char text[160]) {
  if (!w->ok) {
    snprintf(text, 160, "Kanpur   Weather unavailable");
    return;
  }
  int stale = g_minimal_weather_failed || time(NULL) - g_minimal_weather_updated > 1845 ||
              g_minimal_weather_updated > time(NULL);
  snprintf(text, 160, "Kanpur   %d\u00b0   %s", w->temp,
           stale ? "Last known" : condition(w->code));
}

/* Wrap at whole UTF-8 characters, prefer word boundaries, and ellipsize the
 * final line. A long unbroken title must not overwrite the right margin. */
static int room_text(Canvas *c, int x, int y, const char *text, int size,
                     int width, int max_lines, int leading, Color color) {
  const char *p = text;
  int line = 0;
  while (*p && line < max_lines) {
    while (*p == ' ' || *p == '\n') p++;
    if (!*p) break;
    if (line == max_lines - 1) {
      draw_text_fit(c, x, y, p, size, color, width);
      return y + leading;
    }
    const char *end = p, *space = NULL, *q = p;
    char part[160];
    while (*q && *q != '\n') {
      const char *next = q;
      utf8_next(&next);
      size_t bytes = (size_t)(next - p);
      if (bytes >= sizeof part) break;
      memcpy(part, p, bytes); part[bytes] = 0;
      if (text_width(part, size, 0) > width) break;
      if (*q == ' ') space = q;
      end = next; q = next;
    }
    if (*q && *q != '\n' && space && space > p) end = space;
    if (end == p) { end = p; utf8_next(&end); }
    size_t bytes = (size_t)(end - p);
    memcpy(part, p, bytes); part[bytes] = 0;
    draw_text_fit(c, x, y, part, size, color, width);
    p = end; y += leading; line++;
  }
  return y;
}

static void room_header(Canvas *c, const struct tm *tm, int mode) {
  char date[64], clock[16];
  strftime(date, sizeof date, "%A, %d %B", tm);
  draw_text(c, 104, 88, date, 28, minimal_dim, 0);
  if (mode == ROOM_MUSIC) {
    strftime(clock, sizeof clock, "%H:%M", tm);
    draw_text(c, 1816 - text_width(clock, 80, -2), 52, clock, 80, minimal_fg, -2);
  } else draw_text(c, 1816 - text_width("Aviato", 28, 0), 88, "Aviato", 28, minimal_dim, 0);
}

static void room_music(Canvas *c) {
  Color accent = g_now.art_ok == 1 ? g_spotify_accent : (Color){212,196,148};
  if (g_now.art_ok == 1) blit_art_sized(c, 104, 222, 640);
  else {
    /* The cover slot stays stable while the download retries. */
    Color vinyl = {32,36,33}, groove = {64,73,65};
    fill_disc(c, 424, 542, 296, vinyl);
    ring(c, 424, 542, 245, groove);
    ring(c, 424, 542, 208, groove);
    ring(c, 424, 542, 171, groove);
    fill_disc(c, 424, 542, 70, accent);
    fill_disc(c, 424, 542, 12, minimal_bg);
  }
  fill_disc(c, 850, 294, 6, accent);
  draw_text(c, 872, 278, g_spotify_stale ? "Last known playback" : "Listening on Spotify",
            26, minimal_dim, 0);
  int end = room_text(c, 840, 366, g_now.title, 80, 976, 3, 90, minimal_fg);
  int artist_y = end + 28;
  draw_text_fit(c, 840, artist_y, g_now.artist, 36, minimal_dim, 976);
  draw_text_fit(c, 840, artist_y + 58, g_now.album, 26, minimal_dim, 976);
}

/* Fit is stored for the current and previous quote so a fade does not remeasure. */
typedef struct {
  int ready, slot, size, n, author;
  char lines[4][160];
  char who[80];
} QuoteLayout;

static QuoteLayout g_quote_layout[2];
static int g_quote_from = -1;
static int g_quote_to = -1;
static double g_quote_fade_t0;
static int g_quote_fading;

/* wrap_quote stops at four lines, so a short block can still hide the ending. */
static int quote_covers(const char *text, const char lines[][160], int n) {
  const char *p = text;
  for (int i = 0; i < n; i++) {
    const char *q = lines[i];
    while (*p == ' ') p++;
    while (*q) {
      if (*p != *q) return 0;
      p++;
      q++;
    }
  }
  while (*p == ' ') p++;
  return *p == 0;
}

static QuoteLayout *quote_layout(int slot) {
  if (slot < 0 || slot >= NSAYINGS) slot = 0;
  for (int i = 0; i < 2; i++)
    if (g_quote_layout[i].ready && g_quote_layout[i].slot == slot) return &g_quote_layout[i];
  int dest = g_quote_layout[0].ready ? 1 : 0;
  if (g_quote_layout[0].ready && g_quote_layout[1].ready)
    dest = g_quote_layout[0].slot == g_quote_from ? 1 : 0;
  QuoteLayout *fit = &g_quote_layout[dest];
  const char *text = SAYINGS[slot].text;
  if (!text || !text[0]) text = "No quote yet.";
  int width = QUOTE_COL_W * 9 / 10;
  int saved = g_face_id;
  g_face_id = 1;
  int size = QUOTE_MAX_PX, n = 0;
  char lines[4][160];
  for (; size >= QUOTE_MIN_PX; size -= 4) {
    n = wrap_quote(text, size, width, lines, 4);
    if (n < 1 || !quote_covers(text, lines, n)) continue;
    int author = size * 45 / 100;
    if (author < 26) author = 26;
    int block = n * (size + size / 4) + font_ascent(author) + 24;
    if (block <= 620) break;
  }
  if (size < QUOTE_MIN_PX || n < 1 || !quote_covers(text, lines, n)) {
    size = QUOTE_MIN_PX;
    n = wrap_quote(text, size, width, lines, 4);
    if (n < 1) {
      n = 1;
      snprintf(lines[0], sizeof lines[0], "No quote yet.");
    } else if (!quote_covers(text, lines, n)) {
      size_t len = strlen(lines[n - 1]);
      if (len > 150) len = 150;
      memcpy(lines[n - 1] + len, "...", 4);
    }
  }
  fit->ready = 1;
  fit->slot = slot;
  fit->size = size;
  fit->n = n;
  fit->author = size * 45 / 100;
  if (fit->author < 26) fit->author = 26;
  for (int i = 0; i < n; i++) memcpy(fit->lines[i], lines[i], sizeof fit->lines[i]);
  const char *who = SAYINGS[slot].author;
  snprintf(fit->who, sizeof fit->who, "%s", who && who[0] ? who : "");
  g_face_id = saved;
  return fit;
}

static void quote_note(const struct tm *tm) {
  int slot = quote_hold_slot(tm);
  if (g_quote_to < 0) {
    g_quote_from = g_quote_to = slot;
    g_quote_fade_t0 = mono() - 10;
    return;
  }
  if (slot != g_quote_to) {
    g_quote_from = g_quote_to;
    g_quote_to = slot;
    g_quote_fade_t0 = mono();
  }
}

static int quote_fade_phase(void) {
  double elapsed = mono() - g_quote_fade_t0;
  if (elapsed >= QUOTE_FADE_MS / 1000.0) return 4;
  int phase = (int)(elapsed * 4.0 / (QUOTE_FADE_MS / 1000.0));
  if (phase < 0) phase = 0;
  if (phase > 3) phase = 3;
  return phase;
}

/* Seconds until the next quote, or a short step while the 400ms fade is in progress. */
static double quote_wake(const struct tm *tm, double now) {
  (void)now;
  g_quote_fading = 0;
  if (g_profile != PROFILE_ADAPTIVE || !minimal_active(tm)) return 60;
  if (g_now.playing && g_now.title[0]) return 60;
  quote_note(tm);
  if (quote_fade_phase() < 4) {
    g_quote_fading = 1;
    return 0.08;
  }
  return quote_hold_wait(tm);
}

static void room_draw_quote(Canvas *c, int slot, int alpha) {
  if (alpha <= 0) return;
  QuoteLayout *fit = quote_layout(slot);
  int saved_alpha = g_text_alpha, saved_face = g_face_id;
  g_text_alpha = alpha;
  g_face_id = 1;
  int width = QUOTE_COL_W * 9 / 10;
  int leading = fit->size + fit->size / 4;
  int block = fit->n * leading + 16 + font_ascent(fit->author);
  int y = 220;
  if (block < 640) y += (640 - block) / 2;
  for (int i = 0; i < fit->n; i++)
    draw_text(c, QUOTE_COL_X, y + i * leading, fit->lines[i], fit->size, minimal_fg, 0);
  draw_text_fit(c, QUOTE_COL_X, y + fit->n * leading + 12, fit->who, fit->author, minimal_dim, width);
  g_text_alpha = saved_alpha;
  g_face_id = saved_face;
}

static void room_idle(Canvas *c, const struct tm *tm) {
  char clock[16];
  strftime(clock, sizeof clock, "%H:%M", tm);
  draw_text(c, 94, 296, clock, 300, minimal_fg, -8);
  draw_text(c, 104, 594, "Asia/Kolkata", 26, minimal_dim, 0);
  quote_note(tm);
  int phase = quote_fade_phase();
  int alpha = phase >= 4 ? 255 : (phase + 1) * 255 / 4;
  if (g_quote_from != g_quote_to && alpha < 255)
    room_draw_quote(c, g_quote_from, 255 - alpha);
  room_draw_quote(c, g_quote_to, alpha);
}

static void room_night(Canvas *c, const struct tm *tm) {
  char clock[16], date[64];
  strftime(clock, sizeof clock, "%H:%M", tm);
  strftime(date, sizeof date, "%A, %d %B", tm);
  draw_text(c, (W - text_width(clock, 260, -6)) / 2, 352, clock, 260, room_night_fg, -6);
  draw_text(c, (W - text_width(date, 28, 0)) / 2, 640, date, 28, room_night_dim, 0);
}

static void room_btc_label(char label[120]) {
  char price[96], detail[96];
  minimal_btc_text(price, sizeof price, detail, sizeof detail);
  snprintf(label, 120, "BTC   %s%s", price, strstr(detail, " | stale") ? " (stale)" : "");
}

static void room_footer(Canvas *c, const Weather *w) {
  char weather[160], label[120];
  room_weather(w, weather);
  room_btc_label(label);
  draw_text_fit(c, 104, 962, weather, 28, minimal_dim, 800);
  int price_y = 962 + font_ascent(28) - font_ascent(40);
  draw_text(c, 1816 - text_width(label, 40, 0), price_y, label, 40, minimal_dim, 0);
  if (g_spotify_stale && !g_now.playing)
    draw_text(c, 104, 1005, "Spotify unavailable", 18, minimal_dim, 0);
}

static void room_region(Canvas *c, const struct tm *tm, const Weather *w, int mode, int region) {
  if (mode == ROOM_NIGHT) {
    if (region == 1) room_night(c, tm);
  } else if (region == 0) room_header(c, tm, mode);
  else if (region == 2) room_footer(c, w);
  else if (mode == ROOM_MUSIC) room_music(c);
  else room_idle(c, tm);
}

/* Signatures contain only visible content. Hash full metadata, including the
 * entire URL, so similarly prefixed long tracks cannot leave stale text. */
static void room_signatures(const struct tm *tm, const Weather *w, int mode,
                            char top[384], char body[384], char footer[96]) {
  char date[64], clock[16];
  strftime(date, sizeof date, "%A, %d %B", tm);
  strftime(clock, sizeof clock, "%H:%M", tm);
  top[0] = body[0] = footer[0] = 0;
  if (mode == ROOM_NIGHT) {
    snprintf(body, 384, "%s|%s", clock, date);
    return;
  }
  snprintf(top, 384, "%s|%s", date, mode == ROOM_MUSIC ? clock : "");
  if (mode == ROOM_IDLE) {
    quote_note(tm);
    int phase = quote_fade_phase();
    snprintf(body, 384, "%s|%d|%d", clock, quote_hold_slot(tm), phase);
  }
  else snprintf(body, 384, "%d|%d|%016llx|%016llx|%016llx|%016llx|%u,%u,%u",
    g_now.art_ok, g_spotify_stale,
    (unsigned long long)spotify_url_hash(g_now.title),
    (unsigned long long)spotify_url_hash(g_now.artist),
    (unsigned long long)spotify_url_hash(g_now.album),
    (unsigned long long)spotify_url_hash(g_now.art_url),
    g_spotify_accent.r, g_spotify_accent.g, g_spotify_accent.b);
  char weather[160], label[120], combined[384];
  room_weather(w, weather);
  room_btc_label(label);
  snprintf(combined, sizeof combined, "%s|%s|%d", weather, label,
           g_spotify_stale && !g_now.playing);
  snprintf(footer, 96, "%016llx", (unsigned long long)spotify_url_hash(combined));
}

static int g_room_progress_drawn = -2;
static int g_room_lyric_drawn = -2;

/* Thin strip under the title column. It is outside the cover, so a one-second
 * update does not redraw the album art. */
static void room_progress(Canvas *c, int pct) {
  Rect bar = {840, 800, 1740, 812};
  clear_rect(c, bar, (Color){42, 48, 44});
  if (pct <= 0) return;
  if (pct > 100) pct = 100;
  Rect done = bar;
  done.x1 = bar.x0 + (bar.x1 - bar.x0) * pct / 100;
  if (done.x1 <= done.x0) return;
  Color accent = g_now.art_ok == 1 ? g_spotify_accent : (Color){212, 196, 148};
  clear_rect(c, done, accent);
}

/* Current line, then the next line dimmed. The box sits under the progress
 * strip and to the right of the cover, so a line change does not repaint the
 * album art. No lyric leaves the box the same colour as the room. */
static void room_lyrics(Canvas *c, int index) {
  Rect box = {840, 820, 1816, 918};
  clear_rect(c, box, minimal_bg);
  if (index < 0 || index >= g_lyric_n) return;
  draw_text_fit(c, 840, 824, g_lyrics[index].text, 44, minimal_fg, 976);
  if (index + 1 < g_lyric_n)
    draw_text_fit(c, 840, 878, g_lyrics[index + 1].text, 26, minimal_dim, 976);
}

static void paint_adaptive(Canvas *c, const struct tm *tm, const Weather *w) {
  int mode = room_mode(tm);
  fill(c, mode == ROOM_NIGHT ? room_night_bg : minimal_bg);
  for (int i = 0; i < 3; i++) room_region(c, tm, w, mode, i);
  if (mode == ROOM_MUSIC) {
    room_progress(c, spotify_shown_percent());
    room_lyrics(c, spotify_lyric_index());
  }
}

static int render_adaptive(Canvas *c, RenderState *state, const struct tm *tm, const Weather *w) {
  char top[384], body[384], footer[96];
  int mode = room_mode(tm);
  room_signatures(tm, w, mode, top, body, footer);
  int full = !state->initialized || mode != state->adaptive_mode;
  int dirty[] = {full || strcmp(top, state->markets), full || strcmp(body, state->portfolio),
                 full || strcmp(footer, state->system)};
  int changed = 0;
  Color bg = mode == ROOM_NIGHT ? room_night_bg : minimal_bg;
  for (int i = 0; i < 3; i++) if (dirty[i]) {
    clear_rect(c, room_regions[i], bg);
    room_region(c, tm, w, mode, i);
    changed = 1;
  }
  /* Progress is local. Do not set changed: the strip is already on the
   * framebuffer, and a shared frame must not rewrite the whole BMP each second. */
  int pct = mode == ROOM_MUSIC ? spotify_shown_percent() : -1;
  if (mode == ROOM_MUSIC && (dirty[1] || pct != g_room_progress_drawn))
    room_progress(c, pct);
  g_room_progress_drawn = pct;
  /* The loop wakes on the next lyric timestamp. Redraw only when the line changes. */
  int lyric = mode == ROOM_MUSIC ? spotify_lyric_index() : -1;
  if (mode == ROOM_MUSIC && (dirty[1] || lyric != g_room_lyric_drawn))
    room_lyrics(c, lyric);
  g_room_lyric_drawn = lyric;
  state->initialized = 1; state->adaptive_mode = mode; state->tm = *tm;
  snprintf(state->markets, sizeof state->markets, "%s", top);
  snprintf(state->portfolio, sizeof state->portfolio, "%s", body);
  snprintf(state->system, sizeof state->system, "%s", footer);
  g_face_dirty = 0;
  return changed;
}
#endif
