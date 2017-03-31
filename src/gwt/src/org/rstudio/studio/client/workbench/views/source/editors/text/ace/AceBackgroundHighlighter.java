/*
 * AceBackgroundHighlighter.java
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
package org.rstudio.studio.client.workbench.views.source.editors.text.ace;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import org.rstudio.core.client.HandlerRegistrations;
import org.rstudio.core.client.ListUtil;
import org.rstudio.core.client.StringUtil;
import org.rstudio.core.client.regex.Match;
import org.rstudio.core.client.regex.Pattern;
import org.rstudio.studio.client.workbench.views.source.editors.text.AceEditor;
import org.rstudio.studio.client.workbench.views.source.editors.text.events.DocumentChangedEvent;
import org.rstudio.studio.client.workbench.views.source.editors.text.events.EditorModeChangedEvent;
import org.rstudio.studio.client.workbench.views.source.editors.text.events.FoldChangeEvent;

import com.google.gwt.event.logical.shared.AttachEvent;

public class AceBackgroundHighlighter
      implements EditorModeChangedEvent.Handler,
                 DocumentChangedEvent.Handler,
                 FoldChangeEvent.Handler,
                 AttachEvent.Handler
{
   private static enum State
   {
      UNKNOWN,
      TEXT,
      CHUNK_START,
      CHUNK_BODY,
      CHUNK_END
   };
   
   private static class HighlightPattern
   {
      public HighlightPattern(String begin, String end)
      {
         this.begin = Pattern.create(begin, "");
         this.end = Pattern.create(end, "");
      }
      
      public Pattern begin;
      public Pattern end;
   }
   
   public AceBackgroundHighlighter(AceEditor editor)
   {
      editor_ = editor;
      
      highlightPatterns_ = new ArrayList<HighlightPattern>();
      handlers_ = new HandlerRegistrations(
            editor.addEditorModeChangedHandler(this),
            editor.addDocumentChangedHandler(this),
            editor.addFoldChangeHandler(this),
            editor.addAttachHandler(this));
      
      int n = editor.getRowCount();
      rowStates_ = new ArrayList<State>(n);
      for (int i = 0; i < n; i++)
         rowStates_.set(i, State.UNKNOWN);
      
      refreshHighlighters();
   }
   
   // Handlers ----
   @Override
   public void onEditorModeChanged(EditorModeChangedEvent event)
   {
      refreshHighlighters();
   }
   
   @Override
   public void onDocumentChanged(DocumentChangedEvent event)
   {
      AceDocumentChangeEventNative nativeEvent = event.getEvent();
      
      String action = nativeEvent.getAction();
      String text = nativeEvent.getText();
      
      Range range = nativeEvent.getRange();
      int startRow = range.getStart().getRow();
      int endRow = range.getEnd().getRow();
      
      // TODO: this will need to change with the next version of Ace,
      // as the layout of document changed events will have changed there
      rowStates_.set(startRow, State.UNKNOWN);
      if (action.equals("insertLines"))
      {
         int newlineCount = endRow - startRow;
         for (int i = 0; i < newlineCount; i++)
            rowStates_.add(startRow, State.UNKNOWN);
      }
      else if (action.equals("removeLines"))
      {
         int newlineCount = endRow - startRow;
         for (int i = 0; i < newlineCount; i++)
            rowStates_.remove(startRow);
      }
      else if (action.equals("insertText") && text.equals("\n"))
      {
         rowStates_.add(startRow, State.UNKNOWN);
      }
      else if (action.equals("removeText") && text.equals("\n"))
      {
         rowStates_.remove(startRow);
      }
      
      synchronizeFrom(startRow);
   }
   
   @Override
   public void onFoldChange(FoldChangeEvent event)
   {
      // TODO
   }
   
   @Override
   public void onAttachOrDetach(AttachEvent event)
   {
      // TODO
   }
   
   // Private Methods ----
   HighlightPattern selectPattern(String line)
   {
      for (HighlightPattern pattern : highlightPatterns_)
      {
         Pattern begin = pattern.begin;
         Match match = begin.match(line, 0);
         if (match != null)
            return pattern;
      }
      return null;
   }
   
   private void initializeHighlighter(int startRow)
   {
      for (int row = startRow; row >= 0; row--)
      {
         State state = rowStates_.get(row);
         switch (state)
         {
         case TEXT:
            break;
            
         case CHUNK_BODY:
         case CHUNK_END:
         case UNKNOWN:
            continue;
            
         case CHUNK_START:
            String line = editor_.getLine(row);
            activeHighlightPattern_ = selectPattern(line);
            return;
         }
      }
      
      activeHighlightPattern_ = null;
   }
   
   private void finalizeHighlighter()
   {
      activeHighlightPattern_ = null;
   }
   
   private boolean beginHighlight(String line)
   {
      HighlightPattern pattern = selectPattern(line);
      if (pattern != null)
      {
         activeHighlightPattern_ = pattern;
         return true;
      }
      
      return false;
   }
   
   private boolean endHighlight(String line)
   {
      HighlightPattern pattern = activeHighlightPattern_;
      if (pattern.e)
   }
   
   private State computeState(int row)
   {
      String line = editor_.getLine(row);
      State state = row >= 0
            ? rowStates_.get(row - 1)
            : State.TEXT;
      
      switch (state)
      {
      case UNKNOWN:
         return State.UNKNOWN;
      case TEXT:
      case CHUNK_END:
         return beginHighlight(line)
               ? State.CHUNK_START
               : State.TEXT;
      case CHUNK_START:
      case CHUNK_BODY:
         return endHighlight(line)
               ? State.CHUNK_END
               : State.CHUNK_BODY;
      }
   }
   
   private void synchronizeFrom(int startRow)
   {
      // if this row has no state, then we need to look
      // back until we find a row with cached state
      while (startRow > 0 && rowStates_.get(startRow - 1) == State.UNKNOWN)
         startRow--;
      
      initializeHighlighter(startRow);
      
      int endRow = editor_.getRowCount();
      for (int row = startRow; row < endRow; row++)
      {
         // determine what state this row is in
         State state = computeState(row);
         
         // if there is no change in state, then exit early
         if (state == rowStates_.get(row))
            break;
         
         rowStates_.set(row, state);
      }
      
      finalizeHighlighter(startRow);
   }
   
   private void refreshHighlighters()
   {
      highlightPatterns_.clear();
      
      String modeId = editor_.getModeId();
      if (StringUtil.isNullOrEmpty(modeId))
         return;
      
      if (HIGHLIGHT_PATTERN_REGISTRY.containsKey(modeId))
      {
         highlightPatterns_.addAll(HIGHLIGHT_PATTERN_REGISTRY.get(modeId));
      }
   }
   
   private static List<HighlightPattern> cStyleHighlightPatterns()
   {
      return ListUtil.create(
            new HighlightPattern(
                  "^\\s*[/][*}{3,}\\s*[Rr]\\s*$",
                  "^\\s*[*]+[/]")
            );
   }
   
   private static List<HighlightPattern> htmlStyleHighlightPatterns()
   {
      return ListUtil.create(
            new HighlightPattern(
                  "^<!--\\s*begin[.]rcode\\s*(?:.*)",
                  "^\\s*end[.]rcode\\s*-->")
            );
   }
   
   private static List<HighlightPattern> sweaveHighlightPatterns()
   {
      return ListUtil.create(
            new HighlightPattern(
                  "<<(.*?)>>",
                  "^\\s*@\\s*$")
            );
   }
   
   private static final List<HighlightPattern> rMarkdownHighlightPatterns()
   {
      return ListUtil.create(
            
            // code chunks
            new HighlightPattern(
                  "^(?:[ ]{4})?`{3,}\\s*\\{(?:.*)\\}\\s*$",
                  "^(?:[ ]{4})?`{3,}\\s*$"),
            
            // latex blocks
            new HighlightPattern(
                  "^[$][$]\\s*$",
                  "^[$][$]\\s*$")
            
            );
            
   }
   
   private final AceEditor editor_;
   private HighlightPattern activeHighlightPattern_;
   private final List<HighlightPattern> highlightPatterns_;
   private final HandlerRegistrations handlers_;
   private final List<State> rowStates_;
   
   private static final Map<String, List<HighlightPattern>> HIGHLIGHT_PATTERN_REGISTRY;
   
   static {
      HIGHLIGHT_PATTERN_REGISTRY = new HashMap<String, List<HighlightPattern>>();
      HIGHLIGHT_PATTERN_REGISTRY.put("mode/rmarkdown", rMarkdownHighlightPatterns());
      HIGHLIGHT_PATTERN_REGISTRY.put("mode/c_cpp", cStyleHighlightPatterns());
      HIGHLIGHT_PATTERN_REGISTRY.put("mode/sweave", sweaveHighlightPatterns());
      HIGHLIGHT_PATTERN_REGISTRY.put("mode/rhtml", htmlStyleHighlightPatterns());
   }
}
