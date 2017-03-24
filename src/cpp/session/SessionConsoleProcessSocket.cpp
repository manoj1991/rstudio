/*
 * SessionConsoleProcessSocket.cpp
 *
 * Copyright (C) 2009-17 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "SessionConsoleProcessSocket.hpp"

#include <core/FilePath.hpp>
#include <core/json/Json.hpp>

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <websocketpp/frame.hpp>

namespace rstudio {
namespace session {
namespace console_process {

using namespace rstudio::core;

typedef websocketpp::server<websocketpp::config::asio> server;
typedef server::message_ptr message_ptr;

namespace {

bool s_didSeedRand = false;


void onOpen(server* s, websocketpp::connection_hdl hdl)
{
   if (s)
   {
      LOG_ERROR_MESSAGE("onOpen");
   }
}

void onClose(server* s, websocketpp::connection_hdl hdl)
{
   if (s)
   {
      LOG_ERROR_MESSAGE("onClose");
   }
}

void onHttp(server* s, websocketpp::connection_hdl hdl)
{
   if (s)
   {
      LOG_ERROR_MESSAGE("onHttp");
   }
   server::connection_ptr con = s->get_con_from_hdl(hdl);

   std::stringstream output;
   output << "<html><body><pre>" << std::endl;
   output << "Interesting diagnostics here.";
   output << "</pre></body><html>" << std::endl;

   con->set_status(websocketpp::http::status_code::ok);
   con->set_body(output.str());
}

void onMessage(server* s, websocketpp::connection_hdl hdl, message_ptr msg)
{
   if (s)
   {
      LOG_ERROR_MESSAGE("onMessage");
   }
}

} // anonymous namespace

struct ConsoleProcessSocket::Impl
{
   Impl()
      :
        port_(0),
        serverRunning_(false)
   {}


   int port_;
   boost::thread websocketThread_;
   bool serverRunning_;
   server wsServer_;
};

ConsoleProcessSocket::ConsoleProcessSocket()
   :
     pImpl_(new Impl())
{
}

ConsoleProcessSocket::~ConsoleProcessSocket()
{
   try
   {
      stopAll();
      stopServer();
   }
   catch (...)
   {
   }
}

Error ConsoleProcessSocket::listen(
      const std::string& terminalHandle,
      const ConsoleProcessSocketCallbacks& callbacks)
{
   Error error = ensureServerRunning();
   if (error)
      return error;

   handle_ = terminalHandle;
   callbacks_ = callbacks;

   return error;
}

Error ConsoleProcessSocket::stop(const std::string& terminalHandle)
{
   if (handle_.compare(terminalHandle))
   {
      return systemError(boost::system::errc::invalid_argument,
                         terminalHandle,
                         ERROR_LOCATION);
   }
   return stopAll();
}

Error ConsoleProcessSocket::stopAll()
{
   Error err;
   handle_.clear();
   return err;
}

int ConsoleProcessSocket::count() const
{
   if (handle_.empty())
      return 0;
   else
      return 1;
}

int ConsoleProcessSocket::port(const std::string& terminalHandle) const
{
   if (!handle_.compare(terminalHandle))
   {
      return pImpl_->port_;
   }
   else
   {
      return 0;
   }
}

Error ConsoleProcessSocket::stopServer()
{
   try
   {
      if (pImpl_->serverRunning_)
      {
         stopAll();
         pImpl_->wsServer_.stop();
         pImpl_->serverRunning_ = false;
         pImpl_->port_ = 0;
         pImpl_->websocketThread_.join();
      }
   }
   catch (websocketpp::exception const& e)
   {
      LOG_ERROR_MESSAGE(e.what());
      return systemError(boost::system::errc::invalid_argument,
                         e.what(),
                         ERROR_LOCATION);
   }
   catch (...)
   {
      LOG_ERROR_MESSAGE("Unknown exception stopping terminal websocket server");
      return systemError(boost::system::errc::invalid_argument,
                         "Unknown exception", ERROR_LOCATION);
   }
   return Success();
}

Error ConsoleProcessSocket::ensureServerRunning()
{
   if (pImpl_->serverRunning_)
      return Success();

   long port = 0;
   unsigned portRetries = 0;

   // initialize seed for random port selection
   if (!s_didSeedRand)
   {
      srand(time(NULL));
      s_didSeedRand = true;
   }

   // no user-specified port; pick a random port
   port = 3000 + (rand() % 5000);

   try
   {
      pImpl_->wsServer_.set_access_channels(websocketpp::log::alevel::none);
      pImpl_->wsServer_.init_asio();

      pImpl_->wsServer_.set_message_handler(
               boost::bind(&onMessage,&pImpl_->wsServer_, _1, _2));
      pImpl_->wsServer_.set_http_handler(
               boost::bind(&onHttp, &pImpl_->wsServer_, _1));
      pImpl_->wsServer_.set_close_handler(
               boost::bind(&onClose, &pImpl_->wsServer_, _1));
      pImpl_->wsServer_.set_open_handler
            (boost::bind(&onOpen, &pImpl_->wsServer_, _1));

      // try to bind to the given port
      do
      {
         try
         {
            if (core::FilePath("/proc/net/if_inet6").exists())
            {
               // listen will fail without ipv6 support on the machine so we
               // only use it for machines with a ipv6 stack
               // TODO (gary) need a cross-platform way to do this
               pImpl_->wsServer_.listen(port);
            }
            else
            {
               // no ipv6 support, fall back to ipv4
               pImpl_->wsServer_.listen(boost::asio::ip::tcp::v4(), port);
            }

            pImpl_->wsServer_.start_accept();
            break;
         }
         catch (websocketpp::exception const& e)
         {
            // fail if this isn't the code we're expecting
            // (we're only trying to deal with address in use errors here)
            if (e.code() != websocketpp::transport::asio::error::pass_through)
            {
               return systemError(boost::system::errc::invalid_argument,
                                  e.what(), ERROR_LOCATION);
            }

            // try another random port
            port = 3000 + (rand() % 5000);
         }
      }
      while (++portRetries < 20);

      // if we used up all our retries, abort now
      if (portRetries == 20)
      {
         return systemError(boost::system::errc::not_supported,
                            "Couldn't find an available port",
                            ERROR_LOCATION);
      }

      // TODO (gary) Should we have a shutdown timer if there are no
      // connections after a certain period of time?

      // start server
      core::thread::safeLaunchThread(
               boost::bind(&ConsoleProcessSocket::watchSocket, this),
               &pImpl_->websocketThread_);

      pImpl_->port_ = port;
      pImpl_->serverRunning_ = true;
   }
   catch (websocketpp::exception const& e)
   {
      LOG_ERROR_MESSAGE(e.what());
      return systemError(boost::system::errc::invalid_argument,
                            e.what(), ERROR_LOCATION);
   }
   catch (...)
   {
      LOG_ERROR_MESSAGE("Unknown exception starting up terminal websocket");
      return systemError(boost::system::errc::invalid_argument,
                         "Unknown exception", ERROR_LOCATION);
   }
   return Success();
}

void ConsoleProcessSocket::watchSocket()
{
   pImpl_->wsServer_.run();
}

bool ConsoleProcessSocket::serverRunning() const
{
   return pImpl_->serverRunning_;
}

} // namespace console_process
} // namespace session
} // namespace rstudio
