// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_ASSISTANT_UI_MAIN_STAGE_UI_ELEMENT_CONTAINER_VIEW_H_
#define ASH_ASSISTANT_UI_MAIN_STAGE_UI_ELEMENT_CONTAINER_VIEW_H_

#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include "ash/assistant/model/assistant_interaction_model_observer.h"
#include "ash/assistant/ui/base/assistant_scroll_view.h"
#include "base/macros.h"
#include "ui/views/view_observer.h"

namespace ui {
class CallbackLayerAnimationObserver;
}  // namespace ui

namespace views {
class NativeViewHost;
}  // namespace views

namespace ash {

class AssistantCardElement;
class AssistantController;
class AssistantResponse;
class AssistantTextElement;
class AssistantUiElement;
enum class AssistantUiElementType;

// UiElementContainerView is the child of AssistantMainView concerned with
// laying out text views and embedded card views in response to Assistant
// interaction model UI element events.
class UiElementContainerView : public AssistantScrollView,
                               public AssistantInteractionModelObserver {
 public:
  explicit UiElementContainerView(AssistantController* assistant_controller);
  ~UiElementContainerView() override;

  // AssistantScrollView:
  const char* GetClassName() const override;
  gfx::Size CalculatePreferredSize() const override;
  int GetHeightForWidth(int width) const override;
  void OnContentsPreferredSizeChanged(views::View* content_view) override;
  void PreferredSizeChanged() override;

  // AssistantInteractionModelObserver:
  void OnCommittedQueryChanged(const AssistantQuery& query) override;
  void OnResponseChanged(const AssistantResponse& response) override;
  void OnResponseCleared() override;

 private:
  void InitLayout();

  void OnResponseAdded(const AssistantResponse& response);
  void OnAllUiElementsAdded();
  bool OnAllUiElementsExitAnimationEnded(
      const ui::CallbackLayerAnimationObserver& observer);
  void OnUiElementAdded(const AssistantUiElement* ui_element);
  void OnCardElementAdded(const AssistantCardElement* card_element);
  void OnCardReady(const base::Optional<base::UnguessableToken>& embed_token);
  void OnTextElementAdded(const AssistantTextElement* text_element);

  // Assistant cards are rendered asynchronously before being added to the view
  // hierarchy. For this reason, it is necessary to pend any UI elements that
  // arrive between the time a render request is sent and the time at which the
  // view is finally embedded. Failure to do so could result in a mismatch
  // between the ordering of UI elements received and their corresponding views.
  void SetProcessingUiElement(bool is_processing);
  void ProcessPendingUiElements();

  // Sets whether or not PreferredSizeChanged events should be propagated.
  void SetPropagatePreferredSizeChanged(bool propagate);

  void ReleaseAllCards();

  AssistantController* const assistant_controller_;  // Owned by Shell.

  // Uniquely identifies cards owned by AssistantCardRenderer.
  std::vector<base::UnguessableToken> id_token_list_;

  // Owned by AssistantInteractionModel.
  std::deque<const AssistantUiElement*> pending_ui_element_list_;

  // Whether a UI element is currently being processed. If true, new UI elements
  // are added to |pending_ui_element_list_| and processed later.
  bool is_processing_ui_element_ = false;

  // Whether we should allow propagation of PreferredSizeChanged events. Because
  // we only animate views in/out in batches, we can prevent over-propagation of
  // PreferredSizeChanged events by waiting until the entirety of a response has
  // been added/removed before propagating. This reduces layout passes.
  bool propagate_preferred_size_changed_ = true;

  // Cached references to the native view hosts associated with card elements.
  // We maintain a reference so long as the native view host is attached so that
  // we can detach before removal from the view hierarchy and destruction.
  std::vector<views::NativeViewHost*> native_view_hosts_;

  // UI elements will be animated on their own layers. We track the desired
  // opacity to which each layer should be animated when processing the next
  // query response.
  std::vector<std::pair<ui::LayerOwner*, float>> ui_element_views_;

  std::unique_ptr<ui::CallbackLayerAnimationObserver>
      ui_elements_exit_animation_observer_;

  // Whether or not the card we are adding is the first card for the current
  // Assistant response. The first card requires the addition of a top margin.
  bool is_first_card_ = true;

  // Weak pointer factory used for card rendering requests.
  base::WeakPtrFactory<UiElementContainerView> render_request_weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(UiElementContainerView);
};

}  // namespace ash

#endif  // ASH_ASSISTANT_UI_MAIN_STAGE_UI_ELEMENT_CONTAINER_VIEW_H_
