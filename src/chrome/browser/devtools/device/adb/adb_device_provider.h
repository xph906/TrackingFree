// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_DEVTOOLS_DEVICE_ADB_ADB_DEVICE_PROVIDER_H_
#define CHROME_BROWSER_DEVTOOLS_DEVICE_ADB_ADB_DEVICE_PROVIDER_H_

#include "chrome/browser/devtools/device/android_device_manager.h"

class AdbDeviceProvider : public AndroidDeviceManager::DeviceProvider {
 public:
  typedef DeviceProvider::QueryDevicesCallback QueryDevicesCallback;

  virtual void QueryDevices(const QueryDevicesCallback& callback) OVERRIDE;

 private:
  virtual ~AdbDeviceProvider();
};

#endif  // CHROME_BROWSER_DEVTOOLS_DEVICE_ADB_ADB_DEVICE_PROVIDER_H_
