/**
 * @file types.hpp
 * @brief Common type definitions for Trossen SDK.
 */

#ifndef TROSSEN_SDK__TYPES_HPP
#define TROSSEN_SDK__TYPES_HPP

#include <memory>
#include <vector>

#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen
{

using ProducerMetadataList = std::vector<std::shared_ptr<hw::PolledProducer::ProducerMetadata>>;

}  // namespace trossen

#endif  // TROSSEN_SDK__TYPES_HPP
