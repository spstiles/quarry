#include "FilePanel.h"

#include "NavIcons.h"
#include "util.h"

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <system_error>

#include <wx/button.h>
#include <wx/dataview.h>
#include <wx/bmpbuttn.h>
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
#include <wx/textfile.h>
#include <wx/utils.h>

namespace fs = std::filesystem;

namespace {
constexpr int COL_NAME = 0;
constexpr int COL_TYPE = 1;
constexpr int COL_SIZE = 2;
constexpr int COL_MOD = 3;

constexpr int COL_PLACE_LABEL = 0;
constexpr int COL_PLACE_PATH = 1;

class TreeNodeData final : public wxTreeItemData {
public:
  explicit TreeNodeData(fs::path p) : path(std::move(p)) {}
  fs::path path;
};

enum class TreeIcon : int {
  Folder = 0,
  Home,
  Drive,
  Places,
  Filesystem,
};

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
  wxSingleChoiceDialog dlg(parent,
                           wxString::Format("Destination already exists:\n\n%s", dst.string()),
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

std::optional<fs::path> ReadXdgUserDir(const std::string& key) {
  // Reads ~/.config/user-dirs.dirs (common on Linux desktops).
  const auto home = wxGetHomeDir().ToStdString();
  if (home.empty()) return std::nullopt;
  const fs::path path = fs::path(home) / ".config" / "user-dirs.dirs";

  std::error_code ec;
  if (!fs::exists(path, ec)) return std::nullopt;

  wxTextFile tf;
  if (!tf.Open(path.string())) return std::nullopt;

  const std::string prefix = "XDG_" + key + "_DIR=";
  for (size_t i = 0; i < tf.GetLineCount(); i++) {
    const auto line = tf.GetLine(i).ToStdString();
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
    const auto status = de.symlink_status(statEc);
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
    });
  }

  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
    if (a.isDir != b.isDir) return a.isDir > b.isDir;
    return a.name < b.name;
  });

  return entries;
}
} // namespace

FilePanel::FilePanel(wxWindow* parent) : wxPanel(parent, wxID_ANY) { BuildLayout(); }

void FilePanel::BuildLayout() {
  split_ = new wxSplitterWindow(this, wxID_ANY);
  split_->SetSashGravity(0.0);
  split_->SetMinimumPaneSize(160);

  auto* sidebarPane = new wxPanel(split_, wxID_ANY);
  auto* sidebarSizer = new wxBoxSizer(wxVERTICAL);
  sidebarSplit_ = new wxSplitterWindow(sidebarPane, wxID_ANY);
  sidebarSplit_->SetSashGravity(0.35);
  sidebarSplit_->SetMinimumPaneSize(80);

  auto* placesPane = new wxPanel(sidebarSplit_, wxID_ANY);
  auto* placesSizer = new wxBoxSizer(wxVERTICAL);
  placesSizer->Add(new wxStaticText(placesPane, wxID_ANY, "Places"), 0,
                   wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

  places_ = new wxDataViewListCtrl(placesPane, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxDV_ROW_LINES);
  places_->AppendIconTextColumn("Place", wxDATAVIEW_CELL_INERT, 160, wxALIGN_LEFT,
                                wxDATAVIEW_COL_RESIZABLE);
  places_->AppendTextColumn("Path", wxDATAVIEW_CELL_INERT, 0, wxALIGN_LEFT,
                            wxDATAVIEW_COL_HIDDEN);
  placesSizer->Add(places_, 1, wxEXPAND | wxALL, 8);
  placesPane->SetSizer(placesSizer);

  auto* foldersPane = new wxPanel(sidebarSplit_, wxID_ANY);
  auto* foldersSizer = new wxBoxSizer(wxVERTICAL);
  foldersSizer->Add(new wxStaticText(foldersPane, wxID_ANY, "Folders"), 0,
                    wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

  folderTree_ = new wxTreeCtrl(foldersPane, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxTR_HAS_BUTTONS | wxTR_HIDE_ROOT | wxTR_LINES_AT_ROOT);
  foldersSizer->Add(folderTree_, 1, wxEXPAND | wxALL, 8);
  foldersPane->SetSizer(foldersSizer);

  sidebarSplit_->SplitHorizontally(placesPane, foldersPane, 220);
  sidebarSizer->Add(sidebarSplit_, 1, wxEXPAND);
  sidebarPane->SetSizer(sidebarSizer);

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

  statusText_ = new wxStaticText(listPane, wxID_ANY, "");
  listSizer->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
  listSizer->Add(statusText_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM | wxTOP, 8);
  listPane->SetSizer(listSizer);

  split_->SplitVertically(sidebarPane, listPane, 300);

  auto* sizer = new wxBoxSizer(wxVERTICAL);
  sizer->Add(split_, 1, wxEXPAND);
  SetSizer(sizer);

  BindEvents();
  UpdateStatusText();
  UpdateNavButtons();
  UpdateNavIcons();
  BuildPlaces();
  BuildFolderTree();
  SyncSidebarToCurrentDir();
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

  // Sidebar navigation.
  if (places_) {
    places_->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [this](wxDataViewEvent&) {
      if (ignoreSidebarEvent_) return;
      const int row = places_->GetSelectedRow();
      if (row == wxNOT_FOUND) return;
      wxVariant pathVar;
      places_->GetValue(pathVar, static_cast<unsigned int>(row), COL_PLACE_PATH);
      const auto p = pathVar.GetString().ToStdString();
      if (!p.empty()) NavigateTo(fs::path(p), /*recordHistory=*/true);
    });
    places_->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent&) {
      if (ignoreSidebarEvent_) return;
      const int row = places_->GetSelectedRow();
      if (row == wxNOT_FOUND) return;
      wxVariant pathVar;
      places_->GetValue(pathVar, static_cast<unsigned int>(row), COL_PLACE_PATH);
      const auto p = pathVar.GetString().ToStdString();
      if (!p.empty()) NavigateTo(fs::path(p), /*recordHistory=*/true);
    });
  }

  if (folderTree_) {
    folderTree_->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent&) { OnTreeSelectionChanged(); });
    folderTree_->Bind(wxEVT_TREE_ITEM_EXPANDING, [this](wxTreeEvent& e) {
      if (ignoreSidebarEvent_) return;
      const auto item = e.GetItem();
      if (!item.IsOk()) return;
      auto* data = dynamic_cast<TreeNodeData*>(folderTree_->GetItemData(item));
      if (!data) return;

      if (folderTree_->GetChildrenCount(item, false) == 0) {
        PopulateFolderChildren(item, data->path);
      }
    });
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

  if (places_) {
    places_->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
      lastFocus_ = LastFocus::Tree;
      if (onFocus_) onFocus_();
      e.Skip();
    });
    places_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
      lastFocus_ = LastFocus::Tree;
      if (onFocus_) onFocus_();
      e.Skip();
    });
  }

  if (folderTree_) {
    folderTree_->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
      lastFocus_ = LastFocus::Tree;
      if (onFocus_) onFocus_();
      e.Skip();
    });
    folderTree_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
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
  SyncSidebarToCurrentDir();
  UpdateStatusText();
}

void FilePanel::RefreshTree() {
  if (currentDir_.empty()) return;
  RefreshTreeNode(currentDir_);
}

void FilePanel::NavigateUp() {
  if (currentDir_.empty()) return;
  auto parent = currentDir_.parent_path();
  if (parent.empty()) parent = currentDir_; // likely at root
  NavigateTo(parent, /*recordHistory=*/true);
}

void FilePanel::FocusPrimary() {
  if (lastFocus_ == LastFocus::Tree) {
    if (folderTree_) {
      folderTree_->SetFocus();
      return;
    }
    if (places_) {
      places_->SetFocus();
      return;
    }
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

  const auto next = currentDir_ / GetRowName(list_, static_cast<unsigned int>(row));
  NavigateTo(next, /*recordHistory=*/true);
}

void FilePanel::OnTreeSelectionChanged() {
  if (ignoreSidebarEvent_) return;
  if (!folderTree_) return;
  const auto item = folderTree_->GetSelection();
  if (!item.IsOk()) return;
  auto* data = dynamic_cast<TreeNodeData*>(folderTree_->GetItemData(item));
  if (!data) return;
  if (data->path.empty()) return;
  NavigateTo(data->path, /*recordHistory=*/true);
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
  const auto name = GetRowName(list_, static_cast<unsigned int>(row));
  if (name.empty()) return;

  const auto path = currentDir_ / name;
  if (typeVar.GetString() == "Dir") {
    NavigateTo(path, /*recordHistory=*/true);
    return;
  }

  wxLaunchDefaultApplication(path.string());
}

bool FilePanel::LoadDirectory(const fs::path& dir) {
  std::vector<std::string> selectedNames;
  std::optional<std::string> currentName;
  if (list_) {
    wxDataViewItemArray items;
    list_->GetSelections(items);
    selectedNames.reserve(items.size());
    for (const auto& item : items) {
      const int row = list_->ItemToRow(item);
      if (row == wxNOT_FOUND) continue;
      const auto name = GetRowName(list_, static_cast<unsigned int>(row));
      if (!name.empty()) selectedNames.push_back(name);
    }

    const auto curItem = list_->GetCurrentItem();
    const int curRow = list_->ItemToRow(curItem);
    if (curRow != wxNOT_FOUND) {
      const auto name = GetRowName(list_, static_cast<unsigned int>(curRow));
      if (!name.empty()) currentName = name;
    }
  }

  std::error_code ec;
  const auto canonical = fs::weakly_canonical(dir, ec);
  const auto resolved = ec ? dir : canonical;

  if (!fs::exists(resolved, ec) || !fs::is_directory(resolved, ec)) {
    wxMessageBox(wxString::Format("Not a directory:\n\n%s", resolved.string()), "Quarry",
                 wxOK | wxICON_WARNING, this);
    return false;
  }

  currentDir_ = resolved;
  if (pathCtrl_) pathCtrl_->ChangeValue(currentDir_.string());
  SyncSidebarToCurrentDir();

  std::string err;
  const auto entries = ListDir(currentDir_, &err);
  if (!err.empty()) {
    wxMessageBox(wxString::Format("Unable to list directory:\n\n%s\n\n%s", currentDir_.string(), err),
                 "Quarry", wxOK | wxICON_ERROR, this);
  }
  Populate(entries);
  ReselectAndReveal(selectedNames, currentName);
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
    wxMessageBox(wxString::Format("Rename failed:\n\n%s", ec.message()), "Rename",
                 wxOK | wxICON_ERROR, this);
    SetRowName(list_, static_cast<unsigned int>(row), oldName, renamedDir);
    return;
  }

  RefreshAll();
  if (renamedDir) RefreshTree();
  NotifyDirContentsChanged(renamedDir);
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
  if (!currentDir_.empty()) {
    std::error_code ec;
    const auto space = fs::space(currentDir_, ec);
    if (!ec) {
      freeText = HumanSize(space.available) + " free of " + HumanSize(space.capacity);
    }
  }

  const auto label = wxString::Format(
      "Items: %zu (%zu dirs, %zu files)   Selected: %zu (%zu dirs, %zu files)   Selected size: %s   Free: %s",
      total, totalDirs, totalFiles,
      selectedCount, selectedDirs, selectedFiles,
      HumanSize(selectedBytes),
      freeText);
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
    list_->AppendItem(cols);
  }
  list_->Thaw();
}

void FilePanel::ReselectAndReveal(const std::vector<std::string>& selectedNames,
                                  const std::optional<std::string>& currentName) {
  if (!list_) return;
  if (currentEntries_.empty()) return;

  std::unordered_map<std::string, int> nameToRow;
  nameToRow.reserve(currentEntries_.size());
  for (size_t i = 0; i < currentEntries_.size(); i++) {
    nameToRow.emplace(currentEntries_[i].name, static_cast<int>(i));
  }

  list_->Freeze();
  list_->UnselectAll();

  wxDataViewItem revealItem;

  for (const auto& name : selectedNames) {
    const auto it = nameToRow.find(name);
    if (it == nameToRow.end()) continue;
    const auto item = list_->RowToItem(it->second);
    if (!item.IsOk()) continue;
    list_->Select(item);
    if (!revealItem.IsOk()) revealItem = item;
  }

  if (!revealItem.IsOk() && currentName) {
    const auto it = nameToRow.find(*currentName);
    if (it != nameToRow.end()) {
      revealItem = list_->RowToItem(it->second);
    }
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

  const auto child = GetRowName(list_, static_cast<unsigned int>(row));
  if (child.empty()) return std::nullopt;
  return currentDir_ / child;
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
    const auto name = GetRowName(list_, static_cast<unsigned int>(row));
    if (name.empty()) continue;
    rows.emplace_back(row, currentDir_ / name);
  }

  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

  std::vector<fs::path> paths;
  paths.reserve(rows.size());
  for (const auto& [_, p] : rows) paths.push_back(p);
  return paths;
}

void FilePanel::BeginInlineRename() {
  wxDataViewItemArray items;
  list_->GetSelections(items);
  if (items.size() != 1) return;
  list_->EditItem(items[0], list_->GetColumn(COL_NAME));
}

void FilePanel::CreateFolder() {
  const auto name = wxGetTextFromUser("Folder name:", "Create Folder", "", this).ToStdString();
  if (name.empty()) return;
  if (name.find('/') != std::string::npos) {
    wxMessageBox("Invalid folder name.", "Create Folder", wxOK | wxICON_WARNING, this);
    return;
  }

  std::error_code ec;
  fs::create_directory(currentDir_ / name, ec);
  if (ec) {
    wxMessageBox(wxString::Format("Create folder failed:\n\n%s", ec.message()), "Create Folder",
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
      wxMessageDialog dlg(this,
                          wxString::Format("%s failed:\n\n%s\n\nContinue?",
                                           isMove ? "Move" : "Copy", result.message),
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
                          wxString::Format("Trash failed:\n\n%s\n\nContinue?", result.message),
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
                          wxString::Format("Delete failed:\n\n%s\n\nContinue?", result.message),
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

void FilePanel::RefreshTreeNode(const fs::path& dir) {
  if (dir.empty()) return;
  BuildPlaces();
  BuildFolderTree();
  SyncSidebarToCurrentDir();
}

void FilePanel::BuildPlaces() {
  if (!places_) return;
  places_->Freeze();
  places_->DeleteAllItems();

  const auto addPlace = [&](const wxString& label, const fs::path& path, const wxBitmapBundle& icon) {
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) return;
    wxVector<wxVariant> cols;
    wxDataViewIconText it(label, icon);
    wxVariant v0;
    v0 << it;
    cols.push_back(v0);
    cols.push_back(wxVariant(path.string()));
    places_->AppendItem(cols);
  };

  const auto home = wxGetHomeDir().ToStdString();
  if (!home.empty()) {
    addPlace("Home", fs::path(home),
             wxArtProvider::GetBitmapBundle(wxART_GO_HOME, wxART_OTHER, wxSize(16, 16)));
  }

  const struct Place { const char* label; const char* key; const char* fallback; } placeList[] = {
      {"Desktop", "DESKTOP", "Desktop"},
      {"Documents", "DOCUMENTS", "Documents"},
      {"Downloads", "DOWNLOAD", "Downloads"},
      {"Music", "MUSIC", "Music"},
      {"Pictures", "PICTURES", "Pictures"},
      {"Videos", "VIDEOS", "Videos"},
  };

  for (const auto& p : placeList) {
    fs::path dirPath;
    if (const auto xdg = ReadXdgUserDir(p.key)) dirPath = *xdg;
    else dirPath = DefaultUserDir(p.fallback);
    addPlace(p.label, dirPath,
             wxArtProvider::GetBitmapBundle(wxART_FOLDER, wxART_OTHER, wxSize(16, 16)));
  }

  places_->Thaw();
}

void FilePanel::BuildFolderTree() {
  if (!folderTree_) return;
  folderTree_->Freeze();
  folderTree_->DeleteAllItems();
  folderRoot_ = wxTreeItemId();

  auto* images = new wxImageList(16, 16, true);
  images->Add(wxArtProvider::GetBitmap(wxART_FOLDER, wxART_OTHER, wxSize(16, 16)));         // Folder
  images->Add(wxArtProvider::GetBitmap(wxART_GO_HOME, wxART_OTHER, wxSize(16, 16)));       // Home
  images->Add(wxArtProvider::GetBitmap(wxART_HARDDISK, wxART_OTHER, wxSize(16, 16)));      // Drive
  folderTree_->AssignImageList(images);

  const auto root = folderTree_->AddRoot("root");
  folderRoot_ = folderTree_->AppendItem(root, "/", static_cast<int>(TreeIcon::Drive));
  folderTree_->SetItemData(folderRoot_, new TreeNodeData(fs::path("/")));
  folderTree_->Expand(folderRoot_);
  PopulateFolderChildren(folderRoot_, fs::path("/"));

  folderTree_->Thaw();
}

void FilePanel::PopulateFolderChildren(const wxTreeItemId& parent, const fs::path& dir) {
  if (!folderTree_) return;
  std::error_code ec;
  if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return;

  folderTree_->DeleteChildren(parent);

  size_t added = 0;
  for (const auto& de : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
    if (ec) break;
    std::error_code sec;
    if (!de.is_directory(sec) || sec) continue;
    const auto name = de.path().filename().string();
    if (name.empty()) continue;
    const auto item = folderTree_->AppendItem(parent, name, static_cast<int>(TreeIcon::Folder));
    folderTree_->SetItemData(item, new TreeNodeData(de.path()));
    added++;
    if (added >= 400) break;
  }
}

wxTreeItemId FilePanel::EnsureFolderPathSelected(const fs::path& dir) {
  if (!folderTree_ || !folderRoot_.IsOk()) return wxTreeItemId();
  if (dir.empty()) return wxTreeItemId();

  fs::path current = "/";
  wxTreeItemId currentItem = folderRoot_;
  folderTree_->Expand(currentItem);

  for (const auto& part : dir.lexically_relative("/")) {
    if (part.empty()) continue;
    current /= part;

    if (folderTree_->GetChildrenCount(currentItem, false) == 0) {
      auto* data = dynamic_cast<TreeNodeData*>(folderTree_->GetItemData(currentItem));
      if (data && !data->path.empty()) PopulateFolderChildren(currentItem, data->path);
    }

    wxTreeItemIdValue ck;
    wxTreeItemId found;
    auto c = folderTree_->GetFirstChild(currentItem, ck);
    while (c.IsOk()) {
      auto* data = dynamic_cast<TreeNodeData*>(folderTree_->GetItemData(c));
      if (data && data->path == current) {
        found = c;
        break;
      }
      c = folderTree_->GetNextChild(currentItem, ck);
    }
    if (!found.IsOk()) break;
    currentItem = found;
    folderTree_->Expand(currentItem);
  }

  folderTree_->SelectItem(currentItem);
  folderTree_->EnsureVisible(currentItem);
  return currentItem;
}

void FilePanel::SyncSidebarToCurrentDir() {
  if (currentDir_.empty()) return;
  ignoreSidebarEvent_ = true;

  if (places_) {
    bool selected = false;
    const auto count = places_->GetItemCount();
    for (unsigned int r = 0; r < count; r++) {
      wxVariant pathVar;
      places_->GetValue(pathVar, r, COL_PLACE_PATH);
      const auto p = pathVar.GetString().ToStdString();
      if (!p.empty() && fs::path(p) == currentDir_) {
        places_->SelectRow(r);
        selected = true;
        break;
      }
    }
    if (!selected) places_->UnselectAll();
  }

  if (folderTree_ && folderRoot_.IsOk()) {
    EnsureFolderPathSelected(currentDir_);
  }

  ignoreSidebarEvent_ = false;
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
  menu.Enable(ID_CTX_OPEN, hasSelection);
  menu.Enable(ID_CTX_COPY, hasSelection);
  menu.Enable(ID_CTX_CUT, hasSelection);
  menu.Enable(ID_CTX_RENAME, selected.size() == 1);
  menu.Enable(ID_CTX_TRASH, hasSelection);
  menu.Enable(ID_CTX_DELETE_PERM, hasSelection);
  menu.Enable(ID_CTX_PASTE, g_clipboard && !g_clipboard->paths.empty());

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
