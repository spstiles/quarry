#pragma once

#include <functional>
#include <filesystem>
#include <string>

#include <wx/window.h>

struct OpResult {
  bool ok{false};
  wxString message;
};

std::string HumanSize(std::uintmax_t bytes);
std::string FormatFileTime(const std::filesystem::file_time_type& ft);

bool ConfirmFileOp(wxWindow* parent,
                   const wxString& action,
                   const std::filesystem::path& src,
                   const std::filesystem::path& dst);

using CancelFn = std::function<bool()>;
using CopyProgressFn = std::function<void(const std::filesystem::path& current)>;
using CopyBytesProgressFn = std::function<void(std::uintmax_t bytesDelta)>;

OpResult CopyPathRecursive(const std::filesystem::path& src, const std::filesystem::path& dst);
OpResult CopyPathRecursive(const std::filesystem::path& src,
                           const std::filesystem::path& dst,
                           const CancelFn& shouldCancel,
                           const CopyProgressFn& onProgress,
                           const CopyBytesProgressFn& onBytes);
OpResult MovePath(const std::filesystem::path& src, const std::filesystem::path& dst);
OpResult MovePath(const std::filesystem::path& src,
                  const std::filesystem::path& dst,
                  const CancelFn& shouldCancel,
                  const CopyProgressFn& onProgress,
                  const CopyBytesProgressFn& onBytes);
OpResult DeletePath(const std::filesystem::path& src);
OpResult TrashPath(const std::filesystem::path& src);
