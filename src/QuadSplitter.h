#pragma once

#include <wx/panel.h>

class QuadSplitter final : public wxPanel {
public:
  explicit QuadSplitter(wxWindow* parent, wxWindowID id = wxID_ANY);

  void SetWindows(wxWindow* topLeft,
                  wxWindow* topRight,
                  wxWindow* bottomLeft,
                  wxWindow* bottomRight);

  int GetVerticalSashPosition() const { return vSashPos_; }
  int GetHorizontalSashPosition() const { return hSashPos_; }
  void SetVerticalSashPosition(int pos);
  void SetHorizontalSashPosition(int pos);

private:
  enum class DragMode { None, Vertical, Horizontal };

  void LayoutChildren();
  void EnsureInitialSashes(const wxSize& client);
  int ClampVSash(int pos, const wxSize& client) const;
  int ClampHSash(int pos, const wxSize& client) const;

  wxRect VerticalSashRect(const wxSize& client) const;
  wxRect HorizontalSashRect(const wxSize& client) const;

  void OnSize(wxSizeEvent& event);
  void OnPaint(wxPaintEvent& event);
  void OnLeftDown(wxMouseEvent& event);
  void OnLeftUp(wxMouseEvent& event);
  void OnMotion(wxMouseEvent& event);
  void OnLeave(wxMouseEvent& event);

  void UpdateHoverCursor(const wxPoint& pt);

  wxWindow* tl_{nullptr};
  wxWindow* tr_{nullptr};
  wxWindow* bl_{nullptr};
  wxWindow* br_{nullptr};

  int vSashPos_{-1};
  int hSashPos_{-1};
  DragMode drag_{DragMode::None};
  bool initialSet_{false};

  static constexpr int kSashSize = 6;
  static constexpr int kGrabSize = 6;

  wxDECLARE_EVENT_TABLE();
};

