#include "NavIcons.h"

#include <wx/bitmap.h>
#include <wx/image.h>

namespace {
const char* SvgFor(NavIcon icon) {
  // Simple monochrome icons (Material-style). We recolor after rasterization.
  switch (icon) {
    case NavIcon::Back:
      return R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill="#000" d="M15.41 7.41 14 6l-6 6 6 6 1.41-1.41L10.83 12z"/></svg>)svg";
    case NavIcon::Forward:
      return R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill="#000" d="M8.59 16.59 10 18l6-6-6-6-1.41 1.41L13.17 12z"/></svg>)svg";
    case NavIcon::Up:
      return R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill="#000" d="M7.41 15.41 12 10.83l4.59 4.58L18 14l-6-6-6 6z"/></svg>)svg";
    case NavIcon::Refresh:
      return R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill="#000" d="M17.65 6.35A7.95 7.95 0 0 0 12 4V1L7 6l5 5V7a5 5 0 1 1-5 5H5a7 7 0 1 0 12.65-5.65z"/></svg>)svg";
    case NavIcon::Home:
      return R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill="#000" d="M10 20v-6h4v6h5v-8h3L12 3 2 12h3v8z"/></svg>)svg";
  }
  return "";
}

wxBitmap TintMonochrome(const wxBitmap& src, const wxColour& color) {
  if (!src.IsOk()) return src;

  wxImage img = src.ConvertToImage();
  if (!img.HasAlpha()) img.InitAlpha();

  const int w = img.GetWidth();
  const int h = img.GetHeight();

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const unsigned char a = img.GetAlpha(x, y);
      if (a == 0) continue;
      img.SetRGB(x, y, color.Red(), color.Green(), color.Blue());
    }
  }

  return wxBitmap(img);
}
} // namespace

wxBitmapBundle MakeNavIconBundle(NavIcon icon, const wxSize& size, const wxColour& color) {
  // Provide a reasonable default size definition; wx will scale as needed.
  const wxSize sizeDef = size.IsFullySpecified() ? size : wxSize(24, 24);
  const auto bundle = wxBitmapBundle::FromSVG(SvgFor(icon), sizeDef);
  const auto bmp = bundle.GetBitmap(sizeDef);
  return wxBitmapBundle::FromBitmap(TintMonochrome(bmp, color));
}

