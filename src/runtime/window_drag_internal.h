#pragma once

#include <functional>
#include <memory>

#include <huxerui/environment.h>

#include "gesture_internal.h"

namespace huxerui {
class UiWindow;
namespace detail {

struct UiExecutionState;

/// Retained native transport identity for one existing PointerSession, never a second recognizer or public payload.
/// All operations run on the original application thread. Native completion after disconnection is harmless.
class WindowDrag final : public std::enable_shared_from_this<WindowDrag> {
public:
  bool Move(UiWindow& window, Point position, bool moved = true);
  void Leave(UiWindow& window);
  bool Drop(UiWindow& window, Point position);
  void Finish(bool canceled);
  bool Active() const;
  void SetNativeCancel(std::function<void()> cancel);

private:
  WindowDrag(UiWindow& source, const DragSourceRecognitionState& recognition, const DragDropSession& session);
  void Refresh(bool moved, const FrameInfo* frame = nullptr);
  void Dispatch(UiWindow& window, const ActiveDropTarget& target, DropTargetDispatch::Function function, Point position);
  void Exit();
  void Close();

  std::weak_ptr<UiExecutionState> source_;
  std::weak_ptr<UiExecutionState> receiver_;
  DragEvent source_event_;
  std::type_index payload_type_ = typeid(void);
  std::shared_ptr<const void> payload_;
  std::function<View()> preview_;
  std::shared_ptr<const Environment> environment_;
  Point preview_grab_offset_;
  Point position_;
  std::optional<ActiveDropTarget> target_;
  std::optional<LayerId> preview_layer_;
  std::function<void()> cancel_native_;
  bool active_ = true;
  bool dropped_ = false;

  friend class PointerInteraction;
};

} // namespace detail
} // namespace huxerui
