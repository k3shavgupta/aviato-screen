#ifndef AVIATO_MINIMAL_SCREEN_H
#define AVIATO_MINIMAL_SCREEN_H

static time_t g_minimal_weather_updated;
static int g_minimal_weather_failed;
static double g_minimal_disk_free = -1;

static void sample_minimal_system(void) {
  sample_system();
  struct statvfs disk;
  g_minimal_disk_free = -1;
  if (statvfs("/", &disk) == 0 && disk.f_blocks && disk.f_frsize)
    g_minimal_disk_free = (double)disk.f_bavail * disk.f_frsize / (1024.0 * 1024 * 1024);
}
static int minimal_active(const struct tm *tm) { return tm->tm_hour >= 8 && tm->tm_hour < 23; }

/* Coinbase's decimal string is deliberately narrower than strtod: no signs,
 * whitespace, exponent, escapes, non-finite values, or unbounded numbers. */
static int minimal_usd_parse(const char *body, Quote *out) {
  if (!json_document(body)) return 0;
  JsonValue root = {json_space(body), body + strlen(body)};
  JsonValue data = json_member(root, "data");
  if (!json_literal(json_member(data, "base"), "\"BTC\"") ||
      !json_literal(json_member(data, "currency"), "\"USD\"")) return 0;
  JsonValue amount = json_member(data, "amount");
  if (!amount.start || *amount.start != '"' || amount.end - amount.start < 3 ||
      amount.end - amount.start > 32) return 0;
  const char *p = amount.start + 1, *end = amount.end - 1;
  if (!isdigit((unsigned char)*p)) return 0;
  while (p < end && isdigit((unsigned char)*p)) p++;
  if (p < end && *p == '.') {
    p++;
    if (p == end) return 0;
    while (p < end && isdigit((unsigned char)*p)) p++;
  }
  if (p != end) return 0;
  double price = strtod(amount.start + 1, NULL);
  Quote next = {0};
  if (!take_price(&next, price, 0.00000001, 10000000)) return 0;
  *out = next;
  return 1;
}

/* A healthy Coinbase reply on this link can take a little over two seconds.
 * The old two-second cap treated that as a failure and left the price blank. */
static int fetch_minimal_btc(void) {
  if (!curl_open(&g_coin)) return 0;
  curl_easy_setopt(g_coin, CURLOPT_TIMEOUT, 8L);
  curl_easy_setopt(g_coin, CURLOPT_CONNECTTIMEOUT, 5L);
  if (!curl_fetch(g_coin, "https://api.coinbase.com/v2/prices/BTC-USD/spot") ||
      !minimal_usd_parse(g_body.data, &g_markets.btc)) return 0;
  g_btc_updated = time(NULL);
  return 1;
}

/* Failed reads retry soon, then back off, and never wait out the success
 * interval. One blip must not leave the screen blank for ten or sixty minutes. */
static void minimal_fail_wait(Refresh *r, double now, int success, double first, double cap) {
  if (success) {
    r->retry = r->interval;
    r->next = now + r->interval;
    return;
  }
  double wait = r->retry >= r->interval ? first : r->retry * 2;
  if (wait > cap) wait = cap;
  r->retry = wait;
  r->next = now + wait;
}

#include "spotify-screen.h"

static void collect_minimal(const struct tm *tm, Weather *weather) {
  if (!minimal_active(tm)) return;
  if (mono() >= g_coin_refresh.next) {
    g_btc_failed = !fetch_minimal_btc();
    if (g_btc_failed) note_fail("btc");
    minimal_fail_wait(&g_coin_refresh, mono(), !g_btc_failed, 30, 120);
  }
  /* Recheck wall time after each bounded network operation at the boundary. */
  struct tm current = now_ist();
  if (!minimal_active(&current)) return;
  if (mono() >= g_weather_refresh.next) {
    g_minimal_weather_failed = load_weather(weather) != 0;
    if (!g_minimal_weather_failed) g_minimal_weather_updated = time(NULL);
    minimal_fail_wait(&g_weather_refresh, mono(), !g_minimal_weather_failed, 60, 300);
  }
  current = now_ist();
  if (!minimal_active(&current)) return;
  if (mono() >= g_system_refresh.next) {
    sample_minimal_system();
    refresh_done(&g_system_refresh, mono(), 1);
  }
  current = now_ist();
  if (!minimal_active(&current)) return;
  if (mono() >= g_spotify_refresh.next) spotify_collect();
}

static const Color minimal_bg = {21,25,22};
static const Color minimal_fg = {235,236,229};
static const Color minimal_dim = {160,170,156};

static void minimal_btc_text(char *price, size_t pn, char *detail, size_t dn) {
  snprintf(price, pn, "Unavailable");
  snprintf(detail, dn, "Coinbase | unavailable");
  if (!g_markets.btc.ok) return;
  char number[64], stamp[24];
  fmt_quote(number, sizeof number, g_markets.btc.value, 2);
  snprintf(price, pn, "$%s USD", number);
  time_t ist = g_btc_updated + 19800;
  struct tm local;
  gmtime_r(&ist, &local);
  strftime(stamp, sizeof stamp, "%d %b %H:%M IST", &local);
  int stale = g_btc_failed || time(NULL) - g_btc_updated > 345 || g_btc_updated > time(NULL);
  snprintf(detail, dn, "Coinbase | %s%s", stamp, stale ? " | stale" : "");
}

static void draw_text_fit(Canvas *c, int x, int y, const char *text, int size, Color color, int max_width) {
  if (text_width(text, size, 0) <= max_width) {
    draw_text(c, x, y, text, size, color, 0);
    return;
  }
  char fitted[180];
  size_t length = strlen(text);
  if (length > 150) length = 150;
  memcpy(fitted, text, length);
  fitted[length] = 0;
  while (length && text_width(fitted, size, 0) + text_width("...", size, 0) > max_width) {
    length--;
    while (length && ((unsigned char)fitted[length] & 0xC0) == 0x80) length--;
    fitted[length] = 0;
  }
  memcpy(fitted + length, "...", 4);
  draw_text(c, x, y, fitted, size, color, 0);
}

static void update_face_animation(double now) {
  int active = g_minimal && g_profile != PROFILE_ADAPTIVE && !g_shared_frame && agent_current_state() == AGENT_RUNNING;
  if (!active) {
    if (g_face_blink) { g_face_blink = 0; g_face_dirty = 1; }
    g_face_deadline = 0;
    return;
  }
  if (g_face_deadline <= 0) {
    g_face_deadline = now + 5;
    return;
  }
  if (now < g_face_deadline) return;
  g_face_blink = !g_face_blink;
  g_face_dirty = 1;
  g_face_deadline = now + (g_face_blink ? 0.2 : 6.0);
}

/* Each region has a textual signature; only changed regions are cleared. */
static void minimal_lines(const struct tm *tm, const Weather *w, char top[384],
                          char middle[384], char health[96]) {
  char price[96], detail[96], date[64], weather[96], title[96], artist[96];
  minimal_btc_text(price, sizeof price, detail, sizeof detail);
  strftime(date, sizeof date, "%A, %d %B", tm);
  if (!w->ok) snprintf(weather, sizeof weather, "--|unavailable");
  else snprintf(weather, sizeof weather, "%d|%s|%d", w->temp,
    condition(w->code)[0] ? condition(w->code) : "Kanpur",
    g_minimal_weather_failed || time(NULL) - g_minimal_weather_updated > 1845);
  snprintf(top, 384, "%s|%02d:%02d|%s|%s|%s", date, tm->tm_hour, tm->tm_min, price, detail, weather);
  if (g_now.playing && g_now.title[0]) {
    snprintf(title, sizeof title, "%.80s", g_now.title);
    snprintf(artist, sizeof artist, "%.60s", g_now.artist);
  } else if (g_spotify_stale) {
    snprintf(title, sizeof title, "unavailable");
    artist[0] = 0;
  } else {
    snprintf(title, sizeof title, "idle");
    artist[0] = 0;
  }
  const char *id = g_now.art_url;
  size_t id_len = strlen(id);
  if (id_len > 40) id += id_len - 40;
  int agent_state = g_shared_frame ? AGENT_IDLE : agent_current_state();
  snprintf(middle, 384, "%d|%d|%d|%s|%s|%.40s|%.40s|%d|%d", g_now.playing, g_now.art_ok,
           g_spotify_stale, title, artist, id, g_now.album, saying_slot(tm), agent_state);
  if (g_system.total > 0) {
    char cpu[32], disk[32];
    if (g_system.cpu >= 0) snprintf(cpu, sizeof cpu, "CPU %d\u00b0C", g_system.cpu);
    else snprintf(cpu, sizeof cpu, "CPU unavailable");
    if (g_minimal_disk_free >= 0) snprintf(disk, sizeof disk, "%.0f GB free", g_minimal_disk_free);
    else snprintf(disk, sizeof disk, "disk free unavailable");
    char metrics[96];
    snprintf(metrics, sizeof metrics, "load %.2f | RAM %.1f/%.1f GB",
      g_system.load, g_system.used, g_system.total);
    snprintf(health, 96, "AVIATO | %.20s | %.36s | %.23s", cpu, metrics, disk);
  } else snprintf(health, 96, "AVIATO  |  machine readings unavailable");
}

static void spotify_mark(Canvas *c, int x, int y, int live) {
  Color edge = {92, 99, 88};
  Color vinyl = {32, 36, 33};
  Color gold = g_profile == PROFILE_ALBUM && g_now.art_ok == 1 ? g_spotify_accent : (Color){212, 196, 148};
  int side = NOW_ART;
  if (live) blit_art(c, x, y);
  else {
    int cx = x + side / 2, cy = y + side / 2, r = side / 2 - 22;
    fill_disc(c, cx, cy, r, vinyl);
    ring(c, cx, cy, r * 62 / 100, edge);
    ring(c, cx, cy, r * 38 / 100, edge);
    fill_disc(c, cx, cy, r * 18 / 100, gold);
    fill_disc(c, cx, cy, r * 4 / 100, minimal_bg);
  }
  hair(c, x, x + side - 1, y, edge);
  hair(c, x, x + side - 1, y + side - 1, edge);
  vhair(c, x, y, y + side - 1, edge);
  vhair(c, x + side - 1, y, y + side - 1, edge);
}

static void minimal_spotify(Canvas *c) {
  char title[160], artist[180], album[160];
  album[0] = 0;
  if (g_now.playing && g_now.title[0]) {
    snprintf(title, sizeof title, "%s", g_now.title);
    if (g_spotify_stale && g_now.artist[0]) snprintf(artist, sizeof artist, "%s  stale", g_now.artist);
    else if (g_spotify_stale) snprintf(artist, sizeof artist, "stale");
    else snprintf(artist, sizeof artist, "%s", g_now.artist);
    snprintf(album, sizeof album, "%s", g_now.album);
  } else if (g_spotify_stale) {
    snprintf(title, sizeof title, "Spotify unavailable");
    artist[0] = 0;
  } else {
    snprintf(title, sizeof title, "Not listening");
    artist[0] = 0;
  }
  int y = 618;
  int live = g_now.playing && g_now.art_ok == 1;
  spotify_mark(c, 116, y, live);
  int x = 116 + NOW_ART + 40;
  int width = 510;
  Color gold = g_profile == PROFILE_ALBUM && g_now.art_ok == 1 ? g_spotify_accent : (Color){212, 196, 148};
  draw_text(c, x, y + 18, "SPOTIFY", 20, minimal_dim, 3);
  hair(c, x, x + 84, y + 50, gold);
  draw_text_fit(c, x, y + 72, title, 48, minimal_fg, width);
  if (artist[0]) draw_text_fit(c, x, y + 140, artist, 26, minimal_dim, width);
  if (album[0]) draw_text_fit(c, x, y + 180, album, 22, minimal_dim, width);
}

static void minimal_agent_face(Canvas *c) {
  int state = g_shared_frame ? AGENT_IDLE : agent_current_state();
  if (state == AGENT_IDLE || state == AGENT_STALE) return;
  Color color = (Color){157, 195, 179};
  const char *label = "WORKING";
  if (state == AGENT_APPROVAL) { color = (Color){230, 175, 106}; label = "WAITING"; }
  else if (state == AGENT_STOPPED) label = "DONE";
  else if (state == AGENT_ERROR) { color = (Color){223, 142, 122}; label = "ERROR"; }
  draw_agent_face(c, 1762, 824, 64, state, color);
  draw_text(c, 1717, 870, label, 13, color, 1);
}

static void minimal_quote(Canvas *c, const struct tm *tm) {
  int slot = saying_slot(tm);
  Color accent = g_profile == PROFILE_ALBUM && g_now.art_ok == 1 ? g_spotify_accent : (Color){212, 196, 148};
  const int x = 980, y = 626, max_text = 670;
  draw_text(c, x, y, "A NOTE TO KEEP", 18, minimal_dim, 3);
  hair(c, x, x + 100, y + 34, accent);

  char lines[3][160];
  int count = wrap_quote(SAYINGS[slot].text, 34, max_text, lines, 3);
  int line_height = 46;
  int text_y = y + 64;
  for (int i = 0; i < count; i++)
    draw_text(c, x, text_y + i * line_height, lines[i], 34, minimal_fg, 0);
  int author_y = text_y + count * line_height + 6;
  draw_text_fit(c, x, author_y, SAYINGS[slot].author, 21, minimal_dim, max_text);

  minimal_agent_face(c);
}

static void minimal_region(Canvas *c, const struct tm *tm, const Weather *w, int region) {
  char top[384], middle[384], health[96];
  minimal_lines(tm, w, top, middle, health);
  if (region == 0) {
    char date[64], clock[16], price[96], detail[96];
    strftime(date, sizeof date, "%A, %d %B", tm);
    strftime(clock, sizeof clock, "%H:%M", tm);
    minimal_btc_text(price, sizeof price, detail, sizeof detail);
    draw_text(c, 116, 70, "AVIATO", 28, minimal_dim, 3);
    {
      char temp[24], sky[48];
      int stale = w->ok && (g_minimal_weather_failed || time(NULL) - g_minimal_weather_updated > 1845);
      if (!w->ok) {
        snprintf(temp, sizeof temp, "--");
        snprintf(sky, sizeof sky, "unavailable");
      } else {
        const char *cond = condition(w->code);
        snprintf(temp, sizeof temp, "%d\u00b0", w->temp);
        if (stale || !cond || !cond[0]) snprintf(sky, sizeof sky, "%s", stale ? "stale" : "Kanpur");
        else {
          size_t i = 0;
          for (; cond[i] && i + 1 < sizeof sky; i++) sky[i] = (char)tolower((unsigned char)cond[i]);
          sky[i] = 0;
        }
      }
      int date_size = 26, temp_size = 34, sky_size = 18;
      int date_w = text_width(date, date_size, 0);
      int temp_w = text_width(temp, temp_size, 0);
      int sky_w = text_width(sky, sky_size, 1);
      int right = 1804;
      int date_x = right - date_w;
      int sky_x = date_x - 32 - sky_w;
      int temp_x = sky_x - 14 - temp_w;
      int base = 74 + font_ascent(date_size);
      draw_text(c, temp_x, base - font_ascent(temp_size), temp, temp_size, minimal_fg, 0);
      draw_text(c, sky_x, base - font_ascent(sky_size) - 1, sky, sky_size, minimal_dim, 1);
      draw_text(c, date_x, base - font_ascent(date_size), date, date_size, minimal_dim, 0);
    }
    draw_text(c, 100, 230, clock, 250, minimal_fg, -8);
    draw_text(c, 116, 500, "Asia/Kolkata  /  IST", 28, minimal_dim, 0);
    draw_text(c, 1100, 250, "BITCOIN", 30, minimal_dim, 2);
    draw_text(c, 1100, 325, price, 72, minimal_fg, 0);
    draw_text(c, 1100, 440, detail, 24, minimal_dim, 0);
  } else if (region == 1) {
    minimal_spotify(c);
    minimal_quote(c, tm);
  } else draw_text(c, 116, 980, health, 26, minimal_dim, 0);
}

#include "adaptive-screen.h"

static void paint_minimal(Canvas *c, const struct tm *tm, const Weather *w) {
  if (g_profile == PROFILE_ADAPTIVE) { paint_adaptive(c, tm, w); return; }
  fill(c, minimal_bg);
  for (int i=0; i<3; i++) minimal_region(c, tm, w, i);
}

static int render_minimal(Canvas *c, RenderState *state, const struct tm *tm, const Weather *w) {
  if (g_profile == PROFILE_ADAPTIVE) return render_adaptive(c, state, tm, w);
  char top[384], middle[384], health[96];
  minimal_lines(tm, w, top, middle, health);
  int changed = 0;
  int dirty[] = {!state->initialized || strcmp(top, state->markets),
    !state->initialized || strcmp(middle, state->portfolio),
    !state->initialized || strcmp(health, state->system)};
  const Rect areas[] = {{0,0,W,600},{0,600,W,900},{0,900,W,H}};
  for (int i=0; i<3; i++) if (dirty[i]) {
    clear_rect(c, areas[i], minimal_bg);
    minimal_region(c, tm, w, i);
    changed = 1;
  }
  if (g_face_dirty && state->initialized && !dirty[1]) {
    clear_rect(c, (Rect){1688, 786, 1838, 898}, minimal_bg);
    minimal_agent_face(c);
    changed = 1;
  }
  g_face_dirty = 0;
  state->initialized = 1; state->tm = *tm;
  snprintf(state->markets, sizeof state->markets, "%s", top);
  snprintf(state->portfolio, sizeof state->portfolio, "%s", middle);
  snprintf(state->system, sizeof state->system, "%s", health);
  return changed;
}
#endif
