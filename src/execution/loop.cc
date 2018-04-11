/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include "loop.hh"

#include <stdexcept>

#include "net/http_response_parser.hh"
#include "thunk/ggutils.hh"
#include "util/exception.hh"
#include "util/optional.hh"

using namespace std;
using namespace PollerShortNames;

using ReductionResult = gg::cache::ReductionResult;

ExecutionLoop::ExecutionLoop()
  : signals_( { SIGCHLD, SIGCONT, SIGHUP, SIGTERM, SIGQUIT, SIGINT } ),
    signal_fd_( signals_ )
{
  signals_.set_as_mask();

  poller_.add_action(
    Poller::Action(
      signal_fd_.fd(), Direction::In,
      [&]() { return handle_signal( signal_fd_.read_signal() ); },
      [&]() { return ( child_processes_.size() > 0 or
                       connections_.size() > 0 or
                       ssl_connections_.size() > 0 ); }
    )
  );
}

Poller::Result ExecutionLoop::loop_once( const int timeout_ms )
{
  return poller_.poll( timeout_ms );
}

template<>
typename list<shared_ptr<TCPConnection>>::iterator
ExecutionLoop::create_connection( TCPSocket && socket )
{
  return connections_.emplace( connections_.end(),
                               make_shared<TCPConnection>( move( socket ) ) );
}

template<>
typename list<shared_ptr<SSLConnection>>::iterator
ExecutionLoop::create_connection( NBSecureSocket && socket )
{
  return ssl_connections_.emplace( ssl_connections_.end(),
                                   make_shared<SSLConnection>( move( socket ) ) );
}

template<>
void ExecutionLoop::remove_connection<TCPConnection>( const list<shared_ptr<TCPConnection>>::iterator & it )
{
  connections_.erase( it );
}

template<>
void ExecutionLoop::remove_connection<SSLConnection>( const list<shared_ptr<SSLConnection>>::iterator & it )
{
  ssl_connections_.erase( it );
}

template<>
void ExecutionLoop::add_connection( const shared_ptr<TCPConnection> & connection,
                                    const function<bool(string &&)> & data_callback,
                                    const function<void()> & error_callback,
                                    const function<void()> & close_callback )
{
  auto fderror_callback =
    [error_callback, close_callback]
    {
      error_callback();
      close_callback();
    };

  poller_.add_action(
    Poller::Action(
      connection->socket_, Direction::Out,
      [connection] ()
      {
        string::const_iterator last_write =
          connection->socket_.write( connection->write_buffer_.begin(),
                                     connection->write_buffer_.cend() );

        connection->write_buffer_.erase( 0, last_write - connection->write_buffer_.cbegin() );
        return ResultType::Continue;
      },
      [connection] { return connection->write_buffer_.size(); },
      fderror_callback
    )
  );

  poller_.add_action(
    Poller::Action(
      connection->socket_, Direction::In,
      [connection, data_callback { move( data_callback ) },
       close_callback { move( close_callback ) }, this] ()
      {
        string data { move( connection->socket_.read() ) };

        if ( data.empty() or not data_callback( move( data ) ) ) {
          close_callback();
          return ResultType::CancelAll;
        }

        return ResultType::Continue;
      },
      [connection]() { return true; },
      fderror_callback
    )
  );
}

template<>
void ExecutionLoop::add_connection( const shared_ptr<SSLConnection> & connection,
                                    const function<bool(string &&)> & data_callback,
                                    const function<void()> & error_callback,
                                    const function<void()> & close_callback )
{
  auto fderror_callback =
    [error_callback, close_callback]
    {
      error_callback();
      close_callback();
    };

  poller_.add_action(
    Poller::Action(
      connection->socket_, Direction::Out,
      [connection] ()
      {
        connection->socket_.ezwrite( move( connection->write_buffer_ ) );
        connection->write_buffer_ = string {};
        return ResultType::Continue;
      },
      [connection] { return connection->write_buffer_.size(); },
      fderror_callback
    )
  );

  poller_.add_action(
    Poller::Action(
      connection->socket_, Direction::In,
      [connection, data_callback, close_callback] ()
      {
        string data { move( connection->socket_.ezread() ) };

        if ( data.empty() or not data_callback( move( data ) ) ) {
          close_callback();
          return ResultType::CancelAll;
        }

        return ResultType::Continue;
      },
      [connection]() { return true; },
      fderror_callback
    )
  );
}

template<>
shared_ptr<TCPConnection>
ExecutionLoop::make_connection( const Address & address,
                                const function<bool(string &&)> & data_callback,
                                const function<void()> & error_callback,
                                const function<void()> & close_callback )
{
  TCPSocket socket;
  socket.set_blocking( false );
  socket.connect_nonblock( address );

  auto connection_it = create_connection<TCPSocket>( move( socket ) );
  auto full_close_callback = [connection_it, close_callback, this] () {
    close_callback();
    remove_connection<TCPConnection>( connection_it );
  };

  add_connection( *connection_it, data_callback, error_callback, close_callback );
  return *connection_it;
}

template<>
shared_ptr<SSLConnection>
ExecutionLoop::make_connection( const Address & address,
                                const function<bool(string &&)> & data_callback,
                                const function<void()> & error_callback,
                                const function<void()> & close_callback )
{
  TCPSocket socket;
  socket.set_blocking( false );
  socket.connect_nonblock( address );
  NBSecureSocket secure_socket { move( ssl_context_.new_secure_socket( move( socket ) ) ) };
  secure_socket.connect();

  auto connection_it = create_connection<NBSecureSocket>( move( secure_socket ) );
  auto full_close_callback = [connection_it, close_callback, this] () {
    close_callback();
    remove_connection<SSLConnection>( connection_it );
  };

  add_connection( *connection_it, data_callback, error_callback, full_close_callback );
  return *connection_it;
}

template<class ConnectionType>
uint64_t ExecutionLoop::make_http_request( const string & tag,
                                           const Address & address,
                                           const HTTPRequest & request,
                                           HTTPResponseCallbackFunc response_callback,
                                           FailureCallbackFunc failure_callback )
{
  const uint64_t connection_id = current_id_++;

  auto parser = make_shared<HTTPResponseParser>();
  parser->new_request_arrived( request );

  auto data_callback =
    [parser, connection_id, tag, response_callback] ( string && data ) {
      parser->parse( data );

      if ( not parser->empty() ) {
        response_callback( connection_id, tag, parser->front() );
        parser->pop();
        return false;
      }

      return true;
    };

  auto error_callback =
    [connection_id, tag, failure_callback]
    { failure_callback( connection_id, tag ); };

  auto close_callback = [] {};

  auto connection = make_connection<ConnectionType>( address, data_callback, error_callback, close_callback );

  connection->write_buffer_ = move( request.str() );

  return connection_id;
}

uint64_t ExecutionLoop::make_listener( const Address & address,
                                       const function<bool(ExecutionLoop &,
                                                           shared_ptr<TCPConnection> &)> & connection_callback )
{
  TCPSocket socket;
  socket.set_blocking( false );
  socket.set_reuseaddr();
  socket.bind( address );
  socket.listen();

  auto connection_it = create_connection<TCPSocket>( move( socket ) );
  shared_ptr<TCPConnection> & connection_ptr = *connection_it;

  poller_.add_action( Poller::Action( (*connection_it)->socket_,
    Direction::In,
    [connection_ptr, connection_callback, this] () -> ResultType
    {
      auto new_connection = create_connection<TCPSocket>( move( connection_ptr->socket_.accept() ) );

      if ( not connection_callback( *this, *new_connection ) ) {
        return ResultType::CancelAll;
      }

      return ResultType::Continue;
    } ) );

  return current_id_++;
}

uint64_t ExecutionLoop::add_child_process( const string & tag,
                                           LocalCallbackFunc callback,
                                           FailureCallbackFunc /* failure_callback */,
                                           function<int()> && child_procedure )
{
  child_processes_.emplace_back( current_id_, callback, ChildProcess( tag, move( child_procedure ) ) );
  return current_id_++;
}

Poller::Action::Result ExecutionLoop::handle_signal( const signalfd_siginfo & sig )
{
  switch ( sig.ssi_signo ) {
  case SIGCONT:
    for ( auto & child : child_processes_ ) {
      get<2>( child ).resume();
    }
    break;

  case SIGCHLD:
    if ( child_processes_.empty() ) {
      throw runtime_error( "received SIGCHLD without any managed children" );
    }

    for ( auto it = child_processes_.begin(); it != child_processes_.end(); it++ ) {
      ChildProcess & child = get<2>( *it );

      if ( child.terminated() or ( not child.waitable() ) ) {
        continue;
      }

      child.wait( true );

      if ( child.terminated() ) {
        if ( child.exit_status() != 0 ) {
          child.throw_exception();
        }

        auto & callback = get<1>( *it );
        callback( get<0>( *it ), child.name() );

        it = child_processes_.erase( it );
        it--;
      }
      else if ( not child.running() ) {
        /* suspend parent too */
        CheckSystemCall( "raise", raise( SIGSTOP ) );
      }
    }

    break;

  case SIGHUP:
  case SIGTERM:
  case SIGQUIT:
  case SIGINT:
    throw runtime_error( "interrupted by signal" );

  default:
    throw runtime_error( "unknown signal" );
  }

  return ResultType::Continue;
}

template
uint64_t ExecutionLoop::make_http_request<TCPConnection>( const string &,
                                                          const Address &,
                                                          const HTTPRequest &,
                                                          HTTPResponseCallbackFunc,
                                                          FailureCallbackFunc );

template
uint64_t ExecutionLoop::make_http_request<SSLConnection>( const string &,
                                                          const Address &,
                                                          const HTTPRequest &,
                                                          HTTPResponseCallbackFunc,
                                                          FailureCallbackFunc );
