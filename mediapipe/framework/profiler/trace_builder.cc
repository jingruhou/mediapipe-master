// Copyright 2018 The MediaPipe Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mediapipe/framework/profiler/trace_builder.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "mediapipe/framework/calculator_profile.pb.h"
#include "mediapipe/framework/packet.h"
#include "mediapipe/framework/port/integral_types.h"
#include "mediapipe/framework/timestamp.h"

namespace mediapipe {
// Each calculator task is identified by node_id, input_ts, and event_type.
// Each stream hop is identified by stream_id, packet_ts, and event_type.
struct TaskId {
  int id;
  mediapipe::Timestamp ts;
  int event_type;
  inline bool operator==(const TaskId& other) const {
    return id == other.id && ts == other.ts && event_type == other.event_type;
  }
  inline size_t hash() const { return id + ts.Value() + (event_type << 10); }
};
}  // namespace mediapipe

namespace std {
// std::hash specialization for TaskId.
template <>
struct hash<mediapipe::TaskId> {
  std::size_t operator()(const mediapipe::TaskId& task_id) const {
    return task_id.hash();
  }
};
}  // namespace std

namespace mediapipe {

namespace {

// For each event-type, whether packet details are logged.
// The event-types are:
//   UNKNOWN, OPEN, PROCESS, CLOSE,
//   NOT_READY, READY_FOR_PROCESS, READY_FOR_CLOSE, THROTTLED, UNTHROTTLED
//   CPU_TASK_USER, CPU_TASK_SYSTEM, GPU_TASK, DSP_TASK, TPU_TASK
constexpr bool kProfilerPacketEvents[] = {  //
    false, true,  true,  true,              //
    false, false, false, false, false,      //
    true,  true,  true,  true,  true};

// For each calculator method, whether StreamTraces are desired.
constexpr bool kProfilerStreamEvents[] = {  //
    false, true,  true,  true,              //
    false, false, false, false, false,      //
    true,  true,  false, false, false};

// A map defining int32 identifiers for std::string object pointers.
// Lookup is fast when the same std::string object is used frequently.
class StringIdMap {
 public:
  // Returns the int32 identifier for a std::string object pointer.
  int32 operator[](const std::string* id) {
    if (id == nullptr) {
      return 0;
    }
    auto pointer_id = pointer_id_map_.find(id);
    if (pointer_id != pointer_id_map_.end()) {
      return pointer_id->second;
    }
    auto string_id = string_id_map_.find(*id);
    if (string_id == string_id_map_.end()) {
      string_id_map_[*id] = next_id++;
      string_id = string_id_map_.find(*id);
    }
    pointer_id_map_[id] = string_id->second;
    return string_id->second;
  }
  void clear() { pointer_id_map_.clear(), string_id_map_.clear(); }
  const std::unordered_map<std::string, int32>& map() { return string_id_map_; }

 private:
  std::unordered_map<const std::string*, int32> pointer_id_map_;
  std::unordered_map<std::string, int32> string_id_map_;
  int32 next_id = 0;
};

// A map defining int32 identifiers for object pointers.
class AddressIdMap {
 public:
  int32 operator[](const void* id) {
    auto pointer_id = pointer_id_map_.find(id);
    if (pointer_id != pointer_id_map_.end()) {
      return pointer_id->second;
    }
    return pointer_id_map_[id] = next_id++;
  }
  void clear() { pointer_id_map_.clear(); }
  const std::unordered_map<const void*, int32>& map() {
    return pointer_id_map_;
  }

 private:
  std::unordered_map<const void*, int32> pointer_id_map_;
  int32 next_id = 0;
};

// Returns a vector of id names indexed by id.
std::vector<std::string> GetIdNames(StringIdMap id_map) {
  std::vector<std::string> result;
  for (const auto& it : id_map.map()) {
    result.resize(std::max(result.size(), static_cast<size_t>(it.second + 1)));
    result[it.second] = it.first;
  }
  return result;
}

}  // namespace

// Builds a GraphTrace for packets over a range of timestamps.
class TraceBuilder::Impl {
  using EventList = std::vector<const TraceEvent*>;

 public:
  Impl() {
    // Define the zero id's.  Id 0 is reserved to indicate "unassigned" as
    // required by proto3.  Also, id 0 is used to represent any unspecified
    // stream, node, or packet.
    static std::string* empty_string = new std::string("");
    stream_id_map_[empty_string];
    packet_data_id_map_[0];
  }

  static Timestamp TimestampAfter(const TraceBuffer& buffer,
                                  absl::Time begin_time) {
    Timestamp max_ts = Timestamp::Min();
    for (auto iter = buffer.begin(); iter < buffer.end(); ++iter) {
      TraceEvent event = *iter;
      if (event.event_time >= begin_time) break;
      max_ts = std::max(max_ts, event.input_ts);
    }
    return max_ts + 1;
  }

  void CreateTrace(const TraceBuffer& buffer, absl::Time begin_time,
                   absl::Time end_time, GraphTrace* result) {
    // Snapshot recent TraceEvents
    std::vector<TraceEvent> snapshot;
    snapshot.reserve(10000);
    TraceBuffer::iterator buffer_end = buffer.end();
    for (auto iter = buffer.begin(); iter < buffer_end; ++iter) {
      TraceEvent event = *iter;
      if (event.event_time >= begin_time && event.event_time < end_time) {
        snapshot.push_back(event);
      }
    }
    SetBaseTime(snapshot);

    // Index TraceEvents by task-id and stream-hop-id.
    for (const TraceEvent& event : snapshot) {
      if (!kProfilerPacketEvents[static_cast<int>(event.event_type)]) {
        continue;
      }
      TaskId task_id{event.node_id, event.input_ts, event.event_type};
      TaskId hop_id{stream_id_map_[event.stream_id], event.packet_ts,
                    event.event_type};

      if (event.is_finish) {
        hop_events_[hop_id] = &event;
      }
      task_events_[task_id].push_back(&event);
    }

    // Construct the GraphTrace.
    result->Clear();
    result->set_base_time(base_time_);
    result->set_base_timestamp(base_ts_);
    std::unordered_set<TaskId> task_ids;
    for (const TraceEvent& event : snapshot) {
      if (!kProfilerPacketEvents[static_cast<int>(event.event_type)]) {
        BuildEventLog(event, result->add_calculator_trace());
        continue;
      }
      TaskId task_id{event.node_id, event.input_ts, event.event_type};
      if (task_ids.count(task_id) == 0) {
        task_ids.insert(task_id);
        BuildCalculatorTrace(task_events_[task_id],
                             result->add_calculator_trace());
      }
    }
    for (std::string& name : GetIdNames(stream_id_map_)) {
      result->add_stream_name(name);
    }
  }

  void CreateLog(const TraceBuffer& buffer, absl::Time begin_time,
                 absl::Time end_time, GraphTrace* result) {
    // Snapshot recent TraceEvents
    std::vector<TraceEvent> snapshot;
    snapshot.reserve(10000);
    TraceBuffer::iterator buffer_end = buffer.end();
    for (auto iter = buffer.begin(); iter < buffer_end; ++iter) {
      TraceEvent event = *iter;
      if (event.event_time >= begin_time && event.event_time < end_time) {
        snapshot.push_back(event);
      }
    }
    SetBaseTime(snapshot);

    // Log each TraceEvent.
    result->Clear();
    result->set_base_time(base_time_);
    result->set_base_timestamp(base_ts_);
    for (const TraceEvent& event : snapshot) {
      BuildEventLog(event, result->add_calculator_trace());
    }
    for (std::string& name : GetIdNames(stream_id_map_)) {
      result->add_stream_name(name);
    }
  }

  void Clear() {
    task_events_.clear();
    hop_events_.clear();
  }

 private:
  // Calculate the base timestamp and time.
  void SetBaseTime(const std::vector<TraceEvent>& snapshot) {
    if (base_time_ == std::numeric_limits<int64>::max()) {
      for (const TraceEvent& event : snapshot) {
        if (!event.input_ts.IsSpecialValue()) {
          base_ts_ = std::min(base_ts_, event.input_ts.Value());
        }
        if (!event.packet_ts.IsSpecialValue()) {
          base_ts_ = std::min(base_ts_, event.packet_ts.Value());
        }
        base_time_ = std::min(base_time_, ToUnixMicros(event.event_time));
      }
      if (base_time_ == std::numeric_limits<int64>::max()) {
        base_time_ = 0;
      }
      if (base_ts_ == std::numeric_limits<int64>::max()) {
        base_ts_ = 0;
      }
    }
  }

  // Return a timestamp in micros relative to the base timetamp.
  int64 LogTimestamp(Timestamp ts) { return ts.Value() - base_ts_; }

  // Return a time in micros relative to the base time.
  int64 LogTime(absl::Time time) { return ToUnixMicros(time) - base_time_; }

  // Returns the output event that produced an input packet.
  const TraceEvent* FindOutputEvent(const TraceEvent& event) {
    TaskId hop_id{stream_id_map_[event.stream_id], event.packet_ts,
                  event.event_type};
    return hop_events_[hop_id];
  }

  // Construct the StreamTrace for a TraceEvent.
  void BuildStreamTrace(const TraceEvent& event,
                        GraphTrace::StreamTrace* result) {
    if (event.is_finish) {
      result->set_stream_id(stream_id_map_[event.stream_id]);
      result->set_packet_timestamp(LogTimestamp(event.packet_ts));
      return;
    }
    result->set_stream_id(stream_id_map_[event.stream_id]);
    result->set_packet_timestamp(LogTimestamp(event.packet_ts));
    result->set_finish_time(LogTime(event.event_time));
    result->set_packet_id(packet_data_id_map_[event.packet_data_id]);
    const TraceEvent* output_event = FindOutputEvent(event);
    if (output_event) {
      result->set_start_time(LogTime(output_event->event_time));
    }
  }

  // Construct the CalculatorTrace for a set of TraceEvents.
  void BuildCalculatorTrace(const EventList& task_events,
                            GraphTrace::CalculatorTrace* result) {
    absl::Time start_time = absl::InfiniteFuture();
    absl::Time finish_time = absl::InfiniteFuture();
    for (const TraceEvent* event : task_events) {
      if (result->input_trace().size() + result->output_trace().size() == 0) {
        result->set_node_id(event->node_id);
        result->set_event_type(event->event_type);
        result->set_input_timestamp(LogTimestamp(event->input_ts));
        result->set_thread_id(event->thread_id);
      }
      if (event->is_finish) {
        finish_time = std::min(finish_time, event->event_time);
      } else {
        start_time = std::min(start_time, event->event_time);
      }
      if (kProfilerStreamEvents[static_cast<int>(event->event_type)]) {
        if (event->is_finish) {
          BuildStreamTrace(*event, result->add_output_trace());
          auto s = result->output_trace(result->output_trace_size() - 1);
        } else {
          BuildStreamTrace(*event, result->add_input_trace());
        }
      }
    }
    if (finish_time < absl::InfiniteFuture()) {
      result->set_finish_time(LogTime(finish_time));
    }
    if (start_time < absl::InfiniteFuture()) {
      result->set_start_time(LogTime(start_time));
    }
  }

  // Construct the protobuf log record for a single TraceEvent.
  void BuildEventLog(const TraceEvent& event,
                     GraphTrace::CalculatorTrace* result) {
    if (event.is_finish) {
      result->set_finish_time(LogTime(event.event_time));
    } else {
      result->set_start_time(LogTime(event.event_time));
    }
    result->set_node_id(event.node_id);
    result->set_event_type(event.event_type);
    if (event.input_ts != Timestamp::Unset()) {
      result->set_input_timestamp(LogTimestamp(event.input_ts));
    }
    result->set_thread_id(event.thread_id);
    if (kProfilerStreamEvents[static_cast<int>(event.event_type)]) {
      if (event.stream_id) {
        auto stream_trace = event.is_finish ? result->add_output_trace()
                                            : result->add_input_trace();
        stream_trace->set_stream_id(stream_id_map_[event.stream_id]);
        stream_trace->set_packet_timestamp(LogTimestamp(event.packet_ts));
        stream_trace->set_packet_id(packet_data_id_map_[event.packet_data_id]);
      }
    }
  }

  // TraceEvents indexed by task-id.
  std::unordered_map<TaskId, EventList> task_events_;
  // Output TraceEvents indexed by stream-hop-id.
  std::unordered_map<TaskId, const TraceEvent*> hop_events_;
  // Map from stream name pointers to int32 identifiers.
  StringIdMap stream_id_map_;
  // Map from packet data pointers to int32 identifiers.
  AddressIdMap packet_data_id_map_;
  // The timestamp represented as 0 in the trace.
  int64 base_ts_ = std::numeric_limits<int64>::max();
  // The time represented as 0 in the trace.
  int64 base_time_ = std::numeric_limits<int64>::max();
};

TraceBuilder::TraceBuilder() : impl_(new Impl) {}
TraceBuilder::~TraceBuilder() {}

Timestamp TraceBuilder::TimestampAfter(const TraceBuffer& buffer,
                                       absl::Time begin_time) {
  return Impl::TimestampAfter(buffer, begin_time);
}
void TraceBuilder::CreateTrace(const TraceBuffer& buffer, absl::Time begin_time,
                               absl::Time end_time, GraphTrace* result) {
  impl_->CreateTrace(buffer, begin_time, end_time, result);
}
void TraceBuilder::CreateLog(const TraceBuffer& buffer, absl::Time begin_time,
                             absl::Time end_time, GraphTrace* result) {
  impl_->CreateLog(buffer, begin_time, end_time, result);
}
void TraceBuilder::Clear() { impl_->Clear(); }

// Defined here since inline constants fail to link in android builds.
const TraceEvent::EventType  //
    TraceEvent::UNKNOWN = GraphTrace::UNKNOWN,
    TraceEvent::OPEN = GraphTrace::OPEN,
    TraceEvent::PROCESS = GraphTrace::PROCESS,
    TraceEvent::CLOSE = GraphTrace::CLOSE,
    TraceEvent::NOT_READY = GraphTrace::NOT_READY,
    TraceEvent::READY_FOR_PROCESS = GraphTrace::READY_FOR_PROCESS,
    TraceEvent::READY_FOR_CLOSE = GraphTrace::READY_FOR_CLOSE,
    TraceEvent::THROTTLED = GraphTrace::THROTTLED,
    TraceEvent::UNTHROTTLED = GraphTrace::UNTHROTTLED,
    TraceEvent::CPU_TASK_USER = GraphTrace::CPU_TASK_USER,
    TraceEvent::CPU_TASK_SYSTEM = GraphTrace::CPU_TASK_SYSTEM,
    TraceEvent::GPU_TASK = GraphTrace::GPU_TASK,
    TraceEvent::DSP_TASK = GraphTrace::DSP_TASK,
    TraceEvent::TPU_TASK = GraphTrace::TPU_TASK;

}  // namespace mediapipe
