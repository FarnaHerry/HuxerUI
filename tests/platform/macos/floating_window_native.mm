#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <catch2/catch_amalgamated.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <huxerui/huxerui.h>

namespace huxerui::test {
namespace {

CGFloat MacPrimaryScreenHeight() {
  NSArray<NSScreen*>* screens = NSScreen.screens;
  return screens.count == 0 ? 0.0 : screens.firstObject.frame.size.height;
}

/// Captures one window's composited content, including its per-pixel alpha, at nominal resolution.
struct WindowPixels {
  std::vector<std::uint8_t> rgba;
  std::size_t width = 0;
  std::size_t height = 0;
  bool valid = false;

  explicit WindowPixels(NSWindow* window) {
    if (window == nil) return;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGImageRef image = CGWindowListCreateImage(
        CGRectNull, kCGWindowListOptionIncludingWindow, window.windowNumber, kCGWindowImageNominalResolution
    );
#pragma clang diagnostic pop
    if (image == nullptr) return;
    width = CGImageGetWidth(image);
    height = CGImageGetHeight(image);
    if (width > 0 && height > 0 && width <= 4096 && height <= 4096) {
      rgba.assign(width * height * 4, 0);
      CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
      CGContextRef bitmap =
          CGBitmapContextCreate(rgba.data(), width, height, 8, width * 4, space, kCGImageAlphaPremultipliedLast);
      if (bitmap != nullptr) {
        CGContextDrawImage(bitmap, CGRectMake(0.0, 0.0, static_cast<CGFloat>(width), static_cast<CGFloat>(height)),
                           image);
        CFRelease(bitmap);
        valid = true;
      }
      CGColorSpaceRelease(space);
    }
    CGImageRelease(image);
  }

  /// Samples the window point in top-left-based view coordinates. The drawn fixture is vertically symmetric at the
  /// sampled points, so the reading is stable against the drawing context's row order.
  std::array<std::uint8_t, 4> At(float x, float y) const {
    if (!valid) return {0, 0, 0, 0};
    const auto column = static_cast<std::size_t>(std::clamp<float>(x, 0.0F, static_cast<float>(width - 1)));
    const auto row = static_cast<std::size_t>(std::clamp<float>(y, 0.0F, static_cast<float>(height - 1)));
    const std::size_t index = (row * width + column) * 4;
    return {rgba[index], rgba[index + 1], rgba[index + 2], rgba[index + 3]};
  }
};

NSEvent* MouseSyntheticEvent(NSWindow* window, NSPoint view_point, NSEventType type, CGFloat pressure) {
  NSView* content = window.contentView;
  const CGFloat height = content == nil ? 0.0 : content.bounds.size.height;
  return [NSEvent mouseEventWithType:type
                            location:NSMakePoint(view_point.x, height - view_point.y)
                       modifierFlags:0
                           timestamp:0
                        windowNumber:window.windowNumber
                             context:nil
                         eventNumber:0
                          clickCount:1
                            pressure:pressure];
}

/// Delivers a pointer sequence directly to the window's content view without synthetic HID events.
void DeliverPointer(NSWindow* window, NSEventType type, NSPoint view_point) {
  NSView* content = window.contentView;
  if (content == nil) return;
  const CGFloat pressure = type == NSEventTypeLeftMouseUp ? 0.0 : 1.0;
  NSEvent* event = MouseSyntheticEvent(window, view_point, type, pressure);
  switch (type) {
  case NSEventTypeLeftMouseDown: [content mouseDown:event]; break;
  case NSEventTypeLeftMouseDragged: [content mouseDragged:event]; break;
  case NSEventTypeLeftMouseUp: [content mouseUp:event]; break;
  default: break;
  }
}

void ClickViewPoint(NSWindow* window, NSPoint view_point) {
  DeliverPointer(window, NSEventTypeLeftMouseDown, view_point);
  DeliverPointer(window, NSEventTypeLeftMouseUp, view_point);
}

struct NativeWatch {
  __weak NSWindow* window;
  __weak NSView* canvas;

  explicit NativeWatch(NSWindow* native) : window(native), canvas(native.contentView) {}

  NSWindow* Window() { return window; }
  bool Released() { return window == nil && canvas == nil; }
};

struct FloatingScenario {
  std::vector<std::unique_ptr<NativeWatch>> watches;
  std::optional<ApplicationHandle> application;
  std::optional<WindowHandle> primary;
  std::optional<WindowHandle> pet;
  std::optional<WindowHandle> modal;
  std::function<bool()> advance;
  std::exception_ptr failure;
  std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
  WindowRequestResult begin_move_result = WindowRequestResult::Disconnected;
  Point drag_origin{};
  bool drag_origin_valid = false;
  int clicks = 0;
  int cancels = 0;
  int drag_cancels = 0;
  int pet_compositions = 0;
  int modal_compositions = 0;
  bool completed = false;

  void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("HuxerUI " + message);
  }

  void Watch(const std::string& title) {
    for (NSWindow* window in NSApplication.sharedApplication.windows) {
      if (title == window.title.UTF8String) {
        watches.push_back(std::make_unique<NativeWatch>(window));
        return;
      }
    }
    throw std::runtime_error("HuxerUI native floating-window was not found");
  }
};

FloatingScenario* current_scenario = nullptr;

View FloatingPetContent(bool pet, bool modal = false) {
  auto& scenario = *current_scenario;
  if (modal) {
    ++scenario.modal_compositions;
    return Text("Native modal settings");
  }
  if (pet) {
    ++scenario.pet_compositions;
    scenario.pet = UseWindow();
    scenario.application = UseApplication();
    return Canvas([](PaintContext& paint, Size) {
      paint.DrawRect({40.0F, 40.0F, 120.0F, 120.0F}, Color::Rgb(240, 60, 30));
      paint.DrawRect({20.0F, 40.0F, 20.0F, 120.0F}, Color::Rgb(240, 60, 30, 0.5F));
    })
        .With(Indication{}, DragGesture{})
        .OnClick([&scenario] { ++scenario.clicks; })
        .On<DragEvents::Started>([&scenario](const DragEvent&) {
          scenario.begin_move_result = scenario.pet->BeginMove();
        })
        .On<DragEvents::Canceled>([&scenario](const DragEvent&) { ++scenario.drag_cancels; })
        .On<ViewEvents::Pointer>([&scenario](const PointerEvent& event) {
          if (event.type == PointerEventType::Cancel) ++scenario.cancels;
        });
  }
  scenario.primary = UseWindow();
  scenario.application = UseApplication();
  return Text("Floating primary");
}

View FloatingPetRoot() { return FloatingPetContent(false); }

void RunFloatingScenario(FloatingScenario& scenario) {
  current_scenario = &scenario;
  REQUIRE(NSThread.isMainThread);
  NSTimer* timer = [NSTimer scheduledTimerWithTimeInterval:0.01 repeats:YES block:^(NSTimer* sender) {
    @autoreleasepool {
      auto& scenario = *current_scenario;
      try {
        scenario.Check(std::chrono::steady_clock::now() < scenario.deadline, "native floating-window test timed out");
        if (!scenario.advance()) return;
        scenario.completed = true;
      } catch (...) {
        scenario.failure = std::current_exception();
      }
      [sender invalidate];
      if (scenario.application) scenario.application->Quit();
    }
  }];
  std::exception_ptr runner_failure;
  try {
    const Application application{
        FloatingPetRoot,
        {
            .window = {.title = "HuxerUI native floating primary", .initial_size = {320.0F, 240.0F}},
            .show_debug_overlay = false,
        },
    };
    @autoreleasepool {
      scenario.Check(detail::RunPlatformApplication(application) == 0, "native application returned a failure code");
    }
  } catch (...) {
    runner_failure = std::current_exception();
  }
  [timer invalidate];
  current_scenario = nullptr;
  if (scenario.failure) std::rethrow_exception(scenario.failure);
  if (runner_failure) std::rethrow_exception(runner_failure);
  REQUIRE(scenario.completed);
  // AppKit and main-queue blocks release their final autoreleased references on a later native turn.
  const auto release_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  bool released = false;
  do {
    @autoreleasepool {
      [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode
                            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
      released = std::all_of(scenario.watches.begin(), scenario.watches.end(),
                             [](const auto& watch) { return watch->Released(); });
    }
  } while (!released && std::chrono::steady_clock::now() < release_deadline);
  for (std::size_t index = 0; index != scenario.watches.size(); ++index) {
    INFO(index);
    REQUIRE(scenario.watches[index]->Released());
  }
}

bool Near(float value, float expected, float margin) {
  return std::abs(value - expected) <= margin;
}

} // namespace

TEST_CASE("MacNativeFloatingUtilityWindowsPresentAlphaGeometryAndStacking", "[macos-native]") {
  FloatingScenario scenario;
  const WindowCapabilities expected_capabilities{
      .transparent_surface = true, .utility_presentation = true, .passive_display = true,
      .topmost = true, .geometry = true, .native_move = true, .input_passthrough = true,
  };
  const auto pet_options = [] {
    return WindowOptions{
        .title = "HuxerUI native pet",
        .initial_size = {200.0F, 200.0F},
        .initial_position = Point{120.0F, 140.0F},
        .presentation = WindowPresentation::Utility,
        .transparent_surface = true,
        .show_without_activation = true,
        .topmost = true,
    };
  };
  int phase = 0;
  scenario.advance = [&]() {
    auto& checks = scenario;
    switch (phase) {
    case 0: {
      if (!checks.application || !checks.primary) return false;
      checks.pet = checks.application->OpenWindow([] { return FloatingPetContent(true); }, pet_options());
      checks.Watch("HuxerUI native pet");
      checks.Watch("HuxerUI native floating primary");
      ++phase;
      return false;
    }
    case 1: {
      if (checks.pet_compositions == 0) return false;
      NSWindow* pet = checks.watches[0]->Window();
      NSWindow* primary = checks.watches[1]->Window();
      checks.Check(pet != nil, "pet window disappeared before verification");
      checks.Check([pet isKindOfClass:NSPanel.class], "utility presentation did not use an NSPanel");
      const NSWindowStyleMask style = pet.styleMask;
      checks.Check((style & NSWindowStyleMaskTitled) == 0, "utility window kept a native title bar");
      checks.Check((style & NSWindowStyleMaskResizable) == 0, "utility window stayed resizable");
      checks.Check((style & NSWindowStyleMaskNonactivatingPanel) != 0, "passive display missed the nonactivating style");
      const NSWindowCollectionBehavior behavior = pet.collectionBehavior;
      checks.Check((behavior & NSWindowCollectionBehaviorIgnoresCycle) != 0, "utility window joined window cycling");
      checks.Check((behavior & NSWindowCollectionBehaviorFullScreenAuxiliary) != 0,
                   "utility window could not share a full-screen space");
      checks.Check(!pet.isOpaque, "transparent surface stayed opaque");
      checks.Check([pet.backgroundColor isEqual:[NSColor clearColor]], "transparent surface kept an opaque background");
      checks.Check(!pet.hasShadow, "transparent surface kept a window shadow");
      checks.Check(pet.level == NSFloatingWindowLevel, "topmost creation did not apply the floating level");
      checks.Check(!pet.isKeyWindow, "passive display stole key status");
      checks.Check(primary.isKeyWindow, "passive display unkeyed the primary window");
      const NSSize content = pet.contentView.bounds.size;
      checks.Check(Near(content.width, 200.0F, 0.5F) && Near(content.height, 200.0F, 0.5F),
                   "utility window did not apply its initial size");
      const std::optional<Point> origin = checks.pet->Position();
      checks.Check(origin.has_value() && Near(origin->x, 120.0F, 1.0F) && Near(origin->y, 140.0F, 1.0F),
                   "initial position did not round trip");
      checks.Check(checks.pet->Capabilities() == expected_capabilities, "AppKit capabilities were incomplete");
      NSScreen* screen = pet.screen != nil ? pet.screen : NSScreen.mainScreen;
      const NSRect visible = screen.visibleFrame;
      const CGFloat primary_height = MacPrimaryScreenHeight();
      const std::optional<Rect> work = checks.pet->WorkArea();
      checks.Check(work.has_value() && Near(work->x, static_cast<float>(visible.origin.x), 1.0F) &&
                       Near(work->y, static_cast<float>(primary_height - NSMaxY(visible)), 1.0F) &&
                       Near(work->width, static_cast<float>(visible.size.width), 1.0F) &&
                       Near(work->height, static_cast<float>(visible.size.height), 1.0F),
                   "work area did not match the visible frame in logical coordinates");
      ++phase;
      return false;
    }
    case 2: {
      NSWindow* pet = checks.watches[0]->Window();
      const WindowPixels pixels(pet);
      if (!pixels.valid) {
        checks.Check(std::chrono::steady_clock::now() + std::chrono::seconds(8) < checks.deadline,
                     "native window capture stayed unavailable");
        return false;
      }
      const auto corner = pixels.At(5.0F, 5.0F);
      const auto body = pixels.At(100.0F, 100.0F);
      const auto edge = pixels.At(30.0F, 100.0F);
      if (corner[3] > 2 || corner[0] > 4 || corner[1] > 4 || corner[2] > 4 || body[0] < 236 || body[0] > 244 ||
          body[1] < 56 || body[1] > 64 || body[2] < 26 || body[2] > 34 || body[3] < 253 || edge[3] < 122 ||
          edge[3] > 134 || edge[0] < 112 || edge[0] > 128 || edge[1] < 26 || edge[1] > 34 || edge[2] < 11 ||
          edge[2] > 19) {
        checks.Check(std::chrono::steady_clock::now() + std::chrono::seconds(8) < checks.deadline,
                     "native transparent surface did not present the fixture pixels: corner=" +
                         std::to_string(corner[0]) + "," + std::to_string(corner[1]) + "," +
                         std::to_string(corner[2]) + "," + std::to_string(corner[3]) + " body=" +
                         std::to_string(body[0]) + "," + std::to_string(body[1]) + "," + std::to_string(body[2]) +
                         "," + std::to_string(body[3]) + " edge=" + std::to_string(edge[0]) + "," +
                         std::to_string(edge[1]) + "," + std::to_string(edge[2]) + "," + std::to_string(edge[3]) +
                         " capture=" + std::to_string(pixels.width) + "x" + std::to_string(pixels.height));
        return false;
      }
      ++phase;
      return false;
    }
    case 3: {
      NSWindow* pet = checks.watches[0]->Window();
      checks.Check(checks.pet->SetTopmost(false) == WindowRequestResult::Submitted, "SetTopmost(false) was refused");
      checks.Check(pet.level == NSNormalWindowLevel, "SetTopmost(false) did not clear the floating level");
      checks.Check(checks.pet->SetTopmost(true) == WindowRequestResult::Submitted, "SetTopmost(true) was refused");
      checks.Check(pet.level == NSFloatingWindowLevel, "SetTopmost(true) did not restore the floating level");
      checks.Check(checks.pet->SetPosition({180.0F, 160.0F}) == WindowRequestResult::Submitted,
                   "SetPosition was refused");
      const std::optional<Point> moved = checks.pet->Position();
      checks.Check(moved.has_value() && Near(moved->x, 180.0F, 1.0F) && Near(moved->y, 160.0F, 1.0F),
                   "SetPosition did not round trip");
      const Point before_resize = moved.value_or(Point{});
      checks.Check(checks.pet->SetClientSize({220.0F, 180.0F}) == WindowRequestResult::Submitted,
                   "SetClientSize was refused");
      const NSSize resized = pet.contentView.bounds.size;
      const std::optional<Point> after_resize = checks.pet->Position();
      checks.Check(Near(resized.width, 220.0F, 0.5F) && Near(resized.height, 180.0F, 0.5F),
                   "SetClientSize did not apply the client dimensions");
      checks.Check(after_resize.has_value() && Near(after_resize->x, before_resize.x, 1.0F) &&
                       Near(after_resize->y, before_resize.y, 1.0F),
                   "SetClientSize did not preserve the client top-left corner");
      bool rejected_position = false;
      bool rejected_size = false;
      try {
        static_cast<void>(checks.pet->SetPosition({std::numeric_limits<float>::quiet_NaN(), 0.0F}));
      } catch (const std::invalid_argument&) {
        rejected_position = true;
      }
      try {
        static_cast<void>(checks.pet->SetClientSize({0.0F, 100.0F}));
      } catch (const std::invalid_argument&) {
        rejected_size = true;
      }
      checks.Check(rejected_position && rejected_size, "invalid geometry did not throw std::invalid_argument");
      checks.pet->Activate();
      ++phase;
      return false;
    }
    case 4: {
      NSWindow* pet = checks.watches[0]->Window();
      checks.Check(pet.isKeyWindow, "explicit Activate did not grant the utility window key status");
      checks.pet->Retire();
      ++phase;
      return false;
    }
    case 5: {
      if (!checks.watches[0]->Released()) return false;
      checks.Check(checks.pet->SetInputPassthrough(true) == WindowRequestResult::Disconnected,
                   "a retired handle still performed native requests");
      checks.Check(checks.pet->Capabilities() == WindowCapabilities{}, "a retired handle still reported capabilities");
      checks.pet = checks.application->OpenWindow([] { return FloatingPetContent(true); }, pet_options());
      checks.Watch("HuxerUI native pet");
      ++phase;
      return false;
    }
    case 6: {
      if (checks.pet_compositions < 2) return false;
      NSWindow* pet = checks.watches[2]->Window();
      const WindowPixels pixels(pet);
      if (!pixels.valid) return false;
      const auto body = pixels.At(100.0F, 100.0F);
      const auto corner = pixels.At(5.0F, 5.0F);
      if (corner[3] > 2 || body[0] < 236 || body[3] < 253) return false;
      checks.Check(pet != nil, "reopened utility window disappeared");
      checks.pet->Retire();
      ++phase;
      return false;
    }
    default: return checks.watches[2]->Released();
    }
  };
  REQUIRE_NOTHROW(RunFloatingScenario(scenario));
}

TEST_CASE("MacNativeFloatingWindowPassthroughAndNativeMoveKeepInputContracts", "[macos-native]") {
  FloatingScenario scenario;
  int phase = 0;
  scenario.advance = [&]() {
    auto& checks = scenario;
    switch (phase) {
    case 0: {
      if (!checks.application || !checks.primary) return false;
      checks.pet = checks.application->OpenWindow(
          [] { return FloatingPetContent(true); },
          {
          .title = "HuxerUI native pet",
          .initial_size = {200.0F, 200.0F},
          .initial_position = Point{120.0F, 140.0F},
          .presentation = WindowPresentation::Utility,
          .transparent_surface = true,
          .show_without_activation = true,
      }
      );
      checks.Watch("HuxerUI native pet");
      checks.Watch("HuxerUI native floating primary");
      ++phase;
      return false;
    }
    case 1: {
      if (checks.pet_compositions == 0) return false;
      NSWindow* pet = checks.watches[0]->Window();
      checks.Check(checks.pet->BeginMove() == WindowRequestResult::Rejected,
                   "BeginMove without a triggering pointer event was accepted");
      checks.Check(!pet.ignoresMouseEvents, "utility window started with input excluded");
      ClickViewPoint(pet, NSMakePoint(100.0, 100.0));
      checks.Check(checks.clicks == 1, "the first synthesized click did not reach the application");
      checks.Check(checks.pet->BeginMove() == WindowRequestResult::Rejected,
                   "BeginMove reused a released mouse-down event");
      DeliverPointer(pet, NSEventTypeLeftMouseDown, NSMakePoint(100.0, 100.0));
      checks.Check(checks.pet->SetInputPassthrough(true) == WindowRequestResult::Submitted,
                   "SetInputPassthrough(true) was refused");
      checks.Check(pet.ignoresMouseEvents, "passthrough did not exclude native pointer input");
      checks.Check(checks.cancels >= 1, "passthrough did not cancel the active pointer sequence");
      DeliverPointer(pet, NSEventTypeLeftMouseUp, NSMakePoint(100.0, 100.0));
      checks.Check(checks.clicks == 1, "a cancelled pointer sequence still produced a click");
      checks.Check(checks.pet->SetInputPassthrough(false) == WindowRequestResult::Submitted,
                   "SetInputPassthrough(false) was refused");
      checks.Check(!pet.ignoresMouseEvents, "restoring input did not re-enable native pointer input");
      ClickViewPoint(pet, NSMakePoint(100.0, 100.0));
      checks.Check(checks.clicks == 2, "the click after restoring input did not reach the application");
      ++phase;
      return false;
    }
    case 2: {
      NSWindow* pet = checks.watches[0]->Window();
      const std::optional<Point> before_drag = checks.pet->Position();
      if (before_drag.has_value()) {
        checks.drag_origin = *before_drag;
        checks.drag_origin_valid = true;
      }
      DeliverPointer(pet, NSEventTypeLeftMouseDown, NSMakePoint(100.0, 100.0));
      DeliverPointer(pet, NSEventTypeLeftMouseDragged, NSMakePoint(112.0, 100.0));
      DeliverPointer(pet, NSEventTypeLeftMouseDragged, NSMakePoint(126.0, 100.0));
      // A native move may consume mouse-up; leave this shared sequence active until queued cancellation.
      checks.Check(checks.begin_move_result == WindowRequestResult::Submitted,
                   "BeginMove from a live drag gesture was not submitted");
      ++phase;
      return false;
    }
    case 3: {
      // The deferred native drag completes without a live physical button and must not disturb the session.
      NSWindow* pet = checks.watches[0]->Window();
      checks.Check(pet != nil, "the native move request retired the utility window");
      checks.Check(!pet.ignoresMouseEvents, "the native move request excluded pointer input");
      checks.Check(checks.cancels >= 2, "native move did not cancel the shared pointer sequence");
      checks.Check(checks.drag_cancels == 1, "native move did not cancel the accepted drag gesture exactly once");
      checks.Check(checks.pet->BeginMove() == WindowRequestResult::Rejected,
                   "native move reused a completed trigger");
      ClickViewPoint(pet, NSMakePoint(100.0, 100.0));
      checks.Check(checks.clicks == 3, "input stayed captured after native move cancellation");
      const std::optional<Point> after_drag = checks.pet->Position();
      checks.Check(checks.drag_origin_valid && after_drag.has_value() &&
                       Near(after_drag->x, checks.drag_origin.x, 1.0F) && Near(after_drag->y, checks.drag_origin.y, 1.0F),
                   "the synthetic session moved the window without physical pointer input");
      checks.modal = checks.application->OpenWindow(
          [] { return FloatingPetContent(false, true); },
          {.title = "HuxerUI native pet modal", .initial_size = {240.0F, 160.0F}},
          {.owner = *checks.pet, .modal = true}
      );
      checks.Watch("HuxerUI native pet modal");
      ++phase;
      return false;
    }
    case 4: {
      if (checks.modal_compositions == 0) return false;
      NSWindow* pet = checks.watches[0]->Window();
      checks.Check(pet.ignoresMouseEvents, "a modal window did not block the utility window's native input");
      checks.Check(checks.pet->SetInputPassthrough(true) == WindowRequestResult::Submitted,
                   "SetInputPassthrough(true) under a modal was refused");
      checks.Check(pet.ignoresMouseEvents, "modal blocking and passthrough did not combine");
      checks.modal->Retire();
      ++phase;
      return false;
    }
    case 5: {
      if (!checks.watches[2]->Released()) return false;
      NSWindow* pet = checks.watches[0]->Window();
      checks.Check(pet.ignoresMouseEvents, "modal retirement cleared the requested passthrough");
      checks.Check(checks.pet->SetInputPassthrough(false) == WindowRequestResult::Submitted,
                   "SetInputPassthrough(false) after a modal was refused");
      checks.Check(!pet.ignoresMouseEvents, "restoring input did not clear native pointer exclusion");
      checks.pet->Retire();
      ++phase;
      return false;
    }
    default: return checks.watches[0]->Released();
    }
  };
  REQUIRE_NOTHROW(RunFloatingScenario(scenario));
}

} // namespace huxerui::test
