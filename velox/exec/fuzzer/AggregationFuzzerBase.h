/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "velox/common/file/FileSystems.h"
#include "velox/connectors/hive/HiveConnector.h"
#include "velox/dwio/dwrf/RegisterDwrfReader.h"
#include "velox/dwio/dwrf/RegisterDwrfWriter.h"
#include "velox/exec/Aggregate.h"
#include "velox/exec/Split.h"
#include "velox/exec/fuzzer/FuzzerUtil.h"
#include "velox/exec/fuzzer/InputGenerator.h"
#include "velox/exec/fuzzer/ReferenceQueryRunner.h"
#include "velox/exec/fuzzer/ResultVerifier.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/expression/fuzzer/FuzzerToolkit.h"
#include "velox/vector/fuzzer/VectorFuzzer.h"
#include "velox/vector/tests/utils/VectorMaker.h"

DECLARE_int32(steps);

DECLARE_int32(duration_sec);

DECLARE_int32(batch_size);

DECLARE_int32(num_batches);

DECLARE_int32(max_num_varargs);

DECLARE_double(null_ratio);

DECLARE_string(repro_persist_path);

DECLARE_bool(persist_and_run_once);

DECLARE_bool(log_signature_stats);

namespace facebook::velox::exec::test {

using facebook::velox::fuzzer::CallableSignature;
using facebook::velox::fuzzer::SignatureTemplate;

constexpr const std::string_view kPlanNodeFileName = "plan_nodes";

class AggregationFuzzerBase {
 public:
  AggregationFuzzerBase(
      size_t initialSeed,
      const std::unordered_map<std::string, std::shared_ptr<ResultVerifier>>&
          customVerificationFunctions,
      const std::unordered_map<std::string, std::shared_ptr<InputGenerator>>&
          customInputGenerators,
      VectorFuzzer::Options::TimestampPrecision timestampPrecision,
      const std::unordered_map<std::string, std::string>& queryConfigs,
      const std::unordered_map<std::string, std::string>& hiveConfigs,
      bool orderableGroupKeys,
      std::unique_ptr<ReferenceQueryRunner> referenceQueryRunner,
      std::optional<VectorFuzzer::Options> fuzzerOptions = std::nullopt)
      : customVerificationFunctions_{customVerificationFunctions},
        customInputGenerators_{customInputGenerators},
        queryConfigs_{queryConfigs},
        orderableGroupKeys_{orderableGroupKeys},
        persistAndRunOnce_{FLAGS_persist_and_run_once},
        reproPersistPath_{FLAGS_repro_persist_path},
        referenceQueryRunner_{std::move(referenceQueryRunner)},
        vectorFuzzer_{
            fuzzerOptions.has_value() ? fuzzerOptions.value()
                                      : getFuzzerOptions(timestampPrecision),
            pool_.get()} {
    filesystems::registerLocalFileSystem();
    connector::registerConnectorFactory(
        std::make_shared<connector::hive::HiveConnectorFactory>());
    registerHiveConnector(hiveConfigs);
    dwrf::registerDwrfReaderFactory();
    dwrf::registerDwrfWriterFactory();

    for (const auto& type : referenceQueryRunner_->supportedScalarTypes()) {
      if (!type->isReal() && !type->isDouble()) {
        supportedKeyTypes_.push_back(type);
      }
    }

    seed(initialSeed);
  }

  struct PlanWithSplits {
    core::PlanNodePtr plan;
    std::vector<exec::Split> splits;
  };

  struct FunctionsStats {
    size_t numFunctions = 0;
    size_t numSignatures = 0;
    size_t numSupportedFunctions = 0;
    size_t numSupportedSignatures = 0;
  };

  struct SignatureStats {
    /// Number of times a signature was chosen.
    size_t numRuns{0};

    /// Number of times generated query plan failed.
    size_t numFailed{0};
  };

  static VectorFuzzer::Options getFuzzerOptions(
      VectorFuzzer::Options::TimestampPrecision timestampPrecision) {
    VectorFuzzer::Options opts;
    opts.vectorSize = FLAGS_batch_size;
    opts.stringVariableLength = true;
    opts.stringLength = 4'000;
    opts.nullRatio = FLAGS_null_ratio;
    opts.timestampPrecision = timestampPrecision;
    return opts;
  }

 protected:
  struct Stats {
    // Names of functions that were tested.
    std::unordered_set<std::string> functionNames;

    // Number of iterations using aggregations over sorted inputs.
    size_t numSortedInputs{0};

    // Number of iterations where results were verified against reference DB,
    size_t numVerified{0};

    // Number of iterations where results verification was skipped because
    // function results are non-determinisic.
    size_t numVerificationSkipped{0};

    // Number of iterations where results verification was skipped because
    // reference DB doesn't support the query.
    size_t numReferenceQueryNotSupported{0};

    // Number of iterations where results verification was skipped because
    // reference DB failed to execute the query.
    size_t numReferenceQueryFailed{0};

    // Number of iterations where aggregation failed.
    size_t numFailed{0};

    void print(size_t numIterations) const;

    void updateReferenceQueryStats(ReferenceQueryErrorCode errorCode);
  };

  int32_t randInt(int32_t min, int32_t max);

  bool addSignature(
      const std::string& name,
      const FunctionSignaturePtr& signature);

  void addAggregationSignatures(
      const AggregateFunctionSignatureMap& signatureMap);

  std::shared_ptr<InputGenerator> findInputGenerator(
      const CallableSignature& signature);

  PlanWithSplits deserialize(const folly::dynamic& obj);

  void seed(size_t seed) {
    currentSeed_ = seed;
    vectorFuzzer_.reSeed(seed);
    rng_.seed(currentSeed_);
  }

  void reSeed() {
    seed(rng_());
  }

  // Generates at least one and up to 5 scalar columns to be used as grouping,
  // partition or sorting keys.
  // Column names are generated using template '<prefix>N', where N is
  // zero-based ordinal number of the column.
  std::vector<std::string> generateKeys(
      const std::string& prefix,
      std::vector<std::string>& names,
      std::vector<TypePtr>& types);

  // Similar to generateKeys, but restricts types to orderable types (i.e. no
  // maps). For k-RANGE frame bounds, rangeFrame must be set to true so only
  // one sorting key is generated.
  std::vector<std::string> generateSortingKeys(
      const std::string& prefix,
      std::vector<std::string>& names,
      std::vector<TypePtr>& types,
      bool rangeFrame = false,
      const std::vector<TypePtr>& scalarTypes = defaultScalarTypes(),
      std::optional<uint32_t> numKeys = std::nullopt);

  std::pair<CallableSignature, SignatureStats&> pickSignature();

  std::vector<RowVectorPtr> generateInputData(
      std::vector<std::string> names,
      std::vector<TypePtr> types,
      const std::optional<CallableSignature>& signature);

  /// Generate a RowVector of the given types of children with an additional
  /// child named "row_number" of INTEGER row numbers that differentiates every
  /// row. Row numbers start from 0. This additional input vector is needed for
  /// result verification of window aggregations.
  /// @param windowFrameBounds Names of frame bound columns of a window
  /// operation. These columns are fuzzed without NULLs.
  std::vector<RowVectorPtr> generateInputDataWithRowNumber(
      std::vector<std::string> names,
      std::vector<TypePtr> types,
      const std::vector<std::string>& partitionKeys,
      const std::vector<std::string>& windowFrameBounds,
      const std::vector<std::string>& sortingKeys,
      const CallableSignature& signature);

  velox::fuzzer::ResultOrError execute(
      const core::PlanNodePtr& plan,
      const std::vector<exec::Split>& splits = {},
      bool injectSpill = false,
      bool abandonPartial = false,
      int32_t maxDrivers = 2);

  void compare(
      const velox::fuzzer::ResultOrError& actual,
      bool customVerification,
      const std::vector<std::shared_ptr<ResultVerifier>>& customVerifiers,
      const velox::fuzzer::ResultOrError& expected);

  /// Returns false if the type or its children are unsupported.
  /// Currently returns false if type is Date,IntervalDayTime or Unknown.
  /// @param type
  /// @return bool
  bool isSupportedType(const TypePtr& type) const;

  // @param customVerification If false, results are compared as is. Otherwise,
  // only row counts are compared.
  // @param customVerifiers Custom verifier for each aggregate function. These
  // can be null. If not null and customVerification is true, custom verifier is
  // used to further verify the results.
  void testPlan(
      const PlanWithSplits& planWithSplits,
      bool injectSpill,
      bool abandonPartial,
      bool customVerification,
      const std::vector<std::shared_ptr<ResultVerifier>>& customVerifiers,
      const velox::fuzzer::ResultOrError& expected,
      int32_t maxDrivers = 2);

  void printSignatureStats();

  const std::unordered_map<std::string, std::shared_ptr<ResultVerifier>>
      customVerificationFunctions_;
  const std::unordered_map<std::string, std::shared_ptr<InputGenerator>>
      customInputGenerators_;
  const std::unordered_map<std::string, std::string> queryConfigs_;

  // Whether group keys must be orderable or be just comparable.
  bool orderableGroupKeys_;
  const bool persistAndRunOnce_;
  const std::string reproPersistPath_;

  std::unique_ptr<ReferenceQueryRunner> referenceQueryRunner_;

  std::vector<CallableSignature> signatures_;
  std::vector<SignatureTemplate> signatureTemplates_;

  FunctionsStats functionsStats;

  // Stats for 'signatures_' and 'signatureTemplates_'. Stats for 'signatures_'
  // come before stats for 'signatureTemplates_'.
  std::vector<SignatureStats> signatureStats_;

  FuzzerGenerator rng_;
  size_t currentSeed_{0};

  std::shared_ptr<memory::MemoryPool> rootPool_{
      memory::memoryManager()->addRootPool()};
  std::shared_ptr<memory::MemoryPool> pool_{rootPool_->addLeafChild("leaf")};
  std::shared_ptr<memory::MemoryPool> writerPool_{
      rootPool_->addAggregateChild("aggregationFuzzerWriter")};
  VectorFuzzer vectorFuzzer_;
  std::vector<TypePtr> supportedKeyTypes_;
};

// Returns true if the elapsed time is greater than or equal to
// FLAGS_duration_sec. If FLAGS_duration_sec is 0, returns true if the
// iterations is greater than or equal to FLAGS_steps.
template <typename T>
bool isDone(size_t i, T startTime) {
  if (FLAGS_duration_sec > 0) {
    std::chrono::duration<double> elapsed =
        std::chrono::system_clock::now() - startTime;
    return elapsed.count() >= FLAGS_duration_sec;
  }
  return i >= FLAGS_steps;
}

// Prints statistics about supported and unsupported function signatures.
void printStats(const AggregationFuzzerBase::FunctionsStats& stats);

// Prints (n / total) in percentage format.
std::string printPercentageStat(size_t n, size_t total);

// Makes an aggregation call string for the given function name and arguments.
std::string makeFunctionCall(
    const std::string& name,
    const std::vector<std::string>& argNames,
    bool sortedInputs = false,
    bool distinctInputs = false,
    bool ignoreNulls = false);

// Returns a list of column names from c0 to cn.
std::vector<std::string> makeNames(size_t n);

// Persists plans to files under basePath.
void persistReproInfo(
    const std::vector<AggregationFuzzerBase::PlanWithSplits>& plans,
    const std::string& basePath);

// Returns the function name used in a WindowNode. The input `node` should be a
// pointer to a WindowNode.
std::vector<std::string> retrieveWindowFunctionName(
    const core::PlanNodePtr& node);

} // namespace facebook::velox::exec::test
