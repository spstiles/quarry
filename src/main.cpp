#include "MainFrame.h"

#include <wx/cmdline.h>
#include <wx/wx.h>

#include <string>
#include <vector>

class QuarryApp final : public wxApp {
public:
  void OnInitCmdLine(wxCmdLineParser& parser) override {
    wxApp::OnInitCmdLine(parser);

    parser.AddParam("PATH_OR_URI", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL);
    parser.AddParam("PATH_OR_URI", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL);

    parser.SetLogo("Usage: quarry [PATH_OR_URI] [PATH_OR_URI]\n\n"
                   "If 1 argument is provided, both panes open it.\n"
                   "If 2 arguments are provided, top opens the first and bottom opens the second.\n");
  }

  bool OnCmdLineParsed(wxCmdLineParser& parser) override {
    if (!wxApp::OnCmdLineParsed(parser)) return false;

    const size_t count = parser.GetParamCount();
    if (count >= 1) {
      m_topDir = parser.GetParam(0).ToStdString();
      m_bottomDir = (count >= 2) ? parser.GetParam(1).ToStdString() : m_topDir;
    }
    return true;
  }

  bool OnInit() override {
    if (!wxApp::OnInit()) return false;

    auto* frame = new MainFrame(m_topDir, m_bottomDir);
    frame->Show(true);
    return true;
  }

private:
  std::string m_topDir;
  std::string m_bottomDir;
};

wxIMPLEMENT_APP(QuarryApp);
