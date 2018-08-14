/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#include "config.h"

#include "audit.h"
#include "auditd_audit_events.h"
#include "configureevent.h"
#include "event.h"
#include "eventdescriptor.h"

#include <cJSON.h>
#include <logger/logger.h>
#include <memcached/isotime.h>
#include <nlohmann/json.hpp>
#include <platform/dirutils.h>
#include <utilities/logtags.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <queue>
#include <sstream>
#include <string>

AuditImpl::AuditImpl(std::string config_file,
                     SERVER_COOKIE_API* sapi,
                     const std::string& host)
    : Audit(),
      auditfile(host),
      configfile(std::move(config_file)),
      cookie_api(sapi),
      hostname(host) {
    if (!configfile.empty() && !configure()) {
        throw std::runtime_error(
                "Audit::Audit(): Failed to configure audit daemon");
    }

    std::unique_lock<std::mutex> lock(producer_consumer_lock);
    if (cb_create_named_thread(
                &consumer_tid,
                [](void* audit) {
                    static_cast<AuditImpl*>(audit)->consume_events();
                },
                this,
                0,
                "mc:auditd") != 0) {
        throw std::runtime_error("Failed to create audit thread");
    }
    events_arrived.wait(lock);
}

AuditImpl::~AuditImpl() {
    nlohmann::json payload;
    create_audit_event(AUDITD_AUDIT_SHUTTING_DOWN_AUDIT_DAEMON, payload);
    put_event(AUDITD_AUDIT_SHUTTING_DOWN_AUDIT_DAEMON, payload.dump());

    {
        // Set the flag to request the audit consumer to stop
        std::lock_guard<std::mutex> guard(producer_consumer_lock);
        stop_audit_consumer = true;
        // The consume thread may be waiting on the condition variable
        // so we should kick it in the but as well to make sure that
        // it is let loose
        events_arrived.notify_all();
    }

    // Wait for the consumer thread to stop
    cb_join_thread(consumer_tid);
}

void AuditImpl::create_audit_event(uint32_t event_id, nlohmann::json& payload) {
    // Add common fields to the audit event
    payload["timestamp"] = ISOTime::generatetimestamp();
    nlohmann::json real_userid;
    real_userid["domain"] = "internal";
    real_userid["user"] = "couchbase";
    payload["real_userid"] = real_userid;

    switch (event_id) {
        case AUDITD_AUDIT_CONFIGURED_AUDIT_DAEMON:
            payload["auditd_enabled"] = config.is_auditd_enabled();
            payload["descriptors_path"] = config.get_descriptors_path();
            payload["hostname"] = hostname;
            payload["log_path"] = config.get_log_directory();
            payload["rotate_interval"] = config.get_rotate_interval();
            payload["version"] = config.get_version();
            payload["uuid"] = config.get_uuid();
            return;

        case AUDITD_AUDIT_SHUTTING_DOWN_AUDIT_DAEMON:
            return;
    }

    throw std::logic_error(
            "Audit::create_audit_event: Invalid event identifier specified");
}

bool AuditImpl::add_event_descriptor(cJSON* event_ptr) {
    if (event_ptr == nullptr) {
        LOG_WARNING(
                "Audit::add_event_descriptor: No JSON data "
                "provided");
        return false;
    }

    try {
        auto entry = std::make_unique<EventDescriptor>(event_ptr);
        events.insert(std::pair<uint32_t, std::unique_ptr<EventDescriptor>>(
                entry->getId(), std::move(entry)));
        return true;
    } catch (const std::bad_alloc&) {
        LOG_WARNING(
                "Audit::add_event_descriptor: Failed to allocate "
                "memory");
    } catch (const std::logic_error& le) {
        LOG_WARNING(R"(Audit::add_event_descriptor: JSON key "{}" error)",
                    le.what());
    }

    return false;
}

bool AuditImpl::process_module_data_structures(cJSON* module) {
    if (module == NULL) {
        LOG_WARNING(
                "Audit::process_module_data_structures: No JSON data provided "
                "for module");
        return false;
    }
    while (module != NULL) {
        cJSON *mod_ptr = module->child;
        if (mod_ptr == NULL) {
            LOG_WARNING(
                    "Audit::process_module_data_structures: No JSON data "
                    "provided for child node");
            return false;
        }
        while (mod_ptr != NULL) {
            cJSON *event_ptr;
            switch (mod_ptr->type) {
                case cJSON_Number:
                case cJSON_String:
                    break;
                case cJSON_Array:
                    event_ptr = mod_ptr->child;
                    while (event_ptr != NULL) {
                        if (!add_event_descriptor(event_ptr)) {
                            return false;
                        }
                        event_ptr = event_ptr->next;
                    }
                    break;
                default:
                    LOG_WARNING("Audit: JSON unknown field error");
                    return false;
            }
            mod_ptr = mod_ptr->next;
        }
        module = module->next;
    }
    return true;
}

bool AuditImpl::process_module_descriptor(cJSON* module_descriptor) {
    events.clear();
    while(module_descriptor != NULL) {
        switch (module_descriptor->type) {
            case cJSON_Number:
                break;
            case cJSON_Array:
                if (!process_module_data_structures(module_descriptor->child)) {
                    return false;
                }
                break;
            default:
                LOG_WARNING("Audit: JSON unknown field error");
                return false;
        }
        module_descriptor = module_descriptor->next;
    }
    return true;
}

bool AuditImpl::reconfigure(std::string file) {
    configfile = std::move(file);
    return configure();
}

bool AuditImpl::configure() {
    bool is_enabled_before_reconfig = config.is_auditd_enabled();
    const auto configuration = cb::io::loadFile(configfile);
    if (configuration.empty()) {
        return false;
    }

    nlohmann::json config_json;
    try {
        config_json = nlohmann::json::parse(configuration);
    } catch (const nlohmann::json::exception&) {
        LOG_WARNING(
                R"(Audit::configure: JSON parsing error of "{}" with content: "{}")",
                configfile,
                cb::UserDataView(configuration));
        return false;
    }

    bool failure = false;
    try {
        config.initialize_config(config_json);
    } catch (std::string &msg) {
        LOG_WARNING("Audit::configure: Invalid input: {}", msg);
        failure = true;
    } catch (...) {
        LOG_WARNING("Audit::configure: Invalid input");
        failure = true;
    }
    if (failure) {
        return false;
    }

    if (!auditfile.is_open()) {
        try {
            auditfile.cleanup_old_logfile(config.get_log_directory());
        } catch (const std::exception& exception) {
            LOG_WARNING(
                    "Audit::configure(): Failed to clean up old log files: {}",
                    exception.what());
            return false;
        }
    }
    auto audit_events_file =
            config.get_descriptors_path() + "/audit_events.json";
    cb::io::sanitizePath(audit_events_file);
    const auto str = cb::io::loadFile(audit_events_file);
    if (str.empty()) {
        return false;
    }
    cJSON *json_ptr = cJSON_Parse(str.c_str());
    if (json_ptr == NULL) {
        LOG_WARNING(
                R"(Audit::configure: JSON parsing error of "{}" with the content: "{}")",
                audit_events_file,
                cb::UserDataView(str));
        return false;
    }
    if (!process_module_descriptor(json_ptr->child)) {
        cJSON_Delete(json_ptr);
        return false;
    }
    cJSON_Delete(json_ptr);

    auditfile.reconfigure(config);

    // iterate through the events map and update the sync and enabled flags
    for (const auto& event : events) {
        event.second->setSync(config.is_event_sync(event.first));
        // If the event has a state defined then use that
        AuditConfig::EventState state = config.get_event_state(event.first);
        switch (state) {
        case AuditConfig::EventState::enabled:
            event.second->setEnabled(true);
            break;
        case AuditConfig::EventState::disabled:
            event.second->setEnabled(false);
            break;
        case AuditConfig::EventState::undefined:
            // No state defined for the event so don't do anything
            break;
        }
    }

    /*
     * We need to notify if the audit daemon is turned on or off during a
     * reconfigure.  It is also possible that particular audit events may
     * have been enabled or disabled.  Therefore we want to notify all of the
     * current event states whenever we do a reconfigure.
     */
    notify_all_event_states();

    // create event to say done reconfiguration
    if (is_enabled_before_reconfig || config.is_auditd_enabled()) {
        auto evt = events.find(AUDITD_AUDIT_CONFIGURED_AUDIT_DAEMON);
        if (evt == events.end()) {
            LOG_WARNING(
                    "Audit: error: Failed to locate descriptor for event id: "
                    "{}",
                    AUDITD_AUDIT_CONFIGURED_AUDIT_DAEMON);
        } else {
            if (evt->second->isEnabled()) {
                nlohmann::json payload;
                try {
                    create_audit_event(AUDITD_AUDIT_CONFIGURED_AUDIT_DAEMON,
                                       payload);
                    payload["id"] = AUDITD_AUDIT_CONFIGURED_AUDIT_DAEMON;
                    payload["name"] = evt->second->getName();
                    payload["description"] = evt->second->getDescription();

                    if (!(auditfile.ensure_open() && auditfile.write_event_to_disk(payload))) {
                        dropped_events++;
                    }
                } catch (const std::exception& exception) {
                    dropped_events++;
                    LOG_WARNING(
                            "Audit::configure(): Failed to add audit event for "
                            "audit configure: {}",
                            exception.what());
                }
            }
        }
    }

    if (config.is_auditd_enabled()) {
        // If the write_event_to_disk function returns false then it is
        // possible the audit file has been closed.  Therefore ensure
        // the file is open.
        auditfile.ensure_open();
    } else {
        // Audit is disabled, ensure that the audit file is closed
        auditfile.close();
    }

    return true;
}

bool AuditImpl::put_event(uint32_t event_id, cb::const_char_buffer payload) {
    if (!config.is_auditd_enabled()) {
        // Audit is disabled
        return true;
    }

    // @todo I think we should do full validation of the content
    //       in debug mode to ensure that developers actually fill
    //       in the correct fields.. if not we should add an
    //       event to the audit trail saying it is one in an illegal
    //       format (or missing fields)
    try {
        auto new_event = std::make_unique<Event>(event_id, payload);
        std::lock_guard<std::mutex> guard(producer_consumer_lock);
        if (filleventqueue.size() < max_audit_queue) {
            filleventqueue.push(std::move(new_event));
            events_arrived.notify_all();
            return true;
        }
    } catch (const std::bad_alloc&) {
    }

    dropped_events++;
    LOG_WARNING("Audit: Dropping audit event {}: {}",
                event_id,
                cb::UserDataView(payload));
    return false;
}

bool AuditImpl::configure_auditdaemon(const std::string& configfile,
                                      gsl::not_null<const void*> cookie) {
    auto new_event = std::make_unique<ConfigureEvent>(configfile, cookie.get());
    std::lock_guard<std::mutex> guard(producer_consumer_lock);
    filleventqueue.push(std::move(new_event));
    events_arrived.notify_all();
    return true;
}

void AuditImpl::notify_all_event_states() {
    notify_event_state_changed(0, config.is_auditd_enabled());
    for (const auto& event : events) {
        notify_event_state_changed(event.first, event.second->isEnabled());
    }
}

void AuditImpl::add_event_state_listener(
        cb::audit::EventStateListener listener) {
    std::lock_guard<std::mutex> guard(event_state_listener.mutex);
    event_state_listener.clients.push_back(listener);
}

void AuditImpl::notify_event_state_changed(uint32_t id, bool enabled) const {
    std::lock_guard<std::mutex> guard(event_state_listener.mutex);
    for (const auto& func : event_state_listener.clients) {
        func(id, enabled);
    }
}

void AuditImpl::notify_io_complete(gsl::not_null<const void*> cookie,
                                   ENGINE_ERROR_CODE status) {
    cookie_api->notify_io_complete(cookie, status);
}

void AuditImpl::stats(ADD_STAT add_stats, gsl::not_null<const void*> cookie) {
    const auto* enabled = config.is_auditd_enabled() ? "true" : "false";
    add_stats("enabled",
              (uint16_t)strlen("enabled"),
              enabled,
              (uint32_t)strlen(enabled),
              cookie.get());
    const auto num_of_dropped_events = std::to_string(dropped_events);
    add_stats("dropped_events",
              (uint16_t)strlen("dropped_events"),
              num_of_dropped_events.data(),
              (uint32_t)num_of_dropped_events.length(),
              cookie.get());
}

void AuditImpl::consume_events() {
    std::unique_lock<std::mutex> lock(producer_consumer_lock);
    // Tell the main thread that we're up and running
    events_arrived.notify_one();

    while (!stop_audit_consumer) {
        if (filleventqueue.empty()) {
            events_arrived.wait_for(
                    lock,
                    std::chrono::seconds(auditfile.get_seconds_to_rotation()));
            if (filleventqueue.empty()) {
                // We timed out, so just rotate the files
                if (auditfile.maybe_rotate_files()) {
                    // If the file was rotated then we need to open a new
                    // audit.log file.
                    auditfile.ensure_open();
                }
            }
        }
        /* now have producer_consumer lock!
         * event(s) have arrived or shutdown requested
         */
        processeventqueue.swap(filleventqueue);
        lock.unlock();
        // Now outside of the producer_consumer_lock

        while (!processeventqueue.empty()) {
            auto& event = processeventqueue.front();
            if (!event->process(*this)) {
                dropped_events++;
            }
            processeventqueue.pop();
        }
        auditfile.flush();
        lock.lock();
    }

    // close the auditfile
    auditfile.close();
}
