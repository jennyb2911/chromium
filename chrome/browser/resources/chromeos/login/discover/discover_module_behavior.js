// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @fileoverview
 * 'DiscoverModuleBehavior' is a behavior for discover modules.
 * Extends OobeDialogHostBehavior, I18nBehavior.
 */


/**
 * Implements extension of OobeDialogHostBehavior.
 * @polymerBehavior
 */
var DiscoverModuleBehaviorImpl = {
  properties: {
    /**
     * Discover module name. Must be set explicitly.
     */
    module: {
      type: String,
      readOnly: true,
      value: '',
    },
  },

  sendMessage: function(message, parameters) {
    assert(this.module.length > 0);
    chrome.send('discover.' + this.module + '.' + message, parameters);
  },

  show: function() {},

  updateLocalizedContent: function() {
    // Pass to I18nBehavior.
    this.i18nUpdateLocale();
  },
};

var DiscoverModuleBehavior =
    [I18nBehavior, OobeDialogHostBehavior, DiscoverModuleBehaviorImpl];
