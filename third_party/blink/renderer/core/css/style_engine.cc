/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 *           (C) 2001 Dirk Mueller (mueller@kde.org)
 *           (C) 2006 Alexey Proskuryakov (ap@webkit.org)
 * Copyright (C) 2004, 2005, 2006, 2007, 2008, 2009, 2011, 2012 Apple Inc. All
 * rights reserved.
 * Copyright (C) 2008, 2009 Torch Mobile Inc. All rights reserved.
 * (http://www.torchmobile.com/)
 * Copyright (C) 2008, 2009, 2011, 2012 Google Inc. All rights reserved.
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies)
 * Copyright (C) Research In Motion Limited 2010-2011. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "third_party/blink/renderer/core/css/style_engine.h"

#include "third_party/blink/renderer/core/css/css_default_style_sheets.h"
#include "third_party/blink/renderer/core/css/css_font_selector.h"
#include "third_party/blink/renderer/core/css/css_style_sheet.h"
#include "third_party/blink/renderer/core/css/document_style_environment_variables.h"
#include "third_party/blink/renderer/core/css/document_style_sheet_collector.h"
#include "third_party/blink/renderer/core/css/font_face_cache.h"
#include "third_party/blink/renderer/core/css/invalidation/invalidation_set.h"
#include "third_party/blink/renderer/core/css/resolver/scoped_style_resolver.h"
#include "third_party/blink/renderer/core/css/resolver/selector_filter_parent_scope.h"
#include "third_party/blink/renderer/core/css/resolver/style_rule_usage_tracker.h"
#include "third_party/blink/renderer/core/css/resolver/viewport_style_resolver.h"
#include "third_party/blink/renderer/core/css/shadow_tree_style_sheet_collection.h"
#include "third_party/blink/renderer/core/css/style_change_reason.h"
#include "third_party/blink/renderer/core/css/style_environment_variables.h"
#include "third_party/blink/renderer/core/css/style_sheet_contents.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/dom/element_traversal.h"
#include "third_party/blink/renderer/core/dom/layout_tree_builder_traversal.h"
#include "third_party/blink/renderer/core/dom/processing_instruction.h"
#include "third_party/blink/renderer/core/dom/shadow_root.h"
#include "third_party/blink/renderer/core/dom/text.h"
#include "third_party/blink/renderer/core/frame/settings.h"
#include "third_party/blink/renderer/core/html/html_iframe_element.h"
#include "third_party/blink/renderer/core/html/html_link_element.h"
#include "third_party/blink/renderer/core/html/html_slot_element.h"
#include "third_party/blink/renderer/core/html/imports/html_imports_controller.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/probe/core_probes.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/core/svg/svg_style_element.h"
#include "third_party/blink/renderer/platform/fonts/font_cache.h"
#include "third_party/blink/renderer/platform/fonts/font_selector.h"
#include "third_party/blink/renderer/platform/instrumentation/tracing/trace_event.h"

namespace blink {

using namespace HTMLNames;

StyleEngine::StyleEngine(Document& document)
    : document_(&document),
      is_master_(!document.IsHTMLImport()),
      document_style_sheet_collection_(
          DocumentStyleSheetCollection::Create(document)) {
  if (document.GetFrame()) {
    // We don't need to create CSSFontSelector for imported document or
    // HTMLTemplateElement's document, because those documents have no frame.
    font_selector_ = CSSFontSelector::Create(&document);
    font_selector_->RegisterForInvalidationCallbacks(this);
  }
  if (document.IsInMainFrame())
    viewport_resolver_ = ViewportStyleResolver::Create(document);
  if (IsMaster())
    global_rule_set_ = CSSGlobalRuleSet::Create();
  // Document is initially style dirty.
  style_recalc_root_.Update(nullptr, &document);
}

StyleEngine::~StyleEngine() = default;

inline Document* StyleEngine::Master() {
  if (IsMaster())
    return document_;
  HTMLImportsController* import = GetDocument().ImportsController();
  // Document::ImportsController() can return null while executing its
  // destructor.
  if (!import)
    return nullptr;
  return import->Master();
}

TreeScopeStyleSheetCollection& StyleEngine::EnsureStyleSheetCollectionFor(
    TreeScope& tree_scope) {
  if (tree_scope == document_)
    return GetDocumentStyleSheetCollection();

  StyleSheetCollectionMap::AddResult result =
      style_sheet_collection_map_.insert(&tree_scope, nullptr);
  if (result.is_new_entry) {
    result.stored_value->value =
        new ShadowTreeStyleSheetCollection(ToShadowRoot(tree_scope));
  }
  return *result.stored_value->value.Get();
}

TreeScopeStyleSheetCollection* StyleEngine::StyleSheetCollectionFor(
    TreeScope& tree_scope) {
  if (tree_scope == document_)
    return &GetDocumentStyleSheetCollection();

  StyleSheetCollectionMap::iterator it =
      style_sheet_collection_map_.find(&tree_scope);
  if (it == style_sheet_collection_map_.end())
    return nullptr;
  return it->value.Get();
}

const HeapVector<TraceWrapperMember<StyleSheet>>&
StyleEngine::StyleSheetsForStyleSheetList(TreeScope& tree_scope) {
  DCHECK(Master());
  TreeScopeStyleSheetCollection& collection =
      EnsureStyleSheetCollectionFor(tree_scope);
  if (Master()->IsActive()) {
    if (all_tree_scopes_dirty_) {
      // If all tree scopes are dirty, update all of active style. Otherwise, we
      // would have to mark all tree scopes explicitly dirty for stylesheet list
      // or repeatedly update the stylesheet list on styleSheets access. Note
      // that this can only happen once if we kDidLayoutWithPendingSheets in
      // Document::UpdateStyleAndLayoutTreeIgnoringPendingStyleSheets.
      UpdateActiveStyle();
    } else {
      collection.UpdateStyleSheetList();
    }
  }
  return collection.StyleSheetsForStyleSheetList();
}

void StyleEngine::InjectSheet(const StyleSheetKey& key,
                              StyleSheetContents* sheet,
                              WebDocument::CSSOrigin origin) {
  HeapVector<std::pair<StyleSheetKey, TraceWrapperMember<CSSStyleSheet>>>&
      injected_style_sheets = origin == WebDocument::kUserOrigin
                                  ? injected_user_style_sheets_
                                  : injected_author_style_sheets_;
  injected_style_sheets.push_back(
      std::make_pair(key, CSSStyleSheet::Create(sheet, *document_)));
  if (origin == WebDocument::kUserOrigin)
    MarkUserStyleDirty();
  else
    MarkDocumentDirty();
}

void StyleEngine::RemoveInjectedSheet(const StyleSheetKey& key,
                                      WebDocument::CSSOrigin origin) {
  HeapVector<std::pair<StyleSheetKey, TraceWrapperMember<CSSStyleSheet>>>&
      injected_style_sheets = origin == WebDocument::kUserOrigin
                                  ? injected_user_style_sheets_
                                  : injected_author_style_sheets_;
  // Remove the last sheet that matches.
  const auto& it = std::find_if(injected_style_sheets.rbegin(),
                                injected_style_sheets.rend(),
                                [&key](const auto& item) {
                                  return item.first == key;
                                });
  if (it != injected_style_sheets.rend()) {
    injected_style_sheets.erase(std::next(it).base());
    if (origin == WebDocument::kUserOrigin)
      MarkUserStyleDirty();
    else
      MarkDocumentDirty();
  }
}

CSSStyleSheet& StyleEngine::EnsureInspectorStyleSheet() {
  if (inspector_style_sheet_)
    return *inspector_style_sheet_;

  StyleSheetContents* contents =
      StyleSheetContents::Create(CSSParserContext::Create(*document_));
  inspector_style_sheet_ = CSSStyleSheet::Create(contents, *document_);
  MarkDocumentDirty();
  // TODO(futhark@chromium.org): Making the active stylesheets up-to-date here
  // is required by some inspector tests, at least. I theory this should not be
  // necessary. Need to investigate to figure out if/why.
  UpdateActiveStyle();
  return *inspector_style_sheet_;
}

void StyleEngine::AddPendingSheet(StyleEngineContext& context) {
  pending_script_blocking_stylesheets_++;

  context.AddingPendingSheet(GetDocument());
  if (context.AddedPendingSheetBeforeBody()) {
    pending_render_blocking_stylesheets_++;
  } else {
    pending_body_stylesheets_++;
    GetDocument().DidAddPendingStylesheetInBody();
  }
}

// This method is called whenever a top-level stylesheet has finished loading.
void StyleEngine::RemovePendingSheet(Node& style_sheet_candidate_node,
                                     const StyleEngineContext& context) {
  if (style_sheet_candidate_node.isConnected())
    SetNeedsActiveStyleUpdate(style_sheet_candidate_node.GetTreeScope());

  if (context.AddedPendingSheetBeforeBody()) {
    DCHECK_GT(pending_render_blocking_stylesheets_, 0);
    pending_render_blocking_stylesheets_--;
  } else {
    DCHECK_GT(pending_body_stylesheets_, 0);
    pending_body_stylesheets_--;
    if (!pending_body_stylesheets_)
      GetDocument().DidRemoveAllPendingBodyStylesheets();
  }

  // Make sure we knew this sheet was pending, and that our count isn't out of
  // sync.
  DCHECK_GT(pending_script_blocking_stylesheets_, 0);

  pending_script_blocking_stylesheets_--;
  if (pending_script_blocking_stylesheets_)
    return;

  GetDocument().DidRemoveAllPendingStylesheet();
}

void StyleEngine::SetNeedsActiveStyleUpdate(TreeScope& tree_scope) {
  if (GetDocument().IsActive() || !IsMaster())
    MarkTreeScopeDirty(tree_scope);
}

void StyleEngine::AddStyleSheetCandidateNode(Node& node) {
  if (!node.isConnected() || GetDocument().IsDetached())
    return;

  DCHECK(!IsXSLStyleSheet(node));
  TreeScope& tree_scope = node.GetTreeScope();
  EnsureStyleSheetCollectionFor(tree_scope).AddStyleSheetCandidateNode(node);

  SetNeedsActiveStyleUpdate(tree_scope);
  if (tree_scope != document_)
    active_tree_scopes_.insert(&tree_scope);
}

void StyleEngine::RemoveStyleSheetCandidateNode(
    Node& node,
    ContainerNode& insertion_point) {
  DCHECK(!IsXSLStyleSheet(node));
  DCHECK(insertion_point.isConnected());

  ShadowRoot* shadow_root = node.ContainingShadowRoot();
  if (!shadow_root)
    shadow_root = insertion_point.ContainingShadowRoot();

  TreeScope& tree_scope =
      shadow_root ? *ToTreeScope(shadow_root) : ToTreeScope(GetDocument());
  TreeScopeStyleSheetCollection* collection =
      StyleSheetCollectionFor(tree_scope);
  // After detaching document, collection could be null. In the case,
  // we should not update anything. Instead, just return.
  if (!collection)
    return;
  collection->RemoveStyleSheetCandidateNode(node);

  SetNeedsActiveStyleUpdate(tree_scope);
}

void StyleEngine::ModifiedStyleSheetCandidateNode(Node& node) {
  if (node.isConnected())
    SetNeedsActiveStyleUpdate(node.GetTreeScope());
}

void StyleEngine::AdoptedStyleSheetsWillChange(TreeScope& tree_scope,
                                               StyleSheetList* old_sheets,
                                               StyleSheetList* new_sheets) {
  if (GetDocument().IsDetached())
    return;

  unsigned old_sheets_count = old_sheets ? old_sheets->length() : 0;
  unsigned new_sheets_count = new_sheets ? new_sheets->length() : 0;

  unsigned min_count = std::min(old_sheets_count, new_sheets_count);
  unsigned index = 0;
  while (index < min_count &&
         old_sheets->item(index) == new_sheets->item(index)) {
    index++;
  }

  if (old_sheets_count == new_sheets_count && index == old_sheets_count)
    return;

  for (unsigned i = index; i < old_sheets_count; ++i) {
    ToCSSStyleSheet(old_sheets->item(i))
        ->RemovedAdoptedFromTreeScope(tree_scope);
  }
  for (unsigned i = index; i < new_sheets_count; ++i) {
    ToCSSStyleSheet(new_sheets->item(i))->AddedAdoptedToTreeScope(tree_scope);
  }

  if (new_sheets_count) {
    EnsureStyleSheetCollectionFor(tree_scope);
    if (tree_scope != document_)
      active_tree_scopes_.insert(&tree_scope);
  }
  SetNeedsActiveStyleUpdate(tree_scope);
}

void StyleEngine::AddedCustomElementDefaultStyle(CSSStyleSheet* default_style) {
  if (!RuntimeEnabledFeatures::CustomElementDefaultStyleEnabled() ||
      GetDocument().IsDetached())
    return;
  custom_element_default_style_sheets_.insert(default_style);
  global_rule_set_->MarkDirty();
}

void StyleEngine::MediaQueriesChangedInScope(TreeScope& tree_scope) {
  if (ScopedStyleResolver* resolver = tree_scope.GetScopedStyleResolver())
    resolver->SetNeedsAppendAllSheets();
  SetNeedsActiveStyleUpdate(tree_scope);
}

void StyleEngine::WatchedSelectorsChanged() {
  DCHECK(IsMaster());
  DCHECK(global_rule_set_);
  global_rule_set_->InitWatchedSelectorsRuleSet(GetDocument());
  // TODO(futhark@chromium.org): Should be able to use RuleSetInvalidation here.
  GetDocument().SetNeedsStyleRecalc(
      kSubtreeStyleChange, StyleChangeReasonForTracing::Create(
                               StyleChangeReason::kDeclarativeContent));
}

bool StyleEngine::ShouldUpdateDocumentStyleSheetCollection() const {
  return all_tree_scopes_dirty_ || document_scope_dirty_ || font_cache_dirty_;
}

bool StyleEngine::ShouldUpdateShadowTreeStyleSheetCollection() const {
  return all_tree_scopes_dirty_ || !dirty_tree_scopes_.IsEmpty();
}

void StyleEngine::MediaQueryAffectingValueChanged(
    UnorderedTreeScopeSet& tree_scopes) {
  for (TreeScope* tree_scope : tree_scopes) {
    DCHECK(tree_scope != document_);
    ShadowTreeStyleSheetCollection* collection =
        ToShadowTreeStyleSheetCollection(StyleSheetCollectionFor(*tree_scope));
    DCHECK(collection);
    if (collection->MediaQueryAffectingValueChanged())
      SetNeedsActiveStyleUpdate(*tree_scope);
  }
}

void StyleEngine::MediaQueryAffectingValueChanged() {
  if (ClearMediaQueryDependentRuleSets(active_user_style_sheets_))
    MarkUserStyleDirty();
  if (GetDocumentStyleSheetCollection().MediaQueryAffectingValueChanged())
    SetNeedsActiveStyleUpdate(GetDocument());
  MediaQueryAffectingValueChanged(active_tree_scopes_);
  if (resolver_)
    resolver_->UpdateMediaType();
}

void StyleEngine::UpdateActiveStyleSheetsInImport(
    StyleEngine& master_engine,
    DocumentStyleSheetCollector& parent_collector) {
  if (!RuntimeEnabledFeatures::HTMLImportsStyleApplicationEnabled())
    return;

  DCHECK(!IsMaster());
  HeapVector<Member<StyleSheet>> sheets_for_list;
  ImportedDocumentStyleSheetCollector subcollector(parent_collector,
                                                   sheets_for_list);
  GetDocumentStyleSheetCollection().CollectStyleSheets(master_engine,
                                                       subcollector);
  GetDocumentStyleSheetCollection().SwapSheetsForSheetList(sheets_for_list);

  // all_tree_scopes_dirty_ should only be set on main documents, never html
  // imports.
  DCHECK(!all_tree_scopes_dirty_);
  // Mark false for consistency. It is never checked for import documents.
  document_scope_dirty_ = false;
}

void StyleEngine::UpdateActiveStyleSheetsInShadow(
    TreeScope* tree_scope,
    UnorderedTreeScopeSet& tree_scopes_removed) {
  DCHECK_NE(tree_scope, document_);
  ShadowTreeStyleSheetCollection* collection =
      ToShadowTreeStyleSheetCollection(StyleSheetCollectionFor(*tree_scope));
  DCHECK(collection);
  collection->UpdateActiveStyleSheets(*this);
  if (!collection->HasStyleSheetCandidateNodes() &&
      !tree_scope->HasAdoptedStyleSheets()) {
    tree_scopes_removed.insert(tree_scope);
    // When removing TreeScope from ActiveTreeScopes,
    // its resolver should be destroyed by invoking resetAuthorStyle.
    DCHECK(!tree_scope->GetScopedStyleResolver());
  }
}

void StyleEngine::UpdateActiveUserStyleSheets() {
  DCHECK(user_style_dirty_);

  ActiveStyleSheetVector new_active_sheets;
  for (auto& sheet : injected_user_style_sheets_) {
    if (RuleSet* rule_set = RuleSetForSheet(*sheet.second))
      new_active_sheets.push_back(std::make_pair(sheet.second, rule_set));
  }

  ApplyRuleSetChanges(*document_, active_user_style_sheets_, new_active_sheets,
                      kInvalidateAllScopes);

  new_active_sheets.swap(active_user_style_sheets_);
}

void StyleEngine::UpdateActiveStyleSheets() {
  if (!NeedsActiveStyleSheetUpdate())
    return;

  DCHECK(IsMaster());
  DCHECK(!GetDocument().InStyleRecalc());
  DCHECK(GetDocument().IsActive());

  TRACE_EVENT0("blink,blink_style", "StyleEngine::updateActiveStyleSheets");

  if (user_style_dirty_)
    UpdateActiveUserStyleSheets();

  if (ShouldUpdateDocumentStyleSheetCollection())
    GetDocumentStyleSheetCollection().UpdateActiveStyleSheets(*this);

  if (ShouldUpdateShadowTreeStyleSheetCollection()) {
    UnorderedTreeScopeSet tree_scopes_removed;

    if (all_tree_scopes_dirty_) {
      for (TreeScope* tree_scope : active_tree_scopes_)
        UpdateActiveStyleSheetsInShadow(tree_scope, tree_scopes_removed);
    } else {
      for (TreeScope* tree_scope : dirty_tree_scopes_)
        UpdateActiveStyleSheetsInShadow(tree_scope, tree_scopes_removed);
    }
    for (TreeScope* tree_scope : tree_scopes_removed)
      active_tree_scopes_.erase(tree_scope);
  }

  probe::activeStyleSheetsUpdated(document_);

  dirty_tree_scopes_.clear();
  document_scope_dirty_ = false;
  all_tree_scopes_dirty_ = false;
  tree_scopes_removed_ = false;
  user_style_dirty_ = false;
}

void StyleEngine::UpdateViewport() {
  if (viewport_resolver_)
    viewport_resolver_->UpdateViewport(GetDocumentStyleSheetCollection());
}

bool StyleEngine::NeedsActiveStyleUpdate() const {
  return (viewport_resolver_ && viewport_resolver_->NeedsUpdate()) ||
         NeedsActiveStyleSheetUpdate() ||
         (global_rule_set_ && global_rule_set_->IsDirty());
}

void StyleEngine::UpdateActiveStyle() {
  DCHECK(GetDocument().IsActive());
  UpdateViewport();
  UpdateActiveStyleSheets();
  UpdateGlobalRuleSet();
}

const ActiveStyleSheetVector StyleEngine::ActiveStyleSheetsForInspector() {
  if (GetDocument().IsActive())
    UpdateActiveStyle();

  if (active_tree_scopes_.IsEmpty())
    return GetDocumentStyleSheetCollection().ActiveAuthorStyleSheets();

  ActiveStyleSheetVector active_style_sheets;

  active_style_sheets.AppendVector(
      GetDocumentStyleSheetCollection().ActiveAuthorStyleSheets());
  for (TreeScope* tree_scope : active_tree_scopes_) {
    if (TreeScopeStyleSheetCollection* collection =
            style_sheet_collection_map_.at(tree_scope))
      active_style_sheets.AppendVector(collection->ActiveAuthorStyleSheets());
  }

  // FIXME: Inspector needs a vector which has all active stylesheets.
  // However, creating such a large vector might cause performance regression.
  // Need to implement some smarter solution.
  return active_style_sheets;
}

void StyleEngine::ShadowRootRemovedFromDocument(ShadowRoot* shadow_root) {
  style_sheet_collection_map_.erase(shadow_root);
  active_tree_scopes_.erase(shadow_root);
  dirty_tree_scopes_.erase(shadow_root);
  tree_scopes_removed_ = true;
  ResetAuthorStyle(*shadow_root);
}

void StyleEngine::AddTreeBoundaryCrossingScope(const TreeScope& tree_scope) {
  tree_boundary_crossing_scopes_.Add(&tree_scope.RootNode());
}

void StyleEngine::ResetAuthorStyle(TreeScope& tree_scope) {
  tree_boundary_crossing_scopes_.Remove(&tree_scope.RootNode());

  ScopedStyleResolver* scoped_resolver = tree_scope.GetScopedStyleResolver();
  if (!scoped_resolver)
    return;

  global_rule_set_->MarkDirty();
  if (tree_scope.RootNode().IsDocumentNode()) {
    scoped_resolver->ResetAuthorStyle();
    return;
  }

  tree_scope.ClearScopedStyleResolver();
}

void StyleEngine::SetRuleUsageTracker(StyleRuleUsageTracker* tracker) {
  tracker_ = tracker;

  if (resolver_)
    resolver_->SetRuleUsageTracker(tracker_);
}

RuleSet* StyleEngine::RuleSetForSheet(CSSStyleSheet& sheet) {
  if (!sheet.MatchesMediaQueries(EnsureMediaQueryEvaluator()))
    return nullptr;

  AddRuleFlags add_rule_flags = kRuleHasNoSpecialState;
  if (document_->GetSecurityOrigin()->CanRequest(sheet.BaseURL()))
    add_rule_flags = kRuleHasDocumentSecurityOrigin;
  return &sheet.Contents()->EnsureRuleSet(*media_query_evaluator_,
                                          add_rule_flags);
}

void StyleEngine::CreateResolver() {
  resolver_ = StyleResolver::Create(*document_);
  resolver_->SetRuleUsageTracker(tracker_);
}

void StyleEngine::ClearResolvers() {
  DCHECK(!GetDocument().InStyleRecalc());
  DCHECK(IsMaster() || !resolver_);

  GetDocument().ClearScopedStyleResolver();
  for (TreeScope* tree_scope : active_tree_scopes_)
    tree_scope->ClearScopedStyleResolver();

  if (resolver_) {
    TRACE_EVENT1("blink", "StyleEngine::clearResolver", "frame",
                 ToTraceValue(GetDocument().GetFrame()));
    resolver_->Dispose();
    resolver_.Clear();
  }
}

void StyleEngine::DidDetach() {
  ClearResolvers();
  if (global_rule_set_)
    global_rule_set_->Dispose();
  global_rule_set_ = nullptr;
  tree_boundary_crossing_scopes_.Clear();
  dirty_tree_scopes_.clear();
  active_tree_scopes_.clear();
  viewport_resolver_ = nullptr;
  media_query_evaluator_ = nullptr;
  style_invalidation_root_.Clear();
  style_recalc_root_.Clear();
  layout_tree_rebuild_root_.Clear();
  if (font_selector_)
    font_selector_->GetFontFaceCache()->ClearAll();
  font_selector_ = nullptr;
  if (environment_variables_)
    environment_variables_->DetachFromParent();
  environment_variables_ = nullptr;
}

void StyleEngine::ClearFontCache() {
  if (font_selector_)
    font_selector_->GetFontFaceCache()->ClearCSSConnected();
  if (resolver_)
    resolver_->InvalidateMatchedPropertiesCache();
}

void StyleEngine::RefreshFontCache() {
  DCHECK(IsFontCacheDirty());

  ClearFontCache();

  // Rebuild the font cache with @font-face rules from user style sheets.
  for (unsigned i = 0; i < active_user_style_sheets_.size(); ++i) {
    DCHECK(active_user_style_sheets_[i].second);
    AddFontFaceRules(*active_user_style_sheets_[i].second);
  }

  font_cache_dirty_ = false;
}

void StyleEngine::UpdateGenericFontFamilySettings() {
  // FIXME: we should not update generic font family settings when
  // document is inactive.
  DCHECK(GetDocument().IsActive());

  if (!font_selector_)
    return;

  font_selector_->UpdateGenericFontFamilySettings(*document_);
  if (resolver_)
    resolver_->InvalidateMatchedPropertiesCache();
  FontCache::GetFontCache()->InvalidateShapeCache();
}

void StyleEngine::RemoveFontFaceRules(
    const HeapVector<Member<const StyleRuleFontFace>>& font_face_rules) {
  if (!font_selector_)
    return;

  FontFaceCache* cache = font_selector_->GetFontFaceCache();
  for (const auto& rule : font_face_rules)
    cache->Remove(rule);
  if (resolver_)
    resolver_->InvalidateMatchedPropertiesCache();
}

void StyleEngine::MarkTreeScopeDirty(TreeScope& scope) {
  if (scope == document_) {
    MarkDocumentDirty();
    return;
  }

  TreeScopeStyleSheetCollection* collection = StyleSheetCollectionFor(scope);
  DCHECK(collection);
  collection->MarkSheetListDirty();
  dirty_tree_scopes_.insert(&scope);
  GetDocument().ScheduleLayoutTreeUpdateIfNeeded();
}

void StyleEngine::MarkDocumentDirty() {
  document_scope_dirty_ = true;
  document_style_sheet_collection_->MarkSheetListDirty();
  if (RuntimeEnabledFeatures::CSSViewportEnabled())
    ViewportRulesChanged();
  if (GetDocument().ImportLoader())
    GetDocument().MasterDocument().GetStyleEngine().MarkDocumentDirty();
  else
    GetDocument().ScheduleLayoutTreeUpdateIfNeeded();
}

void StyleEngine::MarkUserStyleDirty() {
  user_style_dirty_ = true;
  GetDocument().ScheduleLayoutTreeUpdateIfNeeded();
}

CSSStyleSheet* StyleEngine::CreateSheet(Element& element,
                                        const String& text,
                                        TextPosition start_position,
                                        StyleEngineContext& context) {
  DCHECK(element.GetDocument() == GetDocument());
  CSSStyleSheet* style_sheet = nullptr;

  AddPendingSheet(context);

  AtomicString text_content(text);

  auto result = text_to_sheet_cache_.insert(text_content, nullptr);
  StyleSheetContents* contents = result.stored_value->value;
  if (result.is_new_entry || !contents ||
      !contents->IsCacheableForStyleElement()) {
    result.stored_value->value = nullptr;
    style_sheet = ParseSheet(element, text, start_position);
    if (style_sheet->Contents()->IsCacheableForStyleElement()) {
      result.stored_value->value = style_sheet->Contents();
      sheet_to_text_cache_.insert(style_sheet->Contents(), text_content);
    }
  } else {
    DCHECK(contents);
    DCHECK(contents->IsCacheableForStyleElement());
    DCHECK(contents->HasSingleOwnerDocument());
    contents->SetIsUsedFromTextCache();
    style_sheet =
        CSSStyleSheet::CreateInline(contents, element, start_position);
  }

  DCHECK(style_sheet);
  if (!element.IsInShadowTree()) {
    String title = element.title();
    if (!title.IsEmpty()) {
      style_sheet->SetTitle(title);
      SetPreferredStylesheetSetNameIfNotSet(title);
    }
  }
  return style_sheet;
}

CSSStyleSheet* StyleEngine::ParseSheet(Element& element,
                                       const String& text,
                                       TextPosition start_position) {
  CSSStyleSheet* style_sheet = nullptr;
  style_sheet = CSSStyleSheet::CreateInline(element, NullURL(), start_position,
                                            GetDocument().Encoding());
  style_sheet->Contents()->ParseStringAtPosition(text, start_position);
  return style_sheet;
}

void StyleEngine::CollectUserStyleFeaturesTo(RuleFeatureSet& features) const {
  for (unsigned i = 0; i < active_user_style_sheets_.size(); ++i) {
    CSSStyleSheet* sheet = active_user_style_sheets_[i].first;
    features.ViewportDependentMediaQueryResults().AppendVector(
        sheet->ViewportDependentMediaQueryResults());
    features.DeviceDependentMediaQueryResults().AppendVector(
        sheet->DeviceDependentMediaQueryResults());
    DCHECK(sheet->Contents()->HasRuleSet());
    features.Add(sheet->Contents()->GetRuleSet().Features());
  }
}

void StyleEngine::CollectScopedStyleFeaturesTo(RuleFeatureSet& features) const {
  HeapHashSet<Member<const StyleSheetContents>>
      visited_shared_style_sheet_contents;
  if (GetDocument().GetScopedStyleResolver()) {
    GetDocument().GetScopedStyleResolver()->CollectFeaturesTo(
        features, visited_shared_style_sheet_contents);
  }
  for (TreeScope* tree_scope : active_tree_scopes_) {
    if (ScopedStyleResolver* resolver = tree_scope->GetScopedStyleResolver()) {
      resolver->CollectFeaturesTo(features,
                                  visited_shared_style_sheet_contents);
    }
  }
}

void StyleEngine::FontsNeedUpdate(FontSelector*) {
  if (!GetDocument().IsActive())
    return;

  if (resolver_)
    resolver_->InvalidateMatchedPropertiesCache();
  GetDocument().SetNeedsStyleRecalc(
      kSubtreeStyleChange,
      StyleChangeReasonForTracing::Create(StyleChangeReason::kFonts));
  probe::fontsUpdated(document_, nullptr, String(), nullptr);
}

void StyleEngine::SetFontSelector(CSSFontSelector* font_selector) {
  if (font_selector_)
    font_selector_->UnregisterForInvalidationCallbacks(this);
  font_selector_ = font_selector;
  if (font_selector_)
    font_selector_->RegisterForInvalidationCallbacks(this);
}

void StyleEngine::PlatformColorsChanged() {
  if (resolver_)
    resolver_->InvalidateMatchedPropertiesCache();
  GetDocument().SetNeedsStyleRecalc(
      kSubtreeStyleChange, StyleChangeReasonForTracing::Create(
                               StyleChangeReason::kPlatformColorChange));
}

bool StyleEngine::ShouldSkipInvalidationFor(const Element& element) const {
  if (GetDocument().GetStyleChangeType() >= kSubtreeStyleChange)
    return true;
  if (!element.InActiveDocument())
    return true;
  if (!element.parentNode())
    return true;
  return element.parentNode()->GetStyleChangeType() >= kSubtreeStyleChange;
}

void StyleEngine::ClassChangedForElement(
    const SpaceSplitString& changed_classes,
    Element& element) {
  if (ShouldSkipInvalidationFor(element))
    return;
  InvalidationLists invalidation_lists;
  unsigned changed_size = changed_classes.size();
  const RuleFeatureSet& features = GetRuleFeatureSet();
  for (unsigned i = 0; i < changed_size; ++i) {
    features.CollectInvalidationSetsForClass(invalidation_lists, element,
                                             changed_classes[i]);
  }
  pending_invalidations_.ScheduleInvalidationSetsForNode(invalidation_lists,
                                                         element);
}

void StyleEngine::ClassChangedForElement(const SpaceSplitString& old_classes,
                                         const SpaceSplitString& new_classes,
                                         Element& element) {
  if (ShouldSkipInvalidationFor(element))
    return;

  if (!old_classes.size()) {
    ClassChangedForElement(new_classes, element);
    return;
  }

  // Class vectors tend to be very short. This is faster than using a hash
  // table.
  BitVector remaining_class_bits;
  remaining_class_bits.EnsureSize(old_classes.size());

  InvalidationLists invalidation_lists;
  const RuleFeatureSet& features = GetRuleFeatureSet();

  for (unsigned i = 0; i < new_classes.size(); ++i) {
    bool found = false;
    for (unsigned j = 0; j < old_classes.size(); ++j) {
      if (new_classes[i] == old_classes[j]) {
        // Mark each class that is still in the newClasses so we can skip doing
        // an n^2 search below when looking for removals. We can't break from
        // this loop early since a class can appear more than once.
        remaining_class_bits.QuickSet(j);
        found = true;
      }
    }
    // Class was added.
    if (!found) {
      features.CollectInvalidationSetsForClass(invalidation_lists, element,
                                               new_classes[i]);
    }
  }

  for (unsigned i = 0; i < old_classes.size(); ++i) {
    if (remaining_class_bits.QuickGet(i))
      continue;
    // Class was removed.
    features.CollectInvalidationSetsForClass(invalidation_lists, element,
                                             old_classes[i]);
  }

  pending_invalidations_.ScheduleInvalidationSetsForNode(invalidation_lists,
                                                         element);
}

void StyleEngine::AttributeChangedForElement(
    const QualifiedName& attribute_name,
    Element& element) {
  if (ShouldSkipInvalidationFor(element))
    return;

  InvalidationLists invalidation_lists;
  GetRuleFeatureSet().CollectInvalidationSetsForAttribute(
      invalidation_lists, element, attribute_name);
  pending_invalidations_.ScheduleInvalidationSetsForNode(invalidation_lists,
                                                         element);
}

void StyleEngine::IdChangedForElement(const AtomicString& old_id,
                                      const AtomicString& new_id,
                                      Element& element) {
  if (ShouldSkipInvalidationFor(element))
    return;

  InvalidationLists invalidation_lists;
  const RuleFeatureSet& features = GetRuleFeatureSet();
  if (!old_id.IsEmpty())
    features.CollectInvalidationSetsForId(invalidation_lists, element, old_id);
  if (!new_id.IsEmpty())
    features.CollectInvalidationSetsForId(invalidation_lists, element, new_id);
  pending_invalidations_.ScheduleInvalidationSetsForNode(invalidation_lists,
                                                         element);
}

void StyleEngine::PseudoStateChangedForElement(
    CSSSelector::PseudoType pseudo_type,
    Element& element) {
  if (ShouldSkipInvalidationFor(element))
    return;

  InvalidationLists invalidation_lists;
  GetRuleFeatureSet().CollectInvalidationSetsForPseudoClass(
      invalidation_lists, element, pseudo_type);
  pending_invalidations_.ScheduleInvalidationSetsForNode(invalidation_lists,
                                                         element);
}

void StyleEngine::PartChangedForElement(Element& element) {
  if (ShouldSkipInvalidationFor(element))
    return;
  if (element.GetTreeScope() == document_)
    return;
  if (!GetRuleFeatureSet().InvalidatesParts())
    return;
  element.SetNeedsStyleRecalc(
      kLocalStyleChange,
      StyleChangeReasonForTracing::FromAttribute(HTMLNames::partAttr));
}

void StyleEngine::PartmapChangedForElement(Element& element) {
  if (ShouldSkipInvalidationFor(element))
    return;
  if (!element.GetShadowRoot())
    return;

  InvalidationLists invalidation_lists;
  GetRuleFeatureSet().CollectPartInvalidationSet(invalidation_lists);
  pending_invalidations_.ScheduleInvalidationSetsForNode(invalidation_lists,
                                                         element);
}

void StyleEngine::ScheduleSiblingInvalidationsForElement(
    Element& element,
    ContainerNode& scheduling_parent,
    unsigned min_direct_adjacent) {
  DCHECK(min_direct_adjacent);

  InvalidationLists invalidation_lists;

  const RuleFeatureSet& features = GetRuleFeatureSet();

  if (element.HasID()) {
    features.CollectSiblingInvalidationSetForId(invalidation_lists, element,
                                                element.IdForStyleResolution(),
                                                min_direct_adjacent);
  }

  if (element.HasClass()) {
    const SpaceSplitString& class_names = element.ClassNames();
    for (wtf_size_t i = 0; i < class_names.size(); i++) {
      features.CollectSiblingInvalidationSetForClass(
          invalidation_lists, element, class_names[i], min_direct_adjacent);
    }
  }

  for (const Attribute& attribute : element.Attributes()) {
    features.CollectSiblingInvalidationSetForAttribute(
        invalidation_lists, element, attribute.GetName(), min_direct_adjacent);
  }

  features.CollectUniversalSiblingInvalidationSet(invalidation_lists,
                                                  min_direct_adjacent);

  pending_invalidations_.ScheduleSiblingInvalidationsAsDescendants(
      invalidation_lists, scheduling_parent);
}

void StyleEngine::ScheduleInvalidationsForInsertedSibling(
    Element* before_element,
    Element& inserted_element) {
  unsigned affected_siblings =
      inserted_element.parentNode()->ChildrenAffectedByIndirectAdjacentRules()
          ? UINT_MAX
          : MaxDirectAdjacentSelectors();

  ContainerNode* scheduling_parent =
      inserted_element.ParentElementOrShadowRoot();
  if (!scheduling_parent)
    return;

  ScheduleSiblingInvalidationsForElement(inserted_element, *scheduling_parent,
                                         1);

  for (unsigned i = 1; before_element && i <= affected_siblings;
       i++, before_element =
                ElementTraversal::PreviousSibling(*before_element)) {
    ScheduleSiblingInvalidationsForElement(*before_element, *scheduling_parent,
                                           i);
  }
}

void StyleEngine::ScheduleInvalidationsForRemovedSibling(
    Element* before_element,
    Element& removed_element,
    Element& after_element) {
  unsigned affected_siblings =
      after_element.parentNode()->ChildrenAffectedByIndirectAdjacentRules()
          ? UINT_MAX
          : MaxDirectAdjacentSelectors();

  ContainerNode* scheduling_parent = after_element.ParentElementOrShadowRoot();
  if (!scheduling_parent)
    return;

  ScheduleSiblingInvalidationsForElement(removed_element, *scheduling_parent,
                                         1);

  for (unsigned i = 1; before_element && i <= affected_siblings;
       i++, before_element =
                ElementTraversal::PreviousSibling(*before_element)) {
    ScheduleSiblingInvalidationsForElement(*before_element, *scheduling_parent,
                                           i);
  }
}

void StyleEngine::ScheduleNthPseudoInvalidations(ContainerNode& nth_parent) {
  InvalidationLists invalidation_lists;
  GetRuleFeatureSet().CollectNthInvalidationSet(invalidation_lists);
  pending_invalidations_.ScheduleInvalidationSetsForNode(invalidation_lists,
                                                         nth_parent);
}

void StyleEngine::ScheduleRuleSetInvalidationsForElement(
    Element& element,
    const HeapHashSet<Member<RuleSet>>& rule_sets) {
  AtomicString id;
  const SpaceSplitString* class_names = nullptr;

  if (element.HasID())
    id = element.IdForStyleResolution();
  if (element.HasClass())
    class_names = &element.ClassNames();

  InvalidationLists invalidation_lists;
  for (const auto& rule_set : rule_sets) {
    if (!id.IsNull()) {
      rule_set->Features().CollectInvalidationSetsForId(invalidation_lists,
                                                        element, id);
    }
    if (class_names) {
      wtf_size_t class_name_count = class_names->size();
      for (wtf_size_t i = 0; i < class_name_count; i++) {
        rule_set->Features().CollectInvalidationSetsForClass(
            invalidation_lists, element, (*class_names)[i]);
      }
    }
    for (const Attribute& attribute : element.Attributes()) {
      rule_set->Features().CollectInvalidationSetsForAttribute(
          invalidation_lists, element, attribute.GetName());
    }
  }
  pending_invalidations_.ScheduleInvalidationSetsForNode(invalidation_lists,
                                                         element);
}

void StyleEngine::ScheduleTypeRuleSetInvalidations(
    ContainerNode& node,
    const HeapHashSet<Member<RuleSet>>& rule_sets) {
  InvalidationLists invalidation_lists;
  for (const auto& rule_set : rule_sets) {
    rule_set->Features().CollectTypeRuleInvalidationSet(invalidation_lists,
                                                        node);
  }
  DCHECK(invalidation_lists.siblings.IsEmpty());
  pending_invalidations_.ScheduleInvalidationSetsForNode(invalidation_lists,
                                                         node);

  if (!node.IsShadowRoot())
    return;

  Element& host = ToShadowRoot(node).host();
  if (host.NeedsStyleRecalc())
    return;

  for (auto& invalidation_set : invalidation_lists.descendants) {
    if (invalidation_set->InvalidatesTagName(host)) {
      host.SetNeedsStyleRecalc(kLocalStyleChange,
                               StyleChangeReasonForTracing::Create(
                                   StyleChangeReason::kStyleSheetChange));
      return;
    }
  }
}

void StyleEngine::ScheduleCustomElementInvalidations(
    HashSet<AtomicString> tag_names) {
  scoped_refptr<DescendantInvalidationSet> invalidation_set =
      DescendantInvalidationSet::Create();
  for (auto& tag_name : tag_names) {
    invalidation_set->AddTagName(tag_name);
  }
  invalidation_set->SetTreeBoundaryCrossing();
  InvalidationLists invalidation_lists;
  invalidation_lists.descendants.push_back(invalidation_set);
  pending_invalidations_.ScheduleInvalidationSetsForNode(invalidation_lists,
                                                         *document_);
}

void StyleEngine::InvalidateStyle() {
  StyleInvalidator style_invalidator(
      pending_invalidations_.GetPendingInvalidationMap());
  style_invalidator.Invalidate(GetDocument(),
                               style_invalidation_root_.RootElement());
  style_invalidation_root_.Clear();
}

void StyleEngine::InvalidateSlottedElements(HTMLSlotElement& slot) {
  for (auto& node : slot.FlattenedAssignedNodes()) {
    if (node->IsElementNode()) {
      node->SetNeedsStyleRecalc(kLocalStyleChange,
                                StyleChangeReasonForTracing::Create(
                                    StyleChangeReason::kStyleSheetChange));
    }
  }
}

void StyleEngine::ScheduleInvalidationsForRuleSets(
    TreeScope& tree_scope,
    const HeapHashSet<Member<RuleSet>>& rule_sets,
    InvalidationScope invalidation_scope) {
#if DCHECK_IS_ON()
  // Full scope recalcs should be handled while collecting the ruleSets before
  // calling this method.
  for (auto rule_set : rule_sets)
    DCHECK(!rule_set->Features().NeedsFullRecalcForRuleSetInvalidation());
#endif  // DCHECK_IS_ON()

  TRACE_EVENT0("blink,blink_style",
               "StyleEngine::scheduleInvalidationsForRuleSets");

  ScheduleTypeRuleSetInvalidations(tree_scope.RootNode(), rule_sets);

  bool invalidate_slotted = false;
  if (tree_scope.RootNode().IsShadowRoot()) {
    Element& host = ToShadowRoot(tree_scope.RootNode()).host();
    ScheduleRuleSetInvalidationsForElement(host, rule_sets);
    if (host.GetStyleChangeType() >= kSubtreeStyleChange)
      return;
    for (auto rule_set : rule_sets) {
      if (rule_set->HasSlottedRules()) {
        invalidate_slotted = true;
        break;
      }
    }
  }

  Node* stay_within = &tree_scope.RootNode();
  Element* element = ElementTraversal::FirstChild(*stay_within);
  while (element) {
    ScheduleRuleSetInvalidationsForElement(*element, rule_sets);
    if (invalidate_slotted && IsHTMLSlotElement(element))
      InvalidateSlottedElements(ToHTMLSlotElement(*element));

    if (invalidation_scope == kInvalidateAllScopes) {
      if (ShadowRoot* shadow_root = element->GetShadowRoot()) {
        ScheduleInvalidationsForRuleSets(*shadow_root, rule_sets,
                                         kInvalidateAllScopes);
      }
    }

    if (element->GetStyleChangeType() < kSubtreeStyleChange)
      element = ElementTraversal::Next(*element, stay_within);
    else
      element = ElementTraversal::NextSkippingChildren(*element, stay_within);
  }
}

void StyleEngine::SetStatsEnabled(bool enabled) {
  if (!enabled) {
    style_resolver_stats_ = nullptr;
    return;
  }
  if (!style_resolver_stats_)
    style_resolver_stats_ = StyleResolverStats::Create();
  else
    style_resolver_stats_->Reset();
}

void StyleEngine::SetPreferredStylesheetSetNameIfNotSet(const String& name) {
  DCHECK(!name.IsEmpty());
  if (!preferred_stylesheet_set_name_.IsEmpty())
    return;
  preferred_stylesheet_set_name_ = name;
  MarkDocumentDirty();
}

void StyleEngine::SetHttpDefaultStyle(const String& content) {
  if (!content.IsEmpty())
    SetPreferredStylesheetSetNameIfNotSet(content);
}

void StyleEngine::EnsureUAStyleForFullscreen() {
  DCHECK(IsMaster());
  DCHECK(global_rule_set_);
  if (global_rule_set_->HasFullscreenUAStyle())
    return;
  CSSDefaultStyleSheets::Instance().EnsureDefaultStyleSheetForFullscreen();
  global_rule_set_->MarkDirty();
  UpdateActiveStyle();
}

void StyleEngine::EnsureUAStyleForElement(const Element& element) {
  DCHECK(IsMaster());
  DCHECK(global_rule_set_);
  if (CSSDefaultStyleSheets::Instance().EnsureDefaultStyleSheetsForElement(
          element)) {
    global_rule_set_->MarkDirty();
    UpdateActiveStyle();
  }
}

bool StyleEngine::HasRulesForId(const AtomicString& id) const {
  DCHECK(IsMaster());
  DCHECK(global_rule_set_);
  return global_rule_set_->GetRuleFeatureSet().HasSelectorForId(id);
}

void StyleEngine::InitialStyleChanged() {
  if (viewport_resolver_)
    viewport_resolver_->InitialStyleChanged();

  // Media queries may rely on the initial font size relative lengths which may
  // have changed.
  MediaQueryAffectingValueChanged();

  GetDocument().SetNeedsStyleRecalc(
      kSubtreeStyleChange,
      StyleChangeReasonForTracing::Create(StyleChangeReason::kSettings));
}

void StyleEngine::InitialViewportChanged() {
  if (viewport_resolver_)
    viewport_resolver_->InitialViewportChanged();
}

void StyleEngine::ViewportRulesChanged() {
  if (viewport_resolver_)
    viewport_resolver_->SetNeedsCollectRules();
}

void StyleEngine::HtmlImportAddedOrRemoved() {
  if (GetDocument().ImportLoader()) {
    GetDocument().MasterDocument().GetStyleEngine().HtmlImportAddedOrRemoved();
    return;
  }

  // When we remove an import link and re-insert it into the document, the
  // import Document and CSSStyleSheet pointers are persisted. That means the
  // comparison of active stylesheets is not able to figure out that the order
  // of the stylesheets have changed after insertion.
  //
  // This is also the case when we import the same document twice where the
  // last inserted document is inserted before the first one in dom order where
  // the last would take precedence.
  //
  // Fall back to re-add all sheets to the scoped resolver and recalculate style
  // for the whole document when we remove or insert an import document.
  if (ScopedStyleResolver* resolver = GetDocument().GetScopedStyleResolver()) {
    MarkDocumentDirty();
    resolver->SetNeedsAppendAllSheets();
    GetDocument().SetNeedsStyleRecalc(
        kSubtreeStyleChange, StyleChangeReasonForTracing::Create(
                                 StyleChangeReason::kActiveStylesheetsUpdate));
  }
}

void StyleEngine::V0ShadowAddedOnV1Document() {
  // No need to look into the ScopedStyleResolver for document, as ::slotted
  // never matches anything in a document tree.
  for (TreeScope* tree_scope : active_tree_scopes_) {
    if (ScopedStyleResolver* resolver = tree_scope->GetScopedStyleResolver())
      resolver->V0ShadowAddedOnV1Document();
  }
}

namespace {

enum RuleSetFlags {
  kFontFaceRules = 1 << 0,
  kKeyframesRules = 1 << 1,
  kFullRecalcRules = 1 << 2
};

unsigned GetRuleSetFlags(const HeapHashSet<Member<RuleSet>> rule_sets) {
  unsigned flags = 0;
  for (auto& rule_set : rule_sets) {
    rule_set->CompactRulesIfNeeded();
    if (!rule_set->KeyframesRules().IsEmpty())
      flags |= kKeyframesRules;
    if (!rule_set->FontFaceRules().IsEmpty())
      flags |= kFontFaceRules;
    if (rule_set->NeedsFullRecalcForRuleSetInvalidation())
      flags |= kFullRecalcRules;
  }
  return flags;
}

}  // namespace

void StyleEngine::ApplyRuleSetChanges(
    TreeScope& tree_scope,
    const ActiveStyleSheetVector& old_style_sheets,
    const ActiveStyleSheetVector& new_style_sheets,
    InvalidationScope invalidation_scope) {
  DCHECK(IsMaster());
  DCHECK(global_rule_set_);
  HeapHashSet<Member<RuleSet>> changed_rule_sets;

  ActiveSheetsChange change = CompareActiveStyleSheets(
      old_style_sheets, new_style_sheets, changed_rule_sets);
  bool append_all_sheets = false;

  if (invalidation_scope == kInvalidateCurrentScope) {
    if (ScopedStyleResolver* scoped_resolver =
            tree_scope.GetScopedStyleResolver())
      append_all_sheets = scoped_resolver->NeedsAppendAllSheets();

    // When the font cache is dirty we have to rebuild it and then add all the
    // @font-face rules in the document scope.
    if (IsFontCacheDirty()) {
      DCHECK(tree_scope.RootNode().IsDocumentNode());
      append_all_sheets = true;
    }

    if (change == kNoActiveSheetsChanged && !append_all_sheets)
      return;
  }

  // With rules added or removed, we need to re-aggregate rule meta data.
  global_rule_set_->MarkDirty();

  unsigned changed_rule_flags = GetRuleSetFlags(changed_rule_sets);
  bool fonts_changed = tree_scope.RootNode().IsDocumentNode() &&
                       (changed_rule_flags & kFontFaceRules);
  bool keyframes_changed = changed_rule_flags & kKeyframesRules;
  unsigned append_start_index = 0;

  // We don't need to mark the font cache dirty if new sheets are appended.
  if (fonts_changed && (invalidation_scope == kInvalidateAllScopes ||
                        change == kActiveSheetsChanged)) {
    MarkFontCacheDirty();
  }

  if (invalidation_scope == kInvalidateAllScopes) {
    if (keyframes_changed) {
      if (change == kActiveSheetsChanged)
        ClearKeyframeRules();

      for (auto* it = new_style_sheets.begin(); it != new_style_sheets.end();
           it++) {
        DCHECK(it->second);
        AddKeyframeRules(*it->second);
      }
    }
  }

  if (invalidation_scope == kInvalidateCurrentScope) {
    if (IsFontCacheDirty()) {
      DCHECK(tree_scope.RootNode().IsDocumentNode());
      DCHECK(change != kActiveSheetsAppended || append_all_sheets);
      RefreshFontCache();
    }

    // - If all sheets were removed, we remove the ScopedStyleResolver.
    // - If new sheets were appended to existing ones, start appending after the
    //   common prefix.
    // - For other diffs, reset author style and re-add all sheets for the
    //   TreeScope.
    if (tree_scope.GetScopedStyleResolver()) {
      if (new_style_sheets.IsEmpty())
        ResetAuthorStyle(tree_scope);
      else if (change == kActiveSheetsAppended && !append_all_sheets)
        append_start_index = old_style_sheets.size();
      else
        tree_scope.GetScopedStyleResolver()->ResetAuthorStyle();
    }

    if (!new_style_sheets.IsEmpty()) {
      tree_scope.EnsureScopedStyleResolver().AppendActiveStyleSheets(
          append_start_index, new_style_sheets);
    }
  }

  if (tree_scope.GetDocument().HasPendingForcedStyleRecalc())
    return;

  if (!tree_scope.GetDocument().body() ||
      tree_scope.GetDocument().HasNodesWithPlaceholderStyle()) {
    tree_scope.GetDocument().SetNeedsStyleRecalc(
        kSubtreeStyleChange, StyleChangeReasonForTracing::Create(
                                 StyleChangeReason::kCleanupPlaceholderStyles));
    return;
  }

  if (changed_rule_sets.IsEmpty())
    return;

  if (keyframes_changed)
    ScopedStyleResolver::KeyframesRulesAdded(tree_scope);

  Node& invalidation_root =
      ScopedStyleResolver::InvalidationRootForTreeScope(tree_scope);
  if (invalidation_root.GetStyleChangeType() >= kSubtreeStyleChange)
    return;

  if (fonts_changed || (changed_rule_flags & kFullRecalcRules)) {
    invalidation_root.SetNeedsStyleRecalc(
        kSubtreeStyleChange, StyleChangeReasonForTracing::Create(
                                 StyleChangeReason::kActiveStylesheetsUpdate));
    return;
  }

  ScheduleInvalidationsForRuleSets(tree_scope, changed_rule_sets,
                                   invalidation_scope);
}

const MediaQueryEvaluator& StyleEngine::EnsureMediaQueryEvaluator() {
  if (!media_query_evaluator_) {
    if (GetDocument().GetFrame()) {
      media_query_evaluator_ =
          new MediaQueryEvaluator(GetDocument().GetFrame());
    } else {
      media_query_evaluator_ = new MediaQueryEvaluator("all");
    }
  }
  return *media_query_evaluator_;
}

bool StyleEngine::MediaQueryAffectedByViewportChange() {
  DCHECK(IsMaster());
  DCHECK(global_rule_set_);
  const MediaQueryEvaluator& evaluator = EnsureMediaQueryEvaluator();
  const auto& results = global_rule_set_->GetRuleFeatureSet()
                            .ViewportDependentMediaQueryResults();
  for (unsigned i = 0; i < results.size(); ++i) {
    if (evaluator.Eval(results[i].Expression()) != results[i].Result())
      return true;
  }
  return false;
}

bool StyleEngine::MediaQueryAffectedByDeviceChange() {
  DCHECK(IsMaster());
  DCHECK(global_rule_set_);
  const MediaQueryEvaluator& evaluator = EnsureMediaQueryEvaluator();
  const auto& results =
      global_rule_set_->GetRuleFeatureSet().DeviceDependentMediaQueryResults();
  for (unsigned i = 0; i < results.size(); ++i) {
    if (evaluator.Eval(results[i].Expression()) != results[i].Result())
      return true;
  }
  return false;
}

bool StyleEngine::UpdateRemUnits(const ComputedStyle* old_root_style,
                                 const ComputedStyle* new_root_style) {
  if (!UsesRemUnits())
    return false;
  if (!old_root_style ||
      old_root_style->FontSize() != new_root_style->FontSize()) {
    DCHECK(Resolver());
    // Resolved rem units are stored in the matched properties cache so we need
    // to make sure to invalidate the cache if the documentElement font size
    // changes.
    Resolver()->InvalidateMatchedPropertiesCache();
    return true;
  }
  return false;
}

void StyleEngine::CustomPropertyRegistered() {
  // TODO(timloh): Invalidate only elements with this custom property set
  GetDocument().SetNeedsStyleRecalc(
      kSubtreeStyleChange, StyleChangeReasonForTracing::Create(
                               StyleChangeReason::kPropertyRegistration));
  if (resolver_)
    resolver_->InvalidateMatchedPropertiesCache();
}

void StyleEngine::EnvironmentVariableChanged() {
  GetDocument().SetNeedsStyleRecalc(
      kSubtreeStyleChange, StyleChangeReasonForTracing::Create(
                               StyleChangeReason::kPropertyRegistration));
  if (resolver_)
    resolver_->InvalidateMatchedPropertiesCache();
}

void StyleEngine::MarkForWhitespaceReattachment() {
  for (auto element : whitespace_reattach_set_) {
    if (element->NeedsReattachLayoutTree() || !element->GetLayoutObject())
      continue;
    if (Node* first_child = LayoutTreeBuilderTraversal::FirstChild(*element))
      first_child->MarkAncestorsWithChildNeedsReattachLayoutTree();
  }
}

void StyleEngine::NodeWillBeRemoved(Node& node) {
  if (node.IsElementNode()) {
    pending_invalidations_.RescheduleSiblingInvalidationsAsDescendants(
        ToElement(node));
  }

  // Mark closest ancestor with with LayoutObject to have all whitespace
  // children being considered for re-attachment during the layout tree build.

  LayoutObject* layout_object = node.GetLayoutObject();
  // The removed node does not have a layout object. No sibling whitespace nodes
  // will change rendering.
  if (!layout_object)
    return;
  // Floating or out-of-flow elements do not affect whitespace siblings.
  if (layout_object->IsFloatingOrOutOfFlowPositioned())
    return;
  layout_object = layout_object->Parent();
  while (layout_object->IsAnonymous())
    layout_object = layout_object->Parent();
  DCHECK(layout_object);
  DCHECK(layout_object->GetNode());
  if (layout_object->GetNode()->IsElementNode()) {
    whitespace_reattach_set_.insert(ToElement(layout_object->GetNode()));
    GetDocument().ScheduleLayoutTreeUpdateIfNeeded();
  }
}

void StyleEngine::ChildrenRemoved(ContainerNode& parent) {
  if (!parent.isConnected())
    return;
  if (in_dom_removal_) {
    // This is necessary for nested removals. There are elements which
    // removes parts of its UA shadow DOM as part of being removed which means
    // we do a removal from within another removal where isConnected() is not
    // completely up to date which would confuse this code. Instead we will
    // clean traversal roots properly when we are called from the outer remove.
    // TODO(crbug.com/882869): MediaControlLoadingPanelElement
    // TODO(crbug.com/888448): TextFieldInputType::ListAttributeTargetChanged
    return;
  }
  style_invalidation_root_.ChildrenRemoved(parent);
  style_recalc_root_.ChildrenRemoved(parent);
  DCHECK(!layout_tree_rebuild_root_.GetRootNode());
  layout_tree_rebuild_root_.ChildrenRemoved(parent);
}

void StyleEngine::CollectMatchingUserRules(
    ElementRuleCollector& collector) const {
  for (unsigned i = 0; i < active_user_style_sheets_.size(); ++i) {
    DCHECK(active_user_style_sheets_[i].second);
    collector.CollectMatchingRules(
        MatchRequest(active_user_style_sheets_[i].second, nullptr,
                     active_user_style_sheets_[i].first, i));
  }
}

void StyleEngine::AddFontFaceRules(const RuleSet& rule_set) {
  if (!font_selector_)
    return;

  const HeapVector<Member<StyleRuleFontFace>> font_face_rules =
      rule_set.FontFaceRules();
  for (auto& font_face_rule : font_face_rules) {
    if (FontFace* font_face = FontFace::Create(document_, font_face_rule))
      font_selector_->GetFontFaceCache()->Add(font_face_rule, font_face);
  }
  if (resolver_ && font_face_rules.size())
    resolver_->InvalidateMatchedPropertiesCache();
}

void StyleEngine::AddKeyframeRules(const RuleSet& rule_set) {
  const HeapVector<Member<StyleRuleKeyframes>> keyframes_rules =
      rule_set.KeyframesRules();
  for (unsigned i = 0; i < keyframes_rules.size(); ++i)
    AddKeyframeStyle(keyframes_rules[i]);
}

void StyleEngine::AddKeyframeStyle(StyleRuleKeyframes* rule) {
  AtomicString animation_name(rule->GetName());

  if (rule->IsVendorPrefixed()) {
    KeyframesRuleMap::iterator it = keyframes_rule_map_.find(animation_name);
    if (it == keyframes_rule_map_.end())
      keyframes_rule_map_.Set(animation_name, rule);
    else if (it->value->IsVendorPrefixed())
      keyframes_rule_map_.Set(animation_name, rule);
  } else {
    keyframes_rule_map_.Set(animation_name, rule);
  }
}

StyleRuleKeyframes* StyleEngine::KeyframeStylesForAnimation(
    const AtomicString& animation_name) {
  if (keyframes_rule_map_.IsEmpty())
    return nullptr;

  KeyframesRuleMap::iterator it = keyframes_rule_map_.find(animation_name);
  if (it == keyframes_rule_map_.end())
    return nullptr;

  return it->value.Get();
}

DocumentStyleEnvironmentVariables& StyleEngine::EnsureEnvironmentVariables() {
  if (!environment_variables_) {
    environment_variables_ = DocumentStyleEnvironmentVariables::Create(
        StyleEnvironmentVariables::GetRootInstance(), *document_);
  }
  return *environment_variables_.get();
}

void StyleEngine::RecalcStyle(StyleRecalcChange change) {
  DCHECK(GetDocument().documentElement());
  DCHECK(GetDocument().ChildNeedsStyleRecalc() || change == kForce);

  Element& root_element = style_recalc_root_.RootElement();
  if (change == kForce || &root_element == GetDocument().documentElement()) {
    GetDocument().documentElement()->RecalcStyle(change);
  } else {
    Element* parent = root_element.ParentOrShadowHostElement();
    DCHECK(parent);
    SelectorFilterAncestorScope filter_scope(*parent);
    root_element.RecalcStyle(change);
  }
  for (ContainerNode* ancestor = root_element.ParentOrShadowHostNode();
       ancestor; ancestor = ancestor->ParentOrShadowHostNode()) {
    if (ancestor->IsElementNode())
      ToElement(ancestor)->RecalcStyleForTraversalRootAncestor();
    ancestor->ClearChildNeedsStyleRecalc();
  }
  style_recalc_root_.Clear();
}

void StyleEngine::RebuildLayoutTree() {
  DCHECK(GetDocument().documentElement());
  DCHECK(GetDocument().ChildNeedsReattachLayoutTree());
  DCHECK(!InRebuildLayoutTree());
  in_layout_tree_rebuild_ = true;

  Element& root_element = layout_tree_rebuild_root_.RootElement();
  {
    WhitespaceAttacher whitespace_attacher;
    root_element.RebuildLayoutTree(whitespace_attacher);
  }

  for (ContainerNode* ancestor = root_element.GetReattachParent(); ancestor;
       ancestor = ancestor->GetReattachParent()) {
    if (ancestor->IsElementNode())
      ToElement(ancestor)->RebuildLayoutTreeForTraversalRootAncestor();
    ancestor->ClearChildNeedsStyleRecalc();
    ancestor->ClearChildNeedsReattachLayoutTree();
  }
  layout_tree_rebuild_root_.Clear();
  in_layout_tree_rebuild_ = false;
}

void StyleEngine::UpdateStyleInvalidationRoot(ContainerNode* ancestor,
                                              Node* dirty_node) {
  DCHECK(IsMaster());
  if (GetDocument().IsActive())
    style_invalidation_root_.Update(ancestor, dirty_node);
}

void StyleEngine::UpdateStyleRecalcRoot(ContainerNode* ancestor,
                                        Node* dirty_node) {
  if (!GetDocument().IsActive())
    return;
  if (in_layout_tree_rebuild_) {
    // TODO(futhark@chromium.org): This happens because we call
    // LazyReattachIfAttached() from HTMLSlotElement::DetachLayoutTree(). We
    // probably want to get rid of LazyReattachIfAttached() altogether and call
    // DetachLayoutTree on assigned nodes instead.
    DCHECK_EQ(dirty_node->GetStyleChangeType(), kNeedsReattachStyleChange);
    return;
  }
  style_recalc_root_.Update(ancestor, dirty_node);
}

void StyleEngine::UpdateLayoutTreeRebuildRoot(ContainerNode* ancestor,
                                              Node* dirty_node) {
  if (GetDocument().IsActive())
    layout_tree_rebuild_root_.Update(ancestor, dirty_node);
}

void StyleEngine::Trace(blink::Visitor* visitor) {
  visitor->Trace(document_);
  visitor->Trace(injected_user_style_sheets_);
  visitor->Trace(injected_author_style_sheets_);
  visitor->Trace(active_user_style_sheets_);
  visitor->Trace(custom_element_default_style_sheets_);
  visitor->Trace(keyframes_rule_map_);
  visitor->Trace(inspector_style_sheet_);
  visitor->Trace(document_style_sheet_collection_);
  visitor->Trace(style_sheet_collection_map_);
  visitor->Trace(dirty_tree_scopes_);
  visitor->Trace(active_tree_scopes_);
  visitor->Trace(tree_boundary_crossing_scopes_);
  visitor->Trace(resolver_);
  visitor->Trace(viewport_resolver_);
  visitor->Trace(media_query_evaluator_);
  visitor->Trace(global_rule_set_);
  visitor->Trace(pending_invalidations_);
  visitor->Trace(style_invalidation_root_);
  visitor->Trace(style_recalc_root_);
  visitor->Trace(layout_tree_rebuild_root_);
  visitor->Trace(whitespace_reattach_set_);
  visitor->Trace(font_selector_);
  visitor->Trace(text_to_sheet_cache_);
  visitor->Trace(sheet_to_text_cache_);
  visitor->Trace(tracker_);
  FontSelectorClient::Trace(visitor);
}

}  // namespace blink
