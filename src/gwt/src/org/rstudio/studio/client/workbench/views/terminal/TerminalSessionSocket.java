/*
 * TerminalSessionSocket.java
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

package org.rstudio.studio.client.workbench.views.terminal;

import java.util.LinkedList;

import org.rstudio.core.client.BrowseCap;
import org.rstudio.core.client.Debug;
import org.rstudio.core.client.HandlerRegistrations;
import org.rstudio.core.client.Stopwatch;
import org.rstudio.studio.client.RStudioGinjector;
import org.rstudio.studio.client.common.console.ConsoleOutputEvent;
import org.rstudio.studio.client.common.console.ConsoleProcess;
import org.rstudio.studio.client.common.console.ConsoleProcessInfo;
import org.rstudio.studio.client.common.shell.ShellInput;
import org.rstudio.studio.client.server.VoidServerRequestCallback;
import org.rstudio.studio.client.workbench.prefs.model.UIPrefs;
import org.rstudio.studio.client.workbench.views.terminal.events.TerminalDataInputEvent;
import org.rstudio.studio.client.workbench.views.terminal.xterm.XTermWidget;

import com.google.gwt.core.client.GWT;
import com.google.gwt.event.shared.HandlerRegistration;
import com.google.inject.Inject;
import com.sksamuel.gwt.websockets.CloseEvent;
import com.sksamuel.gwt.websockets.Websocket;
import com.sksamuel.gwt.websockets.WebsocketListenerExt;

/**
 * Manages input and output for the terminal session.
 */
public class TerminalSessionSocket
   implements ConsoleOutputEvent.Handler, 
              TerminalDataInputEvent.Handler
{
   public interface Session
   {
      /**
       * Called when there is user input to process.
       * @param input user input
       */
      void receivedInput(String input);
      
      /**
       * Called when there is output from the server.
       * @param output output from server
       */
      void receivedOutput(String output);
   }
   
   public interface ConnectCallback
   {
      void onConnected();
      void onError(String message);
   }
   
   // Monitor and report input/display lag to console
   class InputEchoTimeMonitor
   {
      class InputDatapoint
      {
         InputDatapoint(String input)
         {
            input_ = input;
            stopWatch_.reset();
         }

         boolean matches(String input, long runningAverage)
         {
            // startsWith allows better chance of matching on Windows, where 
            // winpty often follows each typed character with an escape sequence
            if (input != null && input.startsWith(input_))
            {
               duration_ = stopWatch_.mark("Average " + runningAverage);
               return true;
            }
            return false;
         }
         
         long duration()
         {
            return duration_;
         }
         
         private String input_;
         private Stopwatch stopWatch_ = new Stopwatch();
         private long duration_;
      }
      
      public InputEchoTimeMonitor()
      {
         pending_ = new LinkedList<InputDatapoint>();
      }
      
      public void inputReceived(String input)
      {
         pending_.add(new InputDatapoint(input));
      }
      
      public void outputReceived(String output)
      {
         InputDatapoint item = pending_.poll();
         if (item == null)
            return;
         
         long average = 0;
         if (accumulatedPoints_ > 0)
         {
            average = accumulatedTime_ / accumulatedPoints_;
         }
         if (!item.matches(output, average))
         {
            // output not what we expected, reset the whole list
            pending_.clear();
         }
         else
         {
            accumulatedPoints_++;
            accumulatedTime_ += item.duration();
         }
      }
      
      private LinkedList<InputDatapoint> pending_;
      private long accumulatedPoints_;
      private long accumulatedTime_;
   }
   
   /**
    * Constructor
    * @param session Session to callback with user input and server output.
    * @param xterm Terminal emulator that provides user input, and displays output.
    */
   public TerminalSessionSocket(Session session,
                                XTermWidget xterm)
   {
      RStudioGinjector.INSTANCE.injectMembers(this);

      session_ = session;
      xterm_ = xterm;

      // Show delay between receiving a keystroke and sending it to the 
      // terminal emulator; for diagnostics on laggy typing. Intended for
      // brief use from a command-prompt. Time between input/display shown
      // in console.
      reportTypingLag_ = uiPrefs_.enableReportTerminalLag().getValue();
      if (reportTypingLag_)
      {
         inputEchoTiming_ = new InputEchoTimeMonitor();
      }
   }

   @Inject
   private void initialize(UIPrefs uiPrefs)
   {
      uiPrefs_ = uiPrefs;
   }
   
   /**
    * Connect the input/output channel to the server. This requires that
    * an rsession has already been started via RPC and the consoleProcess
    * received.
    * @param consoleProcess 
    * @param callback result of connect attempt
    */
   public void connect(ConsoleProcess consoleProcess, 
                       final ConnectCallback callback)
   {
      consoleProcess_ = consoleProcess;

      // We keep this handler connected after a disconnect so
      // user input sent via RPC can wake up a suspended session
      if (terminalInputHandler_ == null)
         terminalInputHandler_ = xterm_.addTerminalDataInputHandler(this);

      addHandlerRegistration(consoleProcess_.addConsoleOutputHandler(this));

      switch (consoleProcess_.getProcessInfo().getChannelMode())
      {
      case ConsoleProcessInfo.CHANNEL_RPC:
         Debug.devlog("Using RPC");
         callback.onConnected();
         break;
         
      case ConsoleProcessInfo.CHANNEL_WEBSOCKET:
         // For desktop IDE, talk directly to the websocket, anything else, go through
         // the server via the /p proxy.
         String urlSuffix = consoleProcess_.getProcessInfo().getChannelId() + "/terminal/" + 
               consoleProcess_.getProcessInfo().getHandle() + "/";
         String url;
         if (BrowseCap.isWindowsDesktop() || BrowseCap.isCocoaDesktop() || BrowseCap.isLinuxDesktop())
         {
            url = "ws://127.0.0.1:" + urlSuffix;
         }
         else
         {
            url = GWT.getHostPageBaseURL();
            if (url.startsWith("https:"))
            {
               url = "wss:" + url.substring(6) + "/p/" + urlSuffix;
            } 
            else if (url.startsWith("http:"))
            {
               url = "ws:" + url.substring(5) + "/p/" + urlSuffix;
            }
            else
            {
               // TODO (gary) fall back to RPC if can't discover the protocol
               // to use
               callback.onError("Unable to discover websocket protocol");
               return;
            }
         }

         Debug.devlog("Connecting to " + url);
         socket_ = new Websocket(url);
         socket_.addListener(new WebsocketListenerExt() {

            @Override
            public void onClose(CloseEvent event)
            {
               // TODO (gary)
            }

            @Override
            public void onMessage(String msg)
            {
               onConsoleOutput(new ConsoleOutputEvent(msg));
            }

            @Override
            public void onOpen()
            {
               callback.onConnected();
            }

            @Override
            public void onError()
            {
               // TODO (gary) fall back to RPC if unable to connect
               callback.onError("Unable to connect to websocket");
               return;
            }
         });
         
         socket_.open();
         break;
         
      case ConsoleProcessInfo.CHANNEL_PIPE:
      default:
         callback.onError("Channel type not implemented");
         break;
      }
   }
   
   /**
    * Send user input to the server.
    * @param inputSequence used to fix out-of-order RPC calls
    * @param input text to send
    * @param requestCallback callback
    */
   public void dispatchInput(int inputSequence,
                             String input,
                             VoidServerRequestCallback requestCallback)
   {
      switch (consoleProcess_.getProcessInfo().getChannelMode())
      {
      case ConsoleProcessInfo.CHANNEL_RPC:
         consoleProcess_.writeStandardInput(
               ShellInput.create(inputSequence, input,  true /*echo input*/), 
               requestCallback);
         break;
      case ConsoleProcessInfo.CHANNEL_WEBSOCKET:
         socket_.send(input);
         requestCallback.onResponseReceived(null);
         break;
      case ConsoleProcessInfo.CHANNEL_PIPE:
      default:
         break;
      }
      
   }
   
   /**
    * Send output to the terminal emulator.
    * @param output text to send to the terminal
    */
   public void dispatchOutput(String output)
   {
      xterm_.write(output);
   }

   @Override
   public void onTerminalDataInput(TerminalDataInputEvent event)
   {
      if (reportTypingLag_)
         inputEchoTiming_.inputReceived(event.getData());
      session_.receivedInput(event.getData());
   }

   @Override
   public void onConsoleOutput(ConsoleOutputEvent event)
   {
      if (reportTypingLag_)
         inputEchoTiming_.outputReceived(event.getOutput());
      session_.receivedOutput(event.getOutput());
   }

   private void addHandlerRegistration(HandlerRegistration reg)
   {
      registrations_.add(reg);
   }

   public void unregisterHandlers()
   {
      registrations_.removeHandler();
      if (terminalInputHandler_ != null)
      {
         terminalInputHandler_.removeHandler();
         terminalInputHandler_ = null;
      }
   }

   public void disconnect()
   {
      consoleProcess_ = null;
      registrations_.removeHandler();
   }
 
   private HandlerRegistrations registrations_ = new HandlerRegistrations();
   private final Session session_;
   private final XTermWidget xterm_;
   private ConsoleProcess consoleProcess_;
   private HandlerRegistration terminalInputHandler_;
   private boolean reportTypingLag_;
   private InputEchoTimeMonitor inputEchoTiming_;
   private Websocket socket_;
   
   // Injected ---- 
   private UIPrefs uiPrefs_;
}
