#pragma once

#include <filesystem>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <wx/dataview.h>
#include <wx/treectrl.h>

class wxButton;
class wxDataViewListCtrl;
class wxDataViewEvent;
class wxTextCtrl;
class wxStaticText;
class wxBitmapButton;
class wxPanel;
class wxTreeCtrl;
class wxWindow;
class wxProcess;

struct WxProcessDeleter {
  void operator()(wxProcess* p) const;
};

class FilePanel final {
public:
  struct Entry {
    std::string name;
    bool isDir{false};
    std::uintmax_t size{0};
    std::string modified;
    std::string fullPath;
  };

  FilePanel(wxWindow* sidebarParent, wxWindow* listParent);

  wxWindow* SidebarWindow() const;
  wxWindow* ListWindow() const;

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

  // Seeds credentials for a location for this application instance.
  // This avoids extra auth prompts and can optionally persist via the OS keyring when supported.
  void SeedMountCredentials(const std::string& uri,
                            const std::string& username,
                            const std::string& password,
                            bool rememberForever);

  void BindFocusEvents(std::function<void()> onFocus);
  void BindDirContentsChanged(
      std::function<void(const std::filesystem::path& dir, bool treeChanged)> onChanged);
  void BindDropFiles(std::function<void(const std::vector<std::filesystem::path>& paths, bool move)> onDrop);

  std::array<int, 4> GetListColumnWidths() const;
  void SetListColumnWidths(const std::array<int, 4>& widths);

  // Sorting preferences: 0=Name, 1=Size, 2=Type, 3=Modified.
  int GetSortColumnIndex() const;
  bool IsSortAscending() const { return sortAscending_; }
  void SetSort(int columnIndex, bool ascending);

private:
  enum class ListingMode { Directory, Recent, Gio };
  enum class SortColumn { Name, Type, Size, Modified };

  void BuildLayout(wxWindow* sidebarParent, wxWindow* listParent);
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
  void ShowListContextMenu(const wxDataViewItem& contextItem,
                           const wxPoint& screenPos,
                           bool isBackgroundContext);
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
  void PopulateNetwork(const wxTreeItemId& networkItem);
  wxTreeItemId EnsurePathSelected(const wxTreeItemId& baseItem,
                                 const std::filesystem::path& basePath,
                                 const std::filesystem::path& targetDir);
  void SyncTreeToCurrentDir();

  void SetStatus(const wxString& message);
  void Populate(const std::vector<Entry>& entries);
  wxWindow* DialogParent() const;
  bool IsExtractableArchivePath(const std::filesystem::path& path) const;
  void ExtractArchiveTo(const std::filesystem::path& archivePath, const std::filesystem::path& dstDir);
  bool HasCommand(const wxString& name) const;

  wxPanel* sidebarRoot_{nullptr};
  wxPanel* listRoot_{nullptr};
  wxTextCtrl* pathCtrl_{nullptr};
  wxBitmapButton* backBtn_{nullptr};
  wxBitmapButton* forwardBtn_{nullptr};
  wxBitmapButton* upBtn_{nullptr};
  wxBitmapButton* refreshBtn_{nullptr};
  wxBitmapButton* homeBtn_{nullptr};
  wxButton* goBtn_{nullptr};
  wxTreeCtrl* tree_{nullptr};
  wxDataViewListCtrl* list_{nullptr};
  wxStaticText* statusText_{nullptr};

  std::filesystem::path currentDir_;
  ListingMode listingMode_{ListingMode::Directory};
  std::vector<Entry> currentEntries_{};
  std::function<void()> onFocus_{};
  std::function<void(const std::filesystem::path&, bool)> onDirContentsChanged_{};
  std::function<void(const std::vector<std::filesystem::path>&, bool)> onDropFiles_{};
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
  bool suppressNextContextMenu_{false};

  wxTreeItemId hiddenRoot_{};
  wxTreeItemId computerRoot_{};
  wxTreeItemId homeRoot_{};
  wxTreeItemId fsRoot_{};
  wxTreeItemId devicesRoot_{};
  wxTreeItemId networkRoot_{};
  wxTreeItemId browseNetworkRoot_{};
  wxTreeItemId desktopRoot_{};
  wxTreeItemId documentsRoot_{};
  wxTreeItemId downloadsRoot_{};
  wxTreeItemId musicRoot_{};
  wxTreeItemId picturesRoot_{};
  wxTreeItemId videosRoot_{};
  wxTreeItemId recentRoot_{};
  wxTreeItemId trashRoot_{};

  std::vector<std::unique_ptr<wxProcess, WxProcessDeleter>> extractProcs_{};
};
