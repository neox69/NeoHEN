/* Personal data never reaches the log: IPv4 and MAC redaction. */

#include "test_harness.h"

#include <onion/log_redact.h>

#include <stdio.h>
#include <string.h>

static int expect(const char *in, const char *want) {
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", in);
  const size_t n = onion_log_redact(buf, strlen(buf));
  if (n != strlen(want) || strcmp(buf, want) != 0) {
    fprintf(stderr, "redact(\"%s\") = \"%s\", want \"%s\"\n", in, buf, want);
    return 1;
  }
  return 0;
}

static int test_ipv4(void) {
  int f = 0;
  f += expect("La IP ha cambiado: 192.168.1.34",
              "La IP ha cambiado: <ip>");
  f += expect("http result ip=140.82.121.4 dns=0.012s",
              "http result ip=<ip> dns=0.012s");
  f += expect("[10.0.0.1]:9020 and 172.16.5.9.", "[<ip>]:9020 and <ip>.");
  f += expect("a 255.255.255.0 mask", "a <ip> mask");
  return f;
}

static int test_kept(void) {
  int f = 0;
  f += expect("listening on 0.0.0.0:9048", "listening on 0.0.0.0:9048");
  f += expect("http://127.0.0.1:10101/api", "http://127.0.0.1:10101/api");
  f += expect("Decrypted Version: v0.9.0-beta1 (13.420.000)",
              "Decrypted Version: v0.9.0-beta1 (13.420.000)");
  f += expect("ver 1.2.3.4.5", "ver 1.2.3.4.5");
  f += expect("octet 300.1.1.1", "octet 300.1.1.1");
  f += expect("[442.990] [ShellUI] debug: ok", "[442.990] [ShellUI] debug: ok");
  f += expect("ALLPROC=0xffffffffdd889e80", "ALLPROC=0xffffffffdd889e80");
  f += expect("uuid 3fa85f64-5717-4562-b3fc-2c963f66afa6",
              "uuid 3fa85f64-5717-4562-b3fc-2c963f66afa6");
  return f;
}

static int test_mac(void) {
  int f = 0;
  f += expect("mac=a4:5e:60:12:ab:CD up", "mac=<mac> up");
  f += expect("MAC A4-5E-60-12-AB-CD", "MAC <mac>");
  f += expect("mixed a4:5e-60:12:ab:cd", "mixed a4:5e-60:12:ab:cd");
  f += expect("too long a4:5e:60:12:ab:cd:ef", "too long a4:5e:60:12:ab:cd:ef");
  return f;
}

static int test_multiple_and_length(void) {
  int f = 0;
  f += expect("a 1.1.1.1 b 8.8.8.8 c 00:11:22:33:44:55",
              "a <ip> b <ip> c <mac>");
  char buf[] = "x 192.168.0.1\n";
  const size_t n = onion_log_redact(buf, strlen(buf));
  if (n != strlen("x <ip>\n") || strcmp(buf, "x <ip>\n") != 0)
    f++;
  return f;
}

int test_log_redact_suite(void) {
  int failures = 0;
  failures += onion_test_run("log_redact.ipv4", test_ipv4);
  failures += onion_test_run("log_redact.kept", test_kept);
  failures += onion_test_run("log_redact.mac", test_mac);
  failures += onion_test_run("log_redact.multiple", test_multiple_and_length);
  return failures;
}
