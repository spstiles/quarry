#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <wx/dataview.h>
#include <wx/panel.h>
#include <wx/treectrl.h>

class wxButton;
class wxDataViewListCtrl;
class wxDataViewEvent;
class wxTextCtrl;
class wxSplitterWindow;
class wxStaticText;
class wxBitmapButton;
class wxTreeCtrl;

class FilePanel final : public wxPanel {
public:
  struct Entry {
    std::string name;
    bool isDir{false};
    std::uintmax_t size{0};
    std::string modified;
  };

  explicit FilePanel(wxWindow* parent);

  void SetDirectory(const std::string& path);
  std::filesystem::path GetDirectoryPath() const;
  void RefreshListing();
  void RefreshAll();
  void RefreshTree();
  void NavigateUp();
  void FocusPrimary();
  void SetActiveVisual(bool isActive);

  // Opens selection: if a directory, navigates into it; if a file, launches via desktop.
  void OpenSelection();
  void ShowProperties();

  std::optional<std::filesystem::path> GetSelectedPath() const;
  std::vector<std::filesystem::path> GetSelectedPaths() const;

  void BeginInlineRename();
  void CreateFolder();
  void CopySelection();
  void CutSelection();
  void PasteIntoCurrentDir();
  void TrashSelection();
  void DeleteSelectionPermanent();

  void BindFocusEvents(std::function<void()> onFocus);
  void BindDirContentsChanged(
      std::function<void(const std::filesystem::path& dir, bool treeChanged)> onChanged);

private:
  void BuildLayout();
  void BindEvents();
  void NavigateToTextPath();
  void OpenSelectedIfDir(wxDataViewEvent& event);
  void OnTreeSelectionChanged();
  void OnListValueChanged(wxDataViewEvent& event);
  void UpdateStatusText();
  void UpdateActiveVisuals();
  void NotifyDirContentsChanged(bool treeChanged);
  void RefreshTreeNode(const std::filesystem::path& dir);
  void ReselectAndReveal(const std::vector<std::string>& selectedNames,
                         const std::optional<std::string>& currentName);
  void ShowListContextMenu(wxDataViewEvent& event);
  bool AnySelectedDirs() const;
  bool LoadDirectory(const std::filesystem::path& dir);
  void NavigateTo(const std::filesystem::path& dir, bool recordHistory);
  void ResetHistory(const std::filesystem::path& dir);
  void PushHistory(const std::filesystem::path& dir);
  void GoBack();
  void GoForward();
  void GoHome();
  void UpdateNavButtons();
  void UpdateNavIcons();
  void BuildPlaces();
  void BuildFolderTree();
  void SyncSidebarToCurrentDir();
  void PopulateFolderChildren(const wxTreeItemId& parent, const std::filesystem::path& dir);
  wxTreeItemId EnsureFolderPathSelected(const std::filesystem::path& dir);

  void SetStatus(const wxString& message);
  void Populate(const std::vector<Entry>& entries);

  wxTextCtrl* pathCtrl_{nullptr};
  wxBitmapButton* backBtn_{nullptr};
  wxBitmapButton* forwardBtn_{nullptr};
  wxBitmapButton* upBtn_{nullptr};
  wxBitmapButton* refreshBtn_{nullptr};
  wxBitmapButton* homeBtn_{nullptr};
  wxButton* goBtn_{nullptr};
  wxSplitterWindow* split_{nullptr};
  wxSplitterWindow* sidebarSplit_{nullptr};
  wxDataViewListCtrl* places_{nullptr};
  wxTreeCtrl* folderTree_{nullptr};
  wxDataViewListCtrl* list_{nullptr};
  wxStaticText* statusText_{nullptr};

  std::filesystem::path currentDir_;
  std::vector<Entry> currentEntries_{};
  std::function<void()> onFocus_{};
  std::function<void(const std::filesystem::path&, bool)> onDirContentsChanged_{};
  bool ignoreSidebarEvent_{false};

  enum class LastFocus { Tree, List };
  LastFocus lastFocus_{LastFocus::List};
  bool isActive_{false};

  std::vector<std::filesystem::path> history_{};
  int historyIndex_{-1};

  wxDataViewItem renameArmedItem_{};
  long long renameArmedAtMs_{0};

  wxTreeItemId folderRoot_{};
};
