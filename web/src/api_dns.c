/* NeoWEB - "Block system updates": a preset for nanoDNS's config.
 *
 *   GET  /api/dns/updates                   whether the preset is on
 *   POST /api/dns/updates {"enabled": bool}  put it in or take it out
 *
 * nanoDNS (drakmor/nanoDNS, bundled with NeoHEN as payloads/nanodns.elf)
 * keeps its config in /data/nanodns/nanodns.ini and writes a default one the
 * first time it starts. Its [overrides] section maps shell-style host masks
 * to an IPv4 address; 0.0.0.0 is what its own defaults use to block a host.
 * Masks in [exceptions] are forwarded upstream and skip every override.
 *
 * The preset is a block of lines in [overrides] between two marker comments.
 * Nothing else in the file is touched: turning it off removes just that block.
 *
 * nanoDNS reads its config only when it starts (main.c: load_config() runs
 * once; there is no reload signal). Starting nanodns.elf again kills the
 * running copy first (terminate_existing_instances()), so the change applies
 * after a restart of nanoDNS (Payloads: stop and start it) or the next boot.
 * And it only filters what is asked of it: it listens on bind= (127.0.0.1 by
 * default; 0.0.0.0 for every interface), so the PS5's DNS has to point there.
 *
 * Where the host names come from:
 *   Al-Azif, "Config file for exploit host on Pi-hole" (pi-exploit-host.conf),
 *   https://gist.github.com/Al-Azif/296c7a4319c2fe3b597d96206213cdb6
 *   sections "Redirect PS5 Update PUP CDN Domains" (d*), "Redirect PS5 Update
 *   List Domains" (f*) and "Redirect PS5 Update Feature Domains" (h*), read
 *   2026-11-04. The PS4 twins of the same 39 names (*.ps4.update.playstation.net)
 *   are in djdarien/ps4updateblock (ps4-PSNupdateblock.txt), a published
 *   Pi-hole/AdGuard list "to block PSN updates".
 *   nanoDNS's own default config blocks all of *.playstation.net, which takes
 *   these in too, but users trim it to get PSN back; the preset stays put.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fsutil.h"
#include "log.h"
#include "plat.h"

#define NANODNS_INI  "/data/nanodns/nanodns.ini"
#define NANODNS_MAX  (1 << 20)
#define DNS_BEGIN    "# >>> NeoWEB: block system updates (NeoWEB writes this block; it goes with the switch)"
#define DNS_END      "# <<< NeoWEB: block system updates"

/* 13 regions x the update list (f), the PUP download CDN (d) and the
 * update feature (h) servers; see the sources above */
static const char *const k_regions[] = {"jp", "us", "eu", "kr", "uk", "mx", "au",
                                        "sa", "tw", "ru", "cn", "hk", "br"};
static const char k_kinds[] = "fdh";
#define N_REGIONS (sizeof(k_regions) / sizeof(k_regions[0]))
#define N_HOSTS   (N_REGIONS * (sizeof(k_kinds) - 1))

static void
host_name(size_t i, char *out, size_t size) {
  snprintf(out, size, "%c%s01.ps5.update.playstation.net", k_kinds[i / N_REGIONS],
           k_regions[i % N_REGIONS]);
}

/* the line without leading/trailing blanks (and the \r of CRLF files) */
static size_t
trimmed(const char *line, size_t len, const char **start) {
  while(len && isspace((unsigned char)*line)) {
    line++;
    len--;
  }
  while(len && isspace((unsigned char)line[len - 1])) {
    len--;
  }
  *start = line;
  return len;
}

static int
line_is(const char *s, size_t n, const char *what) {
  return n == strlen(what) && !memcmp(s, what, n);
}

/* "[name]" (case and blanks as nanoDNS takes them); 1 when it is a header,
 * with the name in out */
static int
section_name(const char *s, size_t n, char *out, size_t size) {
  const char *end;
  size_t k;

  if(!n || *s != '[' || !(end = memchr(s, ']', n))) {
    return 0;
  }
  k = trimmed(s + 1, (size_t)(end - s - 1), &s);
  snprintf(out, size, "%.*s", (int)(k < size ? k : size - 1), s);
  return 1;
}

static int
is_overrides(const char *name) {
  return !strcasecmp(name, "overrides") || !strcasecmp(name, "override");
}

static int
is_exceptions(const char *name) {
  return !strcasecmp(name, "exceptions") || !strcasecmp(name, "exception");
}

/* the "host=0.0.0.0" line of preset host i */
static int
is_preset_line(const char *s, size_t n) {
  char want[96];
  for(size_t i = 0; i < N_HOSTS; i++) {
    host_name(i, want, sizeof(want));
    strncat(want, "=0.0.0.0", sizeof(want) - strlen(want) - 1);
    if(line_is(s, n, want)) {
      return 1;
    }
  }
  return 0;
}

/* Shell-style mask match as nanoDNS does it: * ? [abc] [a-z] [!x] [^x];
 * both sides already in lower case */
static int
mask_match(const char *m, const char *d) {
  for(; *m; m++, d++) {
    if(*m == '*') {
      while(*m == '*') {
        m++;
      }
      if(!*m) {
        return 1;
      }
      for(; *d; d++) {
        if(mask_match(m, d)) {
          return 1;
        }
      }
      return 0;
    }
    if(!*d) {
      return 0;
    }
    if(*m == '[') {
      const char *p = m + 1;
      int neg = *p == '!' || *p == '^', hit = 0;
      p += neg;
      if(*p == ']') {
        hit = *d == ']';
        p++;
      }
      for(; *p && *p != ']'; p++) {
        unsigned char lo = (unsigned char)*p, hi = lo;
        if(p[1] == '-' && p[2] && p[2] != ']') {
          hi = (unsigned char)p[2];
          p += 2;
        }
        hit |= (unsigned char)*d >= lo && (unsigned char)*d <= hi;
      }
      if(*p != ']') { /* no closing bracket: a plain '[' */
        if(*d != '[') {
          return 0;
        }
        continue;
      }
      if(hit == neg) {
        return 0;
      }
      m = p;
      continue;
    }
    if(*m != '?' && *m != *d) {
      return 0;
    }
  }
  return !*d;
}

typedef struct dns_state {
  int  on;            /* the whole block is there */
  char bind[64];
  /* [exceptions] masks that let a preset host through */
  char bypass[8][128];
  int  nbypass;
  /* the user's own [overrides] (not the preset's) block every preset host */
  int  covered;
} dns_state_t;

static void
read_state(const char *data, size_t len, dns_state_t *st) {
  char section[32] = "";
  int in_block = 0, seen = 0, closed = 0;
  /* per preset host, the first of the user's rules that takes it (nanoDNS
   * uses the first match): 0 none, 1 blocks it (0.0.0.0), 2 something else */
  unsigned char first[N_HOSTS] = {0};

  memset(st, 0, sizeof(*st));
  snprintf(st->bind, sizeof(st->bind), "127.0.0.1");
  for(size_t at = 0; at < len;) {
    const char *nl = memchr(data + at, '\n', len - at), *s;
    size_t ll = nl ? (size_t)(nl - data - at) : len - at, n;
    char name[32];

    n = trimmed(data + at, ll, &s);
    at += ll + (nl != NULL);
    if(line_is(s, n, DNS_BEGIN)) {
      in_block = 1;
      seen = 0;
      continue;
    }
    if(in_block) {
      if(line_is(s, n, DNS_END)) {
        in_block = 0;
        closed = seen == (int)N_HOSTS;
        continue;
      }
      if(is_preset_line(s, n)) {
        seen++;
        continue;
      }
      /* a line the user put inside the block is theirs: read it as such */
    }
    if(!n || *s == '#' || *s == ';') {
      continue;
    }
    if(section_name(s, n, name, sizeof(name))) {
      /* nanoDNS reads any other section as no section at all */
      snprintf(section, sizeof(section), "%s",
               is_overrides(name) || is_exceptions(name) || !strcasecmp(name, "general") ||
               !strcasecmp(name, "settings") ? name : "");
      continue;
    }
    /* what nanoDNS takes from the line: up to a # or ; comment */
    char item[160];
    size_t k = strcspn(s, "#;\n");
    k = trimmed(s, k < n ? k : n, &s);
    if(!k || k >= sizeof(item)) {
      continue;
    }
    memcpy(item, s, k);
    item[k] = 0;
    if(is_overrides(section)) {
      char *eq = strchr(item, '='), host[96];
      const char *mask, *ip;
      size_t mk, ik;
      if(!eq) {
        continue;
      }
      *eq = 0;
      mk = trimmed(item, strlen(item), &mask);
      ik = trimmed(eq + 1, strlen(eq + 1), &ip);
      if(!mk || !ik) {
        continue;
      }
      char lmask[160];
      for(size_t j = 0; j < mk; j++) {
        lmask[j] = (char)tolower((unsigned char)mask[j]);
      }
      lmask[mk] = 0;
      for(size_t i = 0; i < N_HOSTS; i++) {
        host_name(i, host, sizeof(host));
        if(!first[i] && mask_match(lmask, host)) {
          first[i] = ik == 7 && !memcmp(ip, "0.0.0.0", 7) ? 1 : 2;
        }
      }
    } else if(is_exceptions(section)) {
      char *eq = strchr(item, '='), host[96];
      if(eq) {
        *eq = 0;
      }
      for(char *p = item; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
      }
      k = trimmed(item, strlen(item), &s);
      memmove(item, s, k);
      item[k] = 0;
      for(size_t i = 0; *item && i < N_HOSTS; i++) {
        host_name(i, host, sizeof(host));
        if(mask_match(item, host)) {
          if(st->nbypass < (int)(sizeof(st->bypass) / sizeof(st->bypass[0]))) {
            snprintf(st->bypass[st->nbypass++], sizeof(st->bypass[0]), "%s", item);
          }
          break;
        }
      }
    } else if(!strcasecmp(section, "general") || !strcasecmp(section, "settings") || !*section) {
      if(!strncasecmp(item, "bind", 4)) {
        const char *v = item + 4;
        while(*v == ' ' || *v == '\t') {
          v++;
        }
        if(*v == '=') {
          v++;
          trimmed(v, strlen(v), &v);
          snprintf(st->bind, sizeof(st->bind), "%s", v);
        }
      }
    }
  }
  st->on = closed;
  st->covered = 1;
  for(size_t i = 0; i < N_HOSTS; i++) {
    st->covered &= first[i] == 1;
  }
  st->covered &= !st->nbypass;
}

/* The file without the preset block (and, when on, with it again right under
 * the [overrides] header, or in a new [overrides] at the end). */
static char *
rewrite(const char *data, size_t len, int on, size_t *out_len) {
  size_t cap = len + N_HOSTS * 64 + 256, olen = 0;
  char *out = malloc(cap + 1);
  int in_block = 0, placed = 0;
  const char *eol = memchr(data, '\r', len) ? "\r\n" : "\n";

  if(!out) {
    return NULL;
  }
#define PUT(p, n)                    \
  do {                               \
    memcpy(out + olen, (p), (n));    \
    olen += (n);                     \
  } while(0)
#define PUTS(s) PUT((s), strlen(s))
#define BLOCK()                                       \
  do {                                                \
    char h[96];                                       \
    PUTS(DNS_BEGIN);                                  \
    PUTS(eol);                                        \
    for(size_t i = 0; i < N_HOSTS; i++) {             \
      host_name(i, h, sizeof(h));                     \
      PUTS(h);                                        \
      PUTS("=0.0.0.0");                               \
      PUTS(eol);                                      \
    }                                                 \
    PUTS(DNS_END);                                    \
    PUTS(eol);                                        \
  } while(0)

  for(size_t at = 0; at < len;) {
    const char *nl = memchr(data + at, '\n', len - at), *line = data + at, *s;
    size_t ll = nl ? (size_t)(nl - line) + 1 : len - at, n;
    char name[32];

    at += ll;
    n = trimmed(line, ll, &s);
    if(line_is(s, n, DNS_BEGIN)) {
      in_block = 1;
      continue;
    }
    if(in_block) {
      if(line_is(s, n, DNS_END)) {
        in_block = 0;
        continue;
      }
      if(is_preset_line(s, n)) {
        continue;
      }
      /* a line the user put inside the block is theirs: it stays */
    }
    PUT(line, ll);
    if(!nl) {
      PUTS(eol); /* the file did not end in a newline */
    }
    if(on && !placed && section_name(s, n, name, sizeof(name)) && is_overrides(name)) {
      BLOCK();
      placed = 1;
    }
  }
  if(on && !placed) {
    if(olen) {
      PUTS(eol);
    }
    PUTS("[overrides]");
    PUTS(eol);
    BLOCK();
  }
#undef BLOCK
#undef PUTS
#undef PUT
  out[olen] = 0;
  *out_len = olen;
  return out;
}

static void
reply_state(http_req_t *req, const char *data, size_t len, int exists) {
  dns_state_t st;
  char h[96];
  jw_t w;

  read_state(data ? data : "", data ? len : 0, &st);
  api_begin(&w);
  jw_kbool(&w, "enabled", exists && st.on);
  jw_kbool(&w, "configFound", exists);
  /* blocked anyway by the user's own rules (nanoDNS's defaults block all of
   * *.playstation.net): the preset is not needed while those stay */
  jw_kbool(&w, "covered", exists && st.covered);
  jw_kstr(&w, "config", NANODNS_INI);
  jw_kstr(&w, "bind", st.bind);
  jw_key(&w, "bypass");
  jw_arr(&w);
  for(int i = 0; i < st.nbypass; i++) {
    jw_str(&w, st.bypass[i]);
  }
  jw_arr_end(&w);
  jw_key(&w, "hosts");
  jw_arr(&w);
  for(size_t i = 0; i < N_HOSTS; i++) {
    host_name(i, h, sizeof(h));
    jw_str(&w, h);
  }
  jw_arr_end(&w);
  api_finish(req, &w);
}

void
api_dns_updates(http_req_t *req) {
  char local[PATH_MAX];
  char *data = NULL, *out;
  size_t len = 0, olen = 0;
  int exists;

  plat_path(NANODNS_INI, local, sizeof(local));
  exists = !fs_read_file(local, &data, &len, NANODNS_MAX);
  if(!strcmp(req->method, "POST")) {
    jval_t *b = api_body(req), *e;
    int on;

    if(!b) {
      free(data);
      return;
    }
    e = json_get(b, "enabled");
    if(!e || e->type != J_BOOL) {
      json_free(b);
      free(data);
      http_send_error(req, 400, "enabled must be true or false");
      return;
    }
    on = e->boolean;
    json_free(b);
    if(!exists) {
      /* writing a config of our own would leave out nanoDNS's defaults */
      http_send_error(req, 409, "%s is not there yet: nanoDNS writes it the first time it "
                      "starts. Start nanoDNS once and try again", NANODNS_INI);
      return;
    }
    if(!(out = rewrite(data, len, on, &olen))) {
      free(data);
      http_send_error(req, 500, "out of memory");
      return;
    }
    if(olen != len || memcmp(out, data, len)) {
      if(fs_write_file(local, out, olen)) {
        free(out);
        free(data);
        http_send_error(req, 500, "%s could not be written", NANODNS_INI);
        return;
      }
      LOG_INFO("dns: block system updates %s from %s", on ? "on" : "off", req->client_ip);
    }
    free(data);
    data = out;
    len = olen;
  } else if(strcmp(req->method, "GET")) {
    free(data);
    http_send_error(req, 405, "GET or POST");
    return;
  }
  reply_state(req, data, len, exists);
  free(data);
}
