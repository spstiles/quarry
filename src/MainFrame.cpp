#include "MainFrame.h"

#include "Connections.h"
#include "util.h"

#include <filesystem>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <limits>
#include <chrono>
#include <memory>
#include <algorithm>

#include <wx/accel.h>
#include <wx/aboutdlg.h>
#include <wx/choicdlg.h>
#include <wx/config.h>
#include <wx/display.h>
#include <wx/filefn.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/textdlg.h>
#include <wx/textctrl.h>
#include <wx/choice.h>
#include <wx/checkbox.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/gauge.h>
#include <wx/button.h>
#include <wx/listbox.h>
#include <wx/utils.h>
#include <wx/scrolwin.h>

namespace {
enum MenuId : int {
  ID_Refresh = wxID_HIGHEST + 1,
  ID_ConnectToServer,
  ID_ConnectionsManager,
  ID_Copy,
  ID_Move,
  ID_Trash,
  ID_DeletePermanent,
  ID_Rename,
  ID_MkDir,
  ID_SaveDefaultView,
  ID_LoadDefaultView
};

enum class ExistsChoice { Overwrite, Skip, Rename, Cancel };

ExistsChoice PromptExists(wxWindow* parent, const std::filesystem::path& dst) {
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

}  // namespace

struct AsyncFileOpPrompt {
  enum class Kind { Exists, Error, TrashFailed };
  Kind kind{Kind::Exists};
  std::filesystem::path src{};
  std::filesystem::path dst{};
  std::string errorMessage{};
};

enum class TrashFailChoice { DeletePermanent, Skip, Cancel };

struct AsyncFileOpReply {
  // For Kind::Exists
  ExistsChoice existsChoice{ExistsChoice::Cancel};
  std::optional<std::string> renameTo{};

  // For Kind::Error
  bool continueAfterError{false};

  // For Kind::TrashFailed
  TrashFailChoice trashFailChoice{TrashFailChoice::Cancel};
};

struct AsyncFileOpState {
  std::mutex mu{};
  std::condition_variable cv{};

  std::atomic<bool> cancelRequested{false};
  std::atomic<size_t> done{0};
  std::atomic<std::uintmax_t> bytesDone{0};

  bool finished{false};
  bool hasDir{false};
  bool scanDone{false};
  std::uintmax_t totalBytes{0};
  std::string currentLabel{};

  std::optional<AsyncFileOpPrompt> prompt{};
  std::optional<AsyncFileOpReply> reply{};
};

static bool LooksLikeUriPath(const std::filesystem::path& p) {
  const auto s = p.string();
  return s.find("://") != std::string::npos;
}

static std::uintmax_t EstimateTotalBytes(const std::vector<std::filesystem::path>& sources,
                                         const CancelFn& shouldCancel,
                                         const CopyProgressFn& onProgress) {
  namespace fs = std::filesystem;
  std::uintmax_t total = 0;
  std::error_code ec;

  for (const auto& src : sources) {
    if (shouldCancel && shouldCancel()) break;
    if (src.empty()) continue;

    ec.clear();
    const auto st = fs::symlink_status(src, ec);
    if (ec) continue;

    if (fs::is_directory(st)) {
      fs::recursive_directory_iterator it(src, fs::directory_options::skip_permission_denied, ec);
      if (ec) continue;
      for (const auto& entry : it) {
        if (shouldCancel && shouldCancel()) break;
        if (onProgress) onProgress(entry.path());

        ec.clear();
        const auto est = entry.symlink_status(ec);
        if (ec) continue;
        if (!fs::is_regular_file(est)) continue;

        ec.clear();
        const auto sz = fs::file_size(entry.path(), ec);
        if (ec) continue;
        total += sz;
      }
      continue;
    }

    if (fs::is_regular_file(st)) {
      ec.clear();
      const auto sz = fs::file_size(src, ec);
      if (ec) continue;
      total += sz;
    }
  }

  return total;
}

static wxString FormatHMS(std::chrono::seconds s) {
  const auto total = s.count();
  if (total < 0) return "--:--";
  const auto h = total / 3600;
  const auto m = (total % 3600) / 60;
  const auto sec = total % 60;
  if (h > 99) return "99:59:59";
  return wxString::Format("%02lld:%02lld:%02lld", (long long)h, (long long)m, (long long)sec);
}

struct MainFrame::FileOpSession final {
  wxDialog* dlg{nullptr};
  wxStaticText* titleText{nullptr};
  wxStaticText* detailText{nullptr};
  wxGauge* gauge{nullptr};
  wxButton* cancelBtn{nullptr};
  wxStaticBox* queueBox{nullptr};
  wxScrolledWindow* queueScroll{nullptr};
  wxBoxSizer* queueItemsSizer{nullptr};
  wxButton* clearQueueBtn{nullptr};
  wxTimer timer;
  int timerId{wxID_ANY};

  std::shared_ptr<AsyncFileOpState> state;
  std::thread worker;

  std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
  std::uintmax_t bytesUnit{1};
  int range{100};
  bool configured{false};
  bool promptActive{false};

  explicit FileOpSession(wxEvtHandler* owner) : timer(owner) {}
};

void MainFrame::UpdateQueueUi() {
  if (!fileOp_ || !fileOp_->dlg || !fileOp_->queueScroll || !fileOp_->queueItemsSizer) return;

  auto describe = [](const QueuedOp& op) -> wxString {
    switch (op.kind) {
      case OpKind::CopyMove: {
        const wxString verb = op.move ? "Move" : "Copy";
        const wxString dst = wxString::FromUTF8(op.dstDir.string());
        return wxString::Format("%s %zu item(s) → %s", verb.c_str(), op.sources.size(), dst.c_str());
      }
      case OpKind::Trash:
        return wxString::Format("Trash %zu item(s)", op.sources.size());
      case OpKind::Delete:
        return wxString::Format("Delete %zu item(s)", op.sources.size());
      case OpKind::Extract: {
        const wxString cmd = op.argv.empty() ? "extract" : wxString::FromUTF8(op.argv[0]);
        return wxString::Format("Extract (%s)", cmd.c_str());
      }
    }
    return "Operation";
  };

  fileOp_->queueScroll->Freeze();
  fileOp_->queueItemsSizer->Clear(/*delete_windows=*/true);

  const int count = static_cast<int>(opQueue_.size());
  for (int i = 0; i < count; i++) {
    const auto& op = opQueue_[static_cast<size_t>(i)];
    auto* row = new wxPanel(fileOp_->queueScroll, wxID_ANY);
    auto* hs = new wxBoxSizer(wxHORIZONTAL);
    row->SetSizer(hs);

    auto* vs = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(row, wxID_ANY, describe(op));
    const wxString status = (i == 0) ? "Waiting..." : "Queued...";
    auto* st = new wxStaticText(row, wxID_ANY, status);
    vs->Add(title, 0, wxBOTTOM, 2);
    vs->Add(st, 0);
    hs->Add(vs, 1, wxEXPAND | wxRIGHT, 8);

    auto* cancel = new wxButton(row, wxID_ANY, "Cancel");
    const std::uint64_t id = op.id;
    cancel->Bind(wxEVT_BUTTON, [this, id](wxCommandEvent&) {
      for (auto it = opQueue_.begin(); it != opQueue_.end(); ++it) {
        if (it->id == id) {
          opQueue_.erase(it);
          break;
        }
      }
      UpdateQueueUi();
    });
    hs->Add(cancel, 0, wxALIGN_CENTER_VERTICAL);

    fileOp_->queueItemsSizer->Add(row, 0, wxEXPAND | wxALL, 4);
  }

  if (fileOp_->queueBox) {
    fileOp_->queueBox->SetLabel(count > 0 ? wxString::Format("Queue (%d)", count) : "Queue");
  }
  if (fileOp_->clearQueueBtn) fileOp_->clearQueueBtn->Enable(count > 0);

  fileOp_->queueScroll->FitInside();
  fileOp_->queueScroll->Layout();
  fileOp_->queueScroll->Thaw();
  fileOp_->dlg->Layout();
}

namespace {

std::string PercentEncode(std::string s) {
  auto isUnreserved = [](unsigned char c) -> bool {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
  };
  std::string out;
  out.reserve(s.size());
  const char* hex = "0123456789ABCDEF";
  for (unsigned char c : s) {
    if (isUnreserved(c) || c == '/' ) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[(c >> 4) & 0xF]);
      out.push_back(hex[c & 0xF]);
    }
  }
  return out;
}

enum class ServerType { SMB, SSH, FTP, WebDAV, WebDAVS, AFP };

struct ConnectParams {
  ServerType type{ServerType::SMB};
  std::string server;
  int port{0};
  std::string folder;
  std::string username;
  std::string password;
  bool rememberPassword{false};
};

std::string BuildConnectUri(const ConnectParams& p) {
  auto schemeForType = [](ServerType t) -> std::string {
    switch (t) {
      case ServerType::SMB: return "smb";
      case ServerType::SSH: return "sftp"; // GIO uses sftp:// for SSH file transfers.
      case ServerType::FTP: return "ftp";
      case ServerType::WebDAV: return "dav";
      case ServerType::WebDAVS: return "davs";
      case ServerType::AFP: return "afp";
      default: return "smb";
    }
  };

  const auto scheme = schemeForType(p.type);
  std::string uri = scheme + "://";
  uri += p.server;

  const bool portAllowed = (scheme == "sftp" || scheme == "ftp" || scheme == "dav" || scheme == "davs");
  if (portAllowed && p.port > 0) {
    uri += ":" + std::to_string(p.port);
  }

  std::string path = p.folder;
  if (path.empty()) {
    path = (scheme == "smb" || scheme == "afp") ? "" : "/";
  }
  if (!path.empty() && path.front() != '/') path.insert(path.begin(), '/');
  uri += PercentEncode(path);
  return uri;
}

std::optional<ConnectParams> ShowConnectDialog(wxWindow* parent) {
  wxDialog dlg(parent, wxID_ANY, "Connect to Server", wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

  auto* root = new wxBoxSizer(wxVERTICAL);
  dlg.SetSizer(root);

  // Server details
  auto* serverBox = new wxStaticBoxSizer(wxVERTICAL, &dlg, "Server Details");
  root->Add(serverBox, 0, wxALL | wxEXPAND, 10);

  auto* grid = new wxFlexGridSizer(2, 8, 8);
  grid->AddGrowableCol(1, 1);
  serverBox->Add(grid, 0, wxEXPAND | wxALL, 10);

  auto* serverCtrl = new wxTextCtrl(&dlg, wxID_ANY, "");
  auto* portCtrl = new wxSpinCtrl(&dlg, wxID_ANY);
  portCtrl->SetRange(0, 65535);
  portCtrl->SetValue(0);

  wxArrayString types;
  types.Add("SMB (Windows Share)");
  types.Add("SSH (SFTP)");
  types.Add("FTP");
  types.Add("WebDAV");
  types.Add("WebDAV (HTTPS)");
  types.Add("AFP");
  auto* typeCtrl = new wxChoice(&dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize, types);
  typeCtrl->SetSelection(0);

  auto* folderCtrl = new wxTextCtrl(&dlg, wxID_ANY, "");

  grid->Add(new wxStaticText(&dlg, wxID_ANY, "Server:"), 0, wxALIGN_CENTER_VERTICAL);
  grid->Add(serverCtrl, 1, wxEXPAND);
  grid->Add(new wxStaticText(&dlg, wxID_ANY, "Port:"), 0, wxALIGN_CENTER_VERTICAL);
  grid->Add(portCtrl, 1, wxEXPAND);
  grid->Add(new wxStaticText(&dlg, wxID_ANY, "Type:"), 0, wxALIGN_CENTER_VERTICAL);
  grid->Add(typeCtrl, 1, wxEXPAND);
  grid->Add(new wxStaticText(&dlg, wxID_ANY, "Folder:"), 0, wxALIGN_CENTER_VERTICAL);
  grid->Add(folderCtrl, 1, wxEXPAND);

  // Default port helpers.
  auto defaultPortForSelection = [](int sel) -> int {
    switch (sel) {
      case 1: return 22;  // SSH (SFTP)
      case 2: return 21;  // FTP
      case 3: return 80;  // WebDAV
      case 4: return 443; // WebDAVS
      default: return 0;  // SMB/AFP
    }
  };

  bool portTouched = false;
  portCtrl->Bind(wxEVT_SPINCTRL, [&portTouched](wxSpinEvent&) { portTouched = true; });
  portCtrl->Bind(wxEVT_TEXT, [&portTouched](wxCommandEvent&) { portTouched = true; });

  typeCtrl->Bind(wxEVT_CHOICE, [&](wxCommandEvent&) {
    const int sel = typeCtrl->GetSelection();
    const int def = defaultPortForSelection(sel);
    // Only auto-fill if the user hasn't edited the port yet, or it's currently 0.
    if (!portTouched || portCtrl->GetValue() == 0) {
      portCtrl->SetValue(def);
      portTouched = false;
    }
  });

  // User details
  auto* userBox = new wxStaticBoxSizer(wxVERTICAL, &dlg, "User Details");
  root->Add(userBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  auto* ugrid = new wxFlexGridSizer(2, 8, 8);
  ugrid->AddGrowableCol(1, 1);
  userBox->Add(ugrid, 0, wxEXPAND | wxALL, 10);

  auto* userCtrl = new wxTextCtrl(&dlg, wxID_ANY, "");
  auto* passCtrl = new wxTextCtrl(&dlg, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
  auto* rememberCtrl = new wxCheckBox(&dlg, wxID_ANY, "Remember this password");
  rememberCtrl->SetValue(false);

  ugrid->Add(new wxStaticText(&dlg, wxID_ANY, "User name:"), 0, wxALIGN_CENTER_VERTICAL);
  ugrid->Add(userCtrl, 1, wxEXPAND);
  ugrid->Add(new wxStaticText(&dlg, wxID_ANY, "Password:"), 0, wxALIGN_CENTER_VERTICAL);
  ugrid->Add(passCtrl, 1, wxEXPAND);

  userBox->Add(rememberCtrl, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

  // Buttons
  auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
  auto* saveBtn = new wxButton(&dlg, wxID_ANY, "Save...");
  auto* connectBtn = new wxButton(&dlg, wxID_OK, "Connect");
  auto* cancelBtn = new wxButton(&dlg, wxID_CANCEL, "Cancel");
  connectBtn->SetDefault();
  btnSizer->Add(saveBtn, 0, wxRIGHT, 8);
  btnSizer->AddStretchSpacer(1);
  btnSizer->Add(connectBtn, 0, wxRIGHT, 8);
  btnSizer->Add(cancelBtn, 0);
  root->Add(btnSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  auto readParams = [&]() -> ConnectParams {
    ConnectParams out;
    out.server = serverCtrl->GetValue().ToStdString();
    out.port = portCtrl->GetValue();
    out.folder = folderCtrl->GetValue().ToStdString();
    out.username = userCtrl->GetValue().ToStdString();
    out.password = passCtrl->GetValue().ToStdString();
    out.rememberPassword = rememberCtrl->GetValue();
    switch (typeCtrl->GetSelection()) {
      case 0: out.type = ServerType::SMB; break;
      case 1: out.type = ServerType::SSH; break;
      case 2: out.type = ServerType::FTP; break;
      case 3: out.type = ServerType::WebDAV; break;
      case 4: out.type = ServerType::WebDAVS; break;
      case 5: out.type = ServerType::AFP; break;
      default: out.type = ServerType::SMB; break;
    }
    return out;
  };

  saveBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    const auto p = readParams();
    if (p.server.empty()) {
      wxMessageBox("Please enter a server before saving.", "Quarry", wxOK | wxICON_INFORMATION, &dlg);
      return;
    }

    const auto uri = BuildConnectUri(p);
    wxTextEntryDialog nameDlg(&dlg, "Connection name:", "Save Connection", wxString::FromUTF8(uri));
    if (nameDlg.ShowModal() != wxID_OK) return;
    const auto name = nameDlg.GetValue().ToStdString();
    if (name.empty()) return;

    connections::Connection c;
    c.name = name;
    c.server = p.server;
    c.port = p.port;
    c.folder = p.folder;
    c.username = p.username;
    c.rememberPassword = p.rememberPassword;
    switch (p.type) {
      case ServerType::SMB: c.type = connections::Type::SMB; break;
      case ServerType::SSH: c.type = connections::Type::SSH; break;
      case ServerType::FTP: c.type = connections::Type::FTP; break;
      case ServerType::WebDAV: c.type = connections::Type::WebDAV; break;
      case ServerType::WebDAVS: c.type = connections::Type::WebDAVS; break;
      case ServerType::AFP: c.type = connections::Type::AFP; break;
      default: c.type = connections::Type::Unknown; break;
    }
    connections::Upsert(std::move(c));
    wxMessageBox("Saved.", "Quarry", wxOK | wxICON_INFORMATION, &dlg);
  });

  dlg.Fit();
  dlg.Layout();
  dlg.CentreOnParent();

  if (dlg.ShowModal() != wxID_OK) return std::nullopt;

  auto out = readParams();
  if (out.server.empty()) return std::nullopt;
  return out;
}
} // namespace

MainFrame::MainFrame(std::string topDir, std::string bottomDir)
    : wxFrame(nullptr, wxID_ANY, "Quarry", wxDefaultPosition, wxSize(1200, 700)) {
  BuildMenu();
  BuildLayout();
  BindEvents();

  const auto home = wxGetHomeDir().ToStdString();
  pendingTopDir_ = topDir.empty() ? home : std::move(topDir);
  pendingBottomDir_ = bottomDir.empty() ? pendingTopDir_ : std::move(bottomDir);

  LoadStartupView();

  SetMinSize(wxSize(900, 500));

  // Panels are initialized after the window is shown to avoid GTK warnings
  // caused by laying out scrolled windows at transient tiny sizes during startup.
}

void MainFrame::StartFileOperation(const wxString& title,
                                   const std::vector<std::filesystem::path>& sources,
                                   const std::filesystem::path& dstDir,
                                   bool move) {
  CopyMoveWithProgress(title, sources, dstDir, move);
}

void MainFrame::StartTrashOperation(const std::vector<std::filesystem::path>& sources) {
  TrashWithProgress(sources);
}

void MainFrame::StartDeleteOperation(const std::vector<std::filesystem::path>& sources) {
  DeleteWithProgress(sources);
}

void MainFrame::StartExtractOperation(const std::vector<std::string>& argv,
                                      const std::filesystem::path& refreshDir,
                                      bool treeChanged) {
  ExtractWithProgress(argv, refreshDir, treeChanged);
}

void MainFrame::BuildMenu() {
  auto* fileMenu = new wxMenu();
  fileMenu->Append(wxID_EXIT, "Quit\tCtrl+Q");

  auto* opsMenu = new wxMenu();
  opsMenu->Append(ID_Refresh, "Refresh\tF5");
  opsMenu->AppendSeparator();
  opsMenu->Append(ID_Copy, "Copy to other pane\tCtrl+C");
  opsMenu->Append(ID_Move, "Move to other pane\tCtrl+M");
  opsMenu->Append(ID_Trash, "Move to Trash\tDel");
  opsMenu->Append(ID_DeletePermanent, "Delete permanently\tShift+Del");
  opsMenu->AppendSeparator();
  opsMenu->Append(ID_Rename, "Rename\tF2");
  opsMenu->Append(ID_MkDir, "New Folder\tF7");
  opsMenu->AppendSeparator();
  opsMenu->Append(wxID_PREFERENCES, "Preferences...\tCtrl+,");

  auto* networkMenu = new wxMenu();
  networkMenu->Append(ID_ConnectToServer, "Connect to Server...\tCtrl+L");
  networkMenu->Append(ID_ConnectionsManager, "Connections...");

  auto* viewMenu = new wxMenu();
  viewMenu->Append(ID_SaveDefaultView, "Save View as Default");
  viewMenu->Append(ID_LoadDefaultView, "Load Default View");

  auto* helpMenu = new wxMenu();
  helpMenu->Append(wxID_ABOUT, "About");

  auto* bar = new wxMenuBar();
  bar->Append(fileMenu, "&File");
  bar->Append(opsMenu, "&Edit");
  bar->Append(viewMenu, "&View");
  bar->Append(networkMenu, "&Network");
  bar->Append(helpMenu, "&Help");
  SetMenuBar(bar);
}

void MainFrame::BuildLayout() {
  quad_ = new QuadSplitter(this, wxID_ANY);

  auto* sizer = new wxBoxSizer(wxVERTICAL);
  sizer->Add(quad_, 1, wxEXPAND);
  SetSizer(sizer);
}

void MainFrame::BindEvents() {
  Bind(wxEVT_MENU, &MainFrame::OnQuit, this, wxID_EXIT);
  Bind(wxEVT_MENU, &MainFrame::OnAbout, this, wxID_ABOUT);
  Bind(wxEVT_MENU, &MainFrame::OnPreferences, this, wxID_PREFERENCES);
  Bind(wxEVT_MENU, &MainFrame::OnRefresh, this, ID_Refresh);
  Bind(wxEVT_MENU, &MainFrame::OnConnectToServer, this, ID_ConnectToServer);
  Bind(wxEVT_MENU, &MainFrame::OnConnectionsManager, this, ID_ConnectionsManager);
  Bind(wxEVT_MENU, &MainFrame::OnCopy, this, ID_Copy);
  Bind(wxEVT_MENU, &MainFrame::OnMove, this, ID_Move);
  Bind(wxEVT_MENU, &MainFrame::OnDelete, this, ID_Trash);
  Bind(wxEVT_MENU, &MainFrame::OnDeletePermanent, this, ID_DeletePermanent);
  Bind(wxEVT_MENU, &MainFrame::OnRename, this, ID_Rename);
  Bind(wxEVT_MENU, &MainFrame::OnMkDir, this, ID_MkDir);
  Bind(wxEVT_MENU, [this](wxCommandEvent&) { SaveDefaultView(); }, ID_SaveDefaultView);
  Bind(wxEVT_MENU, [this](wxCommandEvent&) { LoadDefaultView(); }, ID_LoadDefaultView);

  Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& e) {
    if (e.GetActive() && fileOp_ && fileOp_->dlg && fileOp_->dlg->IsShown()) {
      fileOp_->dlg->Raise();
    }
    e.Skip();
  });

  Bind(wxEVT_SHOW, [this](wxShowEvent& e) {
    e.Skip();
    if (!e.IsShown()) return;
    CallAfter([this]() {
      ApplyStartupWindowCascade();
      InitPanelsIfNeeded();
    });
  });

  Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
    wxConfig cfg("Quarry");
    bool restoreLast = false;
    cfg.Read("/prefs/startup/restore_last", &restoreLast, false);
    if (restoreLast) SaveLastView(/*showMessage=*/false);
    e.Skip();
  });

  // Key navigation that should work regardless of which child has focus.
  Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
    InitPanelsIfNeeded();
    auto* active = GetActivePanel();
    if (!active) {
      e.Skip();
      return;
    }

    auto isTextInputFocused = []() -> bool {
      wxWindow* w = wxWindow::FindFocus();
      while (w) {
        if (dynamic_cast<wxTextCtrl*>(w) != nullptr) return true;
        w = w->GetParent();
      }
      return false;
    };

    const int key = e.GetKeyCode();
    if (isTextInputFocused()) {
      e.Skip();
      return;
    }

    if (key == WXK_TAB && !e.ControlDown() && !e.AltDown()) {
      SetActivePane(activePane_ == ActivePane::Top ? ActivePane::Bottom : ActivePane::Top);
      if (auto* a = GetActivePanel()) a->FocusPrimary();
      return;
    }
    if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
      active->OpenSelection();
      return;
    }
    if (key == WXK_BACK) {
      active->NavigateUp();
      return;
    }
    if (key == WXK_F5) {
      wxCommandEvent ev;
      OnRefresh(ev);
      return;
    }
    if (key == WXK_F2) {
      wxCommandEvent ev;
      OnRename(ev);
      return;
    }
    if (key == WXK_F7) {
      wxCommandEvent ev;
      OnMkDir(ev);
      return;
    }
    if (key == WXK_DELETE) {
      wxCommandEvent ev;
      if (e.ShiftDown()) OnDeletePermanent(ev);
      else OnDelete(ev);
      return;
    }
    if (e.ControlDown() && !e.AltDown() && !e.MetaDown()) {
      if (key == 'C' || key == 'c') {
        wxCommandEvent ev;
        OnCopy(ev);
        return;
      }
      if (key == 'M' || key == 'm') {
        wxCommandEvent ev;
        OnMove(ev);
        return;
      }
    }
    e.Skip();
  });

  // Keep global accelerators minimal so normal text editing shortcuts work in the address bar.
  wxAcceleratorEntry entries[1];
  entries[0].Set(wxACCEL_CTRL, (int)'Q', wxID_EXIT);
  SetAcceleratorTable(wxAcceleratorTable(1, entries));
}

void MainFrame::InitPanelsIfNeeded() {
  if (panelsInitialized_) return;
  if (!quad_) return;

  // Only initialize once the frame has a non-trivial size.
  const auto cs = quad_->GetClientSize();
  if (cs.x <= 0 || cs.y <= 0) return;

  panelsInitialized_ = true;

  top_ = std::make_unique<FilePanel>(quad_, quad_);
  bottom_ = std::make_unique<FilePanel>(quad_, quad_);
  quad_->SetWindows(top_->SidebarWindow(),
                    top_->ListWindow(),
                    bottom_->SidebarWindow(),
                    bottom_->ListWindow());

  if (pendingVSash_) quad_->SetVerticalSashPosition(*pendingVSash_);
  if (pendingHSash_) quad_->SetHorizontalSashPosition(*pendingHSash_);

  BindPanelEvents();

  if (pendingTopCols_) top_->SetListColumnWidths(*pendingTopCols_);
  if (pendingBottomCols_) bottom_->SetListColumnWidths(*pendingBottomCols_);

  if (pendingTopSortCol_ || pendingTopSortAsc_) {
    top_->SetSort(pendingTopSortCol_.value_or(0), pendingTopSortAsc_.value_or(true));
  }
  if (pendingBottomSortCol_ || pendingBottomSortAsc_) {
    bottom_->SetSort(pendingBottomSortCol_.value_or(0), pendingBottomSortAsc_.value_or(true));
  }

  if (!pendingTopDir_.empty()) top_->SetDirectory(pendingTopDir_);
  if (!pendingBottomDir_.empty()) bottom_->SetDirectory(pendingBottomDir_);

  SetActivePane(activePane_);
  Layout();
}

void MainFrame::BindPanelEvents() {
  if (!top_ || !bottom_) return;

  top_->BindFocusEvents([this]() { SetActivePane(ActivePane::Top); });
  bottom_->BindFocusEvents([this]() { SetActivePane(ActivePane::Bottom); });

  top_->BindDropFiles([this](const std::vector<std::filesystem::path>& paths, bool move) {
    TransferDroppedPaths(top_.get(), paths, move);
  });
  bottom_->BindDropFiles([this](const std::vector<std::filesystem::path>& paths, bool move) {
    TransferDroppedPaths(bottom_.get(), paths, move);
  });

  auto onDirChanged = [this](const std::filesystem::path& dir, bool treeChanged) {
    RefreshPanelsShowing(dir, treeChanged);
  };
  top_->BindDirContentsChanged(onDirChanged);
  bottom_->BindDirContentsChanged(onDirChanged);
}

void MainFrame::LoadStartupView() {
  wxConfig cfg("Quarry");
  bool restoreLast = false;
  cfg.Read("/prefs/startup/restore_last", &restoreLast, false);

  if (restoreLast) {
    if (LoadViewFromConfig("/view/last", /*applyToPanes=*/false, /*showNoViewMessage=*/false)) {
      // When restoring last view, don't apply the startup cascade offset; users
      // expect the window to reopen exactly where it was.
      skipStartupCascade_ = true;
      return;
    }
  }

  // Default behavior: try to load the saved default view (if any). If none exists,
  // we fall back to built-in defaults.
  LoadViewFromConfig("/view/default", /*applyToPanes=*/false, /*showNoViewMessage=*/false);
}

void MainFrame::SaveDefaultView() {
  SaveViewToConfig("/view/default", /*showMessage=*/true);
}

void MainFrame::LoadDefaultView() {
  LoadDefaultViewInternal(/*applyToPanes=*/true, /*showNoDefaultMessage=*/true);
}

bool MainFrame::LoadDefaultViewInternal(bool applyToPanes, bool showNoDefaultMessage) {
  return LoadViewFromConfig("/view/default", applyToPanes, showNoDefaultMessage);
}

void MainFrame::SaveLastView(bool showMessage) {
  SaveViewToConfig("/view/last", showMessage);
}

void MainFrame::SaveViewToConfig(const wxString& base, bool showMessage) {
  InitPanelsIfNeeded();

  wxConfig cfg("Quarry");

  cfg.Write(base + "/window/maximized", IsMaximized());

  if (!IsMaximized() && !IsIconized()) {
    const auto p = GetPosition();
    const auto s = GetSize();
    cfg.Write(base + "/window/x", p.x);
    cfg.Write(base + "/window/y", p.y);
    cfg.Write(base + "/window/w", s.x);
    cfg.Write(base + "/window/h", s.y);
  }

  if (quad_) {
    const int v = quad_->GetVerticalSashPosition();
    const int h = quad_->GetHorizontalSashPosition();
    if (v > 0) cfg.Write(base + "/split/v", v);
    if (h > 0) cfg.Write(base + "/split/h", h);
  }

  if (top_) {
    const auto widths = top_->GetListColumnWidths();
    for (int i = 0; i < 4; i++) {
      if (widths[static_cast<size_t>(i)] > 0) {
        cfg.Write(wxString::Format(base + "/columns/top/%d", i), widths[static_cast<size_t>(i)]);
      }
    }
    cfg.Write(base + "/sort/top/col", top_->GetSortColumnIndex());
    cfg.Write(base + "/sort/top/asc", top_->IsSortAscending());
  }
  if (bottom_) {
    const auto widths = bottom_->GetListColumnWidths();
    for (int i = 0; i < 4; i++) {
      if (widths[static_cast<size_t>(i)] > 0) {
        cfg.Write(wxString::Format(base + "/columns/bottom/%d", i), widths[static_cast<size_t>(i)]);
      }
    }
    cfg.Write(base + "/sort/bottom/col", bottom_->GetSortColumnIndex());
    cfg.Write(base + "/sort/bottom/asc", bottom_->IsSortAscending());
  }

  cfg.Flush();
  if (showMessage) {
    if (base == "/view/default") {
      wxMessageBox("Default view saved.", "Quarry", wxOK | wxICON_INFORMATION, this);
    } else {
      wxMessageBox("Saved.", "Quarry", wxOK | wxICON_INFORMATION, this);
    }
  }
}

bool MainFrame::LoadViewFromConfig(const wxString& base,
                                  bool applyToPanes,
                                  bool showNoViewMessage) {
  wxConfig cfg("Quarry");

  bool hasAny = false;

  long x = 0, y = 0, w = 0, h = 0;
  const bool hasX = cfg.Read(base + "/window/x", &x);
  const bool hasY = cfg.Read(base + "/window/y", &y);
  const bool hasW = cfg.Read(base + "/window/w", &w);
  const bool hasH = cfg.Read(base + "/window/h", &h);
  if (hasW && hasH && w > 0 && h > 0) {
    SetSize(static_cast<int>(w), static_cast<int>(h));
    hasAny = true;
  }
  if (hasX && hasY) {
    Move(static_cast<int>(x), static_cast<int>(y));
    hasAny = true;
  }

  bool maximized = false;
  if (cfg.Read(base + "/window/maximized", &maximized)) {
    hasAny = true;
    if (maximized) Maximize(true);
    else if (IsMaximized()) Maximize(false);
  }

  long vSash = 0;
  if (cfg.Read(base + "/split/v", &vSash) && vSash > 0) {
    pendingVSash_ = static_cast<int>(vSash);
    hasAny = true;
  }
  long hSash = 0;
  if (cfg.Read(base + "/split/h", &hSash) && hSash > 0) {
    pendingHSash_ = static_cast<int>(hSash);
    hasAny = true;
  }

  std::array<int, 4> topCols{0, 0, 0, 0};
  std::array<int, 4> bottomCols{0, 0, 0, 0};
  bool anyTop = false;
  bool anyBottom = false;
  for (int i = 0; i < 4; i++) {
    long v = 0;
    if (cfg.Read(wxString::Format(base + "/columns/top/%d", i), &v) && v > 0) {
      topCols[static_cast<size_t>(i)] = static_cast<int>(v);
      anyTop = true;
    }
    v = 0;
    if (cfg.Read(wxString::Format(base + "/columns/bottom/%d", i), &v) && v > 0) {
      bottomCols[static_cast<size_t>(i)] = static_cast<int>(v);
      anyBottom = true;
    }
  }
  if (anyTop) {
    pendingTopCols_ = topCols;
    hasAny = true;
  }
  if (anyBottom) {
    pendingBottomCols_ = bottomCols;
    hasAny = true;
  }

  long sortCol = 0;
  bool sortAsc = true;
  if (cfg.Read(base + "/sort/top/col", &sortCol)) {
    pendingTopSortCol_ = static_cast<int>(sortCol);
    hasAny = true;
  }
  if (cfg.Read(base + "/sort/top/asc", &sortAsc)) {
    pendingTopSortAsc_ = sortAsc;
    hasAny = true;
  }
  sortCol = 0;
  sortAsc = true;
  if (cfg.Read(base + "/sort/bottom/col", &sortCol)) {
    pendingBottomSortCol_ = static_cast<int>(sortCol);
    hasAny = true;
  }
  if (cfg.Read(base + "/sort/bottom/asc", &sortAsc)) {
    pendingBottomSortAsc_ = sortAsc;
    hasAny = true;
  }

  if (!hasAny) {
    if (showNoViewMessage) {
      if (base == "/view/default") {
        wxMessageBox("No default view has been saved yet.", "Quarry", wxOK | wxICON_INFORMATION, this);
      } else {
        wxMessageBox("No saved view is available.", "Quarry", wxOK | wxICON_INFORMATION, this);
      }
    }
    return false;
  }

  if (applyToPanes) {
    InitPanelsIfNeeded();
    if (quad_) {
      if (pendingVSash_) quad_->SetVerticalSashPosition(*pendingVSash_);
      if (pendingHSash_) quad_->SetHorizontalSashPosition(*pendingHSash_);
    }
    if (top_) {
      if (pendingTopCols_) top_->SetListColumnWidths(*pendingTopCols_);
      if (pendingTopSortCol_ || pendingTopSortAsc_) {
        top_->SetSort(pendingTopSortCol_.value_or(0), pendingTopSortAsc_.value_or(true));
      }
    }
    if (bottom_) {
      if (pendingBottomCols_) bottom_->SetListColumnWidths(*pendingBottomCols_);
      if (pendingBottomSortCol_ || pendingBottomSortAsc_) {
        bottom_->SetSort(pendingBottomSortCol_.value_or(0), pendingBottomSortAsc_.value_or(true));
      }
    }
    Layout();
  }

  return true;
}

void MainFrame::ApplyStartupWindowCascade() {
  if (startupCascadeApplied_) return;
  startupCascadeApplied_ = true;
  if (skipStartupCascade_) return;

  if (IsMaximized() || IsFullScreen()) return;

  int displayIndex = wxDisplay::GetFromWindow(this);
  if (displayIndex < 0) displayIndex = 0;
  if (displayIndex >= wxDisplay::GetCount()) displayIndex = 0;
  wxDisplay display(displayIndex);
  const wxRect work = display.GetClientArea();
  if (work.width <= 0 || work.height <= 0) return;

  wxPoint pos = GetPosition();
  const wxSize size = GetSize();
  if (pos.x < 0 || pos.y < 0) {
    pos = wxPoint(work.x + 24, work.y + 24);
  }

  wxConfig cfg("Quarry");
  long slot = 0;
  cfg.Read("/runtime/cascade/slot", &slot, 0);
  slot = (slot + 1) % 16;
  cfg.Write("/runtime/cascade/slot", slot);
  cfg.Flush();

  const int dx = 24;
  const int dy = 24;
  int nx = pos.x + static_cast<int>(slot) * dx;
  int ny = pos.y + static_cast<int>(slot) * dy;

  const int maxX = work.x + std::max(0, work.width - size.GetWidth());
  const int maxY = work.y + std::max(0, work.height - size.GetHeight());
  nx = std::max(work.x, std::min(nx, maxX));
  ny = std::max(work.y, std::min(ny, maxY));

  if (nx == pos.x && ny == pos.y) return;
  Move(nx, ny);
}

void MainFrame::TransferDroppedPaths(FilePanel* target,
                                     const std::vector<std::filesystem::path>& sources,
                                     bool move) {
  if (!target) return;
  if (sources.empty()) return;

  const auto dstDir = target->GetDirectoryPath();
  if (dstDir.empty() || !PathExistsAny(dstDir) || !IsDirectoryAny(dstDir)) {
    wxMessageBox("Drop target is not a directory.", "Quarry", wxOK | wxICON_WARNING, this);
    return;
  }

  CopyMoveWithProgress(move ? "Move" : "Copy", sources, dstDir, move);
}

void MainFrame::CopyMoveWithProgress(const wxString& title,
                                     const std::vector<std::filesystem::path>& sources,
                                     const std::filesystem::path& dstDir,
                                     bool move) {
  CopyMoveWithProgressInternal(title, sources, dstDir, move, /*alreadyConfirmed=*/false);
}

void MainFrame::CopyMoveWithProgressInternal(const wxString& title,
                                             const std::vector<std::filesystem::path>& sources,
                                             const std::filesystem::path& dstDir,
                                             bool move,
                                             bool alreadyConfirmed) {
  if (sources.empty()) return;

  if (dstDir.empty() || !PathExistsAny(dstDir) || !IsDirectoryAny(dstDir)) {
    wxMessageBox("Destination is not a directory.", "Quarry", wxOK | wxICON_WARNING, this);
    return;
  }

  // If an operation is already running, queue immediately without extra prompts.
  // Conflicts (overwrite/skip/rename) are resolved per-file when the job runs.
  if (fileOp_) {
    EnqueueOp(QueuedOp{.kind = OpKind::CopyMove,
                       .title = title,
                       .sources = sources,
                       .dstDir = dstDir,
                       .move = move});
    return;
  }

  // Confirmation: copy is safe enough to start immediately; move is more dangerous.
  // Do not show an "overwrite" warning: per-file conflicts are handled during the operation.
  if (!alreadyConfirmed && move) {
    const wxString dstDirWx = wxString::FromUTF8(dstDir.string());
    const wxString confirmMsg = wxString::Format("%s %zu item(s) to:\n\n%s",
                                                 title.c_str(),
                                                 sources.size(),
                                                 dstDirWx.c_str());
    if (wxMessageBox(confirmMsg, title, wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) {
      return;
    }
  }

  fileOp_ = std::make_unique<FileOpSession>(this);
  fileOp_->state = std::make_shared<AsyncFileOpState>();

  {
    for (const auto& p : sources) {
      if (IsDirectoryAny(p)) {
        fileOp_->state->hasDir = true;
        break;
      }
    }
  }

  // Modeless dialog.
  fileOp_->dlg = new wxDialog(this, wxID_ANY, title, wxDefaultPosition, wxSize(650, 340),
                              wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxFRAME_FLOAT_ON_PARENT);
  auto* root = new wxBoxSizer(wxVERTICAL);
  fileOp_->dlg->SetSizer(root);

  fileOp_->titleText = new wxStaticText(fileOp_->dlg, wxID_ANY, move ? "Moving..." : "Copying...");
  fileOp_->detailText = new wxStaticText(fileOp_->dlg, wxID_ANY, "Preparing...");
  fileOp_->gauge = new wxGauge(fileOp_->dlg, wxID_ANY, fileOp_->range);
  fileOp_->cancelBtn = new wxButton(fileOp_->dlg, wxID_CANCEL, "Cancel");

  root->Add(fileOp_->titleText, 0, wxEXPAND | wxALL, 10);
  root->Add(fileOp_->detailText, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
  root->Add(fileOp_->gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

  auto* queueSizer = new wxStaticBoxSizer(wxVERTICAL, fileOp_->dlg, "Queue");
  fileOp_->queueBox = queueSizer->GetStaticBox();
  fileOp_->queueScroll = new wxScrolledWindow(fileOp_->dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                              wxVSCROLL);
  fileOp_->queueScroll->SetScrollRate(0, 10);
  fileOp_->queueItemsSizer = new wxBoxSizer(wxVERTICAL);
  fileOp_->queueScroll->SetSizer(fileOp_->queueItemsSizer);
  queueSizer->Add(fileOp_->queueScroll, 1, wxEXPAND | wxALL, 8);

  auto* qBtns = new wxBoxSizer(wxHORIZONTAL);
  qBtns->AddStretchSpacer(1);
  fileOp_->clearQueueBtn = new wxButton(fileOp_->dlg, wxID_ANY, "Clear Queue");
  qBtns->Add(fileOp_->clearQueueBtn, 0);
  queueSizer->Add(qBtns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

  root->Add(queueSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

  auto* btns = new wxBoxSizer(wxHORIZONTAL);
  btns->AddStretchSpacer(1);
  btns->Add(fileOp_->cancelBtn, 0);
  root->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
  fileOp_->dlg->Layout();
  fileOp_->dlg->Show();

  fileOp_->clearQueueBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    opQueue_.clear();
    UpdateQueueUi();
  });
  UpdateQueueUi();

  // wxTimer events are delivered to the timer owner (not the wxTimer object).
  // Bind them on the dialog so updates continue even while the main window is used.
  fileOp_->timerId = wxWindow::NewControlId();
  fileOp_->timer.SetOwner(fileOp_->dlg, fileOp_->timerId);

  fileOp_->cancelBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    if (!fileOp_ || !fileOp_->state) return;
    fileOp_->state->cancelRequested.store(true);
    fileOp_->cancelBtn->Disable();
    std::lock_guard<std::mutex> lock(fileOp_->state->mu);
    fileOp_->state->cv.notify_all();
  });

  fileOp_->dlg->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
    e.Veto();
  });

  const auto state = fileOp_->state;
  fileOp_->worker = std::thread([state, sources, dstDir, move]() {
    std::error_code workerEc;
    const CancelFn shouldCancel = [state]() { return state->cancelRequested.load(); };

    const CopyProgressFn scanProgress = [state](const std::filesystem::path& current) {
      std::string label = current.filename().string();
      if (label.empty()) label = current.string();
      std::lock_guard<std::mutex> lock(state->mu);
      state->currentLabel = "Scanning: " + label;
    };

    // Only scan local sources for total bytes; for remote sources, just show speed and unknown remaining.
    std::uintmax_t total = 0;
    bool canScan = true;
    for (const auto& p : sources) {
      if (LooksLikeUriPath(p)) {
        canScan = false;
        break;
      }
    }
    if (canScan) total = EstimateTotalBytes(sources, shouldCancel, scanProgress);
    {
      std::lock_guard<std::mutex> lock(state->mu);
      state->totalBytes = total;
      state->scanDone = true;
      if (state->currentLabel.rfind("Scanning:", 0) == 0) state->currentLabel = "Preparing...";
    }
    state->cv.notify_all();

    for (const auto& src : sources) {
      if (state->cancelRequested.load()) break;
      if (src.empty()) {
        state->done.fetch_add(1);
        continue;
      }

      workerEc.clear();
      if (!PathExistsAny(src)) {
        state->done.fetch_add(1);
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(state->mu);
        state->currentLabel = src.filename().string();
      }

      auto dst = JoinDirAndNameAny(dstDir, src.filename().string());

      // Conflict handling (delegated to UI thread).
      bool skipItem = false;
      for (;;) {
        if (state->cancelRequested.load()) break;
        if (!PathExistsAny(dst)) break;

        std::unique_lock<std::mutex> lock(state->mu);
        state->prompt = AsyncFileOpPrompt{.kind = AsyncFileOpPrompt::Kind::Exists, .dst = dst};
        state->reply.reset();
        state->cv.notify_all();
        state->cv.wait(lock, [state]() { return state->reply.has_value() || state->cancelRequested.load(); });
        if (state->cancelRequested.load()) break;

        const auto reply = *state->reply;
        state->prompt.reset();
        state->reply.reset();

        if (reply.existsChoice == ExistsChoice::Skip) {
          skipItem = true;
          break;
        }
        if (reply.existsChoice == ExistsChoice::Cancel) {
          state->cancelRequested.store(true);
          break;
        }
        if (reply.existsChoice == ExistsChoice::Rename) {
          if (!reply.renameTo) {
            state->cancelRequested.store(true);
            break;
          }
          dst = JoinDirAndNameAny(dstDir, *reply.renameTo);
          continue;
        }
        break;  // Overwrite
      }

      if (state->cancelRequested.load()) break;
      if (skipItem) {
        state->done.fetch_add(1);
        continue;
      }

      const CopyProgressFn onProgress = [state, src](const std::filesystem::path& current) {
        const auto rel = current.lexically_relative(src);
        std::string label;
        if (!rel.empty() && rel != current) label = rel.string();
        if (label.empty()) label = current.filename().string();
        if (label.empty()) label = current.string();
        std::lock_guard<std::mutex> lock(state->mu);
        state->currentLabel = std::move(label);
      };

      const CopyBytesProgressFn onBytes = [state](std::uintmax_t bytesDelta) {
        state->bytesDone.fetch_add(bytesDelta);
      };

      const auto result = move ? MovePath(src, dst, shouldCancel, onProgress, onBytes)
                               : CopyPathRecursive(src, dst, shouldCancel, onProgress, onBytes);
      if (state->cancelRequested.load() && !result.ok && result.message == "Canceled") break;
      if (!result.ok) {
        std::unique_lock<std::mutex> lock(state->mu);
        state->prompt = AsyncFileOpPrompt{.kind = AsyncFileOpPrompt::Kind::Error,
                                          .dst = dst,
                                          .errorMessage = result.message.ToStdString()};
        state->reply.reset();
        state->cv.notify_all();
        state->cv.wait(lock, [state]() { return state->reply.has_value() || state->cancelRequested.load(); });
        if (state->cancelRequested.load()) break;

        const auto reply = *state->reply;
        state->prompt.reset();
        state->reply.reset();
        if (!reply.continueAfterError) {
          state->cancelRequested.store(true);
          break;
        }
      }

      state->done.fetch_add(1);
    }

    std::lock_guard<std::mutex> lock(state->mu);
    state->finished = true;
    state->cv.notify_all();
  });

  fileOp_->dlg->Bind(wxEVT_TIMER, [this, title, sources](wxTimerEvent&) {
    if (!fileOp_ || !fileOp_->state) return;
    const auto state = fileOp_->state;

    // Handle any pending prompt from the worker.
    std::optional<AsyncFileOpPrompt> prompt;
    {
      std::lock_guard<std::mutex> lock(state->mu);
      prompt = state->prompt;
    }
    if (prompt && !fileOp_->promptActive) {
      fileOp_->promptActive = true;
      AsyncFileOpReply reply;
      if (prompt->kind == AsyncFileOpPrompt::Kind::Exists) {
        const auto choice = PromptExists(this, prompt->dst);
        reply.existsChoice = choice;
        if (choice == ExistsChoice::Rename) {
          wxTextEntryDialog nameDlg(this, "New name:", "Rename", prompt->dst.filename().string());
          if (nameDlg.ShowModal() != wxID_OK) {
            reply.existsChoice = ExistsChoice::Cancel;
          } else {
            reply.renameTo = nameDlg.GetValue().ToStdString();
          }
        }
      } else if (prompt->kind == AsyncFileOpPrompt::Kind::Error) {
        wxMessageDialog dlg(this,
                            wxString::Format("%s failed:\n\n%s\n\nContinue?",
                                             title.c_str(),
                                             wxString::FromUTF8(prompt->errorMessage).c_str()),
                            title,
                            wxYES_NO | wxNO_DEFAULT | wxICON_ERROR);
        dlg.SetYesNoLabels("Continue", "Cancel");
        reply.continueAfterError = (dlg.ShowModal() == wxID_YES);
      } else {
        wxMessageDialog dlg(this,
                            wxString::Format("Trash failed:\n\n%s\n\nDelete permanently instead?",
                                             wxString::FromUTF8(prompt->errorMessage).c_str()),
                            "Trash failed",
                            wxYES_NO | wxNO_DEFAULT | wxCANCEL | wxICON_ERROR);
        dlg.SetYesNoCancelLabels("Delete", "Skip", "Cancel");
        const int rc = dlg.ShowModal();
        if (rc == wxID_YES) reply.trashFailChoice = TrashFailChoice::DeletePermanent;
        else if (rc == wxID_NO) reply.trashFailChoice = TrashFailChoice::Skip;
        else reply.trashFailChoice = TrashFailChoice::Cancel;
      }

      {
        std::lock_guard<std::mutex> lock(state->mu);
        state->reply = reply;
        state->cv.notify_all();
      }
      fileOp_->promptActive = false;
    }

    const auto bytesDone = state->bytesDone.load();
    const auto done = state->done.load();

    wxString label;
    bool scanDone = false;
    std::uintmax_t totalBytes = 0;
    bool finished = false;
    bool canceling = state->cancelRequested.load();
    {
      std::lock_guard<std::mutex> lock(state->mu);
      label = wxString::FromUTF8(state->currentLabel);
      scanDone = state->scanDone;
      totalBytes = state->totalBytes;
      finished = state->finished;
    }

    if (!fileOp_->configured && scanDone) {
      if (totalBytes > 0) {
        const std::uintmax_t maxInt = static_cast<std::uintmax_t>(std::numeric_limits<int>::max() - 1);
        fileOp_->bytesUnit = std::max<std::uintmax_t>(1, (totalBytes / maxInt) + 1);
        fileOp_->range = static_cast<int>(std::max<std::uintmax_t>(1, totalBytes / fileOp_->bytesUnit));
      } else {
        fileOp_->range = static_cast<int>(std::max<size_t>(1, sources.size()));
      }
      fileOp_->gauge->SetRange(fileOp_->range);
      fileOp_->start = std::chrono::steady_clock::now();
      fileOp_->configured = true;
    }

    if (!fileOp_->configured) {
      fileOp_->gauge->Pulse();
    } else if (totalBytes > 0) {
      const int v = static_cast<int>(std::min<std::uintmax_t>(fileOp_->range, bytesDone / fileOp_->bytesUnit));
      fileOp_->gauge->SetValue(v);
    } else {
      // For remote sources we often don't know total bytes, so keep the gauge active.
      fileOp_->gauge->Pulse();
    }

    wxString remaining = (totalBytes > 0) ? "Remaining: --:--:--" : "Remaining: (unknown)";
    wxString speed = "Speed: -- MB/s";
    wxString copied = "Copied: 0 B";

    if (bytesDone > 0) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - fileOp_->start);
      const auto elapsedSec = std::max<long long>(1, elapsed.count());
      const double bytesPerSec = static_cast<double>(bytesDone) / static_cast<double>(elapsedSec);
      const double mbPerSec = bytesPerSec / (1024.0 * 1024.0);
      speed = wxString::Format("Speed: %.1f MB/s", mbPerSec);
      copied = "Copied: " + wxString::FromUTF8(HumanSize(bytesDone));

      if (totalBytes > 0 && bytesPerSec > 1.0) {
        const auto left = static_cast<double>(totalBytes > bytesDone ? (totalBytes - bytesDone) : 0);
        const auto remSec = static_cast<long long>(left / bytesPerSec);
        remaining = "Remaining: " + FormatHMS(std::chrono::seconds(remSec));
      }
    }

    if (canceling) {
      fileOp_->titleText->SetLabel("Canceling...");
    }
    if (!label.empty()) fileOp_->detailText->SetLabel(label + "\n" + copied + "   " + speed + "   " + remaining);
    else fileOp_->detailText->SetLabel(copied + "   " + speed + "   " + remaining);

    if (fileOp_->dlg) {
      fileOp_->dlg->Layout();
    }

    if (finished) {
      fileOp_->timer.Stop();
      if (fileOp_->worker.joinable()) fileOp_->worker.join();

      if (top_) top_->RefreshAll();
      if (bottom_) bottom_->RefreshAll();
      if (state->hasDir) {
        if (top_) top_->RefreshTree();
        if (bottom_) bottom_->RefreshTree();
      }

      if (fileOp_->dlg) fileOp_->dlg->Destroy();
      fileOp_.reset();
      StartNextQueuedOp();
    }
  }, fileOp_->timerId);

  fileOp_->timer.Start(100);
}

void MainFrame::TrashWithProgress(const std::vector<std::filesystem::path>& sources) {
  TrashWithProgressInternal(sources, /*alreadyConfirmed=*/false);
}

void MainFrame::TrashWithProgressInternal(const std::vector<std::filesystem::path>& sources,
                                          bool alreadyConfirmed) {
  if (sources.empty()) return;

  if (!alreadyConfirmed) {
    const auto message = wxString::Format("Move %zu item(s) to Trash?", sources.size());
    if (wxMessageBox(message, "Trash", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) {
      return;
    }
  }

  if (fileOp_) {
    EnqueueOp(QueuedOp{.kind = OpKind::Trash, .title = "Trash", .sources = sources});
    return;
  }

  fileOp_ = std::make_unique<FileOpSession>(this);
  fileOp_->state = std::make_shared<AsyncFileOpState>();

  bool hasDir = false;
  for (const auto& p : sources) {
    if (IsDirectoryAny(p)) {
      hasDir = true;
      break;
    }
  }
  fileOp_->state->hasDir = hasDir;
  {
    std::lock_guard<std::mutex> lock(fileOp_->state->mu);
    fileOp_->state->scanDone = true;
    fileOp_->state->totalBytes = 0;
    fileOp_->state->currentLabel = "Preparing...";
  }

  fileOp_->dlg = new wxDialog(this, wxID_ANY, "Trash", wxDefaultPosition, wxSize(650, 340),
                              wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxFRAME_FLOAT_ON_PARENT);
  auto* root = new wxBoxSizer(wxVERTICAL);
  fileOp_->dlg->SetSizer(root);

  fileOp_->titleText = new wxStaticText(fileOp_->dlg, wxID_ANY, "Trashing...");
  fileOp_->detailText = new wxStaticText(fileOp_->dlg, wxID_ANY, "Preparing...");
  fileOp_->gauge = new wxGauge(fileOp_->dlg, wxID_ANY, fileOp_->range);
  fileOp_->cancelBtn = new wxButton(fileOp_->dlg, wxID_CANCEL, "Cancel");

  root->Add(fileOp_->titleText, 0, wxEXPAND | wxALL, 10);
  root->Add(fileOp_->detailText, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
  root->Add(fileOp_->gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

  auto* queueSizer = new wxStaticBoxSizer(wxVERTICAL, fileOp_->dlg, "Queue");
  fileOp_->queueBox = queueSizer->GetStaticBox();
  fileOp_->queueScroll = new wxScrolledWindow(fileOp_->dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                              wxVSCROLL);
  fileOp_->queueScroll->SetScrollRate(0, 10);
  fileOp_->queueItemsSizer = new wxBoxSizer(wxVERTICAL);
  fileOp_->queueScroll->SetSizer(fileOp_->queueItemsSizer);
  queueSizer->Add(fileOp_->queueScroll, 1, wxEXPAND | wxALL, 8);

  auto* qBtns = new wxBoxSizer(wxHORIZONTAL);
  qBtns->AddStretchSpacer(1);
  fileOp_->clearQueueBtn = new wxButton(fileOp_->dlg, wxID_ANY, "Clear Queue");
  qBtns->Add(fileOp_->clearQueueBtn, 0);
  queueSizer->Add(qBtns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

  root->Add(queueSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

  auto* btns = new wxBoxSizer(wxHORIZONTAL);
  btns->AddStretchSpacer(1);
  btns->Add(fileOp_->cancelBtn, 0);
  root->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
  fileOp_->dlg->Layout();
  fileOp_->dlg->Show();

  fileOp_->clearQueueBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    opQueue_.clear();
    UpdateQueueUi();
  });
  UpdateQueueUi();

  fileOp_->timerId = wxWindow::NewControlId();
  fileOp_->timer.SetOwner(fileOp_->dlg, fileOp_->timerId);

  fileOp_->cancelBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    if (!fileOp_ || !fileOp_->state) return;
    fileOp_->state->cancelRequested.store(true);
    fileOp_->cancelBtn->Disable();
    std::lock_guard<std::mutex> lock(fileOp_->state->mu);
    fileOp_->state->cv.notify_all();
  });

  fileOp_->dlg->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) { e.Veto(); });

  const auto state = fileOp_->state;
  fileOp_->worker = std::thread([state, sources]() {
    for (const auto& src : sources) {
      if (state->cancelRequested.load()) break;
      if (src.empty()) {
        state->done.fetch_add(1);
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto fn = src.filename().string();
        state->currentLabel = fn.empty() ? src.string() : fn;
      }

      const auto result = TrashPath(src);
      if (!result.ok) {
        std::unique_lock<std::mutex> lock(state->mu);
        state->prompt = AsyncFileOpPrompt{.kind = AsyncFileOpPrompt::Kind::TrashFailed,
                                          .src = src,
                                          .errorMessage = result.message.ToStdString()};
        state->reply.reset();
        state->cv.notify_all();
        state->cv.wait(lock, [state]() { return state->reply.has_value() || state->cancelRequested.load(); });
        if (state->cancelRequested.load()) break;

        const auto reply = *state->reply;
        state->prompt.reset();
        state->reply.reset();

        if (reply.trashFailChoice == TrashFailChoice::Skip) {
          state->done.fetch_add(1);
          continue;
        }
        if (reply.trashFailChoice == TrashFailChoice::Cancel) {
          state->cancelRequested.store(true);
          break;
        }

        const auto delRes = DeletePath(src);
        if (!delRes.ok) {
          std::unique_lock<std::mutex> lock2(state->mu);
          state->prompt = AsyncFileOpPrompt{.kind = AsyncFileOpPrompt::Kind::Error,
                                            .src = src,
                                            .errorMessage = delRes.message.ToStdString()};
          state->reply.reset();
          state->cv.notify_all();
          state->cv.wait(lock2, [state]() { return state->reply.has_value() || state->cancelRequested.load(); });
          if (state->cancelRequested.load()) break;

          const auto r2 = *state->reply;
          state->prompt.reset();
          state->reply.reset();
          if (!r2.continueAfterError) {
            state->cancelRequested.store(true);
            break;
          }
        }
      }

      state->done.fetch_add(1);
    }

    std::lock_guard<std::mutex> lock(state->mu);
    state->finished = true;
    state->cv.notify_all();
  });

  fileOp_->dlg->Bind(wxEVT_TIMER, [this, sources](wxTimerEvent&) {
    if (!fileOp_ || !fileOp_->state) return;
    const auto state = fileOp_->state;

    std::optional<AsyncFileOpPrompt> prompt;
    {
      std::lock_guard<std::mutex> lock(state->mu);
      prompt = state->prompt;
    }
    if (prompt && !fileOp_->promptActive) {
      fileOp_->promptActive = true;
      AsyncFileOpReply reply;
      if (prompt->kind == AsyncFileOpPrompt::Kind::TrashFailed) {
        wxMessageDialog dlg(this,
                            wxString::Format("Trash failed:\n\n%s\n\nDelete permanently instead?",
                                             wxString::FromUTF8(prompt->errorMessage).c_str()),
                            "Trash failed",
                            wxYES_NO | wxNO_DEFAULT | wxCANCEL | wxICON_ERROR);
        dlg.SetYesNoCancelLabels("Delete", "Skip", "Cancel");
        const int rc = dlg.ShowModal();
        if (rc == wxID_YES) reply.trashFailChoice = TrashFailChoice::DeletePermanent;
        else if (rc == wxID_NO) reply.trashFailChoice = TrashFailChoice::Skip;
        else reply.trashFailChoice = TrashFailChoice::Cancel;
      } else if (prompt->kind == AsyncFileOpPrompt::Kind::Error) {
        wxMessageDialog dlg(this,
                            wxString::Format("Delete failed:\n\n%s\n\nContinue?",
                                             wxString::FromUTF8(prompt->errorMessage).c_str()),
                            "Delete failed",
                            wxYES_NO | wxNO_DEFAULT | wxICON_ERROR);
        dlg.SetYesNoLabels("Continue", "Cancel");
        reply.continueAfterError = (dlg.ShowModal() == wxID_YES);
      }

      {
        std::lock_guard<std::mutex> lock(state->mu);
        state->reply = reply;
        state->cv.notify_all();
      }
      fileOp_->promptActive = false;
    }

    const auto done = state->done.load();

    wxString label;
    bool finished = false;
    const bool canceling = state->cancelRequested.load();
    {
      std::lock_guard<std::mutex> lock(state->mu);
      label = wxString::FromUTF8(state->currentLabel);
      finished = state->finished;
    }

    if (!fileOp_->configured) {
      fileOp_->range = static_cast<int>(std::max<size_t>(1, sources.size()));
      fileOp_->gauge->SetRange(fileOp_->range);
      fileOp_->configured = true;
    }

    fileOp_->gauge->SetValue(std::min<int>(fileOp_->range, static_cast<int>(done)));
    if (canceling) fileOp_->titleText->SetLabel("Canceling...");

    fileOp_->detailText->SetLabel(
        wxString::Format("%s\n%zu / %zu", label.c_str(), done, sources.size()));
    if (fileOp_->dlg) fileOp_->dlg->Layout();

    if (finished) {
      fileOp_->timer.Stop();
      if (fileOp_->worker.joinable()) fileOp_->worker.join();
      if (top_) top_->RefreshAll();
      if (bottom_) bottom_->RefreshAll();
      if (state->hasDir) {
        if (top_) top_->RefreshTree();
        if (bottom_) bottom_->RefreshTree();
      }
      if (fileOp_->dlg) fileOp_->dlg->Destroy();
      fileOp_.reset();
      StartNextQueuedOp();
    }
  }, fileOp_->timerId);

  fileOp_->timer.Start(100);
}

void MainFrame::DeleteWithProgress(const std::vector<std::filesystem::path>& sources) {
  DeleteWithProgressInternal(sources, /*alreadyConfirmed=*/false);
}

void MainFrame::DeleteWithProgressInternal(const std::vector<std::filesystem::path>& sources,
                                           bool alreadyConfirmed) {
  if (sources.empty()) return;

  if (!alreadyConfirmed) {
    const auto message = wxString::Format(
        "Permanently delete %zu item(s)?\n\nThis cannot be undone.", sources.size());
    if (wxMessageBox(message, "Delete", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) {
      return;
    }
  }

  if (fileOp_) {
    EnqueueOp(QueuedOp{.kind = OpKind::Delete, .title = "Delete", .sources = sources});
    return;
  }

  fileOp_ = std::make_unique<FileOpSession>(this);
  fileOp_->state = std::make_shared<AsyncFileOpState>();

  bool hasDir = false;
  for (const auto& p : sources) {
    if (IsDirectoryAny(p)) {
      hasDir = true;
      break;
    }
  }
  fileOp_->state->hasDir = hasDir;
  {
    std::lock_guard<std::mutex> lock(fileOp_->state->mu);
    fileOp_->state->scanDone = true;
    fileOp_->state->totalBytes = 0;
    fileOp_->state->currentLabel = "Preparing...";
  }

  fileOp_->dlg = new wxDialog(this, wxID_ANY, "Delete", wxDefaultPosition, wxSize(650, 340),
                              wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxFRAME_FLOAT_ON_PARENT);
  auto* root = new wxBoxSizer(wxVERTICAL);
  fileOp_->dlg->SetSizer(root);

  fileOp_->titleText = new wxStaticText(fileOp_->dlg, wxID_ANY, "Deleting...");
  fileOp_->detailText = new wxStaticText(fileOp_->dlg, wxID_ANY, "Preparing...");
  fileOp_->gauge = new wxGauge(fileOp_->dlg, wxID_ANY, fileOp_->range);
  fileOp_->cancelBtn = new wxButton(fileOp_->dlg, wxID_CANCEL, "Cancel");

  root->Add(fileOp_->titleText, 0, wxEXPAND | wxALL, 10);
  root->Add(fileOp_->detailText, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
  root->Add(fileOp_->gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

  auto* queueSizer = new wxStaticBoxSizer(wxVERTICAL, fileOp_->dlg, "Queue");
  fileOp_->queueBox = queueSizer->GetStaticBox();
  fileOp_->queueScroll = new wxScrolledWindow(fileOp_->dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                              wxVSCROLL);
  fileOp_->queueScroll->SetScrollRate(0, 10);
  fileOp_->queueItemsSizer = new wxBoxSizer(wxVERTICAL);
  fileOp_->queueScroll->SetSizer(fileOp_->queueItemsSizer);
  queueSizer->Add(fileOp_->queueScroll, 1, wxEXPAND | wxALL, 8);

  auto* qBtns = new wxBoxSizer(wxHORIZONTAL);
  qBtns->AddStretchSpacer(1);
  fileOp_->clearQueueBtn = new wxButton(fileOp_->dlg, wxID_ANY, "Clear Queue");
  qBtns->Add(fileOp_->clearQueueBtn, 0);
  queueSizer->Add(qBtns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

  root->Add(queueSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

  auto* btns = new wxBoxSizer(wxHORIZONTAL);
  btns->AddStretchSpacer(1);
  btns->Add(fileOp_->cancelBtn, 0);
  root->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
  fileOp_->dlg->Layout();
  fileOp_->dlg->Show();

  fileOp_->clearQueueBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    opQueue_.clear();
    UpdateQueueUi();
  });
  UpdateQueueUi();

  fileOp_->timerId = wxWindow::NewControlId();
  fileOp_->timer.SetOwner(fileOp_->dlg, fileOp_->timerId);

  fileOp_->cancelBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    if (!fileOp_ || !fileOp_->state) return;
    fileOp_->state->cancelRequested.store(true);
    fileOp_->cancelBtn->Disable();
    std::lock_guard<std::mutex> lock(fileOp_->state->mu);
    fileOp_->state->cv.notify_all();
  });
  fileOp_->dlg->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) { e.Veto(); });

  const auto state = fileOp_->state;
  fileOp_->worker = std::thread([state, sources]() {
    for (const auto& src : sources) {
      if (state->cancelRequested.load()) break;
      if (src.empty()) {
        state->done.fetch_add(1);
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(state->mu);
        const auto fn = src.filename().string();
        state->currentLabel = fn.empty() ? src.string() : fn;
      }

      const auto result = DeletePath(src);
      if (!result.ok) {
        std::unique_lock<std::mutex> lock(state->mu);
        state->prompt = AsyncFileOpPrompt{.kind = AsyncFileOpPrompt::Kind::Error,
                                          .src = src,
                                          .errorMessage = result.message.ToStdString()};
        state->reply.reset();
        state->cv.notify_all();
        state->cv.wait(lock, [state]() { return state->reply.has_value() || state->cancelRequested.load(); });
        if (state->cancelRequested.load()) break;

        const auto reply = *state->reply;
        state->prompt.reset();
        state->reply.reset();
        if (!reply.continueAfterError) {
          state->cancelRequested.store(true);
          break;
        }
      }

      state->done.fetch_add(1);
    }

    std::lock_guard<std::mutex> lock(state->mu);
    state->finished = true;
    state->cv.notify_all();
  });

  fileOp_->dlg->Bind(wxEVT_TIMER, [this, sources](wxTimerEvent&) {
    if (!fileOp_ || !fileOp_->state) return;
    const auto state = fileOp_->state;

    std::optional<AsyncFileOpPrompt> prompt;
    {
      std::lock_guard<std::mutex> lock(state->mu);
      prompt = state->prompt;
    }
    if (prompt && !fileOp_->promptActive) {
      fileOp_->promptActive = true;
      AsyncFileOpReply reply;
      if (prompt->kind == AsyncFileOpPrompt::Kind::Error) {
        wxMessageDialog dlg(this,
                            wxString::Format("Delete failed:\n\n%s\n\nContinue?",
                                             wxString::FromUTF8(prompt->errorMessage).c_str()),
                            "Delete failed",
                            wxYES_NO | wxNO_DEFAULT | wxICON_ERROR);
        dlg.SetYesNoLabels("Continue", "Cancel");
        reply.continueAfterError = (dlg.ShowModal() == wxID_YES);
      }
      {
        std::lock_guard<std::mutex> lock(state->mu);
        state->reply = reply;
        state->cv.notify_all();
      }
      fileOp_->promptActive = false;
    }

    const auto done = state->done.load();
    wxString label;
    bool finished = false;
    const bool canceling = state->cancelRequested.load();
    {
      std::lock_guard<std::mutex> lock(state->mu);
      label = wxString::FromUTF8(state->currentLabel);
      finished = state->finished;
    }

    if (!fileOp_->configured) {
      fileOp_->range = static_cast<int>(std::max<size_t>(1, sources.size()));
      fileOp_->gauge->SetRange(fileOp_->range);
      fileOp_->configured = true;
    }

    fileOp_->gauge->SetValue(std::min<int>(fileOp_->range, static_cast<int>(done)));
    if (canceling) fileOp_->titleText->SetLabel("Canceling...");

    fileOp_->detailText->SetLabel(
        wxString::Format("%s\n%zu / %zu", label.c_str(), done, sources.size()));
    if (fileOp_->dlg) fileOp_->dlg->Layout();

    if (finished) {
      fileOp_->timer.Stop();
      if (fileOp_->worker.joinable()) fileOp_->worker.join();
      if (top_) top_->RefreshAll();
      if (bottom_) bottom_->RefreshAll();
      if (state->hasDir) {
        if (top_) top_->RefreshTree();
        if (bottom_) bottom_->RefreshTree();
      }
      if (fileOp_->dlg) fileOp_->dlg->Destroy();
      fileOp_.reset();
      StartNextQueuedOp();
    }
  }, fileOp_->timerId);

  fileOp_->timer.Start(100);
}

void MainFrame::ExtractWithProgress(const std::vector<std::string>& argv,
                                    const std::filesystem::path& refreshDir,
                                    bool treeChanged) {
  if (argv.empty()) return;
  if (fileOp_) {
    EnqueueOp(QueuedOp{.kind = OpKind::Extract,
                       .title = "Extract",
                       .argv = argv,
                       .refreshDir = refreshDir,
                       .treeChanged = treeChanged});
    return;
  }

  fileOp_ = std::make_unique<FileOpSession>(this);
  fileOp_->state = std::make_shared<AsyncFileOpState>();
  fileOp_->state->hasDir = treeChanged;
  {
    std::lock_guard<std::mutex> lock(fileOp_->state->mu);
    fileOp_->state->scanDone = true;
    fileOp_->state->totalBytes = 0;
    fileOp_->state->currentLabel = "Preparing...";
  }

  fileOp_->dlg = new wxDialog(this, wxID_ANY, "Extract", wxDefaultPosition, wxSize(650, 340),
                              wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxFRAME_FLOAT_ON_PARENT);
  auto* root = new wxBoxSizer(wxVERTICAL);
  fileOp_->dlg->SetSizer(root);

  fileOp_->titleText = new wxStaticText(fileOp_->dlg, wxID_ANY, "Extracting...");
  fileOp_->detailText = new wxStaticText(fileOp_->dlg, wxID_ANY, "Preparing...");
  fileOp_->gauge = new wxGauge(fileOp_->dlg, wxID_ANY, 100);
  fileOp_->cancelBtn = new wxButton(fileOp_->dlg, wxID_CANCEL, "Cancel");

  root->Add(fileOp_->titleText, 0, wxEXPAND | wxALL, 10);
  root->Add(fileOp_->detailText, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
  root->Add(fileOp_->gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

  auto* queueSizer = new wxStaticBoxSizer(wxVERTICAL, fileOp_->dlg, "Queue");
  fileOp_->queueBox = queueSizer->GetStaticBox();
  fileOp_->queueScroll = new wxScrolledWindow(fileOp_->dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                              wxVSCROLL);
  fileOp_->queueScroll->SetScrollRate(0, 10);
  fileOp_->queueItemsSizer = new wxBoxSizer(wxVERTICAL);
  fileOp_->queueScroll->SetSizer(fileOp_->queueItemsSizer);
  queueSizer->Add(fileOp_->queueScroll, 1, wxEXPAND | wxALL, 8);

  auto* qBtns = new wxBoxSizer(wxHORIZONTAL);
  qBtns->AddStretchSpacer(1);
  fileOp_->clearQueueBtn = new wxButton(fileOp_->dlg, wxID_ANY, "Clear Queue");
  qBtns->Add(fileOp_->clearQueueBtn, 0);
  queueSizer->Add(qBtns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

  root->Add(queueSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

  auto* btns = new wxBoxSizer(wxHORIZONTAL);
  btns->AddStretchSpacer(1);
  btns->Add(fileOp_->cancelBtn, 0);
  root->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
  fileOp_->dlg->Layout();
  fileOp_->dlg->Show();

  fileOp_->clearQueueBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    opQueue_.clear();
    UpdateQueueUi();
  });
  UpdateQueueUi();

  fileOp_->timerId = wxWindow::NewControlId();
  fileOp_->timer.SetOwner(fileOp_->dlg, fileOp_->timerId);

  fileOp_->cancelBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    if (!fileOp_ || !fileOp_->state) return;
    fileOp_->state->cancelRequested.store(true);
    fileOp_->cancelBtn->Disable();
  });
  fileOp_->dlg->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) { e.Veto(); });

  const auto state = fileOp_->state;
  fileOp_->worker = std::thread([state, argv]() {
    std::vector<wxString> argsWx;
    argsWx.reserve(argv.size());
    for (const auto& s : argv) argsWx.push_back(wxString::FromUTF8(s));

    {
      std::lock_guard<std::mutex> lock(state->mu);
      state->currentLabel = argv[0];
    }

    std::vector<const wxChar*> cargv;
    cargv.reserve(argsWx.size() + 1);
    for (const auto& s : argsWx) cargv.push_back(s.wc_str());
    cargv.push_back(nullptr);

    const long rc = wxExecute(cargv.data(), wxEXEC_SYNC);
    if (rc != 0) {
      std::unique_lock<std::mutex> lock(state->mu);
      state->prompt = AsyncFileOpPrompt{.kind = AsyncFileOpPrompt::Kind::Error,
                                        .errorMessage = "Extractor failed (exit code " +
                                                        std::to_string(rc) + ")."};
      state->reply.reset();
      state->cv.notify_all();
      state->cv.wait(lock, [state]() { return state->reply.has_value() || state->cancelRequested.load(); });
      state->prompt.reset();
      state->reply.reset();
    }
    state->done.store(1);

    std::lock_guard<std::mutex> lock(state->mu);
    state->finished = true;
    state->cv.notify_all();
  });

  fileOp_->dlg->Bind(wxEVT_TIMER, [this, refreshDir](wxTimerEvent&) {
    if (!fileOp_ || !fileOp_->state) return;
    const auto state = fileOp_->state;

    std::optional<AsyncFileOpPrompt> prompt;
    {
      std::lock_guard<std::mutex> lock(state->mu);
      prompt = state->prompt;
    }
    if (prompt && !fileOp_->promptActive) {
      fileOp_->promptActive = true;
      AsyncFileOpReply reply;
      wxMessageBox(wxString::FromUTF8(prompt->errorMessage), "Extract", wxOK | wxICON_ERROR, this);
      reply.continueAfterError = true;
      {
        std::lock_guard<std::mutex> lock(state->mu);
        state->reply = reply;
        state->cv.notify_all();
      }
      fileOp_->promptActive = false;
    }

    wxString label;
    bool finished = false;
    {
      std::lock_guard<std::mutex> lock(state->mu);
      label = wxString::FromUTF8(state->currentLabel);
      finished = state->finished;
    }

    fileOp_->gauge->Pulse();
    fileOp_->detailText->SetLabel(label);
    if (fileOp_->dlg) fileOp_->dlg->Layout();

    if (finished) {
      fileOp_->timer.Stop();
      if (fileOp_->worker.joinable()) fileOp_->worker.join();
      if (top_ && (refreshDir.empty() || top_->GetDirectoryPath() == refreshDir)) top_->RefreshAll();
      if (bottom_ && (refreshDir.empty() || bottom_->GetDirectoryPath() == refreshDir)) bottom_->RefreshAll();
      if (state->hasDir) {
        if (top_) top_->RefreshTree();
        if (bottom_) bottom_->RefreshTree();
      }
      if (fileOp_->dlg) fileOp_->dlg->Destroy();
      fileOp_.reset();
      StartNextQueuedOp();
    }
  }, fileOp_->timerId);

  fileOp_->timer.Start(100);
}

void MainFrame::EnqueueOp(QueuedOp op) {
  if (op.id == 0) op.id = nextOpId_++;
  opQueue_.push_back(std::move(op));
  UpdateQueueUi();
}

void MainFrame::StartNextQueuedOp() {
  if (fileOp_) return;
  if (opQueue_.empty()) return;

  QueuedOp op = std::move(opQueue_.front());
  opQueue_.pop_front();
  switch (op.kind) {
    case OpKind::CopyMove:
      CopyMoveWithProgressInternal(op.title, op.sources, op.dstDir, op.move, /*alreadyConfirmed=*/true);
      break;
    case OpKind::Trash:
      TrashWithProgressInternal(op.sources, /*alreadyConfirmed=*/true);
      break;
    case OpKind::Delete:
      DeleteWithProgressInternal(op.sources, /*alreadyConfirmed=*/true);
      break;
    case OpKind::Extract:
      ExtractWithProgress(op.argv, op.refreshDir, op.treeChanged);
      break;
  }
}

void MainFrame::SetActivePane(ActivePane pane) {
  activePane_ = pane;
  if (top_) top_->SetActiveVisual(pane == ActivePane::Top);
  if (bottom_) bottom_->SetActiveVisual(pane == ActivePane::Bottom);
}

void MainFrame::RefreshPanelsShowing(const std::filesystem::path& dir, bool treeChanged) {
  if (top_ && top_->GetDirectoryPath() == dir) top_->RefreshAll();
  if (bottom_ && bottom_->GetDirectoryPath() == dir) bottom_->RefreshAll();

  // If directory structure changed (mkdir/rmdir/rename-dir/etc.), refresh both
  // trees so they stay consistent even when panes are in different folders.
  if (treeChanged) {
    if (top_) top_->RefreshTree();
    if (bottom_) bottom_->RefreshTree();
  }
}

FilePanel* MainFrame::GetActivePanel() const {
  return activePane_ == ActivePane::Top ? top_.get() : bottom_.get();
}

FilePanel* MainFrame::GetInactivePanel() const {
  return activePane_ == ActivePane::Top ? bottom_.get() : top_.get();
}

void MainFrame::OnQuit(wxCommandEvent&) { Close(true); }

void MainFrame::OnAbout(wxCommandEvent&) {
#ifdef QUARRY_VERSION
  const wxString version = QUARRY_VERSION;
#else
  const wxString version = "dev";
#endif

  wxAboutDialogInfo info;
  info.SetName("Quarry");
  info.SetVersion(version);
  info.SetDescription("Dual-pane file manager.");
  wxAboutBox(info, this);
}

void MainFrame::OnPreferences(wxCommandEvent&) {
  wxConfig cfg("Quarry");
  bool restoreLast = false;
  cfg.Read("/prefs/startup/restore_last", &restoreLast, false);

  wxDialog dlg(this, wxID_ANY, "Preferences", wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto* root = new wxBoxSizer(wxVERTICAL);
  dlg.SetSizer(root);

  wxArrayString choices;
  choices.Add("Load Default View on startup (use View → Save View as Default)");
  choices.Add("Restore last used view on startup (remember automatically)");
  auto* startupMode = new wxRadioBox(&dlg,
                                     wxID_ANY,
                                     "Startup",
                                     wxDefaultPosition,
                                     wxDefaultSize,
                                     choices,
                                     1,
                                     wxRA_SPECIFY_ROWS);
  startupMode->SetSelection(restoreLast ? 1 : 0);
  root->Add(startupMode, 0, wxALL | wxEXPAND, 10);

  auto* btns = dlg.CreateButtonSizer(wxOK | wxCANCEL);
  root->Add(btns, 0, wxALL | wxEXPAND, 10);

  dlg.Fit();
  dlg.Layout();
  dlg.CentreOnParent();

  if (dlg.ShowModal() != wxID_OK) return;

  const bool newRestoreLast = (startupMode->GetSelection() == 1);
  cfg.Write("/prefs/startup/restore_last", newRestoreLast);
  cfg.Flush();

  if (newRestoreLast) {
    SaveLastView(/*showMessage=*/false);
  }
}

void MainFrame::OnRefresh(wxCommandEvent&) {
  InitPanelsIfNeeded();
  if (top_) top_->RefreshListing();
  if (bottom_) bottom_->RefreshListing();
}

void MainFrame::OnConnectToServer(wxCommandEvent&) {
  InitPanelsIfNeeded();
  if (!top_ || !bottom_) return;
  const auto params = ShowConnectDialog(this);
  if (!params) return;

  const auto uri = BuildConnectUri(*params);

  // Seed creds for this instance so mount/list can proceed without extra prompts.
  if (!params->username.empty() || !params->password.empty()) {
    GetActivePanel()->SeedMountCredentials(uri,
                                          params->username,
                                          params->password,
                                          /*rememberForever=*/params->rememberPassword);
  }

  if (auto* active = GetActivePanel()) {
    active->SetDirectory(uri);
    active->FocusPrimary();
  }

  // Keep both sidebars in sync (e.g., Network group / recent hosts).
  if (top_) top_->RefreshTree();
  if (bottom_) bottom_->RefreshTree();
}

void MainFrame::OnConnectionsManager(wxCommandEvent&) {
  InitPanelsIfNeeded();
  if (!top_ || !bottom_) return;

  wxDialog dlg(this, wxID_ANY, "Connections", wxDefaultPosition, wxSize(760, 420),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto* root = new wxBoxSizer(wxVERTICAL);
  dlg.SetSizer(root);

  auto* split = new wxSplitterWindow(&dlg, wxID_ANY);
  split->SetSashGravity(0.0);
  split->SetMinimumPaneSize(220);

  auto* left = new wxPanel(split);
  auto* right = new wxPanel(split);
  split->SplitVertically(left, right, 260);
  root->Add(split, 1, wxEXPAND | wxALL, 10);

  auto* leftSizer = new wxBoxSizer(wxVERTICAL);
  left->SetSizer(leftSizer);
  auto* list = new wxListBox(left, wxID_ANY);
  leftSizer->Add(list, 1, wxEXPAND);

  auto* rightSizer = new wxBoxSizer(wxVERTICAL);
  right->SetSizer(rightSizer);

  auto* form = new wxFlexGridSizer(2, 8, 8);
  form->AddGrowableCol(1, 1);
  rightSizer->Add(form, 0, wxEXPAND | wxALL, 8);

  auto* nameCtrl = new wxTextCtrl(right, wxID_ANY);
  wxArrayString types;
  types.Add("SMB (Windows Share)");
  types.Add("SSH (SFTP)");
  types.Add("FTP");
  types.Add("WebDAV");
  types.Add("WebDAV (HTTPS)");
  types.Add("AFP");
  auto* typeCtrl = new wxChoice(right, wxID_ANY, wxDefaultPosition, wxDefaultSize, types);
  typeCtrl->SetSelection(0);
  auto* serverCtrl = new wxTextCtrl(right, wxID_ANY);
  auto* portCtrl = new wxSpinCtrl(right, wxID_ANY);
  portCtrl->SetRange(0, 65535);
  portCtrl->SetValue(0);
  auto* folderCtrl = new wxTextCtrl(right, wxID_ANY);
  auto* userCtrl = new wxTextCtrl(right, wxID_ANY);
  auto* passCtrl = new wxTextCtrl(right, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
  auto* rememberCtrl = new wxCheckBox(right, wxID_ANY, "Remember this password");
  rememberCtrl->SetValue(false);

  form->Add(new wxStaticText(right, wxID_ANY, "Name:"), 0, wxALIGN_CENTER_VERTICAL);
  form->Add(nameCtrl, 1, wxEXPAND);
  form->Add(new wxStaticText(right, wxID_ANY, "Type:"), 0, wxALIGN_CENTER_VERTICAL);
  form->Add(typeCtrl, 1, wxEXPAND);
  form->Add(new wxStaticText(right, wxID_ANY, "Server:"), 0, wxALIGN_CENTER_VERTICAL);
  form->Add(serverCtrl, 1, wxEXPAND);
  form->Add(new wxStaticText(right, wxID_ANY, "Port:"), 0, wxALIGN_CENTER_VERTICAL);
  form->Add(portCtrl, 1, wxEXPAND);
  form->Add(new wxStaticText(right, wxID_ANY, "Folder:"), 0, wxALIGN_CENTER_VERTICAL);
  form->Add(folderCtrl, 1, wxEXPAND);
  form->Add(new wxStaticText(right, wxID_ANY, "User name:"), 0, wxALIGN_CENTER_VERTICAL);
  form->Add(userCtrl, 1, wxEXPAND);
  form->Add(new wxStaticText(right, wxID_ANY, "Password:"), 0, wxALIGN_CENTER_VERTICAL);
  form->Add(passCtrl, 1, wxEXPAND);
  rightSizer->Add(rememberCtrl, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

  auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
  auto* newBtn = new wxButton(&dlg, wxID_ANY, "New");
  auto* delBtn = new wxButton(&dlg, wxID_ANY, "Delete");
  auto* saveBtn = new wxButton(&dlg, wxID_ANY, "Save Changes");
  auto* connectBtn = new wxButton(&dlg, wxID_ANY, "Connect");
  auto* closeBtn = new wxButton(&dlg, wxID_CLOSE, "Close");
  btnRow->Add(newBtn, 0, wxRIGHT, 8);
  btnRow->Add(delBtn, 0, wxRIGHT, 16);
  btnRow->Add(saveBtn, 0, wxRIGHT, 8);
  btnRow->Add(connectBtn, 0, wxRIGHT, 8);
  btnRow->AddStretchSpacer(1);
  btnRow->Add(closeBtn, 0);
  root->Add(btnRow, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  auto conns = connections::LoadAll();
  std::vector<std::string> ids;
  ids.reserve(conns.size());
  list->Clear();
  for (const auto& c : conns) {
    ids.push_back(c.id);
    list->Append(wxString::FromUTF8(c.name));
  }

  auto typeIndexFromType = [](connections::Type t) -> int {
    switch (t) {
      case connections::Type::SMB: return 0;
      case connections::Type::SSH: return 1;
      case connections::Type::FTP: return 2;
      case connections::Type::WebDAV: return 3;
      case connections::Type::WebDAVS: return 4;
      case connections::Type::AFP: return 5;
      default: return 0;
    }
  };
  auto typeFromIndex = [](int sel) -> connections::Type {
    switch (sel) {
      case 0: return connections::Type::SMB;
      case 1: return connections::Type::SSH;
      case 2: return connections::Type::FTP;
      case 3: return connections::Type::WebDAV;
      case 4: return connections::Type::WebDAVS;
      case 5: return connections::Type::AFP;
      default: return connections::Type::Unknown;
    }
  };

  auto defaultPortForSelection = [](int sel) -> int {
    switch (sel) {
      case 1: return 22;
      case 2: return 21;
      case 3: return 80;
      case 4: return 443;
      default: return 0;
    }
  };
  bool portTouched = false;
  portCtrl->Bind(wxEVT_SPINCTRL, [&portTouched](wxSpinEvent&) { portTouched = true; });
  portCtrl->Bind(wxEVT_TEXT, [&portTouched](wxCommandEvent&) { portTouched = true; });
  typeCtrl->Bind(wxEVT_CHOICE, [&](wxCommandEvent&) {
    const int sel = typeCtrl->GetSelection();
    const int def = defaultPortForSelection(sel);
    if (!portTouched || portCtrl->GetValue() == 0) {
      portCtrl->SetValue(def);
      portTouched = false;
    }
  });

  auto loadToForm = [&](const connections::Connection& c) {
    nameCtrl->ChangeValue(wxString::FromUTF8(c.name));
    typeCtrl->SetSelection(typeIndexFromType(c.type));
    serverCtrl->ChangeValue(wxString::FromUTF8(c.server));
    portCtrl->SetValue(c.port);
    folderCtrl->ChangeValue(wxString::FromUTF8(c.folder));
    userCtrl->ChangeValue(wxString::FromUTF8(c.username));
    passCtrl->ChangeValue("");
    rememberCtrl->SetValue(c.rememberPassword);
    portTouched = false;
  };

  auto currentId = std::string{};
  list->Bind(wxEVT_LISTBOX, [&](wxCommandEvent&) {
    const int sel = list->GetSelection();
    if (sel == wxNOT_FOUND) return;
    currentId = ids[static_cast<size_t>(sel)];
    const auto it = std::find_if(conns.begin(), conns.end(), [&](const auto& c) { return c.id == currentId; });
    if (it != conns.end()) loadToForm(*it);
  });

  auto gatherFromForm = [&]() -> connections::Connection {
    connections::Connection c;
    c.id = currentId;
    c.name = nameCtrl->GetValue().ToStdString();
    c.type = typeFromIndex(typeCtrl->GetSelection());
    c.server = serverCtrl->GetValue().ToStdString();
    c.port = portCtrl->GetValue();
    c.folder = folderCtrl->GetValue().ToStdString();
    c.username = userCtrl->GetValue().ToStdString();
    c.rememberPassword = rememberCtrl->GetValue();
    return c;
  };

  auto refreshList = [&]() {
    conns = connections::LoadAll();
    ids.clear();
    ids.reserve(conns.size());
    list->Clear();
    for (const auto& c : conns) {
      ids.push_back(c.id);
      list->Append(wxString::FromUTF8(c.name));
    }
  };

  newBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    currentId.clear();
    nameCtrl->ChangeValue("New Connection");
    typeCtrl->SetSelection(0);
    serverCtrl->ChangeValue("");
    portCtrl->SetValue(0);
    folderCtrl->ChangeValue("");
    userCtrl->ChangeValue("");
    passCtrl->ChangeValue("");
    rememberCtrl->SetValue(false);
    portTouched = false;
  });

  delBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    if (currentId.empty()) return;
    connections::Remove(currentId);
    currentId.clear();
    refreshList();
  });

  saveBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    auto c = gatherFromForm();
    if (c.name.empty() || c.server.empty()) {
      wxMessageBox("Name and server are required.", "Quarry", wxOK | wxICON_INFORMATION, &dlg);
      return;
    }
    currentId = connections::Upsert(std::move(c));
    refreshList();
  });

  connectBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    auto c = gatherFromForm();
    if (c.server.empty()) return;
    const auto uri = connections::BuildUri(c);
    if (!c.username.empty() || !passCtrl->GetValue().IsEmpty()) {
      GetActivePanel()->SeedMountCredentials(uri,
                                            c.username,
                                            passCtrl->GetValue().ToStdString(),
                                            /*rememberForever=*/rememberCtrl->GetValue());
    }
    GetActivePanel()->SetDirectory(uri);
    GetActivePanel()->FocusPrimary();
  });

  closeBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dlg.EndModal(wxID_CLOSE); });

  if (!conns.empty()) {
    list->SetSelection(0);
    currentId = ids[0];
    loadToForm(conns[0]);
  }

  dlg.Layout();
  dlg.CentreOnParent();
  dlg.ShowModal();
}

void MainFrame::OnCopy(wxCommandEvent&) {
  InitPanelsIfNeeded();
  if (!top_ || !bottom_) return;
  auto* from = GetActivePanel();
  auto* to = GetInactivePanel();

  const auto sources = from->GetSelectedPaths();
  if (sources.empty()) return;

  const auto dstDir = to->GetDirectoryPath();
  CopyMoveWithProgress("Copy", sources, dstDir, /*move=*/false);
}

void MainFrame::OnMove(wxCommandEvent&) {
  InitPanelsIfNeeded();
  if (!top_ || !bottom_) return;
  auto* from = GetActivePanel();
  auto* to = GetInactivePanel();

  const auto sources = from->GetSelectedPaths();
  if (sources.empty()) return;

  const auto dstDir = to->GetDirectoryPath();
  CopyMoveWithProgress("Move", sources, dstDir, /*move=*/true);
}

void MainFrame::OnDelete(wxCommandEvent&) {
  InitPanelsIfNeeded();
  if (!top_ || !bottom_) return;
  auto* from = GetActivePanel();
  const auto sources = from->GetSelectedPaths();
  if (sources.empty()) return;
  StartTrashOperation(sources);
}

void MainFrame::OnDeletePermanent(wxCommandEvent&) {
  InitPanelsIfNeeded();
  if (!top_ || !bottom_) return;
  auto* from = GetActivePanel();
  const auto sources = from->GetSelectedPaths();
  if (sources.empty()) return;
  StartDeleteOperation(sources);
}

void MainFrame::OnRename(wxCommandEvent&) {
  InitPanelsIfNeeded();
  if (!top_ || !bottom_) return;
  GetActivePanel()->BeginInlineRename();
}

void MainFrame::OnMkDir(wxCommandEvent&) {
  InitPanelsIfNeeded();
  if (!top_ || !bottom_) return;
  GetActivePanel()->CreateFolder();
}
