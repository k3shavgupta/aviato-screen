#ifndef AVIATO_JSON_READER_H
#define AVIATO_JSON_READER_H

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *start, *end; } JsonValue;

static const char *json_space(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  return p;
}

/* Validate structure without allocations; returned end pointers never include
 * a following delimiter. Limit nesting to keep untrusted responses bounded. */
static const char *json_value_end(const char *p, int depth) {
  if (depth > 32) return NULL;
  p = json_space(p);
  if (*p == '"') {
    p++;
    while (*p && *p != '"') {
      if ((unsigned char)*p < 32) return NULL;
      if (*p++ == '\\') {
        if (*p == 'u') {
          p++;
          for (int i = 0; i < 4; i++, p++) if (!isxdigit((unsigned char)*p)) return NULL;
        } else {
          if (!*p || !strchr("\"\\/bfnrt", *p)) return NULL;
          p++;
        }
      }
    }
    return *p == '"' ? p + 1 : NULL;
  }
  if (*p == '{' || *p == '[') {
    int object = *p == '{';
    char close = object ? '}' : ']';
    p = json_space(p + 1);
    if (*p == close) return p + 1;
    for (;;) {
      if (object) {
        if (*p != '"' || !(p = json_value_end(p, depth + 1))) return NULL;
        p = json_space(p);
        if (*p != ':') return NULL;
        p = json_space(p + 1);
      }
      if (!(p = json_value_end(p, depth + 1))) return NULL;
      p = json_space(p);
      if (*p == close) return p + 1;
      if (*p != ',') return NULL;
      p = json_space(p + 1);
    }
  }
  if (!strncmp(p, "true", 4)) return p + 4;
  if (!strncmp(p, "false", 5)) return p + 5;
  if (!strncmp(p, "null", 4)) return p + 4;
  if (*p == '-') p++;
  if (*p == '0') p++;
  else {
    if (*p < '1' || *p > '9') return NULL;
    while (isdigit((unsigned char)*p)) p++;
  }
  if (*p == '.') {
    p++;
    if (!isdigit((unsigned char)*p)) return NULL;
    while (isdigit((unsigned char)*p)) p++;
  }
  if (*p == 'e' || *p == 'E') {
    p++;
    if (*p == '+' || *p == '-') p++;
    if (!isdigit((unsigned char)*p)) return NULL;
    while (isdigit((unsigned char)*p)) p++;
  }
  return p;
}

static int json_document(const char *text) {
  const char *end = json_value_end(text, 0);
  return end && !*json_space(end);
}

/* Provider field names are plain ASCII; escaped key spellings are not matched. */
static JsonValue json_member(JsonValue object, const char *key) {
  JsonValue missing = {0};
  if (!object.start || *object.start != '{') return missing;
  const char *p = json_space(object.start + 1);
  size_t n = strlen(key);
  while (p < object.end && *p == '"') {
    const char *key_end = json_value_end(p, 0);
    if (!key_end) return missing;
    int match = (size_t)(key_end - p) == n + 2 && !strncmp(p + 1, key, n);
    const char *colon = json_space(key_end);
    if (*colon != ':') return missing;
    const char *start = json_space(colon + 1);
    const char *end = json_value_end(start, 0);
    if (!end || end > object.end) return missing;
    if (match) return (JsonValue){start, end};
    p = json_space(end);
    if (*p != ',') break;
    p = json_space(p + 1);
  }
  return missing;
}

static int json_number(JsonValue value, double lo, double hi, double *out) {
  if (!value.start || (*value.start != '-' && !isdigit((unsigned char)*value.start))) return 0;
  char *end;
  double number = strtod(value.start, &end);
  if (end != value.end || !isfinite(number) || number < lo || number > hi) return 0;
  *out = number;
  return 1;
}

static int json_elements(JsonValue array, JsonValue *items, int capacity) {
  if (!array.start || *array.start != '[') return -1;
  const char *p = json_space(array.start + 1);
  int count = 0;
  while (*p != ']') {
    if (p >= array.end || count == capacity) return -1;
    const char *end = json_value_end(p, 0);
    if (!end || end >= array.end) return -1;
    items[count++] = (JsonValue){p, end};
    p = json_space(end);
    if (*p == ']') break;
    if (*p != ',') return -1;
    p = json_space(p + 1);
  }
  return count;
}

#endif
