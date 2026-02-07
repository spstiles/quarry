#include "MainFrame.h"

#include "util.h"

#include <filesystem>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include <wx/accel.h>
#include <wx/choicdlg.h>
#include <wx/filefn.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/textdlg.h>
#include <wx/textctrl.h>
#include <wx/choice.h>
#include <wx/checkbox.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

namespace {
enum MenuId : int {
  ID_Refresh = wxID_HIGHEST + 1,
  ID_ConnectToServer,
  ID_Copy,
  ID_Move,
  ID_Trash,
  ID_DeletePermanent,
  ID_Rename,
  ID_MkDir
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

struct AsyncFileOpPrompt {
  enum class Kind { Exists, Error };
  Kind kind{Kind::Exists};
  std::filesystem::path dst{};
  std::string errorMessage{};
};

struct AsyncFileOpReply {
  // For Kind::Exists
  ExistsChoice existsChoice{ExistsChoice::Cancel};
  std::optional<std::string> renameTo{};

  // For Kind::Error
  bool continueAfterError{false};
};

struct AsyncFileOpState {
  std::mutex mu{};
  std::condition_variable cv{};

  std::atomic<bool> cancelRequested{false};
  std::atomic<size_t> done{0};

  bool finished{false};
  bool hasDir{false};
  std::string currentLabel{};

  std::optional<AsyncFileOpPrompt> prompt{};
  std::optional<AsyncFileOpReply> reply{};
};

bool LooksLikeUriPath(const std::filesystem::path& p) {
  const auto s = p.string();
  return s.find("://") != std::string::npos;
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
  auto* btnSizer = dlg.CreateButtonSizer(wxOK | wxCANCEL);
  root->Add(btnSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  dlg.Fit();
  dlg.Layout();
  dlg.CentreOnParent();

  if (dlg.ShowModal() != wxID_OK) return std::nullopt;

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

  // Basic validation.
  if (out.server.empty()) return std::nullopt;
  return out;
}
} // namespace

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "Quarry", wxDefaultPosition, wxSize(1200, 700)) {
  BuildMenu();
  BuildLayout();
  BindEvents();

  const auto home = wxGetHomeDir().ToStdString();
  top_->SetDirectory(home);
  bottom_->SetDirectory(home);

  SetMinSize(wxSize(900, 500));

  SetActivePane(ActivePane::Top);
}

void MainFrame::BuildMenu() {
  auto* fileMenu = new wxMenu();
  fileMenu->Append(ID_ConnectToServer, "Connect to Server...\tCtrl+L");
  fileMenu->AppendSeparator();
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

  auto* bar = new wxMenuBar();
  bar->Append(fileMenu, "&File");
  bar->Append(opsMenu, "&Operations");
  SetMenuBar(bar);
}

void MainFrame::BuildLayout() {
  splitter_ = new wxSplitterWindow(this, wxID_ANY);
  splitter_->SetSashGravity(0.5);
  splitter_->SetMinimumPaneSize(260);

  top_ = new FilePanel(splitter_);
  bottom_ = new FilePanel(splitter_);
  splitter_->SplitHorizontally(top_, bottom_);

  auto* sizer = new wxBoxSizer(wxVERTICAL);
  sizer->Add(splitter_, 1, wxEXPAND);
  SetSizer(sizer);
}

void MainFrame::BindEvents() {
  Bind(wxEVT_MENU, &MainFrame::OnQuit, this, wxID_EXIT);
  Bind(wxEVT_MENU, &MainFrame::OnRefresh, this, ID_Refresh);
  Bind(wxEVT_MENU, &MainFrame::OnConnectToServer, this, ID_ConnectToServer);
  Bind(wxEVT_MENU, &MainFrame::OnCopy, this, ID_Copy);
  Bind(wxEVT_MENU, &MainFrame::OnMove, this, ID_Move);
  Bind(wxEVT_MENU, &MainFrame::OnDelete, this, ID_Trash);
  Bind(wxEVT_MENU, &MainFrame::OnDeletePermanent, this, ID_DeletePermanent);
  Bind(wxEVT_MENU, &MainFrame::OnRename, this, ID_Rename);
  Bind(wxEVT_MENU, &MainFrame::OnMkDir, this, ID_MkDir);

  top_->BindFocusEvents([this]() { SetActivePane(ActivePane::Top); });
  bottom_->BindFocusEvents([this]() { SetActivePane(ActivePane::Bottom); });

  top_->BindDropFiles([this](const std::vector<std::filesystem::path>& paths, bool move) {
    TransferDroppedPaths(top_, paths, move);
  });
  bottom_->BindDropFiles([this](const std::vector<std::filesystem::path>& paths, bool move) {
    TransferDroppedPaths(bottom_, paths, move);
  });

  auto onDirChanged = [this](const std::filesystem::path& dir, bool treeChanged) {
    RefreshPanelsShowing(dir, treeChanged);
  };
  top_->BindDirContentsChanged(onDirChanged);
  bottom_->BindDirContentsChanged(onDirChanged);

  // Key navigation that should work regardless of which child has focus.
  Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
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
      GetActivePanel()->FocusPrimary();
      return;
    }
    if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
      GetActivePanel()->OpenSelection();
      return;
    }
    if (key == WXK_BACK) {
      GetActivePanel()->NavigateUp();
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

void MainFrame::TransferDroppedPaths(FilePanel* target,
                                     const std::vector<std::filesystem::path>& sources,
                                     bool move) {
  if (!target) return;
  if (sources.empty()) return;

  const auto dstDir = target->GetDirectoryPath();
  std::error_code ec;
  if (dstDir.empty() || !std::filesystem::exists(dstDir, ec) || !std::filesystem::is_directory(dstDir, ec)) {
    wxMessageBox("Drop target is not a local directory.", "Quarry", wxOK | wxICON_WARNING, this);
    return;
  }

  // Validate sources are local filesystem paths.
  for (const auto& p : sources) {
    if (p.empty()) return;
    const auto s = p.string();
    if (s.find("://") != std::string::npos) {
      wxMessageBox("Drag-and-drop from remote locations is not supported yet.", "Quarry",
                   wxOK | wxICON_INFORMATION, this);
      return;
    }
  }

  CopyMoveWithProgress(move ? "Move" : "Copy", sources, dstDir, move);
}

void MainFrame::CopyMoveWithProgress(const wxString& title,
                                     const std::vector<std::filesystem::path>& sources,
                                     const std::filesystem::path& dstDir,
                                     bool move) {
  if (sources.empty()) return;

  if (LooksLikeUriPath(dstDir)) {
    wxMessageBox("Copy/Move to remote locations is not supported yet.", "Quarry",
                 wxOK | wxICON_INFORMATION, this);
    return;
  }

  // Validate sources are local filesystem paths.
  for (const auto& p : sources) {
    if (p.empty()) return;
    if (LooksLikeUriPath(p)) {
      wxMessageBox("Copy/Move from remote locations is not supported yet.", "Quarry",
                   wxOK | wxICON_INFORMATION, this);
      return;
    }
  }

  std::error_code ec;
  if (dstDir.empty() || !std::filesystem::exists(dstDir, ec) || !std::filesystem::is_directory(dstDir, ec)) {
    wxMessageBox("Destination is not a local directory.", "Quarry", wxOK | wxICON_WARNING, this);
    return;
  }

  const wxString dstDirWx = wxString::FromUTF8(dstDir.string());
  const wxString confirmMsg = wxString::Format("%s %zu item(s) to:\n\n%s\n\nExisting files may be overwritten.",
                                               title.c_str(),
                                               sources.size(),
                                               dstDirWx.c_str());
  if (wxMessageBox(confirmMsg, title, wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) {
    return;
  }

  AsyncFileOpState state;
  {
    std::error_code isDirEc;
    for (const auto& p : sources) {
      if (std::filesystem::is_directory(p, isDirEc) && !isDirEc) {
        state.hasDir = true;
        break;
      }
      isDirEc.clear();
    }
  }

  std::thread worker([&state, sources, dstDir, move]() {
    std::error_code workerEc;
    for (const auto& src : sources) {
      if (state.cancelRequested.load()) break;
      if (src.empty()) {
        state.done.fetch_add(1);
        continue;
      }

      workerEc.clear();
      if (!std::filesystem::exists(src, workerEc)) {
        state.done.fetch_add(1);
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(state.mu);
        state.currentLabel = src.filename().string();
      }

      auto dst = dstDir / src.filename();

      // Conflict handling (delegated to UI thread).
      bool skipItem = false;
      for (;;) {
        if (state.cancelRequested.load()) break;
        std::error_code existsEc;
        if (!std::filesystem::exists(dst, existsEc)) break;

        std::unique_lock<std::mutex> lock(state.mu);
        state.prompt = AsyncFileOpPrompt{.kind = AsyncFileOpPrompt::Kind::Exists, .dst = dst};
        state.reply.reset();
        state.cv.notify_all();
        state.cv.wait(lock, [&state]() { return state.reply.has_value() || state.cancelRequested.load(); });
        if (state.cancelRequested.load()) break;

        const auto reply = *state.reply;
        state.prompt.reset();
        state.reply.reset();

        if (reply.existsChoice == ExistsChoice::Skip) {
          skipItem = true;
          break;
        }
        if (reply.existsChoice == ExistsChoice::Cancel) {
          state.cancelRequested.store(true);
          break;
        }
        if (reply.existsChoice == ExistsChoice::Rename) {
          if (!reply.renameTo) {
            state.cancelRequested.store(true);
            break;
          }
          dst = dstDir / *reply.renameTo;
          continue;
        }
        break;  // Overwrite
      }

      if (state.cancelRequested.load()) break;
      if (skipItem) {
        state.done.fetch_add(1);
        continue;
      }

      const CancelFn shouldCancel = [&state]() { return state.cancelRequested.load(); };
      const CopyProgressFn onProgress = [&state, src](const std::filesystem::path& current) {
        const auto rel = current.lexically_relative(src);
        std::string label;
        if (!rel.empty() && rel != current) label = rel.string();
        if (label.empty()) label = current.filename().string();
        if (label.empty()) label = current.string();
        std::lock_guard<std::mutex> lock(state.mu);
        state.currentLabel = std::move(label);
      };

      const auto result = move ? MovePath(src, dst, shouldCancel, onProgress)
                               : CopyPathRecursive(src, dst, shouldCancel, onProgress);
      if (state.cancelRequested.load() && !result.ok && result.message == "Canceled") break;
      if (!result.ok) {
        std::unique_lock<std::mutex> lock(state.mu);
        state.prompt = AsyncFileOpPrompt{.kind = AsyncFileOpPrompt::Kind::Error,
                                         .dst = dst,
                                         .errorMessage = result.message.ToStdString()};
        state.reply.reset();
        state.cv.notify_all();
        state.cv.wait(lock, [&state]() { return state.reply.has_value() || state.cancelRequested.load(); });
        if (state.cancelRequested.load()) break;

        const auto reply = *state.reply;
        state.prompt.reset();
        state.reply.reset();
        if (!reply.continueAfterError) {
          state.cancelRequested.store(true);
          break;
        }
      }

      state.done.fetch_add(1);
    }

    std::lock_guard<std::mutex> lock(state.mu);
    state.finished = true;
    state.cv.notify_all();
  });

  wxProgressDialog progress(move ? "Moving" : "Copying",
                            "Preparing...",
                            static_cast<int>(sources.size()),
                            this,
                            wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);

  int lastValue = -1;
  for (;;) {
    // Handle any pending prompt from the worker.
    std::optional<AsyncFileOpPrompt> prompt;
    {
      std::lock_guard<std::mutex> lock(state.mu);
      prompt = state.prompt;
    }

    if (prompt) {
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
      } else {
        wxMessageDialog dlg(this,
                            wxString::Format("%s failed:\n\n%s\n\nContinue?",
                                             title.c_str(),
                                             wxString::FromUTF8(prompt->errorMessage).c_str()),
                            title,
                            wxYES_NO | wxNO_DEFAULT | wxICON_ERROR);
        dlg.SetYesNoLabels("Continue", "Cancel");
        reply.continueAfterError = (dlg.ShowModal() == wxID_YES);
      }

      {
        std::lock_guard<std::mutex> lock(state.mu);
        state.reply = reply;
        state.cv.notify_all();
      }
    }

    const auto done = state.done.load();
    wxString label;
    {
      std::lock_guard<std::mutex> lock(state.mu);
      label = wxString::FromUTF8(state.currentLabel);
    }

    const int value = static_cast<int>(std::min(done, sources.size()));
    const bool keepGoing = (value != lastValue) ? progress.Update(value, label) : progress.Pulse(label);
    lastValue = value;

    if (!keepGoing) {
      state.cancelRequested.store(true);
      {
        std::lock_guard<std::mutex> lock(state.mu);
        state.cv.notify_all();
      }
      break;
    }

    bool finished = false;
    {
      std::lock_guard<std::mutex> lock(state.mu);
      finished = state.finished;
    }
    if (finished) break;

    wxMilliSleep(30);
    wxYieldIfNeeded();
  }

  if (worker.joinable()) worker.join();

  // Refresh both panes; simplest/robust for now.
  if (top_) top_->RefreshAll();
  if (bottom_) bottom_->RefreshAll();
  if (state.hasDir) {
    if (top_) top_->RefreshTree();
    if (bottom_) bottom_->RefreshTree();
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
  return activePane_ == ActivePane::Top ? top_ : bottom_;
}

FilePanel* MainFrame::GetInactivePanel() const {
  return activePane_ == ActivePane::Top ? bottom_ : top_;
}

void MainFrame::OnQuit(wxCommandEvent&) { Close(true); }

void MainFrame::OnRefresh(wxCommandEvent&) {
  top_->RefreshListing();
  bottom_->RefreshListing();
}

void MainFrame::OnConnectToServer(wxCommandEvent&) {
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

  GetActivePanel()->SetDirectory(uri);
  GetActivePanel()->FocusPrimary();
}

void MainFrame::OnCopy(wxCommandEvent&) {
  auto* from = GetActivePanel();
  auto* to = GetInactivePanel();

  const auto sources = from->GetSelectedPaths();
  if (sources.empty()) return;

  const auto dstDir = to->GetDirectoryPath();
  CopyMoveWithProgress("Copy", sources, dstDir, /*move=*/false);
}

void MainFrame::OnMove(wxCommandEvent&) {
  auto* from = GetActivePanel();
  auto* to = GetInactivePanel();

  const auto sources = from->GetSelectedPaths();
  if (sources.empty()) return;

  const auto dstDir = to->GetDirectoryPath();
  CopyMoveWithProgress("Move", sources, dstDir, /*move=*/true);
}

void MainFrame::OnDelete(wxCommandEvent&) {
  auto* from = GetActivePanel();
  const auto sources = from->GetSelectedPaths();
  if (sources.empty()) return;

  const auto srcDir = from->GetDirectoryPath();
  bool hasDir = false;
  for (const auto& p : sources) {
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec) && !ec) {
      hasDir = true;
      break;
    }
  }
  const auto message = wxString::Format("Move %zu item(s) to Trash?", sources.size());
  if (wxMessageBox(message, "Trash", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) {
    return;
  }

  wxProgressDialog progress("Trashing",
                            "Preparing...",
                            static_cast<int>(sources.size()),
                            this,
                            wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);

  for (size_t i = 0; i < sources.size(); i++) {
    const auto& src = sources[i];
    if (!progress.Update(static_cast<int>(i), src.filename().string())) break;

    const auto result = TrashPath(src);
    if (!result.ok) {
      wxMessageDialog dlg(this,
                          wxString::Format("Trash failed:\n\n%s\n\nDelete permanently instead?",
                                           result.message.c_str()),
                          "Trash failed",
                          wxYES_NO | wxCANCEL | wxNO_DEFAULT | wxICON_ERROR);
      dlg.SetYesNoCancelLabels("Delete", "Skip", "Cancel");
      const int rc = dlg.ShowModal();
      if (rc == wxID_YES) {
        const auto delRes = DeletePath(src);
        if (!delRes.ok) {
          wxMessageDialog dlg2(this,
                               wxString::Format("Delete failed:\n\n%s\n\nContinue?", delRes.message.c_str()),
                               "Delete failed",
                               wxYES_NO | wxNO_DEFAULT | wxICON_ERROR);
          dlg2.SetYesNoLabels("Continue", "Cancel");
          if (dlg2.ShowModal() != wxID_YES) break;
        }
      } else if (rc == wxID_NO) {
        continue;
      } else {
        break;
      }
    }
  }

  RefreshPanelsShowing(srcDir, /*treeChanged=*/hasDir);
}

void MainFrame::OnDeletePermanent(wxCommandEvent&) {
  auto* from = GetActivePanel();
  const auto sources = from->GetSelectedPaths();
  if (sources.empty()) return;

  const auto srcDir = from->GetDirectoryPath();
  bool hasDir = false;
  for (const auto& p : sources) {
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec) && !ec) {
      hasDir = true;
      break;
    }
  }
  const auto message = wxString::Format(
      "Permanently delete %zu item(s)?\n\nThis cannot be undone.", sources.size());
  if (wxMessageBox(message, "Delete", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) {
    return;
  }

  wxProgressDialog progress("Deleting",
                            "Preparing...",
                            static_cast<int>(sources.size()),
                            this,
                            wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);

  for (size_t i = 0; i < sources.size(); i++) {
    const auto& src = sources[i];
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
  RefreshPanelsShowing(srcDir, /*treeChanged=*/hasDir);
}

void MainFrame::OnRename(wxCommandEvent&) {
  GetActivePanel()->BeginInlineRename();
}

void MainFrame::OnMkDir(wxCommandEvent&) {
  GetActivePanel()->CreateFolder();
}
