/*
 * Copyright (c) Intel Corporation.
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

#include "cachelib/allocator/tests/AllocatorMemoryTiersTest.h"

namespace facebook {
namespace cachelib {
namespace tests {

using LruAllocatorMemoryTiersTest = AllocatorMemoryTiersTest<LruAllocator>;

// TODO(MEMORY_TIER): add more tests with different eviction policies
TEST_F(LruAllocatorMemoryTiersTest, MultiTiersFromFileInvalid) { this->testMultiTiersFormFileInvalid(); }
TEST_F(LruAllocatorMemoryTiersTest, MultiTiersFromFileValid) { this->testMultiTiersFromFileValid(); }
TEST_F(LruAllocatorMemoryTiersTest, MultiTiersValidMixed) { this->testMultiTiersValidMixed(); }
TEST_F(LruAllocatorMemoryTiersTest, MultiTiersNumaBindingsSysVValid) { this->testMultiTiersNumaBindingsSysVValid(); }
TEST_F(LruAllocatorMemoryTiersTest, MultiTiersNumaBindingsPosixValid) { this->testMultiTiersNumaBindingsPosixValid(); }
TEST_F(LruAllocatorMemoryTiersTest, AddChainedItemUntilEviction ) { this->testAddChainedItemUntilEviction(); }

} // end of namespace tests
} // end of namespace cachelib
} // end of namespace facebook
