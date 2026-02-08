#pragma once

#include "FilePanel.h"
#include "QuadSplitter.h"

#include <wx/frame.h>

#include <memory>
#include <string>
#include <array>
#include <optional>

class MainFrame final : public wxFrame {
public:
  explicit MainFrame(std::string topDir = {}, std::string bottomDir = {});
  void StartFileOperation(const wxString& title,
                          const std::vector<std::filesystem::path>& sources,
                          const std::filesystem::path& dstDir,
                          bool move);

private:
  enum class ActivePane { Top, Bottom };

  void BuildMenu();
  void BuildLayout();
  void BindEvents();
  void InitPanelsIfNeeded();
  void BindPanelEvents();
  void SaveDefaultView();
  void LoadDefaultView();
  bool LoadDefaultViewInternal(bool applyToPanes, bool showNoDefaultMessage);

  FilePanel* GetActivePanel() const;
  FilePanel* GetInactivePanel() const;

  void OnQuit(wxCommandEvent& event);
  void OnAbout(wxCommandEvent& event);
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

  void SetActivePane(ActivePane pane);
  void RefreshPanelsShowing(const std::filesystem::path& dir, bool treeChanged);

  struct FileOpSession;
  std::unique_ptr<FileOpSession> fileOp_{};

  QuadSplitter* quad_{nullptr};
  std::unique_ptr<FilePanel> top_{};
  std::unique_ptr<FilePanel> bottom_{};
  ActivePane activePane_{ActivePane::Top};

  bool panelsInitialized_{false};
  std::string pendingTopDir_{};
  std::string pendingBottomDir_{};

  std::optional<int> pendingVSash_{};
  std::optional<int> pendingHSash_{};
  std::optional<std::array<int, 4>> pendingTopCols_{};
  std::optional<std::array<int, 4>> pendingBottomCols_{};
  std::optional<int> pendingTopSortCol_{};
  std::optional<bool> pendingTopSortAsc_{};
  std::optional<int> pendingBottomSortCol_{};
  std::optional<bool> pendingBottomSortAsc_{};
};
