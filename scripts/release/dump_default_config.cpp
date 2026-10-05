/* Print the default config.ini of this build (host tool for the install zip). */
#include <onion/settings.hpp>

#include <cstdio>

int main() {
  const onion::Settings defaults{};
  const std::string text = onion::settings_serialize(defaults);
  std::fwrite(text.data(), 1, text.size(), stdout);
  return 0;
}
