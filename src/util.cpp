#include "util.h"

#include <chrono>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include <wx/msgdlg.h>
#include <wx/thread.h>
#include <wx/utils.h>

namespace fs = std::filesystem;

namespace {
OpResult CanceledResult() { return {.ok = false, .message = "Canceled"}; }

bool IsCanceled(const CancelFn& shouldCancel) { return shouldCancel && shouldCancel(); }

OpResult CopyPathRecursiveImpl(const fs::path& src,
                              const fs::path& dst,
                              const CancelFn& shouldCancel,
                              const CopyProgressFn& onProgress) {
  std::error_code ec;

  // Avoid pathological case: copying a directory into itself/subdirectory.
  ec.clear();
  const auto srcCanon = fs::weakly_canonical(src, ec);
  const auto srcCanonOk = !ec;
  ec.clear();
  const auto dstCanon = fs::weakly_canonical(dst, ec);
  const auto dstCanonOk = !ec;
  if (srcCanonOk && dstCanonOk) {
    const auto srcStr = srcCanon.native();
    const auto dstStr = dstCanon.native();
    if (dstStr.size() > srcStr.size() && dstStr.compare(0, srcStr.size(), srcStr) == 0 &&
        (dstStr[srcStr.size()] == '/' || dstStr[srcStr.size()] == '\\')) {
      return {.ok = false, .message = "Destination is inside the source folder."};
    }
  }

  if (IsCanceled(shouldCancel)) return CanceledResult();

  ec.clear();
  const auto st = fs::symlink_status(src, ec);
  if (ec) return {.ok = false, .message = wxString::FromUTF8(ec.message())};

  auto yieldSometimes = [](std::uint64_t& counter) {
    counter++;
    // Copy operations may run on a worker thread; never yield from there.
    if ((counter % 256u) == 0u && wxIsMainThread()) wxYieldIfNeeded();
  };

  std::uint64_t yieldCounter = 0;
  std::uint64_t progressCounter = 0;

  auto progressSometimes = [&](const fs::path& p) {
    if (!onProgress) return;
    progressCounter++;
    if ((progressCounter % 128u) == 0u) onProgress(p);
  };

  if (fs::is_directory(st)) {
    ec.clear();
    fs::create_directories(dst, ec);
    if (ec) return {.ok = false, .message = wxString::FromUTF8(ec.message())};

    fs::recursive_directory_iterator it(src, fs::directory_options::skip_permission_denied, ec);
    if (ec) return {.ok = false, .message = wxString::FromUTF8(ec.message())};

    for (const auto& entry : it) {
      if (IsCanceled(shouldCancel)) return CanceledResult();
      yieldSometimes(yieldCounter);
      progressSometimes(entry.path());

      const auto rel = entry.path().lexically_relative(src);
      if (rel.empty()) continue;
      const auto out = dst / rel;

      ec.clear();
      const auto entrySt = entry.symlink_status(ec);
      if (ec) return {.ok = false, .message = wxString::FromUTF8(ec.message())};

      if (fs::is_directory(entrySt)) {
        ec.clear();
        fs::create_directories(out, ec);
        if (ec) return {.ok = false, .message = wxString::FromUTF8(ec.message())};
        continue;
      }

      if (fs::is_regular_file(entrySt)) {
        ec.clear();
        fs::create_directories(out.parent_path(), ec);
        if (ec) return {.ok = false, .message = wxString::FromUTF8(ec.message())};
        if (IsCanceled(shouldCancel)) return CanceledResult();
        ec.clear();
        fs::copy_file(entry.path(), out, fs::copy_options::overwrite_existing, ec);
        if (ec) return {.ok = false, .message = wxString::FromUTF8(ec.message())};
        continue;
      }

      if (fs::is_symlink(entrySt)) {
        ec.clear();
        fs::create_directories(out.parent_path(), ec);
        if (ec) return {.ok = false, .message = wxString::FromUTF8(ec.message())};
        if (IsCanceled(shouldCancel)) return CanceledResult();
        ec.clear();
        fs::copy(entry.path(), out,
                 fs::copy_options::overwrite_existing | fs::copy_options::copy_symlinks, ec);
        if (ec) return {.ok = false, .message = wxString::FromUTF8(ec.message())};
        continue;
      }

      // Skip other special file types for now.
    }

    if (onProgress) onProgress(src);
    return {.ok = true};
  }

  // Regular file (or symlink to file): copy one item.
  ec.clear();
  fs::create_directories(dst.parent_path(), ec);
  if (ec) return {.ok = false, .message = wxString::FromUTF8(ec.message())};
  if (IsCanceled(shouldCancel)) return CanceledResult();

  ec.clear();
  const auto options = fs::copy_options::overwrite_existing | fs::copy_options::copy_symlinks;
  fs::copy(src, dst, options, ec);
  if (ec) return {.ok = false, .message = wxString::FromUTF8(ec.message())};
  if (onProgress) onProgress(src);
  return {.ok = true};
}
}  // namespace

std::string HumanSize(std::uintmax_t bytes) {
  static constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 5) {
    value /= 1024.0;
    unit++;
  }
  char buf[64];
  if (unit == 0) {
    std::snprintf(buf, sizeof(buf), "%llu %s",
                  static_cast<unsigned long long>(bytes), units[unit]);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
  }
  return std::string(buf);
}

std::string FormatFileTime(const fs::file_time_type& ft) {
  using namespace std::chrono;
  if (ft == fs::file_time_type{}) return "";

  // Convert filesystem clock to system_clock (C++20-friendly pattern).
  const auto sctp = time_point_cast<system_clock::duration>(
      ft - fs::file_time_type::clock::now() + system_clock::now());

  const std::time_t tt = system_clock::to_time_t(sctp);
  std::tm tm{};
  localtime_r(&tt, &tm);

  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
  return std::string(buf);
}

bool ConfirmFileOp(wxWindow* parent,
                   const wxString& action,
                   const fs::path& src,
                   const fs::path& dst) {
  const auto message =
      wxString::Format("%s:\n\n%s\n\nTo:\n\n%s", action, src.string(), dst.string());
  return wxMessageBox(message, action, wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, parent) == wxYES;
}

OpResult CopyPathRecursive(const fs::path& src, const fs::path& dst) {
  return CopyPathRecursiveImpl(src, dst, CancelFn{}, CopyProgressFn{});
}

OpResult CopyPathRecursive(const fs::path& src,
                           const fs::path& dst,
                           const CancelFn& shouldCancel,
                           const CopyProgressFn& onProgress) {
  return CopyPathRecursiveImpl(src, dst, shouldCancel, onProgress);
}

OpResult MovePath(const fs::path& src, const fs::path& dst) {
  return MovePath(src, dst, CancelFn{}, CopyProgressFn{});
}

OpResult MovePath(const fs::path& src,
                  const fs::path& dst,
                  const CancelFn& shouldCancel,
                  const CopyProgressFn& onProgress) {
  std::error_code ec;
  fs::rename(src, dst, ec);
  if (!ec) return {.ok = true};

  // Cross-device moves can fail; fall back to copy+delete.
  const auto copyRes = CopyPathRecursiveImpl(src, dst, shouldCancel, onProgress);
  if (!copyRes.ok) return copyRes;
  if (IsCanceled(shouldCancel)) return CanceledResult();

  const auto delRes = DeletePath(src);
  if (!delRes.ok) return delRes;
  return {.ok = true};
}

OpResult DeletePath(const fs::path& src) {
  std::error_code ec;
  fs::remove_all(src, ec);
  if (ec) return {.ok = false, .message = wxString::FromUTF8(ec.message())};
  return {.ok = true};
}

OpResult TrashPath(const fs::path& src) {
  // Prefer the freedesktop Trash spec implementation via gio.
  // This is intentionally simple for the MVP.
  const wxString cmd0 = "gio";
  const wxString cmd1 = "trash";
  const wxString cmd2 = wxString(src.string());

  const wxChar* const argv[] = {cmd0.wc_str(), cmd1.wc_str(), cmd2.wc_str(), nullptr};
  const long rc = wxExecute(argv, wxEXEC_SYNC);
  if (rc == -1) {
    return {.ok = false, .message = "Unable to run gio (is it installed?)"};
  }
  if (rc != 0) {
    return {.ok = false, .message = wxString::Format("gio trash failed (exit code %ld)", rc)};
  }
  return {.ok = true};
}
