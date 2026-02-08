#include "QuadSplitter.h"

#include <algorithm>

#include <wx/dcclient.h>
#include <wx/settings.h>

namespace {
wxSize EffectiveMinSize(wxWindow* w) {
  if (!w) return wxSize(0, 0);
  wxSize ms = w->GetEffectiveMinSize();
  if (ms.x < 0) ms.x = 0;
  if (ms.y < 0) ms.y = 0;
  return ms;
}
} // namespace

wxBEGIN_EVENT_TABLE(QuadSplitter, wxPanel)
  EVT_SIZE(QuadSplitter::OnSize)
  EVT_PAINT(QuadSplitter::OnPaint)
  EVT_LEFT_DOWN(QuadSplitter::OnLeftDown)
  EVT_LEFT_UP(QuadSplitter::OnLeftUp)
  EVT_MOTION(QuadSplitter::OnMotion)
  EVT_LEAVE_WINDOW(QuadSplitter::OnLeave)
wxEND_EVENT_TABLE()

QuadSplitter::QuadSplitter(wxWindow* parent, wxWindowID id) : wxPanel(parent, id) {
  SetBackgroundStyle(wxBG_STYLE_PAINT);
}

void QuadSplitter::SetWindows(wxWindow* topLeft,
                              wxWindow* topRight,
                              wxWindow* bottomLeft,
                              wxWindow* bottomRight) {
  tl_ = topLeft;
  tr_ = topRight;
  bl_ = bottomLeft;
  br_ = bottomRight;
  LayoutChildren();
  Refresh();
}

void QuadSplitter::SetVerticalSashPosition(int pos) {
  vSashPos_ = pos;
  if (pos >= 0) initialSet_ = true;
  LayoutChildren();
  Refresh();
}

void QuadSplitter::SetHorizontalSashPosition(int pos) {
  hSashPos_ = pos;
  if (pos >= 0) initialSet_ = true;
  LayoutChildren();
  Refresh();
}

void QuadSplitter::EnsureInitialSashes(const wxSize& client) {
  if (client.x <= 0 || client.y <= 0) return;

  // During startup on GTK, wx may send early size/layout passes with very small
  // client sizes. If we "lock in" our initial sash positions during those
  // passes, the UI can start with a nearly-collapsed row/column and trigger
  // GTK negative-allocation warnings in controls with scrollbars.
  //
  // Instead, keep recomputing the initial 50/50 positions until the container
  // is large enough to satisfy the children minimum sizes, then lock.

  if (!initialSet_) {
    // Default sidebar width on first show.
    if (vSashPos_ < 0) vSashPos_ = client.x / 4;
    if (hSashPos_ < 0) hSashPos_ = client.y / 2;

    vSashPos_ = ClampVSash(vSashPos_, client);
    hSashPos_ = ClampHSash(hSashPos_, client);

    int minLeft = 140;
    int minRight = 220;
    int minTop = 140;
    int minBottom = 140;
    if (tl_ && tr_ && bl_ && br_) {
      const auto tlMin = EffectiveMinSize(tl_);
      const auto trMin = EffectiveMinSize(tr_);
      const auto blMin = EffectiveMinSize(bl_);
      const auto brMin = EffectiveMinSize(br_);
      minLeft = std::max(tlMin.x, blMin.x);
      minRight = std::max(trMin.x, brMin.x);
      minTop = std::max(tlMin.y, trMin.y);
      minBottom = std::max(blMin.y, brMin.y);
    }

    if (client.x >= (minLeft + minRight + kSashSize) &&
        client.y >= (minTop + minBottom + kSashSize)) {
      initialSet_ = true;
    }
  }
}

int QuadSplitter::ClampVSash(int pos, const wxSize& client) const {
  const int w = client.x;
  if (w <= 0) return 0;
  if (w <= kSashSize + 1) return 0;

  int minLeft = 140;
  int minRight = 220;
  if (tl_ && tr_ && bl_ && br_) {
    const auto tlMin = EffectiveMinSize(tl_);
    const auto trMin = EffectiveMinSize(tr_);
    const auto blMin = EffectiveMinSize(bl_);
    const auto brMin = EffectiveMinSize(br_);
    minLeft = std::max(tlMin.x, blMin.x);
    minRight = std::max(trMin.x, brMin.x);
  }

  const int maxPos = std::max(0, w - kSashSize);
  int clamped = std::clamp(pos, 0, maxPos);

  // Only enforce minimum pane sizes when there is enough space to satisfy them.
  if (w >= (minLeft + minRight + kSashSize)) {
    const int lo = minLeft;
    const int hi = w - minRight - kSashSize;
    clamped = std::clamp(clamped, lo, hi);
  }

  return clamped;
}

int QuadSplitter::ClampHSash(int pos, const wxSize& client) const {
  const int h = client.y;
  if (h <= 0) return 0;
  if (h <= kSashSize + 1) return 0;

  int minTop = 140;
  int minBottom = 140;
  if (tl_ && tr_ && bl_ && br_) {
    const auto tlMin = EffectiveMinSize(tl_);
    const auto trMin = EffectiveMinSize(tr_);
    const auto blMin = EffectiveMinSize(bl_);
    const auto brMin = EffectiveMinSize(br_);
    minTop = std::max(tlMin.y, trMin.y);
    minBottom = std::max(blMin.y, brMin.y);
  }

  const int maxPos = std::max(0, h - kSashSize);
  int clamped = std::clamp(pos, 0, maxPos);

  if (h >= (minTop + minBottom + kSashSize)) {
    const int lo = minTop;
    const int hi = h - minBottom - kSashSize;
    clamped = std::clamp(clamped, lo, hi);
  }

  return clamped;
}

wxRect QuadSplitter::VerticalSashRect(const wxSize& client) const {
  const int x = std::max(0, vSashPos_);
  return wxRect(x, 0, kSashSize, client.y);
}

wxRect QuadSplitter::HorizontalSashRect(const wxSize& client) const {
  const int y = std::max(0, hSashPos_);
  return wxRect(0, y, client.x, kSashSize);
}

void QuadSplitter::LayoutChildren() {
  const wxSize client = GetClientSize();
  if (client.x <= 0 || client.y <= 0) return;
  EnsureInitialSashes(client);
  if (!tl_ || !tr_ || !bl_ || !br_) return;
  if (vSashPos_ < 0) vSashPos_ = ClampVSash(client.x / 2, client);
  if (hSashPos_ < 0) hSashPos_ = ClampHSash(client.y / 2, client);

  const auto tlMin = EffectiveMinSize(tl_);
  const auto trMin = EffectiveMinSize(tr_);
  const auto blMin = EffectiveMinSize(bl_);
  const auto brMin = EffectiveMinSize(br_);
  const int minLeft = std::max(tlMin.x, blMin.x);
  const int minRight = std::max(trMin.x, brMin.x);
  const int minTop = std::max(tlMin.y, trMin.y);
  const int minBottom = std::max(blMin.y, brMin.y);

  // During startup on GTK, it's possible to get a transient, very small client
  // size before the frame reaches its final size. Laying out scrolled windows
  // (tree/list) at those tiny sizes can trigger GTK negative-allocation warnings.
  // If the container can't satisfy minimum sizes, hide all children and wait
  // for the next size/layout pass.
  if (client.x < (minLeft + minRight + kSashSize) || client.y < (minTop + minBottom + kSashSize)) {
    if (tl_->IsShown()) tl_->Hide();
    if (tr_->IsShown()) tr_->Hide();
    if (bl_->IsShown()) bl_->Hide();
    if (br_->IsShown()) br_->Hide();
    return;
  }

  vSashPos_ = ClampVSash(vSashPos_, client);
  hSashPos_ = ClampHSash(hSashPos_, client);

  const int w = client.x;
  const int h = client.y;
  const int leftW = vSashPos_;
  const int rightW = std::max(0, w - vSashPos_ - kSashSize);
  const int topH = hSashPos_;
  const int bottomH = std::max(0, h - hSashPos_ - kSashSize);

  const wxRect rTL(0, 0, leftW, topH);
  const wxRect rTR(vSashPos_ + kSashSize, 0, rightW, topH);
  const wxRect rBL(0, hSashPos_ + kSashSize, leftW, bottomH);
  const wxRect rBR(vSashPos_ + kSashSize, hSashPos_ + kSashSize, rightW, bottomH);

  // When a quadrant becomes too small during extreme resizes, GTK can produce
  // critical warnings from internal scrollbar layout. Hiding zero-sized
  // children avoids negative allocations downstream.
  const auto apply = [](wxWindow* w, const wxRect& r) {
    const bool show = (r.width > 1 && r.height > 1);
    if (w->IsShown() != show) w->Show(show);
    if (show) w->SetSize(r);
  };

  apply(tl_, rTL);
  apply(tr_, rTR);
  apply(bl_, rBL);
  apply(br_, rBR);
}

void QuadSplitter::OnSize(wxSizeEvent& event) {
  event.Skip();
  LayoutChildren();
  Refresh();
}

void QuadSplitter::OnPaint(wxPaintEvent&) {
  wxPaintDC dc(this);
  const wxSize client = GetClientSize();
  EnsureInitialSashes(client);

  const auto sashCol = wxSystemSettings::GetColour(wxSYS_COLOUR_3DSHADOW);
  dc.SetPen(*wxTRANSPARENT_PEN);
  dc.SetBrush(wxBrush(sashCol));

  if (vSashPos_ >= 0) dc.DrawRectangle(VerticalSashRect(client));
  if (hSashPos_ >= 0) dc.DrawRectangle(HorizontalSashRect(client));
}

void QuadSplitter::OnLeftDown(wxMouseEvent& event) {
  const wxPoint pt = event.GetPosition();
  const wxSize client = GetClientSize();
  EnsureInitialSashes(client);

  const int dx = std::abs(pt.x - vSashPos_);
  const int dy = std::abs(pt.y - hSashPos_);

  const bool nearV = dx <= kGrabSize;
  const bool nearH = dy <= kGrabSize;

  if (nearV && nearH) {
    drag_ = (dx <= dy) ? DragMode::Vertical : DragMode::Horizontal;
  } else if (nearV) {
    drag_ = DragMode::Vertical;
  } else if (nearH) {
    drag_ = DragMode::Horizontal;
  } else {
    drag_ = DragMode::None;
  }

  if (drag_ != DragMode::None) {
    CaptureMouse();
  }
}

void QuadSplitter::OnLeftUp(wxMouseEvent& event) {
  event.Skip();
  if (HasCapture()) ReleaseMouse();
  drag_ = DragMode::None;
  UpdateHoverCursor(event.GetPosition());
}

void QuadSplitter::OnMotion(wxMouseEvent& event) {
  const wxPoint pt = event.GetPosition();
  const wxSize client = GetClientSize();
  EnsureInitialSashes(client);

  if (drag_ == DragMode::Vertical) {
    vSashPos_ = ClampVSash(pt.x, client);
    LayoutChildren();
    Refresh();
    return;
  }
  if (drag_ == DragMode::Horizontal) {
    hSashPos_ = ClampHSash(pt.y, client);
    LayoutChildren();
    Refresh();
    return;
  }

  UpdateHoverCursor(pt);
  event.Skip();
}

void QuadSplitter::OnLeave(wxMouseEvent& event) {
  event.Skip();
  if (drag_ == DragMode::None) SetCursor(wxNullCursor);
}

void QuadSplitter::UpdateHoverCursor(const wxPoint& pt) {
  const wxSize client = GetClientSize();
  EnsureInitialSashes(client);

  const int dx = std::abs(pt.x - vSashPos_);
  const int dy = std::abs(pt.y - hSashPos_);

  if (dx <= kGrabSize && dy <= kGrabSize) {
    SetCursor(wxCursor(wxCURSOR_SIZING));
  } else if (dx <= kGrabSize) {
    SetCursor(wxCursor(wxCURSOR_SIZEWE));
  } else if (dy <= kGrabSize) {
    SetCursor(wxCursor(wxCURSOR_SIZENS));
  } else {
    SetCursor(wxNullCursor);
  }
}
