/**
 * @file trossen_sdk.cpp
 * @brief Python bindings for the Trossen SDK (pybind11).
 *
 * Binding sections are added incrementally across the PR stack:
 *   1. Version constants  (PR A)
 *   2. Data types          (PR B — this PR)
 *   3. Hardware + producers (PR B)
 *   4. Backends + config    (PR B)
 *   5. Teleop + session     (PR C)
 *   6. Utilities            (PR C)
 */

#include <chrono>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <pybind11/numpy.h>

// Local type casters (JSON, chrono, cv::Mat <-> numpy)
#include "trossen_sdk_type_casters.hpp"
#include "trossen_sdk_ndarray_caster.hpp"

// Version — must be included before trossen_slate headers which
// #define VERSION_MAJOR/MINOR/PATCH.
#include "trossen_sdk/version.hpp"

// Save version values before trossen_slate macros clobber the tokens.
namespace {
  constexpr uint32_t kSdkVersionMajor = trossen::core::VERSION_MAJOR;
  constexpr uint32_t kSdkVersionMinor = trossen::core::VERSION_MINOR;
  constexpr uint32_t kSdkVersionPatch = trossen::core::VERSION_PATCH;
}

// Data
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/data/record.hpp"

// Hardware
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"

// Producers
#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/hw/arm/mock_joint_producer.hpp"
#include "trossen_sdk/hw/camera/mock_producer.hpp"

// I/O
#include "trossen_sdk/io/backend.hpp"
#include "trossen_sdk/io/backend_registry.hpp"
#include "trossen_sdk/io/sink.hpp"
#include "trossen_sdk/io/backends/null/null_backend.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_backend.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_backend.hpp"
#include "trossen_sdk/types.hpp"

// Runtime (producer registries only — session manager deferred to PR C)
#include "trossen_sdk/runtime/producer_registry.hpp"
#include "trossen_sdk/runtime/push_producer_registry.hpp"

// Configuration
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/base_config.hpp"
#include "trossen_sdk/configuration/sdk_config.hpp"
#include "trossen_sdk/configuration/types/hardware/arm_config.hpp"
#include "trossen_sdk/configuration/types/hardware/camera_config.hpp"
#include "trossen_sdk/configuration/types/hardware/mobile_base_config.hpp"
#include "trossen_sdk/configuration/types/producers/producer_config.hpp"
#include "trossen_sdk/configuration/types/teleop_config.hpp"
#include "trossen_sdk/configuration/types/backends/trossen_mcap_backend_config.hpp"
#include "trossen_sdk/configuration/types/backends/lerobot_v2_backend_config.hpp"
#include "trossen_sdk/configuration/types/backends/null_backend_config.hpp"
#include "trossen_sdk/configuration/types/runtime/session_manager_config.hpp"

namespace py = pybind11;

// ─── Trampoline classes ─────────────────────────────────────────────────────

class PyHardwareComponent : public trossen::hw::HardwareComponent {
public:
  using HardwareComponent::HardwareComponent;
  void configure(const nlohmann::json& config) override {
    PYBIND11_OVERRIDE_PURE(void, HardwareComponent, configure, config);
  }
  std::string get_type() const override {
    PYBIND11_OVERRIDE_PURE(std::string, HardwareComponent, get_type);
  }
  nlohmann::json get_info() const override {
    PYBIND11_OVERRIDE(nlohmann::json, HardwareComponent, get_info);
  }
};

class PyPolledProducer : public trossen::hw::PolledProducer {
public:
  using PolledProducer::PolledProducer;
  void poll(
      const std::function<void(std::shared_ptr<trossen::data::RecordBase>)>& emit
  ) override {
    PYBIND11_OVERRIDE_PURE(void, PolledProducer, poll, emit);
  }
  std::shared_ptr<ProducerMetadata> metadata() const override {
    PYBIND11_OVERRIDE_PURE(
        std::shared_ptr<ProducerMetadata>, PolledProducer, metadata);
  }
};

class PyPushProducer : public trossen::hw::PushProducer {
public:
  using PushProducer::PushProducer;
  bool start(
      const std::function<void(std::shared_ptr<trossen::data::RecordBase>)>& emit
  ) override {
    PYBIND11_OVERRIDE_PURE(bool, PushProducer, start, emit);
  }
  void stop() override {
    PYBIND11_OVERRIDE_PURE(void, PushProducer, stop);
  }
  std::shared_ptr<trossen::hw::PolledProducer::ProducerMetadata> metadata()
      const override {
    PYBIND11_OVERRIDE(
        std::shared_ptr<trossen::hw::PolledProducer::ProducerMetadata>,
        PushProducer, metadata);
  }
};

class PyBackend : public trossen::io::Backend {
public:
  using Backend::Backend;
  void preprocess_episode() override {
    PYBIND11_OVERRIDE(void, Backend, preprocess_episode);
  }
  bool open() override {
    PYBIND11_OVERRIDE_PURE(bool, Backend, open);
  }
  void write(const trossen::data::RecordBase& record) override {
    PYBIND11_OVERRIDE_PURE(void, Backend, write, record);
  }
  void flush() override {
    PYBIND11_OVERRIDE_PURE(void, Backend, flush);
  }
  void close() override {
    PYBIND11_OVERRIDE_PURE(void, Backend, close);
  }
  void discard_episode() override {
    PYBIND11_OVERRIDE_PURE(void, Backend, discard_episode);
  }
  uint32_t scan_existing_episodes() override {
    PYBIND11_OVERRIDE_PURE(uint32_t, Backend, scan_existing_episodes);
  }
  void set_episode_index(uint32_t episode_index) override {
    PYBIND11_OVERRIDE(void, Backend, set_episode_index, episode_index);
  }
};

// ─── Module definition ──────────────────────────────────────────────────────

PYBIND11_MODULE(trossen_sdk, m) {
  m.doc() = "Trossen SDK Python bindings";

  // ── 1. Version ──────────────────────────────────────────────────────────
  m.def("version", &trossen::core::version, "Get SDK version string");
  m.attr("VERSION_MAJOR") = kSdkVersionMajor;
  m.attr("VERSION_MINOR") = kSdkVersionMinor;
  m.attr("VERSION_PATCH") = kSdkVersionPatch;

  // ── 2. Data types ───────────────────────────────────────────────────────
  using namespace trossen::data;

  m.attr("S_TO_NS") = S_TO_NS;

  py::class_<Timespec>(m, "Timespec")
    .def(py::init<>())
    .def(py::init([](int64_t sec, uint32_t nsec) {
      Timespec ts; ts.sec = sec; ts.nsec = nsec; return ts;
    }), py::arg("sec"), py::arg("nsec"))
    .def_readwrite("sec", &Timespec::sec)
    .def_readwrite("nsec", &Timespec::nsec)
    .def("to_ns", &Timespec::to_ns)
    .def_static("from_ns", &Timespec::from_ns, py::arg("total_ns"))
    .def("__repr__", [](const Timespec& ts) {
      return "Timespec(sec=" + std::to_string(ts.sec) +
             ", nsec=" + std::to_string(ts.nsec) + ")";
    });

  py::class_<Timestamp>(m, "Timestamp")
    .def(py::init<>())
    .def_readwrite("monotonic", &Timestamp::monotonic)
    .def_readwrite("realtime", &Timestamp::realtime)
    .def("__repr__", [](const Timestamp& ts) {
      return "Timestamp(mono=" + std::to_string(ts.monotonic.to_ns()) +
             "ns, real=" + std::to_string(ts.realtime.to_ns()) + "ns)";
    });

  m.def("now_mono", &now_mono, "Get current monotonic time as Timespec");
  m.def("now_real", &now_real, "Get current realtime as Timespec");
  m.def("make_timestamp_now", &make_timestamp_now,
        "Create a Timestamp with both clocks set to now");

  py::class_<RecordBase, std::shared_ptr<RecordBase>>(m, "RecordBase")
    .def(py::init<>())
    .def_readwrite("ts", &RecordBase::ts)
    .def_readwrite("seq", &RecordBase::seq)
    .def_readwrite("id", &RecordBase::id);

  py::class_<JointStateRecord, RecordBase, std::shared_ptr<JointStateRecord>>(
      m, "JointStateRecord")
    .def(py::init<>())
    .def(py::init<const Timestamp&, uint64_t, std::string,
                  const std::vector<float>&, const std::vector<float>&,
                  const std::vector<float>&>(),
         py::arg("ts"), py::arg("seq"), py::arg("id"),
         py::arg("positions"), py::arg("velocities"), py::arg("efforts"))
    .def_readwrite("positions", &JointStateRecord::positions)
    .def_readwrite("velocities", &JointStateRecord::velocities)
    .def_readwrite("efforts", &JointStateRecord::efforts)
    .def("__repr__", [](const JointStateRecord& r) {
      return "JointStateRecord(id='" + r.id +
             "', joints=" + std::to_string(r.positions.size()) + ")";
    });

  py::class_<Odometry2DRecord::Pose>(m, "Odometry2DPose")
    .def(py::init<>())
    .def_readwrite("x", &Odometry2DRecord::Pose::x)
    .def_readwrite("y", &Odometry2DRecord::Pose::y)
    .def_readwrite("theta", &Odometry2DRecord::Pose::theta);

  py::class_<Odometry2DRecord::Twist>(m, "Odometry2DTwist")
    .def(py::init<>())
    .def_readwrite("linear_x", &Odometry2DRecord::Twist::linear_x)
    .def_readwrite("linear_y", &Odometry2DRecord::Twist::linear_y)
    .def_readwrite("angular_z", &Odometry2DRecord::Twist::angular_z);

  py::class_<Odometry2DRecord, RecordBase, std::shared_ptr<Odometry2DRecord>>(
      m, "Odometry2DRecord")
    .def(py::init<>())
    .def_readwrite("pose", &Odometry2DRecord::pose)
    .def_readwrite("twist", &Odometry2DRecord::twist);

  py::class_<ImageRecord, RecordBase, std::shared_ptr<ImageRecord>>(
      m, "ImageRecord")
    .def(py::init<>())
    .def_readwrite("width", &ImageRecord::width)
    .def_readwrite("height", &ImageRecord::height)
    .def_readwrite("channels", &ImageRecord::channels)
    .def_readwrite("encoding", &ImageRecord::encoding)
    .def_readwrite("image", &ImageRecord::image)
    .def_property("depth_image",
      [](const ImageRecord& r) -> py::object {
        if (r.depth_image.has_value()) return py::cast(r.depth_image.value());
        return py::none();
      },
      [](ImageRecord& r, const py::object& obj) {
        if (obj.is_none()) { r.depth_image = std::nullopt; }
        else { r.depth_image = obj.cast<cv::Mat>(); }
      })
    .def_property("depth_scale",
      [](const ImageRecord& r) -> py::object {
        if (r.depth_scale.has_value()) return py::float_(r.depth_scale.value());
        return py::none();
      },
      [](ImageRecord& r, const py::object& obj) {
        if (obj.is_none()) { r.depth_scale = std::nullopt; }
        else { r.depth_scale = obj.cast<float>(); }
      })
    .def("has_depth", &ImageRecord::has_depth);

  // ── 3. Hardware + producers ─────────────────────────────────────────────
  using namespace trossen::hw;

  py::class_<HardwareComponent, PyHardwareComponent,
             std::shared_ptr<HardwareComponent>>(m, "HardwareComponent")
    .def(py::init<const std::string&>(), py::arg("identifier"))
    .def("get_identifier", &HardwareComponent::get_identifier)
    .def("get_type", &HardwareComponent::get_type)
    .def("get_info", &HardwareComponent::get_info);

  py::class_<HardwareRegistry>(m, "HardwareRegistry")
    .def_static("create", &HardwareRegistry::create,
                py::arg("type"), py::arg("identifier"),
                py::arg("config"), py::arg("mark_active") = true)
    .def_static("is_registered", &HardwareRegistry::is_registered, py::arg("type"))
    .def_static("get_registered_types", &HardwareRegistry::get_registered_types);

  py::class_<ActiveHardwareRegistry>(m, "ActiveHardwareRegistry")
    .def_static("get", &ActiveHardwareRegistry::get, py::arg("id"))
    .def_static("get_all", &ActiveHardwareRegistry::get_all)
    .def_static("get_ids", &ActiveHardwareRegistry::get_ids)
    .def_static("is_registered", &ActiveHardwareRegistry::is_registered, py::arg("id"))
    .def_static("clear", &ActiveHardwareRegistry::clear)
    .def_static("count", &ActiveHardwareRegistry::count);

  py::class_<ProducerStats>(m, "ProducerStats")
    .def_readwrite("produced", &ProducerStats::produced)
    .def_readwrite("dropped", &ProducerStats::dropped)
    .def_readwrite("warmup_discarded", &ProducerStats::warmup_discarded)
    .def("__repr__", [](const ProducerStats& s) {
      return "ProducerStats(produced=" + std::to_string(s.produced) +
             ", dropped=" + std::to_string(s.dropped) + ")";
    });

  py::class_<PolledProducer::ProducerMetadata,
             std::shared_ptr<PolledProducer::ProducerMetadata>>(m, "ProducerMetadata")
    .def_readwrite("type", &PolledProducer::ProducerMetadata::type)
    .def_readwrite("id", &PolledProducer::ProducerMetadata::id)
    .def_readwrite("name", &PolledProducer::ProducerMetadata::name)
    .def_readwrite("description", &PolledProducer::ProducerMetadata::description)
    .def("get_info", &PolledProducer::ProducerMetadata::get_info)
    .def("get_stream_info", &PolledProducer::ProducerMetadata::get_stream_info);

  py::class_<PolledProducer, PyPolledProducer, std::shared_ptr<PolledProducer>>(
      m, "PolledProducer")
    .def("poll", &PolledProducer::poll, py::arg("emit"))
    .def("stats", &PolledProducer::stats)
    .def("metadata", &PolledProducer::metadata);

  py::class_<PushProducer, PyPushProducer, std::shared_ptr<PushProducer>>(
      m, "PushProducer")
    .def("start", &PushProducer::start, py::arg("emit"))
    .def("stop", &PushProducer::stop)
    .def("stats", &PushProducer::stats)
    .def("metadata", &PushProducer::metadata);

  py::class_<arm::MockJointStateProducer::Config>(m, "MockJointStateProducerConfig")
    .def(py::init<>())
    .def_readwrite("num_joints", &arm::MockJointStateProducer::Config::num_joints)
    .def_readwrite("rate_hz", &arm::MockJointStateProducer::Config::rate_hz)
    .def_readwrite("id", &arm::MockJointStateProducer::Config::id)
    .def_readwrite("amplitude", &arm::MockJointStateProducer::Config::amplitude);

  py::class_<arm::MockJointStateProducer, PolledProducer,
             std::shared_ptr<arm::MockJointStateProducer>>(
      m, "MockJointStateProducer")
    .def(py::init<arm::MockJointStateProducer::Config>(), py::arg("config"))
    .def("diagnostics", &arm::MockJointStateProducer::diagnostics);

  py::enum_<camera::MockCameraProducer::Pattern>(m, "MockCameraPattern")
    .value("Solid", camera::MockCameraProducer::Pattern::Solid)
    .value("Gradient", camera::MockCameraProducer::Pattern::Gradient)
    .value("Noise", camera::MockCameraProducer::Pattern::Noise)
    .export_values();

  py::class_<camera::MockCameraProducer::Config>(m, "MockCameraProducerConfig")
    .def(py::init<>())
    .def_readwrite("width", &camera::MockCameraProducer::Config::width)
    .def_readwrite("height", &camera::MockCameraProducer::Config::height)
    .def_readwrite("fps", &camera::MockCameraProducer::Config::fps)
    .def_readwrite("stream_id", &camera::MockCameraProducer::Config::stream_id)
    .def_readwrite("encoding", &camera::MockCameraProducer::Config::encoding)
    .def_readwrite("pattern", &camera::MockCameraProducer::Config::pattern);

  py::class_<camera::MockCameraProducer, PolledProducer,
             std::shared_ptr<camera::MockCameraProducer>>(m, "MockCameraProducer")
    .def(py::init<camera::MockCameraProducer::Config>(), py::arg("config"));

  // ── 4. Backends + I/O ───────────────────────────────────────────────────
  using namespace trossen::io;

  py::class_<Backend, PyBackend, std::shared_ptr<Backend>>(m, "Backend")
    .def(py::init<>())
    .def("preprocess_episode", &Backend::preprocess_episode)
    .def("open", &Backend::open)
    .def("write", &Backend::write, py::arg("record"))
    .def("flush", &Backend::flush)
    .def("close", &Backend::close)
    .def("discard_episode", &Backend::discard_episode)
    .def("scan_existing_episodes", &Backend::scan_existing_episodes)
    .def("set_episode_index", &Backend::set_episode_index, py::arg("episode_index"));

  py::class_<Backend::Config>(m, "BackendConfig")
    .def(py::init<>())
    .def_readwrite("type", &Backend::Config::type);

  py::class_<backends::NullBackend, Backend,
             std::shared_ptr<backends::NullBackend>>(m, "NullBackend")
    .def(py::init<const trossen::ProducerMetadataList&>(),
         py::arg("metadata") = trossen::ProducerMetadataList{})
    .def("count", &backends::NullBackend::count);

  py::class_<backends::TrossenMCAPBackend, Backend,
             std::shared_ptr<backends::TrossenMCAPBackend>>(m, "TrossenMCAPBackend")
    .def(py::init<const trossen::ProducerMetadataList&>(),
         py::arg("metadata") = trossen::ProducerMetadataList{})
    .def("stats", &backends::TrossenMCAPBackend::stats);

  py::class_<backends::TrossenMCAPBackend::Stats>(m, "TrossenMCAPBackendStats")
    .def_readonly("joint_states_written",
                  &backends::TrossenMCAPBackend::Stats::joint_states_written)
    .def_readonly("odometry_2d_written",
                  &backends::TrossenMCAPBackend::Stats::odometry_2d_written)
    .def_readonly("images_written",
                  &backends::TrossenMCAPBackend::Stats::images_written)
    .def_readonly("depth_images_written",
                  &backends::TrossenMCAPBackend::Stats::depth_images_written);

  py::class_<backends::LeRobotV2Backend, Backend,
             std::shared_ptr<backends::LeRobotV2Backend>>(m, "LeRobotV2Backend")
    .def(py::init<const trossen::ProducerMetadataList&>(),
         py::arg("metadata") = trossen::ProducerMetadataList{});

  py::class_<Sink>(m, "Sink")
    .def(py::init<std::shared_ptr<Backend>>(), py::arg("backend"))
    .def("enqueue", &Sink::enqueue, py::arg("record"))
    .def("start", &Sink::start)
    .def("stop", &Sink::stop)
    .def("processed_count", &Sink::processed_count);

  py::class_<BackendRegistry>(m, "BackendRegistry")
    .def_static("create", &BackendRegistry::create,
                py::arg("type"),
                py::arg("producer_metadatas") = trossen::ProducerMetadataList{})
    .def_static("is_registered", &BackendRegistry::is_registered, py::arg("type"));

  // ── Producer registries ─────────────────────────────────────────────────
  using namespace trossen::runtime;

  py::class_<ProducerRegistry>(m, "ProducerRegistry")
    .def_static("create", &ProducerRegistry::create,
                py::arg("type"), py::arg("hw"), py::arg("config"))
    .def_static("is_registered", &ProducerRegistry::is_registered, py::arg("type"))
    .def_static("get_registered_types", &ProducerRegistry::get_registered_types);

  py::class_<PushProducerRegistry>(m, "PushProducerRegistry")
    .def_static("create", &PushProducerRegistry::create,
                py::arg("type"), py::arg("hw"), py::arg("config"))
    .def_static("is_registered", &PushProducerRegistry::is_registered,
                py::arg("type"))
    .def_static("get_registered_types",
                &PushProducerRegistry::get_registered_types);

  // ── 5. Configuration types ──────────────────────────────────────────────
  using namespace trossen::configuration;

  py::class_<JsonLoader>(m, "JsonLoader")
    .def_static("load", &JsonLoader::load, py::arg("path"),
                "Load a JSON configuration file and return as dict");

  py::class_<BaseConfig, std::shared_ptr<BaseConfig>>(m, "BaseConfig")
    .def("type", &BaseConfig::type);

  py::class_<GlobalConfig>(m, "GlobalConfig")
    .def_static("instance", &GlobalConfig::instance,
                py::return_value_policy::reference)
    .def("load_from_json", &GlobalConfig::load_from_json, py::arg("json"))
    .def("get", &GlobalConfig::get, py::arg("key"));

  py::class_<ArmConfig>(m, "ArmConfig")
    .def(py::init<>())
    .def_readwrite("ip_address", &ArmConfig::ip_address)
    .def_readwrite("model", &ArmConfig::model)
    .def_readwrite("end_effector", &ArmConfig::end_effector)
    .def_static("from_json", &ArmConfig::from_json, py::arg("json"))
    .def("to_json", &ArmConfig::to_json);

  py::class_<CameraConfig>(m, "CameraConfig")
    .def(py::init<>())
    .def_readwrite("type", &CameraConfig::type)
    .def_readwrite("id", &CameraConfig::id)
    .def_readwrite("serial_number", &CameraConfig::serial_number)
    .def_readwrite("device_index", &CameraConfig::device_index)
    .def_readwrite("backend", &CameraConfig::backend)
    .def_readwrite("width", &CameraConfig::width)
    .def_readwrite("height", &CameraConfig::height)
    .def_readwrite("fps", &CameraConfig::fps)
    .def_readwrite("use_depth", &CameraConfig::use_depth)
    .def_readwrite("force_hardware_reset", &CameraConfig::force_hardware_reset)
    .def_readwrite("extra", &CameraConfig::extra)
    .def_static("from_json", &CameraConfig::from_json, py::arg("json"))
    .def("to_json", &CameraConfig::to_json);

  py::class_<MobileBaseConfig>(m, "MobileBaseConfig")
    .def(py::init<>())
    .def_readwrite("reset_odometry", &MobileBaseConfig::reset_odometry)
    .def_readwrite("enable_torque", &MobileBaseConfig::enable_torque)
    .def_static("from_json", &MobileBaseConfig::from_json, py::arg("json"))
    .def("to_json", &MobileBaseConfig::to_json);

  py::class_<ProducerConfig>(m, "ProducerConfig")
    .def(py::init<>())
    .def_readwrite("type", &ProducerConfig::type)
    .def_readwrite("hardware_id", &ProducerConfig::hardware_id)
    .def_readwrite("stream_id", &ProducerConfig::stream_id)
    .def_readwrite("poll_rate_hz", &ProducerConfig::poll_rate_hz)
    .def_readwrite("use_device_time", &ProducerConfig::use_device_time)
    .def_readwrite("encoding", &ProducerConfig::encoding)
    .def_static("from_json", &ProducerConfig::from_json, py::arg("json"))
    .def("to_registry_json",
         static_cast<nlohmann::json (ProducerConfig::*)() const>(
             &ProducerConfig::to_registry_json),
         "Build JSON for arm/base producer registry")
    .def("to_registry_json_camera",
         static_cast<nlohmann::json (ProducerConfig::*)(int, int, int) const>(
             &ProducerConfig::to_registry_json),
         py::arg("width"), py::arg("height"), py::arg("fps"),
         "Build JSON for camera producer registry");

  py::class_<TeleoperationPair>(m, "TeleoperationPair")
    .def(py::init<>())
    .def_readwrite("leader", &TeleoperationPair::leader)
    .def_readwrite("follower", &TeleoperationPair::follower)
    .def_readwrite("space", &TeleoperationPair::space)
    .def_static("from_json", &TeleoperationPair::from_json, py::arg("json"));

  py::class_<TeleoperationConfig>(m, "TeleoperationConfig")
    .def(py::init<>())
    .def_readwrite("enabled", &TeleoperationConfig::enabled)
    .def_readwrite("rate_hz", &TeleoperationConfig::rate_hz)
    .def_readwrite("pairs", &TeleoperationConfig::pairs)
    .def_static("from_json", &TeleoperationConfig::from_json, py::arg("json"));

  py::class_<SessionManagerConfig, BaseConfig,
             std::shared_ptr<SessionManagerConfig>>(m, "SessionManagerConfig")
    .def(py::init<>())
    .def_property("max_duration",
      [](const SessionManagerConfig& c) -> py::object {
        if (c.max_duration.has_value()) return py::float_(c.max_duration->count());
        return py::none();
      },
      [](SessionManagerConfig& c, const py::object& obj) {
        if (obj.is_none()) { c.max_duration = std::nullopt; }
        else { c.max_duration = std::chrono::duration<double>(obj.cast<double>()); }
      },
      "Max episode duration in seconds (None = unlimited)")
    .def_readwrite("max_episodes", &SessionManagerConfig::max_episodes)
    .def_readwrite("backend_type", &SessionManagerConfig::backend_type)
    .def_property("reset_duration",
      [](const SessionManagerConfig& c) -> py::object {
        if (c.reset_duration.has_value()) return py::float_(c.reset_duration->count());
        return py::none();
      },
      [](SessionManagerConfig& c, const py::object& obj) {
        if (obj.is_none()) { c.reset_duration = std::nullopt; }
        else { c.reset_duration = std::chrono::duration<double>(obj.cast<double>()); }
      },
      "Reset duration between episodes in seconds (None = wait for input)")
    .def_static("from_json", &SessionManagerConfig::from_json, py::arg("json"));

  py::class_<TrossenMCAPBackendConfig, BaseConfig,
             std::shared_ptr<TrossenMCAPBackendConfig>>(
      m, "TrossenMCAPBackendConfig")
    .def(py::init<>())
    .def_readwrite("root", &TrossenMCAPBackendConfig::root)
    .def_readwrite("robot_name", &TrossenMCAPBackendConfig::robot_name)
    .def_readwrite("chunk_size_bytes", &TrossenMCAPBackendConfig::chunk_size_bytes)
    .def_readwrite("compression", &TrossenMCAPBackendConfig::compression)
    .def_readwrite("dataset_id", &TrossenMCAPBackendConfig::dataset_id)
    .def_readwrite("episode_index", &TrossenMCAPBackendConfig::episode_index)
    .def_static("from_json", &TrossenMCAPBackendConfig::from_json, py::arg("json"));

  py::class_<LeRobotV2BackendConfig, BaseConfig,
             std::shared_ptr<LeRobotV2BackendConfig>>(
      m, "LeRobotV2BackendConfig")
    .def(py::init<>())
    .def_readwrite("encoder_threads", &LeRobotV2BackendConfig::encoder_threads)
    .def_readwrite("max_image_queue", &LeRobotV2BackendConfig::max_image_queue)
    .def_readwrite("png_compression_level",
                   &LeRobotV2BackendConfig::png_compression_level)
    .def_readwrite("overwrite_existing", &LeRobotV2BackendConfig::overwrite_existing)
    .def_readwrite("encode_videos", &LeRobotV2BackendConfig::encode_videos)
    .def_readwrite("task_name", &LeRobotV2BackendConfig::task_name)
    .def_readwrite("repository_id", &LeRobotV2BackendConfig::repository_id)
    .def_readwrite("dataset_id", &LeRobotV2BackendConfig::dataset_id)
    .def_readwrite("root", &LeRobotV2BackendConfig::root)
    .def_readwrite("episode_index", &LeRobotV2BackendConfig::episode_index)
    .def_readwrite("chunk_size", &LeRobotV2BackendConfig::chunk_size)
    .def_readwrite("robot_name", &LeRobotV2BackendConfig::robot_name)
    .def_readwrite("fps", &LeRobotV2BackendConfig::fps)
    .def_readwrite("license", &LeRobotV2BackendConfig::license)
    .def_static("from_json", &LeRobotV2BackendConfig::from_json, py::arg("json"));

  py::class_<HardwareConfig>(m, "HardwareConfig")
    .def(py::init<>())
    .def_readwrite("arms", &HardwareConfig::arms)
    .def_readwrite("cameras", &HardwareConfig::cameras)
    .def_property("mobile_base",
      [](const HardwareConfig& c) -> py::object {
        if (c.mobile_base.has_value()) return py::cast(c.mobile_base.value());
        return py::none();
      },
      [](HardwareConfig& c, const py::object& obj) {
        if (obj.is_none()) { c.mobile_base = std::nullopt; }
        else { c.mobile_base = obj.cast<MobileBaseConfig>(); }
      })
    .def_static("from_json", &HardwareConfig::from_json, py::arg("json"));

  py::class_<SdkConfig>(m, "SdkConfig")
    .def(py::init<>())
    .def_readwrite("robot_name", &SdkConfig::robot_name)
    .def_readwrite("hardware", &SdkConfig::hardware)
    .def_readwrite("producers", &SdkConfig::producers)
    .def_readwrite("teleop", &SdkConfig::teleop)
    .def_readwrite("mcap_backend", &SdkConfig::mcap_backend)
    .def_readwrite("session", &SdkConfig::session)
    .def_static("from_json", &SdkConfig::from_json, py::arg("json"))
    .def("populate_global_config", &SdkConfig::populate_global_config);

  // ── Sections 5-6 (teleop, session, utilities) added in PR C ─────────────
}
