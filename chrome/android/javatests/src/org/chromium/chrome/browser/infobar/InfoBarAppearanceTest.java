// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.infobar;

import static junit.framework.Assert.assertEquals;

import android.support.test.filters.MediumTest;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

import org.chromium.base.ThreadUtils;
import org.chromium.base.test.util.CallbackHelper;
import org.chromium.base.test.util.CommandLineFlags;
import org.chromium.base.test.util.Feature;
import org.chromium.chrome.browser.ChromeActivity;
import org.chromium.chrome.browser.ChromeSwitches;
import org.chromium.chrome.browser.tab.EmptyTabObserver;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.test.ScreenShooter;
import org.chromium.chrome.test.ChromeActivityTestRule;
import org.chromium.chrome.test.ChromeJUnit4ClassRunner;
import org.chromium.chrome.test.util.InfoBarTestAnimationListener;
import org.chromium.content_public.browser.test.util.CriteriaHelper;

import java.util.List;
import java.util.concurrent.TimeoutException;

/**
 * Tests for the appearance of InfoBars.
 */
// clang-format off
@RunWith(ChromeJUnit4ClassRunner.class)
@CommandLineFlags.Add({ChromeSwitches.DISABLE_FIRST_RUN_EXPERIENCE})
public class InfoBarAppearanceTest {
    // clang-format on

    @Rule
    public ChromeActivityTestRule<ChromeActivity> mActivityTestRule =
            new ChromeActivityTestRule<>(ChromeActivity.class);

    @Rule
    public ScreenShooter mScreenShooter = new ScreenShooter();

    private InfoBarTestAnimationListener mListener;
    private Tab mTab;

    @Before
    public void setUp() throws InterruptedException {
        mActivityTestRule.startMainActivityOnBlankPage();

        mListener = new InfoBarTestAnimationListener();

        mTab = mActivityTestRule.getActivity().getActivityTab();
        mTab.getInfoBarContainer().addAnimationListener(mListener);
    }

    @Test
    @MediumTest
    @Feature({"InfoBars", "UiCatalogue"})
    public void testFramebustBlockInfoBar() throws Exception {
        FramebustBlockInfoBar infobar = new FramebustBlockInfoBar("http://very.evil.biz");
        captureMiniAndRegularInfobar(infobar);
    }

    @Test
    @MediumTest
    @Feature("InfoBars")
    public void testFramebustBlockInfoBarOverriding() {
        String url1 = "http://very.evil.biz/";
        String url2 = "http://other.evil.biz/";
        List<InfoBar> infobars;
        FramebustBlockInfoBar infoBar;

        ThreadUtils.runOnUiThreadBlocking(() -> {
            mTab.getTabWebContentsDelegateAndroid().showFramebustBlockInfobarForTesting(url1);
        });
        infobars = mTab.getInfoBarContainer().getInfoBarsForTesting();
        assertEquals(1, infobars.size());
        infoBar = (FramebustBlockInfoBar) infobars.get(0);
        assertEquals(url1, infoBar.getBlockedUrl());

        ThreadUtils.runOnUiThreadBlocking(() -> {
            mTab.getTabWebContentsDelegateAndroid().showFramebustBlockInfobarForTesting(url2);
        });
        infobars = mTab.getInfoBarContainer().getInfoBarsForTesting();
        assertEquals(1, infobars.size());
        infoBar = (FramebustBlockInfoBar) infobars.get(0);
        assertEquals(url2, infoBar.getBlockedUrl());
    }

    @Test
    @MediumTest
    @Feature("InfoBars")
    public void testFramebustBlockInfoBarUrlTapped() throws TimeoutException, InterruptedException {
        String url = "http://very.evil.biz";

        CallbackHelper callbackHelper = new CallbackHelper();
        EmptyTabObserver navigationWaiter = new EmptyTabObserver() {
            @Override
            public void onDidStartNavigation(Tab tab, String url, boolean isInMainFrame,
                    boolean isSameDocument, boolean isErrorPage) {
                callbackHelper.notifyCalled();
            }
        };
        mTab.addObserver(navigationWaiter);

        ThreadUtils.runOnUiThreadBlocking(() -> {
            mTab.getTabWebContentsDelegateAndroid().showFramebustBlockInfobarForTesting(url);
        });
        FramebustBlockInfoBar infoBar =
                (FramebustBlockInfoBar) mTab.getInfoBarContainer().getInfoBarsForTesting().get(0);

        ThreadUtils.runOnUiThreadBlocking(infoBar::onLinkClicked); // Once to expand the infobar
        assertEquals(0, callbackHelper.getCallCount());

        ThreadUtils.runOnUiThreadBlocking(infoBar::onLinkClicked); // Now to navigate
        callbackHelper.waitForCallback(0);

        CriteriaHelper.pollUiThread(
                () -> mTab.getInfoBarContainer().getInfoBarsForTesting().isEmpty());
    }

    @Test
    @MediumTest
    @Feature("InfoBars")
    public void testFramebustBlockInfoBarButtonTapped()
            throws TimeoutException, InterruptedException {
        String url = "http://very.evil.biz";

        ThreadUtils.runOnUiThreadBlocking(() -> {
            mTab.getTabWebContentsDelegateAndroid().showFramebustBlockInfobarForTesting(url);
        });
        FramebustBlockInfoBar infoBar =
                (FramebustBlockInfoBar) mTab.getInfoBarContainer().getInfoBarsForTesting().get(0);

        ThreadUtils.runOnUiThreadBlocking(() -> infoBar.onButtonClicked(true));
        CriteriaHelper.pollUiThread(
                () -> mTab.getInfoBarContainer().getInfoBarsForTesting().isEmpty());
    }

    @Test
    @MediumTest
    @Feature({"InfoBars", "UiCatalogue"})
    public void testFramebustBlockInfoBarWithLongMessages() throws Exception {
        FramebustBlockInfoBar infobar = new FramebustBlockInfoBar("https://someverylonglink"
                + "thatwilldefinitelynotfitevenwhenremovingthefilepath.com/somemorestuff");
        captureMiniAndRegularInfobar(infobar);
    }

    @Test
    @MediumTest
    @Feature({"InfoBars", "UiCatalogue"})
    public void testOomInfoBar() throws TimeoutException, InterruptedException {
        ThreadUtils.runOnUiThreadBlocking(
                () -> mTab.getInfoBarContainer().addInfoBarForTesting(new NearOomInfoBar()));
        mListener.addInfoBarAnimationFinished("InfoBar was not added.");
        mScreenShooter.shoot("oom_infobar");
    }

    private void captureMiniAndRegularInfobar(InfoBar infobar)
            throws TimeoutException, InterruptedException {
        ThreadUtils.runOnUiThreadBlocking(
                () -> mTab.getInfoBarContainer().addInfoBarForTesting(infobar));
        mListener.addInfoBarAnimationFinished("InfoBar was not added.");
        mScreenShooter.shoot("compact");

        ThreadUtils.runOnUiThreadBlocking(infobar::onLinkClicked);
        mListener.swapInfoBarAnimationFinished("InfoBar did not expand.");
        mScreenShooter.shoot("expanded");
    }
}
