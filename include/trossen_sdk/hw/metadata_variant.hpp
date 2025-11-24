// file: include/trossen_sdk/metadata/producer_metadata_variant.hpp

#pragma once

#include <variant>
#include <memory>

#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/hw/arm/arm_producer.hpp"
#include "trossen_sdk/hw/arm/mock_joint_producer.hpp"
#include "trossen_sdk/hw/arm/teleop_arm_producer.hpp"
#include "trossen_sdk/hw/arm/teleop_mock_joint_producer.hpp"
#include "trossen_sdk/hw/camera/opencv_producer.hpp"
#include "trossen_sdk/hw/camera/mock_producer.hpp"

namespace trossen::metadata {

using MetadataVariant = std::variant<
    std::shared_ptr<hw::arm::TeleopMockJointStateProducer::TeleopMockJointStateProducerMetadata>,
    std::shared_ptr<hw::arm::TeleopTrossenArmProducer::TeleopTrossenArmProducerMetadata>,
    std::shared_ptr<hw::camera::MockCameraProducer::MockCameraProducerMetadata>,
    std::shared_ptr<hw::camera::OpenCvCameraProducer::OpenCvCameraProducerMetadata>,
    std::shared_ptr<hw::arm::MockJointStateProducer::MockJointStateProducerMetadata>,
    std::shared_ptr<hw::arm::TrossenArmProducer::TrossenArmProducerMetadata>

>;

inline trossen::metadata::MetadataVariant make_metadata_variant(
    std::shared_ptr<trossen::hw::PolledProducer::ProducerMetadata> base)
{
    using namespace trossen;

    // --- ARM TELEOP MOCK ---
    if (auto p = std::dynamic_pointer_cast<
            hw::arm::TeleopMockJointStateProducer::TeleopMockJointStateProducerMetadata
        >(base))
        return p;

    // --- ARM TELEOP real ---
    if (auto p = std::dynamic_pointer_cast<
            hw::arm::TeleopTrossenArmProducer::TeleopTrossenArmProducerMetadata
        >(base))
        return p;

    // --- CAMERA MOCK ---
    if (auto p = std::dynamic_pointer_cast<
            hw::camera::MockCameraProducer::MockCameraProducerMetadata
        >(base))
        return p;

    // --- CAMERA OpenCV ---
    if (auto p = std::dynamic_pointer_cast<
            hw::camera::OpenCvCameraProducer::OpenCvCameraProducerMetadata
        >(base))
        return p;

    // --- ARM mock joint ---
    if (auto p = std::dynamic_pointer_cast<
            hw::arm::MockJointStateProducer::MockJointStateProducerMetadata
        >(base))
        return p;

    // --- ARM real joint ---
    if (auto p = std::dynamic_pointer_cast<
            hw::arm::TrossenArmProducer::TrossenArmProducerMetadata
        >(base))
        return p;

    throw std::runtime_error("Unknown metadata type in make_metadata_variant()");
}

} // namespace trossen::metadata
