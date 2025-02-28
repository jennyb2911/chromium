// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/events/mojo/event_struct_traits.h"

#include "mojo/public/cpp/base/time_mojom_traits.h"
#include "ui/events/event.h"
#include "ui/events/event_utils.h"
#include "ui/events/gesture_event_details.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/events/mojo/event_constants.mojom.h"
#include "ui/latency/mojo/latency_info_struct_traits.h"

namespace mojo {

namespace {

ui::mojom::LocationDataPtr GetLocationData(const ui::LocatedEvent* event) {
  ui::mojom::LocationDataPtr location_data(ui::mojom::LocationData::New());
  location_data->x = event->location_f().x();
  location_data->y = event->location_f().y();
  location_data->screen_x = event->root_location_f().x();
  location_data->screen_y = event->root_location_f().y();
  return location_data;
}

ui::EventPointerType PointerTypeFromPointerKind(ui::mojom::PointerKind kind) {
  switch (kind) {
    case ui::mojom::PointerKind::MOUSE:
      return ui::EventPointerType::POINTER_TYPE_MOUSE;
    case ui::mojom::PointerKind::TOUCH:
      return ui::EventPointerType::POINTER_TYPE_TOUCH;
    case ui::mojom::PointerKind::PEN:
      return ui::EventPointerType::POINTER_TYPE_PEN;
    case ui::mojom::PointerKind::ERASER:
      return ui::EventPointerType::POINTER_TYPE_ERASER;
  }
  NOTREACHED();
  return ui::EventPointerType::POINTER_TYPE_UNKNOWN;
}

void UpdateEventLocation(const ui::mojom::PointerData& pointer_data,
                         EventUniquePtr* out) {
  // TODO(katie): Update LocationData to have two PointF instead of storing this
  // as four floats.
  const gfx::PointF location(pointer_data.location->x,
                             pointer_data.location->y);
  const gfx::PointF screen_location(pointer_data.location->screen_x,
                                    pointer_data.location->screen_y);
  // Set the float location, as the constructor only takes a gfx::Point.
  // This uses the event root_location field to store screen pixel
  // coordinates. See http://crbug.com/608547
  out->get()->AsLocatedEvent()->set_location_f(location);
  out->get()->AsLocatedEvent()->set_root_location_f(screen_location);
}

bool ReadPointerDetails(ui::mojom::EventType event_type,
                        const ui::mojom::PointerData& pointer_data,
                        ui::PointerDetails* out) {
  switch (pointer_data.kind) {
    case ui::mojom::PointerKind::MOUSE: {
      if (event_type == ui::mojom::EventType::POINTER_WHEEL_CHANGED ||
          event_type == ui::mojom::EventType::MOUSE_WHEEL_EVENT) {
        *out = ui::PointerDetails(
            ui::EventPointerType::POINTER_TYPE_MOUSE,
            gfx::Vector2d(static_cast<int>(pointer_data.wheel_data->delta_x),
                          static_cast<int>(pointer_data.wheel_data->delta_y)),
            ui::MouseEvent::kMousePointerId);
      } else {
        *out = ui::PointerDetails(ui::EventPointerType::POINTER_TYPE_MOUSE,
                                  ui::MouseEvent::kMousePointerId);
      }
      return true;
    }
    case ui::mojom::PointerKind::TOUCH:
    case ui::mojom::PointerKind::PEN: {
      const ui::mojom::BrushData& brush_data = *pointer_data.brush_data;
      *out = ui::PointerDetails(
          PointerTypeFromPointerKind(pointer_data.kind),
          pointer_data.pointer_id, brush_data.width, brush_data.height,
          brush_data.pressure, brush_data.twist, brush_data.tilt_x,
          brush_data.tilt_y, brush_data.tangential_pressure);
      return true;
    }
    case ui::mojom::PointerKind::ERASER:
      // TODO(jamescook): Eraser support.
      NOTIMPLEMENTED();
      return false;
  }
  NOTREACHED();
  return false;
}

bool ReadScrollData(ui::mojom::EventDataView* event,
                    base::TimeTicks time_stamp,
                    EventUniquePtr* out) {
  ui::mojom::ScrollDataPtr scroll_data;
  if (!event->ReadScrollData<ui::mojom::ScrollDataPtr>(&scroll_data))
    return false;

  *out = std::make_unique<ui::ScrollEvent>(
      mojo::ConvertTo<ui::EventType>(event->action()),
      gfx::Point(scroll_data->location->x, scroll_data->location->y),
      time_stamp, event->flags(), scroll_data->x_offset, scroll_data->y_offset,
      scroll_data->x_offset_ordinal, scroll_data->y_offset_ordinal,
      scroll_data->finger_count, scroll_data->momentum_phase);
  return true;
}

bool ReadGestureData(ui::mojom::EventDataView* event,
                     base::TimeTicks time_stamp,
                     EventUniquePtr* out) {
  ui::mojom::GestureDataPtr gesture_data;
  if (!event->ReadGestureData<ui::mojom::GestureDataPtr>(&gesture_data))
    return false;

  *out = std::make_unique<ui::GestureEvent>(
      gesture_data->location->x, gesture_data->location->y, event->flags(),
      time_stamp,
      ui::GestureEventDetails(ConvertTo<ui::EventType>(event->action())));
  return true;
}

}  // namespace

static_assert(ui::mojom::kEventFlagNone == static_cast<int32_t>(ui::EF_NONE),
              "EVENT_FLAGS must match");
static_assert(ui::mojom::kEventFlagIsSynthesized ==
                  static_cast<int32_t>(ui::EF_IS_SYNTHESIZED),
              "EVENT_FLAGS must match");
static_assert(ui::mojom::kEventFlagShiftDown ==
                  static_cast<int32_t>(ui::EF_SHIFT_DOWN),
              "EVENT_FLAGS must match");
static_assert(ui::mojom::kEventFlagControlDown ==
                  static_cast<int32_t>(ui::EF_CONTROL_DOWN),
              "EVENT_FLAGS must match");
static_assert(ui::mojom::kEventFlagAltDown ==
                  static_cast<int32_t>(ui::EF_ALT_DOWN),
              "EVENT_FLAGS must match");
static_assert(ui::mojom::kEventFlagCommandDown ==
                  static_cast<int32_t>(ui::EF_COMMAND_DOWN),
              "EVENT_FLAGS must match");
static_assert(ui::mojom::kEventFlagAltgrDown ==
                  static_cast<int32_t>(ui::EF_ALTGR_DOWN),
              "EVENT_FLAGS must match");
static_assert(ui::mojom::kEventFlagMod3Down ==
                  static_cast<int32_t>(ui::EF_MOD3_DOWN),
              "EVENT_FLAGS must match");
static_assert(ui::mojom::kEventFlagNumLockOn ==
                  static_cast<int32_t>(ui::EF_NUM_LOCK_ON),
              "EVENT_FLAGS must match");
static_assert(ui::mojom::kEventFlagCapsLockOn ==
                  static_cast<int32_t>(ui::EF_CAPS_LOCK_ON),
              "EVENT_FLAGS must match");
static_assert(ui::mojom::kEventFlagScrollLockOn ==
                  static_cast<int32_t>(ui::EF_SCROLL_LOCK_ON),
              "EVENT_FLAGS must match");
static_assert(ui::mojom::kEventFlagLeftMouseButton ==
                  static_cast<int32_t>(ui::EF_LEFT_MOUSE_BUTTON),
              "EVENT_FLAGS must match");
static_assert(ui::mojom::kEventFlagMiddleMouseButton ==
                  static_cast<int32_t>(ui::EF_MIDDLE_MOUSE_BUTTON),
              "EVENT_FLAGS must match");
static_assert(ui::mojom::kEventFlagRightMouseButton ==
                  static_cast<int32_t>(ui::EF_RIGHT_MOUSE_BUTTON),
              "EVENT_FLAGS must match");
static_assert(ui::mojom::kEventFlagBackMouseButton ==
                  static_cast<int32_t>(ui::EF_BACK_MOUSE_BUTTON),
              "EVENT_FLAGS must match");
static_assert(ui::mojom::kEventFlagForwardMouseButton ==
                  static_cast<int32_t>(ui::EF_FORWARD_MOUSE_BUTTON),
              "EVENT_FLAGS must match");

// static
ui::mojom::EventType TypeConverter<ui::mojom::EventType,
                                   ui::EventType>::Convert(ui::EventType type) {
  switch (type) {
    case ui::ET_UNKNOWN:
      return ui::mojom::EventType::UNKNOWN;
    case ui::ET_KEY_PRESSED:
      return ui::mojom::EventType::KEY_PRESSED;
    case ui::ET_KEY_RELEASED:
      return ui::mojom::EventType::KEY_RELEASED;
    case ui::ET_POINTER_DOWN:
      return ui::mojom::EventType::POINTER_DOWN;
    case ui::ET_POINTER_MOVED:
      return ui::mojom::EventType::POINTER_MOVED;
    case ui::ET_POINTER_UP:
      return ui::mojom::EventType::POINTER_UP;
    case ui::ET_POINTER_CANCELLED:
      return ui::mojom::EventType::POINTER_CANCELLED;
    case ui::ET_POINTER_ENTERED:
      return ui::mojom::EventType::POINTER_ENTERED;
    case ui::ET_POINTER_EXITED:
      return ui::mojom::EventType::POINTER_EXITED;
    case ui::ET_POINTER_WHEEL_CHANGED:
      return ui::mojom::EventType::POINTER_WHEEL_CHANGED;
    case ui::ET_POINTER_CAPTURE_CHANGED:
      return ui::mojom::EventType::POINTER_CAPTURE_CHANGED;
    case ui::ET_GESTURE_TAP:
      return ui::mojom::EventType::GESTURE_TAP;
    case ui::ET_SCROLL:
      return ui::mojom::EventType::SCROLL;
    case ui::ET_SCROLL_FLING_START:
      return ui::mojom::EventType::SCROLL_FLING_START;
    case ui::ET_SCROLL_FLING_CANCEL:
      return ui::mojom::EventType::SCROLL_FLING_CANCEL;
    case ui::ET_CANCEL_MODE:
      return ui::mojom::EventType::CANCEL_MODE;
    case ui::ET_MOUSE_PRESSED:
      return ui::mojom::EventType::MOUSE_PRESSED_EVENT;
    case ui::ET_MOUSE_DRAGGED:
      return ui::mojom::EventType::MOUSE_DRAGGED_EVENT;
    case ui::ET_MOUSE_RELEASED:
      return ui::mojom::EventType::MOUSE_RELEASED_EVENT;
    case ui::ET_MOUSE_MOVED:
      return ui::mojom::EventType::MOUSE_MOVED_EVENT;
    case ui::ET_MOUSE_ENTERED:
      return ui::mojom::EventType::MOUSE_ENTERED_EVENT;
    case ui::ET_MOUSE_EXITED:
      return ui::mojom::EventType::MOUSE_EXITED_EVENT;
    case ui::ET_MOUSEWHEEL:
      return ui::mojom::EventType::MOUSE_WHEEL_EVENT;
    case ui::ET_MOUSE_CAPTURE_CHANGED:
      return ui::mojom::EventType::MOUSE_CAPTURE_CHANGED_EVENT;
    default:
      NOTREACHED() << "Using unknown event types closes connections:"
                   << ui::EventTypeName(type);
      break;
  }
  return ui::mojom::EventType::UNKNOWN;
}

// static
ui::EventType TypeConverter<ui::EventType, ui::mojom::EventType>::Convert(
    ui::mojom::EventType type) {
  switch (type) {
    case ui::mojom::EventType::UNKNOWN:
      return ui::ET_UNKNOWN;
    case ui::mojom::EventType::KEY_PRESSED:
      return ui::ET_KEY_PRESSED;
    case ui::mojom::EventType::KEY_RELEASED:
      return ui::ET_KEY_RELEASED;
    case ui::mojom::EventType::POINTER_DOWN:
      return ui::ET_POINTER_DOWN;
    case ui::mojom::EventType::POINTER_MOVED:
      return ui::ET_POINTER_MOVED;
    case ui::mojom::EventType::POINTER_UP:
      return ui::ET_POINTER_UP;
    case ui::mojom::EventType::POINTER_CANCELLED:
      return ui::ET_POINTER_CANCELLED;
    case ui::mojom::EventType::POINTER_ENTERED:
      return ui::ET_POINTER_ENTERED;
    case ui::mojom::EventType::POINTER_EXITED:
      return ui::ET_POINTER_EXITED;
    case ui::mojom::EventType::POINTER_WHEEL_CHANGED:
      return ui::ET_POINTER_WHEEL_CHANGED;
    case ui::mojom::EventType::POINTER_CAPTURE_CHANGED:
      return ui::ET_POINTER_CAPTURE_CHANGED;
    case ui::mojom::EventType::GESTURE_TAP:
      return ui::ET_GESTURE_TAP;
    case ui::mojom::EventType::SCROLL:
      return ui::ET_SCROLL;
    case ui::mojom::EventType::SCROLL_FLING_START:
      return ui::ET_SCROLL_FLING_START;
    case ui::mojom::EventType::SCROLL_FLING_CANCEL:
      return ui::ET_SCROLL_FLING_CANCEL;
    case ui::mojom::EventType::MOUSE_PRESSED_EVENT:
      return ui::ET_MOUSE_PRESSED;
    case ui::mojom::EventType::MOUSE_DRAGGED_EVENT:
      return ui::ET_MOUSE_DRAGGED;
    case ui::mojom::EventType::MOUSE_RELEASED_EVENT:
      return ui::ET_MOUSE_RELEASED;
    case ui::mojom::EventType::MOUSE_MOVED_EVENT:
      return ui::ET_MOUSE_MOVED;
    case ui::mojom::EventType::MOUSE_ENTERED_EVENT:
      return ui::ET_MOUSE_ENTERED;
    case ui::mojom::EventType::MOUSE_EXITED_EVENT:
      return ui::ET_MOUSE_EXITED;
    case ui::mojom::EventType::MOUSE_WHEEL_EVENT:
      return ui::ET_MOUSEWHEEL;
    case ui::mojom::EventType::MOUSE_CAPTURE_CHANGED_EVENT:
      return ui::ET_MOUSE_CAPTURE_CHANGED;
    default:
      NOTREACHED();
  }
  return ui::ET_UNKNOWN;
}

// static
ui::mojom::EventType
StructTraits<ui::mojom::EventDataView, EventUniquePtr>::action(
    const EventUniquePtr& event) {
  return mojo::ConvertTo<ui::mojom::EventType>(event->type());
}

// static
int32_t StructTraits<ui::mojom::EventDataView, EventUniquePtr>::flags(
    const EventUniquePtr& event) {
  return event->flags();
}

// static
base::TimeTicks
StructTraits<ui::mojom::EventDataView, EventUniquePtr>::time_stamp(
    const EventUniquePtr& event) {
  return event->time_stamp();
}

// static
const ui::LatencyInfo&
StructTraits<ui::mojom::EventDataView, EventUniquePtr>::latency(
    const EventUniquePtr& event) {
  return *event->latency();
}

// static
ui::mojom::KeyDataPtr
StructTraits<ui::mojom::EventDataView, EventUniquePtr>::key_data(
    const EventUniquePtr& event) {
  if (!event->IsKeyEvent())
    return nullptr;

  const ui::KeyEvent* key_event = event->AsKeyEvent();
  ui::mojom::KeyDataPtr key_data(ui::mojom::KeyData::New());
  key_data->key_code = key_event->GetConflatedWindowsKeyCode();
  key_data->native_key_code =
      ui::KeycodeConverter::DomCodeToNativeKeycode(key_event->code());
  key_data->is_char = key_event->is_char();
  key_data->character = key_event->GetCharacter();
  key_data->windows_key_code = static_cast<ui::mojom::KeyboardCode>(
      key_event->GetLocatedWindowsKeyboardCode());
  key_data->text = key_event->GetText();
  key_data->unmodified_text = key_event->GetUnmodifiedText();
  return key_data;
}

// static
ui::mojom::PointerDataPtr
StructTraits<ui::mojom::EventDataView, EventUniquePtr>::pointer_data(
    const EventUniquePtr& event) {
  if (!event->IsPointerEvent() && !event->IsMouseEvent())
    return nullptr;

  ui::mojom::PointerDataPtr pointer_data(ui::mojom::PointerData::New());
  const ui::PointerDetails* pointer_details;
  if (event->IsPointerEvent()) {
    const ui::PointerEvent* pointer_event = event->AsPointerEvent();
    pointer_data->changed_button_flags = pointer_event->changed_button_flags();
    pointer_data->location = GetLocationData(event->AsLocatedEvent());
    pointer_details = &pointer_event->pointer_details();
  } else {
    // Mouse event
    const ui::MouseEvent* mouse_event = event->AsMouseEvent();
    pointer_data->changed_button_flags = mouse_event->changed_button_flags();
    pointer_data->location = GetLocationData(mouse_event);
    pointer_details = &mouse_event->pointer_details();
  }

  pointer_data->pointer_id = pointer_details->id;
  ui::EventPointerType pointer_type = pointer_details->pointer_type;

  switch (pointer_type) {
    case ui::EventPointerType::POINTER_TYPE_MOUSE:
      pointer_data->kind = ui::mojom::PointerKind::MOUSE;
      break;
    case ui::EventPointerType::POINTER_TYPE_TOUCH:
      pointer_data->kind = ui::mojom::PointerKind::TOUCH;
      break;
    case ui::EventPointerType::POINTER_TYPE_PEN:
      pointer_data->kind = ui::mojom::PointerKind::PEN;
      break;
    case ui::EventPointerType::POINTER_TYPE_ERASER:
      pointer_data->kind = ui::mojom::PointerKind::ERASER;
      break;
    case ui::EventPointerType::POINTER_TYPE_UNKNOWN:
      NOTREACHED();
  }

  ui::mojom::BrushDataPtr brush_data(ui::mojom::BrushData::New());
  // TODO(rjk): this is in the wrong coordinate system
  brush_data->width = pointer_details->radius_x;
  brush_data->height = pointer_details->radius_y;
  brush_data->pressure = pointer_details->force;
  // In theory only pen events should have tilt, tangential_pressure and twist.
  // In practive a JavaScript PointerEvent could have type touch and still have
  // data in those fields.
  brush_data->tilt_x = pointer_details->tilt_x;
  brush_data->tilt_y = pointer_details->tilt_y;
  brush_data->tangential_pressure = pointer_details->tangential_pressure;
  brush_data->twist = pointer_details->twist;
  pointer_data->brush_data = std::move(brush_data);

  // TODO(rjkroege): Plumb raw pointer events on windows.
  // TODO(rjkroege): Handle force-touch on MacOS
  // TODO(rjkroege): Adjust brush data appropriately for Android.

  if (event->type() == ui::ET_POINTER_WHEEL_CHANGED ||
      event->type() == ui::ET_MOUSEWHEEL) {
    ui::mojom::WheelDataPtr wheel_data(ui::mojom::WheelData::New());

    // TODO(rjkroege): Support page scrolling on windows by directly
    // cracking into a mojo event when the native event is available.
    wheel_data->mode = ui::mojom::WheelMode::LINE;
    // TODO(rjkroege): Support precise scrolling deltas.

    if ((event->flags() & ui::EF_SHIFT_DOWN) != 0 &&
        pointer_details->offset.x() == 0) {
      wheel_data->delta_x = pointer_details->offset.y();
      wheel_data->delta_y = 0;
      wheel_data->delta_z = 0;
    } else {
      // TODO(rjkroege): support z in ui::Events.
      wheel_data->delta_x = pointer_details->offset.x();
      wheel_data->delta_y = pointer_details->offset.y();
      wheel_data->delta_z = 0;
    }
    pointer_data->wheel_data = std::move(wheel_data);
  }

  return pointer_data;
}

// static
ui::mojom::GestureDataPtr
StructTraits<ui::mojom::EventDataView, EventUniquePtr>::gesture_data(
    const EventUniquePtr& event) {
  if (!event->IsGestureEvent())
    return nullptr;

  ui::mojom::GestureDataPtr gesture_data(ui::mojom::GestureData::New());
  gesture_data->location = GetLocationData(event->AsLocatedEvent());
  return gesture_data;
}

// static
ui::mojom::ScrollDataPtr
StructTraits<ui::mojom::EventDataView, EventUniquePtr>::scroll_data(
    const EventUniquePtr& event) {
  if (!event->IsScrollEvent())
    return nullptr;

  ui::mojom::ScrollDataPtr scroll_data(ui::mojom::ScrollData::New());
  scroll_data->location = GetLocationData(event->AsLocatedEvent());
  const ui::ScrollEvent* scroll_event = event->AsScrollEvent();
  scroll_data->x_offset = scroll_event->x_offset();
  scroll_data->y_offset = scroll_event->y_offset();
  scroll_data->x_offset_ordinal = scroll_event->x_offset_ordinal();
  scroll_data->y_offset_ordinal = scroll_event->y_offset_ordinal();
  scroll_data->finger_count = scroll_event->finger_count();
  scroll_data->momentum_phase = scroll_event->momentum_phase();
  return scroll_data;
}

// static
base::flat_map<std::string, std::vector<uint8_t>>
StructTraits<ui::mojom::EventDataView, EventUniquePtr>::properties(
    const EventUniquePtr& event) {
  return event->properties() ? *(event->properties()) : ui::Event::Properties();
}

// static
bool StructTraits<ui::mojom::EventDataView, EventUniquePtr>::Read(
    ui::mojom::EventDataView event,
    EventUniquePtr* out) {
  DCHECK(!out->get());

  base::TimeTicks time_stamp;
  if (!event.ReadTimeStamp(&time_stamp))
    return false;

  switch (event.action()) {
    case ui::mojom::EventType::KEY_PRESSED:
    case ui::mojom::EventType::KEY_RELEASED: {
      ui::mojom::KeyDataPtr key_data;
      if (!event.ReadKeyData<ui::mojom::KeyDataPtr>(&key_data))
        return false;

      if (key_data->is_char) {
        *out = std::make_unique<ui::KeyEvent>(
            static_cast<base::char16>(key_data->character),
            static_cast<ui::KeyboardCode>(key_data->key_code),
            ui::DomCode::NONE, event.flags(), time_stamp);
      } else {
        *out = std::make_unique<ui::KeyEvent>(
            event.action() == ui::mojom::EventType::KEY_PRESSED
                ? ui::ET_KEY_PRESSED
                : ui::ET_KEY_RELEASED,
            static_cast<ui::KeyboardCode>(key_data->key_code), event.flags(),
            time_stamp);
      }
      break;
    }
    case ui::mojom::EventType::POINTER_DOWN:
    case ui::mojom::EventType::POINTER_UP:
    case ui::mojom::EventType::POINTER_MOVED:
    case ui::mojom::EventType::POINTER_CANCELLED:
    case ui::mojom::EventType::POINTER_ENTERED:
    case ui::mojom::EventType::POINTER_EXITED:
    case ui::mojom::EventType::POINTER_WHEEL_CHANGED:
    case ui::mojom::EventType::POINTER_CAPTURE_CHANGED: {
      ui::mojom::PointerDataPtr pointer_data;
      if (!event.ReadPointerData<ui::mojom::PointerDataPtr>(&pointer_data))
        return false;

      ui::PointerDetails pointer_details;
      if (!ReadPointerDetails(event.action(), *pointer_data, &pointer_details))
        return false;

      *out = std::make_unique<ui::PointerEvent>(
          mojo::ConvertTo<ui::EventType>(event.action()), gfx::Point(),
          gfx::Point(), event.flags(), pointer_data->changed_button_flags,
          pointer_details, time_stamp);

      UpdateEventLocation(*pointer_data, out);
      break;
    }
    case ui::mojom::EventType::GESTURE_TAP:
      if (!ReadGestureData(&event, time_stamp, out))
        return false;
      break;
    case ui::mojom::EventType::SCROLL:
      if (!ReadScrollData(&event, time_stamp, out))
        return false;
      break;
    case ui::mojom::EventType::SCROLL_FLING_START:
    case ui::mojom::EventType::SCROLL_FLING_CANCEL:
      // SCROLL_FLING_START/CANCEL is represented by a GestureEvent if
      // EF_FROM_TOUCH is set.
      if ((event.flags() & ui::EF_FROM_TOUCH) != 0) {
        if (!ReadGestureData(&event, time_stamp, out))
          return false;
      } else if (!ReadScrollData(&event, time_stamp, out)) {
        return false;
      }
      break;
    case ui::mojom::EventType::CANCEL_MODE:
      *out = std::make_unique<ui::CancelModeEvent>();
      break;
    case ui::mojom::EventType::MOUSE_PRESSED_EVENT:
    case ui::mojom::EventType::MOUSE_RELEASED_EVENT:
    case ui::mojom::EventType::MOUSE_DRAGGED_EVENT:
    case ui::mojom::EventType::MOUSE_MOVED_EVENT:
    case ui::mojom::EventType::MOUSE_ENTERED_EVENT:
    case ui::mojom::EventType::MOUSE_EXITED_EVENT:
    case ui::mojom::EventType::MOUSE_WHEEL_EVENT:
    case ui::mojom::EventType::MOUSE_CAPTURE_CHANGED_EVENT: {
      ui::mojom::PointerDataPtr pointer_data;
      if (!event.ReadPointerData<ui::mojom::PointerDataPtr>(&pointer_data))
        return false;

      ui::PointerDetails pointer_details;
      if (!ReadPointerDetails(event.action(), *pointer_data, &pointer_details))
        return false;

      if (event.action() == ui::mojom::EventType::MOUSE_WHEEL_EVENT) {
        // Use the mouse event to construct a MouseWheel event to avoid asserts
        // in ui::MouseEvent's constructor that ensure it is not used to create
        // a wheel event.
        ui::MouseEvent mouse_event(
            ui::ET_MOUSE_MOVED, gfx::Point(), gfx::Point(), time_stamp,
            event.flags(), pointer_data->changed_button_flags, pointer_details);
        *out = std::make_unique<ui::MouseWheelEvent>(
            mouse_event, pointer_details.offset.x(),
            pointer_details.offset.y());
      } else {
        *out = std::make_unique<ui::MouseEvent>(
            mojo::ConvertTo<ui::EventType>(event.action()), gfx::Point(),
            gfx::Point(), time_stamp, event.flags(),
            pointer_data->changed_button_flags, pointer_details);
      }

      UpdateEventLocation(*pointer_data, out);
      break;
    }
    case ui::mojom::EventType::UNKNOWN:
      NOTREACHED() << "Using unknown event types closes connections";
      return false;
  }

  if (!out->get())
    return false;

  if (!event.ReadLatency((*out)->latency()))
    return false;

  ui::Event::Properties properties;
  if (!event.ReadProperties(&properties))
    return false;
  if (!properties.empty())
    (*out)->SetProperties(properties);

  return true;
}

}  // namespace mojo
