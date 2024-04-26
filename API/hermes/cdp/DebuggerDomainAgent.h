/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_CDP_DEBUGGERDOMAINAGENT_H
#define HERMES_CDP_DEBUGGERDOMAINAGENT_H

#include <functional>
#include <string>

#include <hermes/AsyncDebuggerAPI.h>
#include <hermes/hermes.h>
#include <hermes/inspector/chrome/MessageConverters.h>

#include "DomainAgent.h"

namespace facebook {
namespace hermes {
namespace cdp {

namespace m = ::facebook::hermes::inspector_modern::chrome::message;

namespace {
/// Details about a single Hermes breakpoint, implied by a CDP breakpoint.
struct HermesBreakpoint {
  debugger::BreakpointID breakpointID;
  debugger::ScriptID scriptID;
};

/// Type used to store CDP breakpoint identifiers. These IDs are generated by
/// the CDP Handler, so we can constrain them to a specific range.
using CDPBreakpointID = uint32_t;

/// Description of where breakpoints should be created.
struct CDPBreakpointDescription {
  /// Determines whether this breakpoint can be persisted across sessions
  bool persistable() const {
    // Only persist breakpoints that can apply to future scripts (i.e.
    // breakpoints set on a set of files specified by script URL, not
    // breakpoints set on an exact, session-specific script ID).
    return url.has_value();
  }

  std::optional<std::string> url;
  long long line;
  std::optional<long long> column;
  std::optional<std::string> condition;
};

/// Details of each existing CDP breakpoint, which may correspond to multiple
/// Hermes breakpoints.
struct CDPBreakpoint {
  explicit CDPBreakpoint(CDPBreakpointDescription description)
      : description(description) {}

  // Description of where the breakpoint should be applied
  CDPBreakpointDescription description;

  // Registered breakpoints in Hermes
  std::vector<HermesBreakpoint> hermesBreakpoints;
};

struct HermesBreakpointLocation {
  debugger::BreakpointID id;
  debugger::SourceLocation location;
};
} // namespace

/// Handler for the "Debugger" domain of CDP. Accepts events from the runtime,
/// and CDP requests from the debug client belonging to the "Debugger" domain.
/// Produces CDP responses and events belonging to the "Debugger" domain. All
/// methods expect to be invoked with exclusive access to the runtime.
class DebuggerDomainAgent : public DomainAgent {
 public:
  DebuggerDomainAgent(
      int32_t executionContextID,
      HermesRuntime &runtime,
      debugger::AsyncDebuggerAPI &asyncDebugger,
      SynchronizedOutboundCallback messageCallback,
      std::shared_ptr<old_cdp::RemoteObjectsTable> objTable_);
  ~DebuggerDomainAgent();

  /// Handles Debugger.enable request
  void enable(const m::debugger::EnableRequest &req);
  /// Handles Debugger.disable request
  void disable(const m::debugger::DisableRequest &req);

  /// Handles Debugger.pause request
  void pause(const m::debugger::PauseRequest &req);
  /// Handles Debugger.resume request
  void resume(const m::debugger::ResumeRequest &req);

  /// Handles Debugger.stepInto request
  void stepInto(const m::debugger::StepIntoRequest &req);
  /// Handles Debugger.stepOut request
  void stepOut(const m::debugger::StepOutRequest &req);
  /// Handles Debugger.stepOver request
  void stepOver(const m::debugger::StepOverRequest &req);

  /// Handles Debugger.setPauseOnExceptions
  void setPauseOnExceptions(
      const m::debugger::SetPauseOnExceptionsRequest &req);

  /// Handles Debugger.evaluateOnCallFrame
  void evaluateOnCallFrame(const m::debugger::EvaluateOnCallFrameRequest &req);

  /// Debugger.setBreakpoint creates a CDP breakpoint that applies to exactly
  /// one script (identified by script ID) that does not survive reloads.
  void setBreakpoint(const m::debugger::SetBreakpointRequest &req);
  // Debugger.setBreakpointByUrl creates a CDP breakpoint that may apply to
  // multiple scripts (identified by URL), and survives reloads.
  void setBreakpointByUrl(const m::debugger::SetBreakpointByUrlRequest &req);
  /// Handles Debugger.removeBreakpoint
  void removeBreakpoint(const m::debugger::RemoveBreakpointRequest &req);
  /// Handles Debugger.setBreakpointsActive
  void setBreakpointsActive(
      const m::debugger::SetBreakpointsActiveRequest &req);

 private:
  /// Handle an event originating from the runtime.
  void handleDebuggerEvent(
      HermesRuntime &runtime,
      debugger::AsyncDebuggerAPI &asyncDebugger,
      debugger::DebuggerEventType event);

  /// Send a Pause notification to the debug client with "other" being the
  /// reason
  void sendPausedNotificationToClient();
  /// Send a Pause notification to the debug client with "exception" being the
  /// reason
  void sendPauseOnExceptionNotificationToClient();
  /// Send a Debugger.scriptParsed notification to the debug client
  void sendScriptParsedNotificationToClient(
      const debugger::SourceLocation srcLoc);

  /// Obtain the newly loaded script and send a ScriptParsed notification to the
  /// debug client
  void processNewLoadedScript();

  std::pair<unsigned int, CDPBreakpoint &> createCDPBreakpoint(
      CDPBreakpointDescription &&description,
      std::optional<HermesBreakpoint> hermesBreakpoint = std::nullopt);

  std::optional<HermesBreakpointLocation> createHermesBreakpont(
      debugger::ScriptID scriptID,
      const CDPBreakpointDescription &description);

  std::optional<HermesBreakpointLocation> applyBreakpoint(
      CDPBreakpoint &breakpoint,
      debugger::ScriptID scriptID);

  bool checkDebuggerEnabled(const m::Request &req);
  bool checkDebuggerPaused(const m::Request &req);

  HermesRuntime &runtime_;
  debugger::AsyncDebuggerAPI &asyncDebugger_;

  /// ID for the registered DebuggerEventCallback
  debugger::DebuggerEventCallbackID debuggerEventCallbackId_;

  /// Details of each CDP breakpoint that has been created, and not
  /// yet destroyed.
  std::unordered_map<CDPBreakpointID, CDPBreakpoint> cdpBreakpoints_{};

  /// CDP breakpoint IDs are assigned by the DebuggerDomainAgent. Keep track of
  /// the next available ID.
  CDPBreakpointID nextBreakpointID_ = 1;

  /// Whether the currently installed breakpoints actually take effect. If
  /// they're supposed to be inactive, then debugger agent will automatically
  /// resume execution when breakpoints are hit.
  bool breakpointsActive_ = true;

  /// Whether Debugger.enable was received and wasn't disabled by receiving
  /// Debugger.disable
  bool enabled_;

  /// Whether to consider the debugger as currently paused. There are some
  /// debugger events such as ScriptLoaded where we don't consider the debugger
  /// to be paused.
  bool paused_;
};

} // namespace cdp
} // namespace hermes
} // namespace facebook

#endif // HERMES_CDP_DEBUGGERDOMAINAGENT_H
