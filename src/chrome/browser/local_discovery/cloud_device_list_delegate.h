// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_LOCAL_DISCOVERY_CLOUD_DEVICE_LIST_DELEGATE_H_
#define CHROME_BROWSER_LOCAL_DISCOVERY_CLOUD_DEVICE_LIST_DELEGATE_H_

#include <string>

namespace local_discovery {

class CloudDeviceListDelegate {
 public:
  static const char kDeviceTypePrinter[];

  CloudDeviceListDelegate();
  virtual ~CloudDeviceListDelegate();

  struct Device {
    Device();
    ~Device();

    std::string id;
    std::string type;
    std::string display_name;
    std::string description;
  };

  virtual void OnDeviceListReady() = 0;
  virtual void OnDeviceListUnavailable() = 0;
};

}  // namespace local_discovery

#endif  // CHROME_BROWSER_LOCAL_DISCOVERY_CLOUD_DEVICE_LIST_DELEGATE_H_
