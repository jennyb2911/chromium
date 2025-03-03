// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The <webview> custom element.

var registerElement = require('guestViewContainerElement').registerElement;
var forwardApiMethods = require('guestViewContainerElement').forwardApiMethods;
var GuestViewContainerElement =
    require('guestViewContainerElement').GuestViewContainerElement;
var WebViewImpl = require('webView').WebViewImpl;
var WebViewConstants = require('webViewConstants').WebViewConstants;
var WEB_VIEW_API_METHODS = require('webViewApiMethods').WEB_VIEW_API_METHODS;
var WebViewInternal = getInternalApi ?
    getInternalApi('webViewInternal') :
    require('webViewInternal').WebViewInternal;

class WebViewElement extends GuestViewContainerElement {
  static get observedAttributes() {
    return [
      WebViewConstants.ATTRIBUTE_ALLOWTRANSPARENCY,
      WebViewConstants.ATTRIBUTE_ALLOWSCALING,
      WebViewConstants.ATTRIBUTE_AUTOSIZE, WebViewConstants.ATTRIBUTE_MAXHEIGHT,
      WebViewConstants.ATTRIBUTE_MAXWIDTH, WebViewConstants.ATTRIBUTE_MINHEIGHT,
      WebViewConstants.ATTRIBUTE_MINWIDTH, WebViewConstants.ATTRIBUTE_NAME,
      WebViewConstants.ATTRIBUTE_PARTITION, WebViewConstants.ATTRIBUTE_SRC
    ];
  }

  constructor() {
    super();
    privates(this).internal = new WebViewImpl(this);
  }
}

WebViewElement.prototype.addContentScripts = function(rules) {
  var internal = privates(this).internal;
  return WebViewInternal.addContentScripts(internal.viewInstanceId, rules);
};

WebViewElement.prototype.removeContentScripts = function(names) {
  var internal = privates(this).internal;
  return WebViewInternal.removeContentScripts(internal.viewInstanceId, names);
};

WebViewElement.prototype.insertCSS = function(var_args) {
  var internal = privates(this).internal;
  return internal.executeCode(
      WebViewInternal.insertCSS, $Array.slice(arguments));
};

WebViewElement.prototype.executeScript = function(var_args) {
  var internal = privates(this).internal;
  return internal.executeCode(
      WebViewInternal.executeScript, $Array.slice(arguments));
};

WebViewElement.prototype.print = function() {
  var internal = privates(this).internal;
  return internal.executeCode(
      WebViewInternal.executeScript, [{code: 'window.print();'}]);
};

WebViewElement.prototype.back = function(callback) {
  return $Function.call(
      privates(WebViewElement).originalGo, this, -1, callback);
};

WebViewElement.prototype.canGoBack = function() {
  var internal = privates(this).internal;
  return internal.entryCount > 1 && internal.currentEntryIndex > 0;
};

WebViewElement.prototype.canGoForward = function() {
  var internal = privates(this).internal;
  return internal.currentEntryIndex >= 0 &&
      internal.currentEntryIndex < (internal.entryCount - 1);
};

WebViewElement.prototype.forward = function(callback) {
  return $Function.call(privates(WebViewElement).originalGo, this, 1, callback);
};

WebViewElement.prototype.getProcessId = function() {
  var internal = privates(this).internal;
  return internal.processId;
};

WebViewElement.prototype.getUserAgent = function() {
  var internal = privates(this).internal;
  return internal.userAgentOverride || navigator.userAgent;
};

WebViewElement.prototype.isUserAgentOverridden = function() {
  var internal = privates(this).internal;
  return !!internal.userAgentOverride &&
      internal.userAgentOverride != navigator.userAgent;
};

// Forward remaining WebViewElement.foo* method calls to WebViewImpl.foo* or
// WebViewInternal.foo*.
forwardApiMethods(
    WebViewElement, WebViewImpl, WebViewInternal, WEB_VIEW_API_METHODS);

// Since |back| and |forward| are implemented in terms of |go|, we need to
// keep a reference to the real |go| function, since user code may override
// |WebViewElement.prototype.go|.
privates(WebViewElement).originalGo = WebViewElement.prototype.go;

registerElement('WebView', WebViewElement);
