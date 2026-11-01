// HttpRequest.h: reads one HTTP request's start line and headers, a byte at a time, so the caller can keep servicing the
// USB link between bytes. Plain C++ for test/unit_test.cpp. It keeps only what the routes use.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

class HttpRequest {
 public:
  static const size_t TARGET_MAX = 300, HOST_MAX = 64, COOKIE_MAX = 40, LINE_MAX = 400;

  char   method[8] = "";
  char   target[TARGET_MAX + 1] = "";      // the path; query starts after the ? once split() has run
  char   host[HOST_MAX + 1] = "";
  char   session[COOKIE_MAX + 1] = "";     // the s= cookie
  size_t contentLength = 0;
  bool   wantsHtml = false;                // a browser asking for a page
  bool   markedApi = false;                // the page's X-AirMaster header; another site's script cannot send it unasked
  bool   tooLong = false;

  // True once the blank line after the headers has arrived.
  bool feed(char c) {
    if (c == '\r') return false;
    if (c != '\n') {
      if (n < LINE_MAX) line[n++] = c;
      else tooLong = true;
      return false;
    }
    line[n] = 0;
    bool end = n == 0 && sawStart;
    if (n) takeLine();
    n = 0;
    return end;
  }

  const char *query() const { return q; }
  bool sawAnything() const { return sawStart; }

 private:
  char   line[LINE_MAX + 1];
  size_t n = 0;
  bool   sawStart = false;
  const char *q = nullptr;

  static bool headerIs(const char *h, const char *name) {
    size_t l = strlen(name);
    if (strncasecmp(h, name, l) || h[l] != ':') return false;
    return true;
  }
  static const char *value(const char *h) {
    const char *v = strchr(h, ':') + 1;
    while (*v == ' ') v++;
    return v;
  }

  void takeLine() {
    if (!sawStart) {
      sawStart = true;
      char *sp = strchr(line, ' ');
      if (!sp) { tooLong = true; return; }
      *sp = 0;
      if (strlen(line) >= sizeof(method)) { tooLong = true; return; }
      strcpy(method, line);
      char *path = sp + 1, *end = strchr(path, ' ');
      if (end) *end = 0;
      if (strlen(path) > TARGET_MAX) { tooLong = true; return; }
      strcpy(target, path);
      char *qm = strchr(target, '?');
      if (qm) { *qm = 0; q = qm + 1; }
      return;
    }
    if (headerIs(line, "Content-Length")) {
      contentLength = strtoul(value(line), nullptr, 10);
    } else if (headerIs(line, "Host")) {
      const char *v = value(line);
      if (strlen(v) <= HOST_MAX) strcpy(host, v);
    } else if (headerIs(line, "Accept")) {
      wantsHtml = strstr(value(line), "text/html") != nullptr;
    } else if (headerIs(line, "X-AirMaster")) {
      markedApi = true;
    } else if (headerIs(line, "Cookie")) {
      const char *v = value(line);
      while (v && *v) {
        while (*v == ' ') v++;
        if (!strncmp(v, "s=", 2)) {
          v += 2;
          size_t m = strcspn(v, ";");
          if (m <= COOKIE_MAX) { memcpy(session, v, m); session[m] = 0; }
          break;
        }
        v = strchr(v, ';');
        if (v) v++;
      }
    }
  }
};
