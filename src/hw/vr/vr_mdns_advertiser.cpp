/**
 * @file vr_mdns_advertiser.cpp
 * @brief Avahi-backed implementation of the Quest discovery advertiser.
 */

#include "trossen_sdk/hw/vr/vr_mdns_advertiser.hpp"

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/thread-watch.h>

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>

namespace trossen::hw::vr {

namespace {

// Protocol contract — must match the Python helper / Quest app.
constexpr const char* kServiceType  = "_trossen-vr._tcp";
constexpr const char* kAppTxtRecord = "app=trossen_vr_teleop";

}  // namespace

struct VrMdnsAdvertiser::Impl {
  // Ownership and synchronization live entirely inside Avahi's threaded
  // poll — `avahi_threaded_poll_lock()` serializes any access to the
  // client / entry group from the main thread, and callbacks run with
  // that same lock already held. Mixing a separate mutex with the
  // threaded poll deadlocks on shutdown, because `poll_stop` waits for
  // the poll thread to finish while the poll thread is blocked trying
  // to enter a callback that takes the outer mutex.
  AvahiThreadedPoll* poll{nullptr};
  AvahiClient*       client{nullptr};
  AvahiEntryGroup*   group{nullptr};
  std::string        instance_name;
  std::uint16_t      port{0};
  std::atomic<bool>  running{false};

  // Avahi callbacks — dispatched on the poll thread while the poll's
  // internal lock is held. Safe to touch `client`/`group`/`instance_name`
  // directly here.
  static void entry_group_cb(AvahiEntryGroup* g,
                             AvahiEntryGroupState state,
                             void* userdata);
  static void client_cb(AvahiClient* c,
                        AvahiClientState state,
                        void* userdata);

  // Helpers called with the threaded-poll lock held. The `use_client`
  // parameter lets callers pass the client pointer from a callback,
  // since Avahi may fire the initial client callback *before*
  // `avahi_client_new()` has returned and we have stored the client
  // pointer on the struct.
  bool create_services(AvahiClient* use_client = nullptr);
  void reset_services();
};

void VrMdnsAdvertiser::Impl::reset_services() {
  if (group) {
    avahi_entry_group_free(group);
    group = nullptr;
  }
}

bool VrMdnsAdvertiser::Impl::create_services(AvahiClient* use_client) {
  // Prefer the caller-supplied client pointer over the one stored on
  // `this` — `avahi_client_new()` can fire the initial state callback
  // before it returns, so `client_cb(c=X, state=S_RUNNING)` arrives
  // while `impl_->client` is still null.
  AvahiClient* c = use_client ? use_client : client;
  if (!c) return false;

  if (!group) {
    group = avahi_entry_group_new(c, &Impl::entry_group_cb, this);
    if (!group) {
      std::cerr << "VrMdnsAdvertiser: avahi_entry_group_new failed: "
                << avahi_strerror(avahi_client_errno(c)) << "\n";
      return false;
    }
  }

  if (!avahi_entry_group_is_empty(group)) {
    return true;
  }

  const int rc = avahi_entry_group_add_service(
    group,
    AVAHI_IF_UNSPEC,
    AVAHI_PROTO_INET,     // IPv4, matches the Python helper.
    AvahiPublishFlags{},
    instance_name.c_str(),
    kServiceType,
    /*domain=*/nullptr,
    /*host=*/nullptr,
    port,
    kAppTxtRecord,
    /*terminator=*/static_cast<const char*>(nullptr));
  if (rc < 0) {
    if (rc == AVAHI_ERR_COLLISION) {
      // Peer already owns this instance name — pick a suffixed
      // alternative. The caller (client_cb or entry_group_cb) retries.
      char* alt = avahi_alternative_service_name(instance_name.c_str());
      instance_name = alt;
      avahi_free(alt);
      reset_services();
      return false;
    }
    std::cerr << "VrMdnsAdvertiser: add_service failed: "
              << avahi_strerror(rc) << "\n";
    return false;
  }

  const int commit_rc = avahi_entry_group_commit(group);
  if (commit_rc < 0) {
    std::cerr << "VrMdnsAdvertiser: entry_group_commit failed: "
              << avahi_strerror(commit_rc) << "\n";
    return false;
  }
  return true;
}

void VrMdnsAdvertiser::Impl::entry_group_cb(AvahiEntryGroup* g,
                                            AvahiEntryGroupState state,
                                            void* userdata) {
  auto* self = static_cast<Impl*>(userdata);

  switch (state) {
    case AVAHI_ENTRY_GROUP_ESTABLISHED:
      self->running.store(true);
      break;
    case AVAHI_ENTRY_GROUP_COLLISION: {
      // Another host on the LAN took our name — retry with an
      // alternative ("TrossenVR #2" etc.).
      char* alt = avahi_alternative_service_name(self->instance_name.c_str());
      self->instance_name = alt;
      avahi_free(alt);
      self->reset_services();
      self->create_services(avahi_entry_group_get_client(g));
      break;
    }
    case AVAHI_ENTRY_GROUP_FAILURE:
      std::cerr << "VrMdnsAdvertiser: entry group failure: "
                << avahi_strerror(avahi_client_errno(
                     avahi_entry_group_get_client(g)))
                << "\n";
      self->running.store(false);
      break;
    case AVAHI_ENTRY_GROUP_UNCOMMITED:
    case AVAHI_ENTRY_GROUP_REGISTERING:
      break;
  }
}

void VrMdnsAdvertiser::Impl::client_cb(AvahiClient* c,
                                       AvahiClientState state,
                                       void* userdata) {
  auto* self = static_cast<Impl*>(userdata);

  switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
      // Pass `c` explicitly: the first S_RUNNING callback fires while
      // `avahi_client_new()` is still running, before the caller has
      // stored the client pointer on `impl_->client`.
      self->create_services(c);
      break;
    case AVAHI_CLIENT_S_COLLISION:
    case AVAHI_CLIENT_S_REGISTERING:
      self->reset_services();
      self->running.store(false);
      break;
    case AVAHI_CLIENT_FAILURE:
      std::cerr << "VrMdnsAdvertiser: client failure: "
                << avahi_strerror(avahi_client_errno(c)) << "\n";
      self->running.store(false);
      break;
    case AVAHI_CLIENT_CONNECTING:
      break;
  }
}

VrMdnsAdvertiser::VrMdnsAdvertiser() : impl_(std::make_unique<Impl>()) {}

VrMdnsAdvertiser::~VrMdnsAdvertiser() {
  stop();
}

void VrMdnsAdvertiser::start(std::uint16_t port,
                             const std::string& instance_name) {
  // Idempotent: a repeated call on an already-running advertiser is a
  // no-op when the (port, instance_name) matches. Anything else has to
  // go through stop() first.
  if (impl_->client) {
    if (impl_->port == port && impl_->instance_name == instance_name) {
      return;
    }
    throw std::runtime_error(
      "VrMdnsAdvertiser::start: already running on a different "
      "(port, instance_name). Call stop() before reconfiguring.");
  }

  impl_->port          = port;
  impl_->instance_name = instance_name;

  impl_->poll = avahi_threaded_poll_new();
  if (!impl_->poll) {
    throw std::runtime_error(
      "VrMdnsAdvertiser: avahi_threaded_poll_new failed");
  }

  int err = 0;
  impl_->client = avahi_client_new(
    avahi_threaded_poll_get(impl_->poll),
    AvahiClientFlags{},
    &Impl::client_cb,
    impl_.get(),
    &err);
  if (!impl_->client) {
    avahi_threaded_poll_free(impl_->poll);
    impl_->poll = nullptr;
    throw std::runtime_error(
      std::string{"VrMdnsAdvertiser: avahi_client_new failed ("} +
      avahi_strerror(err) +
      "). Is avahi-daemon running? "
      "Try `systemctl status avahi-daemon`.");
  }

  if (avahi_threaded_poll_start(impl_->poll) < 0) {
    avahi_client_free(impl_->client);
    avahi_threaded_poll_free(impl_->poll);
    impl_->client = nullptr;
    impl_->poll   = nullptr;
    throw std::runtime_error(
      "VrMdnsAdvertiser: avahi_threaded_poll_start failed");
  }
}

void VrMdnsAdvertiser::stop() {
  // Teardown order is strict. We must:
  //   1. Stop the poll *thread* first — this unblocks any callback
  //      currently waiting inside Avahi and prevents new ones from
  //      firing. Doing this without holding any Avahi lock avoids the
  //      deadlock where poll_stop waits for the callback, and the
  //      callback waits for our lock.
  //   2. Free the entry group and client while the thread is quiesced.
  //   3. Free the poll object last.
  if (impl_->poll) {
    avahi_threaded_poll_stop(impl_->poll);
  }
  if (impl_->group) {
    avahi_entry_group_free(impl_->group);
    impl_->group = nullptr;
  }
  if (impl_->client) {
    avahi_client_free(impl_->client);
    impl_->client = nullptr;
  }
  if (impl_->poll) {
    avahi_threaded_poll_free(impl_->poll);
    impl_->poll = nullptr;
  }
  impl_->running.store(false);
  impl_->port = 0;
  impl_->instance_name.clear();
}

bool VrMdnsAdvertiser::is_running() const {
  return impl_ && impl_->running.load();
}

}  // namespace trossen::hw::vr
