// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.download.home.list.view;

import android.content.Context;
import android.content.res.TypedArray;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.view.View;

import org.chromium.chrome.download.R;
import org.chromium.ui.widget.RoundedCornerImageView;

/** Helper class that adds foreground drawable support to {@code RoundedCornerImageView}. */
public class ForegroundRoundedCornerImageView extends RoundedCornerImageView {
    private final ForegroundDrawableCompat mForegroundHelper;

    /** Creates an {@link ForegroundRoundedCornerImageView instance. */
    public ForegroundRoundedCornerImageView(Context context) {
        this(context, null, 0);
    }

    /** Creates an {@link ForegroundRoundedCornerImageView instance. */
    public ForegroundRoundedCornerImageView(Context context, AttributeSet attrs) {
        this(context, attrs, 0);
    }

    /** Creates an {@link ForegroundRoundedCornerImageView instance. */
    public ForegroundRoundedCornerImageView(Context context, AttributeSet attrs, int defStyle) {
        super(context, attrs, defStyle);

        mForegroundHelper = new ForegroundDrawableCompat(this);

        TypedArray types = attrs == null
                ? null
                : context.obtainStyledAttributes(
                          attrs, R.styleable.ForegroundRoundedCornerImageView, 0, 0);

        mForegroundHelper.setDrawable(AutoAnimatorDrawable.wrap(UiUtils.getDrawable(
                context, types, R.styleable.ForegroundRoundedCornerImageView_foregroundCompat)));

        if (types != null) types.recycle();
    }

    /** Sets the foreground drawable of this {@link View} to {@code drawable}. */
    public void setForegroundDrawableCompat(Drawable drawable) {
        mForegroundHelper.setDrawable(drawable);
    }

    // RoundedCornerImageView implementation.
    @Override
    public void draw(Canvas canvas) {
        super.draw(canvas);
        mForegroundHelper.draw(canvas);
    }

    @Override
    protected void onVisibilityChanged(View changedView, int visibility) {
        super.onVisibilityChanged(changedView, visibility);
        mForegroundHelper.onVisibilityChanged(changedView, visibility);
    }

    @Override
    protected void drawableStateChanged() {
        super.drawableStateChanged();
        mForegroundHelper.drawableStateChanged();
    }

    @Override
    protected boolean verifyDrawable(Drawable dr) {
        return super.verifyDrawable(dr) || mForegroundHelper.verifyDrawable(dr);
    }
}