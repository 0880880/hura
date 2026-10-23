#pragma once

#include "export.hpp"
#include <filesystem>
#include <stdint.h>
#include <string_view>
#include <vector>

#include <mio/mmap.hpp>

struct Import {
  std::vector<std::string_view> packages;
};

class Module {
private:
  std::vector<std::streamoff> starts;
  mio::mmap_source mm;

public:
  const std::filesystem::path path;
  std::string_view view;
  std::vector<Export> exports = {};
  std::vector<Import> imports = {};

  Module(std::filesystem::path &path) : path(path) {
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) == 0 && !ec) {
      return;
    }
    if (ec) {
      throw std::runtime_error("Failed to load file.");
    }
    mm.map(path.native(), ec);
    view = std::string_view(mm.data(), mm.size());

    if (!view.empty()) {
      starts.reserve(view.size() / 64);
    }

    for (size_t i = 0; i < view.size(); ++i) {
      if (view[i] == '\n') {
        starts.push_back(static_cast<std::streamoff>(i + 1));
      }
    }
    if (!starts.empty() &&
        starts.back() == static_cast<std::streamoff>(view.size())) {
      starts.pop_back();
    }
  }

  std::string_view get_line(size_t line) const;
};

namespace Compiler {

Module &get_module();

void schedule(std::filesystem::path &path);

void start();

}; // namespace Compiler
