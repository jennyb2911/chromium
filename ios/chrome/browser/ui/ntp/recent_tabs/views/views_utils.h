// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IOS_CHROME_BROWSER_UI_NTP_RECENT_TABS_VIEWS_VIEWS_UTILS_H_
#define IOS_CHROME_BROWSER_UI_NTP_RECENT_TABS_VIEWS_VIEWS_UTILS_H_

#import <UIKit/UIKit.h>

class GURL;
@class FaviconAttributes;
@class UIImage;

namespace ios {
class ChromeBrowserState;
}  // namespace ios

typedef void (^FaviconGetterCompletionBlock)(FaviconAttributes*);

namespace recent_tabs {

// Returns an autoreleased UILabel.
UILabel* CreateMultilineLabel(NSString* text);

// Color helpers.
UIColor* GetTextColorBlue();
UIColor* GetTextColorGray();
UIColor* GetSubtitleColorBlue();
UIColor* GetSubtitleColorGray();
UIColor* GetIconColorBlue();
UIColor* GetIconColorGray();

// Gets the favicon for |url|, calls |block| when loaded.
void GetFavicon(GURL const& url,
                ios::ChromeBrowserState* browserState,
                FaviconGetterCompletionBlock block);
}  // namespace recent_tabs

#endif  // IOS_CHROME_BROWSER_UI_NTP_RECENT_TABS_VIEWS_VIEWS_UTILS_H_
