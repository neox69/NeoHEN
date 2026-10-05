/* Personal-data redaction for log records; see <onion/log_redact.h>. */

#include <onion/log_redact.h>

#include <stdbool.h>
#include <string.h>

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool is_hex(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/*
 * Match a dotted IPv4 address at s[0..n). Returns its length, or 0. The
 * address must not be glued to further digits or dots (so "1.2.3.4.5" and
 * version strings like "13.420.000" are left alone).
 */
static size_t match_ipv4(const char *s, size_t n, unsigned octets[4]) {
  size_t i = 0;
  for (int part = 0; part < 4; part++) {
    if (part > 0) {
      if (i >= n || s[i] != '.')
        return 0;
      i++;
    }
    size_t start = i;
    unsigned value = 0;
    while (i < n && is_digit(s[i]) && i - start < 3) {
      value = value * 10 + (unsigned)(s[i] - '0');
      i++;
    }
    if (i == start || value > 255)
      return 0;
    if (i < n && is_digit(s[i]))
      return 0; /* fourth digit in an octet */
    octets[part] = value;
  }
  if (i < n && (is_digit(s[i]) ||
                (s[i] == '.' && i + 1 < n && is_digit(s[i + 1]))))
    return 0;
  return i;
}

/* aa:bb:cc:dd:ee:ff with one consistent separator (':' or '-'). */
static size_t match_mac(const char *s, size_t n) {
  if (n < 17)
    return 0;
  const char sep = s[2];
  if (sep != ':' && sep != '-')
    return 0;
  for (size_t g = 0; g < 6; g++) {
    const size_t at = g * 3;
    if (!is_hex(s[at]) || !is_hex(s[at + 1]))
      return 0;
    if (g < 5 && s[at + 2] != sep)
      return 0;
  }
  if (n > 17 && (is_hex(s[17]) || s[17] == sep))
    return 0;
  return 17;
}

size_t onion_log_redact(char *buf, size_t len) {
  if (!buf)
    return 0;
  size_t r = 0, w = 0;
  while (r < len) {
    const bool boundary =
        r == 0 || !(is_hex(buf[r - 1]) || buf[r - 1] == '.' ||
                    buf[r - 1] == ':' || buf[r - 1] == '-');
    if (boundary) {
      const size_t mac = match_mac(buf + r, len - r);
      if (mac) {
        memcpy(buf + w, "<mac>", 5);
        w += 5;
        r += mac;
        continue;
      }
      unsigned o[4];
      const size_t ip = match_ipv4(buf + r, len - r, o);
      if (ip) {
        const bool keep = o[0] == 127 ||
                          (o[0] == 0 && o[1] == 0 && o[2] == 0 && o[3] == 0);
        if (!keep) {
          memcpy(buf + w, "<ip>", 4);
          w += 4;
          r += ip;
          continue;
        }
        memmove(buf + w, buf + r, ip);
        w += ip;
        r += ip;
        continue;
      }
    }
    buf[w++] = buf[r++];
  }
  if (w < len)
    buf[w] = '\0';
  return w;
}
