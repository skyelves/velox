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

#include <gflags/gflags.h>
#include "velox/serializers/PrestoSerializer.h"

#include "velox/exec/tests/JoinSpillInputBenchmarkBase.h"

using namespace facebook::velox;

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  memory::MemoryManager::initialize(memory::MemoryManager::Options{});
  serializer::presto::PrestoVectorSerde::registerVectorSerde();
  filesystems::registerLocalFileSystem();
  auto test = std::make_unique<exec::test::JoinSpillInputBenchmarkBase>();
  test->setUp();
  test->run();
  test->printStats();
  test->cleanup();
  return 0;
}
