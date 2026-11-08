#define clock_gettime test_clock_gettime
#define time test_time
#define main aviato_application_main
#include "../aviato-screen.c"
#undef main
#undef time
#undef clock_gettime
#include <assert.h>
#include <stdarg.h>

static double test_now = 10000;
static time_t test_wall = 1793016000;
static int coin_requests, metal_requests, weather_requests, fail_requests;
typedef struct { const char *url; long timeout, connect_timeout; } TestHandle;
static TestHandle handles[8];
static int handle_count;

int test_clock_gettime(clockid_t clock, struct timespec *ts) {
  (void)clock;
  ts->tv_sec = (time_t)test_now;
  ts->tv_nsec = (long)((test_now - ts->tv_sec) * 1e9);
  return 0;
}

time_t test_time(time_t *out) {
  if (out) *out = test_wall;
  return test_wall;
}

CURL *curl_easy_init(void) {
  assert(handle_count < 8);
  return (CURL *)&handles[handle_count++];
}

int curl_easy_setopt(CURL *c, int option, ...) {
  va_list args;
  va_start(args, option);
  if (option == CURLOPT_URL) ((TestHandle *)c)->url = va_arg(args, const char *);
  if (option == CURLOPT_TIMEOUT) ((TestHandle *)c)->timeout = va_arg(args, long);
  if (option == CURLOPT_CONNECTTIMEOUT) ((TestHandle *)c)->connect_timeout = va_arg(args, long);
  va_end(args);
  return 0;
}

int curl_easy_perform(CURL *c) {
  const char *url = ((TestHandle *)c)->url;
  const char *json;
  if (strstr(url, "api.coinbase.com/v2/prices/BTC-USD/spot")) {
    coin_requests++;
    json = "{\"data\":{\"base\":\"BTC\",\"currency\":\"USD\",\"amount\":\"80391.70\"}}";
  } else if (strstr(url, "ticker/price?symbol=BTCUSDT")) {
    coin_requests++;
    json = "{\"symbol\":\"BTCUSDT\",\"price\":\"80391.70\"}";
  } else if (strstr(url, "binance")) {
    coin_requests++;
    json = "[{\"symbol\":\"BTCUSDT\",\"lastPrice\":\"100000\",\"priceChangePercent\":\"1.2\"},"
           "{\"symbol\":\"ETHUSDT\",\"lastPrice\":\"4000\",\"priceChangePercent\":\"-0.4\"}]";
  } else if (strstr(url, "k3shav.me/api/spotify")) {
    json = "{\"isLive\":false,\"title\":\"Not listening right now\",\"artist\":\"Spotify offline\",\"albumArt\":\"\"}";
  } else if (strstr(url, "open-meteo")) {
    weather_requests++;
    json = "{\"current\":{\"temperature_2m\":25.4,\"weather_code\":0},\"daily\":{"
           "\"time\":[\"2026-10-26\",\"2026-10-27\"],\"temperature_2m_max\":[30,31],"
           "\"temperature_2m_min\":[20,21]}}";
  } else {
    metal_requests++;
    json = strstr(url, "XAG") || strstr(url, "SI=F") ? "{\"price\":50}" : "{\"price\":4000}";
  }
  if (fail_requests) return 28;
  return curl_write((char *)json, 1, strlen(json), &g_body) == strlen(json) ? 0 : 23;
}

void curl_easy_cleanup(CURL *c) { (void)c; }

int curl_easy_getinfo(CURL *c, int option, ...) {
  va_list args;
  va_start(args, option);
  if (option == CURLINFO_RESPONSE_CODE) {
    long *http = va_arg(args, long *);
    /* The stub returns a JSON body, so the screen sees a successful response. */
    if (http) *http = 200;
  }
  va_end(args);
  (void)c;
  return 0;
}

static int frame_checks;
static void test_portfolio_cache(void) {
  const char *valid = "{\"schema\":1,\"status\":\"ok\",\"currency\":\"INR\",\"updated_at\":1000,"
      "\"holdings_value\":12500.25,\"holdings_pnl\":100,\"cash\":900,\"positions_pnl\":-50,\"unpriced\":0}";
  Portfolio p = {0};
  assert(portfolio_parse(valid, &p, 1100));
  assert(p.present && p.holdings == 12500.25 && p.cash == 900);
  Portfolio saved = p;
  const char *bad[] = {"{}", "{\"schema\":1,\"status\":\"ok\"}",
    "{\"schema\":1,\"status\":\"stale\",\"currency\":\"INR\",\"updated_at\":99999}",
    "{\"schema\":1,\"status\":\"ok\",\"cash\":NaN}"};
  for (size_t i=0; i<sizeof bad/sizeof bad[0]; i++) {
    assert(!portfolio_parse(bad[i], &p, 1100));
    assert(!memcmp(&p, &saved, sizeof p));
  }
  char line[384];
  portfolio_text(&p, line, sizeof line, 1100, 1);
  assert(strstr(line,"12500.25") && strstr(line,"INR"));
  portfolio_text(&p, line, sizeof line, 1400, 1);
  assert(strstr(line,"stale"));
  portfolio_text(&p, line, sizeof line, 1100, 0);
  assert(!line[0] && "shared screens must hide all portfolio content");
  char path[] = "/tmp/aviato-cache-test-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0 && write(fd, valid, strlen(valid)) == (ssize_t)strlen(valid));
  close(fd);
  p = (Portfolio){0};
  assert(portfolio_load(path, &p, 1100));
  assert(chmod(path,0644) == 0);
  assert(!portfolio_load(path, &p, 1100));
  assert(p.status == PORT_STALE);
  unlink(path);
  assert(!portfolio_load(path, &p, 1100) && !p.present);
  puts("PASS: private portfolio cache validation, freshness and hidden shared values");
}

static void test_display_handover(void) {
  char path[] = "/tmp/aviato-display-test-XXXXXX";
  assert(mkdtemp(path));
  Display display;
  assert(display_open(&display, path) == 0);
  Canvas fb = {.w=W, .h=H, .pitch=W*4, .fd=-1};
  fb.pix = calloc((size_t)fb.pitch * fb.h, 1);
  RenderState state = {.initialized=1};
  Canvas *target = display_begin(&display, &fb, &state);
  assert(target == &fb && !g_shared_frame);
  int media = openat(display.dirfd,"display.lock",O_RDWR);
  assert(media >= 0 && flock(media,LOCK_EX|LOCK_NB) < 0);
  assert(display_end(&display,target,0) == 0);
  assert(flock(media,LOCK_EX|LOCK_NB) == 0);
  target = display_begin(&display,&fb,&state);
  assert(target && target != &fb && !state.initialized && g_shared_frame);
  target->pix[0] = 123;
  assert(fb.pix[0] == 0 && "media rendering must never write framebuffer");
  assert(display_end(&display,target,1) == 0);
  int frame = openat(display.dirfd,"screensaver.bmp",O_RDONLY);
  char signature[2];
  assert(frame >= 0 && read(frame,signature,2)==2 && !memcmp(signature,"BM",2));
  close(frame);
  struct stat first, second;
  assert(fstatat(display.dirfd,"screensaver.bmp",&first,0)==0);
  assert(display_end(&display,target,0) == 0);
  assert(fstatat(display.dirfd,"screensaver.bmp",&second,0)==0 && first.st_ino==second.st_ino);
  assert(flock(media,LOCK_UN)==0);
  state.initialized=1;
  assert(display_begin(&display,&fb,&state)==&fb && !state.initialized && !g_shared_frame);
  assert(!display.offscreen.pix && "native idle releases media frame memory");
  assert(display_end(&display,&fb,0)==0);
  close(media);
  unlinkat(display.dirfd,"native.pid",0);
  unlinkat(display.dirfd,"display.lock",0);
  close(display.lockfd); close(display.dirfd);
  rmdir(path); free(fb.pix);
  puts("PASS: display lock handover, atomic frames, unchanged frame skip and recovery");
}
static void check_frame(Canvas *actual, RenderState *state, struct tm *tm, Weather *weather) {
  Canvas expected = {.w=W, .h=H, .pitch=W*4, .fd=-1};
  size_t bytes = (size_t)expected.pitch * expected.h;
  expected.pix = calloc(bytes, 1);
  assert(expected.pix);
  paint(&expected, tm, weather);
  render_incremental(actual, state, tm, weather);
  for (size_t i = 0; i < bytes; i++) {
    if (actual->pix[i] != expected.pix[i]) {
      fprintf(stderr, "frame mismatch at %02d:%02d pixel %zu,%zu channel %zu (%u != %u)\n",
              tm->tm_hour, tm->tm_min, (i % expected.pitch) / 4, i / expected.pitch, i % 4,
              actual->pix[i], expected.pix[i]);
      abort();
    }
  }
  free(expected.pix);
  frame_checks++;
}


static void test_minimal_quotes(void) {
  Quote quote = {0};
  assert(minimal_usd_parse("{\"data\":{\"base\":\"BTC\",\"currency\":\"USD\",\"amount\":\"80391.70\"}}", &quote));
  assert(quote.ok && quote.value == 80391.70);
  const char *bad[] = {"{}", "{\"data\":{\"base\":\"BTC\",\"currency\":\"USDT\",\"amount\":\"99\"}}",
    "{\"data\":{\"base\":\"ETH\",\"currency\":\"USD\",\"amount\":\"99\"}}"};
  Quote saved = quote;
  for (size_t i=0; i<sizeof bad/sizeof bad[0]; i++) {
    assert(!minimal_usd_parse(bad[i], &quote)); assert(!memcmp(&quote, &saved, sizeof quote));
  }
  const char *amounts[] = {"0", "-1", "+1", "NaN", "Infinity", "1e4", " 23", "23 ", "1.", ".2", "10000001", "1x", ""};
  for (size_t i=0; i<sizeof amounts/sizeof amounts[0]; i++) {
    char body[256];
    snprintf(body, sizeof body, "{\"data\":{\"base\":\"BTC\",\"currency\":\"USD\",\"amount\":\"%s\"}}", amounts[i]);
    assert(!minimal_usd_parse(body, &quote)); assert(!memcmp(&quote, &saved, sizeof quote));
  }
  assert(!minimal_usd_parse("{\"data\":{\"base\":\"BTC\",\"currency\":\"USD\",\"amount\":42}}", &quote));
  char top[384], middle[384], health[96];
  struct tm tm = {.tm_hour=12}; Weather weather = {0};
  g_system.total = 8; g_system.cpu = -1;
  minimal_lines(&tm, &weather, top, middle, health);
  assert(!strstr(health, "-1"));
  assert(strstr(top, "12:00"));
  assert(!strstr(middle, "LOCAL"));
  assert(strstr(middle, "idle"));
  assert(minimal_bg.r == 21 && minimal_bg.g == 25 && minimal_bg.b == 22);
  g_system = (SystemInfo){0};
  assert(fetch_minimal_btc());
  assert(((TestHandle *)g_coin)->timeout == 8 && ((TestHandle *)g_coin)->connect_timeout == 5);
  coin_requests = 0;
  g_markets = (Markets){0};
  g_btc_updated = 0;
}

static void test_quote_schedule(void) {
  struct tm tm = {.tm_yday=100, .tm_hour=10, .tm_min=0};
  int first = saying_slot(&tm);
  tm.tm_min = 9;
  assert(saying_slot(&tm) == first);
  tm.tm_min = 10;
  int second = saying_slot(&tm);
  assert(second != first);
  tm.tm_min = 19;
  assert(saying_slot(&tm) == second);
  tm.tm_min = 20;
  assert(saying_slot(&tm) != second);
  tm.tm_hour = 11;
  tm.tm_min = 0;
  assert(saying_slot(&tm) != second);
  assert(NSAYINGS >= 18);
  struct tm hold = {.tm_yday=0, .tm_hour=0, .tm_min=0, .tm_sec=0};
  int held = quote_hold_slot(&hold);
  hold.tm_sec = 44;
  assert(quote_hold_slot(&hold) == held);
  hold.tm_sec = 45;
  assert(quote_hold_slot(&hold) != held);
}

static void test_spotify_art_cache(void) {
  static const unsigned char bmp[] = {
    'B','M',70,0,0,0,0,0,0,0,54,0,0,0,
    40,0,0,0,2,0,0,0,2,0,0,0,1,0,24,0,0,0,0,0,16,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,255,0,255,0,0,0,255,0,0,255,255,255,0,0
  };
  const char *saved_home_value = getenv("HOME");
  char *saved_home = saved_home_value ? strdup(saved_home_value) : NULL;
  char home[] = "/tmp/aviato-spotify-cache-XXXXXX";
  assert(mkdtemp(home));
  char parent[128], dir[160], cache[200];
  snprintf(parent, sizeof parent, "%s/.cache", home);
  assert(mkdir(parent, 0700) == 0);
  assert(setenv("HOME", home, 1) == 0);
  snprintf(dir, sizeof dir, "%s/aviato-screen", parent);
  snprintf(cache, sizeof cache, "%s/spotify-cover.bin", dir);

  NowPlaying before = g_now;
  Color old_accent = g_spotify_accent;
  const char *url = "https://i.scdn.co/image/cache-test";
  assert(spotify_decode_art(bmp, sizeof bmp));
  assert(g_now.art_ok == 1);
  spotify_cache_save(url, bmp, sizeof bmp);
  memset(g_now.art, 0, sizeof g_now.art);
  g_now.art_ok = -1;
  assert(spotify_cache_load(url));
  assert(g_now.art_ok == 1 && g_now.art[0] + g_now.art[1] + g_now.art[2] > 0);
  g_now.art_ok = -1;
  assert(!spotify_cache_load("https://i.scdn.co/image/another-track"));

  assert(unlink(cache) == 0);
  assert(rmdir(dir) == 0);
  assert(rmdir(parent) == 0);
  assert(rmdir(home) == 0);
  if (saved_home) { assert(setenv("HOME", saved_home, 1) == 0); free(saved_home); }
  else unsetenv("HOME");
  g_now = before;
  g_spotify_accent = old_accent;
}

static void test_adaptive_display(Canvas *canvas) {
  setenv("SCREEN_PROFILE", "adaptive", 1);
  configure_ambient();
  assert(g_minimal && !g_ambient);
  unsetenv("SCREEN_PROFILE");
  RenderState state = {0};
  struct tm tm = {.tm_year=126,.tm_mon=10,.tm_mday=8,.tm_wday=0,.tm_yday=311,.tm_hour=14,.tm_min=5};
  Weather weather = {.ok=1,.temp=26,.code=0};
  g_now = (NowPlaying){0}; g_spotify_stale = 0;
  g_minimal_weather_updated = test_wall;
  check_frame(canvas, &state, &tm, &weather);
  assert(!render_incremental(canvas, &state, &tm, &weather));
  g_agent = (AgentStatus){.present=1,.state=AGENT_RUNNING,.updated_at=test_wall,.expires_at=test_wall+900};
  update_face_animation(test_now);
  assert(g_face_deadline == 0);
  assert(!render_incremental(canvas, &state, &tm, &weather));
  tm.tm_min=10; check_frame(canvas, &state, &tm, &weather);
  assert(spotify_apply("{\"isLive\":true,\"title\":\"Teardrop\",\"artist\":\"Massive Attack\",\"album\":\"Mezzanine\"}"));
  check_frame(canvas, &state, &tm, &weather);
  unsigned char red[] = {210,30,40};
  assert(spotify_scale(red, 1, 1)); g_now.art_ok=1;
  check_frame(canvas, &state, &tm, &weather);
  size_t colored=0;
  for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
    unsigned char *p=canvas->pix+(size_t)y*canvas->pitch+x*4;
    if (p[2]==210 && p[1]==30 && p[0]==40) colored++;
  }
  assert(colored >= 600u*600u);
  memset(g_now.title, 'W', sizeof g_now.title-1);
  g_now.title[sizeof g_now.title-1]=0;
  check_frame(canvas, &state, &tm, &weather);
  g_now.title[120]='M'; check_frame(canvas, &state, &tm, &weather);
  g_spotify_stale=1; check_frame(canvas, &state, &tm, &weather);
  tm.tm_hour=22; tm.tm_min=59; check_frame(canvas, &state, &tm, &weather);
  tm.tm_hour=23; tm.tm_min=0; check_frame(canvas, &state, &tm, &weather);
  weather.temp++; g_markets.btc.value++; g_spotify_stale=0;
  assert(!render_incremental(canvas, &state, &tm, &weather));
  assert(spotify_apply("{\"isLive\":false}"));
  assert(!render_incremental(canvas, &state, &tm, &weather));
  tm.tm_hour=0; tm.tm_mday++; tm.tm_yday++; tm.tm_wday++;
  check_frame(canvas, &state, &tm, &weather);
  tm.tm_hour=7; tm.tm_min=59; check_frame(canvas, &state, &tm, &weather);
  tm.tm_hour=8; tm.tm_min=0; check_frame(canvas, &state, &tm, &weather);
  assert(spotify_apply("{\"isLive\":true,\"title\":\"Caf\\u00e9\",\"artist\":\"Massive Attack\"}"));
  check_frame(canvas, &state, &tm, &weather);
  assert(spotify_apply("{\"isLive\":false}"));
  check_frame(canvas, &state, &tm, &weather);
  Canvas clipped={.w=640,.h=480,.pitch=640*4,.fd=-1};
  clipped.pix=calloc((size_t)clipped.pitch*clipped.h,1); assert(clipped.pix);
  g_now.playing=1; g_now.art_ok=1; strcpy(g_now.title,"Clipped canvas");
  paint(&clipped,&tm,&weather);
  free(clipped.pix);
  g_now=(NowPlaying){0}; g_agent=(AgentStatus){0};
  g_profile=PROFILE_FOCUS;
}

int main(void) {
  test_minimal_quotes();
  test_quote_schedule();
  test_spotify_art_cache();
  setenv("SCREEN_PROFILE", "minimal", 1);
  configure_ambient();
  configure_refresh();
  assert(g_coin_refresh.interval == 60);
  assert(g_weather_refresh.interval == 900);
  unsetenv("SCREEN_PROFILE");
  g_ambient = 0;
  g_minimal = 0;
  configure_refresh();
  test_portfolio_cache();
  test_display_handover();
  refresh_prices(1, 1);
  assert(coin_requests == 1 && metal_requests == 2);
  test_now += 6;
  refresh_prices(1, 0);
  assert(coin_requests == 1 && "crypto must stay asleep until its 60-second deadline");
  puts("PASS: refresh cadence");
  test_now += 54;
  refresh_prices(1, 0);
  assert(coin_requests == 2 && metal_requests == 2);
  fail_requests = 1;
  test_now += 60;
  refresh_prices(1, 0);
  assert(coin_requests == 3 && g_markets.btc.ok && g_markets.btc.value == 100000);
  assert(g_coin_refresh.next == test_now + 120);
  test_now += 60;
  refresh_prices(1, 0);
  assert(coin_requests == 3);
  fail_requests = 0;
  test_now += 60;
  refresh_prices(1, 0);
  assert(coin_requests == 4 && g_coin_refresh.next == test_now + 60);
  int previous = coin_requests;
  test_now += 4000;
  refresh_prices(0, 0);
  assert(coin_requests == previous);
  Refresh retry = {60, 0, 60};
  for (int i = 0; i < 20; i++) refresh_done(&retry, 100, 0);
  assert(retry.next == 3700);
  refresh_done(&retry, 100, 1);
  assert(retry.next == 160);
  puts("PASS: failure backoff, recovery, night pause, last good values");

  Weather weather = {0};
  assert(load_weather(&weather) == 0 && weather.ok && weather.temp == 25 && weather.n == 2);
  Weather saved = weather;
  assert(parse_weather("{\"temperature_2m\":25", &weather) == -1);
  assert(!memcmp(&weather, &saved, sizeof weather));
  assert(parse_weather("{\"current\":{\"temperature_2m\":25,\"weather_code\":0},\"daily\":{"
                       "\"time\":[\"2026-10-26\",\"2026-10-27\"],\"temperature_2m_max\":[null,31],"
                       "\"temperature_2m_min\":[20,21]}}", &weather) == -1);
  assert(!memcmp(&weather, &saved, sizeof weather));
  assert(parse_weather("{\"current\":{\"temperature_2m\":25,\"weather_code\":0},\"daily\":{"
                       "\"time\":[\"2026-10-26\",\"2026-10-27\"],\"temperature_2m_max\":[30],"
                       "\"temperature_2m_min\":[20,21]}}", &weather) == -1);
  assert(!memcmp(&weather, &saved, sizeof weather));
  assert(parse_weather("{\"temperature_2m\":NaN}", &weather) == -1);
  assert(!memcmp(&weather, &saved, sizeof weather));
  fail_requests = 1;
  assert(load_weather(&weather) == -1 && !memcmp(&weather, &saved, sizeof weather));
  fail_requests = 0;
  Body body = {0};
  char byte = 'x';
  assert(curl_write(&byte, 1, BODY_LIMIT + 1, &body) == 0 && !body.data);
  assert(curl_write(&byte, (size_t)-1, 2, &body) == 0 && !body.data);
  assert(curl_write(&byte, 1, 1, &body) == 1 && !strcmp(body.data, "x"));
  free(body.data);
  setenv("SCREEN_CRYPTO_INTERVAL", "5", 1);
  assert(env_interval("SCREEN_CRYPTO_INTERVAL", 60, 60) == 60);
  setenv("SCREEN_CRYPTO_INTERVAL", "120", 1);
  assert(env_interval("SCREEN_CRYPTO_INTERVAL", 60, 60) == 120);
  setenv("SCREEN_CRYPTO_INTERVAL", "999999999999999999999", 1);
  assert(env_interval("SCREEN_CRYPTO_INTERVAL", 60, 60) == 60);
  unsetenv("SCREEN_CRYPTO_INTERVAL");
  struct tm tm = {.tm_hour = 12, .tm_sec = 55};
  g_weather_refresh.next = g_coin_refresh.next = g_metal_refresh.next = g_system_refresh.next = test_now + 60;
  assert(next_delay(&tm, test_now) == 5);
  g_coin_refresh.next = test_now + 2;
  assert(next_delay(&tm, test_now) == 2);
  tm.tm_hour = 2;
  assert(next_delay(&tm, test_now) == 5);
  struct tm displayed = tm;
  displayed.tm_min = 34;
  displayed.tm_sec = 59;
  struct tm finished = displayed;
  finished.tm_min = 35;
  finished.tm_sec = 0;
  assert(frame_delay(&displayed, &finished, test_now) == 0);
  assert(frame_delay(&finished, &finished, test_now) > 0);
  const char *valid_json[] = {"{}", "[]", " null ", "{\"x\":[true,false,null,1,-2.3e+4,\"a\\u0020b\"]}"};
  const char *invalid_json[] = {"", "{", "[1,]", "{\"x\":}", "01", "1e", "true extra", "\"bad\\q\"", "\"\\u00\"", "{\"x\":NaN}", "{\"x\":1}suffix"};
  for (size_t i = 0; i < sizeof valid_json / sizeof valid_json[0]; i++) assert(json_document(valid_json[i]));
  for (size_t i = 0; i < sizeof invalid_json / sizeof invalid_json[0]; i++) assert(!json_document(invalid_json[i]));
  puts("PASS: weather, response bounds, configuration, deadlines");

  assert(open_font() == 0);
  Canvas canvas = {.w = W, .h = H, .pitch = W * 4, .fd = -1};
  canvas.pix = calloc((size_t)canvas.pitch * canvas.h, 1);
  assert(canvas.pix);
  RenderState state = {0};
  tm = (struct tm){.tm_year=126, .tm_mon=9, .tm_mday=26, .tm_wday=1, .tm_hour=12};
  assert(render_incremental(&canvas, &state, &tm, &weather));
  assert(!render_incremental(&canvas, &state, &tm, &weather) && "unchanged display must not repaint");
  check_frame(&canvas, &state, &tm, &weather);
  g_portfolio = (Portfolio){.present=1,.status=PORT_OK,.updated_at=test_wall,
    .has_values=1,.has_holdings=1,.holdings=123456.78,.cash=900,.positions=-50};
  check_frame(&canvas,&state,&tm,&weather);
  g_portfolio.holdings=1; g_portfolio.cash=0;
  check_frame(&canvas,&state,&tm,&weather);
  g_shared_frame=1;
  check_frame(&canvas,&state,&tm,&weather);
  g_shared_frame=0;
  check_frame(&canvas,&state,&tm,&weather);
  g_portfolio.status=PORT_LOGIN;
  check_frame(&canvas,&state,&tm,&weather);
  g_portfolio=(Portfolio){0};
  check_frame(&canvas,&state,&tm,&weather);
  tm.tm_min++;
  check_frame(&canvas, &state, &tm, &weather);
  weather.temp = -10;
  weather.code = 95;
  check_frame(&canvas, &state, &tm, &weather);
  weather.temp = 8;
  weather.code = 0;
  weather.n = 0;
  check_frame(&canvas, &state, &tm, &weather);
  weather.ok = 0;
  check_frame(&canvas, &state, &tm, &weather);
  weather = saved;
  g_markets.btc.value = 9999;
  g_markets.btc.change = -0.01;
  g_markets.eth.ok = 0;
  g_markets.silver.value = 9.99;
  check_frame(&canvas, &state, &tm, &weather);
  g_markets.btc.value += 0.1;
  assert(!render_incremental(&canvas, &state, &tm, &weather) && "invisible price changes must not repaint");
  g_system = (SystemInfo){.cpu=100, .load=1.25, .used=9.5, .total=12};
  check_frame(&canvas, &state, &tm, &weather);
  g_system = (SystemInfo){.cpu=-1, .load=0.01, .used=1, .total=12};
  check_frame(&canvas, &state, &tm, &weather);
  /* Every quote slot and theme, plus clocks/weather shrinking and growing. */
  for (int day = 26; day <= 27; day++) {
    for (int hour = 0; hour < 24; hour++) {
      tm.tm_mday = day;
      tm.tm_yday = 298 + day - 26;
      tm.tm_wday = day == 26 ? 1 : 2;
      tm.tm_hour = hour;
      tm.tm_min = 0;
      check_frame(&canvas, &state, &tm, &weather);
      tm.tm_min = 59;
      weather.temp = hour - 10;
      weather.code = hour % 2 ? 0 : 3;
      check_frame(&canvas, &state, &tm, &weather);
    }
  }
  /* Ambient dirty rectangles must erase long states and match full frames. */
  g_ambient = 1;
  state.initialized = 0;
  for (int hour = 0; hour < 24; hour++) {
    tm.tm_hour = hour;
    tm.tm_min = hour;
    for (int status = AGENT_IDLE; status <= AGENT_STALE; status++) {
      g_agent = (AgentStatus){.present=1, .state=status, .updated_at=test_wall,
                            .running=2, .waiting=1};
      check_frame(&canvas, &state, &tm, &weather);
      assert(!render_incremental(&canvas, &state, &tm, &weather));
    }
  }
  g_shared_frame = 1;
  check_frame(&canvas, &state, &tm, &weather);
  g_shared_frame = 0;
  check_frame(&canvas, &state, &tm, &weather);
  g_agent.updated_at = test_wall - 901;
  check_frame(&canvas, &state, &tm, &weather);
  g_agent.updated_at = test_wall - 10;
  g_agent.expires_at = test_wall - 1;
  check_frame(&canvas, &state, &tm, &weather);
  g_agent = (AgentStatus){0};
  check_frame(&canvas, &state, &tm, &weather);
  for (int i = 0; i < 17; i++) {
    tm.tm_hour = i / 12;
    tm.tm_min = (i % 12) * 5;
    check_frame(&canvas, &state, &tm, &weather);
  }
  int before_btc = coin_requests;
  g_coin_refresh = (Refresh){60,0,60};
  fail_requests = 0;
  refresh_ambient_btc();
  assert(coin_requests == before_btc + 1 && g_markets.btc.ok);
  check_frame(&canvas, &state, &tm, &weather);
  test_now += 59;
  refresh_ambient_btc();
  assert(coin_requests == before_btc + 1);
  test_now += 1;
  refresh_ambient_btc();
  assert(coin_requests == before_btc + 2);
  fail_requests = 1;
  test_now += 60;
  refresh_ambient_btc();
  assert(g_btc_failed && g_markets.btc.ok && g_coin_refresh.next == test_now + 120);
  check_frame(&canvas, &state, &tm, &weather);
  fail_requests = 0;
  test_now += 120;
  refresh_ambient_btc();
  assert(!g_btc_failed && g_coin_refresh.next == test_now + 60);
  check_frame(&canvas, &state, &tm, &weather);
  AgentStatus parsed;
  assert(agent_parse("{\"version\":1,\"updated_at\":1793016000,\"running\":1,\"waiting\":0,\"stopped\":0,\"errors\":0,\"state\":\"running\"}", &parsed, test_wall));
  assert(parsed.state == AGENT_RUNNING && parsed.running == 1);
  assert(!agent_parse("{\"version\":1,\"state\":\"running\"}", &parsed, test_wall));
  assert(!agent_parse("{} trailing", &parsed, test_wall));
  tm.tm_sec = 55;
  g_coin_refresh.next = test_now + 60;
  assert(next_delay(&tm, test_now) == 5);
  tm.tm_sec = 0;
  assert(next_delay(&tm, test_now) == 60);
  struct tm sun = {.tm_yday = 309, .tm_hour = 21, .tm_min = 30};
  char light[80], room[128];
  ambient_light_text(light, sizeof light, &sun);
  assert(strcmp(light, "sunrise at 06:21") == 0);
  sun.tm_hour = 14;
  ambient_light_text(light, sizeof light, &sun);
  assert(strcmp(light, "light until 17:24") == 0);
  sun.tm_hour = 6;
  sun.tm_min = 21;
  ambient_light_text(light, sizeof light, &sun);
  assert(strcmp(light, "light until 17:24") == 0);
  ambient_room_text(room, sizeof room);
  assert(strstr(room, "GB"));
  g_ambient = 0; g_minimal = 1;
  state = (RenderState){0};
  g_markets.btc = (Quote){0};
  weather.ok = 0;
  check_frame(&canvas, &state, &tm, &weather);
  assert(!render_incremental(&canvas, &state, &tm, &weather));
  g_markets.btc = (Quote){.ok=1,.value=80391.70};
  g_btc_updated = test_wall; g_btc_failed = 0;
  check_frame(&canvas, &state, &tm, &weather);
  g_btc_failed = 1; check_frame(&canvas, &state, &tm, &weather);
  g_btc_failed = 0; test_wall += 346;
  check_frame(&canvas, &state, &tm, &weather);
  weather.ok = 1; weather.temp = 27;
  g_minimal_weather_updated = test_wall;
  check_frame(&canvas, &state, &tm, &weather);
  g_minimal_weather_failed = 1; check_frame(&canvas, &state, &tm, &weather);
  g_minimal_weather_failed = 0; test_wall += 1846;
  check_frame(&canvas, &state, &tm, &weather);
  for (int i=AGENT_IDLE; i<=AGENT_STALE; i++) {
    g_agent = (AgentStatus){.present=1,.state=i,.running=2,.updated_at=test_wall,.expires_at=test_wall+900};
    check_frame(&canvas, &state, &tm, &weather);
    assert(!render_incremental(&canvas, &state, &tm, &weather));
  }
  for (int h=0; h<24; h++) {
    tm.tm_hour=h; tm.tm_sec=0;
    assert(minimal_active(&tm) == (h>=8 && h<23));
    check_frame(&canvas, &state, &tm, &weather);
  }
  configure_refresh();
  assert(g_coin_refresh.interval==60 && g_weather_refresh.interval==900 && g_system_refresh.interval==60);
  g_coin_refresh.next = g_weather_refresh.next = g_system_refresh.next = 0;
  int before = coin_requests + weather_requests;
  tm.tm_hour=7; tm.tm_min=59;
  collect_minimal(&tm, &weather);
  assert(before == coin_requests + weather_requests);
  assert(next_delay(&tm, test_now)==60);
  tm.tm_hour=23; collect_minimal(&tm, &weather);
  assert(before == coin_requests + weather_requests);
  /* Freeze wall time at 08:00 IST for the collector's post-request checks. */
  test_wall = 1792981800;
  tm = now_ist(); assert(tm.tm_hour==8);
  collect_minimal(&tm, &weather);
  assert(coin_requests + weather_requests == before + 2);
  assert(g_coin_refresh.next==test_now+60 && g_weather_refresh.next==test_now+900 && g_system_refresh.next==test_now+60);
  double deadline = g_coin_refresh.next;
  test_now += 59; collect_minimal(&tm, &weather);
  assert(g_coin_refresh.next==deadline);
  test_now += 1; fail_requests=1;
  Quote retained = g_markets.btc; time_t stamp = g_btc_updated;
  collect_minimal(&tm, &weather);
  assert(g_btc_failed && !memcmp(&retained,&g_markets.btc,sizeof retained) && stamp==g_btc_updated);
  assert(g_coin_refresh.next==test_now+30);
  char price[96], source[96];
  minimal_btc_text(price, sizeof price, source, sizeof source);
  assert(strstr(price, "USD") && strstr(source, "Coinbase") && strstr(source, "stale") && strstr(source, "IST"));
  NowPlaying saved_now = g_now;
  assert(spotify_apply("{\"isLive\":false,\"title\":\"Not listening right now\",\"artist\":\"Spotify offline\",\"albumArt\":\"\"}"));
  assert(!g_now.playing);
  assert(spotify_apply("{\"isLive\":true,\"title\":\"Caf\\u00e9\",\"artist\":\"Massive Attack\",\"albumArt\":\"https://i.scdn.co/image/abc\",\"progressMs\":13000,\"durationMs\":180000,\"syncedLyrics\":\"[00:01.00] first\\n[00:12.50] second line\\n[00:40.00] later\"}"));
  assert(g_now.playing && !strcmp(g_now.title, "Café") && !strcmp(g_now.artist, "Massive Attack"));
  assert(g_lyric_n == 3 && g_lyrics[0].at_ms == 1000 && g_lyrics[1].at_ms == 12500);
  assert(!strcmp(g_lyrics[1].text, "second line"));
  g_now.progress_at = mono();
  g_now.progress_ms = 13000;
  assert(spotify_lyric_index() == 1);
  assert(spotify_apply("{\"isLive\":true,\"title\":\"Caf\\u00e9\",\"artist\":\"Massive Attack\",\"albumArt\":\"https://i.scdn.co/image/abc\"}"));
  assert(g_lyric_n == 0);
  assert(!strcmp(g_now.art_url, "https://i.scdn.co/image/abc") && g_now.art_ok == 0);
  assert(!spotify_apply("{") && g_now.playing && !strcmp(g_now.title, "Café"));
  g_now = saved_now;
  test_now += 30; collect_minimal(&tm, &weather);
  assert(g_btc_failed && g_coin_refresh.next==test_now+60);
  test_now += 60; fail_requests=0; collect_minimal(&tm,&weather);
  assert(!g_btc_failed && g_coin_refresh.next==test_now+60);
  g_minimal = 0; g_ambient = 1;
  Canvas small = {.w=640, .h=480, .pitch=640*4, .fd=-1};
  small.pix = calloc((size_t)small.pitch * small.h, 1);
  assert(small.pix);
  clear_rect(&small, (Rect){-50,-30,2000,1200}, ambient_bg);
  paint_ambient(&small, &tm);
  RenderState small_state = {0};
  render_ambient(&small, &small_state, &tm);
  tm.tm_min = (tm.tm_min + 1) % 60;
  render_ambient(&small, &small_state, &tm);
  free(small.pix);
  test_adaptive_display(&canvas);
  free(canvas.pix);
  puts("PASS: unchanged frames skip painting");
  printf("PASS: %d incremental frames exactly match full paints\n", frame_checks);
  free(g_body.data);
  free(g_ttf);
  return 0;
}
