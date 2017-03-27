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


#include <session/SessionConsoleProcessSocket.hpp>

#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/lexical_cast.hpp>

#include <easywsclient/easywsclient.hpp>
#include <easywsclient/easywsclient.cpp>

#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>

#include <tests/TestThat.hpp>

namespace rstudio {
namespace session {
namespace console_process {

using namespace console_process;

namespace {

const std::string kCloseMessage = "CLOSE CONNECTION";

// convenience wrapper for testing ConsoleProcessSocket
class SocketHarness : public boost::enable_shared_from_this<SocketHarness>
{
public:
   ConsoleProcessSocketCallbacks createSocketCallbacks()
   {
      using boost::bind;
      ConsoleProcessSocketCallbacks cb;
      cb.onSocketClosed = bind(&SocketHarness::onSocketClosed,
                         SocketHarness::shared_from_this());
      return cb;
   }

   SocketHarness()
   {
   }

   ~SocketHarness()
   {
      socket.stopServer();
   }

   core::Error ensureServerRunning()
   {
      return socket.ensureServerRunning(createSocketCallbacks());
   }

   void onSocketClosed()
   {
      LOG_ERROR_MESSAGE("onClosed");
   }

   void onReceivedInput(const std::string& input)
   {
      if (!input.compare(kCloseMessage))
      {
         socket.stop("abcd");
         socket.sendText("abcd", kCloseMessage);
         return;
      }
      receivedInput.append(input);
   }

   ConsoleProcessSocket socket;
   std::string receivedInput;
};

//typedef websocketpp::client<websocketpp::config::asio_client> client;
//client s_client;

//// class for testing communication with the server
//class SocketClientAsio
//{
//public:
//   SocketClientAsio(const std::string& handle, boost::shared_ptr<SocketHarness> pServerSocket)
//      :
//        handle_(handle),
//        pServerSocket_(pServerSocket)
//   {}

//   bool connectDisconnectSocket()
//   {
//      try
//      {
//         return true;
//      }
//      catch (std::exception& e)
//      {
//         std::cout << "Exception: " << e.what() << "\n";
//      }
//      return false;
//   }

//private:
//   std::string handle_;
//   client endpoint;
//   boost::shared_ptr<SocketHarness> pServerSocket_;
//};


// class for testing communication with the server
using easywsclient::WebSocket;
class SocketClient
{
public:
   SocketClient(const std::string& handle, boost::shared_ptr<SocketHarness> pServerSocket)
      :
        handle_(handle),
        pServerSocket_(pServerSocket),
        gotOpened_(false),
        gotClosed_(false)
   {
      std::string url("ws://localhost:" +
                      boost::lexical_cast<std::string>(pServerSocket_->socket.port()) + "/terminal/" +
                      handle_ + "/");
      ws_ = WebSocket::from_url(url);
   }

   void onReceivedInput(const std::string& input)
   {
      input_.append(input);
   }

   void onConnectionOpened()
   {
      gotOpened_ = true;
   }

   void onConnectionClosed()
   {
      gotClosed_ = true;
   }

   ConsoleProcessSocketConnectionCallbacks createConsoleProcessSocketConnectionCallbacks()
   {
      ConsoleProcessSocketConnectionCallbacks cb;
      cb.onReceivedInput = boost::bind(&SocketClient::onReceivedInput, this, _1);
      cb.onConnectionOpened = boost::bind(&SocketClient::onConnectionOpened, this);
      cb.onConnectionClosed = boost::bind(&SocketClient::onConnectionClosed, this);
      return cb;
   }

   core::Error listen()
   {
      return pServerSocket_->socket.listen(handle_, createConsoleProcessSocketConnectionCallbacks());
   }

   core::Error stop()
   {
      return pServerSocket_->socket.stop(handle_);
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

   void handle_message(const std::string& message)
   {
      if (!message.compare(kCloseMessage))
      {
         ws_->close();
      }
   }

   void poll()
   {
      if (!ws_)
         return;
      while (ws_->getReadyState() != WebSocket::CLOSED)
      {
         ws_->poll();
         ws_->dispatch(boost::bind(&SocketClient::handle_message, this, _1));
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

   std::string getInput() { return input_; }
   bool gotOpened() { return gotOpened_; }
   bool gotClosed() { return gotClosed_; }

private:
   std::string handle_;
   easywsclient::WebSocket::pointer ws_;
   boost::shared_ptr<SocketHarness> pServerSocket_;
   std::string input_;
   bool gotOpened_;
   bool gotClosed_;
};

} // anonymous namespace

context("input output channel for interactive terminals")
{
   const std::string handle1 = "abcd";

   test_that("new socket object has zero connections count")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      expect_true(pSocket->socket.connectionCount() == 0);
   }

   test_that("new socket object has zero port")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      expect_true(pSocket->socket.port() == 0);
   }

   test_that("cannot listen if server is not running")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      SocketClient client(handle1, pSocket);
      core::Error err = client.listen();
      expect_true(err);
   }

   test_that("can start and stop the socket server")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      core::Error err = pSocket->ensureServerRunning();
      expect_false(err);

      err = pSocket->socket.stopServer();
      expect_false(err);
   }

   test_that("server port returned correctly when started and stopped")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      pSocket->ensureServerRunning();
      expect_true(pSocket->socket.port() > 0);
      pSocket->socket.stopServer();
      expect_true(pSocket->socket.port() == 0);
   }

   test_that("stop listening to unknown handle returns error")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      core::Error err = pSocket->ensureServerRunning();
      expect_false(err);
      SocketClient client(handle1, pSocket);
      err = client.stop();
      expect_false(!err);
   }

   test_that("can start and stop listening to a handle")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      core::Error err = pSocket->ensureServerRunning();
      expect_false(err);
      SocketClient client(handle1, pSocket);
      err = client.listen();
      expect_true(!err);
      err = client.stop();
      expect_true(!err);

      err = pSocket->socket.stopServer();
      expect_true(!err);
   }

   test_that("can start and stop listening to all handles")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      core::Error err = pSocket->ensureServerRunning();
      expect_false(err);
      SocketClient client(handle1, pSocket);
      err = client.listen();
      expect_true(!err);
      err = client.stop();
      expect_true(!err);

      err = pSocket->socket.stopServer();
      expect_true(!err);
   }

   test_that("can connect to server and send a message")
   {
      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
      core::Error err = pSocket->ensureServerRunning();
      expect_false(err);
      SocketClient client(handle1, pSocket);
      err = client.listen();
      expect_true(!err);

      const std::string message = "Hello World!";

      expect_true(client.sendMessage(message));
      expect_true(client.sendMessage(kCloseMessage));
      client.poll();

      for (;;)
      {
         if (!pSocket->receivedInput.compare(message))
            break;
      }

      err = client.stop();
      expect_true(!err);
      err = pSocket->socket.stopServer();
      expect_true(!err);
   }

//   test_that("can use socketclientasio")
//   {
//      boost::shared_ptr<SocketHarness> pSocket = boost::make_shared<SocketHarness>();
//      core::Error err = pSocket->ensureServerRunning();
//      expect_false(err);
//      SocketClientAsio socket(handle1, pSocket);

//      err = pSocket->socket.stopServer();
//      expect_true(!err);
//   }
}

} // namespace console_process
} // namespace session
} // namespace rstudio
