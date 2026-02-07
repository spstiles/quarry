#pragma once

#include "FilePanel.h"

#include <wx/frame.h>

class wxSplitterWindow;

class MainFrame final : public wxFrame {
public:
  MainFrame();

private:
  enum class ActivePane { Top, Bottom };

  void BuildMenu();
  void BuildLayout();
  void BindEvents();

  FilePanel* GetActivePanel() const;
  FilePanel* GetInactivePanel() const;

  void OnQuit(wxCommandEvent& event);
  void OnRefresh(wxCommandEvent& event);
  void OnConnectToServer(wxCommandEvent& event);
  void OnCopy(wxCommandEvent& event);
  void OnMove(wxCommandEvent& event);
  void OnDelete(wxCommandEvent& event);
  void OnDeletePermanent(wxCommandEvent& event);
  void OnRename(wxCommandEvent& event);
  void OnMkDir(wxCommandEvent& event);

  void SetActivePane(ActivePane pane);
  void RefreshPanelsShowing(const std::filesystem::path& dir, bool treeChanged);

  wxSplitterWindow* splitter_{nullptr};
  FilePanel* top_{nullptr};
  FilePanel* bottom_{nullptr};
  ActivePane activePane_{ActivePane::Top};
};
