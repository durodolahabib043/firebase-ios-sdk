/*
 * Copyright 2018 Google
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Firestore/core/test/firebase/firestore/util/executor_test.h"

#include <chrono>  // NOLINT(build/c++11)
#include <cstdlib>
#include <future>  // NOLINT(build/c++11)
#include <string>
#include <thread>  // NOLINT(build/c++11)

#include "Firestore/core/src/firebase/firestore/util/executor_std.h"
#include "Firestore/core/test/firebase/firestore/util/async_tests_util.h"
#include "gtest/gtest.h"

namespace firebase {
namespace firestore {
namespace util {

namespace chr = std::chrono;
using internal::Schedule;

class ScheduleTest : public ::testing::Test {
 public:
  ScheduleTest() : start_time{now()} {
  }

  using ScheduleT = Schedule<int>;

  int TryPop() {
    int read = -1;
    schedule.PopIfDue(&read);
    return read;
  }

  ScheduleT schedule;
  ScheduleT::TimePoint start_time;
  int out = 0;
};

// Schedule tests

TEST_F(ScheduleTest, PopIfDue_Immediate) {
  EXPECT_FALSE(schedule.PopIfDue(nullptr));

  // Push values in a deliberately non-sorted order.
  schedule.Push(3, start_time);
  schedule.Push(1, start_time);
  schedule.Push(2, start_time);
  EXPECT_FALSE(schedule.empty());
  EXPECT_EQ(schedule.size(), 3u);

  EXPECT_EQ(TryPop(), 3);
  EXPECT_EQ(TryPop(), 1);
  EXPECT_EQ(TryPop(), 2);
  EXPECT_FALSE(schedule.PopIfDue(nullptr));
  EXPECT_TRUE(schedule.empty());
  EXPECT_EQ(schedule.size(), 0u);
}

TEST_F(ScheduleTest, PopIfDue_Delayed) {
  schedule.Push(1, start_time + chr::milliseconds(5));
  schedule.Push(2, start_time + chr::milliseconds(3));
  schedule.Push(3, start_time + chr::milliseconds(1));

  EXPECT_FALSE(schedule.PopIfDue(&out));
  std::this_thread::sleep_for(chr::milliseconds(5));

  EXPECT_EQ(TryPop(), 3);
  EXPECT_EQ(TryPop(), 2);
  EXPECT_EQ(TryPop(), 1);
  EXPECT_TRUE(schedule.empty());
}

TEST_F(ScheduleTest, PopBlocking) {
  schedule.Push(1, start_time + chr::milliseconds(3));
  EXPECT_FALSE(schedule.PopIfDue(&out));

  schedule.PopBlocking(&out);
  EXPECT_EQ(out, 1);
  EXPECT_GE(now(), start_time + chr::milliseconds(3));
  EXPECT_TRUE(schedule.empty());
}

TEST_F(ScheduleTest, RemoveIf) {
  schedule.Push(1, start_time);
  schedule.Push(2, now() + chr::minutes(1));
  EXPECT_TRUE(schedule.RemoveIf(&out, [](const int v) { return v == 1; }));
  EXPECT_EQ(out, 1);
  EXPECT_TRUE(schedule.RemoveIf(&out, [](const int v) { return v == 2; }));
  EXPECT_EQ(out, 2);
  EXPECT_TRUE(schedule.empty());
}

TEST_F(ScheduleTest, Ordering) {
  schedule.Push(11, start_time + chr::milliseconds(5));
  schedule.Push(1, start_time);
  schedule.Push(2, start_time);
  schedule.Push(9, start_time + chr::milliseconds(2));
  schedule.Push(3, start_time);
  schedule.Push(10, start_time + chr::milliseconds(3));
  schedule.Push(12, start_time + chr::milliseconds(5));
  schedule.Push(4, start_time);
  schedule.Push(5, start_time);
  schedule.Push(6, start_time);
  schedule.Push(8, start_time + chr::milliseconds(1));
  schedule.Push(7, start_time);

  std::vector<int> values;
  const auto append = [&] {
    values.push_back(0);
    schedule.PopBlocking(&values.back());
  };
  while (!schedule.empty()) {
    append();
  }
  const std::vector<int> expected = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  EXPECT_EQ(values, expected);
}

TEST_F(ScheduleTest, AddingEntryUnblocksEmptyQueue) {
  const auto future = std::async(std::launch::async, [&] {
    ASSERT_FALSE(schedule.PopIfDue(&out));
    schedule.PopBlocking(&out);
    EXPECT_EQ(out, 1);
  });

  std::this_thread::sleep_for(chr::milliseconds(5));
  schedule.Push(1, start_time);
  ABORT_ON_TIMEOUT(future);
}

TEST_F(ScheduleTest, PopBlockingUnblocksOnNewPastDueEntries) {
  const auto far_away = start_time + chr::seconds(10);
  schedule.Push(5, far_away);

  const auto future = std::async(std::launch::async, [&] {
    ASSERT_FALSE(schedule.PopIfDue(&out));
    schedule.PopBlocking(&out);
    EXPECT_EQ(out, 3);
  });

  std::this_thread::sleep_for(chr::milliseconds(5));
  schedule.Push(3, start_time);
  ABORT_ON_TIMEOUT(future);
}

TEST_F(ScheduleTest, PopBlockingAdjustsWaitTimeOnNewSoonerEntries) {
  const auto far_away = start_time + chr::seconds(10);
  schedule.Push(5, far_away);

  const auto future = std::async(std::launch::async, [&] {
    ASSERT_FALSE(schedule.PopIfDue(&out));
    schedule.PopBlocking(&out);
    EXPECT_EQ(out, 3);
    // Make sure schedule hasn't been waiting longer than necessary.
    EXPECT_LE(now(), far_away);
  });

  std::this_thread::sleep_for(chr::milliseconds(5));
  schedule.Push(3, start_time + chr::milliseconds(100));
  ABORT_ON_TIMEOUT(future);
}

TEST_F(ScheduleTest, PopBlockingCanReadjustTimeIfSeveralElementsAreAdded) {
  const auto far_away = start_time + chr::seconds(5);
  const auto very_far_away = start_time + chr::seconds(10);
  schedule.Push(3, very_far_away);

  const auto future = std::async(std::launch::async, [&] {
    ASSERT_FALSE(schedule.PopIfDue(&out));
    schedule.PopBlocking(&out);
    EXPECT_EQ(out, 1);
    EXPECT_LE(now(), far_away);
  });

  std::this_thread::sleep_for(chr::milliseconds(5));
  schedule.Push(2, far_away);
  std::this_thread::sleep_for(chr::milliseconds(1));
  schedule.Push(1, start_time + chr::milliseconds(100));
  ABORT_ON_TIMEOUT(future);
}

TEST_F(ScheduleTest, PopBlockingNoticesRemovals) {
  const auto future = std::async(std::launch::async, [&] {
    schedule.Push(1, start_time + chr::milliseconds(50));
    schedule.Push(2, start_time + chr::milliseconds(100));
    ASSERT_FALSE(schedule.PopIfDue(&out));
    schedule.PopBlocking(&out);
    EXPECT_EQ(out, 2);
  });

  while (schedule.empty()) {
    std::this_thread::sleep_for(chr::milliseconds(1));
  }
  const bool removed =
      schedule.RemoveIf(nullptr, [](const int v) { return v == 1; });
  EXPECT_TRUE(removed);
  ABORT_ON_TIMEOUT(future);
}

TEST_F(ScheduleTest, PopBlockingIsNotAffectedByIrrelevantRemovals) {
  const auto future = std::async(std::launch::async, [&] {
    schedule.Push(1, start_time + chr::milliseconds(50));
    schedule.Push(2, start_time + chr::seconds(10));
    ASSERT_FALSE(schedule.PopIfDue(&out));
    schedule.PopBlocking(&out);
    EXPECT_EQ(out, 1);
  });

  while (schedule.empty()) {
    std::this_thread::sleep_for(chr::milliseconds(1));
  }
  const bool removed =
      schedule.RemoveIf(nullptr, [](const int v) { return v == 2; });
  EXPECT_TRUE(removed);
  ABORT_ON_TIMEOUT(future);
}

// ExecutorStd tests

using internal::Executor;

namespace {

inline std::unique_ptr<Executor> ExecutorFactory() {
  return std::unique_ptr<Executor>(new internal::ExecutorStd());
}

}  // namespace

INSTANTIATE_TEST_CASE_P(ExecutorTestStd,
                        ExecutorTest,
                        ::testing::Values(ExecutorFactory));

}  // namespace util
}  // namespace firestore
}  // namespace firebase
