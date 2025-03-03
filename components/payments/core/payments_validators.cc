// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/payments/core/payments_validators.h"

#include "third_party/re2/src/re2/re2.h"
#include "url/gurl.h"

namespace payments {
namespace {

// We limit the maximum length of string to 2048 bytes for security reasons.
static const size_t maximumStringLength = 2048;

}  // namespace

// static
bool PaymentsValidators::IsValidCurrencyCodeFormat(
    const std::string& code,
    std::string* optional_error_message) {
  if (RE2::FullMatch(code, "[A-Z]{3}"))
    return true;

  if (optional_error_message)
    *optional_error_message = "'" + code +
                              "' is not a valid ISO 4217 currency code, should "
                              "be well-formed 3-letter alphabetic code.";

  return false;
}

// static
bool PaymentsValidators::IsValidAmountFormat(
    const std::string& amount,
    std::string* optional_error_message) {
  if (RE2::FullMatch(amount, "-?[0-9]+(\\.[0-9]+)?"))
    return true;

  if (optional_error_message)
    *optional_error_message = "'" + amount + "' is not a valid amount format";

  return false;
}

// static
bool PaymentsValidators::IsValidCountryCodeFormat(
    const std::string& code,
    std::string* optional_error_message) {
  if (RE2::FullMatch(code, "[A-Z]{2}"))
    return true;

  if (optional_error_message)
    *optional_error_message = "'" + code +
                              "' is not a valid CLDR country code, should be 2 "
                              "upper case letters [A-Z]";

  return false;
}

// static
bool PaymentsValidators::IsValidLanguageCodeFormat(
    const std::string& code,
    std::string* optional_error_message) {
  if (RE2::FullMatch(code, "([a-z]{2,3})?"))
    return true;

  if (optional_error_message)
    *optional_error_message =
        "'" + code +
        "' is not a valid BCP-47 language code, should be "
        "2-3 lower case letters [a-z]";

  return false;
}

// static
bool PaymentsValidators::IsValidScriptCodeFormat(
    const std::string& code,
    std::string* optional_error_message) {
  if (RE2::FullMatch(code, "([A-Z][a-z]{3})?"))
    return true;

  if (optional_error_message)
    *optional_error_message =
        "'" + code +
        "' is not a valid ISO 15924 script code, should be "
        "an upper case letter [A-Z] followed by 3 lower "
        "case letters [a-z]";

  return false;
}

// static
void PaymentsValidators::SplitLanguageTag(const std::string& tag,
                                          std::string* language_code,
                                          std::string* script_code) {
  RE2::FullMatch(tag, "^([a-z]{2})(-([A-Z][a-z]{3}))?(-[A-Za-z]+)*$",
                 language_code, (void*)nullptr, script_code);
}

// static
bool PaymentsValidators::IsValidErrorMsgFormat(
    const std::string& error,
    std::string* optional_error_message) {
  if (error.length() <= maximumStringLength)
    return true;

  if (optional_error_message)
    *optional_error_message =
        "Error message should be at most 2048 characters long";

  return false;
}

// static
bool PaymentsValidators::IsValidAddressErrorsFormat(
    const mojom::AddressErrorsPtr& errors,
    std::string* optional_error_message) {
  return errors &&
         IsValidErrorMsgFormat(errors->address_line, optional_error_message) &&
         IsValidErrorMsgFormat(errors->city, optional_error_message) &&
         IsValidErrorMsgFormat(errors->country, optional_error_message) &&
         IsValidErrorMsgFormat(errors->dependent_locality,
                               optional_error_message) &&
         IsValidErrorMsgFormat(errors->language_code, optional_error_message) &&
         IsValidErrorMsgFormat(errors->organization, optional_error_message) &&
         IsValidErrorMsgFormat(errors->phone, optional_error_message) &&
         IsValidErrorMsgFormat(errors->postal_code, optional_error_message) &&
         IsValidErrorMsgFormat(errors->recipient, optional_error_message) &&
         IsValidErrorMsgFormat(errors->region, optional_error_message) &&
         IsValidErrorMsgFormat(errors->region_code, optional_error_message) &&
         IsValidErrorMsgFormat(errors->sorting_code, optional_error_message);
}

// static
bool PaymentsValidators::IsValidPayerErrorsFormat(
    const mojom::PayerErrorFieldsPtr& errors,
    std::string* optional_error_message) {
  return errors &&
         IsValidErrorMsgFormat(errors->email, optional_error_message) &&
         IsValidErrorMsgFormat(errors->name, optional_error_message) &&
         IsValidErrorMsgFormat(errors->phone, optional_error_message);
}

// static
bool PaymentsValidators::IsValidPaymentValidationErrorsFormat(
    const mojom::PaymentValidationErrorsPtr& errors,
    std::string* optional_error_message) {
  return errors &&
         IsValidAddressErrorsFormat(errors->shipping_address,
                                    optional_error_message) &&
         IsValidPayerErrorsFormat(errors->payer, optional_error_message);
}

}  // namespace payments
