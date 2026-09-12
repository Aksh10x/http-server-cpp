// http-server-cpp: a from-scratch HTTP/1.1 server over raw TCP sockets.
// C++ port of the original Go implementation - same wire behavior, same
// endpoints, same manual header/body parsing approach.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kPort = 4221;
constexpr size_t kRecvChunkSize = 4096;

// Buffered reader over a raw socket fd, playing the role bufio.Reader plays
// in the Go version: lets us read line-by-line for headers, then switch to
// reading an exact byte count for the body.
class SocketReader {
 public:
  explicit SocketReader(int fd) : fd_(fd) {}

  // Reads up to and including the next '\n'. Returns false only when the
  // connection closed with nothing left to return.
  bool ReadLine(std::string& out) {
    out.clear();
    while (true) {
      if (pos_ < buffer_.size()) {
        char c = buffer_[pos_++];
        out.push_back(c);
        if (c == '\n') return true;
      } else if (!Fill()) {
        return !out.empty();
      }
    }
  }

  // Reads exactly n bytes into out. Returns false if the connection closed
  // before n bytes were available.
  bool ReadExact(size_t n, std::string& out) {
    out.clear();
    out.reserve(n);
    while (out.size() < n) {
      if (pos_ < buffer_.size()) {
        size_t avail = buffer_.size() - pos_;
        size_t need = n - out.size();
        size_t take = std::min(avail, need);
        out.append(buffer_, pos_, take);
        pos_ += take;
      } else if (!Fill()) {
        return false;
      }
    }
    return true;
  }

 private:
  bool Fill() {
    char tmp[kRecvChunkSize];
    ssize_t r = recv(fd_, tmp, sizeof(tmp), 0);
    if (r <= 0) return false;
    buffer_.assign(tmp, tmp + r);
    pos_ = 0;
    return true;
  }

  int fd_;
  std::string buffer_;
  size_t pos_ = 0;
};

std::string ToLower(const std::string& s) {
  std::string r = s;
  for (auto& c : r) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return r;
}

std::string TrimSpace(const std::string& s) {
  size_t start = 0, end = s.size();
  while (start < end && isspace(static_cast<unsigned char>(s[start]))) start++;
  while (end > start && isspace(static_cast<unsigned char>(s[end - 1]))) end--;
  return s.substr(start, end - start);
}

std::vector<std::string> Split(const std::string& s, const std::string& delim) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(delim, start);
    if (pos == std::string::npos) {
      parts.push_back(s.substr(start));
      break;
    }
    parts.push_back(s.substr(start, pos - start));
    start = pos + delim.size();
  }
  return parts;
}

bool StartsWith(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool GzipCompress(const std::string& input, std::string& output) {
  z_stream zs{};
  // windowBits = 15 | 16 selects gzip framing instead of raw zlib/deflate.
  if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 | 16, 8,
                    Z_DEFAULT_STRATEGY) != Z_OK) {
    return false;
  }

  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  zs.avail_in = static_cast<uInt>(input.size());

  int ret;
  char outbuf[4096];
  do {
    zs.next_out = reinterpret_cast<Bytef*>(outbuf);
    zs.avail_out = sizeof(outbuf);
    ret = deflate(&zs, Z_FINISH);
    output.append(outbuf, sizeof(outbuf) - zs.avail_out);
  } while (ret == Z_OK);

  deflateEnd(&zs);
  return ret == Z_STREAM_END;
}

struct ParsedRequest {
  std::string method;
  std::vector<std::string> targets;  // request path split on '/'
  std::vector<std::string> lines;    // request line + header lines
  std::string body;
};

std::string HandleRequest(const ParsedRequest& req, const std::string& dir) {
  const auto& targets = req.targets;
  std::string response;

  if (targets.size() < 2 || targets[1].empty()) {
    response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";

  } else if (targets[1] == "echo") {
    std::string echoed = targets.size() > 2 ? targets[2] : "";
    bool gzip_handled = false;

    for (const auto& line : req.lines) {
      if (StartsWith(line, "Accept-Encoding:")) {
        std::string encodings = TrimSpace(line.substr(std::string("Accept-Encoding:").size()));
        std::vector<std::string> accepted = Split(encodings, ", ");
        if (std::find(accepted.begin(), accepted.end(), "gzip") != accepted.end()) {
          std::string compressed;
          if (!GzipCompress(echoed, compressed)) {
            response = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
          } else {
            std::ostringstream oss;
            oss << "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: "
                << compressed.size() << "\r\nContent-Encoding: gzip\r\n\r\n" << compressed;
            response = oss.str();
          }
          gzip_handled = true;
          break;
        }
      }
    }

    if (!gzip_handled) {
      std::ostringstream oss;
      oss << "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: "
          << echoed.size() << "\r\n\r\n" << echoed;
      response = oss.str();
    }

  } else if (targets[1] == "user-agent") {
    for (const auto& line : req.lines) {
      if (StartsWith(line, "User-Agent:")) {
        std::string ua = TrimSpace(line.substr(std::string("User-Agent:").size()));
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: "
            << ua.size() << "\r\n\r\n" << ua;
        response = oss.str();
        break;
      }
    }
    if (response.empty()) {
      response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
    }

  } else if (targets[1] == "files") {
    std::string filename = targets.size() > 2 ? targets[2] : "";
    std::string file_path = dir + "/" + filename;

    if (req.method == "GET") {
      std::ifstream file(file_path, std::ios::binary);
      if (!file) {
        response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
      } else {
        std::ostringstream contents;
        contents << file.rdbuf();
        std::string content = contents.str();
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: "
            << content.size() << "\r\n\r\n" << content;
        response = oss.str();
      }
    } else if (req.method == "POST") {
      mkdir(dir.c_str(), 0755);
      std::ofstream file(file_path, std::ios::binary | std::ios::trunc);
      if (!file) {
        response = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
      } else {
        file.write(req.body.data(), static_cast<std::streamsize>(req.body.size()));
        file.close();
        response = "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n";
      }
    } else {
      response = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
    }

  } else {
    response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
  }

  bool should_close = std::find(req.lines.begin(), req.lines.end(), "Connection: close") !=
                       req.lines.end();
  if (should_close) {
    size_t pos = response.find("\r\n\r\n");
    if (pos != std::string::npos) {
      response.replace(pos, 4, "\r\nConnection: close\r\n\r\n");
    }
  }

  return response;
}

void HandleConnection(int client_fd, std::string dir) {
  SocketReader reader(client_fd);

  while (true) {
    std::vector<std::string> lines;
    int content_length = 0;

    // --- Read headers line-by-line until the blank line that ends them ---
    while (true) {
      std::string line;
      if (!reader.ReadLine(line)) {
        close(client_fd);
        return;
      }
      std::string trimmed = TrimSpace(line);
      lines.push_back(trimmed);
      if (trimmed.empty()) break;
      if (StartsWith(ToLower(line), "content-length:")) {
        auto parts = Split(line, ":");
        if (parts.size() > 1) content_length = std::atoi(TrimSpace(parts[1]).c_str());
      }
    }

    if (lines.empty() || lines[0].empty()) {
      close(client_fd);
      return;
    }

    // --- Read body, if any ---
    std::string body;
    if (content_length > 0) {
      if (!reader.ReadExact(static_cast<size_t>(content_length), body)) {
        close(client_fd);
        return;
      }
    }

    ParsedRequest req;
    req.lines = lines;
    req.body = body;

    auto request_line_parts = Split(lines[0], " ");
    if (request_line_parts.size() < 2) {
      close(client_fd);
      return;
    }
    req.method = request_line_parts[0];
    req.targets = Split(request_line_parts[1], "/");

    std::string response = HandleRequest(req, dir);

    bool should_close = std::find(lines.begin(), lines.end(), "Connection: close") != lines.end();

    ssize_t sent = send(client_fd, response.data(), response.size(), 0);
    if (sent < 0 || should_close) {
      close(client_fd);
      return;
    }
  }
}

std::string ParseDirectoryFlag(int argc, char** argv) {
  std::string dir = "./";
  const std::string kEq = "--directory=";
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--directory" && i + 1 < argc) {
      dir = argv[++i];
    } else if (StartsWith(arg, kEq)) {
      dir = arg.substr(kEq.size());
    }
  }
  return dir;
}

}  // namespace

int main(int argc, char** argv) {
  std::cout << "Logs from your program will appear here!" << std::endl;

  // Writing to a peer that has already closed its end must not kill the
  // whole process with SIGPIPE.
  std::signal(SIGPIPE, SIG_IGN);

  std::string dir = ParseDirectoryFlag(argc, argv);

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    std::cerr << "Failed to create socket\n";
    return 1;
  }

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(kPort);

  if (bind(server_fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) != 0) {
    std::cerr << "Failed to bind to port " << kPort << "\n";
    return 1;
  }

  if (listen(server_fd, 128) != 0) {
    std::cerr << "listen failed\n";
    return 1;
  }

  std::cout << "Port " << kPort << " Binded" << std::endl;

  while (true) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
    if (client_fd < 0) {
      std::cerr << "Error accepting connection\n";
      continue;
    }
    std::thread(HandleConnection, client_fd, dir).detach();
  }

  close(server_fd);
  return 0;
}
