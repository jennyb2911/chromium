// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/storage/cached_storage_area.h"

#include "base/metrics/histogram_macros.h"
#include "base/rand_util.h"
#include "mojo/public/cpp/bindings/strong_associated_binding.h"
#include "third_party/blink/public/platform/scheduler/web_thread_scheduler.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"
#include "third_party/blink/renderer/platform/wtf/text/string_buffer.h"
#include "third_party/blink/renderer/platform/wtf/text/unicode.h"
#include "third_party/blink/renderer/platform/wtf/text/utf8.h"

namespace blink {

namespace {

// Don't change or reorder any of the values in this enum, as these values
// are serialized on disk.
enum class StorageFormat : uint8_t { UTF16 = 0, Latin1 = 1 };

class GetAllCallback : public mojom::blink::StorageAreaGetAllCallback {
 public:
  static mojom::blink::StorageAreaGetAllCallbackAssociatedPtrInfo CreateAndBind(
      base::OnceCallback<void(bool)> callback) {
    mojom::blink::StorageAreaGetAllCallbackAssociatedPtrInfo ptr_info;
    auto request = mojo::MakeRequest(&ptr_info);
    mojo::MakeStrongAssociatedBinding(
        base::WrapUnique(new GetAllCallback(std::move(callback))),
        std::move(request));
    return ptr_info;
  }

 private:
  explicit GetAllCallback(base::OnceCallback<void(bool)> callback)
      : m_callback(std::move(callback)) {}
  void Complete(bool success) override { std::move(m_callback).Run(success); }

  base::OnceCallback<void(bool)> m_callback;
};

// These methods are used to pack and unpack the page_url/storage_area_id into
// source strings to/from the browser.
String PackSource(const KURL& page_url, const String& storage_area_id) {
  return page_url.GetString() + "\n" + storage_area_id;
}

void UnpackSource(const String& source,
                  KURL* page_url,
                  String* storage_area_id) {
  Vector<String> result;
  source.Split("\n", true, result);
  DCHECK_EQ(result.size(), 2u);
  *page_url = KURL(result[0]);
  *storage_area_id = result[1];
}

}  // namespace

// static
scoped_refptr<CachedStorageArea> CachedStorageArea::CreateForLocalStorage(
    scoped_refptr<const SecurityOrigin> origin,
    mojo::InterfacePtr<mojom::blink::StorageArea> area,
    scoped_refptr<base::SingleThreadTaskRunner> ipc_runner) {
  return base::AdoptRef(new CachedStorageArea(
      std::move(origin), std::move(area), std::move(ipc_runner)));
}

// static
scoped_refptr<CachedStorageArea> CachedStorageArea::CreateForSessionStorage(
    scoped_refptr<const SecurityOrigin> origin,
    mojo::AssociatedInterfacePtr<mojom::blink::StorageArea> area,
    scoped_refptr<base::SingleThreadTaskRunner> ipc_runner) {
  return base::AdoptRef(new CachedStorageArea(
      std::move(origin), std::move(area), std::move(ipc_runner)));
}

unsigned CachedStorageArea::GetLength() {
  EnsureLoaded();
  return map_->GetLength();
}

String CachedStorageArea::GetKey(unsigned index) {
  EnsureLoaded();
  return map_->GetKey(index);
}

String CachedStorageArea::GetItem(const String& key) {
  EnsureLoaded();
  return map_->GetItem(key);
}

bool CachedStorageArea::SetItem(const String& key,
                                const String& value,
                                Source* source) {
  DCHECK(areas_->Contains(source));

  // A quick check to reject obviously overbudget items to avoid priming the
  // cache.
  if ((key.length() + value.length()) * 2 >
      mojom::blink::StorageArea::kPerStorageAreaQuota)
    return false;

  EnsureLoaded();
  String old_value;
  if (!map_->SetItem(key, value, &old_value))
    return false;

  // Determine data formats.
  const FormatOption key_format = GetKeyFormat();
  const FormatOption value_format = GetValueFormat();

  // Ignore mutations to |key| until OnSetItemComplete.
  auto ignore_add_result = ignore_key_mutations_.insert(key, 1);
  if (!ignore_add_result.is_new_entry)
    ignore_add_result.stored_value->value++;

  base::Optional<Vector<uint8_t>> optional_old_value;
  if (!old_value.IsNull())
    optional_old_value = StringToUint8Vector(old_value, value_format);

  KURL page_url = source->GetPageUrl();
  String source_id = areas_->at(source);

  blink::WebScopedVirtualTimePauser virtual_time_pauser =
      source->CreateWebScopedVirtualTimePauser(
          "CachedStorageArea",
          WebScopedVirtualTimePauser::VirtualTaskDuration::kNonInstant);
  virtual_time_pauser.PauseVirtualTime();
  mojo_area_->Put(StringToUint8Vector(key, key_format),
                  StringToUint8Vector(value, value_format), optional_old_value,
                  PackSource(page_url, source_id),
                  WTF::Bind(&CachedStorageArea::OnSetItemComplete,
                            weak_factory_.GetWeakPtr(), key,
                            std::move(virtual_time_pauser)));
  if (IsSessionStorage() && old_value != value) {
    for (const auto& area : *areas_) {
      if (area.key != source)
        area.key->EnqueueStorageEvent(key, old_value, value, page_url);
    }
  }
  return true;
}

void CachedStorageArea::RemoveItem(const String& key, Source* source) {
  DCHECK(areas_->Contains(source));

  EnsureLoaded();
  String old_value;
  if (!map_->RemoveItem(key, &old_value))
    return;

  // Determine data formats.
  const FormatOption key_format = GetKeyFormat();
  const FormatOption value_format = GetValueFormat();

  // Ignore mutations to |key| until OnRemoveItemComplete.
  auto ignore_add_result = ignore_key_mutations_.insert(key, 1);
  if (!ignore_add_result.is_new_entry)
    ignore_add_result.stored_value->value++;

  base::Optional<Vector<uint8_t>> optional_old_value;
  if (should_send_old_value_on_mutations_)
    optional_old_value = StringToUint8Vector(old_value, value_format);

  KURL page_url = source->GetPageUrl();
  String source_id = areas_->at(source);

  blink::WebScopedVirtualTimePauser virtual_time_pauser =
      source->CreateWebScopedVirtualTimePauser(
          "CachedStorageArea",
          WebScopedVirtualTimePauser::VirtualTaskDuration::kNonInstant);
  virtual_time_pauser.PauseVirtualTime();
  mojo_area_->Delete(StringToUint8Vector(key, key_format), optional_old_value,
                     PackSource(page_url, source_id),
                     WTF::Bind(&CachedStorageArea::OnRemoveItemComplete,
                               weak_factory_.GetWeakPtr(), key,
                               std::move(virtual_time_pauser)));
  if (IsSessionStorage()) {
    for (const auto& area : *areas_) {
      if (area.key != source)
        area.key->EnqueueStorageEvent(key, old_value, String(), page_url);
    }
  }
}

void CachedStorageArea::Clear(Source* source) {
  DCHECK(areas_->Contains(source));

  bool already_empty = false;
  if (IsSessionStorage()) {
    EnsureLoaded();
    already_empty = map_->GetLength() == 0u;
  }
  // No need to prime the cache in this case.
  Reset();
  map_ = std::make_unique<StorageAreaMap>(
      mojom::blink::StorageArea::kPerStorageAreaQuota);
  ignore_all_mutations_ = true;

  KURL page_url = source->GetPageUrl();
  String source_id = areas_->at(source);

  blink::WebScopedVirtualTimePauser virtual_time_pauser =
      source->CreateWebScopedVirtualTimePauser(
          "CachedStorageArea",
          WebScopedVirtualTimePauser::VirtualTaskDuration::kNonInstant);
  virtual_time_pauser.PauseVirtualTime();
  mojo_area_->DeleteAll(
      PackSource(page_url, source_id),
      WTF::Bind(&CachedStorageArea::OnClearComplete, weak_factory_.GetWeakPtr(),
                std::move(virtual_time_pauser)));
  if (IsSessionStorage() && !already_empty) {
    for (const auto& area : *areas_) {
      if (area.key != source)
        area.key->EnqueueStorageEvent(String(), String(), String(), page_url);
    }
  }
}

String CachedStorageArea::RegisterSource(Source* source) {
  String id = String::Number(base::RandUint64());
  areas_->insert(source, id);
  return id;
}

CachedStorageArea::CachedStorageArea(
    scoped_refptr<const SecurityOrigin> origin,
    mojo::InterfacePtr<mojom::blink::StorageArea> area,
    scoped_refptr<base::SingleThreadTaskRunner> ipc_runner)
    : origin_(std::move(origin)),
      mojo_area_(area.get()),
      mojo_area_ptr_(std::move(area)),
      binding_(this),
      areas_(new HeapHashMap<WeakMember<Source>, String>),
      weak_factory_(this) {
  mojom::blink::StorageAreaObserverAssociatedPtrInfo ptr_info;
  binding_.Bind(mojo::MakeRequest(&ptr_info), std::move(ipc_runner));
  mojo_area_->AddObserver(std::move(ptr_info));
}

CachedStorageArea::CachedStorageArea(
    scoped_refptr<const SecurityOrigin> origin,
    mojo::AssociatedInterfacePtr<mojom::blink::StorageArea> area,
    scoped_refptr<base::SingleThreadTaskRunner> ipc_runner)
    : origin_(std::move(origin)),
      mojo_area_(area.get()),
      mojo_area_associated_ptr_(std::move(area)),
      binding_(this),
      areas_(new HeapHashMap<WeakMember<Source>, String>),
      weak_factory_(this) {
  mojom::blink::StorageAreaObserverAssociatedPtrInfo ptr_info;
  binding_.Bind(mojo::MakeRequest(&ptr_info), std::move(ipc_runner));
  mojo_area_->AddObserver(std::move(ptr_info));
}

CachedStorageArea::~CachedStorageArea() = default;

void CachedStorageArea::KeyAdded(const Vector<uint8_t>& key,
                                 const Vector<uint8_t>& value,
                                 const String& source) {
  DCHECK(!IsSessionStorage());
  KeyAddedOrChanged(key, value, String(), source);
}

void CachedStorageArea::KeyChanged(const Vector<uint8_t>& key,
                                   const Vector<uint8_t>& new_value,
                                   const Vector<uint8_t>& old_value,
                                   const String& source) {
  DCHECK(!IsSessionStorage());
  KeyAddedOrChanged(
      key, new_value,
      Uint8VectorToString(old_value, FormatOption::kLocalStorageDetectFormat),
      source);
}

void CachedStorageArea::KeyDeleted(const Vector<uint8_t>& key,
                                   const Vector<uint8_t>& old_value,
                                   const String& source) {
  DCHECK(!IsSessionStorage());
  KURL page_url;
  String storage_area_id;
  UnpackSource(source, &page_url, &storage_area_id);

  String key_string =
      Uint8VectorToString(key, FormatOption::kLocalStorageDetectFormat);

  bool from_local_area = false;
  for (const auto& area : *areas_) {
    if (area.value == storage_area_id) {
      from_local_area = true;
    } else {
      area.key->EnqueueStorageEvent(
          key_string,
          Uint8VectorToString(old_value,
                              FormatOption::kLocalStorageDetectFormat),
          String(), page_url);
    }
  }

  if (map_ && !from_local_area) {
    // This was from another process or the storage area is gone. If the former,
    // remove it from our cache if we haven't already changed it and are waiting
    // for the confirmation callback. In the latter case, we won't do anything
    // because ignore_key_mutations_ won't be updated until the callback runs.
    if (!ignore_all_mutations_ &&
        ignore_key_mutations_.find(key_string) == ignore_key_mutations_.end())
      map_->RemoveItem(key_string, nullptr);
  }
}

void CachedStorageArea::AllDeleted(const String& source) {
  KURL page_url;
  String storage_area_id;
  UnpackSource(source, &page_url, &storage_area_id);

  bool from_local_area = false;
  for (const auto& area : *areas_) {
    if (area.value == storage_area_id) {
      from_local_area = true;
    } else {
      area.key->EnqueueStorageEvent(String(), String(), String(), page_url);
    }
  }

  if (map_ && !from_local_area && !ignore_all_mutations_) {
    auto old = std::move(map_);
    map_ = std::make_unique<StorageAreaMap>(
        mojom::blink::StorageArea::kPerStorageAreaQuota);

    // We have to retain local additions which happened after this clear
    // operation from another process.
    auto iter = ignore_key_mutations_.begin();
    while (iter != ignore_key_mutations_.end()) {
      String value = old->GetItem(iter->key);
      if (!value.IsNull())
        map_->SetItemIgnoringQuota(iter->key, value);
      ++iter;
    }
  }
}

void CachedStorageArea::ShouldSendOldValueOnMutations(bool value) {
  DCHECK(!IsSessionStorage());
  should_send_old_value_on_mutations_ = value;
}

void CachedStorageArea::KeyAddedOrChanged(const Vector<uint8_t>& key,
                                          const Vector<uint8_t>& new_value,
                                          const String& old_value,
                                          const String& source) {
  DCHECK(!IsSessionStorage());
  KURL page_url;
  String storage_area_id;
  UnpackSource(source, &page_url, &storage_area_id);

  String key_string =
      Uint8VectorToString(key, FormatOption::kLocalStorageDetectFormat);
  String new_value_string =
      Uint8VectorToString(new_value, FormatOption::kLocalStorageDetectFormat);

  bool from_local_area = false;
  for (const auto& area : *areas_) {
    if (area.value == storage_area_id) {
      from_local_area = true;
    } else {
      area.key->EnqueueStorageEvent(key_string, old_value, new_value_string,
                                    page_url);
    }
  }

  if (map_ && !from_local_area) {
    // This was from another process or the storage area is gone. If the former,
    // apply it to our cache if we haven't already changed it and are waiting
    // for the confirmation callback. In the latter case, we won't do anything
    // because ignore_key_mutations_ won't be updated until the callback runs.
    if (!ignore_all_mutations_ &&
        ignore_key_mutations_.find(key_string) == ignore_key_mutations_.end()) {
      // We turn off quota checking here to accommodate the over budget
      // allowance that's provided in the browser process.
      map_->SetItemIgnoringQuota(key_string, new_value_string);
    }
  }
}

void CachedStorageArea::OnSetItemComplete(const String& key,
                                          WebScopedVirtualTimePauser,
                                          bool success) {
  if (!success) {
    Reset();
    return;
  }

  auto it = ignore_key_mutations_.find(key);
  DCHECK(it != ignore_key_mutations_.end());
  if (--it->value == 0)
    ignore_key_mutations_.erase(it);
}

void CachedStorageArea::OnRemoveItemComplete(const String& key,
                                             WebScopedVirtualTimePauser,
                                             bool success) {
  DCHECK(success);
  auto it = ignore_key_mutations_.find(key);
  DCHECK(it != ignore_key_mutations_.end());
  if (--it->value == 0)
    ignore_key_mutations_.erase(it);
}

void CachedStorageArea::OnClearComplete(WebScopedVirtualTimePauser,
                                        bool success) {
  DCHECK(success);
  DCHECK(ignore_all_mutations_);
  ignore_all_mutations_ = false;
}

void CachedStorageArea::OnGetAllComplete(bool success) {
  // Since the GetAll method is synchronous, we need this asynchronously
  // delivered notification to avoid applying changes to the returned array
  // that we already have.
  DCHECK(success);
  DCHECK(ignore_all_mutations_);
  ignore_all_mutations_ = false;
}

void CachedStorageArea::EnsureLoaded() {
  if (map_)
    return;

  base::TimeTicks before = base::TimeTicks::Now();

  ignore_all_mutations_ = true;
  bool success = false;
  Vector<mojom::blink::KeyValuePtr> data;
  mojo_area_->GetAll(
      GetAllCallback::CreateAndBind(WTF::Bind(
          &CachedStorageArea::OnGetAllComplete, weak_factory_.GetWeakPtr())),
      &success, &data);

  // Determine data formats.
  const FormatOption key_format = GetKeyFormat();
  const FormatOption value_format = GetValueFormat();

  map_ = std::make_unique<StorageAreaMap>(
      mojom::blink::StorageArea::kPerStorageAreaQuota);
  for (const auto& item : data) {
    map_->SetItemIgnoringQuota(Uint8VectorToString(item->key, key_format),
                               Uint8VectorToString(item->value, value_format));
  }

  base::TimeDelta time_to_prime = base::TimeTicks::Now() - before;
  UMA_HISTOGRAM_TIMES("LocalStorage.MojoTimeToPrime", time_to_prime);

  size_t local_storage_size_kb = map_->quota_used() / 1024;
  // Track localStorage size, from 0-6MB. Note that the maximum size should be
  // 10MB, but we add some slop since we want to make sure the max size is
  // always above what we see in practice, since histograms can't change.
  UMA_HISTOGRAM_CUSTOM_COUNTS("LocalStorage.MojoSizeInKB",
                              local_storage_size_kb, 1, 6 * 1024, 50);
  if (local_storage_size_kb < 100) {
    UMA_HISTOGRAM_TIMES("LocalStorage.MojoTimeToPrimeForUnder100KB",
                        time_to_prime);
  } else if (local_storage_size_kb < 1000) {
    UMA_HISTOGRAM_TIMES("LocalStorage.MojoTimeToPrimeFor100KBTo1MB",
                        time_to_prime);
  } else {
    UMA_HISTOGRAM_TIMES("LocalStorage.MojoTimeToPrimeFor1MBTo5MB",
                        time_to_prime);
  }
}

void CachedStorageArea::Reset() {
  map_ = nullptr;
  ignore_key_mutations_.clear();
  ignore_all_mutations_ = false;
  weak_factory_.InvalidateWeakPtrs();
}

CachedStorageArea::FormatOption CachedStorageArea::GetKeyFormat() const {
  return IsSessionStorage() ? FormatOption::kSessionStorageForceUTF8
                            : FormatOption::kLocalStorageDetectFormat;
}

CachedStorageArea::FormatOption CachedStorageArea::GetValueFormat() const {
  return IsSessionStorage() ? FormatOption::kSessionStorageForceUTF16
                            : FormatOption::kLocalStorageDetectFormat;
}

bool CachedStorageArea::IsSessionStorage() const {
  return mojo_area_associated_ptr_.is_bound();
}

// static
String CachedStorageArea::Uint8VectorToString(const Vector<uint8_t>& input,
                                              FormatOption format_option) {
  if (input.IsEmpty())
    return g_empty_string;
  const size_t input_size = input.size();
  String result;
  bool corrupt = false;
  switch (format_option) {
    case FormatOption::kSessionStorageForceUTF16: {
      if (input_size % sizeof(UChar) != 0) {
        corrupt = true;
        break;
      }
      StringBuffer<UChar> buffer(input_size / sizeof(UChar));
      std::memcpy(buffer.Characters(), input.data(), input_size);
      result = String::Adopt(buffer);
      break;
    }
    case FormatOption::kSessionStorageForceUTF8: {
      // TODO(mek): When this lived in content it used to do a "lenient"
      // conversion, while this is a strict conversion. Figure out if that
      // difference actually matters in practice.
      result = String::FromUTF8(input.data(), input_size);
      if (result.IsNull()) {
        corrupt = true;
        break;
      }
      break;
    }
    case FormatOption::kLocalStorageDetectFormat: {
      StorageFormat format = static_cast<StorageFormat>(input[0]);
      const size_t payload_size = input_size - 1;
      switch (format) {
        case StorageFormat::UTF16: {
          if (payload_size % sizeof(UChar) != 0) {
            corrupt = true;
            break;
          }
          StringBuffer<UChar> buffer(payload_size / sizeof(UChar));
          std::memcpy(buffer.Characters(), input.data() + 1, payload_size);
          result = String::Adopt(buffer);
          break;
        }
        case StorageFormat::Latin1:
          result = String(reinterpret_cast<const char*>(input.data() + 1),
                          payload_size);
          break;
        default:
          corrupt = true;
      }
      break;
    }
  }
  if (corrupt) {
    // TODO(mek): Better error recovery when corrupt (or otherwise invalid) data
    // is detected.
    LOCAL_HISTOGRAM_BOOLEAN("LocalStorageCachedArea.CorruptData", true);
    LOG(ERROR) << "Corrupt data in domstorage";
    return g_empty_string;
  }
  return result;
}

// static
Vector<uint8_t> CachedStorageArea::StringToUint8Vector(
    const String& input,
    FormatOption format_option) {
  switch (format_option) {
    case FormatOption::kSessionStorageForceUTF16: {
      Vector<uint8_t> result(input.length() * sizeof(UChar));
      input.CopyTo(reinterpret_cast<UChar*>(result.data()), 0, input.length());
      return result;
    }
    case FormatOption::kSessionStorageForceUTF8: {
      unsigned length = input.length();
      if (input.Is8Bit() && input.ContainsOnlyASCII()) {
        Vector<uint8_t> result(length);
        std::memcpy(result.data(), input.Characters8(), length);
        return result;
      }
      // Handle 8 bit case where it's not only ascii.
      if (input.Is8Bit()) {
        // This code is copied from WTF::String::Utf8(), except the vector
        // doesn't have a stack-allocated capacity.
        // We do this because there isn't a way to transform the CString we get
        // from WTF::String::Utf8() to a Vector without an extra copy.
        if (length > std::numeric_limits<unsigned>::max() / 3)
          return Vector<uint8_t>();
        Vector<uint8_t> buffer_vector(length * 3);
        uint8_t* buffer = buffer_vector.data();
        const LChar* characters = input.Characters8();

        WTF::Unicode::ConversionResult result =
            WTF::Unicode::ConvertLatin1ToUTF8(
                &characters, characters + length,
                reinterpret_cast<char**>(&buffer),
                reinterpret_cast<char*>(buffer + buffer_vector.size()));
        // (length * 3) should be sufficient for any conversion
        DCHECK_NE(result, WTF::Unicode::kTargetExhausted);
        buffer_vector.Shrink(buffer - buffer_vector.data());
        return buffer_vector;
      }

      // TODO(dmurph): Figure out how to avoid a copy here.
      // TODO(dmurph): Handle invalid UTF16 better. https://crbug.com/873280.
      CString utf8 = input.Utf8(
          WTF::kStrictUTF8ConversionReplacingUnpairedSurrogatesWithFFFD);
      Vector<uint8_t> result(utf8.length());
      std::memcpy(result.data(), utf8.data(), utf8.length());
      return result;
    }
    case FormatOption::kLocalStorageDetectFormat: {
      if (input.ContainsOnlyLatin1()) {
        Vector<uint8_t> result(input.length() + 1);
        result[0] = static_cast<uint8_t>(StorageFormat::Latin1);
        if (input.Is8Bit()) {
          std::memcpy(result.data() + 1, input.Characters8(), input.length());
        } else {
          for (unsigned i = 0; i < input.length(); ++i) {
            result[i + 1] = input[i];
          }
        }
        return result;
      }
      DCHECK(!input.Is8Bit());
      Vector<uint8_t> result(input.length() * sizeof(UChar) + 1);
      result[0] = static_cast<uint8_t>(StorageFormat::UTF16);
      std::memcpy(result.data() + 1, input.Characters16(),
                  input.length() * sizeof(UChar));
      return result;
    }
  }
  NOTREACHED();
}

}  // namespace blink
