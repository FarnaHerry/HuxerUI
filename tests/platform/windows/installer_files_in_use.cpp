#include <catch2/catch_amalgamated.hpp>

#include <iterator>

#include "windows_installer_files_in_use.h"

TEST_CASE("Installer files-in-use details show MSI application names without metadata") {
  const wchar_t* const fields[] = {L"42", L"Editor", L"43", nullptr, L"44", L"", L"45", L"应用", L"46"};
  CHECK(huxerui::detail::InstallerFilesInUseNames(fields, std::size(fields), true) == L"Editor\n应用");
}

TEST_CASE("Installer files-in-use details accept flat notifications and empty payloads") {
  const wchar_t* const fields[] = {L"Editor", L"", L"Terminal"};
  CHECK(huxerui::detail::InstallerFilesInUseNames(fields, std::size(fields), false) == L"Editor\nTerminal");
  CHECK(huxerui::detail::InstallerFilesInUseNames(nullptr, 0, true).empty());
}
