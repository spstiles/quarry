#include "MainFrame.h"

#include "util.h"

#include <filesystem>

#include <wx/accel.h>
#include <wx/choicdlg.h>
#include <wx/filefn.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/textdlg.h>

namespace {
enum MenuId : int {
  ID_Refresh = wxID_HIGHEST + 1,
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
  Bind(wxEVT_MENU, &MainFrame::OnCopy, this, ID_Copy);
  Bind(wxEVT_MENU, &MainFrame::OnMove, this, ID_Move);
  Bind(wxEVT_MENU, &MainFrame::OnDelete, this, ID_Trash);
  Bind(wxEVT_MENU, &MainFrame::OnDeletePermanent, this, ID_DeletePermanent);
  Bind(wxEVT_MENU, &MainFrame::OnRename, this, ID_Rename);
  Bind(wxEVT_MENU, &MainFrame::OnMkDir, this, ID_MkDir);

  top_->BindFocusEvents([this]() { SetActivePane(ActivePane::Top); });
  bottom_->BindFocusEvents([this]() { SetActivePane(ActivePane::Bottom); });

  auto onDirChanged = [this](const std::filesystem::path& dir, bool treeChanged) {
    RefreshPanelsShowing(dir, treeChanged);
  };
  top_->BindDirContentsChanged(onDirChanged);
  bottom_->BindDirContentsChanged(onDirChanged);

  // Key navigation that should work regardless of which child has focus.
  Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
    const int key = e.GetKeyCode();
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
    e.Skip();
  });

  wxAcceleratorEntry entries[8];
  entries[0].Set(wxACCEL_CTRL, (int)'Q', wxID_EXIT);
  entries[1].Set(wxACCEL_NORMAL, WXK_F5, ID_Refresh);
  entries[2].Set(wxACCEL_CTRL, (int)'C', ID_Copy);
  entries[3].Set(wxACCEL_CTRL, (int)'M', ID_Move);
  entries[4].Set(wxACCEL_NORMAL, WXK_DELETE, ID_Trash);
  entries[5].Set(wxACCEL_NORMAL, WXK_F2, ID_Rename);
  entries[6].Set(wxACCEL_NORMAL, WXK_F7, ID_MkDir);
  entries[7].Set(wxACCEL_SHIFT, WXK_DELETE, ID_DeletePermanent);
  SetAcceleratorTable(wxAcceleratorTable(8, entries));
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

void MainFrame::OnCopy(wxCommandEvent&) {
  auto* from = GetActivePanel();
  auto* to = GetInactivePanel();

  const auto sources = from->GetSelectedPaths();
  if (sources.empty()) return;

  const auto dstDir = to->GetDirectoryPath();
  bool hasDir = false;
  for (const auto& p : sources) {
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec) && !ec) {
      hasDir = true;
      break;
    }
  }
  const auto confirmMsg = wxString::Format(
      "Copy %zu item(s) to:\n\n%s\n\nExisting files may be overwritten.",
      sources.size(), dstDir.string());
  if (wxMessageBox(confirmMsg, "Copy", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) {
    return;
  }

  wxProgressDialog progress("Copying",
                            "Preparing...",
                            static_cast<int>(sources.size()),
                            this,
                            wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);

  bool cancelAll = false;
  for (size_t i = 0; i < sources.size(); i++) {
    const auto& src = sources[i];
    auto dst = dstDir / src.filename();

    if (!progress.Update(static_cast<int>(i), src.filename().string())) break;

    // Conflict handling.
    bool skipItem = false;
    for (;;) {
      std::error_code existsEc;
      if (!std::filesystem::exists(dst, existsEc)) break;

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
        dst = dstDir / nameDlg.GetValue().ToStdString();
        continue;
      }
      break; // Overwrite
    }

    if (cancelAll) break;
    if (skipItem) continue;

    const auto result = CopyPathRecursive(src, dst);
    if (!result.ok) {
      wxMessageDialog dlg(this,
                          wxString::Format("Copy failed:\n\n%s\n\nContinue?", result.message),
                          "Copy failed",
                          wxYES_NO | wxNO_DEFAULT | wxICON_ERROR);
      dlg.SetYesNoLabels("Continue", "Cancel");
      if (dlg.ShowModal() != wxID_YES) break;
    }
  }
  RefreshPanelsShowing(dstDir, /*treeChanged=*/hasDir);
}

void MainFrame::OnMove(wxCommandEvent&) {
  auto* from = GetActivePanel();
  auto* to = GetInactivePanel();

  const auto sources = from->GetSelectedPaths();
  if (sources.empty()) return;

  const auto srcDir = from->GetDirectoryPath();
  const auto dstDir = to->GetDirectoryPath();
  bool hasDir = false;
  for (const auto& p : sources) {
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec) && !ec) {
      hasDir = true;
      break;
    }
  }
  const auto confirmMsg = wxString::Format(
      "Move %zu item(s) to:\n\n%s\n\nExisting files may be overwritten.",
      sources.size(), dstDir.string());
  if (wxMessageBox(confirmMsg, "Move", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) {
    return;
  }

  wxProgressDialog progress("Moving",
                            "Preparing...",
                            static_cast<int>(sources.size()),
                            this,
                            wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME);

  bool cancelAll = false;
  for (size_t i = 0; i < sources.size(); i++) {
    const auto& src = sources[i];
    auto dst = dstDir / src.filename();

    if (!progress.Update(static_cast<int>(i), src.filename().string())) break;

    // Conflict handling.
    bool skipItem = false;
    for (;;) {
      std::error_code existsEc;
      if (!std::filesystem::exists(dst, existsEc)) break;

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
        dst = dstDir / nameDlg.GetValue().ToStdString();
        continue;
      }
      break; // Overwrite
    }

    if (cancelAll) break;
    if (skipItem) continue;

    const auto result = MovePath(src, dst);
    if (!result.ok) {
      wxMessageDialog dlg(this,
                          wxString::Format("Move failed:\n\n%s\n\nContinue?", result.message),
                          "Move failed",
                          wxYES_NO | wxNO_DEFAULT | wxICON_ERROR);
      dlg.SetYesNoLabels("Continue", "Cancel");
      if (dlg.ShowModal() != wxID_YES) break;
    }
  }
  RefreshPanelsShowing(srcDir, /*treeChanged=*/hasDir);
  RefreshPanelsShowing(dstDir, /*treeChanged=*/hasDir);
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
                                           result.message),
                          "Trash failed",
                          wxYES_NO | wxCANCEL | wxNO_DEFAULT | wxICON_ERROR);
      dlg.SetYesNoCancelLabels("Delete", "Skip", "Cancel");
      const int rc = dlg.ShowModal();
      if (rc == wxID_YES) {
        const auto delRes = DeletePath(src);
        if (!delRes.ok) {
          wxMessageDialog dlg2(this,
                               wxString::Format("Delete failed:\n\n%s\n\nContinue?", delRes.message),
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
                          wxString::Format("Delete failed:\n\n%s\n\nContinue?", result.message),
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
