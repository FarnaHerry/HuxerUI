#include <windows.h>
#include <huxerui/huxerui.h>

#include <memory>
#include <type_traits>

namespace {

class WindowsFirstRuntime : public huxerui::Runtime {
  std::unique_ptr<huxerui::UiWindow> CreateUiWindow(huxerui::ViewFactory, const huxerui::WindowOptions&) override {
    return {};
  }
};

static_assert(std::is_same_v<
    decltype(CreateWindow(nullptr, nullptr, 0, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr)), HWND>);

}
