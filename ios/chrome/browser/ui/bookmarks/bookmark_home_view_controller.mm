// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/bookmarks/bookmark_home_view_controller.h"

#include "base/bind.h"
#include "base/mac/foundation_util.h"
#include "base/metrics/user_metrics.h"
#include "base/strings/sys_string_conversions.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/favicon/core/fallback_url_util.h"
#include "components/favicon/core/favicon_server_fetcher_params.h"
#include "components/favicon/core/large_icon_service.h"
#include "components/favicon_base/fallback_icon_style.h"
#include "components/strings/grit/components_strings.h"
#include "ios/chrome/browser/bookmarks/bookmark_model_factory.h"
#include "ios/chrome/browser/bookmarks/bookmarks_utils.h"
#include "ios/chrome/browser/browser_state/chrome_browser_state.h"
#import "ios/chrome/browser/experimental_flags.h"
#include "ios/chrome/browser/favicon/ios_chrome_large_icon_service_factory.h"
#import "ios/chrome/browser/metrics/new_tab_page_uma.h"
#import "ios/chrome/browser/ui/alert_coordinator/action_sheet_coordinator.h"
#import "ios/chrome/browser/ui/alert_coordinator/alert_coordinator.h"
#import "ios/chrome/browser/ui/authentication/signin_promo_view_configurator.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_edit_view_controller.h"
#include "ios/chrome/browser/ui/bookmarks/bookmark_empty_background.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_folder_editor_view_controller.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_folder_view_controller.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_home_consumer.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_home_mediator.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_home_shared_state.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_home_waiting_view.h"
#include "ios/chrome/browser/ui/bookmarks/bookmark_interaction_controller.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_interaction_controller_delegate.h"
#include "ios/chrome/browser/ui/bookmarks/bookmark_model_bridge_observer.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_navigation_controller.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_path_cache.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_ui_constants.h"
#import "ios/chrome/browser/ui/bookmarks/bookmark_utils_ios.h"
#import "ios/chrome/browser/ui/bookmarks/cells/bookmark_folder_item.h"
#import "ios/chrome/browser/ui/bookmarks/cells/bookmark_home_node_item.h"
#import "ios/chrome/browser/ui/bookmarks/cells/bookmark_table_cell.h"
#import "ios/chrome/browser/ui/bookmarks/cells/bookmark_table_cell_title_edit_delegate.h"
#import "ios/chrome/browser/ui/bookmarks/cells/bookmark_table_signin_promo_cell.h"
#import "ios/chrome/browser/ui/commands/application_commands.h"
#import "ios/chrome/browser/ui/icons/chrome_icon.h"
#import "ios/chrome/browser/ui/keyboard/UIKeyCommand+Chrome.h"
#import "ios/chrome/browser/ui/material_components/utils.h"
#import "ios/chrome/browser/ui/rtl_geometry.h"
#import "ios/chrome/browser/ui/table_view/cells/table_view_url_item.h"
#import "ios/chrome/browser/ui/table_view/chrome_table_view_styler.h"
#import "ios/chrome/browser/ui/table_view/table_view_model.h"
#import "ios/chrome/browser/ui/table_view/table_view_navigation_controller_constants.h"
#import "ios/chrome/browser/ui/ui_util.h"
#import "ios/chrome/browser/ui/uikit_ui_util.h"
#import "ios/chrome/browser/ui/url_loader.h"
#import "ios/chrome/common/favicon/favicon_attributes.h"
#import "ios/chrome/common/favicon/favicon_view.h"
#import "ios/chrome/common/ui_util/constraints_ui_util.h"
#include "ios/chrome/grit/ios_strings.h"
#import "ios/third_party/material_components_ios/src/components/Typography/src/MaterialTypography.h"
#import "ios/web/public/navigation_manager.h"
#include "ios/web/public/referrer.h"
#include "skia/ext/skia_utils_ios.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/l10n/l10n_util_mac.h"

using bookmarks::BookmarkNode;

// Used to store a pair of NSIntegers when storing a NSIndexPath in C++
// collections.
using IntegerPair = std::pair<NSInteger, NSInteger>;

namespace {
typedef NS_ENUM(NSInteger, BookmarksContextBarState) {
  BookmarksContextBarNone,            // No state.
  BookmarksContextBarDefault,         // No selection is possible in this state.
  BookmarksContextBarBeginSelection,  // This is the clean start state,
  // selection is possible, but nothing is
  // selected yet.
  BookmarksContextBarSingleURLSelection,       // Single URL selected state.
  BookmarksContextBarMultipleURLSelection,     // Multiple URLs selected state.
  BookmarksContextBarSingleFolderSelection,    // Single folder selected.
  BookmarksContextBarMultipleFolderSelection,  // Multiple folders selected.
  BookmarksContextBarMixedSelection,  // Multiple URL / Folders selected.
};

// Light gray color that matches the favicon background image color to eliminate
// setting a non-opaque background color.
const CGFloat kFallbackIconDefaultBackgroundColor = 0xf1f3f4;

// Grayscale fallback favicon light gray text color.
const CGFloat kFallbackIconDefaultTextColorWhitePercentage = 0.66;

// Estimated TableView row height.
const CGFloat kEstimatedRowHeight = 65.0;

// TableView rows that are hidden by the NavigationBar, causing them to be
// "visible" for the tableView but not for the user. This is used to calculate
// the top most visibile table view indexPath row.
// TODO(crbug.com/879001): This value is aproximate based on the standard (no
// dynamic type) height. If the dynamic font is too large or too small it will
// result in a small offset on the cache, in order to prevent this we need to
// calculate this value dynamically.
const int kRowsHiddenByNavigationBar = 3;

// NetworkTrafficAnnotationTag for fetching favicon from a Google server.
const net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("bookmarks_get_large_icon", R"(
                                        semantics {
                                        sender: "Bookmarks"
                                        description:
                                          "Sends a request to a Google server to retrieve the favicon bitmap "
                                          "for a bookmark."
                                        trigger:
                                          "A request can be sent if Chrome does not have a favicon for a "
                                          "bookmark."
                                        data: "Page URL and desired icon size."
                                        destination: GOOGLE_OWNED_SERVICE
                                        }
                                        policy {
                                        cookies_allowed: NO
                                        setting: "This feature cannot be disabled by settings."
                                        policy_exception_justification: "Not implemented."
                                        }
                                        )");

// Returns a vector of all URLs in |nodes|.
std::vector<GURL> GetUrlsToOpen(const std::vector<const BookmarkNode*>& nodes) {
  std::vector<GURL> urls;
  for (const BookmarkNode* node : nodes) {
    if (node->is_url()) {
      urls.push_back(node->url());
    }
  }
  return urls;
}

// Shadow opacity for the NavigationController Toolbar.
const CGFloat kShadowOpacity = 0.12f;
// Shadow radius for the NavigationController Toolbar.
const CGFloat kShadowRadius = 12.0f;
}  // namespace

// An AlertCoordinator with the "Action Sheet" style that does not provide an
// anchor rect to its UIPopoverPresentationController. This is used by the
// legacy bookmarks implementation in order to consistently render as an action
// sheet and not as a popover.
@interface LegacyBookmarksActionSheetCoordinator : AlertCoordinator
@end

@implementation LegacyBookmarksActionSheetCoordinator

- (UIAlertController*)alertControllerWithTitle:(NSString*)title
                                       message:(NSString*)message {
  return [UIAlertController
      alertControllerWithTitle:title
                       message:message
                preferredStyle:UIAlertControllerStyleActionSheet];
}

@end

@interface BookmarkHomeViewController ()<BookmarkFolderViewControllerDelegate,
                                         BookmarkHomeConsumer,
                                         BookmarkHomeSharedStateObserver,
                                         BookmarkInteractionControllerDelegate,
                                         BookmarkModelBridgeObserver,
                                         BookmarkTableCellTitleEditDelegate,
                                         UIGestureRecognizerDelegate,
                                         UISearchControllerDelegate,
                                         UISearchResultsUpdating,
                                         UITableViewDataSource,
                                         UITableViewDelegate> {
  // Bridge to register for bookmark changes.
  std::unique_ptr<bookmarks::BookmarkModelBridge> _bridge;

  // The root node, whose child nodes are shown in the bookmark table view.
  const bookmarks::BookmarkNode* _rootNode;

  // YES if NSLayoutConstraits were added.
  BOOL _addedConstraints;

  // Map of favicon load tasks for each index path. Used to keep track of
  // pending favicon load operations so that they can be cancelled upon cell
  // reuse. Keys are (section, item) pairs of cell index paths.
  std::map<IntegerPair, base::CancelableTaskTracker::TaskId> _faviconLoadTasks;
  // Task tracker used for async favicon loads.
  base::CancelableTaskTracker _faviconTaskTracker;
}

// Shared state between BookmarkHome classes.  Used as a temporary refactoring
// aid.
@property(nonatomic, strong) BookmarkHomeSharedState* sharedState;

// The bookmark model used.
@property(nonatomic, assign) bookmarks::BookmarkModel* bookmarks;

// The user's browser state model used.
@property(nonatomic, assign) ios::ChromeBrowserState* browserState;

// The mediator that provides data for this view controller.
@property(nonatomic, strong) BookmarkHomeMediator* mediator;

// The view controller used to pick a folder in which to move the selected
// bookmarks.
@property(nonatomic, strong) BookmarkFolderViewController* folderSelector;

// Object to load URLs.
@property(nonatomic, weak) id<UrlLoader> loader;

// The current state of the context bar UI.
@property(nonatomic, assign) BookmarksContextBarState contextBarState;

// When the view is first shown on the screen, this property represents the
// cached value of the top most visible indexPath row of the table view. This
// property is set to nil after it is used.
@property(nonatomic, assign) int cachedIndexPathRow;

// Set to YES, only when this view controller instance is being created
// from cached path. Once the view controller is shown, this is set to NO.
// This is so that the cache code is called only once in loadBookmarkViews.
@property(nonatomic, assign) BOOL isReconstructingFromCache;

// Dispatcher for sending commands.
@property(nonatomic, readonly, weak) id<ApplicationCommands> dispatcher;

// The current search term.  Set to the empty string when no search is active.
@property(nonatomic, assign) NSString* searchTerm;

// This ViewController's searchController;
@property(nonatomic, strong) UISearchController* searchController;

// Navigation UIToolbar Delete button.
@property(nonatomic, strong) UIBarButtonItem* deleteButton;

// Navigation UIToolbar More button.
@property(nonatomic, strong) UIBarButtonItem* moreButton;

// Scrim when search box in focused.
@property(nonatomic, strong) UIControl* scrimView;

// Background shown when there is no bookmarks or folders at the current root
// node.
@property(nonatomic, strong) BookmarkEmptyBackground* emptyTableBackgroundView;

// The loading spinner background which appears when loading the BookmarkModel
// or syncing.
@property(nonatomic, strong) BookmarkHomeWaitingView* spinnerView;

// The action sheet coordinator, if one is currently being shown.
@property(nonatomic, strong) AlertCoordinator* actionSheetCoordinator;

@property(nonatomic, strong)
    BookmarkInteractionController* bookmarkInteractionController;

@end

@implementation BookmarkHomeViewController

@synthesize bookmarks = _bookmarks;
@synthesize browserState = _browserState;
@synthesize folderSelector = _folderSelector;
@synthesize loader = _loader;
@synthesize homeDelegate = _homeDelegate;
@synthesize contextBarState = _contextBarState;
@synthesize dispatcher = _dispatcher;
@synthesize cachedIndexPathRow = _cachedIndexPathRow;
@synthesize isReconstructingFromCache = _isReconstructingFromCache;
@synthesize sharedState = _sharedState;
@synthesize mediator = _mediator;
@synthesize searchController = _searchController;
@synthesize searchTerm = _searchTerm;
@synthesize deleteButton = _deleteButton;
@synthesize moreButton = _moreButton;
@synthesize scrimView = _scrimView;
@synthesize spinnerView = _spinnerView;
@synthesize emptyTableBackgroundView = _emptyTableBackgroundView;
@synthesize actionSheetCoordinator = _actionSheetCoordinator;
@synthesize bookmarkInteractionController = _bookmarkInteractionController;

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

#pragma mark - Initializer

- (instancetype)initWithLoader:(id<UrlLoader>)loader
                  browserState:(ios::ChromeBrowserState*)browserState
                    dispatcher:(id<ApplicationCommands>)dispatcher {
  DCHECK(browserState);
  if (experimental_flags::IsBookmarksUIRebootEnabled()) {
    self =
        [super initWithTableViewStyle:UITableViewStylePlain
                          appBarStyle:ChromeTableViewControllerStyleNoAppBar];
  } else {
    self =
        [super initWithTableViewStyle:UITableViewStylePlain
                          appBarStyle:ChromeTableViewControllerStyleWithAppBar];
  }
  if (self) {
    _browserState = browserState->GetOriginalChromeBrowserState();
    _loader = loader;
    _dispatcher = dispatcher;

    _bookmarks = ios::BookmarkModelFactory::GetForBrowserState(browserState);

    _bridge.reset(new bookmarks::BookmarkModelBridge(self, _bookmarks));
  }
  return self;
}

- (void)dealloc {
  [self.mediator disconnect];
  _faviconTaskTracker.TryCancelAll();
  _sharedState.tableView.dataSource = nil;
  _sharedState.tableView.delegate = nil;
}

- (void)setRootNode:(const bookmarks::BookmarkNode*)rootNode {
  _rootNode = rootNode;
}

- (NSArray<BookmarkHomeViewController*>*)cachedViewControllerStack {
  // This method is only designed to be called for the view controller
  // associated with the root node.
  DCHECK(self.bookmarks->loaded());
  DCHECK_EQ(_rootNode, self.bookmarks->root_node());

  NSMutableArray<BookmarkHomeViewController*>* stack = [NSMutableArray array];
  // On UIRefresh we need to configure the root controller Navigationbar at this
  // time when reconstructing from cache, or there will be a loading flicker if
  // this gets done on viewDidLoad.
  if (experimental_flags::IsBookmarksUIRebootEnabled())
    [self setupNavigationForBookmarkHomeViewController:self
                                     usingBookmarkNode:_rootNode];
  [stack addObject:self];

  int64_t cachedFolderID;
  int cachedIndexPathRow;
  // If cache is present then reconstruct the last visited bookmark from
  // cache.
  if (![BookmarkPathCache
          getBookmarkTopMostRowCacheWithPrefService:self.browserState
                                                        ->GetPrefs()
                                              model:self.bookmarks
                                           folderId:&cachedFolderID
                                         topMostRow:&cachedIndexPathRow] ||
      cachedFolderID == self.bookmarks->root_node()->id()) {
    return stack;
  }

  NSArray* path =
      bookmark_utils_ios::CreateBookmarkPath(self.bookmarks, cachedFolderID);
  if (!path) {
    return stack;
  }

  DCHECK_EQ(self.bookmarks->root_node()->id(),
            [[path firstObject] longLongValue]);
  for (NSUInteger ii = 1; ii < [path count]; ii++) {
    int64_t nodeID = [[path objectAtIndex:ii] longLongValue];
    const BookmarkNode* node =
        bookmark_utils_ios::FindFolderById(self.bookmarks, nodeID);
    DCHECK(node);
    // if node is an empty permanent node, stop.
    if (node->empty() && IsPrimaryPermanentNode(node, self.bookmarks)) {
      break;
    }

    BookmarkHomeViewController* controller =
        [self createControllerWithRootFolder:node];
    // On UIRefresh we need to configure the controller's Navigationbar at this
    // time when reconstructing from cache, or there will be a loading flicker
    // if this gets done on viewDidLoad.
    if (experimental_flags::IsBookmarksUIRebootEnabled())
      [self setupNavigationForBookmarkHomeViewController:controller
                                       usingBookmarkNode:node];
    if (nodeID == cachedFolderID) {
      controller.cachedIndexPathRow = cachedIndexPathRow;
    }
    [stack addObject:controller];
  }
  return stack;
}

#pragma mark - UIViewController

- (void)viewDidLoad {
  [super viewDidLoad];

  // Set Navigation Bar, Toolbar and TableView appearance.
  if (experimental_flags::IsBookmarksUIRebootEnabled()) {
    self.navigationController.navigationBarHidden = NO;
    self.navigationController.toolbar.translucent = YES;
    // Add a tableFooterView in order to disable separators at the bottom of the
    // tableView.
    self.tableView.tableFooterView = [[UIView alloc] init];
  } else {
    self.navigationController.navigationBarHidden = YES;
    self.navigationController.toolbar.translucent = NO;
    self.navigationController.toolbar.barTintColor = [UIColor whiteColor];
    self.navigationController.toolbar.layer.shadowRadius = kShadowRadius;
    self.navigationController.toolbar.layer.shadowOpacity = kShadowOpacity;
    // Disable separators while the loading spinner is showing.
    // |loadBookmarkView| will bring them back if needed.
    self.tableView.separatorStyle = UITableViewCellSeparatorStyleNone;
  }
  self.navigationController.toolbar.accessibilityIdentifier =
      kBookmarkHomeUIToolbarIdentifier;

  if (experimental_flags::IsBookmarksUIRebootEnabled()) {
    // SearchController Configuration.
    // Init the searchController with nil so the results are displayed on the
    // same TableView.
    self.searchController =
        [[UISearchController alloc] initWithSearchResultsController:nil];
    self.searchController.dimsBackgroundDuringPresentation = NO;
    self.searchController.searchBar.userInteractionEnabled = NO;
    self.searchController.delegate = self;
    self.searchController.searchResultsUpdater = self;
    self.searchController.searchBar.backgroundColor = [UIColor clearColor];
    self.searchController.searchBar.accessibilityIdentifier =
        kBookmarkHomeSearchBarIdentifier;

    // UIKit needs to know which controller will be presenting the
    // searchController. If we don't add this trying to dismiss while
    // SearchController is active will fail.
    self.definesPresentationContext = YES;

    self.scrimView = [[UIControl alloc] initWithFrame:self.tableView.bounds];
    self.scrimView.backgroundColor =
        [UIColor colorWithWhite:0
                          alpha:kTableViewNavigationWhiteAlphaForSearchScrim];
    self.scrimView.translatesAutoresizingMaskIntoConstraints = NO;
    self.scrimView.autoresizingMask =
        UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.scrimView.accessibilityIdentifier = kBookmarkHomeSearchScrimIdentifier;
    [self.scrimView addTarget:self
                       action:@selector(dismissSearchController:)
             forControlEvents:UIControlEventAllTouchEvents];

    // For iOS 11 and later, place the search bar in the navigation bar.
    // Otherwise place the search bar in the table view's header.
    if (@available(iOS 11, *)) {
      self.navigationItem.searchController = self.searchController;
      self.navigationItem.hidesSearchBarWhenScrolling = NO;

      // Center search bar vertically so it looks centered in the header when
      // searching.  The cancel button is centered / decentered on
      // viewWillAppear and viewDidDisappear.
      UIOffset offset =
          UIOffsetMake(0.0f, kTableViewNavigationVerticalOffsetForSearchHeader);
      self.searchController.searchBar.searchFieldBackgroundPositionAdjustment =
          offset;
    } else {
      self.tableView.tableHeaderView = self.searchController.searchBar;
    }

    self.searchTerm = @"";
  }

  if (self.bookmarks->loaded()) {
    [self loadBookmarkViews];
  } else {
    [self showLoadingSpinnerBackground];
  }
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  // Set the delegate here to make sure it is working when navigating in the
  // ViewController hierarchy (as each view controller is setting itself as
  // delegate).
  self.navigationController.interactivePopGestureRecognizer.delegate = self;

  // Hide the toolbar if we're displaying the root node.
  if (self.bookmarks->loaded() &&
      (_rootNode != self.bookmarks->root_node() ||
       self.sharedState.currentlyShowingSearchResults)) {
    self.navigationController.toolbarHidden = NO;
  } else {
    self.navigationController.toolbarHidden = YES;
  }

  if (@available(iOS 11, *)) {
    // Center search bar's cancel button vertically so it looks centered.
    // We change the cancel button proxy styles, so we will return it to
    // default in viewDidDisappear.
    UIOffset offset =
        UIOffsetMake(0.0f, kTableViewNavigationVerticalOffsetForSearchHeader);
    UIBarButtonItem* cancelButton = [UIBarButtonItem
        appearanceWhenContainedInInstancesOfClasses:@[ [UISearchBar class] ]];
    [cancelButton setTitlePositionAdjustment:offset
                               forBarMetrics:UIBarMetricsDefault];
  }
}

- (void)viewWillDisappear:(BOOL)animated {
  [super viewWillDisappear:animated];

  if (@available(iOS 11, *)) {
    // Restore to default origin offset for cancel button proxy style.
    UIBarButtonItem* cancelButton = [UIBarButtonItem
        appearanceWhenContainedInInstancesOfClasses:@[ [UISearchBar class] ]];
    [cancelButton setTitlePositionAdjustment:UIOffsetZero
                               forBarMetrics:UIBarMetricsDefault];
  }
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  // Check that the tableView still contains as many rows, and that
  // |self.cachedIndexPathRow| is not 0.
  if (self.cachedIndexPathRow &&
      [self.tableView numberOfRowsInSection:0] > self.cachedIndexPathRow) {
    NSIndexPath* indexPath =
        [NSIndexPath indexPathForRow:self.cachedIndexPathRow inSection:0];
    [self.tableView scrollToRowAtIndexPath:indexPath
                          atScrollPosition:UITableViewScrollPositionTop
                                  animated:NO];
    self.cachedIndexPathRow = 0;
  }
}

- (BOOL)prefersStatusBarHidden {
  return NO;
}

- (void)traitCollectionDidChange:(UITraitCollection*)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  // Stop edit of current bookmark folder name, if any.
  [self.sharedState.editingFolderCell stopEdit];
}

- (NSArray*)keyCommands {
  __weak BookmarkHomeViewController* weakSelf = self;
  return @[ [UIKeyCommand cr_keyCommandWithInput:UIKeyInputEscape
                                   modifierFlags:Cr_UIKeyModifierNone
                                           title:nil
                                          action:^{
                                            [weakSelf navigationBarCancel:nil];
                                          }] ];
}

- (UIStatusBarStyle)preferredStatusBarStyle {
  return UIStatusBarStyleDefault;
}

#pragma mark - Protected

- (void)loadBookmarkViews {
  DCHECK(_rootNode);
  [self loadModel];

  self.sharedState =
      [[BookmarkHomeSharedState alloc] initWithBookmarkModel:_bookmarks
                                           displayedRootNode:_rootNode];
  self.sharedState.tableViewModel = self.tableViewModel;
  self.sharedState.tableView = self.tableView;
  self.sharedState.observer = self;
  self.sharedState.currentlyShowingSearchResults = NO;

  // Configure the table view.
  self.sharedState.tableView.accessibilityIdentifier = @"bookmarksTableView";
  self.sharedState.tableView.estimatedRowHeight = kEstimatedRowHeight;
  self.tableView.sectionHeaderHeight = 0;
  // Setting a sectionFooterHeight of 0 will be the same as not having a
  // footerView, which shows a cell separator for the last cell. Removing this
  // line will also create a default footer of height 30.
  self.tableView.sectionFooterHeight = 1;
  self.sharedState.tableView.allowsMultipleSelectionDuringEditing = YES;
  if (!experimental_flags::IsBookmarksUIRebootEnabled()) {
    self.sharedState.tableView.separatorStyle =
        UITableViewCellSeparatorStyleNone;
  }

  UILongPressGestureRecognizer* longPressRecognizer =
      [[UILongPressGestureRecognizer alloc]
          initWithTarget:self
                  action:@selector(handleLongPress:)];
  longPressRecognizer.numberOfTouchesRequired = 1;
  longPressRecognizer.delegate = self;
  [self.sharedState.tableView addGestureRecognizer:longPressRecognizer];

  // Create the mediator and hook up the table view.
  self.mediator =
      [[BookmarkHomeMediator alloc] initWithSharedState:self.sharedState
                                           browserState:self.browserState];
  self.mediator.consumer = self;
  [self.mediator startMediating];

  [self setupNavigationForBookmarkHomeViewController:self
                                   usingBookmarkNode:_rootNode];

  [self setupContextBar];

  if (self.isReconstructingFromCache) {
    [self setupUIStackCacheIfApplicable];
  }

  if (experimental_flags::IsBookmarksUIRebootEnabled()) {
    self.searchController.searchBar.userInteractionEnabled = YES;
  }

  DCHECK(self.bookmarks->loaded());
  DCHECK([self isViewLoaded]);
}

- (void)cacheIndexPathRow {
  // Cache IndexPathRow for BookmarkTableView.
  int topMostVisibleIndexPathRow = [self topMostVisibleIndexPathRow];
  [BookmarkPathCache
      cacheBookmarkTopMostRowWithPrefService:self.browserState->GetPrefs()
                                    folderId:_rootNode->id()
                                  topMostRow:topMostVisibleIndexPathRow];
}

#pragma mark - BookmarkHomeConsumer

- (void)refreshContents {
  if (experimental_flags::IsBookmarksUIRebootEnabled() &&
      self.sharedState.currentlyShowingSearchResults) {
    NSString* noResults = l10n_util::GetNSString(IDS_HISTORY_NO_SEARCH_RESULTS);
    [self.mediator computeBookmarkTableViewDataMatching:self.searchTerm
                             orShowMessageWhenNoResults:noResults];
  } else {
    [self.mediator computeBookmarkTableViewData];
  }
  [self cancelAllFaviconLoads];
  [self handleRefreshContextBar];
  [self.sharedState.editingFolderCell stopEdit];
  [self.sharedState.tableView reloadData];
  if (self.sharedState.currentlyInEditMode &&
      !self.sharedState.editNodes.empty()) {
    [self restoreRowSelection];
  }
}

// Asynchronously loads favicon for given index path. The loads are cancelled
// upon cell reuse automatically.  When the favicon is not found in cache, try
// loading it from a Google server if |continueToGoogleServer| is YES,
// otherwise, use the fall back icon style.
- (void)loadFaviconAtIndexPath:(NSIndexPath*)indexPath
        continueToGoogleServer:(BOOL)continueToGoogleServer {
  const bookmarks::BookmarkNode* node = [self nodeAtIndexPath:indexPath];
  if (node->is_folder()) {
    return;
  }

  CGFloat scale = [UIScreen mainScreen].scale;
  CGFloat desiredFaviconSizeInPixel =
      scale * [BookmarkHomeSharedState desiredFaviconSizePt];
  CGFloat minFaviconSizeInPixel =
      scale * [BookmarkHomeSharedState minFaviconSizePt];

  // Start loading a favicon.
  __weak BookmarkHomeViewController* weakSelf = self;
  GURL blockURL(node->url());
  NSString* fallbackText =
      base::SysUTF16ToNSString(favicon::GetFallbackIconText(blockURL));
  void (^faviconLoadedFromCacheBlock)(const favicon_base::LargeIconResult&) = ^(
      const favicon_base::LargeIconResult& result) {
    BookmarkHomeViewController* strongSelf = weakSelf;
    if (!strongSelf) {
      return;
    }
    // TODO(crbug.com/697329) When fetching icon from server to replace existing
    // cache is allowed, fetch icon from server here when cached icon is smaller
    // than the desired size.
    if (!result.bitmap.is_valid() && continueToGoogleServer &&
        strongSelf.sharedState.faviconDownloadCount <
            [BookmarkHomeSharedState maxDownloadFaviconCount]) {
      void (^faviconLoadedFromServerBlock)(
          favicon_base::GoogleFaviconServerRequestStatus status) =
          ^(const favicon_base::GoogleFaviconServerRequestStatus status) {
            if (status ==
                favicon_base::GoogleFaviconServerRequestStatus::SUCCESS) {
              BookmarkHomeViewController* strongSelf = weakSelf;
              // GetLargeIconOrFallbackStyleFromGoogleServerSkippingLocalCache
              // is not cancellable.  So need to check if node has been changed
              // before proceeding to favicon update.  Also when searching,
              // indexPath can point beyond the end of the current list if the
              // icon was for a previous, longer, filtered view of items.
              if (!strongSelf || ![strongSelf hasItemAtIndexPath:indexPath] ||
                  [strongSelf nodeAtIndexPath:indexPath] != node) {
                return;
              }
              // Favicon should be ready in cache now.  Fetch it again.
              [strongSelf loadFaviconAtIndexPath:indexPath
                          continueToGoogleServer:NO];
            }
          };  // faviconLoadedFromServerBlock

      strongSelf.sharedState.faviconDownloadCount++;
      IOSChromeLargeIconServiceFactory::GetForBrowserState(self.browserState)
          ->GetLargeIconOrFallbackStyleFromGoogleServerSkippingLocalCache(
              favicon::FaviconServerFetcherParams::CreateForMobile(
                  node->url(), minFaviconSizeInPixel,
                  desiredFaviconSizeInPixel),
              /*may_page_url_be_private=*/true, kTrafficAnnotation,
              base::BindRepeating(faviconLoadedFromServerBlock));
    }
    // Due to search filtering, we also need to validate the indexPath
    // requested versus what is in the table now.
    if (![strongSelf hasItemAtIndexPath:indexPath] ||
        [strongSelf nodeAtIndexPath:indexPath] != node) {
      return;
    }
    [strongSelf updateCellAtIndexPath:indexPath
                  withLargeIconResult:result
                         fallbackText:fallbackText];
  };  // faviconLoadedFromCacheBlock

  base::CancelableTaskTracker::TaskId taskId =
      IOSChromeLargeIconServiceFactory::GetForBrowserState(self.browserState)
          ->GetLargeIconOrFallbackStyle(
              node->url(), minFaviconSizeInPixel, desiredFaviconSizeInPixel,
              base::BindRepeating(faviconLoadedFromCacheBlock),
              &_faviconTaskTracker);
  _faviconLoadTasks[IntegerPair(indexPath.section, indexPath.item)] = taskId;
}

- (void)updateTableViewBackgroundStyle:(BookmarkHomeBackgroundStyle)style {
  if (style == BookmarkHomeBackgroundStyleDefault) {
    [self hideLoadingSpinnerBackground];
    [self hideEmptyBackground];
  } else if (style == BookmarkHomeBackgroundStyleLoading) {
    [self hideEmptyBackground];
    [self showLoadingSpinnerBackground];
  } else if (style == BookmarkHomeBackgroundStyleEmpty) {
    [self hideLoadingSpinnerBackground];
    [self showEmptyBackground];
  }
}

- (void)showSignin:(ShowSigninCommand*)command {
  [self.dispatcher showSignin:command
           baseViewController:self.navigationController];
}

- (void)configureSigninPromoWithConfigurator:
            (SigninPromoViewConfigurator*)configurator
                                 atIndexPath:(NSIndexPath*)indexPath
                             forceReloadCell:(BOOL)forceReloadCell {
  BookmarkTableSigninPromoCell* signinPromoCell =
      base::mac::ObjCCast<BookmarkTableSigninPromoCell>(
          [self.sharedState.tableView cellForRowAtIndexPath:indexPath]);
  if (!signinPromoCell) {
    return;
  }
  // Should always reconfigure the cell size even if it has to be reloaded,
  // to make sure it has the right size to compute the cell size.
  [configurator configureSigninPromoView:signinPromoCell.signinPromoView];
  if (forceReloadCell) {
    // The section should be reload to update the cell height.
    NSIndexSet* indexSet = [NSIndexSet indexSetWithIndex:indexPath.section];
    [self.sharedState.tableView reloadSections:indexSet
                              withRowAnimation:UITableViewRowAnimationNone];
  }
}

#pragma mark - Action sheet callbacks

// Opens the folder move editor for the given node.
- (void)moveNodes:(const std::set<const BookmarkNode*>&)nodes {
  DCHECK(!self.folderSelector);
  DCHECK(nodes.size() > 0);
  const BookmarkNode* editedNode = *(nodes.begin());
  const BookmarkNode* selectedFolder = editedNode->parent();
  self.folderSelector = [[BookmarkFolderViewController alloc]
      initWithBookmarkModel:self.bookmarks
           allowsNewFolders:YES
                editedNodes:nodes
               allowsCancel:YES
             selectedFolder:selectedFolder];
  self.folderSelector.delegate = self;
  UINavigationController* navController = [[BookmarkNavigationController alloc]
      initWithRootViewController:self.folderSelector];
  [navController setModalPresentationStyle:UIModalPresentationFormSheet];
  [self presentViewController:navController animated:YES completion:NULL];
}

// Deletes the current node.
- (void)deleteNodes:(const std::set<const BookmarkNode*>&)nodes {
  DCHECK_GE(nodes.size(), 1u);
  bookmark_utils_ios::DeleteBookmarksWithUndoToast(nodes, self.bookmarks,
                                                   self.browserState);
  [self setTableViewEditing:NO];
}

// Opens the editor on the given node.
- (void)editNode:(const BookmarkNode*)node {
  if (!self.bookmarkInteractionController) {
    self.bookmarkInteractionController = [[BookmarkInteractionController alloc]
        initWithBrowserState:self.browserState
                      loader:self.loader
            parentController:self
                  dispatcher:self.dispatcher];
    self.bookmarkInteractionController.delegate = self;
  }

  [self.bookmarkInteractionController presentEditorForNode:node];
}

- (void)openAllNodes:(const std::vector<const bookmarks::BookmarkNode*>&)nodes
         inIncognito:(BOOL)inIncognito
              newTab:(BOOL)newTab {
  [self cacheIndexPathRow];
  std::vector<GURL> urls = GetUrlsToOpen(nodes);
  [self.homeDelegate bookmarkHomeViewControllerWantsDismissal:self
                                             navigationToUrls:urls
                                                  inIncognito:inIncognito
                                                       newTab:newTab];
}

#pragma mark - Navigation Bar Callbacks

- (void)navigationBarCancel:(id)sender {
  [self navigateAway];
  [self dismissWithURL:GURL()];
}

#pragma mark - More Private Methods

- (void)handleSelectUrlForNavigation:(const GURL&)url {
  [self dismissWithURL:url];
}

- (void)handleSelectFolderForNavigation:(const bookmarks::BookmarkNode*)folder {
  BookmarkHomeViewController* controller =
      [self createControllerWithRootFolder:folder];
  [self.navigationController pushViewController:controller animated:YES];
}

- (void)handleSelectNodesForDeletion:
    (const std::set<const bookmarks::BookmarkNode*>&)nodes {
  [self deleteNodes:nodes];
}

- (void)handleSelectEditNodes:
    (const std::set<const bookmarks::BookmarkNode*>&)nodes {
  // Early return if bookmarks table is not in edit mode.
  if (!self.sharedState.currentlyInEditMode) {
    return;
  }

  if (nodes.size() == 0) {
    // if nothing to select, exit edit mode.
    if (![self hasBookmarksOrFolders]) {
      [self setTableViewEditing:NO];
      return;
    }
    [self setContextBarState:BookmarksContextBarBeginSelection];
    return;
  }
  if (nodes.size() == 1) {
    const bookmarks::BookmarkNode* node = *nodes.begin();
    if (node->is_url()) {
      [self setContextBarState:BookmarksContextBarSingleURLSelection];
    } else if (node->is_folder()) {
      [self setContextBarState:BookmarksContextBarSingleFolderSelection];
    }
    return;
  }

  BOOL foundURL = NO;
  BOOL foundFolder = NO;
  for (const BookmarkNode* node : nodes) {
    if (!foundURL && node->is_url()) {
      foundURL = YES;
    } else if (!foundFolder && node->is_folder()) {
      foundFolder = YES;
    }
    // Break early, if we found both types of nodes.
    if (foundURL && foundFolder) {
      break;
    }
  }

  // Only URLs are selected.
  if (foundURL && !foundFolder) {
    [self setContextBarState:BookmarksContextBarMultipleURLSelection];
    return;
  }
  // Only Folders are selected.
  if (!foundURL && foundFolder) {
    [self setContextBarState:BookmarksContextBarMultipleFolderSelection];
    return;
  }
  // Mixed selection.
  if (foundURL && foundFolder) {
    [self setContextBarState:BookmarksContextBarMixedSelection];
    return;
  }

  NOTREACHED();
  return;
}

- (void)handleMoveNode:(const bookmarks::BookmarkNode*)node
            toPosition:(int)position {
  bookmark_utils_ios::UpdateBookmarkPositionWithUndoToast(
      node, _rootNode, position, self.bookmarks, self.browserState);
}

- (void)handleRefreshContextBar {
  // At default state, the enable state of context bar buttons could change
  // during refresh.
  if (self.contextBarState == BookmarksContextBarDefault) {
    [self setBookmarksContextBarButtonsDefaultState];
  }
}

- (BOOL)isAtTopOfNavigation {
  return (self.navigationController.topViewController == self);
}

#pragma mark - BookmarkTableCellTitleEditDelegate

- (void)textDidChangeTo:(NSString*)newName {
  DCHECK(self.sharedState.editingFolderNode);
  self.sharedState.addingNewFolder = NO;
  if (newName.length > 0) {
    self.sharedState.bookmarkModel->SetTitle(self.sharedState.editingFolderNode,
                                             base::SysNSStringToUTF16(newName));
  }
  self.sharedState.editingFolderNode = nullptr;
  self.sharedState.editingFolderCell = nil;
  [self refreshContents];
}

#pragma mark - BookmarkFolderViewControllerDelegate

- (void)folderPicker:(BookmarkFolderViewController*)folderPicker
    didFinishWithFolder:(const BookmarkNode*)folder {
  DCHECK(folder);
  DCHECK(!folder->is_url());
  DCHECK_GE(folderPicker.editedNodes.size(), 1u);

  bookmark_utils_ios::MoveBookmarksWithUndoToast(
      folderPicker.editedNodes, self.bookmarks, folder, self.browserState);

  [self setTableViewEditing:NO];
  [self.navigationController dismissViewControllerAnimated:YES completion:NULL];
  self.folderSelector.delegate = nil;
  self.folderSelector = nil;
}

- (void)folderPickerDidCancel:(BookmarkFolderViewController*)folderPicker {
  [self setTableViewEditing:NO];
  [self.navigationController dismissViewControllerAnimated:YES completion:NULL];
  self.folderSelector.delegate = nil;
  self.folderSelector = nil;
}

#pragma mark - BookmarkInteractionControllerDelegate

- (void)bookmarkInteractionControllerWillCommitTitleOrUrlChange:
    (BookmarkInteractionController*)controller {
  [self setTableViewEditing:NO];
}

- (void)bookmarkInteractionControllerDidStop:
    (BookmarkInteractionController*)controller {
  // TODO(crbug.com/805182): Use this method to tear down
  // |self.bookmarkInteractionController|.
}

#pragma mark - BookmarkModelBridgeObserver

- (void)bookmarkModelLoaded {
  DCHECK(!_rootNode);
  [self setRootNode:self.bookmarks->root_node()];

  // If the view hasn't loaded yet, then return early. The eventual call to
  // viewDidLoad will properly initialize the views.  This early return must
  // come *after* the call to setRootNode above.
  if (![self isViewLoaded])
    return;

  int64_t unusedFolderId;
  int unusedIndexPathRow;
  // Bookmark Model is loaded after presenting Bookmarks,  we need to check
  // again here if restoring of cache position is needed.  It is to prevent
  // crbug.com/765503.
  if ([BookmarkPathCache
          getBookmarkTopMostRowCacheWithPrefService:self.browserState
                                                        ->GetPrefs()
                                              model:self.bookmarks
                                           folderId:&unusedFolderId
                                         topMostRow:&unusedIndexPathRow]) {
    self.isReconstructingFromCache = YES;
  }

  DCHECK(self.spinnerView);
  __weak BookmarkHomeViewController* weakSelf = self;
  [self.spinnerView stopWaitingWithCompletion:^{
    BookmarkHomeViewController* strongSelf = weakSelf;
    // Early return if the controller has been deallocated.
    if (!strongSelf)
      return;
    [UIView animateWithDuration:kTableViewNavigationScrimFadeDuration
        animations:^{
          strongSelf.spinnerView.alpha = 0.0;
        }
        completion:^(BOOL finished) {
          self.sharedState.tableView.backgroundView = nil;
          self.spinnerView = nil;
        }];
    [strongSelf loadBookmarkViews];
    [strongSelf.sharedState.tableView reloadData];
  }];
}

- (void)bookmarkNodeChanged:(const BookmarkNode*)node {
  // No-op here.  Bookmarks might be refreshed in BookmarkHomeMediator.
}

- (void)bookmarkNodeChildrenChanged:(const BookmarkNode*)bookmarkNode {
  // No-op here.  Bookmarks might be refreshed in BookmarkHomeMediator.
}

- (void)bookmarkNode:(const BookmarkNode*)bookmarkNode
     movedFromParent:(const BookmarkNode*)oldParent
            toParent:(const BookmarkNode*)newParent {
  // No-op here.  Bookmarks might be refreshed in BookmarkHomeMediator.
}

- (void)bookmarkNodeDeleted:(const BookmarkNode*)node
                 fromFolder:(const BookmarkNode*)folder {
  if (_rootNode == node) {
    [self setTableViewEditing:NO];
  }
}

- (void)bookmarkModelRemovedAllNodes {
  // No-op
}

#pragma mark - Accessibility

- (BOOL)accessibilityPerformEscape {
  [self dismissWithURL:GURL()];
  return YES;
}

#pragma mark - private

// Check if any of our controller is presenting. We don't consider when this
// controller is presenting the search controller.
// Note that when adding a controller that can present, it should be added in
// context here.
- (BOOL)isAnyControllerPresenting {
  return (([self presentedViewController] &&
           [self presentedViewController] != self.searchController) ||
          [self.searchController presentedViewController] ||
          [self.navigationController presentedViewController]);
}

- (void)setupUIStackCacheIfApplicable {
  self.isReconstructingFromCache = NO;

  NSArray<BookmarkHomeViewController*>* replacementViewControllers =
      [self cachedViewControllerStack];
  DCHECK(replacementViewControllers);
  [self.navigationController setViewControllers:replacementViewControllers];
}

// Set up context bar for the new UI.
- (void)setupContextBar {
  if (_rootNode != self.bookmarks->root_node() ||
      self.sharedState.currentlyShowingSearchResults) {
    self.navigationController.toolbarHidden = NO;
    [self setContextBarState:BookmarksContextBarDefault];
  } else {
    self.navigationController.toolbarHidden = YES;
  }
}

// Set up navigation bar for |viewController|'s navigationBar using |node|.
- (void)setupNavigationForBookmarkHomeViewController:
            (BookmarkHomeViewController*)viewController
                                   usingBookmarkNode:
                                       (const bookmarks::BookmarkNode*)node {
  if (experimental_flags::IsBookmarksUIRebootEnabled()) {
    viewController.navigationItem.leftBarButtonItem.action = @selector(back);
    // Disable large titles on every VC but the root controller.
    if (@available(iOS 11, *)) {
      if (node != self.bookmarks->root_node()) {
        viewController.navigationItem.largeTitleDisplayMode =
            UINavigationItemLargeTitleDisplayModeNever;
      }
    }
  } else {
    viewController.navigationController.navigationBarHidden = YES;
    if (viewController.navigationController.viewControllers.count > 1) {
      // Add custom back button.
      UIBarButtonItem* backButton =
          [ChromeIcon templateBarButtonItemWithImage:[ChromeIcon backIcon]
                                              target:self
                                              action:@selector(back)];
      viewController.navigationItem.leftBarButtonItem = backButton;
    }
  }

  // Add custom title.
  viewController.title = bookmark_utils_ios::TitleForBookmarkNode(node);

  // Add custom done button.
  viewController.navigationItem.rightBarButtonItem =
      [self customizedDoneButton];
}

// Back button callback for the new ui.
- (void)back {
  [self navigateAway];
  [self.navigationController popViewControllerAnimated:YES];
}

- (UIBarButtonItem*)customizedDoneButton {
  UIBarButtonItem* doneButton = [[UIBarButtonItem alloc]
      initWithTitle:l10n_util::GetNSString(IDS_IOS_NAVIGATION_BAR_DONE_BUTTON)
              style:UIBarButtonItemStyleDone
             target:self
             action:@selector(navigationBarCancel:)];
  doneButton.accessibilityLabel =
      l10n_util::GetNSString(IDS_IOS_NAVIGATION_BAR_DONE_BUTTON);
  doneButton.accessibilityIdentifier =
      kBookmarkHomeNavigationBarDoneButtonIdentifier;
  return doneButton;
}

// Saves the current position and asks the delegate to open the url, if delegate
// is set, otherwise opens the URL using loader.
- (void)dismissWithURL:(const GURL&)url {
  [self cacheIndexPathRow];
  if (self.homeDelegate) {
    std::vector<GURL> urls;
    if (url.is_valid())
      urls.push_back(url);
    [self.homeDelegate bookmarkHomeViewControllerWantsDismissal:self
                                               navigationToUrls:urls];
  } else {
    // Before passing the URL to the block, make sure the block has a copy of
    // the URL and not just a reference.
    const GURL localUrl(url);
    dispatch_async(dispatch_get_main_queue(), ^{
      [self loadURL:localUrl];
    });
  }
}

- (void)loadURL:(const GURL&)url {
  if (url.is_empty() || url.SchemeIs(url::kJavaScriptScheme))
    return;

  new_tab_page_uma::RecordAction(self.browserState,
                                 new_tab_page_uma::ACTION_OPENED_BOOKMARK);
  base::RecordAction(
      base::UserMetricsAction("MobileBookmarkManagerEntryOpened"));
  web::NavigationManager::WebLoadParams params(url);
  params.transition_type = ui::PAGE_TRANSITION_AUTO_BOOKMARK;
  [self.loader loadURLWithParams:params];
}

- (void)addNewFolder {
  [self.sharedState.editingFolderCell stopEdit];
  if (!self.sharedState.tableViewDisplayedRootNode) {
    return;
  }
  self.sharedState.addingNewFolder = YES;
  base::string16 folderTitle = base::SysNSStringToUTF16(
      l10n_util::GetNSString(IDS_IOS_BOOKMARK_NEW_GROUP_DEFAULT_NAME));
  self.sharedState.editingFolderNode =
      self.sharedState.bookmarkModel->AddFolder(
          self.sharedState.tableViewDisplayedRootNode,
          self.sharedState.tableViewDisplayedRootNode->child_count(),
          folderTitle);

  BookmarkHomeNodeItem* nodeItem = [[BookmarkHomeNodeItem alloc]
      initWithType:BookmarkHomeItemTypeBookmark
      bookmarkNode:self.sharedState.editingFolderNode];
  [self.sharedState.tableViewModel
                      addItem:nodeItem
      toSectionWithIdentifier:BookmarkHomeSectionIdentifierBookmarks];

  // Insert the new folder cell at the end of the table.
  NSIndexPath* newRowIndexPath =
      [self.sharedState.tableViewModel indexPathForItem:nodeItem];
  NSMutableArray* newRowIndexPaths =
      [[NSMutableArray alloc] initWithObjects:newRowIndexPath, nil];
  [self.sharedState.tableView beginUpdates];
  [self.sharedState.tableView
      insertRowsAtIndexPaths:newRowIndexPaths
            withRowAnimation:UITableViewRowAnimationNone];
  [self.sharedState.tableView endUpdates];

  // Scroll to the end of the table
  [self.sharedState.tableView
      scrollToRowAtIndexPath:newRowIndexPath
            atScrollPosition:UITableViewScrollPositionBottom
                    animated:YES];
}

- (BookmarkHomeViewController*)createControllerWithRootFolder:
    (const bookmarks::BookmarkNode*)folder {
  BookmarkHomeViewController* controller =
      [[BookmarkHomeViewController alloc] initWithLoader:_loader
                                            browserState:self.browserState
                                              dispatcher:self.dispatcher];
  [controller setRootNode:folder];
  controller.homeDelegate = self.homeDelegate;
  return controller;
}

// Sets the editing mode for tableView, update context bar and search state
// accordingly.
- (void)setTableViewEditing:(BOOL)editing {
  self.sharedState.currentlyInEditMode = editing;
  [self setContextBarState:editing ? BookmarksContextBarBeginSelection
                                   : BookmarksContextBarDefault];
  if (experimental_flags::IsBookmarksUIRebootEnabled()) {
    self.searchController.searchBar.userInteractionEnabled = !editing;
    self.searchController.searchBar.alpha =
        editing ? kTableViewNavigationAlphaForDisabledSearchBar : 1.0;
  }
}

// Row selection of the tableView will be cleared after reloadData.  This
// function is used to restore the row selection.  It also updates editNodes in
// case some selected nodes are removed.
- (void)restoreRowSelection {
  // Create a new editNodes set to check if some selected nodes are removed.
  std::set<const bookmarks::BookmarkNode*> newEditNodes;

  // Add selected nodes to editNodes only if they are not removed (still exist
  // in the table).
  NSArray<TableViewItem*>* items = [self.sharedState.tableViewModel
      itemsInSectionWithIdentifier:BookmarkHomeSectionIdentifierBookmarks];
  for (TableViewItem* item in items) {
    BookmarkHomeNodeItem* nodeItem =
        base::mac::ObjCCastStrict<BookmarkHomeNodeItem>(item);
    const BookmarkNode* node = nodeItem.bookmarkNode;
    if (self.sharedState.editNodes.find(node) !=
        self.sharedState.editNodes.end()) {
      newEditNodes.insert(node);
      // Reselect the row of this node.
      NSIndexPath* itemPath =
          [self.sharedState.tableViewModel indexPathForItem:nodeItem];
      [self.sharedState.tableView
          selectRowAtIndexPath:itemPath
                      animated:NO
                scrollPosition:UITableViewScrollPositionNone];
    }
  }

  // if editNodes is changed, update it.
  if (self.sharedState.editNodes.size() != newEditNodes.size()) {
    self.sharedState.editNodes = newEditNodes;
    [self handleSelectEditNodes:self.sharedState.editNodes];
  }
}

- (BOOL)allowsNewFolder {
  // When the current root node has been removed remotely (becomes NULL),
  // or when displaying search results, creating new folder is forbidden.
  return self.sharedState.tableViewDisplayedRootNode != NULL &&
         !self.sharedState.currentlyShowingSearchResults;
}

- (int)topMostVisibleIndexPathRow {
  // If on root node screen, return 0.
  if (self.sharedState.tableViewDisplayedRootNode ==
      self.sharedState.bookmarkModel->root_node()) {
    return 0;
  }

  // If no rows in table, return 0.
  NSArray* visibleIndexPaths = [self.tableView indexPathsForVisibleRows];
  if (!visibleIndexPaths.count)
    return 0;

  // If the first row is still visible, return 0.
  NSIndexPath* topMostIndexPath = [visibleIndexPaths objectAtIndex:0];
  if (topMostIndexPath.row == 0)
    return 0;

  // Return the first visible row not covered by the NavigationBar.
  topMostIndexPath =
      [visibleIndexPaths objectAtIndex:kRowsHiddenByNavigationBar];
  return topMostIndexPath.row;
}

- (void)navigateAway {
  [self.sharedState.editingFolderCell stopEdit];
}

// Returns YES if the given node is a url or folder node.
- (BOOL)isUrlOrFolder:(const BookmarkNode*)node {
  return node->type() == BookmarkNode::URL ||
         node->type() == BookmarkNode::FOLDER;
}

// Returns the bookmark node associated with |indexPath|.
- (const BookmarkNode*)nodeAtIndexPath:(NSIndexPath*)indexPath {
  TableViewItem* item =
      [self.sharedState.tableViewModel itemAtIndexPath:indexPath];

  if (item.type == BookmarkHomeItemTypeBookmark) {
    BookmarkHomeNodeItem* nodeItem =
        base::mac::ObjCCastStrict<BookmarkHomeNodeItem>(item);
    return nodeItem.bookmarkNode;
  }

  NOTREACHED();
  return nullptr;
}

- (BOOL)hasItemAtIndexPath:(NSIndexPath*)indexPath {
  return [self.sharedState.tableViewModel hasItemAtIndexPath:indexPath];
}

- (BOOL)hasBookmarksOrFolders {
  return self.sharedState.tableViewDisplayedRootNode &&
         !self.sharedState.tableViewDisplayedRootNode->empty();
}

- (std::vector<const bookmarks::BookmarkNode*>)getEditNodesInVector {
  std::vector<const bookmarks::BookmarkNode*> nodes;
  if (self.sharedState.currentlyShowingSearchResults) {
    // Create a vector of edit nodes in the same order as the selected nodes.
    const std::set<const bookmarks::BookmarkNode*> editNodes =
        self.sharedState.editNodes;
    std::copy(editNodes.begin(), editNodes.end(), std::back_inserter(nodes));
  } else {
    // Create a vector of edit nodes in the same order as the nodes in folder.
    int childCount = self.sharedState.tableViewDisplayedRootNode->child_count();
    for (int i = 0; i < childCount; ++i) {
      const BookmarkNode* node =
          self.sharedState.tableViewDisplayedRootNode->GetChild(i);
      if (self.sharedState.editNodes.find(node) !=
          self.sharedState.editNodes.end()) {
        nodes.push_back(node);
      }
    }
  }
  return nodes;
}

// Dismiss the search controller when there's a touch event on the scrim.
- (void)dismissSearchController:(UIControl*)sender {
  if (self.searchController.active) {
    self.searchController.active = NO;
  }
}

// Show scrim overlay and hide toolbar.
- (void)showScrim {
  self.navigationController.toolbarHidden = YES;
  self.scrimView.alpha = 0.0f;
  CGSize contentSize = self.tableView.contentSize;
  self.scrimView.frame =
      CGRectMake(0.0f, 0.0f, contentSize.width,
                 std::max(contentSize.height, self.view.bounds.size.height));
  [self.tableView addSubview:self.scrimView];
  [UIView animateWithDuration:kTableViewNavigationScrimFadeDuration
                   animations:^{
                     self.scrimView.alpha = 1.0f;
                   }];
}

// Hide scrim and restore toolbar.
- (void)hideScrim {
  [UIView animateWithDuration:kTableViewNavigationScrimFadeDuration
      animations:^{
        self.scrimView.alpha = 0.0f;
      }
      completion:^(BOOL finished) {
        [self.scrimView removeFromSuperview];
      }];
  [self setupContextBar];
}

#pragma mark - Loading and Empty States

// Shows loading spinner background view.
- (void)showLoadingSpinnerBackground {
  if (!self.spinnerView) {
    self.spinnerView = [[BookmarkHomeWaitingView alloc]
          initWithFrame:self.sharedState.tableView.bounds
        backgroundColor:[UIColor clearColor]];
    [self.spinnerView startWaiting];
  }
  self.tableView.backgroundView = self.spinnerView;
}

// Hide the loading spinner if it is showing.
- (void)hideLoadingSpinnerBackground {
  if (self.spinnerView) {
    [self.spinnerView stopWaitingWithCompletion:^{
      [UIView animateWithDuration:0.2
          animations:^{
            self.spinnerView.alpha = 0.0;
          }
          completion:^(BOOL finished) {
            self.sharedState.tableView.backgroundView = nil;
            self.spinnerView = nil;
          }];
    }];
  }
}

// Shows empty bookmarks background view.
- (void)showEmptyBackground {
  if (!self.emptyTableBackgroundView) {
    // Set up the background view shown when the table is empty.
    self.emptyTableBackgroundView = [[BookmarkEmptyBackground alloc]
        initWithFrame:self.sharedState.tableView.bounds];
    self.emptyTableBackgroundView.autoresizingMask =
        UIViewAutoresizingFlexibleHeight | UIViewAutoresizingFlexibleWidth;
    self.emptyTableBackgroundView.text =
        l10n_util::GetNSString(IDS_IOS_BOOKMARK_NO_BOOKMARKS_LABEL);
    self.emptyTableBackgroundView.frame = self.sharedState.tableView.bounds;
  }
  self.sharedState.tableView.backgroundView = self.emptyTableBackgroundView;
}

- (void)hideEmptyBackground {
  self.sharedState.tableView.backgroundView = nil;
}

#pragma mark - ContextBarDelegate implementation

// Called when the leading button is clicked.
- (void)leadingButtonClicked {
  // Ignore the button tap if any of our controllers is presenting.
  if ([self isAnyControllerPresenting]) {
    return;
  }
  const std::set<const bookmarks::BookmarkNode*> nodes =
      self.sharedState.editNodes;
  switch (self.contextBarState) {
    case BookmarksContextBarDefault:
      // New Folder clicked.
      [self addNewFolder];
      break;
    case BookmarksContextBarBeginSelection:
      // This must never happen, as the leading button is disabled at this
      // point.
      NOTREACHED();
      break;
    case BookmarksContextBarSingleURLSelection:
    case BookmarksContextBarMultipleURLSelection:
    case BookmarksContextBarSingleFolderSelection:
    case BookmarksContextBarMultipleFolderSelection:
    case BookmarksContextBarMixedSelection:
      // Delete clicked.
      [self deleteNodes:nodes];
      base::RecordAction(
          base::UserMetricsAction("MobileBookmarkManagerRemoveSelected"));
      break;
    case BookmarksContextBarNone:
    default:
      NOTREACHED();
  }
}

// Called when the center button is clicked.
- (void)centerButtonClicked {
  // Ignore the button tap if any of our controller is presenting.
  if ([self isAnyControllerPresenting]) {
    return;
  }
  const std::set<const bookmarks::BookmarkNode*> nodes =
      self.sharedState.editNodes;
  // Center button is shown and is clickable only when at least
  // one node is selected.
  DCHECK(nodes.size() > 0);

  if (experimental_flags::IsBookmarksUIRebootEnabled()) {
    self.actionSheetCoordinator = [[ActionSheetCoordinator alloc]
        initWithBaseViewController:self
                             title:nil
                           message:nil
                     barButtonItem:self.moreButton];
  } else {
    self.actionSheetCoordinator = [[LegacyBookmarksActionSheetCoordinator alloc]
        initWithBaseViewController:self
                             title:nil
                           message:nil];
  }

  switch (self.contextBarState) {
    case BookmarksContextBarSingleURLSelection:
      [self configureCoordinator:self.actionSheetCoordinator
            forSingleBookmarkURL:*(nodes.begin())];
      break;
    case BookmarksContextBarMultipleURLSelection:
      [self configureCoordinator:self.actionSheetCoordinator
          forMultipleBookmarkURLs:nodes];
      break;
    case BookmarksContextBarSingleFolderSelection:
      [self configureCoordinator:self.actionSheetCoordinator
          forSingleBookmarkFolder:*(nodes.begin())];
      break;
    case BookmarksContextBarMultipleFolderSelection:
    case BookmarksContextBarMixedSelection:
      [self configureCoordinator:self.actionSheetCoordinator
          forMixedAndMultiFolderSelection:nodes];
      break;
    case BookmarksContextBarDefault:
    case BookmarksContextBarBeginSelection:
    case BookmarksContextBarNone:
      // Center button is disabled in these states.
      NOTREACHED();
      break;
  }

  [self.actionSheetCoordinator start];
}

// Called when the trailing button, "Select" or "Cancel" is clicked.
- (void)trailingButtonClicked {
  // Ignore the button tap if any of our controller is presenting.
  if ([self isAnyControllerPresenting]) {
    return;
  }
  // Toggle edit mode.
  [self setTableViewEditing:!self.sharedState.currentlyInEditMode];
}

#pragma mark - ContextBarStates

// Customizes the context bar buttons based the |state| passed in.
- (void)setContextBarState:(BookmarksContextBarState)state {
  _contextBarState = state;
  switch (state) {
    case BookmarksContextBarDefault:
      [self setBookmarksContextBarButtonsDefaultState];
      break;
    case BookmarksContextBarBeginSelection:
      [self setBookmarksContextBarSelectionStartState];
      break;
    case BookmarksContextBarSingleURLSelection:
    case BookmarksContextBarMultipleURLSelection:
    case BookmarksContextBarMultipleFolderSelection:
    case BookmarksContextBarMixedSelection:
    case BookmarksContextBarSingleFolderSelection:
      // Reset to start state, and then override with customizations that apply.
      [self setBookmarksContextBarSelectionStartState];
      self.moreButton.enabled = YES;
      self.deleteButton.enabled = YES;
      break;
    case BookmarksContextBarNone:
    default:
      break;
  }
}

- (void)setBookmarksContextBarButtonsDefaultState {
  // Set New Folder button
  NSString* titleString =
      l10n_util::GetNSString(IDS_IOS_BOOKMARK_CONTEXT_BAR_NEW_FOLDER);
  UIBarButtonItem* newFolderButton =
      [[UIBarButtonItem alloc] initWithTitle:titleString
                                       style:UIBarButtonItemStylePlain
                                      target:self
                                      action:@selector(leadingButtonClicked)];
  newFolderButton.accessibilityIdentifier =
      kBookmarkHomeLeadingButtonIdentifier;
  newFolderButton.enabled = [self allowsNewFolder];

  // Spacer button.
  UIBarButtonItem* spaceButton = [[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace
                           target:nil
                           action:nil];

  // Set Edit button.
  titleString = l10n_util::GetNSString(IDS_IOS_BOOKMARK_CONTEXT_BAR_EDIT);
  UIBarButtonItem* editButton =
      [[UIBarButtonItem alloc] initWithTitle:titleString
                                       style:UIBarButtonItemStylePlain
                                      target:self
                                      action:@selector(trailingButtonClicked)];
  editButton.accessibilityIdentifier = kBookmarkHomeTrailingButtonIdentifier;
  editButton.enabled = [self hasBookmarksOrFolders];

  [self setToolbarItems:@[ newFolderButton, spaceButton, editButton ]
               animated:NO];
}

- (void)setBookmarksContextBarSelectionStartState {
  // Disabled Delete button.
  NSString* titleString =
      l10n_util::GetNSString(IDS_IOS_BOOKMARK_CONTEXT_BAR_DELETE);
  self.deleteButton =
      [[UIBarButtonItem alloc] initWithTitle:titleString
                                       style:UIBarButtonItemStylePlain
                                      target:self
                                      action:@selector(leadingButtonClicked)];
  self.deleteButton.tintColor = [UIColor redColor];
  self.deleteButton.enabled = NO;
  self.deleteButton.accessibilityIdentifier =
      kBookmarkHomeLeadingButtonIdentifier;

  // Disabled More button.
  titleString = l10n_util::GetNSString(IDS_IOS_BOOKMARK_CONTEXT_BAR_MORE);
  self.moreButton =
      [[UIBarButtonItem alloc] initWithTitle:titleString
                                       style:UIBarButtonItemStylePlain
                                      target:self
                                      action:@selector(centerButtonClicked)];
  self.moreButton.enabled = NO;
  self.moreButton.accessibilityIdentifier = kBookmarkHomeCenterButtonIdentifier;

  // Enabled Cancel button.
  titleString = l10n_util::GetNSString(IDS_CANCEL);
  UIBarButtonItem* cancelButton =
      [[UIBarButtonItem alloc] initWithTitle:titleString
                                       style:UIBarButtonItemStylePlain
                                      target:self
                                      action:@selector(trailingButtonClicked)];
  cancelButton.accessibilityIdentifier = kBookmarkHomeTrailingButtonIdentifier;

  // Spacer button.
  UIBarButtonItem* spaceButton = [[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace
                           target:nil
                           action:nil];

  [self setToolbarItems:@[
    self.deleteButton, spaceButton, self.moreButton, spaceButton, cancelButton
  ]
               animated:NO];
}

#pragma mark - Context Menu

- (void)configureCoordinator:(AlertCoordinator*)coordinator
     forMultipleBookmarkURLs:(const std::set<const BookmarkNode*>)nodes {
  __weak BookmarkHomeViewController* weakSelf = self;
  coordinator.alertController.view.accessibilityIdentifier =
      @"bookmark_context_menu";

  [coordinator
      addItemWithTitle:l10n_util::GetNSString(
                           IDS_IOS_BOOKMARK_CONTEXT_MENU_OPEN)
                action:^{
                  std::vector<const BookmarkNode*> nodes =
                      [weakSelf getEditNodesInVector];
                  [weakSelf openAllNodes:nodes inIncognito:NO newTab:NO];
                }
                 style:UIAlertActionStyleDefault];

  [coordinator
      addItemWithTitle:l10n_util::GetNSString(
                           IDS_IOS_BOOKMARK_CONTEXT_MENU_OPEN_INCOGNITO)
                action:^{
                  std::vector<const BookmarkNode*> nodes =
                      [weakSelf getEditNodesInVector];
                  [weakSelf openAllNodes:nodes inIncognito:YES newTab:NO];
                }
                 style:UIAlertActionStyleDefault];

  [coordinator addItemWithTitle:l10n_util::GetNSString(
                                    IDS_IOS_BOOKMARK_CONTEXT_MENU_MOVE)
                         action:^{
                           [weakSelf moveNodes:nodes];
                         }
                          style:UIAlertActionStyleDefault];

  [coordinator addItemWithTitle:l10n_util::GetNSString(IDS_CANCEL)
                         action:nil
                          style:UIAlertActionStyleCancel];
}

- (void)configureCoordinator:(AlertCoordinator*)coordinator
        forSingleBookmarkURL:(const BookmarkNode*)node {
  __weak BookmarkHomeViewController* weakSelf = self;
  std::string urlString = node->url().possibly_invalid_spec();
  coordinator.alertController.view.accessibilityIdentifier =
      @"bookmark_context_menu";

  [coordinator addItemWithTitle:l10n_util::GetNSString(
                                    IDS_IOS_BOOKMARK_CONTEXT_MENU_EDIT)
                         action:^{
                           [weakSelf editNode:node];
                         }
                          style:UIAlertActionStyleDefault];

  [coordinator
      addItemWithTitle:l10n_util::GetNSString(
                           IDS_IOS_CONTENT_CONTEXT_OPENLINKNEWTAB)
                action:^{
                  std::vector<const BookmarkNode*> nodes = {node};
                  [weakSelf openAllNodes:nodes inIncognito:NO newTab:YES];
                }
                 style:UIAlertActionStyleDefault];

  [coordinator
      addItemWithTitle:l10n_util::GetNSString(
                           IDS_IOS_CONTENT_CONTEXT_OPENLINKNEWINCOGNITOTAB)
                action:^{
                  std::vector<const BookmarkNode*> nodes = {node};
                  [weakSelf openAllNodes:nodes inIncognito:YES newTab:YES];
                }
                 style:UIAlertActionStyleDefault];

  [coordinator
      addItemWithTitle:l10n_util::GetNSString(IDS_IOS_CONTENT_CONTEXT_COPY)
                action:^{
                  UIPasteboard* pasteboard = [UIPasteboard generalPasteboard];
                  pasteboard.string = base::SysUTF8ToNSString(urlString);
                  [weakSelf setTableViewEditing:NO];
                }
                 style:UIAlertActionStyleDefault];

  [coordinator addItemWithTitle:l10n_util::GetNSString(IDS_CANCEL)
                         action:nil
                          style:UIAlertActionStyleCancel];
}

- (void)configureCoordinator:(AlertCoordinator*)coordinator
     forSingleBookmarkFolder:(const BookmarkNode*)node {
  __weak BookmarkHomeViewController* weakSelf = self;
  coordinator.alertController.view.accessibilityIdentifier =
      @"bookmark_context_menu";

  [coordinator addItemWithTitle:l10n_util::GetNSString(
                                    IDS_IOS_BOOKMARK_CONTEXT_MENU_EDIT_FOLDER)
                         action:^{
                           [weakSelf editNode:node];
                         }
                          style:UIAlertActionStyleDefault];

  [coordinator addItemWithTitle:l10n_util::GetNSString(
                                    IDS_IOS_BOOKMARK_CONTEXT_MENU_MOVE)
                         action:^{
                           std::set<const BookmarkNode*> nodes;
                           nodes.insert(node);
                           [weakSelf moveNodes:nodes];
                         }
                          style:UIAlertActionStyleDefault];

  [coordinator addItemWithTitle:l10n_util::GetNSString(IDS_CANCEL)
                         action:nil
                          style:UIAlertActionStyleCancel];
}

- (void)configureCoordinator:(AlertCoordinator*)coordinator
    forMixedAndMultiFolderSelection:
        (const std::set<const bookmarks::BookmarkNode*>)nodes {
  __weak BookmarkHomeViewController* weakSelf = self;
  coordinator.alertController.view.accessibilityIdentifier =
      @"bookmark_context_menu";

  [coordinator addItemWithTitle:l10n_util::GetNSString(
                                    IDS_IOS_BOOKMARK_CONTEXT_MENU_MOVE)
                         action:^{
                           [weakSelf moveNodes:nodes];
                         }
                          style:UIAlertActionStyleDefault];

  [coordinator addItemWithTitle:l10n_util::GetNSString(IDS_CANCEL)
                         action:nil
                          style:UIAlertActionStyleCancel];
}

#pragma mark - Favicon Handling

- (void)updateCellAtIndexPath:(NSIndexPath*)indexPath
                    withImage:(UIImage*)image
              backgroundColor:(UIColor*)backgroundColor
                    textColor:(UIColor*)textColor
                 fallbackText:(NSString*)fallbackText {
  if (experimental_flags::IsBookmarksUIRebootEnabled()) {
    TableViewURLCell* URLCell = base::mac::ObjCCastStrict<TableViewURLCell>(
        [self.sharedState.tableView cellForRowAtIndexPath:indexPath]);
    if (!URLCell)
      return;
    if (image)
      [URLCell.faviconView
          configureWithAttributes:[FaviconAttributes
                                      attributesWithImage:image]];
    else {
      // UI Refresh fallback colors are default fixed.
      textColor =
          [UIColor colorWithWhite:kFallbackIconDefaultTextColorWhitePercentage
                            alpha:1.0];
      backgroundColor = UIColorFromRGB(kFallbackIconDefaultBackgroundColor);
      [URLCell.faviconView
          configureWithAttributes:[FaviconAttributes
                                      attributesWithMonogram:fallbackText
                                                   textColor:textColor
                                             backgroundColor:backgroundColor
                                      defaultBackgroundColor:NO]];
    }
  } else {
    BookmarkTableCell* cell =
        [self.sharedState.tableView cellForRowAtIndexPath:indexPath];
    if (!cell)
      return;
    if (image) {
      [cell setImage:image];
    } else {
      [cell setPlaceholderText:fallbackText
                     textColor:textColor
               backgroundColor:backgroundColor];
    }
  }
}

- (void)updateCellAtIndexPath:(NSIndexPath*)indexPath
          withLargeIconResult:(const favicon_base::LargeIconResult&)result
                 fallbackText:(NSString*)fallbackText {
  UIImage* favIcon = nil;
  UIColor* backgroundColor = nil;
  UIColor* textColor = nil;

  if (result.bitmap.is_valid()) {
    scoped_refptr<base::RefCountedMemory> data = result.bitmap.bitmap_data;
    favIcon = [UIImage
        imageWithData:[NSData dataWithBytes:data->front() length:data->size()]];
    fallbackText = nil;
    // Update the time when the icon was last requested - postpone thus the
    // automatic eviction of the favicon from the favicon database.
    IOSChromeLargeIconServiceFactory::GetForBrowserState(self.browserState)
        ->TouchIconFromGoogleServer(result.bitmap.icon_url);
  } else if (result.fallback_icon_style) {
    backgroundColor =
        skia::UIColorFromSkColor(result.fallback_icon_style->background_color);
    textColor =
        skia::UIColorFromSkColor(result.fallback_icon_style->text_color);
  }

  [self updateCellAtIndexPath:indexPath
                    withImage:favIcon
              backgroundColor:backgroundColor
                    textColor:textColor
                 fallbackText:fallbackText];
}

// Cancels all async loads of favicons. Subclasses should call this method when
// the bookmark model is going through significant changes, then manually call
// loadFaviconAtIndexPath: for everything that needs to be loaded; or
// just reload relevant cells.
- (void)cancelAllFaviconLoads {
  _faviconTaskTracker.TryCancelAll();
}

- (void)cancelLoadingFaviconAtIndexPath:(NSIndexPath*)indexPath {
  _faviconTaskTracker.TryCancel(
      _faviconLoadTasks[IntegerPair(indexPath.section, indexPath.item)]);
}

#pragma mark - UIGestureRecognizerDelegate and gesture handling

- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer*)gestureRecognizer {
  if (gestureRecognizer ==
      self.navigationController.interactivePopGestureRecognizer) {
    return self.navigationController.viewControllers.count > 1;
  }
  return YES;
}

- (BOOL)gestureRecognizer:(UIGestureRecognizer*)gestureRecognizer
       shouldReceiveTouch:(UITouch*)touch {
  // Ignore long press in edit mode.
  if (self.sharedState.currentlyInEditMode) {
    return NO;
  }
  return YES;
}

- (void)handleLongPress:(UILongPressGestureRecognizer*)gestureRecognizer {
  if (self.sharedState.currentlyInEditMode ||
      gestureRecognizer.state != UIGestureRecognizerStateBegan) {
    return;
  }
  CGPoint touchPoint =
      [gestureRecognizer locationInView:self.sharedState.tableView];
  NSIndexPath* indexPath =
      [self.sharedState.tableView indexPathForRowAtPoint:touchPoint];
  if (indexPath == nil || [self.sharedState.tableViewModel
                              sectionIdentifierForSection:indexPath.section] !=
                              BookmarkHomeSectionIdentifierBookmarks) {
    return;
  }

  const BookmarkNode* node = [self nodeAtIndexPath:indexPath];
  // Disable the long press gesture if it is a permanent node (not an URL or
  // Folder).
  if (!node || ![self isUrlOrFolder:node]) {
    return;
  }

  if (experimental_flags::IsBookmarksUIRebootEnabled()) {
    self.actionSheetCoordinator = [[ActionSheetCoordinator alloc]
        initWithBaseViewController:self
                             title:nil
                           message:nil
                              rect:CGRectMake(touchPoint.x, touchPoint.y, 1, 1)
                              view:self.tableView];
  } else {
    self.actionSheetCoordinator = [[LegacyBookmarksActionSheetCoordinator alloc]
        initWithBaseViewController:self
                             title:nil
                           message:nil];
  }

  if (node->is_url()) {
    [self configureCoordinator:self.actionSheetCoordinator
          forSingleBookmarkURL:node];
  } else if (node->is_folder()) {
    [self configureCoordinator:self.actionSheetCoordinator
        forSingleBookmarkFolder:node];
  } else {
    NOTREACHED();
    return;
  }

  [self.actionSheetCoordinator start];
}

#pragma mark UISearchResultsUpdating

- (void)updateSearchResultsForSearchController:
    (UISearchController*)searchController {
  DCHECK_EQ(self.searchController, searchController);
  NSString* text = searchController.searchBar.text;
  self.searchTerm = text;

  if (text.length == 0) {
    if (self.sharedState.currentlyShowingSearchResults) {
      self.sharedState.currentlyShowingSearchResults = NO;
      // Restore current list.
      [self.mediator computeBookmarkTableViewData];
      [self.sharedState.tableView reloadData];
      [self showScrim];
    }
  } else {
    if (!self.sharedState.currentlyShowingSearchResults) {
      self.sharedState.currentlyShowingSearchResults = YES;
      [self hideScrim];
    }
    // Replace current list with search result, but doesn't change
    // the 'regular' model for this page, which we can restore when search
    // is terminated.
    NSString* noResults = l10n_util::GetNSString(IDS_HISTORY_NO_SEARCH_RESULTS);
    [self.mediator computeBookmarkTableViewDataMatching:text
                             orShowMessageWhenNoResults:noResults];
    [self.sharedState.tableView reloadData];
  }
}

#pragma mark UISearchControllerDelegate

- (void)willPresentSearchController:(UISearchController*)searchController {
  [self showScrim];
}

- (void)willDismissSearchController:(UISearchController*)searchController {
  // Avoid scrim being put back on in updateSearchResultsForSearchController.
  self.sharedState.currentlyShowingSearchResults = NO;
  // Restore current list.
  [self.mediator computeBookmarkTableViewData];
  [self.sharedState.tableView reloadData];
}

- (void)didDismissSearchController:(UISearchController*)searchController {
  [self hideScrim];
}

#pragma mark - BookmarkHomeSharedStateObserver

- (void)sharedStateDidClearEditNodes:(BookmarkHomeSharedState*)sharedState {
  [self handleSelectEditNodes:sharedState.editNodes];
}

#pragma mark - UITableViewDataSource

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  UITableViewCell* cell =
      [super tableView:tableView cellForRowAtIndexPath:indexPath];
  TableViewItem* item =
      [self.sharedState.tableViewModel itemAtIndexPath:indexPath];

  if (item.type == BookmarkHomeItemTypeBookmark) {
    BookmarkHomeNodeItem* nodeItem =
        base::mac::ObjCCastStrict<BookmarkHomeNodeItem>(item);
    if (experimental_flags::IsBookmarksUIRebootEnabled()) {
      if (nodeItem.bookmarkNode->is_folder() &&
          nodeItem.bookmarkNode == self.sharedState.editingFolderNode) {
        TableViewBookmarkFolderCell* tableCell =
            base::mac::ObjCCastStrict<TableViewBookmarkFolderCell>(cell);
        // Delay starting edit, so that the cell is fully created. This is
        // needed when scrolling away and then back into the editingCell,
        // without the delay the cell will resign first responder before its
        // created.
        dispatch_async(dispatch_get_main_queue(), ^{
          self.sharedState.editingFolderCell = tableCell;
          [tableCell startEdit];
          tableCell.textDelegate = self;
        });
      }
    } else {
      BookmarkTableCell* tableCell =
          base::mac::ObjCCastStrict<BookmarkTableCell>(cell);
      if (nodeItem.bookmarkNode == self.sharedState.editingFolderNode) {
        // Delay starting edit, so that the cell is fully created.
        dispatch_async(dispatch_get_main_queue(), ^{
          self.sharedState.editingFolderCell = tableCell;
          [tableCell startEdit];
          tableCell.textDelegate = self;
        });
      }
    }

    // Cancel previous load attempts.
    [self cancelLoadingFaviconAtIndexPath:indexPath];
    // Load the favicon from cache.  If not found, try fetching it from a
    // Google Server.
    [self loadFaviconAtIndexPath:indexPath continueToGoogleServer:YES];
  }

  return cell;
}

- (BOOL)tableView:(UITableView*)tableView
    canEditRowAtIndexPath:(NSIndexPath*)indexPath {
  // Filtered results are always a URL and editable.
  if (self.sharedState.currentlyShowingSearchResults) {
    return YES;
  }
  TableViewItem* item =
      [self.sharedState.tableViewModel itemAtIndexPath:indexPath];
  if (item.type != BookmarkHomeItemTypeBookmark) {
    // Can only edit bookmarks.
    return NO;
  }

  // If the cell at |indexPath| is being edited (which happens when creating a
  // new Folder) return NO.
  if ([tableView indexPathForCell:self.sharedState.editingFolderCell] ==
      indexPath) {
    return NO;
  }

  // Enable the swipe-to-delete gesture and reordering control for nodes of
  // type URL or Folder, but not the permanent ones.
  BookmarkHomeNodeItem* nodeItem =
      base::mac::ObjCCastStrict<BookmarkHomeNodeItem>(item);
  const BookmarkNode* node = nodeItem.bookmarkNode;
  return [self isUrlOrFolder:node];
}

- (void)tableView:(UITableView*)tableView
    commitEditingStyle:(UITableViewCellEditingStyle)editingStyle
     forRowAtIndexPath:(NSIndexPath*)indexPath {
  TableViewItem* item =
      [self.sharedState.tableViewModel itemAtIndexPath:indexPath];
  if (item.type != BookmarkHomeItemTypeBookmark) {
    // Can only commit edits for bookmarks.
    return;
  }

  if (editingStyle == UITableViewCellEditingStyleDelete) {
    BookmarkHomeNodeItem* nodeItem =
        base::mac::ObjCCastStrict<BookmarkHomeNodeItem>(item);
    const BookmarkNode* node = nodeItem.bookmarkNode;
    std::set<const BookmarkNode*> nodes;
    nodes.insert(node);
    [self handleSelectNodesForDeletion:nodes];
    base::RecordAction(
        base::UserMetricsAction("MobileBookmarkManagerEntryDeleted"));
  }
}

- (BOOL)tableView:(UITableView*)tableView
    canMoveRowAtIndexPath:(NSIndexPath*)indexPath {
  // No reorering with filtered results.
  if (self.sharedState.currentlyShowingSearchResults) {
    return NO;
  }
  TableViewItem* item =
      [self.sharedState.tableViewModel itemAtIndexPath:indexPath];
  if (item.type != BookmarkHomeItemTypeBookmark) {
    // Can only move bookmarks.
    return NO;
  }

  return YES;
}

- (void)tableView:(UITableView*)tableView
    moveRowAtIndexPath:(NSIndexPath*)sourceIndexPath
           toIndexPath:(NSIndexPath*)destinationIndexPath {
  if (sourceIndexPath.row == destinationIndexPath.row ||
      self.sharedState.currentlyShowingSearchResults) {
    return;
  }
  const BookmarkNode* node = [self nodeAtIndexPath:sourceIndexPath];
  // Calculations: Assume we have 3 nodes A B C. Node positions are A(0), B(1),
  // C(2) respectively. When we move A to after C, we are moving node at index 0
  // to 3 (position after C is 3, in terms of the existing contents). Hence add
  // 1 when moving forward. When moving backward, if C(2) is moved to Before B,
  // we move node at index 2 to index 1 (position before B is 1, in terms of the
  // existing contents), hence no change in index is necessary. It is required
  // to make these adjustments because this is how bookmark_model handles move
  // operations.
  int newPosition = sourceIndexPath.row < destinationIndexPath.row
                        ? destinationIndexPath.row + 1
                        : destinationIndexPath.row;
  [self handleMoveNode:node toPosition:newPosition];
}

#pragma mark - UITableViewDelegate

- (CGFloat)tableView:(UITableView*)tableView
    heightForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (!experimental_flags::IsBookmarksUIRebootEnabled()) {
    NSInteger sectionIdentifier = [self.sharedState.tableViewModel
        sectionIdentifierForSection:indexPath.section];
    if (sectionIdentifier == BookmarkHomeSectionIdentifierBookmarks) {
      return kEstimatedRowHeight;
    }
  }
  return UITableViewAutomaticDimension;
}

- (void)tableView:(UITableView*)tableView
    didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  NSInteger sectionIdentifier = [self.sharedState.tableViewModel
      sectionIdentifierForSection:indexPath.section];
  if (sectionIdentifier == BookmarkHomeSectionIdentifierBookmarks) {
    const BookmarkNode* node = [self nodeAtIndexPath:indexPath];
    DCHECK(node);
    // If table is in edit mode, record all the nodes added to edit set.
    if (self.sharedState.currentlyInEditMode) {
      self.sharedState.editNodes.insert(node);
      [self handleSelectEditNodes:self.sharedState.editNodes];
      return;
    }
    [self.sharedState.editingFolderCell stopEdit];
    if (node->is_folder()) {
      [self handleSelectFolderForNavigation:node];
    } else {
      // Open URL. Pass this to the delegate.
      [self handleSelectUrlForNavigation:node->url()];
    }
  }
  // Deselect row.
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
}

- (void)tableView:(UITableView*)tableView
    didDeselectRowAtIndexPath:(NSIndexPath*)indexPath {
  NSInteger sectionIdentifier = [self.sharedState.tableViewModel
      sectionIdentifierForSection:indexPath.section];
  if (sectionIdentifier == BookmarkHomeSectionIdentifierBookmarks &&
      self.sharedState.currentlyInEditMode) {
    const BookmarkNode* node = [self nodeAtIndexPath:indexPath];
    DCHECK(node);
    self.sharedState.editNodes.erase(node);
    [self handleSelectEditNodes:self.sharedState.editNodes];
  }
}

@end
