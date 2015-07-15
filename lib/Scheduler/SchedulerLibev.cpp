////////////////////////////////////////////////////////////////////////////////
/// @brief input-output scheduler using libev
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2014 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
/// @author Achim Brandt
/// @author Copyright 2014, ArangoDB GmbH, Cologne, Germany
/// @author Copyright 2008-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "Scheduler/SchedulerLibev.h"

#ifdef _WIN32
#include "Basics/win-utils.h"
#include <evwrap.h>
#else
#include <ev.h>
#endif

#include "Basics/Exceptions.h"
#include "Basics/logging.h"
#include "Scheduler/SchedulerThread.h"
#include "Scheduler/Task.h"

using namespace triagens::basics;
using namespace triagens::rest;

// -----------------------------------------------------------------------------
// --SECTION--                                                             libev
// -----------------------------------------------------------------------------

/* EV_TIMER is an alias for EV_TIMEOUT */
#ifndef EV_TIMER
#define EV_TIMER EV_TIMEOUT
#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                    private helper
// -----------------------------------------------------------------------------

namespace {

////////////////////////////////////////////////////////////////////////////////
/// @brief async event watcher
////////////////////////////////////////////////////////////////////////////////

  struct AsyncWatcher : public ev_async, Watcher {
    struct ev_loop* loop;
    Task* task;

    AsyncWatcher () 
      : Watcher(EVENT_ASYNC) {
    }
  };

////////////////////////////////////////////////////////////////////////////////
/// @brief async event callback
////////////////////////////////////////////////////////////////////////////////

  void asyncCallback (struct ev_loop*, ev_async* w, int revents) {
    AsyncWatcher* watcher = (AsyncWatcher*) w;
    Task* task = watcher->task;

    if (task != nullptr && (revents & EV_ASYNC) && task->isActive()) {
      task->handleEvent(watcher, EVENT_ASYNC);
    }
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief waker callback
////////////////////////////////////////////////////////////////////////////////

  void wakerCallback (struct ev_loop* loop, ev_async*, int) {
    ev_unloop(loop, EVUNLOOP_ALL);
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief socket event watcher
////////////////////////////////////////////////////////////////////////////////

  struct SocketWatcher : public ev_io, Watcher {
    struct ev_loop* loop;
    Task* task;
    
    SocketWatcher () 
      : Watcher(EVENT_SOCKET_READ) {
    }
  };

////////////////////////////////////////////////////////////////////////////////
/// @brief socket event callback
////////////////////////////////////////////////////////////////////////////////

  void socketCallback (struct ev_loop*, ev_io* w, int revents) {
    SocketWatcher* watcher = (SocketWatcher*) w;
    Task* task = watcher->task;

    if (task != nullptr && task->isActive()) {
      if (revents & EV_READ) {
        if (revents & EV_WRITE) {
          task->handleEvent(watcher, EVENT_SOCKET_READ | EVENT_SOCKET_WRITE);
        }
        else {
          task->handleEvent(watcher, EVENT_SOCKET_READ);
        }
      }
      else if (revents & EV_WRITE) {
        task->handleEvent(watcher, EVENT_SOCKET_WRITE);
      }
    }
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief periodic event watcher
////////////////////////////////////////////////////////////////////////////////

  struct PeriodicWatcher : public ev_periodic, Watcher {
    struct ev_loop* loop;
    Task* task;
    
    PeriodicWatcher () 
      : Watcher(EVENT_PERIODIC) {
    }
  };

////////////////////////////////////////////////////////////////////////////////
/// @brief periodic event callback
////////////////////////////////////////////////////////////////////////////////

  void periodicCallback (struct ev_loop*, ev_periodic* w, int revents) {
    PeriodicWatcher* watcher = (PeriodicWatcher*) w;
    Task* task = watcher->task;

    if (task != nullptr && (revents & EV_PERIODIC) && task->isActive()) {
      task->handleEvent(watcher, EVENT_PERIODIC);
    }
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief signal event watcher
////////////////////////////////////////////////////////////////////////////////

  struct SignalWatcher : public ev_signal, Watcher {
    struct ev_loop* loop;
    Task* task;
    
    SignalWatcher () 
      : Watcher(EVENT_SIGNAL) {
    }
  };

////////////////////////////////////////////////////////////////////////////////
/// @brief signal event callback
////////////////////////////////////////////////////////////////////////////////

  void signalCallback (struct ev_loop*, ev_signal* w, int revents) {
    SignalWatcher* watcher = (SignalWatcher*) w;
    Task* task = watcher->task;

    if (task != nullptr && (revents & EV_SIGNAL) && task->isActive()) {
      task->handleEvent(watcher, EVENT_SIGNAL);
    }
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief timer event watcher
////////////////////////////////////////////////////////////////////////////////

  struct TimerWatcher : public ev_timer, Watcher {
    struct ev_loop* loop;
    Task* task;
    
    TimerWatcher () 
      : Watcher(EVENT_TIMER) {
    }
  };

////////////////////////////////////////////////////////////////////////////////
/// @brief timer event callback
////////////////////////////////////////////////////////////////////////////////

  void timerCallback (struct ev_loop*, ev_timer* w, int revents) {
    TimerWatcher* watcher = (TimerWatcher*) w;
    Task* task = watcher->task;

    if (task != nullptr && (revents & EV_TIMER) && task->isActive()) {
      task->handleEvent(watcher, EVENT_TIMER);
    }
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                              class SchedulerLibev
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                             static public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the available backends
////////////////////////////////////////////////////////////////////////////////

int SchedulerLibev::availableBackends () {
  return ev_supported_backends();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief set the libev allocator to our own allocator
///
/// this is done to avoid the numerous memory problems as reported by Valgrind
////////////////////////////////////////////////////////////////////////////////

void SchedulerLibev::switchAllocator () {
#ifdef TRI_ENABLE_MAINTAINER_MODE
  static bool switched = false;

  if (! switched) {
    // set the libev allocator to our own allocator
    ev_set_allocator(
#ifdef EV_THROW
      reinterpret_cast<void *(*)(void *ptr, long size) EV_THROW>
#endif
      (&TRI_WrappedReallocate));

    switched = true;
  }
#endif
}

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a scheduler
////////////////////////////////////////////////////////////////////////////////

SchedulerLibev::SchedulerLibev (size_t concurrency, int backend)
  : Scheduler(concurrency),
    _backend(backend) {

  switchAllocator();

  //_backend = 1;

  // report status
  LOG_TRACE("supported backends: %d", (int) ev_supported_backends());
  LOG_TRACE("recommended backends: %d", (int) ev_recommended_backends());
  LOG_TRACE("embeddable backends: %d", (int) ev_embeddable_backends());
  LOG_TRACE("backend flags: %d", (int) backend);

  // construct the loops
  _loops = new struct ev_loop*[nrThreads];

  ((struct ev_loop**) _loops)[0] = ev_default_loop(_backend);

  for (size_t i = 1;  i < nrThreads;  ++i) {
    ((struct ev_loop**) _loops)[i] = ev_loop_new(_backend);
  }

  // construct the scheduler threads
  threads = new SchedulerThread* [nrThreads];
  _wakers = new ev_async*[nrThreads];

  for (size_t i = 0;  i < nrThreads;  ++i) {
    threads[i] = new SchedulerThread(this, EventLoop(i), i == 0);

    ev_async* w = new ev_async;

    ev_async_init(w, wakerCallback);
    ev_async_start(((struct ev_loop**) _loops)[i], w);

    ((ev_async**) _wakers)[i] = w;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief deletes a scheduler
////////////////////////////////////////////////////////////////////////////////

SchedulerLibev::~SchedulerLibev () {

  // begin shutdown sequence within threads
  for (size_t i = 0;  i < nrThreads;  ++i) {
    threads[i]->beginShutdown();
  }

  // force threads to shutdown
  for (size_t i = 0;  i < nrThreads;  ++i) {
    threads[i]->stop();
  }

  for (size_t i = 0;  i < 100 && isRunning();  ++i) {
    usleep(100);
  }

  // shutdown loops
  for (size_t i = 1;  i < nrThreads;  ++i) {
    ev_async_stop(((struct ev_loop**) _loops)[i], ((ev_async**) _wakers)[i]);
    ev_loop_destroy(((struct ev_loop**) _loops)[i]);
  }

  ev_async_stop(((struct ev_loop**) _loops)[0], ((ev_async**) _wakers)[0]);
  ev_default_destroy();

  // and delete threads
  for (size_t i = 0;  i < nrThreads;  ++i) {
    delete threads[i];
    delete ((ev_async**) _wakers)[i];
  }

  // delete loops buffer
  delete[] ((struct ev_loop**) _loops);

  // delete threads buffer and wakers
  delete[] threads;
  delete[] (ev_async**)_wakers;
}

// -----------------------------------------------------------------------------
// --SECTION--                                                 Scheduler methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

void SchedulerLibev::eventLoop (EventLoop loop) {
  struct ev_loop* l = (struct ev_loop*) lookupLoop(loop);

  ev_loop(l, 0);
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

void SchedulerLibev::wakeupLoop (EventLoop loop) {
  if (size_t(loop) >= nrThreads) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "unknown loop");
  }

  ev_async_send(((struct ev_loop**) _loops)[loop], ((ev_async**) _wakers)[loop]);
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

void SchedulerLibev::uninstallEvent (EventToken watcher) {
  if (watcher == nullptr) {
    return;
  }
  
  EventType type = watcher->type;

  switch (type) {
    case EVENT_ASYNC: {
      AsyncWatcher* w = (AsyncWatcher*) watcher;
      ev_async_stop(w->loop, (ev_async*) w);
      delete w;

      break;
    }

    case EVENT_PERIODIC: {
      PeriodicWatcher* w = (PeriodicWatcher*) watcher;
      ev_periodic_stop(w->loop, (ev_periodic*) w);
      delete w;

      break;
    }

    case EVENT_SIGNAL: {
      SignalWatcher* w = (SignalWatcher*) watcher;
      ev_signal_stop(w->loop, (ev_signal*) w);
      delete w;

      break;
    }

    case EVENT_SOCKET_READ: {
      SocketWatcher* w = (SocketWatcher*) watcher;
      ev_io_stop(w->loop, (ev_io*) w);
      delete w;

      break;
    }


    case EVENT_TIMER: {
      TimerWatcher* w = (TimerWatcher*) watcher;
      ev_timer_stop(w->loop, (ev_timer*) w);
      delete w;

      break;
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

EventToken SchedulerLibev::installAsyncEvent (EventLoop loop, Task* task) {
  AsyncWatcher* watcher = new AsyncWatcher;
  watcher->loop = (struct ev_loop*) lookupLoop(loop);
  watcher->task = task;

  ev_async* w = (ev_async*) watcher;
  ev_async_init(w, asyncCallback);
  ev_async_start(watcher->loop, w);

  return watcher;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

void SchedulerLibev::sendAsync (EventToken token) {
  AsyncWatcher* watcher = (AsyncWatcher*) token;
  
  if (watcher == nullptr) {
    return;
  }

  ev_async* w = (ev_async*) watcher;
  ev_async_send(watcher->loop, w);
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

EventToken SchedulerLibev::installPeriodicEvent (EventLoop loop, Task* task, double offset, double interval) {
  PeriodicWatcher* watcher = new PeriodicWatcher;
  watcher->loop = (struct ev_loop*) lookupLoop(loop);
  watcher->task = task;

  ev_periodic* w = (ev_periodic*) watcher;
  ev_periodic_init(w, periodicCallback, offset, interval, 0);
  ev_periodic_start(watcher->loop, w);

  return watcher;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

void SchedulerLibev::rearmPeriodic (EventToken token, double offset, double interval) {
  PeriodicWatcher* watcher = (PeriodicWatcher*) token;
  
  if (watcher == nullptr) {
    return;
  }

  ev_periodic* w = (ev_periodic*) watcher;
  ev_periodic_set(w, offset, interval, 0);
  ev_periodic_again(watcher->loop, w);
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

EventToken SchedulerLibev::installSignalEvent (EventLoop loop, Task* task, int signal) {
  SignalWatcher* watcher = new SignalWatcher;
  watcher->loop = (struct ev_loop*) lookupLoop(loop);
  watcher->task = task;

  ev_signal* w = (ev_signal*) watcher;
  ev_signal_init(w, signalCallback, signal);
  ev_signal_start(watcher->loop, w);

  return watcher;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

  // ..........................................................................
  // Windows likes to operate on SOCKET types (sort of handles) while libev
  // likes to operate on file descriptors
  // Our abstraction for sockets allows to use exactly the same code
  // ..........................................................................

EventToken SchedulerLibev::installSocketEvent (EventLoop loop, EventType type, Task* task, TRI_socket_t socket) {
  SocketWatcher* watcher = new SocketWatcher;
  watcher->loop = (struct ev_loop*) lookupLoop(loop);
  watcher->task = task;

  int flags = 0;

  if (type & EVENT_SOCKET_READ) {
    flags |= EV_READ;
  }

  if (type & EVENT_SOCKET_WRITE) {
    flags |= EV_WRITE;
  }

  ev_io* w = (ev_io*) watcher;
  // Note that we do not use TRI_get_fd_or_handle_of_socket here because even
  // under Windows we want get the entry fileDescriptor here because
  // of the reason that is mentioned above!
  ev_io_init(w, socketCallback, socket.fileDescriptor, flags);
  ev_io_start(watcher->loop, w);

  return watcher;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

void SchedulerLibev::startSocketEvents (EventToken token) {
  SocketWatcher* watcher = (SocketWatcher*) token;

  if (watcher == nullptr) {
    return;
  }

  ev_io* w = (ev_io*) watcher;

  if (! ev_is_active(w)) {
    ev_io_start(watcher->loop, w);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

void SchedulerLibev::stopSocketEvents (EventToken token) {
  SocketWatcher* watcher = (SocketWatcher*) token;

  if (watcher == nullptr) {
    return;
  }

  ev_io* w = (ev_io*) watcher;

  if (ev_is_active(w)) {
    ev_io_stop(watcher->loop, w);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

EventToken SchedulerLibev::installTimerEvent (EventLoop loop, Task* task, double timeout) {
  TimerWatcher* watcher = new TimerWatcher;
  watcher->loop = (struct ev_loop*) lookupLoop(loop);
  watcher->task = task;

  ev_timer* w = (ev_timer*) watcher;
  ev_timer_init(w, timerCallback, timeout, 0.0);
  ev_timer_start(watcher->loop, w);

  return watcher;
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

void SchedulerLibev::clearTimer (EventToken token) {
  TimerWatcher* watcher = (TimerWatcher*) token;

  if (watcher == nullptr) {
    return;
  }

  ev_timer* w = (ev_timer*) watcher;
  ev_timer_stop(watcher->loop, w);
}

////////////////////////////////////////////////////////////////////////////////
/// {@inheritDoc}
////////////////////////////////////////////////////////////////////////////////

void SchedulerLibev::rearmTimer (EventToken token, double timeout) {
  TimerWatcher* watcher = (TimerWatcher*) token;

  if (watcher == nullptr) {
    return;
  }

  ev_timer* w = (ev_timer*) watcher;
  ev_timer_set(w, 0.0, timeout);
  ev_timer_again(watcher->loop, w);
}

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief looks up an event lookup
////////////////////////////////////////////////////////////////////////////////

void* SchedulerLibev::lookupLoop (EventLoop loop) {
  if (size_t(loop) >= nrThreads) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "unknown loop");
  }

  return ((struct ev_loop**) _loops)[loop];
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
