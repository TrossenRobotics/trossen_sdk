/**
 * @file test_null_backend.cpp
 * @brief Unit tests for NullBackend
 */

#include <vector>

#include "gtest/gtest.h"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/io/backends/null/null_backend.hpp"

using trossen::data::ImageRecord;
using trossen::data::JointStateRecord;
using trossen::data::RecordBase;
using trossen::io::backends::NullBackend;


// TODO (shantanuparab-tr): Update the unit test specifically ones related to configuration as
// they make no sense after the recent changes to configuration management.
// Fixes are done to satisfy compilation but the tests themselves need to be reviewed.

// TODO(lukeschmitt-tr): Add multithreading tests

// Test NullBackend construction
TEST(NullBackendTest, Construction) {
  NullBackend backend;
  EXPECT_EQ(backend.count(), 0);
}

// Test NullBackend construction with custom URI
TEST(NullBackendTest, ConstructionWithURI) {
  NullBackend backend;
  EXPECT_EQ(backend.count(), 0);
}

// Test open operation
TEST(NullBackendTest, Open) {
  NullBackend backend;
  EXPECT_TRUE(backend.open());
}

// Test write single record
TEST(NullBackendTest, WriteSingleRecord) {
  NullBackend backend;
  backend.open();

  JointStateRecord record;
  record.seq = 1;
  record.id = "test_joint";
  record.positions = {1.0f, 2.0f, 3.0f};

  backend.write(record);

  EXPECT_EQ(backend.count(), 1);
}

// Test write multiple records
TEST(NullBackendTest, WriteMultipleRecords) {
  NullBackend backend;
  backend.open();

  JointStateRecord record1;
  record1.seq = 1;

  JointStateRecord record2;
  record2.seq = 2;

  JointStateRecord record3;
  record3.seq = 3;

  backend.write(record1);
  backend.write(record2);
  backend.write(record3);

  EXPECT_EQ(backend.count(), 3);
}

// Test write_batch with multiple records
TEST(NullBackendTest, WriteBatch) {
  NullBackend backend;
  backend.open();

  JointStateRecord record1;
  record1.seq = 1;

  JointStateRecord record2;
  record2.seq = 2;

  ImageRecord record3;
  record3.seq = 3;

  std::vector<const RecordBase*> batch = {&record1, &record2, &record3};
  backend.write_batch(batch);

  EXPECT_EQ(backend.count(), 3);
}

// Test write_batch with empty batch
TEST(NullBackendTest, WriteBatchEmpty) {
  NullBackend backend;
  backend.open();

  std::vector<const RecordBase*> batch;
  backend.write_batch(batch);

  EXPECT_EQ(backend.count(), 0);
}

// Test write batch with single record
TEST(NullBackendTest, WriteBatchSingleRecord) {
  NullBackend backend;
  backend.open();

  JointStateRecord record;
  record.seq = 42;

  std::vector<const RecordBase*> batch = {&record};
  backend.write_batch(batch);

  EXPECT_EQ(backend.count(), 1);
}

// Test mixed write and write_batch
TEST(NullBackendTest, MixedWriteOperations) {
  NullBackend backend;
  backend.open();

  JointStateRecord record1;
  backend.write(record1);  // count = 1

  JointStateRecord record2;
  JointStateRecord record3;
  std::vector<const RecordBase*> batch = {&record2, &record3};
  backend.write_batch(batch);  // count = 3

  ImageRecord record4;
  backend.write(record4);  // count = 4

  EXPECT_EQ(backend.count(), 4);
}

// Test flush (no-op but should not crash)
TEST(NullBackendTest, Flush) {
  NullBackend backend;
  backend.open();

  JointStateRecord record;
  backend.write(record);

  EXPECT_NO_THROW(backend.flush());
  EXPECT_EQ(backend.count(), 1);
}

// Test close
TEST(NullBackendTest, Close) {
  NullBackend backend;
  backend.open();

  JointStateRecord record;
  backend.write(record);

  EXPECT_NO_THROW(backend.close());
  EXPECT_EQ(backend.count(), 1);  // Count persists after close
}

// Test write after close (should still work for null backend)
TEST(NullBackendTest, WriteAfterClose) {
  NullBackend backend;
  backend.open();
  backend.close();

  JointStateRecord record;
  backend.write(record);

  EXPECT_EQ(backend.count(), 1);
}

// Test large batch
TEST(NullBackendTest, LargeBatch) {
  NullBackend backend;
  backend.open();

  std::vector<JointStateRecord> records(1000);
  std::vector<const RecordBase*> batch;
  for (size_t i = 0; i < records.size(); ++i) {
    records[i].seq = i;
    batch.push_back(&records[i]);
  }

  backend.write_batch(batch);

  EXPECT_EQ(backend.count(), 1000);
}

// Test counter accuracy with many operations
TEST(NullBackendTest, CounterAccuracy) {
  NullBackend backend;
  backend.open();

  // Write 10 individual records
  for (int i = 0; i < 10; ++i) {
    JointStateRecord record;
    backend.write(record);
  }
  EXPECT_EQ(backend.count(), 10);

  // Write batch of 5
  std::vector<JointStateRecord> records(5);
  std::vector<const RecordBase*> batch;
  for (auto& rec : records) {
    batch.push_back(&rec);
  }
  backend.write_batch(batch);
  EXPECT_EQ(backend.count(), 15);

  // Write 3 more individual
  for (int i = 0; i < 3; ++i) {
    JointStateRecord record;
    backend.write(record);
  }
  EXPECT_EQ(backend.count(), 18);
}

// Test writing different record types
TEST(NullBackendTest, DifferentRecordTypes) {
  NullBackend backend;
  backend.open();

  JointStateRecord joint_record;
  joint_record.positions = {1.0f, 2.0f};
  backend.write(joint_record);

  ImageRecord image_record;
  image_record.width = 640;
  image_record.height = 480;
  backend.write(image_record);

  EXPECT_EQ(backend.count(), 2);
}

// Test that backend discards data (doesn't store it)
TEST(NullBackendTest, DataIsDiscarded) {
  NullBackend backend;
  backend.open();

  // Create a record with large data
  JointStateRecord record;
  record.positions.resize(10000, 1.0f);
  record.velocities.resize(10000, 2.0f);
  record.efforts.resize(10000, 3.0f);

  // Write should succeed and count should increment
  backend.write(record);
  EXPECT_EQ(backend.count(), 1);

  // But the backend doesn't actually store the data
  // (This is implicit in the null backend design)
}

// Test URI is stored (inherited from Backend base class)
TEST(NullBackendTest, URIStorage) {
  NullBackend backend1;

  NullBackend backend2;

  // The base class stores the URI, though we can't directly test it without accessing
  // protected/private members. This test just ensures construction with different URIs doesn't
  // crash.
  EXPECT_TRUE(backend1.open());
  EXPECT_TRUE(backend2.open());
}
