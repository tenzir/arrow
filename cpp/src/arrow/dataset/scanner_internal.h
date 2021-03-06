// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <memory>
#include <utility>

#include "arrow/dataset/dataset_internal.h"
#include "arrow/dataset/filter.h"
#include "arrow/dataset/partition.h"
#include "arrow/dataset/scanner.h"

namespace arrow {
namespace dataset {

inline RecordBatchIterator FilterRecordBatch(RecordBatchIterator it,
                                             const ExpressionEvaluator& evaluator,
                                             const Expression& filter, MemoryPool* pool) {
  return MakeMaybeMapIterator(
      [&filter, &evaluator, pool](std::shared_ptr<RecordBatch> in) {
        return evaluator.Evaluate(filter, *in, pool).Map([&](compute::Datum selection) {
          return evaluator.Filter(selection, in);
        });
      },
      std::move(it));
}

inline RecordBatchIterator ProjectRecordBatch(RecordBatchIterator it,
                                              RecordBatchProjector* projector,
                                              MemoryPool* pool) {
  return MakeMaybeMapIterator(
      [=](std::shared_ptr<RecordBatch> in) {
        // The RecordBatchProjector is shared accross ScanTasks of the same
        // Fragment. The resize operation of missing columns is not thread safe.
        // Ensure that each ScanTask gets his own projector.
        RecordBatchProjector local_projector{*projector};
        return local_projector.Project(*in, pool);
      },
      std::move(it));
}

class FilterAndProjectScanTask : public ScanTask {
 public:
  explicit FilterAndProjectScanTask(std::shared_ptr<ScanTask> task,
                                    std::shared_ptr<Expression> partition)
      : ScanTask(task->options(), task->context()),
        task_(std::move(task)),
        partition_(std::move(partition)),
        filter_(NULLPTR),
        projector_(options()->projector) {}

  Result<RecordBatchIterator> Execute() override {
    ARROW_ASSIGN_OR_RAISE(auto it, task_->Execute());

    filter_ = options()->filter->Assume(partition_);
    auto filter_it =
        FilterRecordBatch(std::move(it), *options_->evaluator, *filter_, context_->pool);

    if (partition_) {
      RETURN_NOT_OK(
          KeyValuePartitioning::SetDefaultValuesFromKeys(*partition_, &projector_));
    }
    return ProjectRecordBatch(std::move(filter_it), &projector_, context_->pool);
  }

 private:
  std::shared_ptr<ScanTask> task_;
  std::shared_ptr<Expression> partition_;
  std::shared_ptr<Expression> filter_;
  RecordBatchProjector projector_;
};

/// \brief GetScanTaskIterator transforms an Iterator<Fragment> in a
/// flattened Iterator<ScanTask>.
inline ScanTaskIterator GetScanTaskIterator(FragmentIterator fragments,
                                            std::shared_ptr<ScanOptions> options,
                                            std::shared_ptr<ScanContext> context) {
  // Fragment -> ScanTaskIterator
  auto fn = [options,
             context](std::shared_ptr<Fragment> fragment) -> Result<ScanTaskIterator> {
    ARROW_ASSIGN_OR_RAISE(auto scan_task_it, fragment->Scan(options, context));

    auto partition = fragment->partition_expression();
    // Apply the filter and/or projection to incoming RecordBatches by
    // wrapping the ScanTask with a FilterAndProjectScanTask
    auto wrap_scan_task =
        [partition](std::shared_ptr<ScanTask> task) -> std::shared_ptr<ScanTask> {
      return std::make_shared<FilterAndProjectScanTask>(std::move(task),
                                                        std::move(partition));
    };

    return MakeMapIterator(wrap_scan_task, std::move(scan_task_it));
  };

  // Iterator<Iterator<ScanTask>>
  auto maybe_scantask_it = MakeMaybeMapIterator(fn, std::move(fragments));

  // Iterator<ScanTask>
  return MakeFlattenIterator(std::move(maybe_scantask_it));
}

}  // namespace dataset
}  // namespace arrow
