/**
 * @file stub_producer_metadata.hpp
 * @brief Producer metadata for test producers that carry no stream info.
 */

#ifndef TESTS__STUB_PRODUCER_METADATA_HPP_
#define TESTS__STUB_PRODUCER_METADATA_HPP_

#include "nlohmann/json.hpp"

#include "trossen_sdk/hw/producer_base.hpp"

/// @brief Metadata whose get_stream_info() is empty, for producers that are not recorded.
struct StubProducerMetadata : public trossen::hw::PolledProducer::ProducerMetadata {
  StubProducerMetadata() { id = "stub"; }
  nlohmann::ordered_json get_stream_info() const override { return {}; }
};

#endif  // TESTS__STUB_PRODUCER_METADATA_HPP_
