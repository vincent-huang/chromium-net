// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_BASE_NET_LOG_H_
#define NET_BASE_NET_LOG_H_
#pragma once

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/memory/ref_counted.h"
#include "net/base/net_export.h"

namespace base {
class TimeTicks;
class Value;
}

namespace net {

// NetLog is the destination for log messages generated by the network stack.
// Each log message has a "source" field which identifies the specific entity
// that generated the message (for example, which URLRequest or which
// SocketStream).
//
// To avoid needing to pass in the "source id" to the logging functions, NetLog
// is usually accessed through a BoundNetLog, which will always pass in a
// specific source ID.
//
class NET_EXPORT NetLog {
 public:
  enum EventType {
#define EVENT_TYPE(label) TYPE_ ## label,
#include "net/base/net_log_event_type_list.h"
#undef EVENT_TYPE
    EVENT_COUNT
  };

  // The 'phase' of an event trace (whether it marks the beginning or end
  // of an event.).
  enum EventPhase {
    PHASE_NONE,
    PHASE_BEGIN,
    PHASE_END,
  };

  // The "source" identifies the entity that generated the log message.
  enum SourceType {
#define SOURCE_TYPE(label) SOURCE_ ## label,
#include "net/base/net_log_source_type_list.h"
#undef SOURCE_TYPE
    SOURCE_COUNT
  };

  // Identifies the entity that generated this log. The |id| field should
  // uniquely identify the source, and is used by log observers to infer
  // message groupings. Can use NetLog::NextID() to create unique IDs.
  struct NET_EXPORT Source {
    static const uint32 kInvalidId = 0;

    Source() : type(SOURCE_NONE), id(kInvalidId) {}
    Source(SourceType type, uint32 id) : type(type), id(id) {}
    bool is_valid() const { return id != kInvalidId; }

    // The caller takes ownership of the returned Value*.
    base::Value* ToValue() const;

    SourceType type;
    uint32 id;
  };

  // Base class for associating additional parameters with an event. Log
  // observers need to know what specific derivations of EventParameters a
  // particular EventType uses, in order to get at the individual components.
  class NET_EXPORT EventParameters
      : public base::RefCountedThreadSafe<EventParameters> {
   public:
    EventParameters() {}
    virtual ~EventParameters() {}

    // Serializes the parameters to a Value tree. This is intended to be a
    // lossless conversion, which is used to serialize the parameters to JSON.
    // The caller takes ownership of the returned Value*.
    virtual base::Value* ToValue() const = 0;

   private:
    DISALLOW_COPY_AND_ASSIGN(EventParameters);
  };

  // Specifies the granularity of events that should be emitted to the log.
  enum LogLevel {
    // Log everything possible, even if it is slow and memory expensive.
    // Includes logging of transferred bytes.
    LOG_ALL,

    // Log all events, but do not include the actual transferred bytes as
    // parameters for bytes sent/received events.
    LOG_ALL_BUT_BYTES,

    // Only log events which are cheap, and don't consume much memory.
    LOG_BASIC,
  };

  // An observer, that must ensure its own thread safety, for events
  // being added to a NetLog.
  class NET_EXPORT ThreadSafeObserver {
   public:
    // Constructs an observer that wants to see network events, with
    // the specified minimum event granularity.  A ThreadSafeObserver can only
    // observe a single NetLog at a time.
    //
    // Observers will be called on the same thread an entry is added on,
    // and are responsible for ensuring their own thread safety.
    //
    // Observers must stop watching a NetLog before either the Observer or the
    // NetLog is destroyed.
    ThreadSafeObserver();
    virtual ~ThreadSafeObserver();

    // Returns the minimum log level for events this observer wants to
    // receive.  Must not be called when not watching a NetLog.
    LogLevel log_level() const;

    // Returns the NetLog we are currently watching, if any.  Returns NULL
    // otherwise.
    NetLog* net_log() const;

    // This method will be called on the thread that the event occurs on.  It
    // is the responsibility of the observer to handle it in a thread safe
    // manner.
    //
    // It is illegal for an Observer to call any NetLog or
    // NetLog::Observer functions in response to a call to OnAddEntry.
    virtual void OnAddEntry(EventType type,
                            const base::TimeTicks& time,
                            const Source& source,
                            EventPhase phase,
                            EventParameters* params) = 0;

   private:
    friend class NetLog;

    // Both of these values are only modified by the NetLog.
    LogLevel log_level_;
    NetLog* net_log_;

    DISALLOW_COPY_AND_ASSIGN(ThreadSafeObserver);
  };

  NetLog() {}
  virtual ~NetLog() {}

  // Emits an event to the log stream.
  //  |type| - The type of the event.
  //  |time| - The time when the event occurred.
  //  |source| - The source that generated the event.
  //  |phase| - An optional parameter indicating whether this is the start/end
  //            of an action.
  //  |params| - Optional (may be NULL) parameters for this event.
  //             The specific subclass of EventParameters is defined
  //             by the contract for events of this |type|.
  //             TODO(eroman): Take a scoped_refptr<> instead.
  virtual void AddEntry(EventType type,
                        const base::TimeTicks& time,
                        const Source& source,
                        EventPhase phase,
                        EventParameters* params) = 0;

  // Returns a unique ID which can be used as a source ID.
  virtual uint32 NextID() = 0;

  // Returns the logging level for this NetLog. This is used to avoid computing
  // and saving expensive log entries.
  virtual LogLevel GetLogLevel() const = 0;

  // Adds an observer and sets its log level.  The observer must not be
  // watching any NetLog, including this one, when this is called.
  //
  // Typical observers should specify LOG_BASIC.
  //
  // Observers that need to see the full granularity of events can specify
  // LOG_ALL_BUT_BYTES. However, doing so will have performance consequences.
  //
  // NetLog implementations must call NetLog::OnAddObserver to update the
  // observer's internal state.
  virtual void AddThreadSafeObserver(ThreadSafeObserver* observer,
                                     LogLevel log_level) = 0;

  // Sets the log level of |observer| to |log_level|.  |observer| must be
  // watching |this|.  NetLog implementations must call
  // NetLog::OnSetObserverLogLevel to update the observer's internal state.
  virtual void SetObserverLogLevel(ThreadSafeObserver* observer,
                                   LogLevel log_level) = 0;

  // Removes an observer.  NetLog implementations must call
  // NetLog::OnAddObserver to update the observer's internal state.
  //
  // For thread safety reasons, it is recommended that this not be called in
  // an object's destructor.
  virtual void RemoveThreadSafeObserver(ThreadSafeObserver* observer) = 0;

  // Converts a time to the string format that the NetLog uses to represent
  // times.  Strings are used since integers may overflow.
  static std::string TickCountToString(const base::TimeTicks& time);

  // Returns a C-String symbolic name for |event_type|.
  static const char* EventTypeToString(EventType event_type);

  // Returns a dictionary that maps event type symbolic names to their enum
  // values.  Caller takes ownership of the returned Value.
  static base::Value* GetEventTypesAsValue();

  // Returns a C-String symbolic name for |source_type|.
  static const char* SourceTypeToString(SourceType source_type);

  // Returns a dictionary that maps source type symbolic names to their enum
  // values.  Caller takes ownership of the returned Value.
  static base::Value* GetSourceTypesAsValue();

  // Returns a C-String symbolic name for |event_phase|.
  static const char* EventPhaseToString(EventPhase event_phase);

  // Serializes the specified event to a DictionaryValue.
  // If |use_strings| is true, uses strings rather than numeric ids.
  static base::Value* EntryToDictionaryValue(NetLog::EventType type,
                                             const base::TimeTicks& time,
                                             const NetLog::Source& source,
                                             NetLog::EventPhase phase,
                                             NetLog::EventParameters* params,
                                             bool use_strings);

 protected:
  // Subclasses must call these in the corresponding functions to set an
  // observer's |net_log_| and |log_level_| values.
  void OnAddObserver(ThreadSafeObserver* observer, LogLevel log_level);
  void OnSetObserverLogLevel(ThreadSafeObserver* observer,
                             LogLevel log_level);
  void OnRemoveObserver(ThreadSafeObserver* observer);

 private:
  DISALLOW_COPY_AND_ASSIGN(NetLog);
};

// Helper that binds a Source to a NetLog, and exposes convenience methods to
// output log messages without needing to pass in the source.
class NET_EXPORT BoundNetLog {
 public:
  BoundNetLog() : net_log_(NULL) {}

  BoundNetLog(const NetLog::Source& source, NetLog* net_log)
      : source_(source), net_log_(net_log) {
  }

  // Convenience methods that call through to the NetLog, passing in the
  // currently bound source.
  void AddEntry(NetLog::EventType type,
                NetLog::EventPhase phase,
                const scoped_refptr<NetLog::EventParameters>& params) const;

  void AddEntryWithTime(
      NetLog::EventType type,
      const base::TimeTicks& time,
      NetLog::EventPhase phase,
      const scoped_refptr<NetLog::EventParameters>& params) const;

  // Convenience methods that call through to the NetLog, passing in the
  // currently bound source, current time, and a fixed "capture phase"
  // (begin, end, or none).
  void AddEvent(NetLog::EventType event_type,
                const scoped_refptr<NetLog::EventParameters>& params) const;
  void BeginEvent(NetLog::EventType event_type,
                  const scoped_refptr<NetLog::EventParameters>& params) const;
  void EndEvent(NetLog::EventType event_type,
                const scoped_refptr<NetLog::EventParameters>& params) const;

  // Just like AddEvent, except |net_error| is a net error code.  A parameter
  // called "net_error" with the indicated value will be recorded for the event.
  // |net_error| must be negative, and not ERR_IO_PENDING, as it's not a true
  // error.
  void AddEventWithNetErrorCode(NetLog::EventType event_type,
                                int net_error) const;

  // Just like EndEvent, except |net_error| is a net error code.  If it's
  // negative, a parameter called "net_error" with a value of |net_error| is
  // associated with the event.  Otherwise, the end event has no parameters.
  // |net_error| must not be ERR_IO_PENDING, as it's not a true error.
  void EndEventWithNetErrorCode(NetLog::EventType event_type,
                                int net_error) const;

  // Logs a byte transfer event to the NetLog.  Determines whether to log the
  // received bytes or not based on the current logging level.
  void AddByteTransferEvent(NetLog::EventType event_type,
                            int byte_count, const char* bytes) const;

  NetLog::LogLevel GetLogLevel() const;

  // Returns true if the log level is LOG_ALL.
  bool IsLoggingBytes() const;

  // Returns true if the log level is LOG_ALL or LOG_ALL_BUT_BYTES.
  bool IsLoggingAllEvents() const;

  // Helper to create a BoundNetLog given a NetLog and a SourceType. Takes care
  // of creating a unique source ID, and handles the case of NULL net_log.
  static BoundNetLog Make(NetLog* net_log, NetLog::SourceType source_type);

  const NetLog::Source& source() const { return source_; }
  NetLog* net_log() const { return net_log_; }

 private:
  NetLog::Source source_;
  NetLog* net_log_;
};

// NetLogStringParameter is a subclass of EventParameters that encapsulates a
// single std::string parameter.
class NET_EXPORT NetLogStringParameter : public NetLog::EventParameters {
 public:
  // |name| must be a string literal.
  NetLogStringParameter(const char* name, const std::string& value);
  virtual ~NetLogStringParameter();

  const std::string& value() const {
    return value_;
  }

  virtual base::Value* ToValue() const OVERRIDE;

 private:
  const char* const name_;
  const std::string value_;
};

// NetLogIntegerParameter is a subclass of EventParameters that encapsulates a
// single integer parameter.
class NET_EXPORT NetLogIntegerParameter : public NetLog::EventParameters {
 public:
  // |name| must be a string literal.
  NetLogIntegerParameter(const char* name, int value)
      : name_(name), value_(value) {}

  int value() const {
    return value_;
  }

  virtual base::Value* ToValue() const OVERRIDE;

 private:
  const char* name_;
  const int value_;
};

// NetLogSourceParameter is a subclass of EventParameters that encapsulates a
// single NetLog::Source parameter.
class NET_EXPORT NetLogSourceParameter : public NetLog::EventParameters {
 public:
  // |name| must be a string literal.
  NetLogSourceParameter(const char* name, const NetLog::Source& value)
      : name_(name), value_(value) {}

  const NetLog::Source& value() const {
    return value_;
  }

  virtual base::Value* ToValue() const OVERRIDE;

 private:
  const char* name_;
  const NetLog::Source value_;
};

// ScopedNetLogEvent logs a begin event on creation, and the corresponding end
// event on destruction.
class NET_EXPORT_PRIVATE ScopedNetLogEvent {
 public:
  ScopedNetLogEvent(const BoundNetLog& net_log,
                    NetLog::EventType event_type,
                    const scoped_refptr<NetLog::EventParameters>& params);

  ~ScopedNetLogEvent();

  // Sets the parameters that will logged on object destruction.  Can be called
  // at most once for a given ScopedNetLogEvent object.  If not called, the end
  // event will have no parameters.
  void SetEndEventParameters(
      const scoped_refptr<NetLog::EventParameters>& end_event_params);

  const BoundNetLog& net_log() const;

 private:
  BoundNetLog net_log_;
  const NetLog::EventType event_type_;
  scoped_refptr<NetLog::EventParameters> end_event_params_;
};

}  // namespace net

#endif  // NET_BASE_NET_LOG_H_
