/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#pragma once

#include <osquery/core.h>
#include <osquery/flags.h>
#include <osquery/sql.h>

namespace osquery {

DECLARE_int32(worker_threads);
DECLARE_string(extensions_socket);

/**
 * @brief Helper struct for managing extenion metadata.
 *
 * This structure should match the members of Thrift's InternalExtensionInfo.
 */
struct ExtensionInfo {
  std::string name;
  std::string version;
  std::string min_sdk_version;
  std::string sdk_version;
};

typedef std::map<RouteUUID, ExtensionInfo> ExtensionList;

inline std::string getExtensionSocket(
    RouteUUID uuid, const std::string& path = FLAGS_extensions_socket) {
  if (uuid == 0) {
    return path;
  } else {
    return path + "." + std::to_string(uuid);
  }
}

/// External (extensions) SQL implementation of the osquery query API.
Status queryExternal(const std::string& query, QueryData& results);

/// External (extensions) SQL implementation of the osquery getQueryColumns API.
Status getQueryColumnsExternal(const std::string& q,
                               tables::TableColumns& columns);

/// External (extensions) SQL implementation plugin provider for "sql" registry.
class ExternalSQLPlugin : SQLPlugin {
 public:
  Status query(const std::string& q, QueryData& results) const {
    return queryExternal(q, results);
  }

  Status getQueryColumns(const std::string& q,
                         tables::TableColumns& columns) const {
    return getQueryColumnsExternal(q, columns);
  }
};

/// Status get a list of active extenions.
Status getExtensions(ExtensionList& extensions);

/// Internal getExtensions using a UNIX domain socket path.
Status getExtensions(const std::string& manager_path,
                     ExtensionList& extensions);

/// Ping an extension manager or extension.
Status pingExtension(const std::string& path);

/**
 * @brief Call a Plugin exposed by an Extension Registry route.
 *
 * This is mostly a Registry%-internal method used to call an ExtensionHandler
 * call API if a Plugin is requested and had matched an Extension route.
 *
 * @param uuid Route UUID of the matched Extension
 * @param registry The string name for the registry.
 * @param item A string identifier for this registry item.
 * @param request The plugin request input.
 * @param response The plugin response output.
 * @return Success indicates Extension API call success and Extension's
 * Registry::call success.
 */
Status callExtension(const RouteUUID uuid,
                     const std::string& registry,
                     const std::string& item,
                     const PluginRequest& request,
                     PluginResponse& response);

/// Internal callExtension implementation using a UNIX domain socket path.
Status callExtension(const std::string& extension_path,
                     const std::string& registry,
                     const std::string& item,
                     const PluginRequest& request,
                     PluginResponse& response);

/// The main runloop entered by an Extension, start an ExtensionRunner thread.
Status startExtension(const std::string& name, const std::string& version);

/// The main runloop entered by an Extension, start an ExtensionRunner thread.
Status startExtension(const std::string& name,
                      const std::string& version,
                      const std::string& min_sdk_version);

/// Internal startExtension implementation using a UNIX domain socket path.
Status startExtension(const std::string& manager_path,
                      const std::string& name,
                      const std::string& version,
                      const std::string& min_sdk_version,
                      const std::string& sdk_version);

/// Start an ExtensionWatcher thread.
Status startExtensionWatcher(const std::string& manager_path,
                             size_t interval,
                             bool fatal);

/// Start an ExtensionManagerRunner thread.
Status startExtensionManager();

/// Internal startExtensionManager implementation.
Status startExtensionManager(const std::string& manager_path);
}
