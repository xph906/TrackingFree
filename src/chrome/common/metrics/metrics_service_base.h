// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_COMMON_METRICS_METRICS_SERVICE_BASE_H_
#define CHROME_COMMON_METRICS_METRICS_SERVICE_BASE_H_

#include "base/basictypes.h"
#include "base/metrics/histogram_base.h"
#include "base/metrics/histogram_flattener.h"
#include "base/metrics/histogram_snapshot_manager.h"
#include "components/metrics/metrics_log_manager.h"

namespace base {
class HistogramSamples;
}  // namespace base

// This class provides base functionality for logging metrics data.
// TODO(ananta): Factor out more common code from chrome and chrome frame
// metrics service into this class.
class MetricsServiceBase : public base::HistogramFlattener {
 public:
  // HistogramFlattener interface (override) methods.
  virtual void RecordDelta(const base::HistogramBase& histogram,
                           const base::HistogramSamples& snapshot) OVERRIDE;
  virtual void InconsistencyDetected(
      base::HistogramBase::Inconsistency problem) OVERRIDE;
  virtual void UniqueInconsistencyDetected(
      base::HistogramBase::Inconsistency problem) OVERRIDE;
  virtual void InconsistencyDetectedInLoggedCount(int amount) OVERRIDE;

 protected:
  // The metrics service will persist it's unsent logs by storing them in
  // |local_state|, and will not persist ongoing logs over
  // |max_ongoing_log_size|.
  MetricsServiceBase(PrefService* local_state, size_t max_ongoing_log_size);
  virtual ~MetricsServiceBase();

  // The metrics server's URL.
  static const char kServerUrl[];

  // The MIME type for the uploaded metrics data.
  static const char kMimeType[];

  // Record complete list of histograms into the current log.
  // Called when we close a log.
  void RecordCurrentHistograms();

  // Record complete list of stability histograms into the current log,
  // i.e., histograms with the |kUmaStabilityHistogramFlag| flag set.
  void RecordCurrentStabilityHistograms();

  // Manager for the various in-flight logs.
  metrics::MetricsLogManager log_manager_;

 private:
  // |histogram_snapshot_manager_| prepares histogram deltas for transmission.
  base::HistogramSnapshotManager histogram_snapshot_manager_;

  DISALLOW_COPY_AND_ASSIGN(MetricsServiceBase);
};

#endif  // CHROME_COMMON_METRICS_METRICS_SERVICE_BASE_H_
