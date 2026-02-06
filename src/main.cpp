#include "MainFrame.h"

#include <wx/wx.h>

class QuarryApp final : public wxApp {
public:
  bool OnInit() override {
    if (!wxApp::OnInit()) return false;
    auto* frame = new MainFrame();
    frame->Show(true);
    return true;
  }
};

wxIMPLEMENT_APP(QuarryApp);

