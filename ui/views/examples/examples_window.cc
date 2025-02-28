// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/views/examples/examples_window.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

#include "base/macros.h"
#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/combobox_model.h"
#include "ui/base/ui_base_paths.h"
#include "ui/views/background.h"
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/controls/label.h"
#include "ui/views/examples/box_layout_example.h"
#include "ui/views/examples/bubble_example.h"
#include "ui/views/examples/button_example.h"
#include "ui/views/examples/button_sticker_sheet.h"
#include "ui/views/examples/checkbox_example.h"
#include "ui/views/examples/combobox_example.h"
#include "ui/views/examples/dialog_example.h"
#include "ui/views/examples/label_example.h"
#include "ui/views/examples/link_example.h"
#include "ui/views/examples/menu_example.h"
#include "ui/views/examples/message_box_example.h"
#include "ui/views/examples/multiline_example.h"
#include "ui/views/examples/progress_bar_example.h"
#include "ui/views/examples/radio_button_example.h"
#include "ui/views/examples/scroll_view_example.h"
#include "ui/views/examples/slider_example.h"
#include "ui/views/examples/tabbed_pane_example.h"
#include "ui/views/examples/table_example.h"
#include "ui/views/examples/text_example.h"
#include "ui/views/examples/textfield_example.h"
#include "ui/views/examples/throbber_example.h"
#include "ui/views/examples/toggle_button_example.h"
#include "ui/views/examples/tree_view_example.h"
#include "ui/views/examples/vector_example.h"
#include "ui/views/examples/widget_example.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/layout/grid_layout.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"

namespace views {
namespace examples {

using ExampleVector = std::vector<std::unique_ptr<ExampleBase>>;

namespace {

// Creates the default set of examples.
ExampleVector CreateExamples() {
  ExampleVector examples;
  examples.push_back(std::make_unique<BoxLayoutExample>());
  examples.push_back(std::make_unique<BubbleExample>());
  examples.push_back(std::make_unique<ButtonExample>());
  examples.push_back(std::make_unique<ButtonStickerSheet>());
  examples.push_back(std::make_unique<CheckboxExample>());
  examples.push_back(std::make_unique<ComboboxExample>());
  examples.push_back(std::make_unique<DialogExample>());
  examples.push_back(std::make_unique<LabelExample>());
  examples.push_back(std::make_unique<LinkExample>());
  examples.push_back(std::make_unique<MenuExample>());
  examples.push_back(std::make_unique<MessageBoxExample>());
  examples.push_back(std::make_unique<MultilineExample>());
  examples.push_back(std::make_unique<ProgressBarExample>());
  examples.push_back(std::make_unique<RadioButtonExample>());
  examples.push_back(std::make_unique<ScrollViewExample>());
  examples.push_back(std::make_unique<SliderExample>());
  examples.push_back(std::make_unique<TabbedPaneExample>());
  examples.push_back(std::make_unique<TableExample>());
  examples.push_back(std::make_unique<TextExample>());
  examples.push_back(std::make_unique<TextfieldExample>());
  examples.push_back(std::make_unique<ToggleButtonExample>());
  examples.push_back(std::make_unique<ThrobberExample>());
  examples.push_back(std::make_unique<TreeViewExample>());
  examples.push_back(std::make_unique<VectorExample>());
  examples.push_back(std::make_unique<WidgetExample>());
  return examples;
}

struct ExampleTitleCompare {
  bool operator()(const std::unique_ptr<ExampleBase>& a,
                  const std::unique_ptr<ExampleBase>& b) {
    return a->example_title() < b->example_title();
  }
};

ExampleVector GetExamplesToShow(ExampleVector extra) {
  ExampleVector examples = CreateExamples();
  std::move(extra.begin(), extra.end(), std::back_inserter(examples));
  std::sort(examples.begin(), examples.end(), ExampleTitleCompare());
  return examples;
}

}  // namespace

// Model for the examples that are being added via AddExample().
class ComboboxModelExampleList : public ui::ComboboxModel {
 public:
  ComboboxModelExampleList() {}
  ~ComboboxModelExampleList() override {}

  void SetExamples(ExampleVector examples) {
    example_list_ = std::move(examples);
  }

  // ui::ComboboxModel:
  int GetItemCount() const override { return example_list_.size(); }
  base::string16 GetItemAt(int index) override {
    return base::UTF8ToUTF16(example_list_[index]->example_title());
  }

  View* GetItemViewAt(int index) {
    return example_list_[index]->example_view();
  }

 private:
  ExampleVector example_list_;

  DISALLOW_COPY_AND_ASSIGN(ComboboxModelExampleList);
};

class ExamplesWindowContents : public WidgetDelegateView,
                               public ComboboxListener {
 public:
  ExamplesWindowContents(base::OnceClosure on_close, ExampleVector examples)
      : combobox_(new Combobox(&combobox_model_)),
        example_shown_(new View),
        status_label_(new Label),
        on_close_(std::move(on_close)) {
    instance_ = this;
    combobox_->set_listener(this);
    combobox_model_.SetExamples(std::move(examples));
    combobox_->ModelChanged();

    SetBackground(CreateStandardPanelBackground());
    GridLayout* layout =
        SetLayoutManager(std::make_unique<views::GridLayout>(this));
    ColumnSet* column_set = layout->AddColumnSet(0);
    column_set->AddPaddingColumn(0, 5);
    column_set->AddColumn(GridLayout::FILL, GridLayout::FILL, 1,
                          GridLayout::USE_PREF, 0, 0);
    column_set->AddPaddingColumn(0, 5);
    layout->AddPaddingRow(0, 5);
    layout->StartRow(0 /* no expand */, 0);
    layout->AddView(combobox_);

    if (combobox_model_.GetItemCount() > 0) {
      layout->StartRow(1, 0);
      example_shown_->SetLayoutManager(std::make_unique<FillLayout>());
      example_shown_->AddChildView(combobox_model_.GetItemViewAt(0));
      layout->AddView(example_shown_);
    }

    layout->StartRow(0 /* no expand */, 0);
    layout->AddView(status_label_);
    layout->AddPaddingRow(0, 5);
  }

  ~ExamplesWindowContents() override {
    // Delete |combobox_| first as it references |combobox_model_|.
    delete combobox_;
    combobox_ = NULL;
  }

  // Prints a message in the status area, at the bottom of the window.
  void SetStatus(const std::string& status) {
    status_label_->SetText(base::UTF8ToUTF16(status));
  }

  static ExamplesWindowContents* instance() { return instance_; }

 private:
  // WidgetDelegateView:
  bool CanResize() const override { return true; }
  bool CanMaximize() const override { return true; }
  bool CanMinimize() const override { return true; }
  base::string16 GetWindowTitle() const override {
    return base::ASCIIToUTF16("Views Examples");
  }
  void WindowClosing() override {
    instance_ = NULL;
    if (on_close_)
      std::move(on_close_).Run();
  }
  gfx::Size CalculatePreferredSize() const override {
    return gfx::Size(800, 300);
  }

  // ComboboxListener:
  void OnPerformAction(Combobox* combobox) override {
    DCHECK_EQ(combobox, combobox_);
    DCHECK(combobox->selected_index() < combobox_model_.GetItemCount());
    example_shown_->RemoveAllChildViews(false);
    example_shown_->AddChildView(combobox_model_.GetItemViewAt(
        combobox->selected_index()));
    example_shown_->RequestFocus();
    SetStatus(std::string());
    Layout();
  }

  static ExamplesWindowContents* instance_;
  ComboboxModelExampleList combobox_model_;
  Combobox* combobox_;
  View* example_shown_;
  Label* status_label_;
  base::OnceClosure on_close_;

  DISALLOW_COPY_AND_ASSIGN(ExamplesWindowContents);
};

// static
ExamplesWindowContents* ExamplesWindowContents::instance_ = NULL;

void ShowExamplesWindow(base::OnceClosure on_close,
                        gfx::NativeWindow window_context,
                        ExampleVector extra_examples) {
  if (ExamplesWindowContents::instance()) {
    ExamplesWindowContents::instance()->GetWidget()->Activate();
  } else {
    ExampleVector examples = GetExamplesToShow(std::move(extra_examples));
    Widget* widget = new Widget;
    Widget::InitParams params;
    params.delegate =
        new ExamplesWindowContents(std::move(on_close), std::move(examples));
    params.context = window_context;
    widget->Init(params);
    widget->Show();
  }
}

void LogStatus(const std::string& string) {
  ExamplesWindowContents::instance()->SetStatus(string);
}

}  // namespace examples
}  // namespace views
