// Copyright 2020 The TensorStore Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TENSORSTORE_KVSTORE_TEST_UTIL_H_
#define TENSORSTORE_KVSTORE_TEST_UTIL_H_

#include <map>
#include <optional>
#include <string>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/functional/function_ref.h"
#include "absl/strings/cord.h"
#include "absl/time/time.h"
#include "tensorstore/internal/queue_testutil.h"
#include "tensorstore/kvstore/driver.h"
#include "tensorstore/kvstore/generation.h"
#include "tensorstore/kvstore/generation_testutil.h"
#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/util/future.h"
#include "tensorstore/util/result.h"
#include "tensorstore/util/status.h"

namespace tensorstore {
namespace internal {

/// Tests all operations on `store`, which should be empty.
///
/// \param get_key Maps arbitrary strings (which are nonetheless valid file
///     paths) to keys in the format expected by `store`.  For stores that
///     support file paths as keys, `get_key` can simply be the identity
///     function.  This function must ensure that a given input key always maps
///     to the same output key, and distinct input keys always map to distinct
///     output keys.
void TestKeyValueStoreBasicFunctionality(
    const KvStore& store,
    absl::FunctionRef<std::string(std::string key)> get_key);

inline void TestKeyValueStoreBasicFunctionality(const KvStore& store) {
  return TestKeyValueStoreBasicFunctionality(
      store, [](std::string key) { return key; });
}

/// Tests DeleteRange on `store`, which should be empty.
void TestKeyValueStoreDeleteRange(const KvStore& store);

/// Tests DeleteRange on `store`, which should be empty.
void TestKeyValueStoreDeletePrefix(const KvStore& store);

/// Tests DeleteRange on `store`, which should be empty.
void TestKeyValueStoreDeleteRangeToEnd(const KvStore& store);

/// Tests DeleteRange on `store`, which should be empty.
void TestKeyValueStoreDeleteRangeFromBeginning(const KvStore& store);

struct KeyValueStoreSpecRoundtripOptions {
  kvstore::SpecRequestOptions spec_request_options;
  JsonSerializationOptions json_serialization_options;
  // Checks that data persists after re-opening from the returned spec.
  bool check_data_persists = true;
};

/// Tests that calling `kvstore::Open` with `spec` returns a `KvStore` whose
/// `spec()` method returns `spec`, and that data persists when re-opening using
/// the same `spec` after closing.
void TestKeyValueStoreSpecRoundtrip(
    ::nlohmann::json json_spec,
    const KeyValueStoreSpecRoundtripOptions& options = {});

/// Tests that the KvStore spec constructed from `json_spec` corresponds to the
/// URL representation `url`.
void TestKeyValueStoreUrlRoundtrip(::nlohmann::json json_spec,
                                   std::string_view url);

/// Returns the contents of `kv_store` as an `std::map`.
Result<std::map<kvstore::Key, kvstore::Value>> GetMap(const KvStore& store);

/// Returns a GMock matcher for a `kvstore::ReadResult` or
/// `Result<kvstore::ReadResult>`.
template <typename ValueMatcher>
::testing::Matcher<Result<kvstore::ReadResult>> MatchesKvsReadResult(
    ValueMatcher value,
    ::testing::Matcher<StorageGeneration> generation = ::testing::_,
    ::testing::Matcher<absl::Time> time = ::testing::_) {
  using ReadResult = kvstore::ReadResult;
  ::testing::Matcher<kvstore::ReadResult::State> state_matcher;
  ::testing::Matcher<kvstore::Value> value_matcher;
  if constexpr (std::is_convertible_v<ValueMatcher,
                                      ::testing::Matcher<kvstore::Value>>) {
    value_matcher = ::testing::Matcher<kvstore::Value>(value);
    state_matcher = kvstore::ReadResult::kValue;
  } else {
    static_assert(
        std::is_convertible_v<ValueMatcher,
                              ::testing::Matcher<kvstore::ReadResult::State>>);
    value_matcher = absl::Cord();
    state_matcher = ::testing::Matcher<kvstore::ReadResult::State>(value);
  }
  return ::testing::Optional(::testing::AllOf(
      ::testing::Field("state", &ReadResult::state, state_matcher),
      ::testing::Field("value", &ReadResult::value, value_matcher),
      ::testing::Field("stamp", &ReadResult::stamp,
                       MatchesTimestampedStorageGeneration(generation, time))));
}

/// Overload that permits an `absl::Cord` matcher to be specified for the
/// `value`.
::testing::Matcher<Result<kvstore::ReadResult>> MatchesKvsReadResult(
    ::testing::Matcher<kvstore::Value> value,
    ::testing::Matcher<StorageGeneration> generation = ::testing::_,
    ::testing::Matcher<absl::Time> time = ::testing::_);

/// Returns a GMock matcher for a "not found" `kvstore::ReadResult`.
inline ::testing::Matcher<Result<kvstore::ReadResult>>
MatchesKvsReadResultNotFound(
    ::testing::Matcher<absl::Time> time = ::testing::_) {
  return MatchesKvsReadResult(kvstore::ReadResult::kMissing,
                              ::testing::Not(StorageGeneration::Unknown()),
                              time);
}

/// Returns a GMock matcher for an "aborted" `kvstore::ReadResult`.
inline ::testing::Matcher<Result<kvstore::ReadResult>>
MatchesKvsReadResultAborted(
    ::testing::Matcher<absl::Time> time = ::testing::_) {
  return MatchesKvsReadResult(kvstore::ReadResult::kUnspecified, ::testing::_,
                              time);
}

/// Mock KeyValueStore that simply records requests in a queue.
///
/// This can be used to test the behavior of code that interacts with a
/// `KeyValueStore`, and to inject errors to test error handling.
class MockKeyValueStore : public kvstore::Driver {
 public:
  using MockPtr = IntrusivePtr<MockKeyValueStore>;
  static MockKeyValueStore::MockPtr Make() {
    return MakeIntrusivePtr<MockKeyValueStore>();
  }

  struct ReadRequest {
    Promise<ReadResult> promise;
    Key key;
    ReadOptions options;
    void operator()(kvstore::DriverPtr target) const {
      LinkResult(promise, target->Read(key, options));
    }
  };

  struct WriteRequest {
    Promise<TimestampedStorageGeneration> promise;
    Key key;
    std::optional<Value> value;
    WriteOptions options;
    void operator()(kvstore::DriverPtr target) const {
      LinkResult(promise, target->Write(key, value, options));
    }
  };

  struct DeleteRangeRequest {
    Promise<void> promise;
    KeyRange range;
    void operator()(kvstore::DriverPtr target) const {
      LinkResult(promise, target->DeleteRange(range));
    }
  };

  struct ListRequest {
    ListOptions options;
    AnyFlowReceiver<Status, Key> receiver;
  };

  Future<ReadResult> Read(Key key, ReadOptions options) override;

  Future<TimestampedStorageGeneration> Write(Key key,
                                             std::optional<Value> value,
                                             WriteOptions options) override;

  void ListImpl(ListOptions options,
                AnyFlowReceiver<Status, Key> receiver) override;

  Future<void> DeleteRange(KeyRange range) override;

  void GarbageCollectionVisit(
      garbage_collection::GarbageCollectionVisitor& visitor) const final;

  ConcurrentQueue<ReadRequest> read_requests;
  ConcurrentQueue<WriteRequest> write_requests;
  ConcurrentQueue<ListRequest> list_requests;
  ConcurrentQueue<DeleteRangeRequest> delete_range_requests;
};

/// Context resource for a `MockKeyValueStore`.
///
/// To use a `MockKeyValueStore` where a KeyValueStore must be specified via a
/// JSON specification, specify:
///
///     {"driver": "mock_key_value_store"}
///
/// When opened, this will return a `KeyValueStore` that forwards to the
/// `MockKeyValueStore` specified in the `Context`.
///
/// For example:
///
///     auto context = Context::Default();
///
///     TENSORSTORE_ASSERT_OK_AND_ASSIGN(
///         auto mock_key_value_store_resource,
///         context.GetResource<
///             tensorstore::internal::MockKeyValueStoreResource>());
///     MockKeyValueStore *mock_key_value_store =
///         mock_key_value_store_resource->get();
///
///     auto store_future = tensorstore::Open(context, ::nlohmann::json{
///         {"driver", "n5"},
///         {"kvstore", {{"driver", "mock_key_value_store"}}},
///         ...
///     });
///
struct MockKeyValueStoreResource {
  static constexpr char id[] = "mock_key_value_store";
  using Resource = MockKeyValueStore::MockPtr;
};

}  // namespace internal
}  // namespace tensorstore

#endif  // TENSORSTORE_KVSTORE_TEST_UTIL_H_
