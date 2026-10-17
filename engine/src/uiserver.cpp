// The UI server only uses POSIX interfaces; ask libc for them explicitly so the
// file also builds with strict -std=c++NN on non-bionic hosts (Cygwin, glibc).
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "uiserver.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "util.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace {

const char* kUiServerVersion = "1.5.0";
const size_t kMaxBody = 1u << 20;
const size_t kMaxCapture = 2u << 20;
const int kExecTimeoutSec = 120;
const int kReadTimeoutMs = 15000;

void sleep_ms(long ms) {
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(ms / 1000);
  ts.tv_nsec = static_cast<long>((ms % 1000) * 1000000L);
  nanosleep(&ts, NULL);
}

struct UiServerConfig {
  int port;
  long idle;
  std::string root;
  std::string cli;
  std::string state;
  std::string token;
  UiServerConfig() : port(0), idle(1800) {}
};

struct HttpRequest {
  std::string method;
  std::string path;
  std::string query;
  std::string body;
  std::vector<std::pair<std::string, std::string> > headers;
};

std::string g_state_path;
std::string g_token;
int g_listen_fd = -1;

void on_term_signal(int) {
  if (!g_state_path.empty()) unlink(g_state_path.c_str());
  _exit(0);
}

std::string trim(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && isspace((unsigned char)s[b])) b++;
  while (e > b && isspace((unsigned char)s[e - 1])) e--;
  return s.substr(b, e - b);
}

std::string to_lower(const std::string& s) {
  std::string o = s;
  for (size_t i = 0; i < o.size(); i++) {
    o[i] = (char)tolower((unsigned char)o[i]);
  }
  return o;
}

std::string header_get(const std::vector<std::pair<std::string, std::string> >& h,
                       const std::string& name) {
  for (size_t i = 0; i < h.size(); i++) {
    if (h[i].first == name) return h[i].second;
  }
  return std::string();
}

bool mkdirs(const std::string& path) {
  if (path.empty()) return false;
  std::string cur;
  size_t i = 0;
  if (path[0] == '/') {
    cur = "/";
    i = 1;
  }
  while (i <= path.size()) {
    size_t j = path.find('/', i);
    std::string part = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
    if (!part.empty()) {
      if (!cur.empty() && cur[cur.size() - 1] != '/') cur += '/';
      cur += part;
      if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    if (j == std::string::npos) break;
    i = j + 1;
  }
  return true;
}

bool read_file(const std::string& path, std::string& out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[8192];
  size_t n = 0;
  out.clear();
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
    out.append(buf, n);
    if (out.size() > kMaxCapture) {
      fclose(f);
      return false;
    }
  }
  fclose(f);
  return true;
}

std::string random_token() {
  unsigned char buf[16];
  FILE* f = fopen("/dev/urandom", "rb");
  size_t n = (f != NULL) ? fread(buf, 1, sizeof buf, f) : 0;
  if (f) fclose(f);
  if (n != sizeof buf) {
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    for (size_t i = 0; i < sizeof buf; i++) {
      seed = seed * 1103515245u + 12345u;
      buf[i] = (unsigned char)((seed >> 16) & 0xff);
    }
  }
  char h[4];
  std::string out;
  for (size_t i = 0; i < sizeof buf; i++) {
    snprintf(h, sizeof h, "%02x", buf[i]);
    out += h;
  }
  return out;
}

std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 16);
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      case '\b': o += "\\b"; break;
      case '\f': o += "\\f"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof b, "\\u%04x", (unsigned int)c);
          o += b;
        } else {
          o += (char)c;
        }
        break;
    }
  }
  return o;
}

void send_all(int fd, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
    if (n <= 0) {
      if (errno == EINTR) continue;
      break;
    }
    off += (size_t)n;
  }
}

void http_reply(int fd, int code, const char* status, const std::string& ctype,
                const std::string& body, bool head_only) {
  char hdr[512];
  snprintf(hdr, sizeof hdr,
           "HTTP/1.1 %d %s\r\n"
           "Content-Type: %s\r\n"
           "Content-Length: %lu\r\n"
           "Cache-Control: no-store\r\n"
           "X-Content-Type-Options: nosniff\r\n"
           "Referrer-Policy: no-referrer\r\n"
           "Connection: close\r\n"
           "\r\n",
           code, status, ctype.c_str(), (unsigned long)body.size());
  send_all(fd, std::string(hdr));
  if (!head_only) send_all(fd, body);
}

void http_json(int fd, int code, const char* status, const std::string& body) {
  http_reply(fd, code, status, "application/json; charset=utf-8", body, false);
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string url_decode(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int hi = hex_value(s[i + 1]);
      int lo = hex_value(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        o += (char)((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    o += s[i];
  }
  return o;
}

std::string query_value(const std::string& query, const std::string& key) {
  size_t pos = 0;
  while (pos < query.size()) {
    size_t amp = query.find('&', pos);
    std::string item = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    size_t eq = item.find('=');
    if (eq != std::string::npos) {
      if (item.substr(0, eq) == key) return url_decode(item.substr(eq + 1));
    }
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return std::string();
}

bool read_http_request(int fd, HttpRequest& req) {
  std::string data;
  char buf[8192];
  size_t hdr_end = std::string::npos;
  while (hdr_end == std::string::npos) {
    if (data.size() > 65536) return false;
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    int r = poll(&p, 1, kReadTimeoutMs);
    if (r <= 0) return false;
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return false;
    data.append(buf, (size_t)n);
    hdr_end = data.find("\r\n\r\n");
  }
  std::string head = data.substr(0, hdr_end);
  std::string rest = data.substr(hdr_end + 4);
  size_t line_end = head.find("\r\n");
  std::string first = head.substr(0, line_end == std::string::npos ? std::string::npos : line_end);
  size_t sp1 = first.find(' ');
  if (sp1 == std::string::npos) return false;
  size_t sp2 = first.find(' ', sp1 + 1);
  if (sp2 == std::string::npos) return false;
  req.method = first.substr(0, sp1);
  std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);
  size_t qpos = target.find('?');
  if (qpos == std::string::npos) {
    req.path = target;
  } else {
    req.path = target.substr(0, qpos);
    req.query = target.substr(qpos + 1);
  }
  size_t pos = (line_end == std::string::npos) ? head.size() : line_end + 2;
  while (pos < head.size()) {
    size_t eol = head.find("\r\n", pos);
    std::string line = head.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string name = to_lower(trim(line.substr(0, colon)));
      std::string value = trim(line.substr(colon + 1));
      req.headers.push_back(std::make_pair(name, value));
    }
    if (eol == std::string::npos) break;
    pos = eol + 2;
  }
  size_t content_length = 0;
  std::string cl = header_get(req.headers, "content-length");
  if (!cl.empty()) content_length = (size_t)strtoul(cl.c_str(), NULL, 10);
  if (content_length > kMaxBody) return false;
  while (rest.size() < content_length) {
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    int r = poll(&p, 1, kReadTimeoutMs);
    if (r <= 0) return false;
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return false;
    rest.append(buf, (size_t)n);
  }
  req.body = rest.substr(0, content_length);
  return true;
}

bool parse_exec_args(const std::string& body, std::vector<std::string>& out) {
  size_t p = body.find("\"args\"");
  if (p == std::string::npos) return false;
  p = body.find(':', p);
  if (p == std::string::npos) return false;
  p++;
  while (p < body.size() && isspace((unsigned char)body[p])) p++;
  if (p >= body.size() || body[p] != '[') return false;
  p++;
  while (p < body.size()) {
    while (p < body.size() && (isspace((unsigned char)body[p]) || body[p] == ',')) p++;
    if (p >= body.size()) return false;
    if (body[p] == ']') return !out.empty();
    if (body[p] != '"') return false;
    p++;
    std::string s;
    while (p < body.size() && body[p] != '"') {
      char c = body[p];
      if (c == '\\' && p + 1 < body.size()) {
        p++;
        char e = body[p];
        switch (e) {
          case 'n': s += '\n'; break;
          case 't': s += '\t'; break;
          case 'r': s += '\r'; break;
          case 'b': s += '\b'; break;
          case 'f': s += '\f'; break;
          case 'u': {
            if (p + 4 >= body.size()) return false;
            unsigned int cp = 0;
            for (int k = 0; k < 4; k++) {
              int v = hex_value(body[p + 1 + k]);
              if (v < 0) return false;
              cp = cp * 16u + (unsigned int)v;
            }
            p += 4;
            if (cp < 0x80) {
              s += (char)cp;
            } else if (cp < 0x800) {
              s += (char)(0xC0 | (cp >> 6));
              s += (char)(0x80 | (cp & 0x3F));
            } else {
              s += (char)(0xE0 | (cp >> 12));
              s += (char)(0x80 | ((cp >> 6) & 0x3F));
              s += (char)(0x80 | (cp & 0x3F));
            }
            break;
          }
          default: s += e; break;
        }
        p++;
      } else {
        s += c;
        p++;
      }
    }
    if (p >= body.size()) return false;
    p++;
    out.push_back(s);
    if (out.size() > 16) return false;
  }
  return false;
}

bool allowed_ui_command(const std::string& sub) {
  static const char* kAllowed[] = {
      "status", "env", "version", "images", "paths", "path-add", "path-del",
      "cfg", "cfg-set", "mount", "unmount", "convert", "ventoy-init",
      "ventoy-add", "blank", "jobs", "job-poll", "job-cancel", "log", "diag",
      "stop"};
  for (size_t i = 0; i < sizeof(kAllowed) / sizeof(kAllowed[0]); i++) {
    if (sub == kAllowed[i]) return true;
  }
  return false;
}

int exec_capture(const UiServerConfig& cfg, const std::vector<std::string>& args,
                 std::string& out_text, std::string& err_text) {
  int op[2];
  int ep[2];
  if (pipe(op) != 0) return -1;
  if (pipe(ep) != 0) {
    close(op[0]);
    close(op[1]);
    return -1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(op[0]);
    close(op[1]);
    close(ep[0]);
    close(ep[1]);
    return -1;
  }
  if (pid == 0) {
    dup2(op[1], 1);
    dup2(ep[1], 2);
    close(op[0]);
    close(op[1]);
    close(ep[0]);
    close(ep[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(cfg.cli.c_str()));
    for (size_t i = 0; i < args.size(); i++) {
      argv.push_back(const_cast<char*>(args[i].c_str()));
    }
    argv.push_back(NULL);
    execv(cfg.cli.c_str(), &argv[0]);
    _exit(127);
  }
  close(op[1]);
  close(ep[1]);
  int fds[2];
  fds[0] = op[0];
  fds[1] = ep[0];
  std::string* outs[2];
  outs[0] = &out_text;
  outs[1] = &err_text;
  out_text.clear();
  err_text.clear();
  bool open_fd[2];
  open_fd[0] = true;
  open_fd[1] = true;
  int open_count = 2;
  time_t started = time(NULL);
  while (open_count > 0) {
    if (time(NULL) - started > kExecTimeoutSec) {
      kill(pid, SIGKILL);
      break;
    }
    struct pollfd p[2];
    int idx[2];
    int nfds = 0;
    for (int i = 0; i < 2; i++) {
      if (!open_fd[i]) continue;
      p[nfds].fd = fds[i];
      p[nfds].events = POLLIN;
      p[nfds].revents = 0;
      idx[nfds] = i;
      nfds++;
    }
    if (nfds == 0) break;
    int r = poll(p, (nfds_t)nfds, 500);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0) continue;
    for (int k = 0; k < nfds; k++) {
      if (p[k].revents == 0) continue;
      int i = idx[k];
      char buf[8192];
      ssize_t n = read(fds[i], buf, sizeof buf);
      if (n > 0) {
        if (outs[i]->size() + (size_t)n > kMaxCapture) {
          kill(pid, SIGKILL);
          open_fd[i] = false;
          open_count--;
        } else {
          outs[i]->append(buf, (size_t)n);
        }
      } else if (n == 0) {
        open_fd[i] = false;
        open_count--;
      } else if (errno != EINTR && errno != EAGAIN) {
        open_fd[i] = false;
        open_count--;
      }
    }
  }
  close(op[0]);
  close(ep[0]);
  int status = 0;
  time_t deadline = time(NULL) + 5;
  while (true) {
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) break;
    if (w < 0) {
      status = 0;
      break;
    }
    if (time(NULL) > deadline) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      break;
    }
    sleep_ms(20);
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return -1;
}

std::string content_type_for(const std::string& path) {
  size_t dot = path.rfind('.');
  std::string ext = (dot == std::string::npos) ? std::string() : to_lower(path.substr(dot + 1));
  if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
  if (ext == "js" || ext == "mjs") return "application/javascript; charset=utf-8";
  if (ext == "css") return "text/css; charset=utf-8";
  if (ext == "json") return "application/json; charset=utf-8";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "webp") return "image/webp";
  if (ext == "ico") return "image/x-icon";
  if (ext == "woff2") return "font/woff2";
  if (ext == "txt" || ext == "md") return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

void serve_static(int fd, const UiServerConfig& cfg, const std::string& path, bool head_only) {
  std::string rel = path;
  if (rel.empty() || rel == "/") rel = "/index.html";
  if (rel.find("..") != std::string::npos) {
    http_json(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"bad path\"}");
    return;
  }
  std::string full = cfg.root + rel;
  char resolved[PATH_MAX];
  char root_resolved[PATH_MAX];
  if (realpath(full.c_str(), resolved) == NULL ||
      realpath(cfg.root.c_str(), root_resolved) == NULL) {
    http_json(fd, 404, "Not Found", "{\"ok\":false,\"error\":\"not found\"}");
    return;
  }
  std::string r(resolved);
  std::string rr(root_resolved);
  if (r.size() <= rr.size() || r.compare(0, rr.size(), rr) != 0 || r[rr.size()] != '/') {
    http_json(fd, 403, "Forbidden", "{\"ok\":false,\"error\":\"forbidden\"}");
    return;
  }
  std::string body;
  if (!read_file(r, body)) {
    http_json(fd, 404, "Not Found", "{\"ok\":false,\"error\":\"not found\"}");
    return;
  }
  http_reply(fd, 200, "OK", content_type_for(r), body, head_only);
}

bool validate_exec_args(const std::vector<std::string>& args, std::string& err) {
  if (args.size() < 2) {
    err = "missing command";
    return false;
  }
  if (args.size() > 16) {
    err = "too many arguments";
    return false;
  }
  if (args[0] != "ui") {
    err = "only ui commands are allowed";
    return false;
  }
  if (!allowed_ui_command(args[1])) {
    err = "command not allowed: " + args[1];
    return false;
  }
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i].size() > 1024) {
      err = "argument too long";
      return false;
    }
  }
  if (args[1] == "mount" && args.size() < 3) {
    err = "mount needs an image path";
    return false;
  }
  return true;
}

void handle_client(int fd, const UiServerConfig& cfg, int port) {
  HttpRequest req;
  if (!read_http_request(fd, req)) {
    http_json(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"malformed request\"}");
    return;
  }
  std::string host = to_lower(trim(header_get(req.headers, "host")));
  std::string expected1 = "127.0.0.1:" + std::to_string(port);
  std::string expected2 = "localhost:" + std::to_string(port);
  if (host != expected1 && host != expected2) {
    http_json(fd, 403, "Forbidden", "{\"ok\":false,\"error\":\"invalid host\"}");
    return;
  }
  bool head_only = (req.method == "HEAD");
  if (req.method == "GET" || req.method == "HEAD") {
    if (req.path == "/api/ping") {
      char buf[256];
      snprintf(buf, sizeof buf,
               "{\"ok\":true,\"app\":\"isodrive\",\"version\":\"%s\",\"pid\":%ld,\"token_required\":true}",
               kUiServerVersion, (long)getpid());
      http_json(fd, 200, "OK", std::string(buf));
      return;
    }
    if (req.path == "/api/token") {
      http_json(fd, 403, "Forbidden", "{\"ok\":false,\"error\":\"forbidden\"}");
      return;
    }
    serve_static(fd, cfg, req.path, head_only);
    return;
  }
  if (req.method == "POST" && req.path == "/api/exec") {
    std::string token = header_get(req.headers, "x-isodrive-token");
    if (token.empty()) token = query_value(req.query, "token");
    if (token != g_token) {
      http_json(fd, 403, "Forbidden", "{\"ok\":false,\"error\":\"invalid token\"}");
      return;
    }
    std::vector<std::string> args;
    if (!parse_exec_args(req.body, args)) {
      http_json(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"bad json body\"}");
      return;
    }
    std::string verr;
    if (!validate_exec_args(args, verr)) {
      std::string body = "{\"ok\":false,\"error\":\"" + json_escape(verr) + "\"}";
      http_json(fd, 400, "Bad Request", body);
      return;
    }
    if (args[1] == "stop") {
      // "ui stop" is answered by the server itself: reply first, then signal the
      // accept loop (our parent) so the listener closes and the state file is
      // removed by on_term_signal().  Spawning the shell CLI here would work too
      // but would race its own parent's death.
      // Keep the /api/exec envelope shape: the WebUI parses stdout as the CLI JSON.
      std::string stop_json = "{\"ok\":true,\"stopped\":true,\"running\":false}\n";
      std::string body = std::string("{\"ok\":true,\"code\":0,\"stdout\":\"") +
                         json_escape(stop_json) + "\",\"stderr\":\"\"}";
      http_json(fd, 200, "OK", body);
      kill(getppid(), SIGTERM);
      return;
    }
    std::string out_text;
    std::string err_text;
    int code = exec_capture(cfg, args, out_text, err_text);
    char head[128];
    snprintf(head, sizeof head, "{\"ok\":%s,\"code\":%d,\"stdout\":\"",
             code == 0 ? "true" : "false", code);
    std::string body = std::string(head) + json_escape(out_text) +
                       "\",\"stderr\":\"" + json_escape(err_text) + "\"}";
    http_json(fd, 200, "OK", body);
    return;
  }
  http_json(fd, 404, "Not Found", "{\"ok\":false,\"error\":\"not found\"}");
}

bool write_state_file(const UiServerConfig& cfg, int port) {
  std::string dir;
  size_t slash = cfg.state.rfind('/');
  if (slash != std::string::npos) dir = cfg.state.substr(0, slash);
  if (!dir.empty() && !mkdirs(dir)) {
    fprintf(stderr, "[ui] cannot create state dir %s\n", dir.c_str());
  }
  std::string tmp = cfg.state + ".tmp." + std::to_string((long long)getpid());
  std::string url = "http://127.0.0.1:" + std::to_string(port) + "/#" + cfg.token;
  std::string data = "port=" + std::to_string(port) + "\ntoken=" + cfg.token +
                     "\npid=" + std::to_string((long long)getpid()) +
                     "\nurl=" + url + "\nstarted=" + std::to_string((long long)time(NULL)) +
                     "\nversion=" + kUiServerVersion + "\n";
  FILE* f = fopen(tmp.c_str(), "w");
  if (!f) {
    fprintf(stderr, "[ui] cannot write state file %s: %s\n", tmp.c_str(), strerror(errno));
    return false;
  }
  fwrite(data.data(), 1, data.size(), f);
  fclose(f);
  chmod(tmp.c_str(), 0600);
  if (rename(tmp.c_str(), cfg.state.c_str()) != 0) {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

void print_usage() {
  fprintf(stderr,
          "usage: isodrive serve --root DIR --cli PATH [--port N] [--state FILE]\n"
          "                     [--token HEX] [--idle SEC]\n"
          "Internal loopback WebUI server. Started by \"isodrive ui start\".\n");
}

}  // namespace

int isodrive_ui_server_main(int argc, char** argv) {
  UiServerConfig cfg;
  cfg.state = std::string(kStateDir) + "/ui.state";
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      print_usage();
      return 0;
    } else if (a == "--port" && i + 1 < argc) {
      cfg.port = atoi(argv[++i]);
    } else if (a == "--root" && i + 1 < argc) {
      cfg.root = argv[++i];
    } else if (a == "--cli" && i + 1 < argc) {
      cfg.cli = argv[++i];
    } else if (a == "--state" && i + 1 < argc) {
      cfg.state = argv[++i];
    } else if (a == "--token" && i + 1 < argc) {
      cfg.token = argv[++i];
    } else if (a == "--idle" && i + 1 < argc) {
      cfg.idle = atol(argv[++i]);
    } else {
      fprintf(stderr, "[ui] unknown argument: %s\n", a.c_str());
      print_usage();
      return 2;
    }
  }
  if (cfg.root.empty() || cfg.cli.empty()) {
    print_usage();
    return 2;
  }
  while (!cfg.root.empty() && cfg.root[cfg.root.size() - 1] == '/') {
    cfg.root.erase(cfg.root.size() - 1);
  }
  if (cfg.token.empty()) cfg.token = random_token();
  g_token = cfg.token;
  g_state_path = cfg.state;

  signal(SIGPIPE, SIG_IGN);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_term_signal;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    fprintf(stderr, "[ui] socket: %s\n", strerror(errno));
    return 1;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((unsigned short)cfg.port);
  if (bind(fd, (struct sockaddr*)&addr, sizeof addr) != 0) {
    fprintf(stderr, "[ui] bind 127.0.0.1:%d: %s\n", cfg.port, strerror(errno));
    close(fd);
    return 1;
  }
  if (listen(fd, 16) != 0) {
    fprintf(stderr, "[ui] listen: %s\n", strerror(errno));
    close(fd);
    return 1;
  }
  socklen_t alen = sizeof addr;
  if (getsockname(fd, (struct sockaddr*)&addr, &alen) != 0) {
    close(fd);
    return 1;
  }
  int port = (int)ntohs(addr.sin_port);
  g_listen_fd = fd;
  if (!write_state_file(cfg, port)) {
    close(fd);
    return 1;
  }
  fprintf(stderr, "[ui] ISODrive+ UI server listening on 127.0.0.1:%d root=%s idle=%ld\n", port,
          cfg.root.c_str(), cfg.idle);
  fflush(stderr);

  time_t last_activity = time(NULL);
  bool running = true;
  while (running) {
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    int r = poll(&p, 1, 1000);
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
    if (r > 0 && (p.revents & POLLIN)) {
      int conn = accept(fd, NULL, NULL);
      if (conn >= 0) {
        fcntl(conn, F_SETFD, FD_CLOEXEC);
        last_activity = time(NULL);
        pid_t pid = fork();
        if (pid == 0) {
          close(fd);
          g_listen_fd = -1;
          handle_client(conn, cfg, port);
          close(conn);
          _exit(0);
        }
        close(conn);
      }
    }
    if (cfg.idle > 0 && time(NULL) - last_activity >= cfg.idle) {
      fprintf(stderr, "[ui] idle timeout, exiting\n");
      running = false;
    }
  }
  close(fd);
  unlink(cfg.state.c_str());
  return 0;
}
