#include <catch2/catch_amalgamated.hpp>

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <huxerui/huxerui.h>

#include "win32_application_runner.h"

namespace huxerui::test {
namespace {

void Await(const std::function<bool()>& condition,
           std::source_location location = std::source_location::current()) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!condition()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      FAIL("Timed out waiting for native condition at line " << location.line());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

struct ProbeData {
  HWND window = nullptr;
  volatile LONG ready = 0;
  volatile LONG clicks = 0;
};

LRESULT CALLBACK ProbeProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
  auto* data = reinterpret_cast<ProbeData*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    data = static_cast<ProbeData*>(reinterpret_cast<CREATESTRUCTW*>(l_param)->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
  }
  if (message == WM_LBUTTONDOWN && data) InterlockedIncrement(&data->clicks);
  if (message == WM_TIMER) DestroyWindow(window);
  if (message == WM_DESTROY) PostQuitMessage(0);
  return DefWindowProcW(window, message, w_param, l_param);
}

struct ProbeProcess {
  HANDLE mapping = nullptr;
  ProbeData* data = nullptr;
  PROCESS_INFORMATION process{};
  POINT cursor{};
  HWND foreground = GetForegroundWindow();

  ProbeProcess() { GetPhysicalCursorPos(&cursor); }
  ~ProbeProcess() {
    if (data && data->window) PostMessageW(data->window, WM_CLOSE, 0, 0);
    if (process.hProcess) {
      if (WaitForSingleObject(process.hProcess, 5000) == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 1);
      CloseHandle(process.hProcess);
    }
    if (process.hThread) CloseHandle(process.hThread);
    if (data) UnmapViewOfFile(data);
    if (mapping) CloseHandle(mapping);
    SetPhysicalCursorPos(cursor.x, cursor.y);
    if (IsWindow(foreground)) SetForegroundWindow(foreground);
  }

  void Start() {
    const std::wstring name = L"Local\\HuxerUIPetProbe-" + std::to_wstring(GetCurrentProcessId());
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ProbeData), name.c_str());
    REQUIRE(mapping != nullptr);
    data = static_cast<ProbeData*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ProbeData)));
    REQUIRE(data != nullptr);
    *data = ProbeData{};
    wchar_t executable[32768]{};
    REQUIRE(GetModuleFileNameW(nullptr, executable, 32768) != 0);
    std::wstring command = L"\"" + std::wstring(executable) +
        L"\" [win32-pet-input-target] --name " + name + L" --reporter compact";
    STARTUPINFOW startup{sizeof(STARTUPINFOW)};
    REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process));
    Await([this] { return InterlockedCompareExchange(&data->ready, 0, 0) != 0; });
  }
};

void Click(POINT point) {
  REQUIRE(SetCursorPos(point.x, point.y));
  INPUT input[2]{};
  input[0].type = INPUT_MOUSE;
  input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
  input[1].type = INPUT_MOUSE;
  input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
  REQUIRE(SendInput(2, input, sizeof(INPUT)) == 2);
}

COLORREF DesktopPixel(POINT point) {
  const HDC screen = GetDC(nullptr);
  const HDC dc = CreateCompatibleDC(screen);
  const HBITMAP bitmap = CreateCompatibleBitmap(screen, 1, 1);
  const HGDIOBJ previous = SelectObject(dc, bitmap);
  const bool copied = BitBlt(dc, 0, 0, 1, 1, screen, point.x, point.y, SRCCOPY | CAPTUREBLT) != FALSE;
  const COLORREF pixel = copied ? GetPixel(dc, 0, 0) : CLR_INVALID;
  SelectObject(dc, previous);
  DeleteObject(bitmap);
  DeleteDC(dc);
  ReleaseDC(nullptr, screen);
  return pixel;
}

bool HasInputDesktop() {
  USEROBJECTFLAGS flags{};
  DWORD needed = 0;
  if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &flags, sizeof(flags), &needed) ||
      (flags.dwFlags & WSF_VISIBLE) == 0) return false;
  const HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!input) return false;
  wchar_t input_name[256]{};
  wchar_t thread_name[256]{};
  const bool same = GetUserObjectInformationW(input, UOI_NAME, input_name, sizeof(input_name), &needed) &&
      GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
          thread_name, sizeof(thread_name), &needed) && std::wstring_view(input_name) == thread_name;
  CloseDesktop(input);
  return same;
}

struct NativePetState {
  std::optional<WindowHandle> window;
  std::optional<ApplicationHandle> application;
  std::atomic<bool> ready{false};
  std::atomic<int> clicks{0};
  std::atomic<int> cancels{0};
};

View NativePetContent() {
  if (UseEnvironment<WindowIdentity>().key == "modal") return Text("Native modal settings");
  const auto state = UseService<NativePetState>();
  state->window = UseWindow();
  state->application = UseApplication();
  state->ready.store(true);
  return Canvas([](PaintContext& paint, Size) {
    paint.DrawRect({40.0F, 40.0F, 120.0F, 120.0F}, Color::Rgb(240, 60, 30));
    paint.DrawRect({20.0F, 40.0F, 20.0F, 120.0F}, Color::Rgb(240, 60, 30, 0.5F));
  }).With(Indication{}, DragGesture{}).OnClick([state] { ++state->clicks; })
      .On<DragEvents::Started>([state](const DragEvent&) { static_cast<void>(state->window->BeginMove()); })
      .On<ViewEvents::Pointer>([state](const PointerEvent& event) {
        if (event.type == PointerEventType::Cancel) ++state->cancels;
      });
}

struct NativePetRunner {
  std::thread thread;
  UiThreadDispatcher post;
  std::atomic<HWND> native{nullptr};
  std::atomic<bool> finished{false};
  std::exception_ptr failure;
  std::shared_ptr<NativePetState> state;

  explicit NativePetRunner(std::shared_ptr<NativePetState> value) : state(std::move(value)) {}

  ~NativePetRunner() {
    if (thread.joinable()) {
      if (state->ready.load() && !finished.load()) post([value = state] { value->application->Quit(); });
      else if (native.load()) PostMessageW(native.load(), WM_CLOSE, 0, 0);
      thread.join();
    }
  }

  void Invoke(std::function<void()> callback) {
    auto done = std::make_shared<std::promise<void>>();
    auto result = done->get_future();
    post([callback = std::move(callback), done] {
      try { callback(); done->set_value(); }
      catch (...) { done->set_exception(std::current_exception()); }
    });
    REQUIRE(result.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
    result.get();
  }
};

} // namespace

TEST_CASE("Win32PetInputTarget", "[.][win32-pet-input-target]") {
  int count = 0;
  wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  std::wstring name;
  for (int index = 1; index + 1 < count; ++index) {
    if (std::wstring_view(arguments[index]) == L"--name") name = arguments[index + 1];
  }
  LocalFree(arguments);
  if (!name.starts_with(L"Local\\HuxerUIPetProbe-")) SKIP("Run through [win32-native-pet]");
  const HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
  REQUIRE(mapping != nullptr);
  auto* data = static_cast<ProbeData*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ProbeData)));
  REQUIRE(data != nullptr);
  const HBRUSH background = CreateSolidBrush(RGB(30, 100, 160));
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = ProbeProcedure;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hbrBackground = background;
  window_class.lpszClassName = L"HuxerUIPetInputTarget";
  REQUIRE(RegisterClassW(&window_class) != 0);
  const HWND window = CreateWindowExW(0, window_class.lpszClassName, L"HuxerUI input test target", WS_POPUP,
      50, 50, 900, 750, nullptr, nullptr, window_class.hInstance, data);
  REQUIRE(window != nullptr);
  ShowWindow(window, SW_SHOW);
  REQUIRE(SetWindowPos(window, HWND_TOPMOST, 50, 50, 900, 750, SWP_SHOWWINDOW));
  SetForegroundWindow(window);
  UpdateWindow(window);
  SetTimer(window, 1, 30000, nullptr);
  data->window = window;
  InterlockedExchange(&data->ready, 1);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
  DeleteObject(background);
  UnmapViewOfFile(data);
  CloseHandle(mapping);
}

TEST_CASE("Win32FloatingPetPreservesDesktopAlphaFocusAndCrossProcessInput", "[.][win32-native-pet]") {
  if (!HasInputDesktop()) SKIP("Native pointer and focus checks require the visible input desktop");
  ProbeProcess probe;
  probe.Start();
  RECT target_bounds{};
  REQUIRE(GetWindowRect(probe.data->window, &target_bounds));
  Click({target_bounds.left + 20, target_bounds.top + 20});
  INFO("Input target " << probe.data->window << "; foreground " << GetForegroundWindow()
       << "; received clicks " << InterlockedCompareExchange(&probe.data->clicks, 0, 0));
  Await([&] { return GetForegroundWindow() == probe.data->window; });
  REQUIRE(SetWindowPos(probe.data->window, HWND_NOTOPMOST, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
  const auto state = std::make_shared<NativePetState>();
  const Application application{NativePetContent, {
      .window = {.title = "HuxerUI Native Pet", .initial_size = {200.0F, 200.0F},
          .initial_position = Point{100.0F, 100.0F}, .presentation = WindowPresentation::Utility,
          .transparent_surface = true, .show_without_activation = true, .topmost = true},
      .show_debug_overlay = false,
      .application_hooks = {[state](ApplicationContext& context) { context.Provide(state); }},
  }};
  NativePetRunner runner(state);
  runner.thread = std::thread([&] {
    try {
      static_cast<void>(detail::RunWin32PlatformApplication(application, [&](UiThreadDispatcher post, HWND window) {
        runner.post = std::move(post);
        runner.native.store(window);
      }));
    } catch (...) { runner.failure = std::current_exception(); }
    runner.finished.store(true);
  });
  Await([&] { return runner.state->ready.load() || runner.finished.load(); });
  if (runner.finished.load() && runner.failure) std::rethrow_exception(runner.failure);
  const HWND native = runner.native.load();
  REQUIRE(native != nullptr);
  const LONG_PTR style = GetWindowLongPtrW(native, GWL_EXSTYLE);
  REQUIRE((style & (WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST)) ==
      (WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST));
  RECT client{};
  REQUIRE(GetClientRect(native, &client));
  const auto point = [native, client](int x, int y) {
    POINT value{x * client.right / 200, y * client.bottom / 200};
    ClientToScreen(native, &value);
    return value;
  };
  const POINT corner = point(10, 10);
  const POINT body = point(100, 100);
  const POINT edge = point(30, 100);
  const auto presentation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  Await([&] {
    if (runner.finished.load()) {
      if (runner.failure) std::rethrow_exception(runner.failure);
      FAIL("Native pet application stopped before its first presentation");
    }
    if (std::chrono::steady_clock::now() >= presentation_deadline) {
      FAIL("Native pet presentation: pixel " << DesktopPixel(body) << "; visible " << IsWindowVisible(native)
           << "; client " << client.right << "x" << client.bottom << "; target " << WindowFromPoint(body)
           << "; expected " << native);
    }
    return DesktopPixel(body) == RGB(240, 60, 30);
  });
  REQUIRE(DesktopPixel(corner) == RGB(30, 100, 160));
  const auto blended = DesktopPixel(edge);
  REQUIRE(GetRValue(blended) == Catch::Approx(135).margin(2));
  REQUIRE(GetGValue(blended) == Catch::Approx(80).margin(2));
  REQUIRE(GetBValue(blended) == Catch::Approx(95).margin(2));
  REQUIRE(GetForegroundWindow() == probe.data->window);
  const LONG before = InterlockedCompareExchange(&probe.data->clicks, 0, 0);
  Click(corner);
  Await([&] { return InterlockedCompareExchange(&probe.data->clicks, 0, 0) == before + 1; });
  Click(body);
  Await([&] { return runner.state->clicks.load() == 1; });
  REQUIRE(GetForegroundWindow() == probe.data->window);
  WindowRequestResult result{};
  runner.Invoke([&] { result = runner.state->window->SetInputPassthrough(true); });
  REQUIRE(result == WindowRequestResult::Submitted);
  Click(body);
  Await([&] { return InterlockedCompareExchange(&probe.data->clicks, 0, 0) == before + 2; });
  REQUIRE(DesktopPixel(body) == RGB(240, 60, 30));
  runner.Invoke([&] { result = runner.state->window->SetInputPassthrough(false); });
  REQUIRE(result == WindowRequestResult::Submitted);
  Click(body);
  Await([&] { return runner.state->clicks.load() == 2; });

  INPUT down{};
  down.type = INPUT_MOUSE;
  down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
  REQUIRE(SendInput(1, &down, sizeof(INPUT)) == 1);
  const DWORD ui_thread = GetWindowThreadProcessId(native, nullptr);
  GUITHREADINFO capture{sizeof(GUITHREADINFO)};
  Await([&] { return GetGUIThreadInfo(ui_thread, &capture) && capture.hwndCapture == native; });
  runner.Invoke([&] { result = runner.state->window->SetInputPassthrough(true); });
  REQUIRE(result == WindowRequestResult::Submitted);
  REQUIRE(GetGUIThreadInfo(ui_thread, &capture));
  REQUIRE(capture.hwndCapture == nullptr);
  REQUIRE(runner.state->cancels.load() >= 1);
  down.mi.dwFlags = MOUSEEVENTF_LEFTUP;
  REQUIRE(SendInput(1, &down, sizeof(INPUT)) == 1);

  std::optional<WindowHandle> modal;
  runner.Invoke([&] {
    modal = runner.state->application->OpenWindow(
        {.title = "HuxerUI Native Pet Modal", .key = "modal", .initial_size = {240.0F, 160.0F}},
        {.owner = runner.state->window, .modal = true});
    static_cast<void>(runner.state->window->SetInputPassthrough(false));
  });
  REQUIRE_FALSE(IsWindowEnabled(native));
  runner.Invoke([&] { modal->Retire(); });
  Await([&] { return IsWindowEnabled(native) != FALSE; });
  REQUIRE_THROWS_AS(runner.Invoke([&] {
    static_cast<void>(runner.state->window->SetClientSize({5000.0F, 200.0F}));
  }), std::invalid_argument);

  REQUIRE(SetCursorPos(body.x, body.y));
  down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
  REQUIRE(SendInput(1, &down, sizeof(INPUT)) == 1);
  Await([&] { return GetGUIThreadInfo(ui_thread, &capture) && capture.hwndCapture == native; });
  REQUIRE(SetCursorPos(body.x + 30, body.y));
  Await([&] { return GetGUIThreadInfo(ui_thread, &capture) && (capture.flags & GUI_INMOVESIZE) != 0; });
  REQUIRE(SetCursorPos(body.x + 90, body.y + 45));
  down.mi.dwFlags = MOUSEEVENTF_LEFTUP;
  REQUIRE(SendInput(1, &down, sizeof(INPUT)) == 1);
  Await([&] { return GetGUIThreadInfo(ui_thread, &capture) && (capture.flags & GUI_INMOVESIZE) == 0; });
  std::optional<Point> moved_origin;
  runner.Invoke([&] { moved_origin = runner.state->window->Position(); });
  REQUIRE(moved_origin.has_value());
  REQUIRE(moved_origin->x > 110.0F);
  REQUIRE(moved_origin->y > 105.0F);

  runner.Invoke([&] {
    result = runner.state->window->SetPosition({180.0F, 160.0F});
    const auto origin = runner.state->window->Position();
    if (!origin || std::abs(origin->x - 180.0F) > 1.0F || std::abs(origin->y - 160.0F) > 1.0F) {
      throw std::runtime_error("HuxerUI floating-window position did not round trip");
    }
    static_cast<void>(runner.state->window->SetClientSize({220.0F, 180.0F}));
    static_cast<void>(runner.state->window->SetTopmost(false));
  });
  REQUIRE(result == WindowRequestResult::Submitted);
  REQUIRE((GetWindowLongPtrW(native, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0);
  const DWORD gdi_baseline = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  runner.Invoke([&] { runner.state->window->Retire(); });
  Await([&] { return !IsWindow(native); });
  runner.Invoke([&] { result = runner.state->window->SetInputPassthrough(true); });
  REQUIRE(result == WindowRequestResult::Disconnected);
  for (int cycle = 0; cycle < 4; ++cycle) {
    runner.Invoke([&] {
      runner.state->window = runner.state->application->OpenWindow({
          .title = "HuxerUI Native Pet Reopened", .initial_size = {200.0F, 200.0F},
          .initial_position = Point{100.0F, 100.0F}, .presentation = WindowPresentation::Utility,
          .transparent_surface = true, .show_without_activation = true, .topmost = true});
    });
    const HWND reopened = FindWindowW(nullptr, L"HuxerUI Native Pet Reopened");
    REQUIRE(reopened != nullptr);
    Await([&] { return DesktopPixel(body) == RGB(240, 60, 30); });
    runner.Invoke([&] { runner.state->window->Retire(); });
    Await([&] { return !IsWindow(reopened); });
  }
  REQUIRE(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdi_baseline + 4);
  runner.Invoke([&] { runner.state->application->Quit(); });
  Await([&] { return runner.finished.load(); });
  runner.thread.join();
  if (runner.failure) std::rethrow_exception(runner.failure);
}

} // namespace huxerui::test
