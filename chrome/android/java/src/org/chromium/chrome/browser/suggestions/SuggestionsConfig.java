// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.suggestions;

import android.content.res.Resources;
import android.support.annotation.IntDef;
import android.text.TextUtils;

import org.chromium.base.ApiCompatibilityUtils;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.ChromeFeatureList;
import org.chromium.chrome.browser.util.AccessibilityUtil;
import org.chromium.chrome.browser.widget.displaystyle.UiConfig;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

/**
 * Provides configuration details for suggestions.
 */
public final class SuggestionsConfig {
    @IntDef({TileStyle.MODERN, TileStyle.MODERN_CONDENSED})
    @Retention(RetentionPolicy.SOURCE)
    public @interface TileStyle {
        int MODERN = 1;
        int MODERN_CONDENSED = 2;
    }

    /**
     * Field trial parameter for referrer URL.
     * It must be kept in sync with //components/ntp_suggestions/features.cc
     */
    private static final String REFERRER_URL_PARAM = "referrer_url";

    /**
     * Default value for referrer URL.
     * It must be kept in sync with //components/ntp_suggestions/features.cc
     */
    private static final String DEFAULT_REFERRER_URL = "https://discover.google.com/";

    private SuggestionsConfig() {}

    /**
     * @return Whether scrolling to the bottom of suggestions triggers a load.
     */
    public static boolean scrollToLoad() {
        // The scroll to load feature does not work well for users who require accessibility mode.
        if (AccessibilityUtil.isAccessibilityEnabled()) return false;

        return ChromeFeatureList.isEnabled(ChromeFeatureList.SIMPLIFIED_NTP)
                && ChromeFeatureList.isEnabled(
                           ChromeFeatureList.CONTENT_SUGGESTIONS_SCROLL_TO_LOAD);
    }

    /**
     * @param resources The resources to fetch the color from.
     * @return The background color for the suggestions sheet content.
     */
    public static int getBackgroundColor(Resources resources) {
        return ApiCompatibilityUtils.getColor(resources, R.color.suggestions_modern_bg);
    }

    /**
     * Returns the current tile style, that depends on the enabled features and the screen size.
     */
    @TileStyle
    public static int getTileStyle(UiConfig uiConfig) {
        return uiConfig.getCurrentDisplayStyle().isSmall() ? TileStyle.MODERN_CONDENSED
                                                           : TileStyle.MODERN;
    }

    private static boolean useCondensedTileLayout(boolean isScreenSmall) {
        if (isScreenSmall) return true;

        return false;
    }

    /**
     * @param featureName The feature from {@link ChromeFeatureList}, which provides the referrer
     *                    URL parameter.
     * @return The value of referrer URL to use with content suggestions.
     */
    public static String getReferrerUrl(String featureName) {
        String referrerParamValue =
                ChromeFeatureList.getFieldTrialParamByFeature(featureName, REFERRER_URL_PARAM);

        if (!TextUtils.isEmpty(referrerParamValue)) {
            return referrerParamValue;
        }

        return DEFAULT_REFERRER_URL;
    }
}
