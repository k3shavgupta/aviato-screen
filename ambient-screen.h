#ifndef AVIATO_AMBIENT_SCREEN_H
#define AVIATO_AMBIENT_SCREEN_H

typedef struct {
  int present, running, waiting, stopped, errors;
  time_t updated_at, expires_at;
  int state;
} AgentStatus;

enum { AGENT_IDLE, AGENT_RUNNING, AGENT_APPROVAL, AGENT_STOPPED, AGENT_ERROR, AGENT_STALE };
static AgentStatus g_agent;
static const Color ambient_bg = {11, 17, 23};
static const Color ambient_fg = {226, 229, 218};
static const Color ambient_dim = {143, 163, 168};

static int agent_parse(const char *data, AgentStatus *out, time_t now) {
  if (!json_document(data)) return 0;
  JsonValue root = {json_space(data), data + strlen(data)};
  double version, updated, counts[4];
  const char *keys[] = {"running", "waiting", "stopped", "errors"};
  if (!json_number(json_member(root, "version"), 1, 1, &version) ||
      !json_number(json_member(root, "updated_at"), 0, (double)now + 60, &updated) ||
      floor(updated) != updated) return 0;
  for (int i = 0; i < 4; i++) {
    if (!json_number(json_member(root, keys[i]), 0, 10000, &counts[i]) ||
        floor(counts[i]) != counts[i]) return 0;
  }
  double expiry = updated + 900;
  JsonValue expires = json_member(root, "expires_at");
  if (expires.start && (!json_number(expires, updated, updated + 900, &expiry) ||
                        floor(expiry) != expiry)) return 0;
  const char *states[] = {"\"idle\"", "\"running\"", "\"approval_requested\"",
                          "\"stopped\"", "\"error\"", "\"stale\""};
  JsonValue value = json_member(root, "state");
  for (int i = AGENT_IDLE; i <= AGENT_STALE; i++) {
    if (!json_literal(value, states[i])) continue;
    *out = (AgentStatus){.present=1, .state=i, .updated_at=(time_t)updated, .expires_at=(time_t)expiry,
      .running=(int)counts[0], .waiting=(int)counts[1],
      .stopped=(int)counts[2], .errors=(int)counts[3]};
    return 1;
  }
  return 0;
}

static void sample_agent(void) {
  char path[4096], data[4097];
  const char *home = getenv("HOME");
  if (!home) return;
  int length = snprintf(path, sizeof path, "%s/.cache/aviato-screen/agents.json", home);
  if (length < 0 || length >= (int)sizeof path) return;
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  g_agent = (AgentStatus){0};
  if (fd < 0) return;
  struct stat st;
  ssize_t n = -1;
  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
      !(st.st_mode & 077) && st.st_nlink == 1 && st.st_size > 0 && st.st_size <= 4096)
    n = read(fd, data, sizeof data);
  close(fd);
  if (n <= 0 || n > 4096 || memchr(data, 0, (size_t)n)) return;
  data[n] = 0;
  agent_parse(data, &g_agent, time(NULL));
}

static void configure_ambient(void) {
  const char *profile = getenv("SCREEN_PROFILE");
  char value[32] = "legacy";
  if (profile && *profile) {
    snprintf(value, sizeof value, "%s", profile);
  } else if (!getenv("SCREEN_PREVIEW")) {
    const char *home = getenv("HOME");
    if (home) {
      char path[4096];
      int n = snprintf(path, sizeof path, "%s/.config/aviato-screen/profile", home);
      if (n > 0 && n < (int)sizeof path) {
        FILE *f = fopen(path, "r");
        if (f) {
          if (fgets(value, sizeof value, f)) value[strcspn(value, "\r\n")] = 0;
          fclose(f);
        }
      }
    }
  }

  g_ambient = 0;
  g_minimal = 0;
  g_profile = PROFILE_LEGACY;
  if (!strcmp(value, "ambient")) {
    g_ambient = 1;
    g_profile = PROFILE_AMBIENT;
  } else if (!strcmp(value, "adaptive")) {
    g_minimal = 1;
    g_profile = PROFILE_ADAPTIVE;
  } else if (!strcmp(value, "album")) {
    g_minimal = 1;
    g_profile = PROFILE_ALBUM;
  } else if (!strcmp(value, "focus") || !strcmp(value, "minimal")) {
    g_minimal = 1;
    g_profile = PROFILE_FOCUS;
  }
}

/* The cache is event driven: agent-status.py sends SIGUSR1 after replacing its
 * private JSON file. This also lets an expired status be repainted on time. */
static int agent_current_state(void) {
  if (!g_agent.present) return AGENT_IDLE;
  time_t now = time(NULL);
  time_t expiry = g_agent.expires_at ? g_agent.expires_at : g_agent.updated_at + 900;
  if (now > expiry || g_agent.updated_at > now + 60) return AGENT_STALE;
  return g_agent.state;
}

static void draw_agent_face(Canvas *c, int cx, int cy, int size, int state, Color color) {
  /* A few cached vector primitives keep the pet cheap and legible at a glance. */
  int r = size / 2;
  ring(c, cx, cy, r, color);
  if (g_face_blink) {
    hair(c, cx - r / 2, cx - r / 6, cy - r / 5, color);
    hair(c, cx + r / 6, cx + r / 2, cy - r / 5, color);
  } else {
    fill_disc(c, cx - r / 3, cy - r / 5, 4, color);
    fill_disc(c, cx + r / 3, cy - r / 5, 4, color);
  }
  if (state == AGENT_APPROVAL) {
    hair(c, cx - r / 3, cx + r / 3, cy + r / 3, color);
    fill_disc(c, cx, cy + r / 6, 4, color);
  } else if (state == AGENT_ERROR) {
    hair(c, cx - r / 4, cx + r / 4, cy + r / 3, color);
    fill_disc(c, cx - r / 4, cy + r / 4, 3, color);
    fill_disc(c, cx + r / 4, cy + r / 4, 3, color);
  } else {
    hair(c, cx - r / 4, cx + r / 4, cy + r / 3, color);
  }
}

static int ambient_phase(const struct tm *tm) {
  if (tm->tm_hour < 6 || tm->tm_hour >= 20) return 0;
  if (tm->tm_hour < 10) return 1;
  if (tm->tm_hour < 17) return 2;
  return 3;
}

static void ambient_scene(Canvas *c, const struct tm *tm) {
  int phase = ambient_phase(tm);
  const Color sun[] = {{183,199,198},{208,151,98},{202,188,128},{208,124,87}};
  /* One disc and one water line. Colour changes at the four daily boundaries. */
  fill_disc(c, 1640, 250, 64, sun[phase]);
  if (!phase) fill_disc(c, 1666, 228, 52, ambient_bg);
  hair(c, 1420, 1840, 390, (Color){38, 65, 77});
}

/* Kanpur, from the NOAA solar approximation. Accurate to a couple of minutes,
 * and it costs no network request. n is the 1-based day of the year. */
static int ambient_sun_minutes(int n, int *rise_min, int *set_min) {
  if (n < 1) n = 1;
  if (n > 366) n = 366;
  const double lat = 26.4499, lng = 80.3319, tz = 5.5;
  double g = 2 * M_PI / 365.0 * (n - 1);
  double eq = 229.18 * (0.000075 + 0.001868 * cos(g) - 0.032077 * sin(g)
                        - 0.014615 * cos(2 * g) - 0.040849 * sin(2 * g));
  double decl = 0.006918 - 0.399912 * cos(g) + 0.070257 * sin(g)
                - 0.006758 * cos(2 * g) + 0.000907 * sin(2 * g)
                - 0.002697 * cos(3 * g) + 0.00148 * sin(3 * g);
  double latr = lat * M_PI / 180.0;
  double cos_ha = cos(90.833 * M_PI / 180.0) / (cos(latr) * cos(decl)) - tan(latr) * tan(decl);
  if (cos_ha > 1 || cos_ha < -1) return 0;
  double ha = acos(cos_ha);
  double noon = 720.0 - 4.0 * lng - eq + tz * 60.0;
  *rise_min = (int)lround(noon - ha * (180.0 / M_PI) * 4.0);
  *set_min = (int)lround(noon + ha * (180.0 / M_PI) * 4.0);
  return 1;
}

static void ambient_light_text(char *out, size_t n, const struct tm *tm) {
  int rise, set;
  out[0] = 0;
  if (!ambient_sun_minutes(tm->tm_yday + 1, &rise, &set)) return;
  int now = tm->tm_hour * 60 + tm->tm_min;
  if (now >= rise && now < set)
    snprintf(out, n, "light until %02d:%02d", set / 60, set % 60);
  else
    snprintf(out, n, "sunrise at %02d:%02d", rise / 60, rise % 60);
}

static void ambient_clock(Canvas *c, const struct tm *tm) {
  char stamp[8], date[80], light[64];
  snprintf(stamp, sizeof stamp, "%02d:%02d", tm->tm_hour, tm->tm_min);
  snprintf(date, sizeof date, "%s, %d %s", WDAY[tm->tm_wday], tm->tm_mday, MON[tm->tm_mon]);
  ambient_light_text(light, sizeof light, tm);
  draw_text(c, 110, 168, stamp, 220, ambient_fg, -6);
  draw_text(c, 116, 430, date, 32, ambient_dim, 0);
  if (light[0]) draw_text(c, 116, 484, light, 32, ambient_fg, 0);
}

/* Draw only when a turn needs a glance. Idle is the usual state, so it stays blank. */
static int ambient_status(char *headline, size_t head_size, char *detail, size_t detail_size, Color *color) {
  *color = ambient_dim;
  headline[0] = 0;
  detail[0] = 0;
  if (g_shared_frame) {
    snprintf(headline, head_size, "Aviato");
    return 1;
  }
  if (!g_agent.present) return 0;
  int state = agent_current_state();
  if (state == AGENT_IDLE || state == AGENT_STALE) return 0;
  switch (state) {
    case AGENT_RUNNING:
      snprintf(headline, head_size, "%d turn%s running", g_agent.running, g_agent.running == 1 ? "" : "s");
      *color = (Color){157,195,179};
      break;
    case AGENT_APPROVAL:
      snprintf(headline, head_size, "Waiting for you");
      *color = (Color){230,175,106};
      break;
    case AGENT_STOPPED:
      snprintf(headline, head_size, "Turn finished");
      break;
    case AGENT_ERROR:
      snprintf(headline, head_size, "Request failed");
      *color = (Color){223,142,122};
      break;
    default:
      snprintf(headline, head_size, "Status is old");
      break;
  }
  time_t ist = g_agent.updated_at + 19800;
  struct tm local;
  gmtime_r(&ist, &local);
  char stamp[48];
  strftime(stamp, sizeof stamp, "%d %b %H:%M IST", &local);
  snprintf(detail, detail_size, "%s", stamp);
  return 1;
}

static time_t g_btc_updated;
static int g_btc_failed;

static int fetch_ambient_btc(void) {
  if (!curl_open(&g_coin)) return 0;
  curl_easy_setopt(g_coin, CURLOPT_TIMEOUT, 2L);
  curl_easy_setopt(g_coin, CURLOPT_CONNECTTIMEOUT, 2L);
  if (!curl_fetch(g_coin, "https://api.binance.com/api/v3/ticker/price?symbol=BTCUSDT")) return 0;
  JsonValue root = {json_space(g_body.data), g_body.data + g_body.n};
  if (!json_literal(json_member(root, "symbol"), "\"BTCUSDT\"")) return 0;
  double price;
  if (!json_double_span(g_body.data, g_body.data + g_body.n, "\"price\"", &price) ||
      !take_price(&g_markets.btc, price, 1, 10000000)) return 0;
  g_btc_updated = time(NULL);
  return 1;
}

static void refresh_ambient_btc(void) {
  if (mono() < g_coin_refresh.next) return;
  int ok = fetch_ambient_btc();
  g_btc_failed = !ok;
  refresh_done(&g_coin_refresh, mono(), ok);
}

static int ambient_quote_slot(const struct tm *tm) {
  return saying_slot(tm);
}

static void ambient_agent_at(Canvas *c, int y, const char *headline, const char *detail, Color color) {
  if (g_shared_frame) fill_disc(c, 124, y + 22, 6, color);
  else draw_agent_face(c, 130, y + 25, 48, agent_current_state(), color);
  draw_text(c, 174, y, headline, 30, color, 0);
  if (detail[0]) draw_text(c, 174, y + 42, detail, 20, ambient_dim, 0);
}

static void ambient_btc_text(char *price, size_t price_size, char *detail, size_t detail_size) {
  snprintf(price, price_size, "BTC unavailable");
  snprintf(detail, detail_size, "Binance");
  if (!g_markets.btc.ok) return;
  char number[64];
  fmt_quote(number, sizeof number, g_markets.btc.value, 0);
  int limit = (int)g_coin_refresh.interval + 45;
  if (limit < 120) limit = 120;
  int stale = g_btc_failed || time(NULL) - g_btc_updated > limit;
  snprintf(price, price_size, "btc %s%s", number, stale ? " stale" : "");
  snprintf(detail, detail_size, "%s", stale ? "stale" : "Binance");
}

static void ambient_room_text(char *out, size_t n) {
  double free_gb = 0, total_gb = 0;
  struct statvfs st;
  if (statvfs("/", &st) == 0 && st.f_blocks > 0 && st.f_frsize > 0) {
    total_gb = (double)st.f_blocks * (double)st.f_frsize / (1024.0 * 1024.0 * 1024.0);
    free_gb = (double)st.f_bavail * (double)st.f_frsize / (1024.0 * 1024.0 * 1024.0);
  }
  int cpu = g_system.cpu;
  double used = g_system.used, mem = g_system.total, load = g_system.load;
  int disk_low = total_gb > 0 && (free_gb < 15 || free_gb / total_gb < 0.10);
  int mem_tight = mem > 0 && used / mem >= 0.90;
  if (disk_low) {
    snprintf(out, n, "disk is low, %.0f GB free", free_gb);
    return;
  }
  if (mem_tight) {
    snprintf(out, n, "memory is tight, %.1f of %.0f GB", used, mem);
    return;
  }
  if (cpu >= 85) {
    snprintf(out, n, "cpu is hot, %d\u00b0", cpu);
    return;
  }
  if (cpu >= 0 && total_gb > 0)
    snprintf(out, n, "cpu %d\u00b0, load %.2f, %.1f of %.0f GB, %.0f GB free", cpu, load, used, mem, free_gb);
  else if (cpu >= 0)
    snprintf(out, n, "cpu %d\u00b0, load %.2f, %.1f of %.0f GB", cpu, load, used, mem);
  else if (total_gb > 0)
    snprintf(out, n, "load %.2f, %.1f of %.0f GB, %.0f GB free", load, used, mem, free_gb);
  else
    snprintf(out, n, "load %.2f, %.1f of %.0f GB", load, used, mem);
}

/* Quote, then anything that needs a glance, then the machine. Empty states take no space. */
static void ambient_column(Canvas *c, const struct tm *tm, int show, const char *headline,
                           const char *detail, Color color, const char *price) {
  int y = 572;
  int slot = ambient_quote_slot(tm);
  char lines[2][160];
  int count = wrap_quote(SAYINGS[slot].text, 28, 980, lines, 2);
  for (int i = 0; i < count; i++) {
    draw_text(c, 116, y, lines[i], 28, ambient_fg, 0);
    y += 38;
  }
  draw_text(c, 116, y + 4, SAYINGS[slot].author, 20, ambient_dim, 0);
  y += 72;
  if (show) {
    ambient_agent_at(c, y, headline, detail, color);
    y += detail[0] ? 84 : 48;
  }
  if (g_markets.btc.ok) {
    draw_text(c, 116, y, price, 32, ambient_fg, 0);
    y += 52;
  }
  char room[96];
  ambient_room_text(room, sizeof room);
  draw_text(c, 116, y, room, 22, ambient_dim, 0);
}

static void paint_ambient(Canvas *c, const struct tm *tm) {
  fill(c, ambient_bg);
  ambient_scene(c, tm);
  ambient_clock(c, tm);
  char price[96], checked[96], headline[128], detail[128];
  Color color;
  ambient_btc_text(price, sizeof price, checked, sizeof checked);
  int show = ambient_status(headline, sizeof headline, detail, sizeof detail, &color);
  ambient_column(c, tm, show, headline, detail, color, price);
}

static int render_ambient(Canvas *c, RenderState *state, const struct tm *tm) {
  char headline[128], detail[128], status[384], system[96];
  Color color;
  int show = ambient_status(headline, sizeof headline, detail, sizeof detail, &color);
  snprintf(status, sizeof status, "%d|%s|%s", show, headline, detail);
  ambient_room_text(system, sizeof system);
  char price[96], checked[96], btc[256];
  ambient_btc_text(price, sizeof price, checked, sizeof checked);
  snprintf(btc, sizeof btc, "%s|%s", price, checked);
  int full = !state->initialized || ambient_phase(tm) != ambient_phase(&state->tm);
  int clock = full || tm->tm_year != state->tm.tm_year || tm->tm_yday != state->tm.tm_yday ||
              tm->tm_hour != state->tm.tm_hour || tm->tm_min != state->tm.tm_min;
  int agent = full || strcmp(status, state->markets);
  int metrics = full || strcmp(system, state->system);
  int quote = full || ambient_quote_slot(tm) != ambient_quote_slot(&state->tm);
  int bitcoin = full || strcmp(btc, state->portfolio);
  if (full) paint_ambient(c, tm);
  else {
    if (clock) { clear_rect(c, (Rect){90,150,1280,548}, ambient_bg); ambient_clock(c, tm); }
    if (quote || bitcoin || agent || metrics) {
      clear_rect(c, (Rect){90,548,1280,1040}, ambient_bg);
      ambient_column(c, tm, show, headline, detail, color, price);
    }
  }
  state->initialized = 1;
  state->tm = *tm;
  snprintf(state->markets, sizeof state->markets, "%s", status);
  snprintf(state->system, sizeof state->system, "%s", system);
  snprintf(state->portfolio, sizeof state->portfolio, "%s", btc);
  return full || clock || agent || metrics || quote || bitcoin;
}
#endif
