#pragma once

#include <wx/bmpbndl.h>
#include <wx/colour.h>
#include <wx/gdicmn.h>

enum class NavIcon {
  Back,
  Forward,
  Up,
  Top,
  Refresh,
  Home,
};

wxBitmapBundle MakeNavIconBundle(NavIcon icon, const wxSize& size, const wxColour& color);
