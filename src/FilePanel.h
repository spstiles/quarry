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
    std::string fullPath;
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
  enum class ListingMode { Directory, Recent };
  enum class SortColumn { Name, Type, Size, Modified };

  void BuildLayout();
  void BindEvents();
  void NavigateToTextPath();
  void OpenSelectedIfDir(wxDataViewEvent& event);
  void OnTreeSelectionChanged();
  void OnTreeItemExpanding(wxTreeEvent& event);
  void OnListValueChanged(wxDataViewEvent& event);
  void ResortListing();
  void SortEntries(std::vector<Entry>& entries) const;
  void UpdateSortIndicators();
  void UpdateStatusText();
  void UpdateActiveVisuals();
  void NotifyDirContentsChanged(bool treeChanged);
  void ReselectAndReveal(const std::vector<std::string>& selectedKeys,
                         const std::optional<std::string>& currentKey);
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
  void BuildComputerTree();
  void PopulateDirChildren(const wxTreeItemId& parent, const std::filesystem::path& dir);
  void PopulateDevices(const wxTreeItemId& devicesItem);
  wxTreeItemId EnsurePathSelected(const wxTreeItemId& baseItem,
                                 const std::filesystem::path& basePath,
                                 const std::filesystem::path& targetDir);
  void SyncTreeToCurrentDir();

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
  wxTreeCtrl* tree_{nullptr};
  wxDataViewListCtrl* list_{nullptr};
  wxStaticText* statusText_{nullptr};

  std::filesystem::path currentDir_;
  ListingMode listingMode_{ListingMode::Directory};
  std::vector<Entry> currentEntries_{};
  std::function<void()> onFocus_{};
  std::function<void(const std::filesystem::path&, bool)> onDirContentsChanged_{};
  bool ignoreTreeEvent_{false};

  SortColumn sortColumn_{SortColumn::Name};
  bool sortAscending_{true};

  enum class LastFocus { Tree, List };
  LastFocus lastFocus_{LastFocus::List};
  bool isActive_{false};

  std::vector<std::filesystem::path> history_{};
  int historyIndex_{-1};

  wxDataViewItem renameArmedItem_{};
  long long renameArmedAtMs_{0};
  bool allowInlineEdit_{false};

  wxTreeItemId computerRoot_{};
  wxTreeItemId homeRoot_{};
  wxTreeItemId fsRoot_{};
  wxTreeItemId devicesRoot_{};
  wxTreeItemId desktopRoot_{};
  wxTreeItemId documentsRoot_{};
  wxTreeItemId downloadsRoot_{};
  wxTreeItemId musicRoot_{};
  wxTreeItemId picturesRoot_{};
  wxTreeItemId videosRoot_{};
  wxTreeItemId recentRoot_{};
  wxTreeItemId trashRoot_{};
};
