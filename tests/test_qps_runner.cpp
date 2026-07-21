#include "qps_runner.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

class ScopedQpsProfile {
public:
  explicit ScopedQpsProfile(const char* value) {
    if (const char* current = std::getenv("QPS_PROFILE"); current != nullptr) {
      previous_ = current;
    }
    setenv("QPS_PROFILE", value, 1);
  }

  ~ScopedQpsProfile() {
    if (previous_.has_value()) {
      setenv("QPS_PROFILE", previous_->c_str(), 1);
    } else {
      unsetenv("QPS_PROFILE");
    }
  }

  ScopedQpsProfile(const ScopedQpsProfile&) = delete;
  ScopedQpsProfile& operator=(const ScopedQpsProfile&) = delete;

private:
  std::optional<std::string> previous_;
};

TEST(QpsRunnerTest, SelectsSmokeProfile) {
  ScopedQpsProfile profile("smoke");
  const auto levels = hps::bench::default_qps_levels();

  ASSERT_EQ(levels.size(), 2U);
  EXPECT_EQ(levels[0].concurrency, 1);
  EXPECT_EQ(levels[0].duration_sec, 1);
  EXPECT_EQ(levels[1].concurrency, 4);
  EXPECT_EQ(levels[1].duration_sec, 1);
}

TEST(QpsRunnerTest, SelectsFullProfile) {
  ScopedQpsProfile profile("full");
  const auto levels = hps::bench::default_qps_levels();

  ASSERT_EQ(levels.size(), 7U);
  EXPECT_EQ(levels.front().concurrency, 1);
  EXPECT_EQ(levels.back().concurrency, 1024);
  EXPECT_EQ(levels.back().duration_sec, 15);
}

TEST(QpsRunnerTest, RejectsUnknownProfile) {
  ScopedQpsProfile profile("unknown");
  EXPECT_THROW((void)hps::bench::default_qps_levels(), std::invalid_argument);
}

TEST(QpsRunnerTest, AcceptsVoidAndBoolOperations) {
  int calls = 0;
  auto void_operation = [&calls](int) { ++calls; };
  auto success_operation = [](int) { return true; };
  auto failure_operation = [](int) { return false; };

  EXPECT_TRUE(hps::bench::detail::invoke_qps_operation(void_operation, 0));
  EXPECT_TRUE(hps::bench::detail::invoke_qps_operation(success_operation, 0));
  EXPECT_FALSE(hps::bench::detail::invoke_qps_operation(failure_operation, 0));
  EXPECT_EQ(calls, 1);
}

TEST(QpsRunnerTest, TracksFailuresAndBoundsLatencySamples) {
  std::atomic<int64_t> sequence{0};
  const auto sample = hps::bench::run_qps(1, 1, [&sequence](int) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    return sequence.fetch_add(1, std::memory_order_relaxed) % 2 == 0;
  });

  EXPECT_GT(sample.total_attempts, 0);
  EXPECT_EQ(sample.total_ops + sample.total_errors, sample.total_attempts);
  EXPECT_GT(sample.total_ops, 0);
  EXPECT_GT(sample.total_errors, 0);
  EXPECT_NEAR(sample.error_rate, 50.0, 0.1);
  EXPECT_EQ(sample.latency_samples,
            std::min(static_cast<std::size_t>(sample.total_ops / hps::bench::kSampleInterval),
                     hps::bench::kMaxLatencySamplesPerThread));
}

TEST(QpsRunnerTest, RejectsFailedBenchmarkStep) {
  const std::vector<hps::bench::QpsLevel> levels{{1, 1}};
  EXPECT_THROW(hps::bench::run_qps_steps("预期失败", levels, [](int) { return false; }), std::runtime_error);
}

} // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
