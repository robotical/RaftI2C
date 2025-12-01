/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Data aggregator test
//
// Rob Dobson 2024
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"

#include "PollDataAggregator.h"
#include "OfflineDataStore.h"

// static const char* MODULE_PREFIX = "test_i2c_data_agg";

TEST_CASE("Test PollDataAggregator Initialization", "[PollDataAggregator]") 
{
    PollDataAggregator aggregator;
    aggregator.init(10, 3);
    std::vector<uint8_t> data = {1, 2, 3};
    TEST_ASSERT_TRUE(aggregator.put(12345, data));
}

TEST_CASE("Test PollDataAggregator Put and Get", "[PollDataAggregator]") 
{
    PollDataAggregator aggregator;
    aggregator.init(10, 4);
    std::vector<uint8_t> data = {1, 2, 3, 4};
    TEST_ASSERT_TRUE(aggregator.put(12345, data));
    std::vector<uint8_t> dataOut;
    TEST_ASSERT_TRUE(aggregator.get(dataOut));
    TEST_ASSERT_TRUE(data == dataOut);
}

TEST_CASE("Test PollDataAggregator Put and Get Wrap", "[PollDataAggregator]") 
{
    PollDataAggregator aggregator;
    aggregator.init(3, 3);
    std::vector<uint8_t> data1 = {1, 2, 3};
    std::vector<uint8_t> data2 = {4, 5, 6};
    std::vector<uint8_t> data3 = {7, 8, 9};
    std::vector<uint8_t> data4 = {10, 11, 12};
    uint32_t timeVal = 12345;
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data1));
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data2));
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data3));
    std::vector<uint8_t> dataOut;
    TEST_ASSERT_TRUE(aggregator.get(dataOut));
    TEST_ASSERT_TRUE(data1 == dataOut);
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data4));
    TEST_ASSERT_TRUE(aggregator.get(dataOut));
    TEST_ASSERT_TRUE(data2 == dataOut);
    TEST_ASSERT_TRUE(aggregator.get(dataOut));
    TEST_ASSERT_TRUE(data3 == dataOut);
    TEST_ASSERT_TRUE(aggregator.get(dataOut));
    TEST_ASSERT_TRUE(data4 == dataOut);
}

TEST_CASE("Test PollDataAggregator Put and Get Empty", "[PollDataAggregator]") 
{
    PollDataAggregator aggregator;
    aggregator.init(10, 3);
    std::vector<uint8_t> dataOut;
    TEST_ASSERT_FALSE(aggregator.get(dataOut));
}

TEST_CASE("Test PollDataAggregator Put and Get Full", "[PollDataAggregator]") 
{
    PollDataAggregator aggregator;
    aggregator.init(3,3);
    std::vector<uint8_t> data1 = {1, 2, 3};
    std::vector<uint8_t> data2 = {4, 5, 6};
    std::vector<uint8_t> data3 = {7, 8, 9};
    std::vector<uint8_t> data4 = {10, 11, 12};
    uint32_t timeVal = 12345;
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data1));
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data2));
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data3));
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data4));
    TEST_ASSERT_TRUE(aggregator.count() == 3);
    std::vector<uint8_t> dataOut;
    TEST_ASSERT_TRUE(aggregator.get(dataOut));
    TEST_ASSERT_TRUE(data2 == dataOut);
    TEST_ASSERT_TRUE(aggregator.count() == 2);
    TEST_ASSERT_TRUE(aggregator.get(dataOut));
    TEST_ASSERT_TRUE(data3 == dataOut);
    TEST_ASSERT_TRUE(aggregator.count() == 1);
    TEST_ASSERT_TRUE(aggregator.get(dataOut));
    TEST_ASSERT_TRUE(data4 == dataOut);
    TEST_ASSERT_TRUE(aggregator.count() == 0);
}

TEST_CASE("Test PollDataAggregator Put and Get Full Wrap", "[PollDataAggregator]") 
{
    PollDataAggregator aggregator;
    aggregator.init(3,3);
    std::vector<uint8_t> data1 = {1, 2, 3};
    std::vector<uint8_t> data2 = {4, 5, 6};
    std::vector<uint8_t> data3 = {7, 8, 9};
    std::vector<uint8_t> data4 = {10, 11, 12};
    std::vector<uint8_t> data5 = {13, 14, 15};
    std::vector<uint8_t> data6 = {19, 20, 21};
    uint32_t timeVal = 12345;
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data1));
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data2));
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data3));
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data4));
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data5));
    std::vector<uint8_t> dataOut;
    TEST_ASSERT_TRUE(aggregator.get(dataOut));
    TEST_ASSERT_TRUE(data3 == dataOut);
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data6));
    TEST_ASSERT_TRUE(aggregator.get(dataOut));
    TEST_ASSERT_TRUE(data4 == dataOut);
    TEST_ASSERT_TRUE(aggregator.get(dataOut));
    TEST_ASSERT_TRUE(data5 == dataOut);
    TEST_ASSERT_TRUE(aggregator.get(dataOut));
    TEST_ASSERT_TRUE(data6 == dataOut);
    TEST_ASSERT_FALSE(aggregator.get(dataOut));
}

TEST_CASE("Test PollDataAggregator Put and Get Multiple", "[PollDataAggregator]") 
{
    PollDataAggregator aggregator;
    aggregator.init(4,4);
    std::vector<uint8_t> data1 = {1, 2, 3, 4};
    std::vector<uint8_t> data2 = {5, 6, 7, 8};
    std::vector<uint8_t> data3 = {9, 10, 11, 12};
    std::vector<uint8_t> data4 = {13, 14, 15, 16};
    std::vector<uint8_t> data5 = {17, 18, 19, 20};
    std::vector<uint8_t> data6 = {21, 22, 23, 24};
    std::vector<uint8_t> data7 = {25, 26, 27, 28};
    uint32_t timeVal = 12345;
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data1));
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data2));
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data3));
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data4));
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data5));
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data6));
    std::vector<uint8_t> dataOut;
    uint32_t elemSize = 0;
    std::vector<uint8_t> dataTest3And4 = data3;
    dataTest3And4.insert(dataTest3And4.end(), data4.begin(), data4.end());
    TEST_ASSERT_TRUE(aggregator.get(dataOut, elemSize, 2) == 2);
    TEST_ASSERT_TRUE(dataTest3And4 == dataOut);
    TEST_ASSERT_TRUE(elemSize == 4);
    TEST_ASSERT_TRUE(aggregator.put(timeVal++, data7));
    std::vector<uint8_t> dataTest5to7 = data5;
    dataTest5to7.insert(dataTest5to7.end(), data6.begin(), data6.end());
    dataTest5to7.insert(dataTest5to7.end(), data7.begin(), data7.end());
    TEST_ASSERT_TRUE(aggregator.get(dataOut, elemSize, 5) == 3);
    TEST_ASSERT_TRUE(dataTest5to7 == dataOut);
    TEST_ASSERT_FALSE(aggregator.get(dataOut));
}

TEST_CASE("OfflineDataStore drops oldest and tracks drops", "[OfflineDataStore]")
{
    OfflineDataStore store;
    store.init(2, 3, 2, 1000);
    std::vector<uint8_t> d1 = {0x00, 0x01, 0x02};
    std::vector<uint8_t> d2 = {0x00, 0x03, 0x04};
    std::vector<uint8_t> d3 = {0x00, 0x05, 0x06};
    TEST_ASSERT_TRUE(store.put(1000, 1, d1));
    TEST_ASSERT_TRUE(store.put(2000, 2, d2));
    TEST_ASSERT_TRUE(store.put(3000, 3, d3));
    OfflineDataStats stats = store.getStats();
    TEST_ASSERT_EQUAL_UINT32(2, stats.depth);
    TEST_ASSERT_EQUAL_UINT32(1, stats.drops);
    std::vector<uint8_t> out;
    uint32_t respSize = 0;
    std::vector<OfflineDataMeta> metas;
    TEST_ASSERT_EQUAL_UINT32(2, store.get(out, respSize, 0, metas));
    std::vector<uint8_t> expected = d2;
    expected.insert(expected.end(), d3.begin(), d3.end());
    TEST_ASSERT_EQUAL_UINT32(3, metas.back().seq);
    TEST_ASSERT_EQUAL_UINT32(3, respSize);
    TEST_ASSERT_EQUAL_UINT32(expected.size(), out.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), out.data(), expected.size());
}

TEST_CASE("OfflineDataStore records timestamp wrap", "[OfflineDataStore]")
{
    OfflineDataStore store;
    store.init(3, 2, 2, 1000);
    std::vector<uint8_t> d1 = {0xff, 0xf0}; // large ts close to wrap
    std::vector<uint8_t> d2 = {0x00, 0x05}; // wrap to small value
    TEST_ASSERT_TRUE(store.put(10000 * 1000ULL, 10, d1));
    TEST_ASSERT_TRUE(store.put(20000 * 1000ULL, 11, d2));
    OfflineDataStats stats = store.getStats();
    TEST_ASSERT_EQUAL_UINT32(1, stats.tsWrapCount);
    std::vector<uint8_t> out;
    uint32_t respSize = 0;
    std::vector<OfflineDataMeta> metas;
    TEST_ASSERT_EQUAL_UINT32(2, store.get(out, respSize, 0, metas));
    TEST_ASSERT_TRUE(metas.size() == 2);
    TEST_ASSERT_TRUE(metas.back().tsBaseMs >= (1ULL << 16));
}
