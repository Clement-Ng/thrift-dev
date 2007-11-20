// Copyright (c) 2006- Facebook
// Distributed under the Thrift Software License
//
// See accompanying file LICENSE or visit the Thrift site at:
// http://developers.facebook.com/thrift/

#include "TNonblockingServer.h"

#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

namespace facebook { namespace thrift { namespace server { 

using namespace facebook::thrift::protocol;
using namespace facebook::thrift::transport;
using namespace std;

class TConnection::Task: public Runnable {
 public:
  Task(boost::shared_ptr<TProcessor> processor,
       boost::shared_ptr<TProtocol> input,
       boost::shared_ptr<TProtocol> output,
       int taskHandle) :
    processor_(processor),
    input_(input),
    output_(output),
    taskHandle_(taskHandle) {}

  void run() {
    try {
      while (processor_->process(input_, output_)) {
        if (!input_->getTransport()->peek()) {
          break;
        }
      }
    } catch (TTransportException& ttx) {
      cerr << "TThreadedServer client died: " << ttx.what() << endl;
    } catch (TException& x) {
      cerr << "TThreadedServer exception: " << x.what() << endl;
    } catch (...) {
      cerr << "TThreadedServer uncaught exception." << endl;
    }
    
    // Signal completion back to the libevent thread via a socketpair
    int8_t b = 0;
    if (-1 == send(taskHandle_, &b, sizeof(int8_t), 0)) {
      GlobalOutput("TNonblockingServer::Task: send");
    }
    if (-1 == ::close(taskHandle_)) {
      GlobalOutput("TNonblockingServer::Task: close, possible resource leak");
    }
  }

 private:
  boost::shared_ptr<TProcessor> processor_;
  boost::shared_ptr<TProtocol> input_;
  boost::shared_ptr<TProtocol> output_;
  int taskHandle_;
};

void TConnection::init(int socket, short eventFlags, TNonblockingServer* s) {
  socket_ = socket;
  server_ = s;
  appState_ = APP_INIT;
  eventFlags_ = 0;

  readBufferPos_ = 0;
  readWant_ = 0;

  writeBuffer_ = NULL;
  writeBufferSize_ = 0;
  writeBufferPos_ = 0;

  socketState_ = SOCKET_RECV;
  appState_ = APP_INIT;
  
  taskHandle_ = -1;

  // Set flags, which also registers the event
  setFlags(eventFlags);

  // get input/transports
  factoryInputTransport_ = s->getInputTransportFactory()->getTransport(inputTransport_);
  factoryOutputTransport_ = s->getOutputTransportFactory()->getTransport(outputTransport_);

  // Create protocol
  inputProtocol_ = s->getInputProtocolFactory()->getProtocol(factoryInputTransport_);
  outputProtocol_ = s->getOutputProtocolFactory()->getProtocol(factoryOutputTransport_);
}

void TConnection::workSocket() {
  int flags=0, got=0, left=0, sent=0;
  uint32_t fetch = 0;

  switch (socketState_) {
  case SOCKET_RECV:
    // It is an error to be in this state if we already have all the data
    assert(readBufferPos_ < readWant_);

    // Double the buffer size until it is big enough
    if (readWant_ > readBufferSize_) {
      while (readWant_ > readBufferSize_) {
        readBufferSize_ *= 2;
      }
      readBuffer_ = (uint8_t*)realloc(readBuffer_, readBufferSize_);
      if (readBuffer_ == NULL) {
        GlobalOutput("TConnection::workSocket() realloc");
        close();
        return;
      }
    }

    // Read from the socket
    fetch = readWant_ - readBufferPos_;
    got = recv(socket_, readBuffer_ + readBufferPos_, fetch, 0);
   
    if (got > 0) {
      // Move along in the buffer
      readBufferPos_ += got;

      // Check that we did not overdo it
      assert(readBufferPos_ <= readWant_);
    
      // We are done reading, move onto the next state
      if (readBufferPos_ == readWant_) {
        transition();
      }
      return;
    } else if (got == -1) {
      // Blocking errors are okay, just move on
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }

      if (errno != ECONNRESET) {
        GlobalOutput("TConnection::workSocket() recv -1");
      }
    }

    // Whenever we get down here it means a remote disconnect
    close();
    
    return;

  case SOCKET_SEND:
    // Should never have position past size
    assert(writeBufferPos_ <= writeBufferSize_);

    // If there is no data to send, then let us move on
    if (writeBufferPos_ == writeBufferSize_) {
      fprintf(stderr, "WARNING: Send state with no data to send\n");
      transition();
      return;
    }

    flags = 0;
    #ifdef MSG_NOSIGNAL
    // Note the use of MSG_NOSIGNAL to suppress SIGPIPE errors, instead we
    // check for the EPIPE return condition and close the socket in that case
    flags |= MSG_NOSIGNAL;
    #endif // ifdef MSG_NOSIGNAL

    left = writeBufferSize_ - writeBufferPos_;
    sent = send(socket_, writeBuffer_ + writeBufferPos_, left, flags);

    if (sent <= 0) {
      // Blocking errors are okay, just move on
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      if (errno != EPIPE) {
        GlobalOutput("TConnection::workSocket() send -1");
      }
      close();
      return;
    }

    writeBufferPos_ += sent;

    // Did we overdo it?
    assert(writeBufferPos_ <= writeBufferSize_);

    // We are  done!
    if (writeBufferPos_ == writeBufferSize_) {
      transition();
    }

    return;

  default:
    fprintf(stderr, "Shit Got Ill. Socket State %d\n", socketState_);
    assert(0);
  }
}

/**
 * This is called when the application transitions from one state into
 * another. This means that it has finished writing the data that it needed
 * to, or finished receiving the data that it needed to.
 */
void TConnection::transition() {

  int sz = 0;

  // Switch upon the state that we are currently in and move to a new state
  switch (appState_) {

  case APP_READ_REQUEST:
    // We are done reading the request, package the read buffer into transport
    // and get back some data from the dispatch function
    inputTransport_->resetBuffer(readBuffer_, readBufferPos_);
    outputTransport_->resetBuffer();
    
    if (server_->isThreadPoolProcessing()) {
      // We are setting up a Task to do this work and we will wait on it
      int sv[2];
      if (-1 == socketpair(AF_LOCAL, SOCK_STREAM, 0, sv)) {
        GlobalOutput("TConnection::socketpair() failed");
        // Now we will fall through to the APP_WAIT_TASK block with no response
      } else {
        // Create task and dispatch to the thread manager
        boost::shared_ptr<Runnable> task =
          boost::shared_ptr<Runnable>(new Task(server_->getProcessor(),
                                               inputProtocol_,
                                               outputProtocol_,
                                               sv[1]));
        appState_ = APP_WAIT_TASK;
        event_set(&taskEvent_,
                  taskHandle_ = sv[0],
                  EV_READ,
                  TConnection::taskHandler,
                  this);

        // Add the event and start up the server
        if (-1 == event_add(&taskEvent_, 0)) {
          GlobalOutput("TNonblockingServer::serve(): coult not event_add");
          return;
        }
        server_->addTask(task);

        // Set this connection idle so that libevent doesn't process more
        // data on it while we're still waiting for the threadmanager to
        // finish this task
        setIdle();
        return;
      }
    } else {
      try {
        // Invoke the processor
        server_->getProcessor()->process(inputProtocol_, outputProtocol_);
      } catch (TTransportException &ttx) {
        fprintf(stderr, "TTransportException: Server::process() %s\n", ttx.what());
        close();
        return;
      } catch (TException &x) {
        fprintf(stderr, "TException: Server::process() %s\n", x.what());
        close();     
        return;
      } catch (...) {
        fprintf(stderr, "Server::process() unknown exception\n");
        close();
        return;
      }
    }

    // Intentionally fall through here, the call to process has written into
    // the writeBuffer_

  case APP_WAIT_TASK:
    // We have now finished processing a task and the result has been written
    // into the outputTransport_, so we grab its contents and place them into
    // the writeBuffer_ for actual writing by the libevent thread

    // Get the result of the operation
    outputTransport_->getBuffer(&writeBuffer_, &writeBufferSize_);

    // If the function call generated return data, then move into the send
    // state and get going
    if (writeBufferSize_ > 0) {

      // Move into write state
      writeBufferPos_ = 0;
      socketState_ = SOCKET_SEND;

      if (server_->getFrameResponses()) {
        // Put the frame size into the write buffer
        appState_ = APP_SEND_FRAME_SIZE;
        frameSize_ = (int32_t)htonl(writeBufferSize_);
        writeBuffer_ = (uint8_t*)&frameSize_;
        writeBufferSize_ = 4;
      } else {
        // Go straight into sending the result, do not frame it
        appState_ = APP_SEND_RESULT;
      }

      // Socket into write mode
      setWrite();

      // Try to work the socket immediately
      // workSocket();

      return;
    }

    // In this case, the request was asynchronous and we should fall through
    // right back into the read frame header state
    goto LABEL_APP_INIT;

  case APP_SEND_FRAME_SIZE:

    // Refetch the result of the operation since we put the frame size into
    // writeBuffer_
    outputTransport_->getBuffer(&writeBuffer_, &writeBufferSize_);
    writeBufferPos_ = 0;

    // Now in send result state
    appState_ = APP_SEND_RESULT;

    // Go to work on the socket right away, probably still writeable
    // workSocket();

    return;

  case APP_SEND_RESULT:

    // N.B.: We also intentionally fall through here into the INIT state!

  LABEL_APP_INIT:
  case APP_INIT:

    // Clear write buffer variables
    writeBuffer_ = NULL;
    writeBufferPos_ = 0;
    writeBufferSize_ = 0;

    // Set up read buffer for getting 4 bytes
    readBufferPos_ = 0;
    readWant_ = 4;

    // Into read4 state we go
    socketState_ = SOCKET_RECV;
    appState_ = APP_READ_FRAME_SIZE;

    // Register read event
    setRead();

    // Try to work the socket right away
    // workSocket();

    return;

  case APP_READ_FRAME_SIZE:
    // We just read the request length, deserialize it
    sz = *(int32_t*)readBuffer_;
    sz = (int32_t)ntohl(sz);

    if (sz <= 0) {
      fprintf(stderr, "TConnection:transition() Negative frame size %d, remote side not using TFramedTransport?", sz);
      close();
      return;
    }

    // Reset the read buffer
    readWant_ = (uint32_t)sz;
    readBufferPos_= 0;

    // Move into read request state
    appState_ = APP_READ_REQUEST;

    // Work the socket right away
    // workSocket();

    return;

  default:
    fprintf(stderr, "Totally Fucked. Application State %d\n", appState_);
    assert(0);
  }
}

void TConnection::setFlags(short eventFlags) {
  // Catch the do nothing case
  if (eventFlags_ == eventFlags) {
    return;
  }

  // Delete a previously existing event
  if (eventFlags_ != 0) {
    if (event_del(&event_) == -1) {
      GlobalOutput("TConnection::setFlags event_del");
      return;
    }
  }

  // Update in memory structure
  eventFlags_ = eventFlags;

  // Do not call event_set if there are no flags
  if (!eventFlags_) {
    return;
  }

  /**
   * event_set:
   *
   * Prepares the event structure &event to be used in future calls to
   * event_add() and event_del().  The event will be prepared to call the
   * eventHandler using the 'sock' file descriptor to monitor events.
   *
   * The events can be either EV_READ, EV_WRITE, or both, indicating
   * that an application can read or write from the file respectively without
   * blocking.
   *
   * The eventHandler will be called with the file descriptor that triggered
   * the event and the type of event which will be one of: EV_TIMEOUT,
   * EV_SIGNAL, EV_READ, EV_WRITE.
   *
   * The additional flag EV_PERSIST makes an event_add() persistent until
   * event_del() has been called.
   *
   * Once initialized, the &event struct can be used repeatedly with
   * event_add() and event_del() and does not need to be reinitialized unless
   * the eventHandler and/or the argument to it are to be changed.  However,
   * when an ev structure has been added to libevent using event_add() the
   * structure must persist until the event occurs (assuming EV_PERSIST
   * is not set) or is removed using event_del().  You may not reuse the same
   * ev structure for multiple monitored descriptors; each descriptor needs
   * its own ev.
   */
  event_set(&event_, socket_, eventFlags_, TConnection::eventHandler, this);

  // Add the event
  if (event_add(&event_, 0) == -1) {
    GlobalOutput("TConnection::setFlags(): could not event_add");
  }
}

/**
 * Closes a connection
 */
void TConnection::close() {
  // Delete the registered libevent
  if (event_del(&event_) == -1) {
    GlobalOutput("TConnection::close() event_del");
  }

  // Close the socket
  if (socket_ > 0) {
    ::close(socket_);
  }
  socket_ = 0;

  // close any factory produced transports
  factoryInputTransport_->close();
  factoryOutputTransport_->close();

  // Give this object back to the server that owns it
  server_->returnConnection(this);
}

/**
 * Creates a new connection either by reusing an object off the stack or
 * by allocating a new one entirely
 */
TConnection* TNonblockingServer::createConnection(int socket, short flags) {
  // Check the stack
  if (connectionStack_.empty()) {
    return new TConnection(socket, flags, this);
  } else {
    TConnection* result = connectionStack_.top();
    connectionStack_.pop();
    result->init(socket, flags, this);
    return result;
  }
}

/**
 * Returns a connection to the stack
 */
void TNonblockingServer::returnConnection(TConnection* connection) {
  connectionStack_.push(connection);
}

/**
 * Server socket had something happen
 */
void TNonblockingServer::handleEvent(int fd, short which) {
  // Make sure that libevent didn't fuck up the socket handles
  assert(fd == serverSocket_);
  
  // Server socket accepted a new connection
  socklen_t addrLen;
  struct sockaddr addr;
  addrLen = sizeof(addr);   
  
  // Going to accept a new client socket
  int clientSocket;
  
  // Accept as many new clients as possible, even though libevent signaled only
  // one, this helps us to avoid having to go back into the libevent engine so
  // many times
  while ((clientSocket = accept(fd, &addr, &addrLen)) != -1) {

    // Explicitly set this socket to NONBLOCK mode
    int flags;
    if ((flags = fcntl(clientSocket, F_GETFL, 0)) < 0 ||
        fcntl(clientSocket, F_SETFL, flags | O_NONBLOCK) < 0) {
      GlobalOutput("thriftServerEventHandler: set O_NONBLOCK");
      close(clientSocket);
      return;
    }

    // Create a new TConnection for this client socket.
    TConnection* clientConnection =
      createConnection(clientSocket, EV_READ | EV_PERSIST);

    // Fail fast if we could not create a TConnection object
    if (clientConnection == NULL) {
      fprintf(stderr, "thriftServerEventHandler: failed TConnection factory");
      close(clientSocket);
      return;
    }

    // Put this client connection into the proper state
    clientConnection->transition();
  }
  
  // Done looping accept, now we have to make sure the error is due to
  // blocking. Any other error is a problem
  if (errno != EAGAIN && errno != EWOULDBLOCK) {
    GlobalOutput("thriftServerEventHandler: accept()");
  }
}

/**
 * Main workhorse function, starts up the server listening on a port and
 * loops over the libevent handler.
 */
void TNonblockingServer::serve() {
  // Initialize libevent
  event_init();

  // Print some libevent stats
  fprintf(stderr,
          "libevent %s method %s\n",
          event_get_version(),
          event_get_method());

  struct addrinfo hints, *res, *res0;
  int error;
  char port[sizeof("65536") + 1];
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = PF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  sprintf(port, "%d", port_);

  // Wildcard address
  error = getaddrinfo(NULL, port, &hints, &res0);
  if (error) {
    GlobalOutput("TNonblockingServer::serve() getaddrinfo");
    return;
  }

  // Pick the ipv6 address first since ipv4 addresses can be mapped
  // into ipv6 space.
  for (res = res0; res; res = res->ai_next) {
    if (res->ai_family == AF_INET6 || res->ai_next == NULL)
      break;
  }

  // Create the server socket
  serverSocket_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (serverSocket_ == -1) {
    GlobalOutput("TNonblockingServer::serve() socket() -1");
    return;
  }

  // Set socket to nonblocking mode
  int flags;
  if ((flags = fcntl(serverSocket_, F_GETFL, 0)) < 0 ||
      fcntl(serverSocket_, F_SETFL, flags | O_NONBLOCK) < 0) {
    GlobalOutput("TNonblockingServer::serve() O_NONBLOCK");
    ::close(serverSocket_);
    return;
  }

  int one = 1;
  struct linger ling = {0, 0};
  
  // Set reuseaddr to avoid 2MSL delay on server restart
  setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  // Keepalive to ensure full result flushing
  setsockopt(serverSocket_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

  // Turn linger off to avoid hung sockets
  setsockopt(serverSocket_, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));

  // Set TCP nodelay if available, MAC OS X Hack
  // See http://lists.danga.com/pipermail/memcached/2005-March/001240.html
  #ifndef TCP_NOPUSH
  setsockopt(serverSocket_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  #endif

  if (bind(serverSocket_, res->ai_addr, res->ai_addrlen) == -1) {
    GlobalOutput("TNonblockingServer::serve() bind");
    close(serverSocket_);
    return;
  }

  if (listen(serverSocket_, LISTEN_BACKLOG) == -1) {
    GlobalOutput("TNonblockingServer::serve() listen");
    close(serverSocket_);
    return;
  }

  // Register the server event
  struct event serverEvent;
  event_set(&serverEvent,
            serverSocket_,
            EV_READ | EV_PERSIST,
            TNonblockingServer::eventHandler,
            this);

  // Add the event and start up the server
  if (-1 == event_add(&serverEvent, 0)) {
    GlobalOutput("TNonblockingServer::serve(): coult not event_add");
    return;
  }

  // Run pre-serve callback function if we have one
  if (preServeCallback_) {
    preServeCallback_(preServeCallbackArg_);
  }

  // Run libevent engine, never returns, invokes calls to eventHandler
  event_loop(0);
}

}}} // facebook::thrift::server
