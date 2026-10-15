/* NeoHEN: Toolbox pages that ask NeoWEB to do the work. See neo_web.hpp. */
#include "neo_web.hpp"

#include "hooked_funcs.hpp"
#include "onion_cjson.hpp"
#include "shellui_state.hpp"
#include "toolbox_i18n.hpp"
#include <onion/brand.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kNeoWebPort = 8686;
/* SceShellUI sees /data as /user/data. */
constexpr const char *kNeoWebConfig = ONION_USER_DATA_DIR "/web/config.json";
constexpr const char *kHardDriveIcon = ONION_USER_DATA_DIR "/assets/icon_xml_hardrive.png";
constexpr const char *kSettingsIcon = ONION_USER_DATA_DIR "/assets/icon_xml_settings.png";

/* NeoWEB's optional API token (config.json "token"). */
std::string neoweb_token() {
  std::string text;
  if (FILE *f = fopen(kNeoWebConfig, "rb")) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      text.append(buf, n);
    fclose(f);
  }
  onion_cjson::Root root(text);
  return root ? onion_cjson::string_item(root.get(), "token", "") : "";
}

/* One request to NeoWEB on this console. The HTTP status, or -1 when it did
 * not answer; body gets what came after the headers. */
int neoweb(const char *method, const std::string &path, const std::string &json,
           std::string &body, int timeout_s = 3) {
  body.clear();
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  timeval tv{timeout_s, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kNeoWebPort);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  const std::string token = neoweb_token();
  std::string req = std::string(method) + " " + path +
                    " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
  if (!token.empty())
    req += "X-PS5AIO-Token: " + token + "\r\n";
  if (!std::strcmp(method, "POST"))
    req += "Content-Type: application/json\r\nContent-Length: " +
           std::to_string(json.size()) + "\r\n";
  req += "\r\n" + json;
  for (size_t done = 0; done < req.size();) {
    const ssize_t n = send(fd, req.data() + done, req.size() - done, 0);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      close(fd);
      return -1;
    }
    done += static_cast<size_t>(n);
  }
  std::string reply;
  char buf[8192];
  for (;;) {
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    reply.append(buf, static_cast<size_t>(n));
    if (reply.size() > (8u << 20))
      break;
  }
  close(fd);
  int status = -1;
  if (std::sscanf(reply.c_str(), "HTTP/%*s %d", &status) != 1)
    return -1;
  const size_t sep = reply.find("\r\n\r\n");
  if (sep != std::string::npos)
    body = reply.substr(sep + 4);
  return status;
}

/* "error" of a NeoWEB answer, or what went wrong getting it. */
std::string neoweb_error(int status, const std::string &body) {
  if (status < 0)
    return toolbox_i18n::tr("neoweb.down");
  onion_cjson::Root root(body);
  const char *e = root ? onion_cjson::string_item(root.get(), "error", "") : "";
  return *e ? e : "HTTP " + std::to_string(status);
}

std::string gb(double bytes) {
  char out[32];
  std::snprintf(out, sizeof(out), bytes >= 1e12 ? "%.2f TB" : "%.1f GB",
                bytes >= 1e12 ? bytes / 1e12 : bytes / 1e9);
  return out;
}

/* /user -> internal drive, /mnt/usbN -> USB N+1, /mnt/ext1 -> M.2 SSD. */
std::string drive_name(const std::string &path) {
  if (path == "/user" || path.rfind("/data", 0) == 0)
    return toolbox_i18n::tr("drive.internal");
  if (path.rfind("/mnt/usb", 0) == 0 && path.size() > 8)
    return "USB " + std::to_string(path[8] - '0' + 1);
  if (path.rfind("/mnt/ext", 0) == 0)
    return "SSD M.2";
  return path;
}

struct Drive {
  std::string path;
  double free = 0, total = 0;
};

std::vector<Drive> drives() {
  std::vector<Drive> out;
  std::string body;
  if (neoweb("GET", "/api/sys/mounts", "", body) != 200)
    return out;
  onion_cjson::Root root(body);
  const cJSON *list = root ? onion_cjson::item(root.get(), "mounts") : nullptr;
  for (const cJSON *m = list ? list->child : nullptr; m; m = m->next) {
    const std::string path = onion_cjson::string_item(m, "path", "");
    if (path != "/user" && path.rfind("/mnt/usb", 0) != 0 &&
        path != "/mnt/ext1")
      continue;
    Drive d;
    d.path = path;
    d.free = std::strtod(onion_cjson::scalar_item(m, "free", "0").c_str(), nullptr);
    d.total = std::strtod(onion_cjson::scalar_item(m, "total", "0").c_str(), nullptr);
    if (d.total > 0)
      out.push_back(d);
  }
  return out;
}

template <typename Fn> void run_detached(Fn fn) {
  auto *job = new Fn(std::move(fn));
  pthread_t thread;
  if (pthread_create(
          &thread, nullptr,
          [](void *arg) -> void * {
            Fn *f = static_cast<Fn *>(arg);
            (*f)();
            delete f;
            return nullptr;
          },
          job) == 0)
    pthread_detach(thread);
  else
    delete job;
}

} // namespace

/* ------------------------------------------------------------ status */

void neo_append_status_group(ps5ui::Group &g) {
  std::string body, address = toolbox_i18n::tr("neoweb.down");
  std::vector<std::string> lines;
  if (neoweb("GET", "/api/sys/info", "", body, 2) == 200) {
    onion_cjson::Root root(body);
    const cJSON *info = root.get();
    const cJSON *ips = onion_cjson::item(info, "ips");
    if (ips && ips->child)
      address = "http://" +
                std::string(onion_cjson::string_item(ips->child, "ip", "?")) + ":" +
                std::to_string(kNeoWebPort);
    lines.push_back(toolbox_i18n::format("status.neoweb_fmt", address.c_str()));
    const int cpu = onion_cjson::int_item(info, "cpuTemp", 0);
    const int soc = onion_cjson::int_item(info, "socTemp", 0);
    const int fan = onion_cjson::int_item(info, "fanDuty", -1);
    if (cpu > 0 || soc > 0)
      lines.push_back(toolbox_i18n::format("status.temps_fmt", cpu, soc));
    if (fan >= 0)
      lines.push_back(toolbox_i18n::format("status.fan_fmt", fan));
    for (const Drive &d : drives())
      lines.push_back(toolbox_i18n::format("status.drive_fmt",
                                           drive_name(d.path).c_str(),
                                           gb(d.free).c_str(), gb(d.total).c_str()));
  } else {
    lines.push_back(toolbox_i18n::tr("neoweb.down"));
  }
  g.group(
      "id_group_status", toolbox_i18n::tr("status.group"),
      [&lines](ps5ui::Group &s) {
        for (size_t i = 0; i < lines.size(); ++i)
          s.label("id_status_" + std::to_string(i), lines[i]);
      },
      address, kSettingsIcon);
}

void neo_append_usb_group(ps5ui::Group &g) {
  std::vector<Drive> usb;
  for (const Drive &d : drives())
    if (d.path.rfind("/mnt/usb", 0) == 0)
      usb.push_back(d);
  g.group(
      "id_group_usb", toolbox_i18n::tr("usb.group"),
      [&usb](ps5ui::Group &u) {
        if (usb.empty())
          u.label("id_usb_none", toolbox_i18n::tr("usb.none"));
        for (const Drive &d : usb)
          u.button("id_usb_eject_" + std::string(1, d.path[8]),
                   toolbox_i18n::format("usb.eject_fmt", drive_name(d.path).c_str()),
                   toolbox_i18n::format("usb.free_fmt", gb(d.free).c_str(),
                                        gb(d.total).c_str()),
                   std::nullopt, std::nullopt, ps5ui::Style::None,
                   toolbox_i18n::tr("usb.eject.confirm"),
                   toolbox_i18n::tr("cheats.repo.download.confirm_phrase"));
      },
      toolbox_i18n::tr("usb.group.sub"), kHardDriveIcon);
}

void neo_usb_eject(int usb) {
  run_detached([usb]() {
    const std::string name = "USB " + std::to_string(usb + 1);
    notify("notify.usb.ejecting", name.c_str());
    std::string body;
    const int st = neoweb("POST", "/api/usb/eject",
                          "{\"drive\":\"usb" + std::to_string(usb) +
                              "\",\"closeApp\":true}",
                          body, 30);
    if (st == 200)
      notify("notify.usb.ejected", name.c_str());
    else
      notify("notify.neo.failed", name.c_str(), neoweb_error(st, body).c_str());
  });
}

/* ------------------------------------------------------------ disk page */

namespace {
std::string g_disk_tid;
bool g_disk_pending = false;
std::vector<std::string> g_disk_roots; /* id_disk_move_<n> -> drive path */

/* Waits for a NeoWEB job and says how it ended. */
void watch_job(int id, const std::string &what) {
  for (int i = 0; i < 24 * 60 * 12; ++i) { /* a day at most */
    sleep(5);
    std::string body;
    if (neoweb("GET", "/api/jobs?id=" + std::to_string(id), "", body) != 200)
      continue;
    onion_cjson::Root root(body);
    const cJSON *job = root ? onion_cjson::item(root.get(), "job") : nullptr;
    const std::string state = job ? onion_cjson::string_item(job, "state", "") : "";
    if (state == "running" || state == "queued")
      continue;
    if (state == "done")
      notify("notify.disk.moved", what.c_str());
    else
      notify("notify.neo.failed", what.c_str(),
             job ? onion_cjson::string_item(job, "error", state.c_str()) : "?");
    return;
  }
}
} // namespace

bool neo_disk_page_request(const std::string &uri, const std::string &title_id) {
  if (uri != kNeoActionDisk)
    return false;
  g_disk_tid = title_id.empty() ? g_ui.current_menu_tid : title_id;
  g_disk_pending = true;
  LOG_INFO("disk page for %s", g_disk_tid.c_str());
  return true;
}

bool neo_disk_page_pending() { return g_disk_pending; }

void neo_generate_disk_xml(std::string &xml) {
  g_disk_pending = false;
  g_disk_roots.clear();
  const std::string &tid = g_disk_tid;
  /* Shown in place of the Toolbox root: it must carry the root list's id
   * (see neo_generate_saves_xml). */
  ps5ui::Page page("id_debug_settings",
                   toolbox_i18n::format("disk.title_fmt", tid.c_str()));
  page.root_style(ps5ui::Style::Center);

  std::string body;
  const int st = neoweb("GET", "/api/games/images?id=" + tid, "", body, 3);
  onion_cjson::Root root(body);
  const cJSON *games = root ? onion_cjson::item(root.get(), "games") : nullptr;
  const cJSON *g = games ? games->child : nullptr;
  if (st != 200 || !g) {
    page.label("id_disk_none",
               st != 200 ? neoweb_error(st, body) : toolbox_i18n::tr("disk.not_smp"),
               ps5ui::Style::Center);
    xml = page.build();
    return;
  }
  const std::string source = onion_cjson::string_item(g, "source", "");
  const bool image = !std::strcmp(onion_cjson::string_item(g, "kind", ""), "image");
  const bool running = onion_cjson::bool_item(g, "running", false);
  const double size = std::strtod(onion_cjson::scalar_item(g, "size", "0").c_str(), nullptr);

  const cJSON *roots = onion_cjson::item(root.get(), "roots");
  std::string here;
  for (const cJSON *r = roots ? roots->child : nullptr; r; r = r->next) {
    const std::string path = onion_cjson::string_item(r, "path", "");
    if (source.rfind(path + "/", 0) == 0)
      here = drive_name(path);
  }
  page.label("id_disk_where",
             toolbox_i18n::format(image ? "disk.image_fmt" : "disk.folder_fmt",
                                  here.empty() ? source.c_str() : here.c_str(),
                                  gb(size).c_str()),
             ps5ui::Style::Center);

  /* PlayGo */
  const cJSON *pg = onion_cjson::item(g, "playgo");
  const cJSON *bp = onion_cjson::item(g, "backport");
  const cJSON *im = onion_cjson::item(g, "image");
  bool own = false;
  const cJSON *fl = im ? onion_cjson::item(im, "fakelib") : nullptr;
  for (const cJSON *f = fl ? fl->child : nullptr; f; f = f->next)
    own |= !std::strcmp(onion_cjson::string_value(f, ""), "libScePlayGo.sprx");
  if (!pg || !onion_cjson::bool_item(pg, "present", false))
    page.label("id_disk_pg", toolbox_i18n::tr("disk.pg_none"), ps5ui::Style::Center);
  else if (bp && onion_cjson::bool_item(bp, "playgo", false) &&
           onion_cjson::bool_item(bp, "enabled", false))
    page.button("id_disk_unfix", toolbox_i18n::tr("disk.pg_remove"),
                toolbox_i18n::tr("disk.pg_on"), std::nullopt, std::nullopt,
                ps5ui::Style::Center, toolbox_i18n::tr("disk.pg_remove.confirm"),
                toolbox_i18n::tr("cheats.repo.download.confirm_phrase"));
  else if (own)
    page.label("id_disk_pg", toolbox_i18n::tr("disk.pg_own"), ps5ui::Style::Center);
  else
    page.button("id_disk_fix", toolbox_i18n::tr("disk.pg_apply"),
                toolbox_i18n::tr("disk.pg_apply.sub"), std::nullopt, std::nullopt,
                ps5ui::Style::Center);

  /* Moving it */
  if (!image) {
    page.label("id_disk_move_no", toolbox_i18n::tr("disk.move_folder"), ps5ui::Style::Center);
  } else if (running) {
    page.label("id_disk_move_no", toolbox_i18n::tr("disk.move_close"), ps5ui::Style::Center);
  } else {
    for (const cJSON *r = roots ? roots->child : nullptr; r; r = r->next) {
      const std::string path = onion_cjson::string_item(r, "path", "");
      const double free =
          std::strtod(onion_cjson::scalar_item(r, "free", "0").c_str(), nullptr);
      if (source.rfind(path + "/", 0) == 0 || free < size + 7e7 ||
          g_disk_roots.size() >= static_cast<size_t>(kNeoDiskRoots))
        continue;
      page.button("id_disk_move_" + std::to_string(g_disk_roots.size()),
                  toolbox_i18n::format("disk.move_fmt", drive_name(path).c_str()),
                  toolbox_i18n::format("usb.free_fmt", gb(free).c_str(),
                                       gb(std::strtod(onion_cjson::scalar_item(r, "total", "0").c_str(), nullptr)).c_str()),
                  std::nullopt, std::nullopt, ps5ui::Style::Center,
                  toolbox_i18n::tr("disk.move.confirm"),
                  toolbox_i18n::tr("cheats.repo.download.confirm_phrase"));
      g_disk_roots.push_back(path);
    }
    if (g_disk_roots.empty())
      page.label("id_disk_move_no", toolbox_i18n::tr("disk.move_none"), ps5ui::Style::Center);
  }
  xml = page.build();
}

void neo_disk_fix() {
  const std::string tid = g_disk_tid;
  run_detached([tid]() {
    notify("notify.disk.fixing", tid.c_str());
    std::string body;
    const int st = neoweb("POST", "/api/games/autofix", "{\"id\":\"" + tid + "\"}", body, 60);
    if (st == 200)
      notify("notify.disk.fixed", tid.c_str());
    else
      notify("notify.neo.failed", tid.c_str(), neoweb_error(st, body).c_str());
  });
}

void neo_disk_unfix() {
  const std::string tid = g_disk_tid;
  run_detached([tid]() {
    std::string body;
    const int st = neoweb("POST", "/api/games/unfix", "{\"id\":\"" + tid + "\"}", body);
    if (st == 200)
      notify("notify.disk.unfixed", tid.c_str());
    else
      notify("notify.neo.failed", tid.c_str(), neoweb_error(st, body).c_str());
  });
}

void neo_disk_move(int index) {
  if (index < 0 || static_cast<size_t>(index) >= g_disk_roots.size())
    return;
  const std::string tid = g_disk_tid, to = g_disk_roots[static_cast<size_t>(index)];
  run_detached([tid, to]() {
    std::string body;
    const int st = neoweb("POST", "/api/games/move",
                          "{\"id\":\"" + tid + "\",\"to\":\"" + to + "\"}", body);
    if (st != 200) {
      notify("notify.neo.failed", tid.c_str(), neoweb_error(st, body).c_str());
      return;
    }
    onion_cjson::Root root(body);
    const int job = root ? onion_cjson::int_item(root.get(), "job", 0) : 0;
    const std::string what = tid + " -> " + drive_name(to);
    notify("notify.disk.moving", what.c_str());
    if (job > 0)
      watch_job(job, what);
  });
}
