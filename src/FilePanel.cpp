#include "FilePanel.h"

#include "NavIcons.h"
#include "util.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>
#include <system_error>

#include <wx/button.h>
#include <wx/dataview.h>
#include <wx/bmpbuttn.h>
#include <wx/checkbox.h>
#include <wx/radiobox.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/choicdlg.h>
#include <wx/artprov.h>
#include <wx/imaglist.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/settings.h>
#include <wx/splitter.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/time.h>
#include <wx/treectrl.h>
#include <wx/utils.h>

#include <sys/wait.h>

#ifdef QUARRY_USE_GIO
#include <gio/gio.h>
#endif

namespace fs = std::filesystem;

namespace {
constexpr int COL_NAME = 0;
constexpr int COL_TYPE = 1;
constexpr int COL_SIZE = 2;
constexpr int COL_MOD = 3;
constexpr int COL_FULLPATH = 4;

const fs::path kVirtualRecent{"recent://"};

enum class TreeIcon : int {
  Folder = 0,
  Home,
  Drive,
  Computer,
};

class TreeNodeData final : public wxTreeItemData {
public:
  enum class Kind { Path, DevicesContainer, NetworkContainer };

  explicit TreeNodeData(fs::path p, Kind k = Kind::Path) : path(std::move(p)), kind(k) {}

  fs::path path;
  Kind kind{Kind::Path};
};

std::string PercentDecode(std::string s);
std::string TrimRight(std::string s);

bool LooksLikeUri(const std::string& s) {
  const auto pos = s.find("://");
  return pos != std::string::npos && pos > 0;
}

std::string UriScheme(const std::string& s) {
  const auto pos = s.find("://");
  if (pos == std::string::npos) return {};
  return s.substr(0, pos);
}

std::string UriLastSegment(const std::string& s) {
  const auto pos = s.find("://");
  if (pos == std::string::npos) return {};
  size_t start = pos + 3;
  // Skip any extra slashes.
  while (start < s.size() && s[start] == '/') start++;
  if (start >= s.size()) return {};
  size_t end = s.size();
  while (end > start && s[end - 1] == '/') end--;
  const auto lastSlash = s.rfind('/', end - 1);
  if (lastSlash == std::string::npos || lastSlash < start) return PercentDecode(s.substr(start, end - start));
  return PercentDecode(s.substr(lastSlash + 1, end - (lastSlash + 1)));
}

bool ParseDnssdService(std::string s, std::string* hostOut, std::string* protoOut) {
  // Examples:
  //   dnssd-server-NAS0002._smb._tcp
  //   NAS0002._afp._tcp
  // We want: host=NAS0002, proto=smb/afp
  if (hostOut) hostOut->clear();
  if (protoOut) protoOut->clear();

  s = PercentDecode(s);

  const std::string prefix = "dnssd-server-";
  if (s.rfind(prefix, 0) == 0) s = s.substr(prefix.size());

  const auto sep = s.rfind("._");
  if (sep == std::string::npos) return false;
  if (s.size() < 6) return false;

  const auto tcpPos = s.find("._tcp", sep);
  if (tcpPos == std::string::npos) return false;

  const std::string host = s.substr(0, sep);
  if (host.empty()) return false;

  // s[sep..] looks like "._proto._tcp"
  const size_t protoStart = sep + 2;
  const size_t protoEnd = s.find("._", protoStart);
  if (protoEnd == std::string::npos || protoEnd <= protoStart) return false;
  const std::string proto = s.substr(protoStart, protoEnd - protoStart);
  if (proto.empty()) return false;

  if (hostOut) *hostOut = host;
  if (protoOut) *protoOut = proto;
  return true;
}

std::string UpperAscii(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string UriAuthorityHost(const std::string& uri) {
  const auto pos = uri.find("://");
  if (pos == std::string::npos) return {};
  size_t i = pos + 3;
  while (i < uri.size() && uri[i] == '/') i++;
  if (i >= uri.size()) return {};

  size_t end = uri.find('/', i);
  if (end == std::string::npos) end = uri.size();
  std::string auth = uri.substr(i, end - i);

  // Strip userinfo.
  const auto at = auth.rfind('@');
  if (at != std::string::npos) auth = auth.substr(at + 1);

  // IPv6 [::1]:port
  if (!auth.empty() && auth.front() == '[') {
    const auto rb = auth.find(']');
    if (rb != std::string::npos) return auth.substr(1, rb - 1);
    return auth;
  }

  // Strip port.
  const auto colon = auth.find(':');
  if (colon != std::string::npos) auth = auth.substr(0, colon);
  return PercentDecode(auth);
}

std::string PrettyNetworkLabel(const std::string& uriOrName) {
  // Prefer uri parsing when available.
  if (LooksLikeUri(uriOrName)) {
    const auto scheme = UriScheme(uriOrName);
    if (scheme == "smb" || scheme == "afp" || scheme == "sftp" || scheme == "ftp" || scheme == "dav" ||
        scheme == "davs") {
      const auto host = UriAuthorityHost(uriOrName);
      if (host.empty()) return uriOrName;
      if (scheme == "smb") return host;
      return host + "(" + UpperAscii(scheme) + ")";
    }
  }

  // Otherwise try DNS-SD service name.
  std::string host, proto;
  if (ParseDnssdService(uriOrName, &host, &proto)) {
    if (proto == "smb") return host;
    if (proto == "afpovertcp") return host + "(AFP)";
    return host + "(" + UpperAscii(proto) + ")";
  }

  return PercentDecode(uriOrName);
}

struct RecentHostEntry {
  std::string key;     // canonical key, e.g. smb://nas0002/
  std::string display; // user-facing label, e.g. NAS0002 or NAS0002(AFP)
};

static std::vector<RecentHostEntry> g_recentHosts;

const std::vector<RecentHostEntry>& GetRecentHosts() { return g_recentHosts; }

std::string LowerAscii(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

int UppercaseCount(const std::string& s) {
  int n = 0;
  for (const auto c : s) {
    if (c >= 'A' && c <= 'Z') n++;
  }
  return n;
}

std::optional<std::string> HostRootForUri(const std::string& uri) {
  if (!LooksLikeUri(uri)) return std::nullopt;
  const auto scheme = UriScheme(uri);
  if (scheme != "smb" && scheme != "afp" && scheme != "sftp") return std::nullopt;

  auto host = UriAuthorityHost(uri);
  if (host.empty()) return std::nullopt;
  host = LowerAscii(host);
  return scheme + "://" + host + "/";
}

bool AddRecentHost(const std::string& uri) {
  const auto root = HostRootForUri(uri);
  if (!root) return false;

  const auto scheme = UriScheme(*root);
  const auto hostDisplay = UriAuthorityHost(uri);
  if (hostDisplay.empty()) return false;

  std::string display = hostDisplay;
  if (scheme == "afp") display = hostDisplay + "(AFP)";
  else if (scheme == "sftp") display = hostDisplay + "(SSH)";

  // De-dupe by canonical key (case-insensitive host).
  auto it = std::find_if(g_recentHosts.begin(), g_recentHosts.end(),
                         [&](const RecentHostEntry& e) { return e.key == *root; });
  if (it != g_recentHosts.end()) {
    // Prefer the display with "more" uppercase characters (usually from network discovery).
    if (UppercaseCount(display) > UppercaseCount(it->display)) it->display = display;
    display = it->display;
    g_recentHosts.erase(it);
  }

  g_recentHosts.insert(g_recentHosts.begin(), RecentHostEntry{.key = *root, .display = display});
  if (g_recentHosts.size() > 15) g_recentHosts.resize(15);
  return true;
}

bool IsBareSchemeUri(const std::string& s) {
  const auto pos = s.find("://");
  if (pos == std::string::npos) return false;
  const auto rest = s.substr(pos + 3);
  if (rest.empty()) return true;
  if (rest == "/") return true;
  return false;
}

bool IsGioLocationUri(const std::string& s) {
  if (!LooksLikeUri(s)) return false;
  const auto scheme = UriScheme(s);
  if (scheme == "recent") return false;
  if (scheme == "file") return false;
  return true;
}

std::string ShellQuote(std::string s) {
  // POSIX-safe single-quote escaping.
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('\'');
  for (char c : s) {
    if (c == '\'') out.append("'\\''");
    else out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

std::string TrimRight(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

std::vector<std::string> SplitTabs(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    const size_t end = s.find('\t', start);
    if (end == std::string::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

std::string ExtractAttrValue(const std::string& attrs,
                             const std::string& key,
                             const std::vector<std::string>& followingKeys) {
  const std::string needle = key + "=";
  const auto pos = attrs.find(needle);
  if (pos == std::string::npos) return {};
  const size_t start = pos + needle.size();
  size_t end = attrs.size();
  for (const auto& fk : followingKeys) {
    const std::string nextNeedle = " " + fk + "=";
    const auto npos = attrs.find(nextNeedle, start);
    if (npos != std::string::npos && npos < end) end = npos;
  }
  return attrs.substr(start, end - start);
}

std::string FormatUnixSeconds(long long seconds) {
  if (seconds <= 0) return {};
  const std::time_t tt = static_cast<std::time_t>(seconds);
  std::tm tm{};
  localtime_r(&tt, &tm);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
  return std::string(buf);
}

std::string PercentDecode(std::string s) {
  std::string out;
  out.reserve(s.size());
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };

  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hex(s[i + 1]);
      const int lo = hex(s[i + 2]);
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

std::optional<fs::path> UriToPath(const std::string& uri) {
  // Handles file:// URIs from recently-used.xbel.
  constexpr const char* kFile = "file://";
  if (uri.rfind(kFile, 0) != 0) return std::nullopt;

  std::string rest = uri.substr(std::char_traits<char>::length(kFile));
  // file:///path...
  // file://localhost/path...
  if (rest.rfind("localhost/", 0) == 0) rest = rest.substr(std::char_traits<char>::length("localhost"));

  // Keep leading slash for absolute paths.
  const auto decoded = PercentDecode(rest);
  if (decoded.empty()) return std::nullopt;
  if (decoded[0] != '/') return std::nullopt;
  return fs::path(decoded);
}

std::optional<fs::path> ReadXdgUserDir(const std::string& key) {
  const auto home = wxGetHomeDir().ToStdString();
  if (home.empty()) return std::nullopt;
  const fs::path path = fs::path(home) / ".config" / "user-dirs.dirs";

  std::error_code ec;
  if (!fs::exists(path, ec)) return std::nullopt;

  std::ifstream f(path);
  if (!f.is_open()) return std::nullopt;

  const std::string prefix = "XDG_" + key + "_DIR=";
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind(prefix, 0) != 0) continue;
    auto value = line.substr(prefix.size());
    // Trim quotes.
    if (!value.empty() && (value.front() == '"' || value.front() == '\'')) value.erase(value.begin());
    if (!value.empty() && (value.back() == '"' || value.back() == '\'')) value.pop_back();

    // Expand $HOME.
    const std::string homeVar = "$HOME";
    const auto pos = value.find(homeVar);
    if (pos != std::string::npos) value.replace(pos, homeVar.size(), home);

    if (value.empty()) return std::nullopt;
    return fs::path(value);
  }
  return std::nullopt;
}

fs::path DefaultUserDir(const std::string& name) {
  const auto home = wxGetHomeDir().ToStdString();
  if (home.empty()) return {};
  return fs::path(home) / name;
}

fs::path TrashFilesDir() {
  const auto home = wxGetHomeDir().ToStdString();
  if (home.empty()) return {};
  return fs::path(home) / ".local" / "share" / "Trash" / "files";
}

std::vector<fs::path> ReadRecentPaths(size_t limit) {
  std::vector<fs::path> paths;
  paths.reserve(std::min<size_t>(limit, 200));

  const auto home = wxGetHomeDir().ToStdString();
  if (home.empty()) return paths;

  const fs::path xbel = fs::path(home) / ".local" / "share" / "recently-used.xbel";
  std::error_code ec;
  if (!fs::exists(xbel, ec)) return paths;

  std::ifstream f(xbel);
  if (!f.is_open()) return paths;

  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (content.empty()) return paths;

  std::unordered_map<std::string, bool> seen;
  const std::string needle = "href=\"file://";
  size_t pos = 0;
  while (paths.size() < limit) {
    pos = content.find(needle, pos);
    if (pos == std::string::npos) break;
    pos += 6; // skip href="
    const size_t end = content.find('"', pos);
    if (end == std::string::npos) break;
    const auto uri = content.substr(pos, end - pos);
    pos = end + 1;

    const auto p = UriToPath(uri);
    if (!p) continue;
    const auto key = p->string();
    if (seen.find(key) != seen.end()) continue;
    seen[key] = true;

    if (!fs::exists(*p, ec) || ec) continue;
    paths.push_back(*p);
  }

  return paths;
}

std::vector<FilePanel::Entry> ListGioLocation(const std::string& uri, std::string* err, wxWindow* parentForAuth);
bool GioMountLocation(const std::string& uri, std::string* err, wxWindow* parentForAuth);

#ifdef QUARRY_USE_GIO
struct MountCreds {
  enum class RememberMode { ForgetImmediately = 0, Session = 1, Forever = 2 };

  std::string username;
  std::string password;
  std::string domain;
  bool anonymous{false};
  RememberMode rememberMode{RememberMode::Session};
};

std::string CredsCacheKeyForUri(const std::string& uri) {
  const auto scheme = UriScheme(uri);
  const auto pos = uri.find("://");
  if (pos == std::string::npos) return scheme;
  size_t start = pos + 3;
  while (start < uri.size() && uri[start] == '/') start++;

  // Extract authority/host part.
  const size_t firstSlash = uri.find('/', start);
  const std::string host = (firstSlash == std::string::npos) ? uri.substr(start) : uri.substr(start, firstSlash - start);

  if (scheme == "smb") {
    // smb://HOST/SHARE/...
    if (firstSlash == std::string::npos) return "smb://" + host;
    size_t shareStart = firstSlash + 1;
    while (shareStart < uri.size() && uri[shareStart] == '/') shareStart++;
    const size_t shareEnd = uri.find('/', shareStart);
    const std::string share = (shareStart >= uri.size())
                                  ? std::string{}
                                  : (shareEnd == std::string::npos ? uri.substr(shareStart)
                                                                   : uri.substr(shareStart, shareEnd - shareStart));
    if (share.empty()) return "smb://" + host;
    return "smb://" + host + "/" + share;
  }

  if (!host.empty()) return scheme + "://" + host;
  return scheme;
}

static std::unordered_map<std::string, MountCreds> g_sessionMountCreds;

GPasswordSave RememberModeToPasswordSave(MountCreds::RememberMode mode) {
  switch (mode) {
    case MountCreds::RememberMode::Forever: return G_PASSWORD_SAVE_PERMANENTLY;
    case MountCreds::RememberMode::Session:
    case MountCreds::RememberMode::ForgetImmediately:
    default: return G_PASSWORD_SAVE_NEVER;
  }
}

std::optional<MountCreds> PromptMountCreds(wxWindow* parent,
                                          const std::string& message,
                                          const std::string& defaultUser,
                                          const std::string& defaultDomain,
                                          GAskPasswordFlags flags) {
  wxDialog dlg(parent, wxID_ANY, "Authentication Required", wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

  auto* sizer = new wxBoxSizer(wxVERTICAL);
  dlg.SetSizer(sizer);

  sizer->Add(new wxStaticText(&dlg, wxID_ANY, wxString::FromUTF8(message)),
             0, wxALL | wxEXPAND, 10);

  auto* grid = new wxFlexGridSizer(2, 8, 8);
  grid->AddGrowableCol(1, 1);
  sizer->Add(grid, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  auto* userLabel = new wxStaticText(&dlg, wxID_ANY, "Username");
  auto* userCtrl = new wxTextCtrl(&dlg, wxID_ANY, wxString::FromUTF8(defaultUser));
  auto* passLabel = new wxStaticText(&dlg, wxID_ANY, "Password");
  auto* passCtrl = new wxTextCtrl(&dlg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
  auto* domainLabel = new wxStaticText(&dlg, wxID_ANY, "Domain");
  auto* domainCtrl = new wxTextCtrl(&dlg, wxID_ANY, wxString::FromUTF8(defaultDomain));

  grid->Add(userLabel, 0, wxALIGN_CENTER_VERTICAL);
  grid->Add(userCtrl, 1, wxEXPAND);
  grid->Add(passLabel, 0, wxALIGN_CENTER_VERTICAL);
  grid->Add(passCtrl, 1, wxEXPAND);
  grid->Add(domainLabel, 0, wxALIGN_CENTER_VERTICAL);
  grid->Add(domainCtrl, 1, wxEXPAND);

  auto* anonymous = new wxCheckBox(&dlg, wxID_ANY, "Anonymous");

  wxArrayString rememberChoices;
  rememberChoices.Add("Forget password immediately");
  rememberChoices.Add("Remember until logout");
  rememberChoices.Add("Remember forever");
  auto* rememberMode = new wxRadioBox(&dlg,
                                      wxID_ANY,
                                      "Password",
                                      wxDefaultPosition,
                                      wxDefaultSize,
                                      rememberChoices,
                                      1,
                                      wxRA_SPECIFY_ROWS);
  sizer->Add(anonymous, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
  sizer->Add(rememberMode, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  anonymous->SetValue(false);
  rememberMode->SetSelection(static_cast<int>(MountCreds::RememberMode::Session));

  // Honor requested fields.
  const bool needUser = (flags & G_ASK_PASSWORD_NEED_USERNAME) != 0;
  const bool needPass = (flags & G_ASK_PASSWORD_NEED_PASSWORD) != 0;
  const bool needDomain = (flags & G_ASK_PASSWORD_NEED_DOMAIN) != 0;
  userLabel->Show(needUser);
  userCtrl->Show(needUser);
  passLabel->Show(needPass);
  passCtrl->Show(needPass);
  domainLabel->Show(needDomain);
  domainCtrl->Show(needDomain);

  const bool allowAnon = (flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED) != 0;
  anonymous->Show(allowAnon);

  // Remember forever requires saving support (keyring); session remembering is always available.
  const bool allowForever = (flags & G_ASK_PASSWORD_SAVING_SUPPORTED) != 0;
  rememberMode->Show(needPass);
  rememberMode->Enable(2, allowForever);

  auto* btnSizer = dlg.CreateButtonSizer(wxOK | wxCANCEL);
  sizer->Add(btnSizer, 0, wxALL | wxEXPAND, 10);

  dlg.Fit();
  dlg.Layout();
  dlg.CentreOnParent();

  if (dlg.ShowModal() != wxID_OK) return std::nullopt;

  MountCreds out;
  out.anonymous = allowAnon && anonymous->GetValue();
  if (rememberMode->IsShown()) {
    const int sel = rememberMode->GetSelection();
    if (sel == 0) out.rememberMode = MountCreds::RememberMode::ForgetImmediately;
    else if (sel == 2 && allowForever) out.rememberMode = MountCreds::RememberMode::Forever;
    else out.rememberMode = MountCreds::RememberMode::Session;
  }
  out.username = userCtrl->IsShown() ? userCtrl->GetValue().ToStdString() : "";
  out.password = passCtrl->IsShown() ? passCtrl->GetValue().ToStdString() : "";
  out.domain = domainCtrl->IsShown() ? domainCtrl->GetValue().ToStdString() : "";
  return out;
}

struct MountResult {
  bool ok{false};
  std::string error;
  bool aborted{false};
};

MountResult GioMountWithUi(wxWindow* parent, const std::string& uri) {
  MountResult result;

  GFile* file = g_file_new_for_uri(uri.c_str());
  if (!file) {
    result.error = "Invalid URI.";
    return result;
  }

  GMountOperation* op = g_mount_operation_new();
  if (!op) {
    g_object_unref(file);
    result.error = "Unable to create mount operation.";
    return result;
  }

  struct Ctx {
    wxWindow* parent{nullptr};
    GMainLoop* loop{nullptr};
    MountResult* out{nullptr};
    std::string cacheKey{};
  } ctx{parent, nullptr, &result};

  ctx.cacheKey = CredsCacheKeyForUri(uri);

  g_signal_connect(op,
                   "ask-password",
                   G_CALLBACK(+[](GMountOperation* mountOp,
                                  const char* message,
                                  const char* defaultUser,
                                  const char* defaultDomain,
                                  GAskPasswordFlags flags,
                                  gpointer userData) {
                     auto* c = static_cast<Ctx*>(userData);
                     const std::string msg = message ? message : "Authentication required.";
                     const std::string du = defaultUser ? defaultUser : "";
                     const std::string dd = defaultDomain ? defaultDomain : "";

                     // Session-only cache: if we already authenticated for this server/share in this run,
                     // reuse it even if the user didn’t choose to save permanently.
	                     {
	                       const auto it = g_sessionMountCreds.find(c->cacheKey);
	                       if (it != g_sessionMountCreds.end()) {
	                         const auto& creds = it->second;
	                         if (creds.anonymous) {
	                           g_mount_operation_set_anonymous(mountOp, TRUE);
	                         } else {
	                           g_mount_operation_set_anonymous(mountOp, FALSE);
	                           g_mount_operation_set_username(mountOp, creds.username.c_str());
	                           g_mount_operation_set_password(mountOp, creds.password.c_str());
	                           g_mount_operation_set_domain(mountOp, creds.domain.c_str());
	                         }
	                         g_mount_operation_set_password_save(mountOp, RememberModeToPasswordSave(creds.rememberMode));
	                         g_mount_operation_reply(mountOp, G_MOUNT_OPERATION_HANDLED);
	                         return;
	                       }
	                     }

                     const auto creds = PromptMountCreds(c->parent, msg, du, dd, flags);
                     if (!creds) {
                       c->out->aborted = true;
                       g_mount_operation_reply(mountOp, G_MOUNT_OPERATION_ABORTED);
                       return;
                     }

                     g_mount_operation_set_anonymous(mountOp, creds->anonymous);
	                     if (!creds->anonymous) {
	                       g_mount_operation_set_username(mountOp, creds->username.c_str());
	                       g_mount_operation_set_password(mountOp, creds->password.c_str());
	                       g_mount_operation_set_domain(mountOp, creds->domain.c_str());
	                     }
	                     g_mount_operation_set_password_save(mountOp, RememberModeToPasswordSave(creds->rememberMode));
	                     g_mount_operation_reply(mountOp, G_MOUNT_OPERATION_HANDLED);

	                     // Remember for this instance if requested (session or forever).
	                     if (creds->rememberMode != MountCreds::RememberMode::ForgetImmediately) {
	                       g_sessionMountCreds[c->cacheKey] = *creds;
	                     }
	                   }),
	                   &ctx);

  struct AsyncState {
    GMainLoop* loop{nullptr};
    MountResult* out{nullptr};
  } st;

  ctx.loop = g_main_loop_new(nullptr, FALSE);
  st.loop = ctx.loop;
  st.out = &result;

  g_file_mount_enclosing_volume(
      file,
      G_MOUNT_MOUNT_NONE,
      op,
      nullptr,
      +[](GObject* source, GAsyncResult* res, gpointer userData) {
        auto* s = static_cast<AsyncState*>(userData);
        GError* error = nullptr;
        const gboolean ok = g_file_mount_enclosing_volume_finish(G_FILE(source), res, &error);
        if (!ok) {
          if (error) {
            s->out->error = error->message ? error->message : "Mount failed.";
            g_error_free(error);
          } else {
            s->out->error = "Mount failed.";
          }
        } else {
          s->out->ok = true;
        }
        if (s->loop) g_main_loop_quit(s->loop);
      },
      &st);

  // Run a temporary GLib loop to wait for mount completion.
  g_main_loop_run(ctx.loop);

  g_main_loop_unref(ctx.loop);
  g_object_unref(op);
  g_object_unref(file);

  // If mount failed, clear any cached creds for this target so we prompt again next time.
  if (!result.ok) {
    g_sessionMountCreds.erase(CredsCacheKeyForUri(uri));
  }
  return result;
}

bool GioMountLocation(const std::string& uri, std::string* err, wxWindow* parentForAuth) {
  if (err) err->clear();
  if (!parentForAuth) {
    if (err) *err = "No UI available for authentication.";
    return false;
  }
  const auto mountRes = GioMountWithUi(parentForAuth, uri);
  if (mountRes.ok) return true;
  if (err && !mountRes.error.empty()) *err = mountRes.error;
  return false;
}

std::vector<FilePanel::Entry> ListGioLocation(const std::string& uri, std::string* err, wxWindow* parentForAuth) {
  std::vector<FilePanel::Entry> entries;
  if (err) err->clear();

  GFile* file = g_file_new_for_uri(uri.c_str());
  if (!file) {
    if (err) *err = "Invalid URI.";
    return entries;
  }

  // Mount first so auth can happen via our wx dialog instead of terminal prompts.
  if (parentForAuth) {
    const auto scheme = UriScheme(uri);
    if (scheme == "smb" || scheme == "network") {
      (void)GioMountLocation(uri, /*err=*/nullptr, parentForAuth);
    }
  }

  GError* error = nullptr;
  GFileEnumerator* en = g_file_enumerate_children(
      file,
      "standard::name,standard::type,standard::size,standard::target-uri,time::modified",
      G_FILE_QUERY_INFO_NONE,
      nullptr,
      &error);

  if (!en) {
    if (err) {
      if (error && error->message) *err = error->message;
      else *err = "Unable to list location.";
    }
    if (error) g_error_free(error);
    g_object_unref(file);
    return entries;
  }

  for (;;) {
    GError* nextErr = nullptr;
    GFileInfo* info = g_file_enumerator_next_file(en, nullptr, &nextErr);
    if (!info) {
      if (nextErr) {
        if (err) *err = nextErr->message ? nextErr->message : "Unable to list location.";
        g_error_free(nextErr);
      }
      break;
    }

    const char* name = g_file_info_get_name(info);
    const auto ftype = g_file_info_get_file_type(info);
    // Treat only known navigable container-like types as directories.
    // In particular, G_FILE_TYPE_SPECIAL is often not a directory and attempting to
    // enumerate it yields "Not a directory".
    const bool isDir = ftype == G_FILE_TYPE_DIRECTORY || ftype == G_FILE_TYPE_MOUNTABLE ||
                       ftype == G_FILE_TYPE_SHORTCUT;

    std::uintmax_t size = 0;
    if (!isDir) size = static_cast<std::uintmax_t>(g_file_info_get_size(info));

    std::string modified;
    GDateTime* dt = g_file_info_get_modification_date_time(info);
    if (dt) {
      const gint64 unixSec = g_date_time_to_unix(dt);
      modified = FormatUnixSeconds(static_cast<long long>(unixSec));
    }

    std::string fullPath;
    if (const char* target = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI)) {
      fullPath = target;
    }

    // Build child URI if we weren't given a target.
    GFile* child = nullptr;
    char* childUri = nullptr;
    if (fullPath.empty()) {
      child = g_file_get_child(file, name ? name : "");
      childUri = child ? g_file_get_uri(child) : nullptr;
      if (childUri) fullPath = childUri;
    }

    entries.push_back(FilePanel::Entry{
        .name = name ? name : "",
        .isDir = isDir,
        .size = size,
        .modified = modified,
        .fullPath = fullPath,
    });

    if (childUri) g_free(childUri);
    if (child) g_object_unref(child);
    g_object_unref(info);
  }

  g_object_unref(en);
  g_object_unref(file);
  return entries;
}
#else
std::vector<FilePanel::Entry> ListGioLocation(const std::string& uri, std::string* err, wxWindow*) {
  std::vector<FilePanel::Entry> entries;
  if (err) err->clear();

  // Prefer "gio list" for remote locations (smb://, sftp://, etc.).
  // We include --hidden to match local directory listing behavior.
  const std::string cmd =
      "gio list --hidden -l -u -d -a standard::name,time::modified " + ShellQuote(uri) + " 2>&1";

  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    if (err) *err = "Unable to run gio list.";
    return entries;
  }

  std::string output;
  std::array<char, 4096> buf{};
  while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
    output.append(buf.data());
  }

  const int rcRaw = pclose(pipe);
  int rc = -1;
  if (rcRaw != -1 && WIFEXITED(rcRaw)) rc = WEXITSTATUS(rcRaw);
  if (rc != 0) {
    if (err) {
      auto msg = TrimRight(output);
      if (msg.empty()) msg = "gio list failed.";
      *err = msg;
    }
    return entries;
  }

  std::istringstream in(output);
  std::string line;
  while (std::getline(in, line)) {
    line = TrimRight(line);
    if (line.empty()) continue;
    if (line.rfind("gio:", 0) == 0) continue;

    const auto cols = SplitTabs(line);
    if (cols.size() < 3) continue;

    const std::string itemUri = cols[0];
    std::uintmax_t size = 0;
    try {
      size = static_cast<std::uintmax_t>(std::stoull(cols[1]));
    } catch (...) {
      size = 0;
    }

    const std::string typeToken = cols[2];
    // For GIO, entries like network shares often show up as "(special)" rather than "(directory)".
    // Treat anything that isn't a regular file as directory-like so double-click navigates.
    const bool isDir = (typeToken.find("directory") != std::string::npos) ||
                       (typeToken.find("regular") == std::string::npos);

    std::string name;
    std::string modified;
    if (cols.size() >= 4) {
      const auto& attrs = cols[3];
      name = ExtractAttrValue(attrs, "standard::name", {"time::modified"});
      const auto mod = ExtractAttrValue(attrs, "time::modified", {});
      try {
        modified = FormatUnixSeconds(std::stoll(mod));
      } catch (...) {
        modified.clear();
      }
    }
    if (name.empty()) {
      // Fallback: best-effort basename from URI.
      auto trimmed = itemUri;
      while (trimmed.size() > 3 && trimmed.back() == '/') trimmed.pop_back();
      const auto slash = trimmed.find_last_of('/');
      name = (slash == std::string::npos) ? trimmed : trimmed.substr(slash + 1);
      name = PercentDecode(name);
    }

    entries.push_back(FilePanel::Entry{
        .name = name,
        .isDir = isDir,
        .size = size,
        .modified = modified,
        .fullPath = itemUri,
    });
  }

  return entries;
}

bool GioMountLocation(const std::string& uri, std::string* err, wxWindow*) {
  if (err) err->clear();
  const wxString cmd0 = "gio";
  const wxString cmd1 = "mount";
  const wxString cmd2 = wxString::FromUTF8(uri);
  const wxChar* const argv[] = {cmd0.wc_str(), cmd1.wc_str(), cmd2.wc_str(), nullptr};
  const long rc = wxExecute(argv, wxEXEC_SYNC);
  if (rc == -1) {
    if (err) *err = "Unable to run gio (is it installed?)";
    return false;
  }
  if (rc != 0) {
    if (err) *err = "gio mount failed (exit code " + std::to_string(rc) + ").";
    return false;
  }
  return true;
}
#endif

std::string GetRowName(wxDataViewListCtrl* list, unsigned int row) {
  wxVariant v;
  list->GetValue(v, row, COL_NAME);
  if (v.GetType() == "wxDataViewIconText") {
    wxDataViewIconText it;
    it << v;
    return it.GetText().ToStdString();
  }
  return v.GetString().ToStdString();
}

void SetRowName(wxDataViewListCtrl* list, unsigned int row, const std::string& name, bool isDir) {
  const auto artId = isDir ? wxART_FOLDER : wxART_NORMAL_FILE;
  const auto bundle = wxArtProvider::GetBitmapBundle(artId, wxART_OTHER, wxSize(16, 16));
  wxDataViewIconText iconText(wxString::FromUTF8(name), bundle);
  wxVariant v;
  v << iconText;
  list->SetValue(v, row, COL_NAME);
}

enum MenuId : int {
  ID_CTX_OPEN = wxID_HIGHEST + 200,
  ID_CTX_COPY,
  ID_CTX_CUT,
  ID_CTX_PASTE,
  ID_CTX_RENAME,
  ID_CTX_TRASH,
  ID_CTX_DELETE_PERM,
  ID_CTX_NEW_FOLDER,
  ID_CTX_PROPERTIES
};

enum class ExistsChoice { Overwrite, Skip, Rename, Cancel };

ExistsChoice PromptExists(wxWindow* parent, const fs::path& dst) {
  wxArrayString choices;
  choices.Add("Overwrite");
  choices.Add("Skip");
  choices.Add("Rename");
  choices.Add("Cancel");
  const wxString dstWx = wxString::FromUTF8(dst.string());
  wxSingleChoiceDialog dlg(parent,
                           wxString::Format("Destination already exists:\n\n%s", dstWx.c_str()),
                           "File exists",
                           choices);
  if (dlg.ShowModal() != wxID_OK) return ExistsChoice::Cancel;
  switch (dlg.GetSelection()) {
    case 0: return ExistsChoice::Overwrite;
    case 1: return ExistsChoice::Skip;
    case 2: return ExistsChoice::Rename;
    default: return ExistsChoice::Cancel;
  }
}

struct AppClipboard {
  enum class Mode { Copy, Cut };
  Mode mode{Mode::Copy};
  std::vector<fs::path> paths{};
};

static std::optional<AppClipboard> g_clipboard;

std::vector<FilePanel::Entry> ListDir(const fs::path& dir, std::string* errorMessage) {
  std::vector<FilePanel::Entry> entries;

  std::error_code ec;
  fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
  if (ec) {
    if (errorMessage) *errorMessage = ec.message();
    return entries;
  }

  for (const auto& de : it) {
    std::error_code statEc;
    const auto status = de.status(statEc); // follows symlinks (treat symlink-to-dir as dir)
    if (statEc) continue;

    const bool isDir = fs::is_directory(status);
    std::uintmax_t size = 0;
    if (!isDir) {
      size = de.file_size(ec);
      if (ec) size = 0;
    }

    const auto filename = de.path().filename().string();
    entries.push_back(FilePanel::Entry{
        .name = filename,
        .isDir = isDir,
        .size = size,
        .modified = FormatFileTime(de.last_write_time(ec)),
        .fullPath = de.path().string(),
    });
  }

  return entries;
}
} // namespace

void FilePanel::SeedMountCredentials(const std::string& uri,
                                     const std::string& username,
                                     const std::string& password,
                                     bool rememberForever) {
#ifdef QUARRY_USE_GIO
  const auto cacheKey = CredsCacheKeyForUri(uri);
  if (cacheKey.empty()) return;

  MountCreds creds;
  creds.anonymous = false;
  creds.username = username;
  creds.password = password;
  creds.domain.clear();
  creds.rememberMode =
      rememberForever ? MountCreds::RememberMode::Forever : MountCreds::RememberMode::Session;

  g_sessionMountCreds[cacheKey] = std::move(creds);
#else
  (void)uri;
  (void)username;
  (void)password;
  (void)rememberForever;
#endif
}

FilePanel::FilePanel(wxWindow* parent) : wxPanel(parent, wxID_ANY) { BuildLayout(); }

void FilePanel::BuildLayout() {
  split_ = new wxSplitterWindow(this, wxID_ANY);
  split_->SetSashGravity(0.0);
  split_->SetMinimumPaneSize(160);

  tree_ = new wxTreeCtrl(split_,
                         wxID_ANY,
                         wxDefaultPosition,
                         wxDefaultSize,
                         wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_HIDE_ROOT | wxTR_DEFAULT_STYLE);

  auto* listPane = new wxPanel(split_, wxID_ANY);
  auto* listSizer = new wxBoxSizer(wxVERTICAL);

  auto* listToolbar = new wxBoxSizer(wxHORIZONTAL);
  pathCtrl_ = new wxTextCtrl(listPane, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
  goBtn_ = new wxButton(listPane, wxID_ANY, "Go");

  // Match button height to the address bar height, but give the buttons a bit
  // of extra width so GTK theme padding/borders don't crop the icons.
  const int toolbarHeight = pathCtrl_->GetBestSize().y;
  const int iconSide = std::clamp(toolbarHeight - 10, 18, 24);
  const auto iconSize = wxSize(iconSide, iconSide);

  const auto mkBtn = [&](const wxArtID& id, const wxString& tooltip) -> wxBitmapButton* {
    // placeholder, bitmap set below via UpdateNavIcons()
    auto* btn = new wxBitmapButton(listPane, wxID_ANY, wxNullBitmap);
    btn->SetToolTip(tooltip);
    btn->SetBitmapMargins(2, 2);
    btn->SetMinSize(wxSize(toolbarHeight + 6, toolbarHeight));
    btn->SetMaxSize(wxSize(toolbarHeight + 6, toolbarHeight));
    return btn;
  };

  backBtn_ = mkBtn(wxART_GO_BACK, "Back");
  forwardBtn_ = mkBtn(wxART_GO_FORWARD, "Forward");
  upBtn_ = mkBtn(wxART_GO_UP, "Up");
  refreshBtn_ = mkBtn(wxART_REFRESH, "Refresh");
  homeBtn_ = mkBtn(wxART_GO_HOME, "Home");

  goBtn_->SetMinSize(wxSize(goBtn_->GetBestSize().x, toolbarHeight));
  goBtn_->SetMaxSize(wxSize(-1, toolbarHeight));

  listToolbar->Add(backBtn_, 0, wxEXPAND | wxRIGHT, 4);
  listToolbar->Add(forwardBtn_, 0, wxEXPAND | wxRIGHT, 4);
  listToolbar->Add(upBtn_, 0, wxEXPAND | wxRIGHT, 4);
  listToolbar->Add(refreshBtn_, 0, wxEXPAND | wxRIGHT, 4);
  listToolbar->Add(homeBtn_, 0, wxEXPAND | wxRIGHT, 8);
  listToolbar->Add(pathCtrl_, 1, wxEXPAND | wxRIGHT, 6);
  listToolbar->Add(goBtn_, 0, wxEXPAND);
  listSizer->Add(listToolbar, 0, wxEXPAND | wxALL, 8);

  list_ = new wxDataViewListCtrl(listPane, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                wxDV_ROW_LINES | wxDV_VERT_RULES | wxDV_MULTIPLE);
  list_->AppendIconTextColumn("Name", wxDATAVIEW_CELL_EDITABLE, 260, wxALIGN_LEFT);
  list_->AppendTextColumn("Type", wxDATAVIEW_CELL_INERT, 70, wxALIGN_LEFT);
  list_->AppendTextColumn("Size", wxDATAVIEW_CELL_INERT, 90, wxALIGN_RIGHT);
  list_->AppendTextColumn("Modified", wxDATAVIEW_CELL_INERT, 150, wxALIGN_LEFT);
  list_->AppendTextColumn("FullPath", wxDATAVIEW_CELL_INERT, 0, wxALIGN_LEFT, wxDATAVIEW_COL_HIDDEN);
  // Disable native wxDataViewListCtrl sorting; we manage sorting ourselves so we
  // can enforce "folders first" regardless of the selected column.
  for (unsigned int i = 0; i < list_->GetColumnCount(); i++) {
    if (auto* col = list_->GetColumn(i)) col->SetSortable(false);
  }

  statusText_ = new wxStaticText(listPane, wxID_ANY, "");
  listSizer->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
  listSizer->Add(statusText_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM | wxTOP, 8);
  listPane->SetSizer(listSizer);

  split_->SplitVertically(tree_, listPane, 320);

  auto* sizer = new wxBoxSizer(wxVERTICAL);
  sizer->Add(split_, 1, wxEXPAND);
  SetSizer(sizer);

  BindEvents();
  UpdateStatusText();
  UpdateNavButtons();
  UpdateNavIcons();
  UpdateSortIndicators();
  BuildComputerTree();
  SyncTreeToCurrentDir();
}

void FilePanel::BindEvents() {
  backBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { GoBack(); });
  forwardBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { GoForward(); });
  upBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { NavigateUp(); });
  refreshBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RefreshListing(); });
  homeBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { GoHome(); });
  goBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { NavigateToTextPath(); });
  pathCtrl_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { NavigateToTextPath(); });

  list_->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [this](wxDataViewEvent& e) { OpenSelectedIfDir(e); });
  list_->Bind(wxEVT_DATAVIEW_ITEM_START_EDITING, [this](wxDataViewEvent& e) {
    if (!allowInlineEdit_) {
      e.Veto();
      return;
    }
    e.Skip();
  });
  list_->Bind(wxEVT_DATAVIEW_ITEM_EDITING_DONE, [this](wxDataViewEvent& e) {
    allowInlineEdit_ = false;
    e.Skip();
  });
  list_->Bind(wxEVT_DATAVIEW_COLUMN_HEADER_CLICK, [this](wxDataViewEvent& e) {
    // Prevent wxWidgets native sorting. We keep our own sort order so we can
    // enforce "folders first" regardless of the active sort column.
    e.Veto();

    auto* col = e.GetDataViewColumn();
    if (!col) return;
    const int modelCol = col->GetModelColumn();
    if (modelCol == COL_FULLPATH) return;

    SortColumn clicked;
    switch (modelCol) {
      case COL_NAME: clicked = SortColumn::Name; break;
      case COL_TYPE: clicked = SortColumn::Type; break;
      case COL_SIZE: clicked = SortColumn::Size; break;
      case COL_MOD: clicked = SortColumn::Modified; break;
      default: return;
    }

    if (sortColumn_ == clicked) {
      sortAscending_ = !sortAscending_;
    } else {
      sortColumn_ = clicked;
      sortAscending_ = true;
    }
    ResortListing();
  });
  list_->Bind(wxEVT_DATAVIEW_COLUMN_SORTED, [this](wxDataViewEvent& e) {
    // Extra guard: some platforms may emit this even when we veto header click.
    e.Veto();
    ResortListing();
  });
  list_->Bind(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, [this](wxDataViewEvent& e) { OnListValueChanged(e); });
  list_->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent&) {
    // When selection changes due to a click, arm rename for the selected item.
    wxDataViewItemArray items;
    list_->GetSelections(items);
    if (items.size() == 1) {
      renameArmedItem_ = items[0];
      renameArmedAtMs_ = wxGetLocalTimeMillis().GetValue();
    } else {
      renameArmedItem_ = wxDataViewItem();
      renameArmedAtMs_ = 0;
    }
    UpdateStatusText();
  });
  list_->Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU, [this](wxDataViewEvent& e) { ShowListContextMenu(e); });

  // Click behavior:
  // - Fast double click opens (handled by wxEVT_DATAVIEW_ITEM_ACTIVATED).
  // - Two single clicks on the already-selected item triggers rename.
  list_->Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& e) {
    renameArmedItem_ = wxDataViewItem();
    renameArmedAtMs_ = 0;
    e.Skip();
  });
  list_->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
    if (e.Dragging() && e.LeftIsDown()) {
      renameArmedItem_ = wxDataViewItem();
      renameArmedAtMs_ = 0;
    }
    e.Skip();
  });
  list_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
    if (!list_) return;
    if (e.ControlDown() || e.ShiftDown() || e.AltDown() || e.MetaDown()) {
      renameArmedItem_ = wxDataViewItem();
      renameArmedAtMs_ = 0;
      e.Skip();
      return;
    }

    wxDataViewItem item;
    wxDataViewColumn* col = nullptr;
    list_->HitTest(e.GetPosition(), item, col);
    if (!item.IsOk()) {
      renameArmedItem_ = wxDataViewItem();
      renameArmedAtMs_ = 0;
      e.Skip();
      return;
    }

    if (!list_->IsSelected(item)) {
      // Selection is changing; don't arm rename yet.
      renameArmedItem_ = wxDataViewItem();
      renameArmedAtMs_ = 0;
      e.Skip();
      return;
    }

    const long long now = wxGetLocalTimeMillis().GetValue();
    const int dclickMs = wxSystemSettings::GetMetric(wxSYS_DCLICK_MSEC, this);

    // If this item was already armed and the second click isn't within the
    // double-click interval, initiate rename.
    if (renameArmedItem_.IsOk() && item == renameArmedItem_ && (now - renameArmedAtMs_) > dclickMs) {
      renameArmedItem_ = wxDataViewItem();
      renameArmedAtMs_ = 0;
      BeginInlineRename();
      return;
    }

    // Arm rename for a potential second click.
    renameArmedItem_ = item;
    renameArmedAtMs_ = now;
    e.Skip();
  });

  if (tree_) {
    tree_->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent&) { OnTreeSelectionChanged(); });
    tree_->Bind(wxEVT_TREE_ITEM_EXPANDING, [this](wxTreeEvent& e) { OnTreeItemExpanding(e); });
  }

  list_->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
    lastFocus_ = LastFocus::List;
    if (onFocus_) onFocus_();
    e.Skip();
  });
  list_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
    lastFocus_ = LastFocus::List;
    if (onFocus_) onFocus_();
    e.Skip();
  });

  if (tree_) {
    tree_->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
      lastFocus_ = LastFocus::Tree;
      if (onFocus_) onFocus_();
      e.Skip();
    });
    tree_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
      lastFocus_ = LastFocus::Tree;
      if (onFocus_) onFocus_();
      e.Skip();
    });
  }

  Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& e) {
    UpdateActiveVisuals();
    UpdateNavIcons();
    e.Skip();
  });
}

void FilePanel::UpdateNavIcons() {
  const int toolbarHeight = pathCtrl_ ? pathCtrl_->GetBestSize().y : 28;
  const int iconSide = std::clamp(toolbarHeight - 10, 18, 24);
  const auto iconSize = wxSize(iconSide, iconSide);
  const auto color = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);

  if (backBtn_) backBtn_->SetBitmap(MakeNavIconBundle(NavIcon::Back, iconSize, color));
  if (forwardBtn_) forwardBtn_->SetBitmap(MakeNavIconBundle(NavIcon::Forward, iconSize, color));
  if (upBtn_) upBtn_->SetBitmap(MakeNavIconBundle(NavIcon::Up, iconSize, color));
  if (refreshBtn_) refreshBtn_->SetBitmap(MakeNavIconBundle(NavIcon::Refresh, iconSize, color));
  if (homeBtn_) homeBtn_->SetBitmap(MakeNavIconBundle(NavIcon::Home, iconSize, color));
}

void FilePanel::BindFocusEvents(std::function<void()> onFocus) { onFocus_ = std::move(onFocus); }

void FilePanel::BindDirContentsChanged(
    std::function<void(const fs::path& dir, bool treeChanged)> onChanged) {
  onDirContentsChanged_ = std::move(onChanged);
}

void FilePanel::SetDirectory(const std::string& path) {
  history_.clear();
  historyIndex_ = -1;
  NavigateTo(fs::path(path), /*recordHistory=*/true);
}

fs::path FilePanel::GetDirectoryPath() const { return currentDir_; }

void FilePanel::RefreshListing() {
  if (currentDir_.empty()) return;
  LoadDirectory(currentDir_);
  UpdateStatusText();
}

void FilePanel::RefreshAll() {
  // Avoid full tree rebuild during normal ops; it resets selection/scroll.
  RefreshListing();
  SyncTreeToCurrentDir();
  UpdateStatusText();
}

void FilePanel::RefreshTree() {
  if (currentDir_.empty()) return;
  if (!tree_) return;
  // Rebuild device list (mounts may have changed) and ensure current path is visible.
  BuildComputerTree();
  SyncTreeToCurrentDir();
}

void FilePanel::NavigateUp() {
  if (currentDir_.empty()) return;
  if (listingMode_ == ListingMode::Recent) {
    GoHome();
    return;
  }
  if (listingMode_ == ListingMode::Gio) {
    const std::string uri = currentDir_.string();
    const auto schemePos = uri.find("://");
    if (schemePos == std::string::npos) return;

    std::string s = uri;
    while (s.size() > schemePos + 3 && s.back() == '/') s.pop_back();
    const auto slash = s.rfind('/');
    if (slash == std::string::npos || slash < schemePos + 3) return;
    const auto parent = s.substr(0, slash);
    if (parent.empty() || parent == uri) return;
    NavigateTo(fs::path(parent), /*recordHistory=*/true);
    return;
  }
  auto parent = currentDir_.parent_path();
  if (parent.empty()) parent = currentDir_; // likely at root
  NavigateTo(parent, /*recordHistory=*/true);
}

void FilePanel::FocusPrimary() {
  if (lastFocus_ == LastFocus::Tree) {
    if (tree_) tree_->SetFocus();
    return;
  }
  if (list_) list_->SetFocus();
}

void FilePanel::SetActiveVisual(bool isActive) {
  isActive_ = isActive;
  UpdateActiveVisuals();
}

void FilePanel::UpdateActiveVisuals() {
  // Keep it neutral: subtle header tint when active.
  if (!pathCtrl_) return;

  if (isActive_) {
    const auto base = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    const auto accent = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);

    // Blend accent into base to create a subtle focus cue that works in both
    // light and dark themes.
    const auto blend = [](const wxColour& a, const wxColour& b, double t) -> wxColour {
      auto lerp = [t](unsigned char x, unsigned char y) -> unsigned char {
        const double v = (1.0 - t) * x + t * y;
        if (v < 0.0) return 0;
        if (v > 255.0) return 255;
        return static_cast<unsigned char>(v + 0.5);
      };
      return wxColour(lerp(a.Red(), b.Red()),
                      lerp(a.Green(), b.Green()),
                      lerp(a.Blue(), b.Blue()));
    };

    const bool dark = wxSystemSettings::GetAppearance().IsDark();
    const double t = dark ? 0.18 : 0.12;
    pathCtrl_->SetBackgroundColour(blend(base, accent, t));
  } else {
    pathCtrl_->SetBackgroundColour(wxNullColour);
  }
  pathCtrl_->Refresh();
}

void FilePanel::NavigateToTextPath() {
  const auto text = pathCtrl_->GetValue().ToStdString();
  if (text.empty()) return;
  NavigateTo(fs::path(text), /*recordHistory=*/true);
}

void FilePanel::OpenSelectedIfDir(wxDataViewEvent& event) {
  renameArmedItem_ = wxDataViewItem();
  renameArmedAtMs_ = 0;
  const int row = list_->ItemToRow(event.GetItem());
  if (row == wxNOT_FOUND) return;

  wxVariant typeVar;
  list_->GetValue(typeVar, row, COL_TYPE);
  if (typeVar.GetString() != "Dir") return;

  wxVariant pathVar;
  list_->GetValue(pathVar, row, COL_FULLPATH);
  const auto p = pathVar.GetString().ToStdString();
  if (p.empty()) return;

  NavigateTo(fs::path(p), /*recordHistory=*/true);
}

void FilePanel::OnTreeSelectionChanged() {
  if (ignoreTreeEvent_) return;
  if (!tree_) return;
  const auto item = tree_->GetSelection();
  if (!item.IsOk()) return;
  auto* data = dynamic_cast<TreeNodeData*>(tree_->GetItemData(item));
  if (!data) return;
  if (data->kind == TreeNodeData::Kind::DevicesContainer) return;
  if (data->kind == TreeNodeData::Kind::NetworkContainer) {
    // Selecting the group toggles expand/collapse only.
    if (tree_->IsExpanded(item)) tree_->Collapse(item);
    else tree_->Expand(item);
    return;
  }
  if (data->path.empty()) return;
  NavigateTo(data->path, /*recordHistory=*/true);
}

void FilePanel::OnTreeItemExpanding(wxTreeEvent& event) {
  if (!tree_) return;
  const auto item = event.GetItem();
  if (!item.IsOk()) return;

  auto* data = dynamic_cast<TreeNodeData*>(tree_->GetItemData(item));
  if (!data) return;

  // Devices container refreshes its children on expand.
  if (data->kind == TreeNodeData::Kind::DevicesContainer) {
    PopulateDevices(item);
    return;
  }
  if (data->kind == TreeNodeData::Kind::NetworkContainer) {
    PopulateNetwork(item);
    return;
  }

  // Lazy-load directory children if we only have a dummy placeholder.
  if (!data->path.empty()) {
    wxTreeItemIdValue ck;
    const auto firstChild = tree_->GetFirstChild(item, ck);
    if (firstChild.IsOk() && tree_->GetItemData(firstChild) == nullptr) {
      PopulateDirChildren(item, data->path);
    }
  }
}

void FilePanel::OpenSelection() {
  if (!list_) return;
  int row = list_->GetSelectedRow();
  if (row == wxNOT_FOUND) {
    const auto current = list_->GetCurrentItem();
    row = list_->ItemToRow(current);
  }
  if (row == wxNOT_FOUND) return;

  wxVariant typeVar;
  list_->GetValue(typeVar, row, COL_TYPE);

  wxVariant pathVar;
  list_->GetValue(pathVar, row, COL_FULLPATH);
  const auto p = pathVar.GetString().ToStdString();
  if (p.empty()) return;

  const auto path = fs::path(p);
  if (typeVar.GetString() == "Dir") {
    NavigateTo(path, /*recordHistory=*/true);
    return;
  }

  wxLaunchDefaultApplication(path.string());
}

bool FilePanel::LoadDirectory(const fs::path& dir) {
  // Virtual: Recent
  if (dir == kVirtualRecent || dir.string() == "Recent") {
    listingMode_ = ListingMode::Recent;
    currentDir_ = kVirtualRecent;
    if (pathCtrl_) pathCtrl_->ChangeValue("Recent");

    std::vector<Entry> entries;
    const auto recent = ReadRecentPaths(/*limit=*/200);
    entries.reserve(recent.size());
    for (const auto& p : recent) {
      std::error_code ec;
      const bool isDir = fs::is_directory(p, ec) && !ec;
      std::uintmax_t size = 0;
      if (!isDir) {
        size = fs::file_size(p, ec);
        if (ec) size = 0;
      }
      std::string modified;
      ec.clear();
      const auto ft = fs::last_write_time(p, ec);
      if (!ec) modified = FormatFileTime(ft);
      entries.push_back(Entry{
          .name = p.filename().string(),
          .isDir = isDir,
          .size = size,
          .modified = modified,
          .fullPath = p.string(),
      });
    }

    SortEntries(entries);
    Populate(entries);
    UpdateSortIndicators();
    UpdateStatusText();
    UpdateNavButtons();
    return true;
  }

  // Remote / virtual locations via gio (smb://, sftp://, network://, etc.)
  {
    const std::string dirStr = dir.string();
    if (LooksLikeUri(dirStr)) {
      const auto scheme = UriScheme(dirStr);
      if (scheme == "file") {
        // Treat file:// as a normal local path when possible.
        const auto p = UriToPath(dirStr);
        if (p) return LoadDirectory(*p);
      } else if (IsGioLocationUri(dirStr)) {
        // network:// hosts often appear as network:///HOST. On many systems the
        // listable/browsable URI is smb://HOST/ instead, so we fall back.
        std::string effectiveUri = dirStr;
        if (scheme == "network" && dirStr != "network://") {
          const auto last = UriLastSegment(dirStr);
          if (!last.empty()) {
            std::string host, proto;
            if (ParseDnssdService(last, &host, &proto)) {
              std::string mappedScheme = proto;
              if (proto == "afpovertcp") mappedScheme = "afp";
              effectiveUri = mappedScheme + "://" + host + "/";
            } else {
              effectiveUri = "smb://" + last + "/";
            }
          }
        }

        if (scheme == "smb" && IsBareSchemeUri(dirStr)) {
          wxTextEntryDialog dlg(this,
                                "Enter an SMB URI (example: smb://server/share):",
                                "Connect to Windows Share");
          dlg.SetValue("smb://");
          if (dlg.ShowModal() != wxID_OK) return false;
          const auto uri = dlg.GetValue().ToStdString();
          if (uri.empty() || uri == dirStr) return false;
          return LoadDirectory(fs::path(uri));
        }

        listingMode_ = ListingMode::Gio;
        currentDir_ = fs::path(effectiveUri);
        if (pathCtrl_) pathCtrl_->ChangeValue(currentDir_.string());

        // Best-effort tree sync: highlight Network group.
        SyncTreeToCurrentDir();

        std::vector<std::string> selectedKeys;
        std::optional<std::string> currentKey;
        if (list_) {
          wxDataViewItemArray items;
          list_->GetSelections(items);
          selectedKeys.reserve(items.size());
          for (const auto& item : items) {
            const int row = list_->ItemToRow(item);
            if (row == wxNOT_FOUND) continue;
            wxVariant v;
            list_->GetValue(v, static_cast<unsigned int>(row), COL_FULLPATH);
            const auto key = v.GetString().ToStdString();
            if (!key.empty()) selectedKeys.push_back(key);
          }

          const auto curItem = list_->GetCurrentItem();
          const int curRow = list_->ItemToRow(curItem);
          if (curRow != wxNOT_FOUND) {
            wxVariant v;
            list_->GetValue(v, static_cast<unsigned int>(curRow), COL_FULLPATH);
            const auto key = v.GetString().ToStdString();
            if (!key.empty()) currentKey = key;
          }
        }

        std::string err;
        auto entries = ListGioLocation(effectiveUri, &err, this);
        if (!err.empty()) {
          const bool maybeNeedsMount =
              scheme == "network" || scheme == "smb" ||
              err.find("not mounted") != std::string::npos ||
              TrimRight(err).empty();

          if (maybeNeedsMount) {
            std::string mountErr;
            if (GioMountLocation(effectiveUri, &mountErr, this)) {
              err.clear();
              entries = ListGioLocation(effectiveUri, &err, this);
            } else if (!mountErr.empty()) {
              err = err.empty() ? mountErr : (err + "\n\nMount attempt: " + mountErr);
            }
          }
        }

        if (!err.empty()) {
          wxString help;
          if (err.find("Operation not supported") != std::string::npos) {
            help = "\n\nThis usually means the GIO/GVfs backend for this scheme isn't available on your system.";
          } else if (err.find("not mounted") != std::string::npos) {
            help = "\n\nTry a full URI like smb://server/share (not just smb://).";
          }
          const wxString dirWx = wxString::FromUTF8(effectiveUri);
          const wxString errWx = wxString::FromUTF8(err);
          wxMessageBox(wxString::Format("Unable to list location:\n\n%s\n\n%s%s",
                                        dirWx.c_str(),
                                        errWx.c_str(),
                                        help.c_str()),
                       "Quarry", wxOK | wxICON_ERROR, this);
          return false;
        }

        if ((UriScheme(effectiveUri) == "smb" || UriScheme(effectiveUri) == "afp" ||
             UriScheme(effectiveUri) == "sftp") &&
            AddRecentHost(effectiveUri) &&
            networkRoot_.IsOk()) {
          PopulateNetwork(networkRoot_);
          // Now that the host exists in the sidebar, sync selection to it (instead of the group header).
          SyncTreeToCurrentDir();
        }

        if (UriScheme(effectiveUri) == "network") {
          for (auto& e : entries) {
            const auto src = !e.fullPath.empty() ? e.fullPath : e.name;
            e.name = PrettyNetworkLabel(src);
          }
        }

        SortEntries(entries);
        Populate(entries);
        UpdateSortIndicators();
        ReselectAndReveal(selectedKeys, currentKey);
        UpdateStatusText();
        UpdateNavButtons();
        return true;
      }
    }
  }

  listingMode_ = ListingMode::Directory;

  std::vector<std::string> selectedKeys;
  std::optional<std::string> currentKey;
  if (list_) {
    wxDataViewItemArray items;
    list_->GetSelections(items);
    selectedKeys.reserve(items.size());
    for (const auto& item : items) {
      const int row = list_->ItemToRow(item);
      if (row == wxNOT_FOUND) continue;
      wxVariant v;
      list_->GetValue(v, static_cast<unsigned int>(row), COL_FULLPATH);
      const auto key = v.GetString().ToStdString();
      if (!key.empty()) selectedKeys.push_back(key);
    }

    const auto curItem = list_->GetCurrentItem();
    const int curRow = list_->ItemToRow(curItem);
    if (curRow != wxNOT_FOUND) {
      wxVariant v;
      list_->GetValue(v, static_cast<unsigned int>(curRow), COL_FULLPATH);
      const auto key = v.GetString().ToStdString();
      if (!key.empty()) currentKey = key;
    }
  }

  std::error_code ec;
  const auto canonical = fs::weakly_canonical(dir, ec);
  const auto resolved = ec ? dir : canonical;

  if (!fs::exists(resolved, ec) || !fs::is_directory(resolved, ec)) {
    const wxString resolvedWx = wxString::FromUTF8(resolved.string());
    wxMessageBox(wxString::Format("Not a directory:\n\n%s", resolvedWx.c_str()), "Quarry",
                 wxOK | wxICON_WARNING, this);
    return false;
  }

  currentDir_ = resolved;
  if (pathCtrl_) pathCtrl_->ChangeValue(currentDir_.string());
  SyncTreeToCurrentDir();

  std::string err;
  auto entries = ListDir(currentDir_, &err);
  if (!err.empty()) {
    const wxString dirWx = wxString::FromUTF8(currentDir_.string());
    const wxString errWx = wxString::FromUTF8(err);
    wxMessageBox(wxString::Format("Unable to list directory:\n\n%s\n\n%s",
                                  dirWx.c_str(),
                                  errWx.c_str()),
                 "Quarry", wxOK | wxICON_ERROR, this);
  }
  SortEntries(entries);
  Populate(entries);
  UpdateSortIndicators();
  ReselectAndReveal(selectedKeys, currentKey);
  UpdateStatusText();
  UpdateNavButtons();
  return true;
}

void FilePanel::NavigateTo(const fs::path& dir, bool recordHistory) {
  if (!LoadDirectory(dir)) return;
  if (recordHistory) PushHistory(currentDir_);
  UpdateNavButtons();
}

void FilePanel::ResetHistory(const fs::path& dir) {
  history_.clear();
  historyIndex_ = -1;
  if (!dir.empty()) PushHistory(dir);
  UpdateNavButtons();
}

void FilePanel::PushHistory(const fs::path& dir) {
  if (dir.empty()) return;
  if (historyIndex_ >= 0 && historyIndex_ < static_cast<int>(history_.size()) &&
      history_[historyIndex_] == dir) {
    return;
  }
  if (historyIndex_ + 1 < static_cast<int>(history_.size())) {
    history_.erase(history_.begin() + (historyIndex_ + 1), history_.end());
  }
  history_.push_back(dir);
  historyIndex_ = static_cast<int>(history_.size()) - 1;
}

void FilePanel::GoBack() {
  if (historyIndex_ <= 0) return;
  historyIndex_--;
  LoadDirectory(history_[historyIndex_]);
  UpdateNavButtons();
}

void FilePanel::GoForward() {
  if (historyIndex_ < 0) return;
  if (historyIndex_ + 1 >= static_cast<int>(history_.size())) return;
  historyIndex_++;
  LoadDirectory(history_[historyIndex_]);
  UpdateNavButtons();
}

void FilePanel::GoHome() {
  NavigateTo(fs::path(wxGetHomeDir().ToStdString()), /*recordHistory=*/true);
}

void FilePanel::UpdateNavButtons() {
  const bool canBack = historyIndex_ > 0;
  const bool canForward = historyIndex_ >= 0 && (historyIndex_ + 1) < static_cast<int>(history_.size());
  if (backBtn_) backBtn_->Enable(canBack);
  if (forwardBtn_) forwardBtn_->Enable(canForward);
}

void FilePanel::ShowProperties() {
  const auto selected = GetSelectedPaths();
  if (selected.empty()) return;
  if (selected.size() > 1) {
    wxMessageBox(wxString::Format("%zu items selected.", selected.size()),
                 "Properties", wxOK | wxICON_INFORMATION, this);
    return;
  }

  const auto path = selected[0];
  std::error_code ec;
  const bool isDir = fs::is_directory(path, ec);
  const auto size = (!ec && !isDir) ? fs::file_size(path, ec) : 0;
  const auto type = isDir ? "Directory" : "File";

  wxString msg;
  msg << "Name: " << path.filename().string() << "\n"
      << "Type: " << type << "\n"
      << "Path: " << path.string() << "\n";
  if (!isDir) {
    msg << "Size: " << HumanSize(size) << "\n";
  }

  wxMessageBox(msg, "Properties", wxOK | wxICON_INFORMATION, this);
}

void FilePanel::OnListValueChanged(wxDataViewEvent& event) {
  if (event.GetColumn() != COL_NAME) return;
  if (listingMode_ != ListingMode::Directory) {
    // Disallow rename in virtual views.
    const int row = list_->ItemToRow(event.GetItem());
    if (row != wxNOT_FOUND && row >= 0 && static_cast<size_t>(row) < currentEntries_.size()) {
      const auto& e = currentEntries_[static_cast<size_t>(row)];
      SetRowName(list_, static_cast<unsigned int>(row), e.name, e.isDir);
    }
    return;
  }

  const int row = list_->ItemToRow(event.GetItem());
  if (row == wxNOT_FOUND) return;
  if (row < 0 || static_cast<size_t>(row) >= currentEntries_.size()) return;

  const auto newName = GetRowName(list_, static_cast<unsigned int>(row));
  const auto oldName = currentEntries_[row].name;
  const bool renamedDir = currentEntries_[row].isDir;

  if (newName == oldName) return;

  if (newName.empty() || newName.find('/') != std::string::npos) {
    wxMessageBox("Invalid name.", "Rename", wxOK | wxICON_WARNING, this);
    SetRowName(list_, static_cast<unsigned int>(row), oldName, renamedDir);
    return;
  }

  std::error_code ec;
  fs::rename(currentDir_ / oldName, currentDir_ / newName, ec);
  if (ec) {
    const wxString errWx = wxString::FromUTF8(ec.message());
    wxMessageBox(wxString::Format("Rename failed:\n\n%s", errWx.c_str()), "Rename",
                 wxOK | wxICON_ERROR, this);
    SetRowName(list_, static_cast<unsigned int>(row), oldName, renamedDir);
    return;
  }

  RefreshAll();
  if (renamedDir) RefreshTree();
  NotifyDirContentsChanged(renamedDir);
}

void FilePanel::SortEntries(std::vector<Entry>& entries) const {
  const auto icase = [](std::string s) -> std::string {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };

  const auto isHidden = [](const Entry& e) -> bool {
    return !e.name.empty() && e.name[0] == '.';
  };

  const auto groupRank = [&](const Entry& e) -> int {
    // Desired order (ascending):
    //   folders, hidden folders, files, hidden files
    // Reversed order (descending) reverses the groups as well.
    const bool hidden = isHidden(e);
    int rank = 0;
    if (e.isDir && !hidden) rank = 0;
    else if (e.isDir && hidden) rank = 1;
    else if (!e.isDir && !hidden) rank = 2;
    else rank = 3; // hidden file

    return sortAscending_ ? rank : (3 - rank);
  };

  const auto cmp = [&](const Entry& a, const Entry& b) -> bool {
    const int ga = groupRank(a);
    const int gb = groupRank(b);
    if (ga != gb) return ga < gb;

    int rel = 0;
    switch (sortColumn_) {
      case SortColumn::Name: {
        const auto an = icase(a.name);
        const auto bn = icase(b.name);
        rel = (an < bn) ? -1 : (an > bn ? 1 : 0);
        break;
      }
      case SortColumn::Type: {
        const auto at = a.isDir ? "dir" : "file";
        const auto bt = b.isDir ? "dir" : "file";
        rel = (at < bt) ? -1 : (at > bt ? 1 : 0);
        break;
      }
      case SortColumn::Size: {
        if (a.size < b.size) rel = -1;
        else if (a.size > b.size) rel = 1;
        else rel = 0;
        break;
      }
      case SortColumn::Modified: {
        rel = (a.modified < b.modified) ? -1 : (a.modified > b.modified ? 1 : 0);
        break;
      }
    }

    // Within-group sort direction.
    if (!sortAscending_) rel = -rel;
    if (rel != 0) return rel < 0;
    // tie-breaker
    return icase(a.name) < icase(b.name);
  };

  std::stable_sort(entries.begin(), entries.end(), cmp);
}

void FilePanel::UpdateSortIndicators() {
  if (!list_) return;

  const auto setCol = [&](int pos, bool on) {
    wxDataViewColumn* col = list_->GetColumn(static_cast<unsigned int>(pos));
    if (!col) return;
    if (!on) {
      col->UnsetAsSortKey();
      return;
    }
    col->SetSortOrder(sortAscending_);
  };

  setCol(COL_NAME, sortColumn_ == SortColumn::Name);
  setCol(COL_TYPE, sortColumn_ == SortColumn::Type);
  setCol(COL_SIZE, sortColumn_ == SortColumn::Size);
  setCol(COL_MOD, sortColumn_ == SortColumn::Modified);
}

void FilePanel::ResortListing() {
  if (!list_) return;
  if (currentEntries_.empty()) {
    UpdateSortIndicators();
    return;
  }

  std::vector<std::string> selectedKeys;
  std::optional<std::string> currentKey;

  wxDataViewItemArray items;
  list_->GetSelections(items);
  selectedKeys.reserve(items.size());
  for (const auto& item : items) {
    const int row = list_->ItemToRow(item);
    if (row == wxNOT_FOUND) continue;
    wxVariant v;
    list_->GetValue(v, static_cast<unsigned int>(row), COL_FULLPATH);
    const auto key = v.GetString().ToStdString();
    if (!key.empty()) selectedKeys.push_back(key);
  }

  const auto curItem = list_->GetCurrentItem();
  const int curRow = list_->ItemToRow(curItem);
  if (curRow != wxNOT_FOUND) {
    wxVariant v;
    list_->GetValue(v, static_cast<unsigned int>(curRow), COL_FULLPATH);
    const auto key = v.GetString().ToStdString();
    if (!key.empty()) currentKey = key;
  }

  auto entries = currentEntries_;
  SortEntries(entries);
  Populate(entries);
  UpdateSortIndicators();
  ReselectAndReveal(selectedKeys, currentKey);
  UpdateStatusText();
}

void FilePanel::UpdateStatusText() {
  if (!statusText_) return;

  const size_t total = currentEntries_.size();
  size_t totalDirs = 0;
  for (const auto& e : currentEntries_) {
    if (e.isDir) totalDirs++;
  }
  const size_t totalFiles = total - totalDirs;

  size_t selectedCount = 0;
  size_t selectedDirs = 0;
  std::uintmax_t selectedBytes = 0;

  if (list_) {
    wxDataViewItemArray items;
    list_->GetSelections(items);
    selectedCount = items.size();

    for (const auto& item : items) {
      const int row = list_->ItemToRow(item);
      if (row == wxNOT_FOUND) continue;
      if (row < 0 || static_cast<size_t>(row) >= currentEntries_.size()) continue;
      const auto& e = currentEntries_[static_cast<size_t>(row)];
      if (e.isDir) {
        selectedDirs++;
      } else {
        selectedBytes += e.size;
      }
    }
  }

  const auto selectedFiles = selectedCount - selectedDirs;
  std::string freeText = "n/a";
  if (listingMode_ == ListingMode::Directory && !currentDir_.empty()) {
    std::error_code ec;
    const auto space = fs::space(currentDir_, ec);
    if (!ec) {
      freeText = HumanSize(space.available) + " free of " + HumanSize(space.capacity);
    }
  }

  wxString modeLabel = "Folder";
  if (listingMode_ == ListingMode::Recent) {
    modeLabel = "Recent";
  } else if (listingMode_ == ListingMode::Gio) {
    const auto s = currentDir_.string();
    modeLabel = s.rfind("network://", 0) == 0 ? "Network" : "Remote";
  }

  const wxString selectedSizeWx = wxString::FromUTF8(HumanSize(selectedBytes));
  const wxString freeWx = wxString::FromUTF8(freeText);
  const auto label = wxString::Format(
      "%s   Items: %zu (%zu dirs, %zu files)   Selected: %zu (%zu dirs, %zu files)   Selected size: %s   Free: %s",
      modeLabel.c_str(),
      total, totalDirs, totalFiles,
      selectedCount, selectedDirs, selectedFiles,
      selectedSizeWx.c_str(),
      freeWx.c_str());
  statusText_->SetLabel(label);
}

void FilePanel::SetStatus(const wxString&) {
  // Placeholder: MainFrame owns status bar; we keep per-panel quiet for now.
}

void FilePanel::Populate(const std::vector<Entry>& entries) {
  currentEntries_ = entries;
  if (!list_) return;
  list_->Freeze();
  list_->DeleteAllItems();

  for (const auto& e : entries) {
    wxVector<wxVariant> cols;
    const auto artId = e.isDir ? wxART_FOLDER : wxART_NORMAL_FILE;
    const auto bundle = wxArtProvider::GetBitmapBundle(artId, wxART_OTHER, wxSize(16, 16));
    wxDataViewIconText iconText(wxString::FromUTF8(e.name), bundle);
    wxVariant nameVar;
    nameVar << iconText;
    cols.push_back(nameVar);
    cols.push_back(wxVariant(e.isDir ? "Dir" : "File"));
    cols.push_back(wxVariant(e.isDir ? "" : HumanSize(e.size)));
    cols.push_back(wxVariant(e.modified));
    const auto full = !e.fullPath.empty() ? e.fullPath : (currentDir_ / e.name).string();
    cols.push_back(wxVariant(full));
    list_->AppendItem(cols);
  }
  list_->Thaw();
}

void FilePanel::ReselectAndReveal(const std::vector<std::string>& selectedKeys,
                                 const std::optional<std::string>& currentKey) {
  if (!list_) return;
  if (currentEntries_.empty()) return;

  std::unordered_map<std::string, int> keyToRow;
  keyToRow.reserve(currentEntries_.size());
  for (size_t i = 0; i < currentEntries_.size(); i++) {
    const auto full = !currentEntries_[i].fullPath.empty()
                          ? currentEntries_[i].fullPath
                          : (currentDir_ / currentEntries_[i].name).string();
    keyToRow.emplace(full, static_cast<int>(i));
  }

  list_->Freeze();
  list_->UnselectAll();

  wxDataViewItem revealItem;

  for (const auto& key : selectedKeys) {
    const auto it = keyToRow.find(key);
    if (it == keyToRow.end()) continue;
    const auto item = list_->RowToItem(it->second);
    if (!item.IsOk()) continue;
    list_->Select(item);
    if (!revealItem.IsOk()) revealItem = item;
  }

  if (!revealItem.IsOk() && currentKey) {
    const auto it = keyToRow.find(*currentKey);
    if (it != keyToRow.end()) revealItem = list_->RowToItem(it->second);
  }

  if (revealItem.IsOk()) {
    list_->SetCurrentItem(revealItem);
    list_->EnsureVisible(revealItem, list_->GetColumn(COL_NAME));
  }

  list_->Thaw();
}

std::optional<fs::path> FilePanel::GetSelectedPath() const {
  const int row = list_->GetSelectedRow();
  if (row == wxNOT_FOUND) return std::nullopt;

  wxVariant pathVar;
  list_->GetValue(pathVar, static_cast<unsigned int>(row), COL_FULLPATH);
  const auto p = pathVar.GetString().ToStdString();
  if (p.empty()) return std::nullopt;
  return fs::path(p);
}

std::vector<fs::path> FilePanel::GetSelectedPaths() const {
  wxDataViewItemArray items;
  list_->GetSelections(items);
  if (items.empty()) return {};

  std::vector<std::pair<int, fs::path>> rows;
  rows.reserve(items.size());
  for (const auto& item : items) {
    const int row = list_->ItemToRow(item);
    if (row == wxNOT_FOUND) continue;
    wxVariant pathVar;
    list_->GetValue(pathVar, static_cast<unsigned int>(row), COL_FULLPATH);
    const auto p = pathVar.GetString().ToStdString();
    if (p.empty()) continue;
    rows.emplace_back(row, fs::path(p));
  }

  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

  std::vector<fs::path> paths;
  paths.reserve(rows.size());
  for (const auto& [_, p] : rows) paths.push_back(p);
  return paths;
}

void FilePanel::BeginInlineRename() {
  if (listingMode_ != ListingMode::Directory) return;
  wxDataViewItemArray items;
  list_->GetSelections(items);
  if (items.size() != 1) return;
  allowInlineEdit_ = true;
  list_->EditItem(items[0], list_->GetColumn(COL_NAME));
}

void FilePanel::CreateFolder() {
  if (listingMode_ != ListingMode::Directory) {
    wxMessageBox("Create Folder is not available here.", "Quarry", wxOK | wxICON_INFORMATION, this);
    return;
  }
  const auto name = wxGetTextFromUser("Folder name:", "Create Folder", "", this).ToStdString();
  if (name.empty()) return;
  if (name.find('/') != std::string::npos) {
    wxMessageBox("Invalid folder name.", "Create Folder", wxOK | wxICON_WARNING, this);
    return;
  }

  std::error_code ec;
  fs::create_directory(currentDir_ / name, ec);
  if (ec) {
    const wxString errWx = wxString::FromUTF8(ec.message());
    wxMessageBox(wxString::Format("Create folder failed:\n\n%s", errWx.c_str()), "Create Folder",
                 wxOK | wxICON_ERROR, this);
    return;
  }
  RefreshAll();
  RefreshTree();
  NotifyDirContentsChanged(true);
}

void FilePanel::CopySelection() {
  const auto paths = GetSelectedPaths();
  if (paths.empty()) return;
  g_clipboard = AppClipboard{.mode = AppClipboard::Mode::Copy, .paths = paths};
}

void FilePanel::CutSelection() {
  const auto paths = GetSelectedPaths();
  if (paths.empty()) return;
  g_clipboard = AppClipboard{.mode = AppClipboard::Mode::Cut, .paths = paths};
}

void FilePanel::PasteIntoCurrentDir() {
  if (!g_clipboard || g_clipboard->paths.empty()) return;
  if (listingMode_ != ListingMode::Directory) {
    wxMessageBox("Paste is not available here.", "Quarry", wxOK | wxICON_INFORMATION, this);
    return;
  }
  if (currentDir_.empty()) return;

  const bool isMove = g_clipboard->mode == AppClipboard::Mode::Cut;
  const auto title = isMove ? "Paste (Move)" : "Paste (Copy)";

  wxProgressDialog progress(title,
                            "Preparing...",
                            static_cast<int>(g_clipboard->paths.size()),
                            this,
                            wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);

  bool cancelAll = false;
  for (size_t i = 0; i < g_clipboard->paths.size(); i++) {
    const auto& src = g_clipboard->paths[i];
    if (!fs::exists(src)) continue;

    auto dst = currentDir_ / src.filename();
    if (!progress.Update(static_cast<int>(i), src.filename().string())) break;

    bool skipItem = false;
    for (;;) {
      std::error_code existsEc;
      if (!fs::exists(dst, existsEc)) break;

      const auto choice = PromptExists(this, dst);
      if (choice == ExistsChoice::Skip) {
        skipItem = true;
        break;
      }
      if (choice == ExistsChoice::Cancel) {
        cancelAll = true;
        break;
      }
      if (choice == ExistsChoice::Rename) {
        wxTextEntryDialog nameDlg(this, "New name:", "Rename", dst.filename().string());
        if (nameDlg.ShowModal() != wxID_OK) {
          cancelAll = true;
          break;
        }
        dst = currentDir_ / nameDlg.GetValue().ToStdString();
        continue;
      }
      break; // Overwrite
    }

    if (cancelAll) break;
    if (skipItem) continue;

    const auto result = isMove ? MovePath(src, dst) : CopyPathRecursive(src, dst);
    if (!result.ok) {
      const wxString action = isMove ? "Move" : "Copy";
      wxMessageDialog dlg(this,
                          wxString::Format("%s failed:\n\n%s\n\nContinue?",
                                           action.c_str(),
                                           result.message.c_str()),
                          title,
                          wxYES_NO | wxNO_DEFAULT | wxICON_ERROR);
      dlg.SetYesNoLabels("Continue", "Cancel");
      if (dlg.ShowModal() != wxID_YES) break;
    }
  }

  // If we cut, clear clipboard after paste attempt (common file manager behavior).
  if (isMove) g_clipboard.reset();

  const bool treeChanged = true; // files or dirs might have been added/removed
  RefreshAll();
  RefreshTree();
  NotifyDirContentsChanged(treeChanged);
}

void FilePanel::TrashSelection() {
  const auto paths = GetSelectedPaths();
  if (paths.empty()) return;

  const auto message = wxString::Format("Move %zu item(s) to Trash?", paths.size());
  if (wxMessageBox(message, "Trash", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) {
    return;
  }

  wxProgressDialog progress("Trashing",
                            "Preparing...",
                            static_cast<int>(paths.size()),
                            this,
                            wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);

  for (size_t i = 0; i < paths.size(); i++) {
    const auto& src = paths[i];
    if (!progress.Update(static_cast<int>(i), src.filename().string())) break;

    const auto result = TrashPath(src);
    if (!result.ok) {
      wxMessageDialog dlg(this,
                          wxString::Format("Trash failed:\n\n%s\n\nContinue?", result.message.c_str()),
                          "Trash failed",
                          wxYES_NO | wxNO_DEFAULT | wxICON_ERROR);
      dlg.SetYesNoLabels("Continue", "Cancel");
      if (dlg.ShowModal() != wxID_YES) break;
    }
  }

  const bool treeChanged = AnySelectedDirs();
  RefreshAll();
  if (treeChanged) RefreshTree();
  NotifyDirContentsChanged(treeChanged);
}

void FilePanel::DeleteSelectionPermanent() {
  const auto paths = GetSelectedPaths();
  if (paths.empty()) return;

  const auto message = wxString::Format(
      "Permanently delete %zu item(s)?\n\nThis cannot be undone.", paths.size());
  if (wxMessageBox(message, "Delete", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) {
    return;
  }

  wxProgressDialog progress("Deleting",
                            "Preparing...",
                            static_cast<int>(paths.size()),
                            this,
                            wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);

  for (size_t i = 0; i < paths.size(); i++) {
    const auto& src = paths[i];
    if (!progress.Update(static_cast<int>(i), src.filename().string())) break;

    const auto result = DeletePath(src);
    if (!result.ok) {
      wxMessageDialog dlg(this,
                          wxString::Format("Delete failed:\n\n%s\n\nContinue?", result.message.c_str()),
                          "Delete failed",
                          wxYES_NO | wxNO_DEFAULT | wxICON_ERROR);
      dlg.SetYesNoLabels("Continue", "Cancel");
      if (dlg.ShowModal() != wxID_YES) break;
    }
  }

  const bool treeChanged = AnySelectedDirs();
  RefreshAll();
  if (treeChanged) RefreshTree();
  NotifyDirContentsChanged(treeChanged);
}

void FilePanel::NotifyDirContentsChanged(bool treeChanged) {
  if (onDirContentsChanged_) onDirContentsChanged_(currentDir_, treeChanged);
}

void FilePanel::BuildComputerTree() {
  if (!tree_) return;

  tree_->Freeze();
  tree_->DeleteAllItems();
  hiddenRoot_ = wxTreeItemId();
  computerRoot_ = wxTreeItemId();
  homeRoot_ = wxTreeItemId();
  fsRoot_ = wxTreeItemId();
  devicesRoot_ = wxTreeItemId();
  networkRoot_ = wxTreeItemId();
  browseNetworkRoot_ = wxTreeItemId();
  desktopRoot_ = wxTreeItemId();
  documentsRoot_ = wxTreeItemId();
  downloadsRoot_ = wxTreeItemId();
  musicRoot_ = wxTreeItemId();
  picturesRoot_ = wxTreeItemId();
  videosRoot_ = wxTreeItemId();
  recentRoot_ = wxTreeItemId();
  trashRoot_ = wxTreeItemId();

  auto* images = new wxImageList(16, 16, true);
  images->Add(wxArtProvider::GetBitmap(wxART_FOLDER, wxART_OTHER, wxSize(16, 16)));    // Folder
  images->Add(wxArtProvider::GetBitmap(wxART_GO_HOME, wxART_OTHER, wxSize(16, 16)));  // Home
  images->Add(wxArtProvider::GetBitmap(wxART_HARDDISK, wxART_OTHER, wxSize(16, 16))); // Drive
  images->Add(wxArtProvider::GetBitmap(wxART_HARDDISK, wxART_OTHER, wxSize(16, 16))); // Computer
  tree_->AssignImageList(images);

  hiddenRoot_ = tree_->AddRoot("root");

  computerRoot_ = tree_->AppendItem(hiddenRoot_, "My Computer", static_cast<int>(TreeIcon::Computer));

  const fs::path homePath = wxGetHomeDir().ToStdString();
  homeRoot_ = tree_->AppendItem(computerRoot_, "Home", static_cast<int>(TreeIcon::Home),
                                -1, new TreeNodeData(homePath));

  const auto resolve = [](const char* key, const char* fallback) -> fs::path {
    if (const auto xdg = ReadXdgUserDir(key)) return *xdg;
    return DefaultUserDir(fallback);
  };

  desktopRoot_ = tree_->AppendItem(computerRoot_, "Desktop", static_cast<int>(TreeIcon::Folder),
                                   -1, new TreeNodeData(resolve("DESKTOP", "Desktop")));

  documentsRoot_ = tree_->AppendItem(computerRoot_, "Documents", static_cast<int>(TreeIcon::Folder),
                                     -1, new TreeNodeData(resolve("DOCUMENTS", "Documents")));

  musicRoot_ = tree_->AppendItem(computerRoot_, "Music", static_cast<int>(TreeIcon::Folder),
                                 -1, new TreeNodeData(resolve("MUSIC", "Music")));

  picturesRoot_ = tree_->AppendItem(computerRoot_, "Pictures", static_cast<int>(TreeIcon::Folder),
                                    -1, new TreeNodeData(resolve("PICTURES", "Pictures")));

  videosRoot_ = tree_->AppendItem(computerRoot_, "Videos", static_cast<int>(TreeIcon::Folder),
                                  -1, new TreeNodeData(resolve("VIDEOS", "Videos")));

  downloadsRoot_ = tree_->AppendItem(computerRoot_, "Downloads", static_cast<int>(TreeIcon::Folder),
                                     -1, new TreeNodeData(resolve("DOWNLOAD", "Downloads")));

  recentRoot_ = tree_->AppendItem(computerRoot_, "Recent", static_cast<int>(TreeIcon::Drive),
                                  -1, new TreeNodeData(kVirtualRecent));

  fsRoot_ = tree_->AppendItem(computerRoot_, "File System", static_cast<int>(TreeIcon::Drive),
                              -1, new TreeNodeData(fs::path("/")));

  const auto trashPath = TrashFilesDir();
  if (!trashPath.empty()) {
    std::error_code ec;
    fs::create_directories(trashPath, ec);
  }
  trashRoot_ = tree_->AppendItem(computerRoot_, "Trash", static_cast<int>(TreeIcon::Drive),
                                 -1, new TreeNodeData(trashPath));

  devicesRoot_ = tree_->AppendItem(hiddenRoot_, "Devices", static_cast<int>(TreeIcon::Drive),
                                   -1, new TreeNodeData(fs::path(), TreeNodeData::Kind::DevicesContainer));
  PopulateDevices(devicesRoot_);

  networkRoot_ = tree_->AppendItem(hiddenRoot_, "Network", static_cast<int>(TreeIcon::Drive),
                                   -1, new TreeNodeData(fs::path(), TreeNodeData::Kind::NetworkContainer));
  PopulateNetwork(networkRoot_);

  tree_->Expand(computerRoot_);
  tree_->Expand(devicesRoot_);
  tree_->Expand(networkRoot_);

  tree_->Thaw();
}

static std::string UnescapeProcMountsField(std::string s) {
  // /proc/mounts escapes spaces and tabs using octal sequences (e.g. \040).
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 3 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isdigit(static_cast<unsigned char>(s[i + 2])) &&
        std::isdigit(static_cast<unsigned char>(s[i + 3]))) {
      const int v = (s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0');
      out.push_back(static_cast<char>(v));
      i += 3;
      continue;
    }
    out.push_back(s[i]);
  }
  return out;
}

void FilePanel::PopulateDevices(const wxTreeItemId& devicesItem) {
  if (!tree_ || !devicesItem.IsOk()) return;
  tree_->DeleteChildren(devicesItem);

  std::ifstream f("/proc/mounts");
  if (!f.is_open()) return;

  std::set<std::string> mountpoints;
  std::string dev, mnt, type, opts;
  while (f >> dev >> mnt >> type >> opts) {
    std::string rest;
    std::getline(f, rest);
    const auto mp = UnescapeProcMountsField(mnt);
    if (mp.empty()) continue;
    mountpoints.insert(mp);
  }

  const auto isInteresting = [](const std::string& mp) -> bool {
    if (mp == "/") return false; // already represented by File System
    if (mp.rfind("/run/media/", 0) == 0) return true;
    if (mp.rfind("/media/", 0) == 0) return true;
    if (mp.rfind("/mnt/", 0) == 0) return true;
    return false;
  };

  for (const auto& mp : mountpoints) {
    if (!isInteresting(mp)) continue;
    fs::path p(mp);
    auto label = p.filename().string();
    if (label.empty()) label = mp;
    tree_->AppendItem(devicesItem,
                      wxString::FromUTF8(label),
                      static_cast<int>(TreeIcon::Drive),
                      -1,
                      new TreeNodeData(p));
  }

  if (tree_->GetChildrenCount(devicesItem, false) == 0) {
    tree_->AppendItem(devicesItem, "(none)");
  }
}

void FilePanel::PopulateNetwork(const wxTreeItemId& networkItem) {
  if (!tree_ || !networkItem.IsOk()) return;

  std::string selectedPath;
  const auto selected = tree_->GetSelection();
  if (selected.IsOk()) {
    if (auto* data = dynamic_cast<TreeNodeData*>(tree_->GetItemData(selected))) {
      if (data->kind == TreeNodeData::Kind::Path) selectedPath = data->path.string();
    }
  }

  tree_->Freeze();
  tree_->DeleteChildren(networkItem);

  browseNetworkRoot_ = tree_->AppendItem(networkItem,
                                        "Browse Network",
                                        static_cast<int>(TreeIcon::Drive),
                                        -1,
                                        new TreeNodeData(fs::path("network://")));

  const auto& hosts = GetRecentHosts();
  for (const auto& h : hosts) {
    if (h.key.empty()) continue;
    tree_->AppendItem(networkItem,
                      wxString::FromUTF8(h.display),
                      static_cast<int>(TreeIcon::Drive),
                      -1,
                      new TreeNodeData(fs::path(h.key)));
  }

  if (!selectedPath.empty()) {
    wxTreeItemIdValue ck;
    auto c = tree_->GetFirstChild(networkItem, ck);
    while (c.IsOk()) {
      auto* data = dynamic_cast<TreeNodeData*>(tree_->GetItemData(c));
      if (data && data->kind == TreeNodeData::Kind::Path && data->path.string() == selectedPath) {
        ignoreTreeEvent_ = true;
        tree_->SelectItem(c);
        tree_->EnsureVisible(c);
        ignoreTreeEvent_ = false;
        break;
      }
      c = tree_->GetNextChild(networkItem, ck);
    }
  }

  tree_->Thaw();
}

void FilePanel::PopulateDirChildren(const wxTreeItemId& parent, const fs::path& dir) {
  if (!tree_) return;
  std::error_code ec;
  if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return;

  tree_->DeleteChildren(parent);

  std::vector<fs::path> childDirs;
  for (const auto& de : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
    if (ec) break;
    std::error_code sec;
    if (!de.is_directory(sec) || sec) continue;
    childDirs.push_back(de.path());
    if (childDirs.size() >= 600) break;
  }

  std::sort(childDirs.begin(), childDirs.end(), [](const auto& a, const auto& b) {
    return a.filename().string() < b.filename().string();
  });

  for (const auto& p : childDirs) {
    const auto name = p.filename().string();
    if (name.empty()) continue;
    const auto item = tree_->AppendItem(parent,
                                        wxString::FromUTF8(name),
                                        static_cast<int>(TreeIcon::Folder),
                                        -1,
                                        new TreeNodeData(p));
    tree_->AppendItem(item, " "); // dummy
  }
}

wxTreeItemId FilePanel::EnsurePathSelected(const wxTreeItemId& baseItem,
                                          const fs::path& basePath,
                                          const fs::path& targetDir) {
  if (!tree_) return wxTreeItemId();
  if (!baseItem.IsOk()) return wxTreeItemId();
  if (targetDir.empty()) return baseItem;

  auto ensurePopulated = [&](const wxTreeItemId& item, const fs::path& dir) {
    wxTreeItemIdValue ck;
    const auto firstChild = tree_->GetFirstChild(item, ck);
    if (firstChild.IsOk() && tree_->GetItemData(firstChild) == nullptr) {
      PopulateDirChildren(item, dir);
    }
  };

  fs::path currentPath = basePath;
  wxTreeItemId currentItem = baseItem;
  tree_->Expand(currentItem);
  ensurePopulated(currentItem, currentPath);

  const fs::path rel = targetDir.lexically_relative(basePath);
  for (const auto& part : rel) {
    if (part.empty()) continue;
    currentPath /= part;

    ensurePopulated(currentItem, currentPath.parent_path());

    wxTreeItemIdValue ck;
    wxTreeItemId found;
    auto c = tree_->GetFirstChild(currentItem, ck);
    while (c.IsOk()) {
      auto* data = dynamic_cast<TreeNodeData*>(tree_->GetItemData(c));
      if (data && data->kind == TreeNodeData::Kind::Path && data->path == currentPath) {
        found = c;
        break;
      }
      c = tree_->GetNextChild(currentItem, ck);
    }
    if (!found.IsOk()) break;
    currentItem = found;
    tree_->Expand(currentItem);
  }

  return currentItem;
}

void FilePanel::SyncTreeToCurrentDir() {
  if (!tree_) return;
  if (currentDir_.empty()) return;
  if (!hiddenRoot_.IsOk()) return;

  // Virtual views.
  if (currentDir_ == kVirtualRecent) {
    if (recentRoot_.IsOk()) {
      ignoreTreeEvent_ = true;
      tree_->SelectItem(recentRoot_);
      tree_->EnsureVisible(recentRoot_);
      ignoreTreeEvent_ = false;
    }
    return;
  }

  if (listingMode_ == ListingMode::Gio) {
    if (networkRoot_.IsOk()) {
      const auto uri = currentDir_.string();
      wxTreeItemId best = networkRoot_;

      if (uri.rfind("network://", 0) == 0 && browseNetworkRoot_.IsOk()) {
        best = browseNetworkRoot_;
      } else if (auto root = HostRootForUri(uri)) {
        wxTreeItemIdValue ck;
        auto c = tree_->GetFirstChild(networkRoot_, ck);
        while (c.IsOk()) {
          if (auto* data = dynamic_cast<TreeNodeData*>(tree_->GetItemData(c))) {
            if (data->kind == TreeNodeData::Kind::Path && data->path.string() == *root) {
              best = c;
              break;
            }
          }
          c = tree_->GetNextChild(networkRoot_, ck);
        }
      }

      ignoreTreeEvent_ = true;
      tree_->SelectItem(best);
      tree_->EnsureVisible(best);
      ignoreTreeEvent_ = false;
    }
    return;
  }

  const auto isUnder = [](const fs::path& base, const fs::path& target) -> bool {
    if (base.empty() || target.empty()) return false;
    const auto b = base.lexically_normal().string();
    const auto t = target.lexically_normal().string();
    if (t == b) return true;
    if (t.size() > b.size() && t.rfind(b, 0) == 0 && t[b.size()] == '/') return true;
    return false;
  };

  const auto selectShortcutIfUnder = [&](const wxTreeItemId& shortcutItem) -> bool {
    if (!shortcutItem.IsOk()) return false;
    auto* data = dynamic_cast<TreeNodeData*>(tree_->GetItemData(shortcutItem));
    if (!data || data->path.empty()) return false;
    if (!isUnder(data->path, currentDir_)) return false;
    tree_->SelectItem(shortcutItem);
    tree_->EnsureVisible(shortcutItem);
    return true;
  };

  ignoreTreeEvent_ = true;
  if (selectShortcutIfUnder(desktopRoot_) ||
      selectShortcutIfUnder(documentsRoot_) ||
      selectShortcutIfUnder(downloadsRoot_) ||
      selectShortcutIfUnder(musicRoot_) ||
      selectShortcutIfUnder(picturesRoot_) ||
      selectShortcutIfUnder(videosRoot_) ||
      selectShortcutIfUnder(trashRoot_)) {
    ignoreTreeEvent_ = false;
    return;
  }
  ignoreTreeEvent_ = false;

  if (devicesRoot_.IsOk()) {
    wxTreeItemId best;
    size_t bestLen = 0;

    wxTreeItemIdValue ck;
    auto c = tree_->GetFirstChild(devicesRoot_, ck);
    while (c.IsOk()) {
      auto* data = dynamic_cast<TreeNodeData*>(tree_->GetItemData(c));
      if (data && data->kind == TreeNodeData::Kind::Path && !data->path.empty() && isUnder(data->path, currentDir_)) {
        const size_t len = data->path.string().size();
        if (!best.IsOk() || len > bestLen) {
          best = c;
          bestLen = len;
        }
      }
      c = tree_->GetNextChild(devicesRoot_, ck);
    }

    if (best.IsOk()) {
      ignoreTreeEvent_ = true;
      tree_->SelectItem(best);
      tree_->EnsureVisible(best);
      ignoreTreeEvent_ = false;
      return;
    }
  }

  const fs::path homePath = wxGetHomeDir().ToStdString();
  const auto dirStr = currentDir_.string();
  const auto homeStr = homePath.string();
  const bool inHome = !homeStr.empty() && (dirStr == homeStr ||
                                          (dirStr.rfind(homeStr, 0) == 0 &&
                                           (dirStr.size() == homeStr.size() ||
                                            dirStr[homeStr.size()] == '/')));

  ignoreTreeEvent_ = true;
  const auto item = inHome ? homeRoot_ : fsRoot_;
  if (item.IsOk()) {
    tree_->SelectItem(item);
    tree_->EnsureVisible(item);
  }
  ignoreTreeEvent_ = false;
}

void FilePanel::ShowListContextMenu(wxDataViewEvent& event) {
  if (!list_) return;

  // If we right-click a specific item, ensure it's part of the selection.
  if (event.GetItem().IsOk() && !list_->IsSelected(event.GetItem())) {
    list_->UnselectAll();
    list_->Select(event.GetItem());
    list_->SetCurrentItem(event.GetItem());
  }

  wxMenu menu;
  menu.Append(ID_CTX_OPEN, "Open");
  menu.AppendSeparator();
  menu.Append(ID_CTX_COPY, "Copy");
  menu.Append(ID_CTX_CUT, "Cut");
  menu.Append(ID_CTX_PASTE, "Paste");
  menu.AppendSeparator();
  menu.Append(ID_CTX_RENAME, "Rename");
  menu.Append(ID_CTX_NEW_FOLDER, "New Folder");
  menu.AppendSeparator();
  menu.Append(ID_CTX_TRASH, "Move to Trash");
  menu.Append(ID_CTX_DELETE_PERM, "Delete Permanently");
  menu.AppendSeparator();
  menu.Append(ID_CTX_PROPERTIES, "Properties");

  const auto selected = GetSelectedPaths();
  const bool hasSelection = !selected.empty();
  const bool allowFsOps = listingMode_ == ListingMode::Directory;
  menu.Enable(ID_CTX_OPEN, hasSelection);
  menu.Enable(ID_CTX_COPY, allowFsOps && hasSelection);
  menu.Enable(ID_CTX_CUT, allowFsOps && hasSelection);
  menu.Enable(ID_CTX_RENAME, allowFsOps && selected.size() == 1);
  menu.Enable(ID_CTX_NEW_FOLDER, allowFsOps);
  menu.Enable(ID_CTX_TRASH, allowFsOps && hasSelection);
  menu.Enable(ID_CTX_DELETE_PERM, allowFsOps && hasSelection);
  menu.Enable(ID_CTX_PASTE, allowFsOps && g_clipboard && !g_clipboard->paths.empty());

  menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { OpenSelection(); }, ID_CTX_OPEN);
  menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { CopySelection(); }, ID_CTX_COPY);
  menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { CutSelection(); }, ID_CTX_CUT);
  menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { PasteIntoCurrentDir(); }, ID_CTX_PASTE);
  menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { BeginInlineRename(); }, ID_CTX_RENAME);
  menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { CreateFolder(); }, ID_CTX_NEW_FOLDER);
  menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { TrashSelection(); }, ID_CTX_TRASH);
  menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { DeleteSelectionPermanent(); }, ID_CTX_DELETE_PERM);
  menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { ShowProperties(); }, ID_CTX_PROPERTIES);

  PopupMenu(&menu);
}

bool FilePanel::AnySelectedDirs() const {
  if (!list_) return false;
  wxDataViewItemArray items;
  list_->GetSelections(items);
  for (const auto& item : items) {
    const int row = list_->ItemToRow(item);
    if (row == wxNOT_FOUND) continue;
    if (row < 0 || static_cast<size_t>(row) >= currentEntries_.size()) continue;
    if (currentEntries_[static_cast<size_t>(row)].isDir) return true;
  }
  return false;
}
