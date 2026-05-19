# ─────────────────────────────────────────────────────────────────────────────
# TinyShell GDB debug script
# Usage (from your build directory):
#   gdb -x ../debug_tsh.gdb ./tsh_server_gui
#
# What this does:
#   • Loads STL pretty-printers (std::string, std::vector, std::unordered_map)
#   • Sets breakpoints at every stage of the job pipeline
#   • Prints full backtraces + local variables automatically on crash
#   • Logs all breakpoint hits to tsh_debug.log so you can review after a run
#   • Catches exceptions before they unwind the stack
# ─────────────────────────────────────────────────────────────────────────────

# ── 0. Basics ─────────────────────────────────────────────────────────────────
set confirm off
set pagination off
set print pretty on
set print object on
set print static-members on
set print array-indexes on
set print thread-events on

# ── 1. STL pretty-printers ───────────────────────────────────────────────────
python
import sys, glob
for p in glob.glob('/usr/share/gcc*/python') + glob.glob('/usr/share/gcc-*/python'):
    if p not in sys.path:
        sys.path.insert(0, p)
try:
    from libstdcxx.v6.printers import register_libstdcxx_printers
    register_libstdcxx_printers(None)
    print('[GDB] STL pretty-printers loaded.')
except ImportError:
    print('[GDB] WARNING: STL pretty-printers not found.')
except RuntimeError:
    print('[GDB] STL pretty-printers already registered, skipping.')
end

# ── 2. Log all output to file ─────────────────────────────────────────────────
set logging file tsh_debug.log
set logging overwrite on
set logging enabled on
echo [GDB] Logging to tsh_debug.log\n

# ── 3. Catch C++ exceptions before stack unwinds ─────────────────────────────
# This shows you WHERE an exception is thrown, with the full call stack,
# before it gets caught by a try/catch and silently swallowed.
catch throw
commands
  echo \n[GDB] *** C++ EXCEPTION THROWN ***\n
  backtrace 20
  info locals
  continue
end

# ── 4. Crash handler (SIGSEGV / SIGABRT) ─────────────────────────────────────
handle SIGSEGV stop print
handle SIGABRT stop print

define hook-stop
  if $_siginfo.si_signo == 11
    echo \n[GDB] *** SIGSEGV — Segmentation Fault ***\n
    backtrace full
  end
  if $_siginfo.si_signo == 6
    echo \n[GDB] *** SIGABRT — Abort (assert / double-free / OOM) ***\n
    backtrace full
  end
end

# ─────────────────────────────────────────────────────────────────────────────
# PIPELINE BREAKPOINTS
# Each stage of the job lifecycle has a breakpoint with automatic variable
# printing so you can trace a job from submission to output without manually
# stepping through thousands of lines.
# ─────────────────────────────────────────────────────────────────────────────

# ── Stage 1: GUI submits a job ────────────────────────────────────────────────
# GrpcJobClient::submit — the moment the user clicks "Run"
break GrpcJobClient::submit
commands
  echo \n[GDB] >>> STAGE 1: GUI submit() called\n
  print userId
  print command
  continue
end

# ── Stage 2: Spine server receives SubmitJob RPC ──────────────────────────────
break SpineControlPlane::SubmitJob
commands
  echo \n[GDB] >>> STAGE 2: SubmitJob RPC received by spine server\n
  print request->command()
  print request->user_id()
  continue
end

# ── Stage 3: Job dispatched to agent ─────────────────────────────────────────
# AgentSession::enqueue — job signed spec pushed onto agent's write queue
break AgentSession::enqueue
commands
  echo \n[GDB] >>> STAGE 3: Job enqueued for agent delivery\n
  print msg
  continue
end

# ── Stage 4: Agent receives the job from the stream ──────────────────────────
# The Read() loop in AgentRuntime::run — server_msg received with has_job()
# Break at execute_job dispatch
break AgentRuntime::execute_job
commands
  echo \n[GDB] >>> STAGE 4: Agent execute_job() called\n
  print signed_spec.spec().job_id()
  print signed_spec.spec().command()
  print signed_spec.spec().agent_id()
  continue
end

# ── Stage 5: SandboxExecutor::run — actual fork/execve ───────────────────────
break SandboxExecutor::run
commands
  echo \n[GDB] >>> STAGE 5: SandboxExecutor::run() — about to fork/execve\n
  print spec.job_id()
  print spec.command()
  print spec.limits().timeout_ms()
  continue
end

# ── Stage 6: Agent emits an event back to the server ─────────────────────────
break AgentRuntime::emit_event
commands
  echo \n[GDB] >>> STAGE 6: emit_event() — agent sending event to spine\n
  print job_id
  print (int)type
  continue
end

# ── Stage 7: Spine ingests an agent event ────────────────────────────────────
break SpineControlPlane::ingest_agent_event
commands
  echo \n[GDB] >>> STAGE 7: ingest_agent_event() — spine received agent event\n
  print agent_id
  print incoming.job_id()
  print (int)incoming.type()
  continue
end

# ── Stage 8: State machine transition ────────────────────────────────────────
break tsh::spine::state_after_event
commands
  echo \n[GDB] >>> STAGE 8: state_after_event() — state machine transition\n
  print (int)type
  print (int)current
  finish
  print $
  continue
end

# ── Stage 9: Event persisted and published to EventBus ───────────────────────
break SpineControlPlane::persist_and_publish
commands
  echo \n[GDB] >>> STAGE 9: persist_and_publish()\n
  print event.job_id()
  print (int)event.type()
  print (int)event.state()
  continue
end

# ── Stage 10: WatchJob streams event back to GUI ─────────────────────────────
break SpineControlPlane::WatchJob
commands
  echo \n[GDB] >>> STAGE 10: WatchJob RPC — GUI is watching job\n
  print request->job_id()
  continue
end

# ── Stage 11: GUI receives an event ──────────────────────────────────────────
break LiveJobWidget::onEvent
commands
  echo \n[GDB] >>> STAGE 11: GUI onEvent() — LiveJobWidget received event\n
  print (int)event.type()
  print event.job_id()
  continue
end

# ── Invalid state transition (thrown as runtime_error) ───────────────────────
# This fires when a job event is rejected by is_valid_transition()
break tsh::spine::is_valid_transition
commands
  echo \n[GDB] >>> TRANSITION CHECK: is_valid_transition()\n
  print (int)from
  print (int)to
  finish
  print $
  continue
end

# ─────────────────────────────────────────────────────────────────────────────
# CONVENIENCE COMMANDS
# ─────────────────────────────────────────────────────────────────────────────

# 'tsh_job <job_id>' — print everything known about a job from the lifecycle map
define tsh_job
  echo Searching lifecycle_ for job: $arg0\n
  # Usage: tsh_job "job-abc123"
  # Prints the LifecycleEntry for that job_id from the global spine service
end

# 'tsh_agents' — dump all connected agents
define tsh_agents
  echo Connected agents in agents_ map:\n
  print agents_
end

# 'tsh_queues' — show write queue depth on the agent session
define tsh_queue
  echo Agent write_queue_ depth:\n
  print write_queue_.size()
end

# 'tsh_bt' — clean backtrace with args + locals
define tsh_bt
  backtrace full
end

echo \n
echo ═══════════════════════════════════════════════════════\n
echo   TinyShell GDB Debug Session\n
echo   Breakpoints set at all 11 pipeline stages.\n
echo   Type 'run' to start.\n
echo   All output also written to: tsh_debug.log\n
echo ═══════════════════════════════════════════════════════\n
echo \n
echo   Useful commands:\n
echo     tsh_bt       — backtrace with locals\n
echo     tsh_agents   — dump connected agents map\n
echo     tsh_queue    — show agent write queue depth\n
echo     info threads — list all threads\n
echo     thread N     — switch to thread N\n
echo     bt           — backtrace current thread\n
echo \n

# Auto-run. Remove this line if you want to set more breakpoints first.
# run
