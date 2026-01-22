#include "chain_control_server.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "signal_chain_nodes.h"

namespace pedal::control
{

  using Json = nlohmann::json;

  static void unlinkIfExists(const std::string &p)
  {
    ::unlink(p.c_str());
  }

  static bool writeAll(int fd, const char *buf, size_t n)
  {
    size_t off = 0;
    while (off < n)
    {
      ssize_t w = ::write(fd, buf + off, n - off);
      if (w < 0)
      {
        if (errno == EINTR)
          continue;
        return false;
      }
      off += (size_t)w;
    }
    return true;
  }

  static bool sendJsonLine(int fd, const Json &j)
  {
    std::string s = j.dump();
    s.push_back('\n');
    return writeAll(fd, s.data(), s.size());
  }

  static std::optional<std::string> readLine(int fd, size_t maxBytes)
  {
    std::string out;
    out.reserve(1024);

    char c;
    while (out.size() < maxBytes)
    {
      ssize_t r = ::read(fd, &c, 1);
      if (r == 0)
        break;
      if (r < 0)
      {
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          continue;
        return std::nullopt;
      }
      if (c == '\n')
        break;
      out.push_back(c);
    }

    if (out.empty())
      return std::nullopt;
    return out;
  }

  bool persistChainToDisk(const std::string &path, const pedal::chain::ChainSpec &spec, std::string &err)
  {
    try
    {
      std::filesystem::path p(path);
      std::filesystem::create_directories(p.parent_path());

      const auto tmp = p.parent_path() / (p.filename().string() + ".tmp");

      std::FILE *f = std::fopen(tmp.c_str(), "wb");
      if (!f)
      {
        err = std::string("Failed to open temp file: ") + std::strerror(errno);
        return false;
      }

      const auto j = pedal::chain::chainSpecToJson(spec);
      const std::string s = j.dump(2) + "\n";
      const size_t w = std::fwrite(s.data(), 1, s.size(), f);
      std::fclose(f);

      if (w != s.size())
      {
        err = "Short write";
        return false;
      }

      std::filesystem::rename(tmp, p);
      return true;
    }
    catch (const std::exception &e)
    {
      err = e.what();
      return false;
    }
  }

  static Json handleRequest(ChainRuntimeState *state, const Json &req)
  {
    if (!req.is_object())
      return Json{{"ok", false}, {"error", "request must be an object"}};

    if (!req.contains("cmd") || !req["cmd"].is_string())
      return Json{{"ok", false}, {"error", "missing string cmd"}};

    const std::string cmd = req["cmd"].get<std::string>();

    if (cmd == "list_types")
    {
      return Json{{"ok", true}, {"types", pedal::dsp::nodeTypeManifest()}};
    }

    if (cmd == "get_chain")
    {
      auto current = std::atomic_load_explicit(&state->activeChain, std::memory_order_acquire);
      if (!current)
        return Json{{"ok", false}, {"error", "no active chain"}};

      return Json{{"ok", true}, {"chain", pedal::chain::chainSpecToJson(current->spec())}};
    }

    if (cmd == "set_chain")
    {
      if (!req.contains("chain"))
        return Json{{"ok", false}, {"error", "missing chain"}};

      pedal::chain::ValidationError verr;
      auto parsed = pedal::chain::parseChainJson(req["chain"], verr);
      if (!parsed)
        return Json{{"ok", false}, {"error", verr.message}};

      parsed->sampleRate = state->ctx.sampleRate;

      auto validated = pedal::chain::validateChainSpec(*parsed, verr);
      if (!validated)
        return Json{{"ok", false}, {"error", verr.message}};

      std::string buildErr;
      auto built = pedal::dsp::buildChain(*validated, state->ctx, buildErr);
      if (!built || !built->chain)
        return Json{{"ok", false}, {"error", buildErr}};

      // Persist to disk and publish as pending.
      std::string persistErr;
      if (!persistChainToDisk(state->configPath, *validated, persistErr))
        return Json{{"ok", false}, {"error", "persist failed: " + persistErr}};

      state->lastSpec = *validated;
      std::atomic_store_explicit(&state->pendingChain, built->chain, std::memory_order_release);

      Json resp{{"ok", true}};
      if (!built->warning.empty())
        resp["warning"] = built->warning;
      return resp;
    }

    return Json{{"ok", false}, {"error", "unknown cmd"}};
  }

  std::thread startControlServer(ChainRuntimeState *state)
  {
    return std::thread([state]()
                       {
    const std::string sockPath = state->socketPath;

    int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0)
    {
      std::fprintf(stderr, "Control: socket() failed: %s\n", std::strerror(errno));
      return;
    }

    unlinkIfExists(sockPath);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sockPath.c_str());

    if (::bind(srv, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
      std::fprintf(stderr, "Control: bind(%s) failed: %s\n", sockPath.c_str(), std::strerror(errno));
      ::close(srv);
      return;
    }

    ::chmod(sockPath.c_str(), 0666);

    if (::listen(srv, 4) < 0)
    {
      std::fprintf(stderr, "Control: listen() failed: %s\n", std::strerror(errno));
      ::close(srv);
      return;
    }

    std::printf("Control: unix socket %s\n", sockPath.c_str());

    // Non-blocking accept so we can exit cleanly on shutdown.
    int flags = ::fcntl(srv, F_GETFL, 0);
    if (flags >= 0)
      ::fcntl(srv, F_SETFL, flags | O_NONBLOCK);

    while (state->running.load(std::memory_order_relaxed))
    {
      pollfd pfd{};
      pfd.fd = srv;
      pfd.events = POLLIN;
      int pr = ::poll(&pfd, 1, 200);
      if (pr < 0)
      {
        if (errno == EINTR)
          continue;
        std::fprintf(stderr, "Control: poll() failed: %s\n", std::strerror(errno));
        break;
      }
      if (pr == 0)
        continue;

      int cfd = ::accept(srv, nullptr, nullptr);
      if (cfd < 0)
      {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
          continue;
        std::fprintf(stderr, "Control: accept() failed: %s\n", std::strerror(errno));
        break;
      }

      auto line = readLine(cfd, 1024 * 1024);
      if (!line)
      {
        ::close(cfd);
        continue;
      }

      Json resp;
      try
      {
        Json req = Json::parse(*line);
        resp = handleRequest(state, req);
      }
      catch (const std::exception &e)
      {
        resp = Json{{"ok", false}, {"error", std::string("parse error: ") + e.what()}};
      }

      sendJsonLine(cfd, resp);
      ::close(cfd);
    }

    ::close(srv);
    unlinkIfExists(sockPath); });
  }

} // namespace pedal::control
