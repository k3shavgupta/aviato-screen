/* Offline visual fixtures, using the production painters and real font. */
#define main aviato_application_main
#include "../aviato-screen.c"
#undef main

int main(int argc, char **argv) {
  if (argc != 2) return 1;
  g_minimal = 1; g_profile = PROFILE_ADAPTIVE;
  if (open_font()) return 1;
  Canvas canvas = {.w=W, .h=H, .pitch=W*4, .fd=-1};
  canvas.pix = calloc((size_t)canvas.pitch * canvas.h, 1);
  if (!canvas.pix) return 1;
  struct tm tm = {.tm_year=126,.tm_mon=10,.tm_mday=8,.tm_wday=0,.tm_yday=311,.tm_hour=14,.tm_min=32};
  Weather weather = {.ok=1,.temp=26,.code=0};
  g_minimal_weather_updated = time(NULL);
  g_markets.btc = (Quote){.ok=1,.value=75692.80}; g_btc_updated=time(NULL);
  paint(&canvas,&tm,&weather);
  if (write_bmp("build/adaptive-idle.bmp",&canvas)) return 1;
  spotify_load_preview(argv[1]);
  if (g_now.art_ok != 1) return 1;
  strcpy(g_now.title,"Monster"); strcpy(g_now.artist,"Kanye West");
  strcpy(g_now.album,"My Beautiful Dark Twisted Fantasy");
  paint(&canvas,&tm,&weather);
  if (write_bmp("build/adaptive-music.bmp",&canvas)) return 1;
  strcpy(g_now.title,"A long track title with featured artists and an extended live version");
  g_now.art_ok=-1; g_spotify_stale=1;
  paint(&canvas,&tm,&weather);
  if (write_bmp("build/adaptive-fallback.bmp",&canvas)) return 1;
  tm.tm_hour=23; tm.tm_min=48;
  paint(&canvas,&tm,&weather);
  if (write_bmp("build/adaptive-night.bmp",&canvas)) return 1;
  free(canvas.pix); free(g_ttf);
  return 0;
}
