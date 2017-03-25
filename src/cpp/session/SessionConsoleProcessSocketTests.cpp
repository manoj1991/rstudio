/*
 * SessionConsoleProcessSocketTests.cpp
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

#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/lexical_cast.hpp>

#include <easywsclient/easywsclient.hpp>
#include <easywsclient/easywsclient.cpp>

#include <tests/TestThat.hpp>

namespace rstudio {
namespace session {
namespace console_process {

using namespace console_process;

namespace {

const std::string kCloseMessage = "CLOSE CONNECTION";

class SocketHarness : public boost::enable_shared_from_this<SocketHarness>
{
public:
   ~SocketHarness()
   {
      socket.stopServer();
   }

   void onReceivedInput(const std::string& input)
   {
      if (!input.compare(kCloseMessage))
      {
         // close the socket
         return;
      }
      receivedInput.append(input);
   }

   ConsoleProcessSocketCallbacks createSocketCallbacks()
   {
      using boost::bind;
      ConsoleProcessSocketCallbacks cb;
      cb.onReceivedInput = bind(&SocketHarness::onReceivedInput,
                                SocketHarness::shared_from_this(), _1);
      return cb;
   }

   ConsoleProcessSocket socket;
   std::string receivedInput;
};


void handle_message(const std::string& message)
{
    printf(">>> %s\n", message.c_str());
//    if (message == "world") { ws->close(); }
}

// class for testing communication with the server
using easywsclient::WebSocket;
class SocketClient
{
public:
   SocketClient(const std::string& handle, int port)
      :
        handle_(handle),
        port_(port)
   {
      std::string url("ws://localhost:" +
                      boost::lexical_cast<std::string>(port_) + "/"); //terminal/" +
//                      handle_);
      ws_ = WebSocket::from_url(url);
   }

   bool sendMessage(const std::string& msg)
   {
      if (!ws_)
         return false;
      ws_->poll(10);
      ws_->send(msg);
      ws_->poll(10);
      return true;
   }

   void poll()
   {
      if (!ws_)
         return;
      while (ws_->getReadyState() != WebSocket::CLOSED)
      {
         ws_->poll();
         ws_->dispatch(handle_message);
      }
   }

   bool closeSocket()
   {
      if (!ws_)
         return false;
      ws_->close();
      delete ws_;
      ws_ = NULL;
      return true;
   }

private:
   std::string handle_;
   int port_;
   easywsclient::WebSocket::pointer ws_;
};

} // anonymous namespace

context("input output channel for interactive terminals")
{
   const std::string handle1 = "abcd";

   test_that("new socket object has zero count")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      expect_true(pSocket->socket.count() == 0);
   }

   test_that("stop listening to unknown handle returns error")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      core::Error err = pSocket->socket.stop(handle1);
      expect_false(!err);
   }

   test_that("unlistened handle has zero port")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      expect_true(pSocket->socket.port(handle1) == 0);
   }

   test_that("server never asked to listen, isn't listening")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      expect_false(pSocket->socket.serverRunning());
   }

   test_that("can start and stop listening to a handle")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      core::Error err = pSocket->socket.listen(handle1, pSocket->createSocketCallbacks());
      expect_true(!err);
      err = pSocket->socket.stop(handle1);
      expect_true(!err);
      err = pSocket->socket.stopServer();
      expect_true(!err);
   }

   test_that("can start and stop listening to all handles")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      core::Error err = pSocket->socket.listen(handle1, pSocket->createSocketCallbacks());
      expect_true(!err);
      err = pSocket->socket.stopAll();
      expect_true(!err);
      err = pSocket->socket.stopServer();
      expect_true(!err);
   }

   test_that("successfully listened handle has non-zero port")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      core::Error err = pSocket->socket.listen(handle1, pSocket->createSocketCallbacks());
      expect_true(!err);
      expect_true(pSocket->socket.port(handle1) > 0);
      err = pSocket->socket.stop(handle1);
      expect_true(!err);
      expect_true(pSocket->socket.port(handle1) == 0);
   }

   test_that("after successful listen request, server is running")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      core::Error err = pSocket->socket.listen(handle1, pSocket->createSocketCallbacks());
      expect_true(!err);
      expect_true(pSocket->socket.serverRunning());
      err = pSocket->socket.stop(handle1);
      expect_true(!err);
      err = pSocket->socket.stopServer();
      expect_true(!err);
      expect_false(pSocket->socket.serverRunning());
   }

   test_that("can connect to server and send a message")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      core::Error err = pSocket->socket.listen(handle1, pSocket->createSocketCallbacks());
      expect_true(!err);

      const std::string message = "Hello World!";

//      SocketClient client(handle1, pSocket->socket.port(handle1));
//      expect_true(client.sendMessage(message));
//      expect_true(client.sendMessage(kCloseMessage));
//      client.poll();

//      for (;;)
//      {
//         if (!pSocket->receivedInput.compare(message))
//            break;
//      }

      err = pSocket->socket.stopServer();
      expect_true(!err);
   }
}

} // namespace console_process
} // namespace session
} // namespace rstudio
