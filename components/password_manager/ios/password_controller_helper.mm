// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "components/password_manager/ios/password_controller_helper.h"

#include <stddef.h>

#include "base/strings/sys_string_conversions.h"
#include "base/values.h"
#include "components/autofill/core/common/form_data.h"
#include "components/autofill/core/common/password_form.h"
#include "components/autofill/core/common/password_form_fill_data.h"
#include "components/autofill/ios/browser/autofill_util.h"
#include "components/password_manager/core/browser/form_parsing/ios_form_parser.h"
#include "components/password_manager/ios/account_select_fill_data.h"
#include "components/password_manager/ios/js_password_manager.h"
#import "ios/web/public/web_state/web_frame.h"
#import "ios/web/public/web_state/web_state.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

using autofill::FormData;
using autofill::PasswordForm;
using password_manager::GetPageURLAndCheckTrustLevel;
using password_manager::FillData;
using password_manager::SerializePasswordFormFillData;

namespace password_manager {
bool GetPageURLAndCheckTrustLevel(web::WebState* web_state,
                                  GURL* __nullable page_url) {
  auto trustLevel = web::URLVerificationTrustLevel::kNone;
  GURL dummy;
  if (!page_url) {
    page_url = &dummy;
  }
  *page_url = web_state->GetCurrentURL(&trustLevel);
  return trustLevel == web::URLVerificationTrustLevel::kAbsolute;
}
}  // namespace password_manager

namespace {
// Script command prefix for form changes. Possible command to be sent from
// injected JS is 'passwordForm.submitButtonClick'.
constexpr char kCommandPrefix[] = "passwordForm";
}  // namespace

@interface PasswordControllerHelper ()

@property(nonatomic, weak) id<PasswordControllerHelperDelegate> delegate;

// Handler for injected JavaScript callbacks.
- (BOOL)handleScriptCommand:(const base::DictionaryValue&)JSONCommand;

// Finds the currently submitted password form named |formName| and calls
// |completionHandler| with the populated data structure. |found| is YES if the
// current form was found successfully, NO otherwise.
- (void)extractSubmittedPasswordForm:(const std::string&)formName
                   completionHandler:
                       (void (^)(BOOL found,
                                 const PasswordForm& form))completionHandler;

// Parses the |jsonString| which contatins the password forms found on a web
// page to populate the |forms| vector.
- (void)getPasswordFormsFromJSON:(NSString*)jsonString
                         pageURL:(const GURL&)pageURL
                           forms:(std::vector<autofill::PasswordForm>*)forms;

// Autofills |username| and |password| into the form specified by |formData|,
// invoking |completionHandler| when finished with YES if successful and
// NO otherwise. |completionHandler| may be nil.
- (void)fillPasswordForm:(const autofill::PasswordFormFillData&)formData
            withUsername:(const base::string16&)username
                password:(const base::string16&)password
       completionHandler:(nullable void (^)(BOOL))completionHandler;

@end

// Category for test only.
@interface PasswordControllerHelper (Testing)

// Replaces JsPasswordManager for test.
- (void)setJsPasswordManager:(JsPasswordManager*)jsPasswordManager;

@end

@implementation PasswordControllerHelper {
  // The WebState this instance is observing. Will be null after
  // -webStateDestroyed: has been called.
  web::WebState* _webState;

  // Bridge to observe WebState from Objective-C.
  std::unique_ptr<web::WebStateObserverBridge> _webStateObserverBridge;

  // Bridge to observe form activity in |_webState|.
  std::unique_ptr<autofill::FormActivityObserverBridge>
      _formActivityObserverBridge;
}

#pragma mark - Properties

@synthesize delegate = _delegate;
@synthesize jsPasswordManager = _jsPasswordManager;

- (const GURL&)lastCommittedURL {
  return _webState ? _webState->GetLastCommittedURL() : GURL::EmptyGURL();
}

#pragma mark - Initialization

- (instancetype)initWithWebState:(web::WebState*)webState
                        delegate:
                            (id<PasswordControllerHelperDelegate>)delegate {
  self = [super init];
  if (self) {
    DCHECK(webState);
    _webState = webState;
    _delegate = delegate;
    _webStateObserverBridge =
        std::make_unique<web::WebStateObserverBridge>(self);
    _webState->AddObserver(_webStateObserverBridge.get());
    _formActivityObserverBridge =
        std::make_unique<autofill::FormActivityObserverBridge>(_webState, self);
    _jsPasswordManager = [[JsPasswordManager alloc]
        initWithReceiver:_webState->GetJSInjectionReceiver()];

    __weak PasswordControllerHelper* weakSelf = self;
    auto callback = base::BindRepeating(
        ^bool(const base::DictionaryValue& JSON, const GURL& originURL,
              bool interacting, bool isMainFrame, web::WebFrame* senderFrame) {
          if (!isMainFrame) {
            // Passwords is only supported on main frame.
            return false;
          }
          // |originURL| and |interacting| aren't used.
          return [weakSelf handleScriptCommand:JSON];
        });
    _webState->AddScriptCommandCallback(callback, kCommandPrefix);
  }
  return self;
}

#pragma mark - Dealloc

- (void)dealloc {
  if (_webState) {
    _webState->RemoveScriptCommandCallback(kCommandPrefix);
    _webState->RemoveObserver(_webStateObserverBridge.get());
  }
}

#pragma mark - CRWWebStateObserver

- (void)webStateDestroyed:(web::WebState*)webState {
  DCHECK_EQ(_webState, webState);
  if (_webState) {
    _webState->RemoveScriptCommandCallback(kCommandPrefix);
    _webState->RemoveObserver(_webStateObserverBridge.get());
    _webState = nullptr;
  }
  _webStateObserverBridge.reset();
  _formActivityObserverBridge.reset();
}

#pragma mark - FormActivityObserver

- (void)webState:(web::WebState*)webState
    didSubmitDocumentWithFormNamed:(const std::string&)formName
                          withData:(const std::string&)formData
                    hasUserGesture:(BOOL)hasUserGesture
                   formInMainFrame:(BOOL)formInMainFrame
                           inFrame:(web::WebFrame*)frame {
  DCHECK_EQ(_webState, webState);
  GURL pageURL = webState->GetLastCommittedURL();
  if (pageURL.GetOrigin() != frame->GetSecurityOrigin()) {
    // Passwords is only supported on main frame and iframes with the same
    // origin.
    return;
  }
  __weak PasswordControllerHelper* weakSelf = self;
  // This code is racing against the new page loading and will not get the
  // password form data if the page has changed. In most cases this code wins
  // the race.
  // TODO(crbug.com/418827): Fix this by passing in more data from the JS side.
  id completionHandler = ^(BOOL found, const autofill::PasswordForm& form) {
    PasswordControllerHelper* strongSelf = weakSelf;
    id<PasswordControllerHelperDelegate> strongDelegate = strongSelf.delegate;
    if (!strongSelf || !strongSelf->_webState || !strongDelegate) {
      return;
    }
    [strongDelegate helper:strongSelf
             didSubmitForm:form
               inMainFrame:formInMainFrame];
  };
  // TODO(crbug.com/418827): Use |formData| instead of extracting form again.
  [self extractSubmittedPasswordForm:formName
                   completionHandler:completionHandler];
}

#pragma mark - Private methods

- (BOOL)handleScriptCommand:(const base::DictionaryValue&)JSONCommand {
  std::string command;
  if (!JSONCommand.GetString("command", &command)) {
    return NO;
  }

  if (command != "passwordForm.submitButtonClick") {
    return NO;
  }

  GURL pageURL;
  if (!GetPageURLAndCheckTrustLevel(_webState, &pageURL)) {
    return NO;
  }

  FormData formData;
  if (!autofill::ExtractFormData(JSONCommand, false, base::string16(), pageURL,
                                 pageURL.GetOrigin(), &formData)) {
    return NO;
  }

  std::unique_ptr<PasswordForm> form =
      ParseFormData(formData, password_manager::FormParsingMode::SAVING);
  if (!form) {
    return NO;
  }

  if (_webState && self.delegate) {
    [self.delegate helper:self didSubmitForm:*form inMainFrame:YES];
    return YES;
  }

  return NO;
}

- (void)extractSubmittedPasswordForm:(const std::string&)formName
                   completionHandler:
                       (void (^)(BOOL found,
                                 const PasswordForm& form))completionHandler {
  DCHECK(completionHandler);

  if (!_webState) {
    return;
  }

  GURL pageURL;
  if (!GetPageURLAndCheckTrustLevel(_webState, &pageURL)) {
    completionHandler(NO, PasswordForm());
    return;
  }

  id extractSubmittedFormCompletionHandler = ^(NSString* jsonString) {
    std::unique_ptr<base::Value> formValue = autofill::ParseJson(jsonString);
    if (!formValue) {
      completionHandler(NO, PasswordForm());
      return;
    }

    FormData formData;
    if (!autofill::ExtractFormData(*formValue, false, base::string16(), pageURL,
                                   pageURL.GetOrigin(), &formData)) {
      completionHandler(NO, PasswordForm());
      return;
    }

    std::unique_ptr<PasswordForm> form =
        ParseFormData(formData, password_manager::FormParsingMode::SAVING);
    if (!form) {
      completionHandler(NO, PasswordForm());
      return;
    }

    completionHandler(YES, *form);
  };

  [self.jsPasswordManager extractForm:base::SysUTF8ToNSString(formName)
                    completionHandler:extractSubmittedFormCompletionHandler];
}

- (void)getPasswordFormsFromJSON:(NSString*)jsonString
                         pageURL:(const GURL&)pageURL
                           forms:(std::vector<autofill::PasswordForm>*)forms {
  std::vector<autofill::FormData> formsData;
  // Password is only available on main frame.
  if (!autofill::ExtractFormsData(jsonString, false, base::string16(), pageURL,
                                  pageURL.GetOrigin(), &formsData)) {
    return;
  }

  for (const auto& formData : formsData) {
    std::unique_ptr<PasswordForm> form =
        ParseFormData(formData, password_manager::FormParsingMode::FILLING);
    if (form) {
      forms->push_back(*form);
    }
  }
}

- (void)fillPasswordForm:(const autofill::PasswordFormFillData&)formData
            withUsername:(const base::string16&)username
                password:(const base::string16&)password
       completionHandler:(nullable void (^)(BOOL))completionHandler {
  if (formData.origin.GetOrigin() != self.lastCommittedURL.GetOrigin()) {
    if (completionHandler) {
      completionHandler(NO);
    }
    return;
  }

  // Send JSON over to the web view.
  [self.jsPasswordManager
       fillPasswordForm:SerializePasswordFormFillData(formData)
           withUsername:base::SysUTF16ToNSString(username)
               password:base::SysUTF16ToNSString(password)
      completionHandler:^(BOOL result) {
        if (completionHandler) {
          completionHandler(result);
        }
      }];
}

#pragma mark - Private methods for test only

- (void)setJsPasswordManager:(JsPasswordManager*)jsPasswordManager {
  _jsPasswordManager = jsPasswordManager;
}

#pragma mark - Public methods

- (void)findPasswordFormsWithCompletionHandler:
    (void (^)(const std::vector<autofill::PasswordForm>&))completionHandler {
  if (!_webState) {
    return;
  }

  GURL pageURL;
  if (!GetPageURLAndCheckTrustLevel(_webState, &pageURL)) {
    return;
  }

  __weak PasswordControllerHelper* weakSelf = self;
  [self.jsPasswordManager findPasswordFormsWithCompletionHandler:^(
                              NSString* jsonString) {
    std::vector<autofill::PasswordForm> forms;
    [weakSelf getPasswordFormsFromJSON:jsonString pageURL:pageURL forms:&forms];
    completionHandler(forms);
  }];
}

- (void)fillPasswordForm:(const autofill::PasswordFormFillData&)formData
       completionHandler:(nullable void (^)(BOOL))completionHandler {
  // Don't fill immediately if waiting for the user to type a username.
  if (formData.wait_for_username) {
    if (completionHandler) {
      completionHandler(NO);
    }
    return;
  }

  [self fillPasswordForm:formData
            withUsername:formData.username_field.value
                password:formData.password_field.value
       completionHandler:completionHandler];
}

- (void)fillPasswordFormWithFillData:(const password_manager::FillData&)fillData
                   completionHandler:
                       (nullable void (^)(BOOL))completionHandler {
  [self.jsPasswordManager
       fillPasswordForm:SerializeFillData(fillData)
           withUsername:base::SysUTF16ToNSString(fillData.username_value)
               password:base::SysUTF16ToNSString(fillData.password_value)
      completionHandler:^(BOOL result) {
        if (completionHandler) {
          completionHandler(result);
        }
      }];
}

- (void)findAndFillPasswordFormsWithUserName:(NSString*)username
                                    password:(NSString*)password
                           completionHandler:
                               (nullable void (^)(BOOL))completionHandler {
  __weak PasswordControllerHelper* weakSelf = self;
  [self findPasswordFormsWithCompletionHandler:^(
            const std::vector<autofill::PasswordForm>& forms) {
    PasswordControllerHelper* strongSelf = weakSelf;
    for (const auto& form : forms) {
      autofill::PasswordFormFillData formData;
      std::map<base::string16, const autofill::PasswordForm*> matches;
      // Initialize |matches| to satisfy the expectation from
      // InitPasswordFormFillData() that the preferred match (3rd parameter)
      // should be one of the |matches|.
      matches.insert(std::make_pair(form.username_value, &form));
      autofill::InitPasswordFormFillData(form, matches, &form, false,
                                         &formData);
      [strongSelf fillPasswordForm:formData
                      withUsername:base::SysNSStringToUTF16(username)
                          password:base::SysNSStringToUTF16(password)
                 completionHandler:completionHandler];
    }
  }];
}

@end
