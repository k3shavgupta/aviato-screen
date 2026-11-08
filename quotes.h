#ifndef AVIATO_QUOTES_H
#define AVIATO_QUOTES_H

/* Keep the quote text, author, and provenance together. The display reads this
 * fixed local catalog, so changing a card never creates a network request. */
typedef struct {
  const char *text;
  const char *author;
  const char *source;
} Saying;

static const Saying SAYINGS[] = {
    {"Do things that don't scale.", "Paul Graham", "https://paulgraham.com/ds.html"},
    {"Make something people want.", "Paul Graham", "https://www.ycombinator.com/about"},
    {"Live in the future, then build what's missing.", "Paul Graham", "How to Get Startup Ideas"},
    {"Keep your identity small.", "Paul Graham", "Keep Your Identity Small"},
    {"Play long-term games with long-term people.", "Naval", "https://nav.al/long-term-games"},
    {"Escape competition through authenticity.", "Naval", "The Almanack of Naval Ravikant"},
    {"Impatience with actions, patience with results.", "Naval", "The Almanack of Naval Ravikant"},
    {"If you can't decide, the answer is no.", "Naval", "The Almanack of Naval Ravikant"},
    {"When something is important enough, you do it even if the odds are not in your favor.",
     "Elon Musk", "TED: The future we're building"},
    {"It means saying no to the hundred other good ideas.", "Steve Jobs", "Stanford commencement, 2005"},
    {"Real artists ship.", "Steve Jobs", "Insanely Great: The Life and Times of Macintosh"},
    {"Stay hungry. Stay foolish.", "Steve Jobs", "Stanford commencement, 2005"},
    {"We are stubborn on vision. We are flexible on details.", "Jeff Bezos", "2015 shareholder letter"},
    {"Competition is for losers.", "Peter Thiel", "Zero to One"},
    {"The days are long but the decades are short.", "Sam Altman",
     "https://blog.samaltman.com/the-days-are-long-but-the-decades-are-short"},
    {"Focus is a matter of deciding what things you're not going to do.", "John Carmack", "QuakeCon keynote"},
    {"If you are not embarrassed by the first version of your product, you've launched too late.",
     "Reid Hoffman", "Masters of Scale"},
    {"Software is eating the world.", "Marc Andreessen",
     "https://a16z.com/why-software-is-eating-the-world/"},
    {"We have the opportunity to play some small role in making it real.", "Dario Amodei",
     "https://darioamodei.com/essay/machines-of-loving-grace"},
};

#define NSAYINGS ((int)(sizeof SAYINGS / sizeof SAYINGS[0]))

/* One deterministic local quote slot per ten-minute block in the display's
 * local time. Keeping the schedule wall-clock based avoids a timer loop.
 * The adaptive idle room uses quote_hold_slot() instead, so this stays put. */
static int saying_slot(const struct tm *tm) {
  long slot = (long)tm->tm_yday * 144 + (long)tm->tm_hour * 6 + tm->tm_min / 10;
  if (slot < 0) slot = 0;
  return (int)(slot % NSAYINGS);
}

/* Adaptive idle only. One constant so the hold is not scattered through the painter. */
#define QUOTE_HOLD_SEC 45
#define QUOTE_FADE_MS 400
#define QUOTE_MIN_PX 56
#define QUOTE_MAX_PX 148
#define QUOTE_COL_X 1000
#define QUOTE_COL_W 816

static int quote_hold_slot(const struct tm *tm) {
  const char *forced = getenv("SCREEN_QUOTE_INDEX");
  if (forced && *forced) {
    int index = atoi(forced);
    if (index < 0) index = 0;
    return index % NSAYINGS;
  }
  long sec = (long)tm->tm_yday * 86400L + (long)tm->tm_hour * 3600L +
             (long)tm->tm_min * 60L + tm->tm_sec;
  if (sec < 0) sec = 0;
  return (int)((sec / QUOTE_HOLD_SEC) % NSAYINGS);
}

static double quote_hold_wait(const struct tm *tm) {
  int into = (int)(((long)tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec) % QUOTE_HOLD_SEC);
  int left = QUOTE_HOLD_SEC - into;
  if (left < 1) left = QUOTE_HOLD_SEC;
  return (double)left;
}

#endif
