#pragma once

#include <cstddef>
#include <string>

namespace huxerui::detail {

inline std::wstring InstallerFilesInUseNames(const wchar_t* const* fields, std::size_t count, bool paired_fields) {
  std::wstring names;
  if (fields == nullptr) {
    return names;
  }
  // MSI and Restart Manager records alternate metadata and display names.
  const std::size_t step = paired_fields ? 2 : 1;
  for (std::size_t index = step - 1; index < count; index += step) {
    const wchar_t* name = fields[index];
    if (name == nullptr || *name == L'\0') {
      continue;
    }
    if (!names.empty()) {
      names += L'\n';
    }
    names += name;
  }
  return names;
}

} // namespace huxerui::detail
