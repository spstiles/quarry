#include "Connections.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <optional>
#include <sstream>

#include <wx/config.h>

namespace connections {

namespace {
constexpr const char* kOrderKey = "/connections/order";

std::string PercentDecode(std::string s) {
  auto hex = [](unsigned char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
  };
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hex(static_cast<unsigned char>(s[i + 1]));
      const int lo = hex(static_cast<unsigned char>(s[i + 2]));
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

std::string PercentEncode(std::string s) {
  auto isUnreserved = [](unsigned char c) -> bool {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
  };
  std::string out;
  out.reserve(s.size());
  const char* hex = "0123456789ABCDEF";
  for (unsigned char c : s) {
    if (isUnreserved(c) || c == '/') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[(c >> 4) & 0xF]);
      out.push_back(hex[c & 0xF]);
    }
  }
  return out;
}

std::string ToLower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

Type TypeFromScheme(const std::string& scheme) {
  const auto s = ToLower(scheme);
  if (s == "smb") return Type::SMB;
  if (s == "sftp" || s == "ssh") return Type::SSH;
  if (s == "ftp") return Type::FTP;
  if (s == "dav") return Type::WebDAV;
  if (s == "davs") return Type::WebDAVS;
  if (s == "afp") return Type::AFP;
  return Type::Unknown;
}

std::string SchemeFromType(Type t) {
  switch (t) {
    case Type::SMB: return "smb";
    case Type::SSH: return "sftp";
    case Type::FTP: return "ftp";
    case Type::WebDAV: return "dav";
    case Type::WebDAVS: return "davs";
    case Type::AFP: return "afp";
    default: return "smb";
  }
}

std::vector<std::string> Split(const std::string& s, char delim) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == delim) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string Join(const std::vector<std::string>& parts, char delim) {
  std::string out;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i) out.push_back(delim);
    out += parts[i];
  }
  return out;
}

std::string GenerateId() {
  static unsigned long counter = 0;
  counter++;
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  std::ostringstream oss;
  oss << "c" << std::hex << now << "-" << std::hex << counter;
  return oss.str();
}

wxString KeyFor(const std::string& id, const char* field) {
  return wxString::Format("/connections/%s/%s", id, field);
}

std::vector<std::string> LoadOrder(wxConfig& cfg) {
  wxString order;
  if (!cfg.Read(kOrderKey, &order) || order.empty()) return {};
  std::vector<std::string> ids;
  for (const auto& s : Split(order.ToStdString(), ';')) {
    if (!s.empty()) ids.push_back(s);
  }
  return ids;
}

void SaveOrder(wxConfig& cfg, const std::vector<std::string>& ids) {
  cfg.Write(kOrderKey, wxString::FromUTF8(Join(ids, ';')));
}

}  // namespace

std::string BuildUri(const Connection& c) {
  const auto scheme = SchemeFromType(c.type);
  std::string uri = scheme + "://";

  // Note: we intentionally omit username in the URI for now because:
  // - GIO supports user@host in some schemes, but not consistently.
  // - It's nicer to store username separately and prompt for password at connect time.
  uri += c.server;

  const bool portAllowed = (scheme == "sftp" || scheme == "ftp" || scheme == "dav" || scheme == "davs");
  if (portAllowed && c.port > 0) uri += ":" + std::to_string(c.port);

  std::string path = c.folder;
  if (path.empty()) path = (scheme == "smb" || scheme == "afp") ? "" : "/";
  if (!path.empty() && path.front() != '/') path.insert(path.begin(), '/');

  uri += PercentEncode(path);
  return uri;
}

Connection ParseUri(const std::string& uri) {
  Connection c;
  c.type = Type::Unknown;

  const auto schemePos = uri.find("://");
  if (schemePos == std::string::npos) return c;
  const auto scheme = uri.substr(0, schemePos);
  c.type = TypeFromScheme(scheme);

  const auto rest = uri.substr(schemePos + 3);
  const auto slash = rest.find('/');
  const std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  const std::string path = (slash == std::string::npos) ? "" : rest.substr(slash);

  // authority can be [user@]host[:port]
  std::string hostPort = authority;
  const auto at = hostPort.find('@');
  if (at != std::string::npos) {
    c.username = hostPort.substr(0, at);
    hostPort = hostPort.substr(at + 1);
  }

  const auto colon = hostPort.rfind(':');
  if (colon != std::string::npos) {
    c.server = hostPort.substr(0, colon);
    const auto portStr = hostPort.substr(colon + 1);
    try {
      c.port = std::stoi(portStr);
    } catch (...) {
      c.port = 0;
    }
  } else {
    c.server = hostPort;
  }

  c.folder = PercentDecode(path);
  return c;
}

std::vector<Connection> LoadAll() {
  wxConfig cfg("Quarry");
  const auto order = LoadOrder(cfg);
  std::vector<Connection> out;
  out.reserve(order.size());

  for (const auto& id : order) {
    Connection c;
    c.id = id;
    wxString name;
    if (cfg.Read(KeyFor(id, "name"), &name)) c.name = name.ToStdString();

    long type = 0;
    if (cfg.Read(KeyFor(id, "type"), &type)) c.type = static_cast<Type>(type);

    wxString server;
    if (cfg.Read(KeyFor(id, "server"), &server)) c.server = server.ToStdString();

    long port = 0;
    if (cfg.Read(KeyFor(id, "port"), &port)) c.port = static_cast<int>(port);

    wxString folder;
    if (cfg.Read(KeyFor(id, "folder"), &folder)) c.folder = folder.ToStdString();

    wxString username;
    if (cfg.Read(KeyFor(id, "username"), &username)) c.username = username.ToStdString();

    bool remember = false;
    if (cfg.Read(KeyFor(id, "rememberPassword"), &remember)) c.rememberPassword = remember;

    if (c.name.empty()) c.name = BuildUri(c);
    out.push_back(std::move(c));
  }

  return out;
}

std::string Upsert(Connection c) {
  wxConfig cfg("Quarry");

  auto order = LoadOrder(cfg);
  if (c.id.empty()) c.id = GenerateId();

  const auto exists = std::find(order.begin(), order.end(), c.id) != order.end();
  if (!exists) order.push_back(c.id);

  cfg.Write(KeyFor(c.id, "name"), wxString::FromUTF8(c.name));
  cfg.Write(KeyFor(c.id, "type"), static_cast<long>(c.type));
  cfg.Write(KeyFor(c.id, "server"), wxString::FromUTF8(c.server));
  cfg.Write(KeyFor(c.id, "port"), static_cast<long>(c.port));
  cfg.Write(KeyFor(c.id, "folder"), wxString::FromUTF8(c.folder));
  cfg.Write(KeyFor(c.id, "username"), wxString::FromUTF8(c.username));
  cfg.Write(KeyFor(c.id, "rememberPassword"), c.rememberPassword);

  SaveOrder(cfg, order);
  cfg.Flush();
  return c.id;
}

void Remove(const std::string& id) {
  if (id.empty()) return;
  wxConfig cfg("Quarry");

  auto order = LoadOrder(cfg);
  order.erase(std::remove(order.begin(), order.end(), id), order.end());
  SaveOrder(cfg, order);

  cfg.DeleteGroup(wxString::Format("/connections/%s", id));
  cfg.Flush();
}

}  // namespace connections

