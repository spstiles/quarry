#pragma once

#include <string>
#include <vector>

namespace connections {

enum class Type { SMB, SSH, FTP, WebDAV, WebDAVS, AFP, Unknown };

struct Connection {
  std::string id;
  std::string name;
  Type type{Type::Unknown};
  std::string server;
  int port{0};
  std::string folder;
  std::string username;
  bool rememberPassword{false};
};

std::string BuildUri(const Connection& c);
Connection ParseUri(const std::string& uri);

std::vector<Connection> LoadAll();
// Adds or updates (if id matches an existing entry). Returns the saved id.
std::string Upsert(Connection c);
void Remove(const std::string& id);

}  // namespace connections

