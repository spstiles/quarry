#pragma once

#include "FilePanel.h"
#include "QuadSplitter.h"

#include <wx/frame.h>

#include <memory>
#include <string>
#include <array>
#include <optional>
#include <deque>
#include <cstdint>

class MainFrame final : public wxFrame {
public:
  explicit MainFrame(std::string topDir = {}, std::string bottomDir = {});
  void StartFileOperation(const wxString& title,
                          const std::vector<std::filesystem::path>& sources,
                          const std::filesystem::path& dstDir,
                          bool move);
  void StartTrashOperation(const std::vector<std::filesystem::path>& sources);
  void StartDeleteOperation(const std::vector<std::filesystem::path>& sources);
  void StartExtractOperation(const std::vector<std::string>& argv,
                             const std::filesystem::path& refreshDir,
                             bool treeChanged);

private:
  enum class ActivePane { Top, Bottom };
  enum class OpKind { CopyMove, Trash, Delete, Extract };

  struct QueuedOp {
    std::uint64_t id{0};
    OpKind kind{OpKind::CopyMove};
    wxString title{};
    std::vector<std::filesystem::path> sources{};
    std::filesystem::path dstDir{};
    bool move{false}; // for CopyMove
    std::vector<std::string> argv{}; // for Extract
    std::filesystem::path refreshDir{};
    bool treeChanged{false};
  };

  void BuildMenu();
  void BuildLayout();
  void BindEvents();
  void InitPanelsIfNeeded();
  void BindPanelEvents();
  void SaveDefaultView();
  void LoadDefaultView();
  bool LoadDefaultViewInternal(bool applyToPanes, bool showNoDefaultMessage);
  void ApplyStartupWindowCascade();
  void LoadStartupView();
  void SaveLastView(bool showMessage);
  void SaveViewToConfig(const wxString& base, bool showMessage);
  bool LoadViewFromConfig(const wxString& base, bool applyToPanes, bool showNoViewMessage);

  FilePanel* GetActivePanel() const;
  FilePanel* GetInactivePanel() const;

  void OnQuit(wxCommandEvent& event);
  void OnAbout(wxCommandEvent& event);
  void OnPreferences(wxCommandEvent& event);
  void OnRefresh(wxCommandEvent& event);
  void OnConnectToServer(wxCommandEvent& event);
  void OnConnectionsManager(wxCommandEvent& event);
  void OnCopy(wxCommandEvent& event);
  void OnMove(wxCommandEvent& event);
  void OnDelete(wxCommandEvent& event);
  void OnDeletePermanent(wxCommandEvent& event);
  void OnRename(wxCommandEvent& event);
  void OnMkDir(wxCommandEvent& event);

  void TransferDroppedPaths(FilePanel* target,
                            const std::vector<std::filesystem::path>& sources,
                            bool move);

  void CopyMoveWithProgress(const wxString& title,
                            const std::vector<std::filesystem::path>& sources,
                            const std::filesystem::path& dstDir,
                            bool move);
  void CopyMoveWithProgressInternal(const wxString& title,
                                    const std::vector<std::filesystem::path>& sources,
                                    const std::filesystem::path& dstDir,
                                    bool move,
                                    bool alreadyConfirmed);
  void TrashWithProgress(const std::vector<std::filesystem::path>& sources);
  void TrashWithProgressInternal(const std::vector<std::filesystem::path>& sources,
                                 bool alreadyConfirmed);
  void DeleteWithProgress(const std::vector<std::filesystem::path>& sources);
  void DeleteWithProgressInternal(const std::vector<std::filesystem::path>& sources,
                                  bool alreadyConfirmed);
  void ExtractWithProgress(const std::vector<std::string>& argv,
                           const std::filesystem::path& refreshDir,
                           bool treeChanged);
  void EnqueueOp(QueuedOp op);
  void StartNextQueuedOp();
  void UpdateQueueUi();

  void SetActivePane(ActivePane pane);
  void RefreshPanelsShowing(const std::filesystem::path& dir, bool treeChanged);

  struct FileOpSession;
  std::unique_ptr<FileOpSession> fileOp_{};
  std::deque<QueuedOp> opQueue_{};
  std::uint64_t nextOpId_{1};

  QuadSplitter* quad_{nullptr};
  std::unique_ptr<FilePanel> top_{};
  std::unique_ptr<FilePanel> bottom_{};
  ActivePane activePane_{ActivePane::Top};

  bool panelsInitialized_{false};
  std::string pendingTopDir_{};
  std::string pendingBottomDir_{};
  std::optional<std::filesystem::path> pendingTopSelect_{};
  std::optional<std::filesystem::path> pendingBottomSelect_{};

  std::optional<int> pendingVSash_{};
  std::optional<int> pendingHSash_{};
  std::optional<std::array<int, 4>> pendingTopCols_{};
  std::optional<std::array<int, 4>> pendingBottomCols_{};
  std::optional<int> pendingTopSortCol_{};
  std::optional<bool> pendingTopSortAsc_{};
  std::optional<int> pendingBottomSortCol_{};
  std::optional<bool> pendingBottomSortAsc_{};

  bool startupCascadeApplied_{false};
  bool skipStartupCascade_{false};
};
