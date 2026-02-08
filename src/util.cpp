#include "util.h"

#include <chrono>
#include <atomic>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <system_error>

#include <wx/msgdlg.h>
#include <wx/thread.h>
#include <wx/utils.h>

#ifdef QUARRY_USE_GIO
#include <gio/gio.h>
#endif

namespace fs = std::filesystem;

namespace {
OpResult CanceledResult() { return {.ok = false, .message = "Canceled"}; }

bool IsCanceled(const CancelFn& shouldCancel) { return shouldCancel && shouldCancel(); }

bool LooksLikeUriString(const std::string& s) {
  const auto pos = s.find("://");
  return pos != std::string::npos && pos > 0;
}

#ifdef QUARRY_USE_GIO
struct GioProgressCtx {
  const CancelFn* shouldCancel{nullptr};
  const CopyProgressFn* onProgress{nullptr};
  const CopyBytesProgressFn* onBytes{nullptr};
  GCancellable* cancellable{nullptr};
  std::uintmax_t lastBytes{0};
  std::string label{};
};

void GioProgressCallback(goffset current_num_bytes,
                         goffset total_num_bytes,
                         gpointer user_data) {
  (void)total_num_bytes;
  auto* ctx = static_cast<GioProgressCtx*>(user_data);
  if (!ctx) return;

  if (ctx->shouldCancel && *ctx->shouldCancel && (*ctx->shouldCancel)()) {
    if (ctx->cancellable) g_cancellable_cancel(ctx->cancellable);
    return;
  }

  const std::uintmax_t cur = current_num_bytes > 0 ? static_cast<std::uintmax_t>(current_num_bytes) : 0;
  if (cur >= ctx->lastBytes) {
    const auto delta = cur - ctx->lastBytes;
    ctx->lastBytes = cur;
    if (ctx->onBytes && *ctx->onBytes) (*ctx->onBytes)(delta);
  }
  if (ctx->onProgress && *ctx->onProgress && !ctx->label.empty()) {
    (*ctx->onProgress)(fs::path(ctx->label));
  }
}

OpResult GioDeleteRecursive(GFile* file, GCancellable* cancellable) {
  if (!file) return {.ok = false, .message = "Invalid source."};

  GError* err = nullptr;
  const auto type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable);
  if (type == G_FILE_TYPE_DIRECTORY) {
    GError* enumErr = nullptr;
    GFileEnumerator* en = g_file_enumerate_children(file,
                                                    "standard::name,standard::type",
                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                    cancellable,
                                                    &enumErr);
    if (!en) {
      const wxString msg = enumErr ? wxString::FromUTF8(enumErr->message) : "Unable to enumerate directory.";
      if (enumErr) g_error_free(enumErr);
      return {.ok = false, .message = msg};
    }

    for (;;) {
      if (g_cancellable_is_cancelled(cancellable)) {
        g_object_unref(en);
        return CanceledResult();
      }
      GError* nextErr = nullptr;
      GFileInfo* info = g_file_enumerator_next_file(en, cancellable, &nextErr);
      if (!info) {
        if (nextErr) {
          const wxString msg = wxString::FromUTF8(nextErr->message);
          g_error_free(nextErr);
          g_object_unref(en);
          return {.ok = false, .message = msg};
        }
        break;
      }

      const char* name = g_file_info_get_name(info);
      if (name && *name) {
        GFile* child = g_file_get_child(file, name);
        const auto res = GioDeleteRecursive(child, cancellable);
        g_object_unref(child);
        g_object_unref(info);
        if (!res.ok) {
          g_object_unref(en);
          return res;
        }
      } else {
        g_object_unref(info);
      }
    }

    g_object_unref(en);
  }

  err = nullptr;
  const gboolean ok = g_file_delete(file, cancellable, &err);
  if (!ok) {
    const wxString msg = err ? wxString::FromUTF8(err->message) : "Delete failed.";
    if (err) g_error_free(err);
    return {.ok = false, .message = msg};
  }
  return {.ok = true};
}

OpResult GioCopyRecursive(GFile* src,
                          GFile* dst,
                          GCancellable* cancellable,
                          const CancelFn& shouldCancel,
                          const CopyProgressFn& onProgress,
                          const CopyBytesProgressFn& onBytes) {
  if (!src || !dst) return {.ok = false, .message = "Invalid source or destination."};

  if (IsCanceled(shouldCancel)) {
    g_cancellable_cancel(cancellable);
    return CanceledResult();
  }

  const auto type = g_file_query_file_type(src, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable);
  if (type == G_FILE_TYPE_DIRECTORY) {
    GError* mkErr = nullptr;
    (void)g_file_make_directory_with_parents(dst, cancellable, &mkErr);
    if (mkErr) {
      // Ignore "exists"; otherwise fail.
      if (!g_error_matches(mkErr, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
        const wxString msg = wxString::FromUTF8(mkErr->message);
        g_error_free(mkErr);
        return {.ok = false, .message = msg};
      }
      g_error_free(mkErr);
    }

    GError* enumErr = nullptr;
    GFileEnumerator* en = g_file_enumerate_children(src,
                                                    "standard::name,standard::type",
                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                    cancellable,
                                                    &enumErr);
    if (!en) {
      const wxString msg = enumErr ? wxString::FromUTF8(enumErr->message) : "Unable to enumerate directory.";
      if (enumErr) g_error_free(enumErr);
      return {.ok = false, .message = msg};
    }

    for (;;) {
      if (IsCanceled(shouldCancel) || g_cancellable_is_cancelled(cancellable)) {
        g_cancellable_cancel(cancellable);
        g_object_unref(en);
        return CanceledResult();
      }

      GError* nextErr = nullptr;
      GFileInfo* info = g_file_enumerator_next_file(en, cancellable, &nextErr);
      if (!info) {
        if (nextErr) {
          const wxString msg = wxString::FromUTF8(nextErr->message);
          g_error_free(nextErr);
          g_object_unref(en);
          return {.ok = false, .message = msg};
        }
        break;
      }

      const char* name = g_file_info_get_name(info);
      const auto childType = g_file_info_get_file_type(info);
      if (!name || !*name) {
        g_object_unref(info);
        continue;
      }

      GFile* sChild = g_file_get_child(src, name);
      GFile* dChild = g_file_get_child(dst, name);

      OpResult res;
      if (childType == G_FILE_TYPE_DIRECTORY) {
        res = GioCopyRecursive(sChild, dChild, cancellable, shouldCancel, onProgress, onBytes);
      } else {
        GioProgressCtx ctx;
        ctx.shouldCancel = &shouldCancel;
        ctx.onProgress = &onProgress;
        ctx.onBytes = &onBytes;
        ctx.cancellable = cancellable;
        ctx.lastBytes = 0;
        ctx.label = name;

        if (onProgress) onProgress(fs::path(name));

        GError* copyErr = nullptr;
        const gboolean ok = g_file_copy(sChild,
                                        dChild,
                                        static_cast<GFileCopyFlags>(G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS),
                                        cancellable,
                                        GioProgressCallback,
                                        &ctx,
                                        &copyErr);
        if (!ok) {
          if (copyErr && g_error_matches(copyErr, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_error_free(copyErr);
            res = CanceledResult();
          } else {
            const wxString msg = copyErr ? wxString::FromUTF8(copyErr->message) : "Copy failed.";
            if (copyErr) g_error_free(copyErr);
            res = {.ok = false, .message = msg};
          }
        } else {
          res = {.ok = true};
        }
      }

      g_object_unref(sChild);
      g_object_unref(dChild);
      g_object_unref(info);

      if (!res.ok) {
        g_object_unref(en);
        return res;
      }
    }

    g_object_unref(en);
    return {.ok = true};
  }

  GioProgressCtx ctx;
  ctx.shouldCancel = &shouldCancel;
  ctx.onProgress = &onProgress;
  ctx.onBytes = &onBytes;
  ctx.cancellable = cancellable;
  ctx.lastBytes = 0;
  if (src) {
    char* b = g_file_get_basename(src);
    if (b) {
      ctx.label = b;
      g_free(b);
    }
  }
  if (onProgress && !ctx.label.empty()) onProgress(fs::path(ctx.label));

  GError* copyErr = nullptr;
  const gboolean ok = g_file_copy(src,
                                  dst,
                                  static_cast<GFileCopyFlags>(G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS),
                                  cancellable,
                                  GioProgressCallback,
                                  &ctx,
                                  &copyErr);
  if (!ok) {
    if (copyErr && g_error_matches(copyErr, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_error_free(copyErr);
      return CanceledResult();
    }
    const wxString msg = copyErr ? wxString::FromUTF8(copyErr->message) : "Copy failed.";
    if (copyErr) g_error_free(copyErr);
    return {.ok = false, .message = msg};
  }
  return {.ok = true};
}

OpResult GioMoveAny(const std::string& srcStr,
                    const std::string& dstStr,
                    const CancelFn& shouldCancel,
                    const CopyProgressFn& onProgress,
                    const CopyBytesProgressFn& onBytes) {
  GCancellable* cancellable = g_cancellable_new();
  GFile* src = g_file_new_for_commandline_arg(srcStr.c_str());
  GFile* dst = g_file_new_for_commandline_arg(dstStr.c_str());

  std::atomic_bool done{false};
  std::thread cancelWatcher;
  if (shouldCancel) {
    cancelWatcher = std::thread([&done, cancellable, &shouldCancel]() {
      using namespace std::chrono_literals;
      while (!done.load()) {
        if (shouldCancel()) {
          g_cancellable_cancel(cancellable);
          break;
        }
        std::this_thread::sleep_for(50ms);
      }
    });
  }

  if (IsCanceled(shouldCancel)) {
    g_cancellable_cancel(cancellable);
    g_object_unref(src);
    g_object_unref(dst);
    done.store(true);
    if (cancelWatcher.joinable()) cancelWatcher.join();
    g_object_unref(cancellable);
    return CanceledResult();
  }

  GioProgressCtx ctx;
  ctx.shouldCancel = &shouldCancel;
  ctx.onProgress = &onProgress;
  ctx.onBytes = &onBytes;
  ctx.cancellable = cancellable;
  ctx.lastBytes = 0;

  GError* moveErr = nullptr;
  const gboolean ok = g_file_move(src,
                                  dst,
                                  static_cast<GFileCopyFlags>(G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS),
                                  cancellable,
                                  GioProgressCallback,
                                  &ctx,
                                  &moveErr);
  if (!ok) {
    const bool cancelled = moveErr && g_error_matches(moveErr, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    const bool fallback = moveErr && (g_error_matches(moveErr, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) ||
                                      g_error_matches(moveErr, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED));
    if (cancelled) {
      g_error_free(moveErr);
      g_object_unref(src);
      g_object_unref(dst);
      done.store(true);
      if (cancelWatcher.joinable()) cancelWatcher.join();
      g_object_unref(cancellable);
      return CanceledResult();
    }

    if (fallback) {
      g_error_free(moveErr);
      const auto copyRes = GioCopyRecursive(src, dst, cancellable, shouldCancel, onProgress, onBytes);
      if (!copyRes.ok) {
        g_object_unref(src);
        g_object_unref(dst);
        done.store(true);
        if (cancelWatcher.joinable()) cancelWatcher.join();
        g_object_unref(cancellable);
        return copyRes;
      }
      const auto delRes = GioDeleteRecursive(src, cancellable);
      g_object_unref(src);
      g_object_unref(dst);
      done.store(true);
      if (cancelWatcher.joinable()) cancelWatcher.join();
      g_object_unref(cancellable);
      return delRes;
    }

    const wxString msg = moveErr ? wxString::FromUTF8(moveErr->message) : "Move failed.";
    if (moveErr) g_error_free(moveErr);
    g_object_unref(src);
    g_object_unref(dst);
    done.store(true);
    if (cancelWatcher.joinable()) cancelWatcher.join();
    g_object_unref(cancellable);
    return {.ok = false, .message = msg};
  }

  g_object_unref(src);
  g_object_unref(dst);
  done.store(true);
  if (cancelWatcher.joinable()) cancelWatcher.join();
  g_object_unref(cancellable);
  return {.ok = true};
}

OpResult GioCopyAny(const std::string& srcStr,
                    const std::string& dstStr,
                    const CancelFn& shouldCancel,
                    const CopyProgressFn& onProgress,
                    const CopyBytesProgressFn& onBytes) {
  GCancellable* cancellable = g_cancellable_new();
  GFile* src = g_file_new_for_commandline_arg(srcStr.c_str());
  GFile* dst = g_file_new_for_commandline_arg(dstStr.c_str());

  std::atomic_bool done{false};
  std::thread cancelWatcher;
  if (shouldCancel) {
    cancelWatcher = std::thread([&done, cancellable, &shouldCancel]() {
      using namespace std::chrono_literals;
      while (!done.load()) {
        if (shouldCancel()) {
          g_cancellable_cancel(cancellable);
          break;
        }
        std::this_thread::sleep_for(50ms);
      }
    });
  }

  const auto res = GioCopyRecursive(src, dst, cancellable, shouldCancel, onProgress, onBytes);
  g_object_unref(src);
  g_object_unref(dst);
  done.store(true);
  if (cancelWatcher.joinable()) cancelWatcher.join();
  g_object_unref(cancellable);
  return res;
}
#endif

OpResult CopyRegularFileChunked(const fs::path& src,
                                const fs::path& dst,
                                const CancelFn& shouldCancel,
                                const CopyProgressFn& onProgress,
                                const CopyBytesProgressFn& onBytes) {
  if (IsCanceled(shouldCancel)) return CanceledResult();
  if (onProgress) onProgress(src);

  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);
  if (ec) return {.ok = false, .message = wxString::FromUTF8(ec.message())};

  std::ifstream in(src, std::ios::binary);
  if (!in.is_open()) {
    return {.ok = false, .message = wxString::FromUTF8("Unable to open source file for reading.")};
  }

  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return {.ok = false, .message = wxString::FromUTF8("Unable to open destination file for writing.")};
  }

  static constexpr std::size_t kBufSize = 4 * 1024 * 1024;
  std::string buf;
  buf.resize(kBufSize);

  std::uint64_t chunks = 0;
  while (in) {
    if (IsCanceled(shouldCancel)) {
      out.close();
      std::error_code rmEc;
      fs::remove(dst, rmEc);
      return CanceledResult();
    }

    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto got = in.gcount();
    if (got <= 0) break;

    out.write(buf.data(), got);
    if (!out) {
      return {.ok = false, .message = wxString::FromUTF8("Write failed.")};
    }

    if (onBytes) onBytes(static_cast<std::uintmax_t>(got));
    chunks++;
    if (onProgress && (chunks % 32u) == 0u) onProgress(src);
  }

  out.flush();
  if (!out) return {.ok = false, .message = wxString::FromUTF8("Write failed.")};
  if (onProgress) onProgress(src);
  return {.ok = true};
}

OpResult CopyPathRecursiveImpl(const fs::path& src,
                              const fs::path& dst,
                              const CancelFn& shouldCancel,
                              const CopyProgressFn& onProgress,
                              const CopyBytesProgressFn& onBytes) {
  const auto srcStr0 = src.string();
  const auto dstStr0 = dst.string();
  if (LooksLikeUriString(srcStr0) || LooksLikeUriString(dstStr0)) {
#ifdef QUARRY_USE_GIO
    return GioCopyAny(srcStr0, dstStr0, shouldCancel, onProgress, onBytes);
#else
    return {.ok = false, .message = "Network copy is not available (built without GIO)."};
#endif
  }

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
    if ((progressCounter % 16u) == 0u) onProgress(p);
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
        const auto res = CopyRegularFileChunked(entry.path(), out, shouldCancel, onProgress, onBytes);
        if (!res.ok) return res;
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

  if (fs::is_regular_file(st)) {
    return CopyRegularFileChunked(src, dst, shouldCancel, onProgress, onBytes);
  }

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
  return CopyPathRecursiveImpl(src, dst, CancelFn{}, CopyProgressFn{}, CopyBytesProgressFn{});
}

OpResult CopyPathRecursive(const fs::path& src,
                           const fs::path& dst,
                           const CancelFn& shouldCancel,
                           const CopyProgressFn& onProgress,
                           const CopyBytesProgressFn& onBytes) {
  return CopyPathRecursiveImpl(src, dst, shouldCancel, onProgress, onBytes);
}

OpResult MovePath(const fs::path& src, const fs::path& dst) {
  return MovePath(src, dst, CancelFn{}, CopyProgressFn{}, CopyBytesProgressFn{});
}

OpResult MovePath(const fs::path& src,
                  const fs::path& dst,
                  const CancelFn& shouldCancel,
                  const CopyProgressFn& onProgress,
                  const CopyBytesProgressFn& onBytes) {
  const auto srcStr0 = src.string();
  const auto dstStr0 = dst.string();
  if (LooksLikeUriString(srcStr0) || LooksLikeUriString(dstStr0)) {
#ifdef QUARRY_USE_GIO
    return GioMoveAny(srcStr0, dstStr0, shouldCancel, onProgress, onBytes);
#else
    return {.ok = false, .message = "Network move is not available (built without GIO)."};
#endif
  }

  std::error_code ec;
  fs::rename(src, dst, ec);
  if (!ec) return {.ok = true};

  // Cross-device moves can fail; fall back to copy+delete.
  const auto copyRes = CopyPathRecursiveImpl(src, dst, shouldCancel, onProgress, onBytes);
  if (!copyRes.ok) return copyRes;
  if (IsCanceled(shouldCancel)) return CanceledResult();

  const auto delRes = DeletePath(src);
  if (!delRes.ok) return delRes;
  return {.ok = true};
}

bool PathExistsAny(const fs::path& p) {
  const auto s = p.string();
  if (LooksLikeUriString(s)) {
#ifdef QUARRY_USE_GIO
    GFile* f = g_file_new_for_commandline_arg(s.c_str());
    const gboolean ok = g_file_query_exists(f, nullptr);
    g_object_unref(f);
    return ok != 0;
#else
    return false;
#endif
  }
  std::error_code ec;
  return fs::exists(p, ec);
}

bool IsDirectoryAny(const fs::path& p) {
  const auto s = p.string();
  if (LooksLikeUriString(s)) {
#ifdef QUARRY_USE_GIO
    GFile* f = g_file_new_for_commandline_arg(s.c_str());
    const auto type = g_file_query_file_type(f, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr);
    g_object_unref(f);
    return type == G_FILE_TYPE_DIRECTORY;
#else
    return false;
#endif
  }
  std::error_code ec;
  return fs::is_directory(p, ec);
}

fs::path JoinDirAndNameAny(const fs::path& dir, const std::string& name) {
  const auto base = dir.string();
  if (LooksLikeUriString(base)) {
#ifdef QUARRY_USE_GIO
    GFile* d = g_file_new_for_commandline_arg(base.c_str());
    GFile* c = g_file_get_child(d, name.c_str());
    char* uri = g_file_get_uri(c);
    fs::path out = uri ? fs::path(uri) : fs::path(base + "/" + name);
    if (uri) g_free(uri);
    g_object_unref(c);
    g_object_unref(d);
    return out;
#else
    if (base.empty()) return fs::path(name);
    if (base.back() == '/') return fs::path(base + name);
    return fs::path(base + "/" + name);
#endif
  }
  return dir / fs::path(name);
}

OpResult DeletePath(const fs::path& src) {
  const auto srcStr = src.string();
  if (LooksLikeUriString(srcStr)) {
#ifdef QUARRY_USE_GIO
    GCancellable* cancellable = g_cancellable_new();
    GFile* file = g_file_new_for_commandline_arg(srcStr.c_str());
    const auto res = GioDeleteRecursive(file, cancellable);
    g_object_unref(file);
    g_object_unref(cancellable);
    return res;
#else
    return {.ok = false, .message = "Network delete is not available (built without GIO)."};
#endif
  }

  std::error_code ec;
  fs::remove_all(src, ec);
  if (ec) return {.ok = false, .message = wxString::FromUTF8(ec.message())};
  return {.ok = true};
}

OpResult TrashPath(const fs::path& src) {
  return TrashPath(src, CancelFn{});
}

OpResult TrashPath(const fs::path& src, const CancelFn& shouldCancel) {
  const auto srcStr = src.string();
#ifdef QUARRY_USE_GIO
  // Prefer gio's native trash API when available (works for local and some remote mounts).
  if (IsCanceled(shouldCancel)) return CanceledResult();

  if (LooksLikeUriString(srcStr)) {
    const auto pos = srcStr.find("://");
    const std::string scheme = pos == std::string::npos ? std::string{} : srcStr.substr(0, pos);
    std::string lower;
    lower.reserve(scheme.size());
    for (unsigned char c : scheme) lower.push_back(static_cast<char>(std::tolower(c)));
    if (!lower.empty() && lower != "file") {
      return {.ok = false, .message = "Trash is not supported for remote connections."};
    }
  }

  GCancellable* cancellable = g_cancellable_new();
  std::atomic_bool done{false};
  std::thread cancelWatcher;
  if (shouldCancel) {
    cancelWatcher = std::thread([&done, cancellable, &shouldCancel]() {
      using namespace std::chrono_literals;
      while (!done.load()) {
        if (shouldCancel()) {
          g_cancellable_cancel(cancellable);
          break;
        }
        std::this_thread::sleep_for(50ms);
      }
    });
  }

  GError* err = nullptr;
  GFile* file = g_file_new_for_commandline_arg(srcStr.c_str());
  const gboolean ok = g_file_trash(file, cancellable, &err);
  g_object_unref(file);
  done.store(true);
  if (cancelWatcher.joinable()) cancelWatcher.join();
  g_object_unref(cancellable);
  if (!ok) {
    if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_error_free(err);
      return CanceledResult();
    }
    const bool fallback =
        err && (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) ||
                g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED));
    const wxString msg = err ? wxString::FromUTF8(err->message) : "Trash failed.";
    if (err) g_error_free(err);

    if (!fallback) {
      return {.ok = false, .message = msg};
    }

    // If trash isn't supported, let the caller decide (e.g., prompt to delete permanently).
    return {.ok = false, .message = msg};
  }
  return {.ok = true};
#else
  // Fallback: freedesktop Trash spec implementation via gio command.
  // This is intentionally simple for the MVP.
  const wxString cmd0 = "gio";
  const wxString cmd1 = "trash";
  const wxString cmd2 = wxString(srcStr);

  const wxChar* const argv[] = {cmd0.wc_str(), cmd1.wc_str(), cmd2.wc_str(), nullptr};
  const long rc = wxExecute(argv, wxEXEC_SYNC);
  if (rc == -1) {
    return {.ok = false, .message = "Unable to run gio (is it installed?)"};
  }
  if (rc != 0) {
    return {.ok = false, .message = wxString::Format("gio trash failed (exit code %ld)", rc)};
  }
  return {.ok = true};
#endif
}
