/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <cstdlib>
#include <sstream>

#include <boost/property_tree/json_parser.hpp>

#include <osquery/extensions.h>
#include <osquery/logger.h>
#include <osquery/registry.h>

namespace osquery {

void RegistryHelperCore::remove(const std::string& item_name) {
  if (items_.count(item_name) > 0) {
    items_[item_name]->tearDown();
    items_.erase(item_name);
  }

  // Populate list of aliases to remove (those that mask item_name).
  std::vector<std::string> removed_aliases;
  for (const auto& alias : aliases_) {
    if (alias.second == item_name) {
      removed_aliases.push_back(alias.first);
    }
  }

  for (const auto& alias : removed_aliases) {
    aliases_.erase(alias);
  }
}

RegistryRoutes RegistryHelperCore::getRoutes() const {
  RegistryRoutes route_table;
  for (const auto& item : items_) {
    if (std::find(internal_.begin(), internal_.end(), item.first) !=
        internal_.end()) {
      // This is an internal plugin, do not include the route.
      continue;
    }

    bool has_alias = false;
    for (const auto& alias : aliases_) {
      if (alias.second == item.first) {
        // If the item name is masked by at least one alias, it will not
        // broadcast under the internal item name.
        route_table[alias.first] = item.second->routeInfo();
        has_alias = true;
      }
    }

    if (!has_alias) {
      route_table[item.first] = item.second->routeInfo();
    }
  }
  return route_table;
}

Status RegistryHelperCore::call(const std::string& item_name,
                                const PluginRequest& request,
                                PluginResponse& response) {
  if (items_.count(item_name) > 0) {
    return items_.at(item_name)->call(request, response);
  }

  if (external_.count(item_name) > 0) {
    // The item is a registered extension, call the extension by UUID.
    return callExtension(external_.at(item_name), name_, item_name, request,
                         response);
  } else if (routes_.count(item_name) > 0) {
    // The item has a route, but no extension, pass in the route info.
    response = routes_.at(item_name);
    return Status(0, "Route only");
  }

  return Status(1, "Cannot call registry item: " + item_name);
}

Status RegistryHelperCore::addAlias(const std::string& item_name,
                                    const std::string& alias) {
  if (aliases_.count(alias) > 0) {
    return Status(1, "Duplicate alias: " + alias);
  }
  aliases_[alias] = item_name;
  return Status(0, "OK");
}

const std::string& RegistryHelperCore::getAlias(
    const std::string& alias) const {
  if (aliases_.count(alias) == 0) {
    return alias;
  }
  return aliases_.at(alias);
}

void RegistryHelperCore::setUp() {
  // If this registry does not auto-setup do NOT setup the registry items.
  if (!auto_setup_) {
    return;
  }

  // Try to set up each of the registry items.
  // If they fail, remove them from the registry.
  std::vector<std::string> failed;
  for (auto& item : items_) {
    if (!item.second->setUp().ok()) {
      failed.push_back(item.first);
    }
  }

  for (const auto& failed_item : failed) {
    remove(failed_item);
  }
}

/// Facility method to check if a registry item exists.
bool RegistryHelperCore::exists(const std::string& item_name,
                                bool local) const {
  bool has_local = (items_.count(item_name) > 0);
  bool has_external = (external_.count(item_name) > 0);
  bool has_route = (routes_.count(item_name) > 0);
  return (local) ? has_local : has_local || has_external || has_route;
}

/// Facility method to list the registry item identifiers.
std::vector<std::string> RegistryHelperCore::names() const {
  std::vector<std::string> names;
  for (const auto& item : items_) {
    names.push_back(item.first);
  }

  // Also add names of external plugins.
  for (const auto& item : external_) {
    names.push_back(item.first);
  }
  return names;
}

/// Facility method to count the number of items in this registry.
size_t RegistryHelperCore::count() const { return items_.size(); }

/// Allow the registry to introspect into the registered name (for logging).
void RegistryHelperCore::setName(const std::string& name) { name_ = name; }

const std::map<std::string, PluginRegistryHelperRef>& RegistryFactory::all() {
  return instance().registries_;
}

PluginRegistryHelperRef RegistryFactory::registry(
    const std::string& registry_name) {
  return instance().registries_.at(registry_name);
}

const std::map<std::string, PluginRef> RegistryFactory::all(
    const std::string& registry_name) {
  return instance().registry(registry_name)->all();
}

PluginRef RegistryFactory::get(const std::string& registry_name,
                               const std::string& item_name) {
  return instance().registry(registry_name)->get(item_name);
}

RegistryBroadcast RegistryFactory::getBroadcast() {
  RegistryBroadcast broadcast;
  for (const auto& registry : instance().registries_) {
    broadcast[registry.first] = registry.second->getRoutes();
  }
  return broadcast;
}

Status RegistryFactory::addBroadcast(const RouteUUID& uuid,
                                     const RegistryBroadcast& broadcast) {
  if (instance().extensions_.count(uuid) > 0) {
    return Status(1, "Duplicate extension UUID: " + std::to_string(uuid));
  }

  // Make sure the extension does not broadcast conflicting registry items.
  if (!Registry::allowDuplicates()) {
    for (const auto& registry : broadcast) {
      for (const auto& item : registry.second) {
        if (Registry::exists(registry.first, item.first)) {
          VLOG(1) << "Extension " << uuid
                  << " has duplicate plugin name: " << item.first
                  << " in registry: " << registry.first;
          return Status(1, "Duplicate registry item: " + item.first);
        }
      }
    }
  }

  // Once duplication is satisfied call each registry's addExternal.
  Status status;
  for (const auto& registry : broadcast) {
    status = RegistryFactory::registry(registry.first)
                 ->addExternal(uuid, registry.second);
    if (!status.ok()) {
      // If any registry fails to add the set of external routes, stop.
      break;
    }
  }

  // If any registry failed, remove each (assume a broadcast is atomic).
  if (!status.ok()) {
    for (const auto& registry : broadcast) {
      Registry::registry(registry.first)->removeExternal(uuid);
    }
  }
  instance().extensions_.insert(uuid);
  return status;
}

Status RegistryFactory::removeBroadcast(const RouteUUID& uuid) {
  if (instance().extensions_.count(uuid) == 0) {
    return Status(1, "Unknown extension UUID: " + std::to_string(uuid));
  }

  for (const auto& registry : instance().registries_) {
    registry.second->removeExternal(uuid);
  }
  instance().extensions_.erase(uuid);
  return Status(0, "OK");
}

/// Adds an alias for an internal registry item. This registry will only
/// broadcast the alias name.
Status RegistryFactory::addAlias(const std::string& registry_name,
                                 const std::string& item_name,
                                 const std::string& alias) {
  if (instance().registries_.count(registry_name) == 0) {
    return Status(1, "Unknown registry: " + registry_name);
  }
  return instance().registries_.at(registry_name)->addAlias(item_name, alias);
}

/// Returns the item_name or the item alias if an alias exists.
const std::string& RegistryFactory::getAlias(const std::string& registry_name,
                                             const std::string& alias) {
  if (instance().registries_.count(registry_name) == 0) {
    return alias;
  }
  return instance().registries_.at(registry_name)->getAlias(alias);
}

Status RegistryFactory::call(const std::string& registry_name,
                             const std::string& item_name,
                             const PluginRequest& request,
                             PluginResponse& response) {
  if (!exists(registry_name, item_name)) {
    return Status(1, "Registry: " + registry_name + ", item: " + item_name +
                         " not found");
  }
  // Forward factory call to the registry.
  return registry(registry_name)->call(item_name, request, response);
}

Status RegistryFactory::call(const std::string& registry_name,
                             const std::string& item_name,
                             const PluginRequest& request) {
  PluginResponse response;
  // Wrapper around a call expecting a response.
  return call(registry_name, item_name, request, response);
}

void RegistryFactory::setUp() {
  for (const auto& registry : instance().all()) {
    registry.second->setUp();
  }
}

bool RegistryFactory::exists(const std::string& registry_name,
                             const std::string& item_name,
                             bool local) {
  if (instance().registries_.count(registry_name) == 0) {
    return false;
  }

  // Check the registry.
  return registry(registry_name)->exists(item_name, local);
}

std::vector<std::string> RegistryFactory::names(
    const std::string& registry_name) {
  if (instance().registries_.at(registry_name) == 0) {
    std::vector<std::string> names;
    return names;
  }
  return instance().registry(registry_name)->names();
}

std::vector<RouteUUID> RegistryFactory::routeUUIDs() {
  std::vector<RouteUUID> uuids;
  for (const auto& extension : instance().extensions_) {
    uuids.push_back(extension);
  }
  return uuids;
}

size_t RegistryFactory::count() { return instance().registries_.size(); }

size_t RegistryFactory::count(const std::string& registry_name) {
  if (instance().registries_.count(registry_name) == 0) {
    return 0;
  }
  return instance().registry(registry_name)->count();
}

void Plugin::getResponse(const std::string& key,
                         const PluginResponse& response,
                         boost::property_tree::ptree& tree) {
  for (const auto& item : response) {
    boost::property_tree::ptree child;
    for (const auto& item_detail : item) {
      child.put(item_detail.first, item_detail.second);
    }
    tree.add_child(key, child);
  }
}

void Plugin::setResponse(const std::string& key,
                         const boost::property_tree::ptree& tree,
                         PluginResponse& response) {
  std::ostringstream output;
  boost::property_tree::write_json(output, tree, false);
  response.push_back({{key, output.str()}});
}
}
