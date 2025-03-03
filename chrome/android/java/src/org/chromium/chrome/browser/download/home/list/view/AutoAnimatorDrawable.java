// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.download.home.list.view;

import android.annotation.TargetApi;
import android.graphics.drawable.Animatable;
import android.graphics.drawable.Animatable2;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.InsetDrawable;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.RotateDrawable;
import android.graphics.drawable.ScaleDrawable;
import android.os.Build;
import android.os.Handler;
import android.support.annotation.Nullable;
import android.support.graphics.drawable.Animatable2Compat;
import android.support.v7.graphics.drawable.DrawableWrapper;

/**
 * A helper {@link Drawable} that wraps another {@link Drawable} and starts/stops any
 * {@link Animatable} {@link Drawable}s in the {@link Drawable} hierarchy when this {@link Drawable}
 * is shown or hidden.
 */
public class AutoAnimatorDrawable extends DrawableWrapper {
    // Since Drawables default visible to true by default, we might not get a change and start the
    // animation on the first visibility request.
    private boolean mGotVisibilityCall;

    /**
     * Wraps {@code drawable} and returns a new {@link Drawable} that will automatically start
     * animating all sub-drawables if possible when the {@link Drawable} is visible.  Stops
     * animating when the {@link Drawable} is no longer visible.
     * @param drawable The {@link Drawable} to wrap.
     * @return         A new {@link Drawable} that will automaticaly animate or {@code null} if
     *                 {@code drawable} is {@code null}.
     */
    public static Drawable wrap(@Nullable Drawable drawable) {
        if (drawable == null) return null;
        return new AutoAnimatorDrawable(drawable);
    }

    private AutoAnimatorDrawable(Drawable drawable) {
        super(drawable);
        AutoAnimatorDrawable.attachRestartListeners(this);
    }

    // DrawableWrapper implementation.
    @Override
    public boolean setVisible(boolean visible, boolean restart) {
        boolean changed = super.setVisible(visible, restart);
        if (visible) {
            if (changed || restart || !mGotVisibilityCall) {
                AutoAnimatorDrawable.startAnimatedDrawables(this);
            }
        } else {
            AutoAnimatorDrawable.stopAnimatedDrawables(this);
        }

        mGotVisibilityCall = true;
        return changed;
    }

    private static void startAnimatedDrawables(@Nullable Drawable drawable) {
        AutoAnimatorDrawable.animatedDrawableHelper(drawable, animatable -> animatable.start());
    }

    private static void stopAnimatedDrawables(@Nullable Drawable drawable) {
        AutoAnimatorDrawable.animatedDrawableHelper(drawable, animatable -> animatable.stop());
    }

    @TargetApi(Build.VERSION_CODES.M)
    private static void attachRestartListeners(@Nullable Drawable drawable) {
        AutoAnimatorDrawable.animatedDrawableHelper(drawable, animatable -> {
            if (animatable instanceof Animatable2Compat) {
                ((Animatable2Compat) animatable)
                        .registerAnimationCallback(LazyHolderCompat.INSTANCE);
            } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M
                    && animatable instanceof Animatable2) {
                ((Animatable2) animatable).registerAnimationCallback(LazyHolder.INSTANCE);
            }
        });
    }

    @TargetApi(Build.VERSION_CODES.KITKAT)
    private static void animatedDrawableHelper(
            @Nullable Drawable drawable, org.chromium.base.Callback<Animatable> consumer) {
        if (drawable == null) return;

        if (drawable instanceof Animatable) {
            consumer.onResult((Animatable) drawable);

            // Assume Animatable drawables can handle animating their own internals/sub drawables.
            return;
        }

        if (drawable != drawable.getCurrent()) {
            // Check obvious cases where the current drawable isn't actually being shown.  This
            // should support all {@link DrawableContainer} instances.
            AutoAnimatorDrawable.animatedDrawableHelper(drawable.getCurrent(), consumer);
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M
                && drawable instanceof android.graphics.drawable.DrawableWrapper) {
            // Support all modern versions of drawables that wrap other ones.  This won't cover old
            // versions of Android (see below for other if/else blocks).
            AutoAnimatorDrawable.animatedDrawableHelper(
                    ((android.graphics.drawable.DrawableWrapper) drawable).getDrawable(), consumer);
        } else if (drawable instanceof DrawableWrapper) {
            // Support the AppCompat DrawableWrapper.
            AutoAnimatorDrawable.animatedDrawableHelper(
                    ((DrawableWrapper) drawable).getWrappedDrawable(), consumer);
        } else if (drawable instanceof LayerDrawable) {
            // Support a LayerDrawable and try to animate all layers.
            LayerDrawable layerDrawable = (LayerDrawable) drawable;
            for (int i = 0; i < layerDrawable.getNumberOfLayers(); i++) {
                AutoAnimatorDrawable.animatedDrawableHelper(layerDrawable.getDrawable(i), consumer);
            }
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT
                && drawable instanceof InsetDrawable) {
            // Support legacy versions of InsetDrawable.
            AutoAnimatorDrawable.animatedDrawableHelper(
                    ((InsetDrawable) drawable).getDrawable(), consumer);
        } else if (drawable instanceof RotateDrawable) {
            // Support legacy versions of RotateDrawable.
            AutoAnimatorDrawable.animatedDrawableHelper(
                    ((RotateDrawable) drawable).getDrawable(), consumer);
        } else if (drawable instanceof ScaleDrawable) {
            // Support legacy versions of ScaleDrawable.
            AutoAnimatorDrawable.animatedDrawableHelper(
                    ((ScaleDrawable) drawable).getDrawable(), consumer);
        }
    }

    private static final class LazyHolder {
        private static final AutoRestarter INSTANCE = new AutoRestarter();
    }

    private static final class LazyHolderCompat {
        private static final AutoRestarterCompat INSTANCE = new AutoRestarterCompat();
    }

    private static final class AutoRestarterCompat extends Animatable2Compat.AnimationCallback {
        private final Handler mHandler = new Handler();

        // Animatable2Compat.AnimationCallback implementation.
        @Override
        public void onAnimationEnd(Drawable drawable) {
            if (!(drawable instanceof Animatable)) return;
            mHandler.post(() -> {
                if (drawable.isVisible()) ((Animatable) drawable).start();
            });
        }
    }

    @TargetApi(Build.VERSION_CODES.M)
    private static final class AutoRestarter extends Animatable2.AnimationCallback {
        // Animatable2.AnimationCallback implementation.
        @Override
        public void onAnimationEnd(Drawable drawable) {
            LazyHolderCompat.INSTANCE.onAnimationEnd(drawable);
        }
    }
}