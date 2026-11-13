/*
 * Copyright (c) 2025-2026, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "language.h"
#include "language_highlight_autogen.h"
#include "utilities.h"

#include <nvgui/fonts.hpp>
#include <nvgui/tooltip.hpp>
#include <nvutils/file_operations.hpp>
#include <nvutils/logger.hpp>

#include <imgui/imgui_internal.h>
#include <tinygltf/json.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include <Windows.h>
#else
#include <asm/termbits.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// Define this to print every message sent and received over the server language protocol.
// #define SERVER_VERBOSE

// Define this to keep track of all messages sent to and received from slangd.
// This also writes a JSON file on crash.
// It's disabled in release mode by default because it uses more memory over time.
#ifndef NDEBUG
#define RECORD_SLANGD
#endif

static std::unique_ptr<TextEditor::Language> s_slangInfo;
static std::unique_ptr<TextEditor::Language> s_spirVInfo;

const TextEditor::Language* getSlangLanguageInfo()
{
  if(!s_slangInfo)
  {
    // First time here! Initialize it:
    s_slangInfo                         = std::make_unique<TextEditor::Language>();
    s_slangInfo->name                   = "Slang";
    s_slangInfo->caseSensitive          = true;
    s_slangInfo->preprocess             = L'#';
    s_slangInfo->singleLineComment      = "//";
    s_slangInfo->commentStart           = "/*";
    s_slangInfo->commentEnd             = "*/";
    s_slangInfo->hasSingleQuotedStrings = false;
    s_slangInfo->hasDoubleQuotedStrings = true;
    s_slangInfo->stringEscape           = L'\\';  // I think?
    for(const auto& item : gSlangHighlightKeywords)
    {
      s_slangInfo->keywords.insert(item);
    }
    for(const auto& item : gSlangHighlightDeclarations)
    {
      s_slangInfo->declarations.insert(item);
    }

    s_slangInfo->isPunctuation = TextEditor::Language::C()->isPunctuation;
    s_slangInfo->getIdentifier = TextEditor::Language::C()->getIdentifier;
    s_slangInfo->getNumber     = TextEditor::Language::C()->getNumber;
  }

  return s_slangInfo.get();
}

const TextEditor::Language* getSpirVLanguageInfo()
{
  if(!s_spirVInfo)
  {
    // First time here! Initialize it:
    s_spirVInfo                         = std::make_unique<TextEditor::Language>();
    s_spirVInfo->caseSensitive          = true;
    s_spirVInfo->name                   = "SPIR-V";
    s_spirVInfo->singleLineComment      = ';';
    s_spirVInfo->hasDoubleQuotedStrings = true;
    s_spirVInfo->stringEscape           = L'\\';

    s_spirVInfo->isPunctuation = TextEditor::Language::C()->isPunctuation;  // Close enough
    s_spirVInfo->getIdentifier = TextEditor::Language::C()->getIdentifier;
    s_spirVInfo->getNumber     = TextEditor::Language::C()->getNumber;
  }
  return s_spirVInfo.get();
}

//-----------------------------------------------------------------------------
// TextEditorEx
void TextEditorEx::Render(const char* title, const ImVec2& size, bool border)
{
  render(title, size, border);
  // HACK: O(n)
  // Find the child of the current window
  const ImGuiID myID = ImGui::GetCurrentWindow()->GetID(title);
  for(const ImGuiWindow* otherWindow : GImGui->Windows)
  {
    if(otherWindow != nullptr && otherWindow->ChildId == myID)
    {
      m_screenPosOnRender = otherWindow->DC.CursorPos;
    }
  }
}

bool TextEditorEx::GetCharacterScreenPos(ImVec2& screenPos, int line, int column)
{
  // For reference, see TextEditor::renderText().
  screenPos = m_screenPosOnRender;
  screenPos += ImVec2(textOffset + column * glyphSize.x, line * glyphSize.y);

  return (firstVisibleLine <= line && line <= lastVisibleLine)  //
         && (firstVisibleColumn <= column && column <= lastVisibleColumn);
}

void TextEditorEx::ReplaceRange(int startLine, int startColumn, int endLine, int endColumn, const std::string_view& text)
{
  const Coordinate start(startLine, startColumn);
  const Coordinate end(endLine, endColumn);

  std::shared_ptr<Transaction> transaction = startTransaction();
  deleteText(transaction, start, end);
  cursors.getMain().adjustForDelete(start, end);
  const Coordinate newEnd = insertText(transaction, start, text);
  cursors.getMain().adjustForInsert(start, newEnd);
  endTransaction(std::move(transaction));
}

void TextEditorEx::SetVimModeEnabled(bool enabled)
{
  if(m_vimEnabled == enabled)
  {
    return;
  }
  m_vimEnabled = enabled;
  m_vimMode    = VimMode::eNormal;
  overwrite    = false;
  resetVimPendingState();
  if(m_vimEnabled && !document.isEmpty())
  {
    cursors.clearAdditional(true);
    Coordinate cur = clampVimNormalCoord(cursors.getCurrent().getInteractiveEnd());
    cursors.setCursor(cur);
  }
}

std::string TextEditorEx::GetVimStatusText() const
{
  if(!m_vimEnabled)
  {
    return "";
  }
  std::string status;
  switch(m_vimMode)
  {
    case VimMode::eNormal:
      status = "-- NORMAL --";
      break;
    case VimMode::eInsert:
      status = "-- INSERT --";
      break;
    case VimMode::eReplace:
      status = "-- REPLACE --";
      break;
    case VimMode::eVisual:
      status = "-- VISUAL --";
      break;
    case VimMode::eVisualLine:
      status = "-- VISUAL LINE --";
      break;
  }

  std::string pending;
  if(m_vimCount > 0)
  {
    pending += std::to_string(m_vimCount);
  }
  if(m_vimPendingOp != 0)
  {
    pending += m_vimPendingOp;
  }
  if(m_vimPendingSubOp != 0)
  {
    pending += m_vimPendingSubOp;
  }
  if(!pending.empty())
  {
    status += "  [" + pending + "]";
  }
  return status;
}

void TextEditorEx::resetVimPendingState()
{
  m_vimCount        = 0;
  m_vimPendingOp    = 0;
  m_vimPendingSubOp = 0;
}

TextEditor::Coordinate TextEditorEx::clampVimNormalCoord(Coordinate c) const
{
  c         = document.normalizeCoordinate(c);
  auto& ln  = document[c.line];
  if(ln.empty())
  {
    return Coordinate(c.line, 0);
  }
  size_t idx = document.getIndex(c);
  if(idx >= ln.size())
  {
    idx = ln.size() - 1;
  }
  return Coordinate(c.line, document.getColumn(c.line, idx));
}

TextEditor::Coordinate TextEditorEx::getVimCharEndOnLine(Coordinate c) const
{
  c         = document.normalizeCoordinate(c);
  auto& ln  = document[c.line];
  if(ln.empty())
  {
    return Coordinate(c.line, 0);
  }
  size_t idx = document.getIndex(c);
  if(idx < ln.size())
  {
    idx++;
  }
  return Coordinate(c.line, document.getColumn(c.line, idx));
}

TextEditor::Coordinate TextEditorEx::getVimFirstNonBlank(int line) const
{
  line = std::clamp(line, 0, document.lineCount() - 1);
  const auto& ln = document[line];
  size_t idx = 0;
  while(idx < ln.size() && CodePoint::isWhiteSpace(ln[idx].codepoint))
  {
    idx++;
  }
  if(idx >= ln.size() && !ln.empty())
  {
    idx = ln.size() - 1;
  }
  return Coordinate(line, document.getColumn(line, idx));
}

static int getVimCharClass(ImWchar cp, bool bigWord)
{
  if(TextEditor::CodePoint::isWhiteSpace(cp))
  {
    return 0;
  }
  if(bigWord)
  {
    return 1;
  }
  return TextEditor::CodePoint::isWord(cp) ? 1 : 2;
}

TextEditor::Coordinate TextEditorEx::getVimWordForward(Coordinate from, bool bigWord) const
{
  from       = document.normalizeCoordinate(from);
  int line   = from.line;
  size_t idx = document.getIndex(from);

  if(idx < document[line].size())
  {
    const int startClass = getVimCharClass(document[line][idx].codepoint, bigWord);
    if(startClass != 0)
    {
      while(idx < document[line].size() && getVimCharClass(document[line][idx].codepoint, bigWord) == startClass)
      {
        idx++;
      }
    }
  }

  while(true)
  {
    while(idx < document[line].size() && CodePoint::isWhiteSpace(document[line][idx].codepoint))
    {
      idx++;
    }
    if(idx < document[line].size())
    {
      return Coordinate(line, document.getColumn(line, idx));
    }
    if(line + 1 >= document.lineCount())
    {
      return Coordinate(line, document[line].maxColumn);
    }
    line++;
    idx = 0;
    if(document[line].empty())
    {
      return Coordinate(line, 0);
    }
  }
}

TextEditor::Coordinate TextEditorEx::getVimWordBackward(Coordinate from, bool bigWord) const
{
  from       = document.normalizeCoordinate(from);
  int line   = from.line;
  size_t idx = document.getIndex(from);

  // Step 1 character left (crossing lines if needed)
  while(true)
  {
    if(idx > 0)
    {
      idx--;
      break;
    }
    if(line == 0)
    {
      return Coordinate(0, 0);
    }
    line--;
    idx = document[line].size();
    if(idx == 0)
    {
      return Coordinate(line, 0);
    }
  }

  // Skip whitespace backwards across lines
  while(true)
  {
    while(idx > 0 && CodePoint::isWhiteSpace(document[line][idx].codepoint))
    {
      idx--;
    }
    if(!document[line].empty() && !CodePoint::isWhiteSpace(document[line][idx].codepoint))
    {
      break;
    }
    if(line == 0)
    {
      return Coordinate(0, 0);
    }
    line--;
    if(document[line].empty())
    {
      return Coordinate(line, 0);
    }
    idx = document[line].size() - 1;
  }

  const int targetClass = getVimCharClass(document[line][idx].codepoint, bigWord);
  while(idx > 0 && getVimCharClass(document[line][idx - 1].codepoint, bigWord) == targetClass)
  {
    idx--;
  }
  return Coordinate(line, document.getColumn(line, idx));
}

TextEditor::Coordinate TextEditorEx::getVimWordEnd(Coordinate from, bool bigWord) const
{
  from       = document.normalizeCoordinate(from);
  int line   = from.line;
  size_t idx = document.getIndex(from);

  // Advance at least 1 character
  if(idx + 1 < document[line].size())
  {
    idx++;
  }
  else
  {
    while(true)
    {
      if(line + 1 >= document.lineCount())
      {
        return clampVimNormalCoord(Coordinate(line, document[line].maxColumn));
      }
      line++;
      idx = 0;
      if(!document[line].empty())
      {
        break;
      }
    }
  }

  // Skip whitespace forward across lines
  while(true)
  {
    while(idx < document[line].size() && CodePoint::isWhiteSpace(document[line][idx].codepoint))
    {
      idx++;
    }
    if(idx < document[line].size())
    {
      break;
    }
    if(line + 1 >= document.lineCount())
    {
      return clampVimNormalCoord(Coordinate(line, document[line].maxColumn));
    }
    line++;
    idx = 0;
  }

  const int targetClass = getVimCharClass(document[line][idx].codepoint, bigWord);
  while(idx + 1 < document[line].size() && getVimCharClass(document[line][idx + 1].codepoint, bigWord) == targetClass)
  {
    idx++;
  }
  return Coordinate(line, document.getColumn(line, idx));
}

TextEditor::Coordinate TextEditorEx::getVimMatchingBracket(Coordinate from) const
{
  from          = document.normalizeCoordinate(from);
  const auto& l = document[from.line];
  size_t idx    = document.getIndex(from);
  if(l.empty())
  {
    return from;
  }
  if(idx >= l.size())
  {
    idx = l.size() - 1;
  }

  // Find a bracket on or after idx on the current line
  ImWchar ch     = 0;
  bool    opener = false;
  while(idx < l.size())
  {
    const ImWchar cp = l[idx].codepoint;
    if(CodePoint::isBracketOpener(cp))
    {
      ch     = cp;
      opener = true;
      break;
    }
    if(CodePoint::isBracketCloser(cp))
    {
      ch     = cp;
      opener = false;
      break;
    }
    idx++;
  }
  if(ch == 0)
  {
    return from;
  }

  const ImWchar matchCh = opener ? CodePoint::toPairCloser(ch) : CodePoint::toPairOpener(ch);
  int           depth   = 1;
  int           line    = from.line;

  if(opener)
  {
    size_t i = idx + 1;
    while(line < document.lineCount())
    {
      const auto& curLine = document[line];
      while(i < curLine.size())
      {
        if(curLine[i].codepoint == ch)
        {
          depth++;
        }
        else if(curLine[i].codepoint == matchCh)
        {
          depth--;
          if(depth == 0)
          {
            return Coordinate(line, document.getColumn(line, i));
          }
        }
        i++;
      }
      line++;
      i = 0;
    }
  }
  else
  {
    size_t i = idx;
    while(line >= 0)
    {
      const auto& curLine = document[line];
      while(i > 0)
      {
        i--;
        if(curLine[i].codepoint == ch)
        {
          depth++;
        }
        else if(curLine[i].codepoint == matchCh)
        {
          depth--;
          if(depth == 0)
          {
            return Coordinate(line, document.getColumn(line, i));
          }
        }
      }
      line--;
      if(line >= 0)
      {
        i = document[line].size();
      }
    }
  }
  return from;
}

void TextEditorEx::enterVimNormalMode(bool moveLeftOnExitInsert)
{
  const bool wasInsertOrReplace = (m_vimMode == VimMode::eInsert || m_vimMode == VimMode::eReplace);
  m_vimMode                     = VimMode::eNormal;
  overwrite                     = false;
  resetVimPendingState();

  cursors.clearAdditional(false);
  Coordinate cur = cursors.getCurrent().getInteractiveEnd();
  if(moveLeftOnExitInsert && wasInsertOrReplace)
  {
    size_t idx = document.getIndex(cur);
    if(idx > 0)
    {
      cur = Coordinate(cur.line, document.getColumn(cur.line, idx - 1));
    }
  }
  cur = clampVimNormalCoord(cur);
  cursors.setCursor(cur);
  makeCursorVisible();
}

void TextEditorEx::enterVimInsertMode()
{
  m_vimMode = VimMode::eInsert;
  overwrite = false;
  resetVimPendingState();
  cursors.clearAdditional(false);
  Coordinate cur = cursors.getCurrent().getInteractiveEnd();
  cursors.setCursor(cur);
  makeCursorVisible();
}

void TextEditorEx::updateVimVisualSelection()
{
  m_vimVisualAnchor = clampVimNormalCoord(m_vimVisualAnchor);
  m_vimVisualHead   = clampVimNormalCoord(m_vimVisualHead);

  if(m_vimMode == VimMode::eVisualLine)
  {
    const int firstLine = std::min(m_vimVisualAnchor.line, m_vimVisualHead.line);
    const int lastLine  = std::max(m_vimVisualAnchor.line, m_vimVisualHead.line);
    cursors.setCursor(Coordinate(firstLine, 0), Coordinate(lastLine, document[lastLine].maxColumn));
  }
  else if(m_vimMode == VimMode::eVisual)
  {
    if(m_vimVisualHead >= m_vimVisualAnchor)
    {
      cursors.setCursor(m_vimVisualAnchor, getVimCharEndOnLine(m_vimVisualHead));
    }
    else
    {
      cursors.setCursor(getVimCharEndOnLine(m_vimVisualAnchor), m_vimVisualHead);
    }
  }
  makeCursorVisible();
}

void TextEditorEx::yankVimRange(Coordinate start, Coordinate end, bool linewise)
{
  if(end < start)
  {
    std::swap(start, end);
  }
  m_vimLastYankLine = linewise;
  std::string text;
  if(linewise)
  {
    const int firstLine = std::clamp(start.line, 0, document.lineCount() - 1);
    const int lastLine  = std::clamp(end.line, firstLine, document.lineCount() - 1);
    for(int l = firstLine; l <= lastLine; l++)
    {
      if(l > firstLine)
      {
        text += '\n';
      }
      text += document.getLineText(l);
    }
  }
  else
  {
    text = document.getSectionText(start, end);
  }
  ImGui::SetClipboardText(text.c_str());
}

void TextEditorEx::deleteVimRange(Coordinate start, Coordinate end, bool linewise)
{
  if(readOnly)
  {
    return;
  }
  if(end < start)
  {
    std::swap(start, end);
  }
  yankVimRange(start, end, linewise);

  auto transaction = startTransaction();
  if(linewise)
  {
    const int firstLine = std::clamp(start.line, 0, document.lineCount() - 1);
    const int lastLine  = std::clamp(end.line, firstLine, document.lineCount() - 1);

    Coordinate delStart;
    Coordinate delEnd;
    int        targetLine = firstLine;

    if(firstLine == 0 && lastLine == document.lineCount() - 1)
    {
      delStart   = Coordinate(0, 0);
      delEnd     = Coordinate(lastLine, document[lastLine].maxColumn);
      targetLine = 0;
    }
    else if(lastLine < document.lineCount() - 1)
    {
      delStart   = Coordinate(firstLine, 0);
      delEnd     = Coordinate(lastLine + 1, 0);
      targetLine = firstLine;
    }
    else
    {
      delStart   = Coordinate(firstLine - 1, document[firstLine - 1].maxColumn);
      delEnd     = Coordinate(lastLine, document[lastLine].maxColumn);
      targetLine = firstLine - 1;
    }

    deleteText(transaction, delStart, delEnd);
    endTransaction(transaction);

    Coordinate newCur = getVimFirstNonBlank(std::min(targetLine, document.lineCount() - 1));
    cursors.setCursor(newCur);
  }
  else
  {
    deleteText(transaction, start, end);
    endTransaction(transaction);
    cursors.setCursor(clampVimNormalCoord(start));
  }
  makeCursorVisible();
}

void TextEditorEx::pasteVim(bool after)
{
  if(readOnly)
  {
    return;
  }
  const char* clipboard = ImGui::GetClipboardText();
  if(!clipboard || clipboard[0] == '\0')
  {
    return;
  }

  std::string clipText(clipboard);
  // Normalize CRLF
  clipText.erase(std::remove(clipText.begin(), clipText.end(), '\r'), clipText.end());

  if(m_vimMode == VimMode::eVisual || m_vimMode == VimMode::eVisualLine)
  {
    const bool linewise = (m_vimMode == VimMode::eVisualLine);
    Coordinate start    = cursors.getCurrent().getSelectionStart();
    Coordinate end      = cursors.getCurrent().getSelectionEnd();
    if(linewise)
    {
      start = Coordinate( std::min(m_vimVisualAnchor.line, m_vimVisualHead.line), 0 );
      end   = Coordinate( std::max(m_vimVisualAnchor.line, m_vimVisualHead.line),
                          document[std::max(m_vimVisualAnchor.line, m_vimVisualHead.line)].maxColumn );
    }
    auto transaction = startTransaction();
    deleteText(transaction, start, end);
    Coordinate insEnd = insertText(transaction, start, clipText);
    endTransaction(transaction);
    m_vimMode = VimMode::eNormal;
    resetVimPendingState();
    cursors.setCursor(clampVimNormalCoord(insEnd));
    makeCursorVisible();
    return;
  }

  Coordinate cur         = clampVimNormalCoord(cursors.getCurrent().getInteractiveEnd());
  auto       transaction = startTransaction();

  if(m_vimLastYankLine)
  {
    // Strip trailing newline if present so we control exact line insertion
    if(!clipText.empty() && clipText.back() == '\n')
    {
      clipText.pop_back();
    }
    if(after)
    {
      Coordinate insPos = Coordinate(cur.line, document[cur.line].maxColumn);
      insertText(transaction, insPos, "\n" + clipText);
      endTransaction(transaction);
      cursors.setCursor(getVimFirstNonBlank(std::min(cur.line + 1, document.lineCount() - 1)));
    }
    else
    {
      Coordinate insPos = Coordinate(cur.line, 0);
      insertText(transaction, insPos, clipText + "\n");
      endTransaction(transaction);
      cursors.setCursor(getVimFirstNonBlank(cur.line));
    }
  }
  else
  {
    Coordinate insPos = after ? getVimCharEndOnLine(cur) : cur;
    Coordinate insEnd = insertText(transaction, insPos, clipText);
    endTransaction(transaction);
    size_t idx = document.getIndex(insEnd);
    if(idx > 0)
    {
      insEnd = Coordinate(insEnd.line, document.getColumn(insEnd.line, idx - 1));
    }
    cursors.setCursor(clampVimNormalCoord(insEnd));
  }
  makeCursorVisible();
}

bool TextEditorEx::resolveVimTextObject(char modifier, ImWchar obj, Coordinate cur, Coordinate& outStart, Coordinate& outEnd) const
{
  const bool around = (modifier == 'a');
  cur               = clampVimNormalCoord(cur);
  const auto& line  = document[cur.line];

  if(obj == 'w' || obj == 'W')
  {
    if(line.empty())
    {
      outStart = Coordinate(cur.line, 0);
      outEnd   = Coordinate(cur.line, 0);
      return true;
    }
    const bool bigWord = (obj == 'W');
    size_t     idx     = std::min(document.getIndex(cur), line.size() - 1);
    const int  cls     = getVimCharClass(line[idx].codepoint, bigWord);
    size_t     start   = idx;
    size_t     end     = idx + 1;
    while(start > 0 && getVimCharClass(line[start - 1].codepoint, bigWord) == cls)
    {
      start--;
    }
    while(end < line.size() && getVimCharClass(line[end].codepoint, bigWord) == cls)
    {
      end++;
    }
    if(around)
    {
      if(end < line.size() && CodePoint::isWhiteSpace(line[end].codepoint))
      {
        while(end < line.size() && CodePoint::isWhiteSpace(line[end].codepoint))
        {
          end++;
        }
      }
      else
      {
        while(start > 0 && CodePoint::isWhiteSpace(line[start - 1].codepoint))
        {
          start--;
        }
      }
    }
    outStart = Coordinate(cur.line, document.getColumn(cur.line, start));
    outEnd   = Coordinate(cur.line, document.getColumn(cur.line, end));
    return true;
  }

  // Quoted strings on the current line: ", ', `
  if(obj == '"' || obj == '\'' || obj == '`')
  {
    if(line.empty())
    {
      return false;
    }
    size_t curIdx = std::min(document.getIndex(cur), line.size() - 1);
    std::vector<size_t> quotes;
    for(size_t i = 0; i < line.size(); i++)
    {
      if(line[i].codepoint == obj && (i == 0 || line[i - 1].codepoint != '\\'))
      {
        quotes.push_back(i);
      }
    }
    for(size_t k = 0; k + 1 < quotes.size(); k += 2)
    {
      const size_t q1 = quotes[k];
      const size_t q2 = quotes[k + 1];
      if(curIdx <= q2)
      {
        const size_t s = around ? q1 : (q1 + 1);
        const size_t e = around ? (q2 + 1) : q2;
        outStart       = Coordinate(cur.line, document.getColumn(cur.line, s));
        outEnd         = Coordinate(cur.line, document.getColumn(cur.line, e));
        return true;
      }
    }
    return false;
  }

  // Paired brackets: (), {}, [], <>
  ImWchar openCh  = 0;
  ImWchar closeCh = 0;
  if(obj == '(' || obj == ')' || obj == 'b')
  {
    openCh  = '(';
    closeCh = ')';
  }
  else if(obj == '{' || obj == '}' || obj == 'B')
  {
    openCh  = '{';
    closeCh = '}';
  }
  else if(obj == '[' || obj == ']')
  {
    openCh  = '[';
    closeCh = ']';
  }
  else if(obj == '<' || obj == '>')
  {
    openCh  = '<';
    closeCh = '>';
  }

  if(openCh != 0)
  {
    // Search backward for unmatched openCh (including under cursor if it's openCh)
    int    sLine  = cur.line;
    size_t sIdx   = document.getIndex(cur);
    int    depth  = 0;
    bool   foundS = false;

    if(sIdx < document[sLine].size() && document[sLine][sIdx].codepoint == openCh)
    {
      foundS = true;
    }
    else
    {
      int    l = sLine;
      size_t i = sIdx;
      while(l >= 0 && !foundS)
      {
        const auto& ln = document[l];
        while(i > 0)
        {
          i--;
          if(ln[i].codepoint == closeCh)
          {
            depth++;
          }
          else if(ln[i].codepoint == openCh)
          {
            if(depth == 0)
            {
              sLine  = l;
              sIdx   = i;
              foundS = true;
              break;
            }
            depth--;
          }
        }
        l--;
        if(l >= 0)
        {
          i = document[l].size();
        }
      }
    }

    if(!foundS)
    {
      return false;
    }

    // Search forward from (sLine, sIdx + 1) for matching closeCh
    int    eLine  = sLine;
    size_t eIdx   = sIdx + 1;
    depth         = 1;
    bool   foundE = false;
    while(eLine < document.lineCount() && !foundE)
    {
      const auto& ln = document[eLine];
      while(eIdx < ln.size())
      {
        if(ln[eIdx].codepoint == openCh)
        {
          depth++;
        }
        else if(ln[eIdx].codepoint == closeCh)
        {
          depth--;
          if(depth == 0)
          {
            foundE = true;
            break;
          }
        }
        eIdx++;
      }
      if(!foundE)
      {
        eLine++;
        eIdx = 0;
      }
    }

    if(!foundE)
    {
      return false;
    }

    if(around)
    {
      outStart = Coordinate(sLine, document.getColumn(sLine, sIdx));
      outEnd   = Coordinate(eLine, document.getColumn(eLine, eIdx + 1));
    }
    else
    {
      outStart = Coordinate(sLine, document.getColumn(sLine, sIdx + 1));
      outEnd   = Coordinate(eLine, document.getColumn(eLine, eIdx));
    }
    return true;
  }

  return false;
}

bool TextEditorEx::resolveVimMotion(ImWchar motion, ImWchar arg, int count, Coordinate cur,
                                    Coordinate& outTarget, bool& outLinewise, bool& outInclusive) const
{
  const int n  = std::max(1, count);
  cur          = clampVimNormalCoord(cur);
  outLinewise  = false;
  outInclusive = false;
  outTarget    = cur;

  switch(motion)
  {
    case 'h':
    {
      size_t idx = document.getIndex(cur);
      idx        = (idx > static_cast<size_t>(n)) ? (idx - n) : 0;
      outTarget  = Coordinate(cur.line, document.getColumn(cur.line, idx));
      return true;
    }
    case 'l':
    case ' ':
    {
      const auto& ln = document[cur.line];
      if(ln.empty())
      {
        outTarget = Coordinate(cur.line, 0);
      }
      else
      {
        size_t idx = std::min(document.getIndex(cur) + static_cast<size_t>(n), ln.size());
        outTarget  = Coordinate(cur.line, document.getColumn(cur.line, idx));
      }
      return true;
    }
    case 'j':
    {
      const int targetLine = std::min(document.lineCount() - 1, cur.line + n);
      outTarget            = clampVimNormalCoord(Coordinate(targetLine, cur.column));
      outLinewise          = true;
      return true;
    }
    case 'k':
    {
      const int targetLine = std::max(0, cur.line - n);
      outTarget            = clampVimNormalCoord(Coordinate(targetLine, cur.column));
      outLinewise          = true;
      return true;
    }
    case '0':
    {
      outTarget = Coordinate(cur.line, 0);
      return true;
    }
    case '^':
    case '_':
    {
      outTarget = getVimFirstNonBlank(cur.line);
      return true;
    }
    case '$':
    {
      outTarget    = Coordinate(cur.line, document[cur.line].maxColumn);
      outInclusive = true;
      return true;
    }
    case 'w':
    case 'W':
    {
      Coordinate pos = cur;
      for(int i = 0; i < n; i++)
      {
        pos = getVimWordForward(pos, motion == 'W');
      }
      outTarget = pos;
      return true;
    }
    case 'b':
    case 'B':
    {
      Coordinate pos = cur;
      for(int i = 0; i < n; i++)
      {
        pos = getVimWordBackward(pos, motion == 'B');
      }
      outTarget = pos;
      return true;
    }
    case 'e':
    case 'E':
    {
      Coordinate pos = cur;
      for(int i = 0; i < n; i++)
      {
        pos = getVimWordEnd(pos, motion == 'E');
      }
      outTarget    = pos;
      outInclusive = true;
      return true;
    }
    case 'G':
    {
      const int targetLine = (count > 0) ? std::clamp(count - 1, 0, document.lineCount() - 1) : (document.lineCount() - 1);
      outTarget            = getVimFirstNonBlank(targetLine);
      outLinewise          = true;
      return true;
    }
    case '%':
    {
      Coordinate match = getVimMatchingBracket(cur);
      if(match == cur)
      {
        return false;
      }
      outTarget    = match;
      outInclusive = true;
      return true;
    }
    case '{':
    {
      int line = cur.line;
      for(int i = 0; i < n; i++)
      {
        while(line > 0 && document[line].empty())
        {
          line--;
        }
        while(line > 0 && !document[line].empty())
        {
          line--;
        }
      }
      outTarget = Coordinate(line, 0);
      return true;
    }
    case '}':
    {
      int line = cur.line;
      for(int i = 0; i < n; i++)
      {
        while(line + 1 < document.lineCount() && document[line].empty())
        {
          line++;
        }
        while(line + 1 < document.lineCount() && !document[line].empty())
        {
          line++;
        }
      }
      outTarget = Coordinate(line, 0);
      return true;
    }
    case 'f':
    case 't':
    {
      const auto& ln  = document[cur.line];
      size_t      idx = document.getIndex(cur);
      for(int i = 0; i < n; i++)
      {
        size_t search = idx + 1;
        while(search < ln.size() && ln[search].codepoint != arg)
        {
          search++;
        }
        if(search >= ln.size())
        {
          return false;
        }
        idx = search;
      }
      if(motion == 't' && idx > 0)
      {
        idx--;
      }
      outTarget    = Coordinate(cur.line, document.getColumn(cur.line, idx));
      outInclusive = true;
      return true;
    }
    case 'F':
    case 'T':
    {
      const auto& ln  = document[cur.line];
      size_t      idx = document.getIndex(cur);
      for(int i = 0; i < n; i++)
      {
        if(idx == 0)
        {
          return false;
        }
        size_t search = idx - 1;
        while(true)
        {
          if(ln[search].codepoint == arg)
          {
            idx = search;
            break;
          }
          if(search == 0)
          {
            return false;
          }
          search--;
        }
      }
      if(motion == 'T' && idx + 1 < ln.size())
      {
        idx++;
      }
      outTarget = Coordinate(cur.line, document.getColumn(cur.line, idx));
      return true;
    }
    default:
      return false;
  }
}

void TextEditorEx::applyVimOperatorToRange(char op, Coordinate start, Coordinate end, bool linewise)
{
  if(end < start)
  {
    std::swap(start, end);
  }
  switch(op)
  {
    case 'y':
      yankVimRange(start, end, linewise);
      cursors.setCursor(clampVimNormalCoord(start));
      break;
    case 'd':
      deleteVimRange(start, end, linewise);
      break;
    case 'c':
      if(!readOnly)
      {
        if(linewise)
        {
          yankVimRange(start, end, true);
          const int firstLine = std::clamp(start.line, 0, document.lineCount() - 1);
          const int lastLine  = std::clamp(end.line, firstLine, document.lineCount() - 1);
          // Preserve indentation of firstLine
          std::string indent;
          for(const auto& glyph : document[firstLine])
          {
            if(CodePoint::isWhiteSpace(glyph.codepoint))
            {
              char utf8[4];
              indent.append(utf8, CodePoint::write(utf8, glyph.codepoint));
            }
            else
            {
              break;
            }
          }
          auto       transaction = startTransaction();
          Coordinate delStart(firstLine, 0);
          Coordinate delEnd(lastLine, document[lastLine].maxColumn);
          deleteText(transaction, delStart, delEnd);
          Coordinate insEnd = insertText(transaction, delStart, indent);
          endTransaction(transaction);
          cursors.setCursor(insEnd);
        }
        else
        {
          yankVimRange(start, end, false);
          auto transaction = startTransaction();
          deleteText(transaction, start, end);
          endTransaction(transaction);
          cursors.setCursor(start);
        }
        enterVimInsertMode();
      }
      break;
    case '>':
      if(!readOnly)
      {
        SelectLines(start.line, end.line);
        indentLines();
        cursors.setCursor(getVimFirstNonBlank(start.line));
      }
      break;
    case '<':
      if(!readOnly)
      {
        SelectLines(start.line, end.line);
        deindentLines();
        cursors.setCursor(getVimFirstNonBlank(start.line));
      }
      break;
  }
}

void TextEditorEx::handleVimKeyInNormalOrVisual(ImWchar ch)
{
  const bool isVisual = (m_vimMode == VimMode::eVisual || m_vimMode == VimMode::eVisualLine);
  Coordinate cur      = isVisual ? m_vimVisualHead : clampVimNormalCoord(cursors.getCurrent().getInteractiveEnd());

  auto applyMoveOrOp = [&](Coordinate target, bool linewise, bool inclusive) {
    if(m_vimPendingOp != 0)
    {
      const char op = m_vimPendingOp;
      resetVimPendingState();
      Coordinate s = cur;
      Coordinate e = target;
      if(e < s)
      {
        std::swap(s, e);
        if(inclusive)
        {
          e = getVimCharEndOnLine(e);
        }
      }
      else if(inclusive)
      {
        e = getVimCharEndOnLine(e);
      }
      applyVimOperatorToRange(op, s, e, linewise);
    }
    else if(isVisual)
    {
      m_vimVisualHead = clampVimNormalCoord(target);
      updateVimVisualSelection();
      resetVimPendingState();
    }
    else
    {
      resetVimPendingState();
      cursors.setCursor(clampVimNormalCoord(target));
      makeCursorVisible();
    }
  };

  // 1. Handle pending character-argument commands: r<char>, f<char>, F<char>, t<char>, T<char>
  if(m_vimPendingSubOp == 'f' || m_vimPendingSubOp == 'F' || m_vimPendingSubOp == 't' || m_vimPendingSubOp == 'T')
  {
    const char findCmd = m_vimPendingSubOp;
    m_vimPendingSubOp  = 0;
    Coordinate target;
    bool       linewise  = false;
    bool       inclusive = false;
    if(resolveVimMotion(findCmd, ch, m_vimCount, cur, target, linewise, inclusive))
    {
      applyMoveOrOp(target, linewise, inclusive);
    }
    else
    {
      resetVimPendingState();
    }
    return;
  }

  if(m_vimPendingOp == 'r')
  {
    const int n = std::max(1, m_vimCount);
    resetVimPendingState();
    if(!readOnly && ch >= 32)
    {
      const auto& ln  = document[cur.line];
      size_t      idx = document.getIndex(cur);
      if(idx + n <= ln.size())
      {
        char        utf8[4];
        size_t      len = CodePoint::write(utf8, ch);
        std::string replacement;
        for(int i = 0; i < n; i++)
        {
          replacement.append(utf8, len);
        }
        Coordinate endCoord(cur.line, document.getColumn(cur.line, idx + n));
        auto       transaction = startTransaction();
        deleteText(transaction, cur, endCoord);
        insertText(transaction, cur, replacement);
        endTransaction(transaction);
        cursors.setCursor(Coordinate(cur.line, document.getColumn(cur.line, idx + n - 1)));
        makeCursorVisible();
      }
    }
    return;
  }

  // 2. Handle pending text object modifier ('i' or 'a' in operator-pending or Visual mode)
  if(m_vimPendingSubOp == 'i' || m_vimPendingSubOp == 'a')
  {
    const char mod    = m_vimPendingSubOp;
    m_vimPendingSubOp = 0;
    Coordinate s, e;
    if(resolveVimTextObject(mod, ch, cur, s, e))
    {
      if(m_vimPendingOp != 0)
      {
        const char op = m_vimPendingOp;
        resetVimPendingState();
        applyVimOperatorToRange(op, s, e, false);
      }
      else if(isVisual)
      {
        m_vimMode         = VimMode::eVisual;
        m_vimVisualAnchor = s;
        size_t endIdx     = document.getIndex(e);
        if(endIdx > 0)
        {
          e = Coordinate(e.line, document.getColumn(e.line, endIdx - 1));
        }
        m_vimVisualHead = e;
        updateVimVisualSelection();
        resetVimPendingState();
      }
    }
    else
    {
      resetVimPendingState();
    }
    return;
  }

  // 3. Handle 'g' prefix commands: gg, ge, gu, gU, gc / gcc
  if(m_vimPendingSubOp == 'g')
  {
    m_vimPendingSubOp = 0;
    if(ch == 'g')
    {
      const int targetLine = (m_vimCount > 0) ? std::clamp(m_vimCount - 1, 0, document.lineCount() - 1) : 0;
      applyMoveOrOp(getVimFirstNonBlank(targetLine), true, false);
      return;
    }
    if(ch == 'c')
    {
      if(isVisual)
      {
        toggleComments();
        enterVimNormalMode(false);
      }
      else
      {
        m_vimPendingOp = 'g'; // Wait for second 'c' in gcc
      }
      return;
    }
    resetVimPendingState();
    return;
  }

  if(m_vimPendingOp == 'g')
  {
    resetVimPendingState();
    if(ch == 'c' && !readOnly && language)
    {
      cursors.setCursor(cur);
      toggleComments();
    }
    return;
  }

  // 4. Count prefix [1-9] or [0-9] when count > 0
  if((ch >= '1' && ch <= '9') || (ch == '0' && m_vimCount > 0))
  {
    m_vimCount = std::min(100000, m_vimCount * 10 + static_cast<int>(ch - '0'));
    return;
  }

  // 5. Double-operator linewise commands: dd, cc, yy, >>, <<
  if(m_vimPendingOp != 0 && ch == static_cast<ImWchar>(m_vimPendingOp))
  {
    const char op       = m_vimPendingOp;
    const int  n        = std::max(1, m_vimCount);
    const int  lastLine = std::min(document.lineCount() - 1, cur.line + n - 1);
    resetVimPendingState();
    applyVimOperatorToRange(op, Coordinate(cur.line, 0), Coordinate(lastLine, document[lastLine].maxColumn), true);
    return;
  }

  // 6. Text object start ('i' or 'a') when in operator-pending or Visual mode
  if((m_vimPendingOp != 0 || isVisual) && (ch == 'i' || ch == 'a'))
  {
    m_vimPendingSubOp = static_cast<char>(ch);
    return;
  }

  // 7. Find/till character triggers: f, F, t, T
  if(ch == 'f' || ch == 'F' || ch == 't' || ch == 'T')
  {
    m_vimPendingSubOp = static_cast<char>(ch);
    return;
  }

  // 8. 'g' prefix trigger
  if(ch == 'g')
  {
    m_vimPendingSubOp = 'g';
    return;
  }

  // 9. Motions (in Normal, Visual, or Operator-Pending mode)
  {
    Coordinate target;
    bool       linewise  = false;
    bool       inclusive = false;
    if(resolveVimMotion(ch, 0, m_vimCount, cur, target, linewise, inclusive))
    {
      applyMoveOrOp(target, linewise, inclusive);
      return;
    }
  }

  // If an operator was pending and the key wasn't a valid motion/object, cancel
  if(m_vimPendingOp != 0)
  {
    resetVimPendingState();
    return;
  }

  const int n = std::max(1, m_vimCount);

  // 10. Visual / VisualLine mode operations
  if(isVisual)
  {
    Coordinate selStart = cursors.getCurrent().getSelectionStart();
    Coordinate selEnd   = cursors.getCurrent().getSelectionEnd();
    const bool linewise = (m_vimMode == VimMode::eVisualLine);

    switch(ch)
    {
      case 'v':
        if(m_vimMode == VimMode::eVisual)
        {
          enterVimNormalMode(false);
        }
        else
        {
          m_vimMode = VimMode::eVisual;
          updateVimVisualSelection();
        }
        break;
      case 'V':
        if(m_vimMode == VimMode::eVisualLine)
        {
          enterVimNormalMode(false);
        }
        else
        {
          m_vimMode = VimMode::eVisualLine;
          updateVimVisualSelection();
        }
        break;
      case 'o':
        std::swap(m_vimVisualAnchor, m_vimVisualHead);
        updateVimVisualSelection();
        break;
      case 'd':
      case 'x':
        enterVimNormalMode(false);
        applyVimOperatorToRange('d', selStart, selEnd, linewise);
        break;
      case 'c':
      case 's':
        m_vimMode = VimMode::eNormal;
        applyVimOperatorToRange('c', selStart, selEnd, linewise);
        break;
      case 'y':
        enterVimNormalMode(false);
        applyVimOperatorToRange('y', selStart, selEnd, linewise);
        break;
      case 'p':
      case 'P':
        pasteVim(ch == 'p');
        break;
      case '>':
        indentLines();
        enterVimNormalMode(false);
        break;
      case '<':
        deindentLines();
        enterVimNormalMode(false);
        break;
      case 'u':
        selectionToLowerCase();
        enterVimNormalMode(false);
        break;
      case 'U':
        selectionToUpperCase();
        enterVimNormalMode(false);
        break;
      case '~':
        FilterSelections([](std::string_view text) {
          std::string res;
          auto        it  = text.begin();
          auto        end = text.end();
          char        utf8[4];
          while(it < end)
          {
            ImWchar cp;
            it = CodePoint::read(it, end, &cp);
            cp = CodePoint::isLower(cp) ? CodePoint::toUpper(cp) : CodePoint::toLower(cp);
            res.append(utf8, CodePoint::write(utf8, cp));
          }
          return res;
        });
        enterVimNormalMode(false);
        break;
      default:
        break;
    }
    resetVimPendingState();
    return;
  }

  // 11. Normal mode actions
  resetVimPendingState();
  switch(ch)
  {
    case 'i':
      cursors.setCursor(cur);
      enterVimInsertMode();
      break;
    case 'I':
      cursors.setCursor(getVimFirstNonBlank(cur.line));
      enterVimInsertMode();
      break;
    case 'a':
      cursors.setCursor(getVimCharEndOnLine(cur));
      enterVimInsertMode();
      break;
    case 'A':
      cursors.setCursor(Coordinate(cur.line, document[cur.line].maxColumn));
      enterVimInsertMode();
      break;
    case 'o':
      if(!readOnly)
      {
        cursors.setCursor(Coordinate(cur.line, document[cur.line].maxColumn));
        handleCharacter('\n');
        enterVimInsertMode();
      }
      break;
    case 'O':
      if(!readOnly)
      {
        std::string indent;
        if(autoIndent)
        {
          for(const auto& glyph : document[cur.line])
          {
            if(CodePoint::isWhiteSpace(glyph.codepoint))
            {
              char utf8[4];
              indent.append(utf8, CodePoint::write(utf8, glyph.codepoint));
            }
            else
            {
              break;
            }
          }
        }
        auto       transaction = startTransaction();
        Coordinate lineStart(cur.line, 0);
        insertText(transaction, lineStart, indent + "\n");
        endTransaction(transaction);
        cursors.setCursor(Coordinate(cur.line, document.getColumn(cur.line, indent.size())));
        enterVimInsertMode();
      }
      break;
    case 'R':
      if(!readOnly)
      {
        m_vimMode = VimMode::eReplace;
        overwrite = true;
        cursors.setCursor(cur);
      }
      break;
    case 'v':
      m_vimMode         = VimMode::eVisual;
      m_vimVisualAnchor = cur;
      m_vimVisualHead   = cur;
      updateVimVisualSelection();
      break;
    case 'V':
      m_vimMode         = VimMode::eVisualLine;
      m_vimVisualAnchor = cur;
      m_vimVisualHead   = cur;
      updateVimVisualSelection();
      break;
    case 'd':
    case 'c':
    case 'y':
    case '>':
    case '<':
    case 'r':
      m_vimPendingOp = static_cast<char>(ch);
      m_vimCount     = n;
      break;
    case 'D':
      applyVimOperatorToRange('d', cur, Coordinate(cur.line, document[cur.line].maxColumn), false);
      break;
    case 'C':
      applyVimOperatorToRange('c', cur, Coordinate(cur.line, document[cur.line].maxColumn), false);
      break;
    case 'Y':
    {
      const int lastLine = std::min(document.lineCount() - 1, cur.line + n - 1);
      applyVimOperatorToRange('y', Coordinate(cur.line, 0), Coordinate(lastLine, document[lastLine].maxColumn), true);
      break;
    }
    case 's':
      if(!readOnly)
      {
        const auto& ln  = document[cur.line];
        size_t      idx = std::min(document.getIndex(cur) + static_cast<size_t>(n), ln.size());
        Coordinate  end(cur.line, document.getColumn(cur.line, idx));
        applyVimOperatorToRange('c', cur, end, false);
      }
      break;
    case 'S':
      applyVimOperatorToRange('c', Coordinate(cur.line, 0), Coordinate(cur.line, document[cur.line].maxColumn), true);
      break;
    case 'x':
      if(!readOnly && !document[cur.line].empty())
      {
        const auto& ln  = document[cur.line];
        size_t      idx = std::min(document.getIndex(cur) + static_cast<size_t>(n), ln.size());
        Coordinate  end(cur.line, document.getColumn(cur.line, idx));
        deleteVimRange(cur, end, false);
      }
      break;
    case 'X':
      if(!readOnly && !document[cur.line].empty())
      {
        size_t idx = document.getIndex(cur);
        if(idx > 0)
        {
          size_t     startIdx = (idx > static_cast<size_t>(n)) ? (idx - n) : 0;
          Coordinate start(cur.line, document.getColumn(cur.line, startIdx));
          deleteVimRange(start, cur, false);
        }
      }
      break;
    case 'p':
    case 'P':
      for(int i = 0; i < n; i++)
      {
        pasteVim(ch == 'p');
      }
      break;
    case 'u':
      for(int i = 0; i < n; i++)
      {
        undo();
      }
      cursors.setCursor(clampVimNormalCoord(cursors.getCurrent().getInteractiveEnd()));
      break;
    case 'J':
      if(!readOnly && cur.line + 1 < document.lineCount())
      {
        auto       transaction = startTransaction();
        Coordinate endOfCur(cur.line, document[cur.line].maxColumn);
        Coordinate firstOfNext = getVimFirstNonBlank(cur.line + 1);
        if(document[cur.line + 1].empty())
        {
          firstOfNext = Coordinate(cur.line + 1, 0);
        }
        deleteText(transaction, endOfCur, firstOfNext);
        if(!document[cur.line].empty())
        {
          insertText(transaction, endOfCur, " ");
        }
        endTransaction(transaction);
        cursors.setCursor(clampVimNormalCoord(endOfCur));
      }
      break;
    case '~':
      if(!readOnly && !document[cur.line].empty())
      {
        size_t idx = document.getIndex(cur);
        if(idx < document[cur.line].size())
        {
          ImWchar cp = document[cur.line][idx].codepoint;
          cp         = CodePoint::isLower(cp) ? CodePoint::toUpper(cp) : CodePoint::toLower(cp);
          char   utf8[4];
          size_t len = CodePoint::write(utf8, cp);
          Coordinate nextCoord = getVimCharEndOnLine(cur);
          auto       transaction = startTransaction();
          deleteText(transaction, cur, nextCoord);
          insertText(transaction, cur, std::string_view(utf8, len));
          endTransaction(transaction);
          cursors.setCursor(clampVimNormalCoord(nextCoord));
        }
      }
      break;
    case '/':
      openFindReplace();
      break;
    case 'n':
      findNext();
      cursors.setCursor(clampVimNormalCoord(cursors.getCurrent().getSelectionStart()));
      break;
    case 'N':
      if(!findText.empty())
      {
        // Search backward by finding the last match before current cursor
        Coordinate curStart = cursors.getCurrent().getSelectionStart();
        Coordinate bestStart = Coordinate::invalid();
        Coordinate bestEnd   = Coordinate::invalid();
        Coordinate searchFrom(0, 0);
        Coordinate s, e;
        while(document.findText(searchFrom, findText, caseSensitiveFind, wholeWordFind, s, e))
        {
          if(s < curStart)
          {
            bestStart = s;
            bestEnd   = e;
            if(e <= searchFrom)
            {
              break;
            }
            searchFrom = e;
          }
          else
          {
            break;
          }
        }
        if(!bestStart.isValid())
        {
          // Wrap around to the last match in the document
          searchFrom = curStart;
          while(document.findText(searchFrom, findText, caseSensitiveFind, wholeWordFind, s, e))
          {
            if(s >= searchFrom && (!bestStart.isValid() || s > bestStart))
            {
              bestStart  = s;
              bestEnd    = e;
              searchFrom = e;
            }
            else
            {
              break;
            }
          }
        }
        if(bestStart.isValid())
        {
          cursors.setCursor(bestStart);
          makeCursorVisible();
        }
      }
      break;
    case '*':
    {
      Coordinate s, e;
      if(resolveVimTextObject('i', 'w', cur, s, e) && s < e)
      {
        findText = document.getSectionText(s, e);
        findNext();
        cursors.setCursor(clampVimNormalCoord(cursors.getCurrent().getSelectionStart()));
      }
      break;
    }
    default:
      break;
  }
}

void TextEditorEx::handleKeyboardInputs()
{
  if(!m_vimEnabled)
  {
    TextEditor::handleKeyboardInputs();
    return;
  }

  if(!ImGui::IsWindowFocused())
  {
    return;
  }

  // Take ownership of Escape while the editor child window is focused so
  // Dear ImGui's NavUpdateCancelRequest() in NewFrame() does not use Escape
  // to exit the child window and unfocus the editor before we can handle it.
  ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetCurrentWindow()->ID);

  ImGuiIO&   io    = ImGui::GetIO();
  const bool shift = ImGui::IsKeyDown(ImGuiMod_Shift);
  const bool ctrl  = ImGui::IsKeyDown(ImGuiMod_Ctrl);
  const bool alt   = ImGui::IsKeyDown(ImGuiMod_Alt);

  const bool escapePressed =
      ImGui::IsKeyPressed(ImGuiKey_Escape, ImGuiInputFlags_None, ImGuiKeyOwner_Any)
      || (ctrl && !shift && !alt && ImGui::IsKeyPressed(ImGuiKey_LeftBracket));

  // In Insert or Replace mode, Escape returns to Normal mode; all other keys use standard TextEditor logic
  if(m_vimMode == VimMode::eInsert || m_vimMode == VimMode::eReplace)
  {
    if(escapePressed)
    {
      io.InputQueueCharacters.resize(0);
      enterVimNormalMode(true);
      return;
    }
    if(ctrl && !alt)
    {
      io.InputQueueCharacters.resize(0);
    }
    TextEditor::handleKeyboardInputs();
    return;
  }

  // Normal, Visual, or VisualLine mode
  io.WantCaptureKeyboard = true;
  io.WantTextInput       = true;

  if(escapePressed)
  {
    io.InputQueueCharacters.resize(0);
    enterVimNormalMode(false);
    return;
  }

  // Shortcuts with Ctrl
  if(ctrl && !alt)
  {
    if(!shift && ImGui::IsKeyPressed(ImGuiKey_R))
    {
      redo();
      cursors.setCursor(clampVimNormalCoord(cursors.getCurrent().getInteractiveEnd()));
    }
    else if(!shift && ImGui::IsKeyPressed(ImGuiKey_U))
    {
      const int halfPage = std::max(1, visibleLines / 2);
      handleVimKeyInNormalOrVisual(0); // keep state clean
      Coordinate cur    = (m_vimMode == VimMode::eVisual || m_vimMode == VimMode::eVisualLine)
                              ? m_vimVisualHead
                              : cursors.getCurrent().getInteractiveEnd();
      Coordinate target = clampVimNormalCoord(Coordinate(std::max(0, cur.line - halfPage), cur.column));
      if(m_vimMode == VimMode::eVisual || m_vimMode == VimMode::eVisualLine)
      {
        m_vimVisualHead = target;
        updateVimVisualSelection();
      }
      else
      {
        cursors.setCursor(target);
        makeCursorVisible();
      }
    }
    else if(!shift && ImGui::IsKeyPressed(ImGuiKey_D))
    {
      const int halfPage = std::max(1, visibleLines / 2);
      Coordinate cur     = (m_vimMode == VimMode::eVisual || m_vimMode == VimMode::eVisualLine)
                               ? m_vimVisualHead
                               : cursors.getCurrent().getInteractiveEnd();
      Coordinate target  = clampVimNormalCoord(Coordinate(std::min(document.lineCount() - 1, cur.line + halfPage), cur.column));
      if(m_vimMode == VimMode::eVisual || m_vimMode == VimMode::eVisualLine)
      {
        m_vimVisualHead = target;
        updateVimVisualSelection();
      }
      else
      {
        cursors.setCursor(target);
        makeCursorVisible();
      }
    }
    else if(!shift && ImGui::IsKeyPressed(ImGuiKey_F))
    {
      openFindReplace();
    }
    else if(!shift && ImGui::IsKeyPressed(ImGuiKey_C))
    {
      copy();
    }
    else if(!shift && !readOnly && ImGui::IsKeyPressed(ImGuiKey_V))
    {
      paste();
    }
    else if(!shift && !readOnly && ImGui::IsKeyPressed(ImGuiKey_Z))
    {
      undo();
    }
    else if(!readOnly && ((shift && ImGui::IsKeyPressed(ImGuiKey_Z)) || (!shift && ImGui::IsKeyPressed(ImGuiKey_Y))))
    {
      redo();
    }
    io.InputQueueCharacters.resize(0);
    return;
  }

  // Non-printable navigation keys in Normal/Visual mode
  if(!ctrl && !alt)
  {
    if(ImGui::IsKeyPressed(ImGuiKey_UpArrow))
    {
      handleVimKeyInNormalOrVisual('k');
    }
    else if(ImGui::IsKeyPressed(ImGuiKey_DownArrow))
    {
      handleVimKeyInNormalOrVisual('j');
    }
    else if(ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_Backspace))
    {
      handleVimKeyInNormalOrVisual('h');
    }
    else if(ImGui::IsKeyPressed(ImGuiKey_RightArrow))
    {
      handleVimKeyInNormalOrVisual('l');
    }
    else if(ImGui::IsKeyPressed(ImGuiKey_Home))
    {
      handleVimKeyInNormalOrVisual('0');
    }
    else if(ImGui::IsKeyPressed(ImGuiKey_End))
    {
      handleVimKeyInNormalOrVisual('$');
    }
    else if(ImGui::IsKeyPressed(ImGuiKey_Delete))
    {
      handleVimKeyInNormalOrVisual('x');
    }
    else if(ImGui::IsKeyPressed(ImGuiKey_PageUp))
    {
      moveUp( std::max(1, visibleLines - 2), m_vimMode != VimMode::eNormal );
    }
    else if(ImGui::IsKeyPressed(ImGuiKey_PageDown))
    {
      moveDown( std::max(1, visibleLines - 2), m_vimMode != VimMode::eNormal );
    }
    else if(ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
    {
      handleVimKeyInNormalOrVisual('j');
      handleVimKeyInNormalOrVisual('^');
    }
  }

  // Process layout-translated printable characters for Vim commands
  if(!ctrl && !alt && !io.InputQueueCharacters.empty())
  {
    for(int i = 0; i < io.InputQueueCharacters.size(); i++)
    {
      const ImWchar ch = io.InputQueueCharacters[i];
      if(ch >= 32)
      {
        handleVimKeyInNormalOrVisual(ch);
        if(m_vimMode == VimMode::eInsert || m_vimMode == VimMode::eReplace)
        {
          // Remaining characters in this frame (if any) belong to Insert mode
          for(int j = i + 1; j < io.InputQueueCharacters.size(); j++)
          {
            if(io.InputQueueCharacters[j] == '\n' || io.InputQueueCharacters[j] >= 32)
            {
              handleCharacter(io.InputQueueCharacters[j]);
            }
          }
          break;
        }
      }
    }
    io.InputQueueCharacters.resize(0);
  }
}

void TextEditorEx::renderCursors()
{
  if(!m_vimEnabled || m_vimMode == VimMode::eInsert)
  {
    TextEditor::renderCursors();
    return;
  }

  // Draw block cursor (Normal / Visual / VisualLine) or thick underline (Replace)
  cursorAnimationTimer = std::fmod(cursorAnimationTimer + ImGui::GetIO().DeltaTime, 1.0f);
  if(ImGui::IsWindowFocused())
  {
    ImVec2      cursorScreenPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList        = ImGui::GetWindowDrawList();

    Coordinate pos = (m_vimMode == VimMode::eVisual || m_vimMode == VimMode::eVisualLine)
                         ? m_vimVisualHead
                         : cursors.getCurrent().getInteractiveEnd();
    pos            = clampVimNormalCoord(pos);

    if(pos.line >= firstVisibleLine && pos.line <= lastVisibleLine)
    {
      const float x = cursorScreenPos.x + textOffset + pos.column * glyphSize.x;
      const float y = cursorScreenPos.y + pos.line * glyphSize.y;

      ImVec4 cursorCol = ImGui::ColorConvertU32ToFloat4(palette.get(Color::cursor));
      if(m_vimMode == VimMode::eReplace)
      {
        cursorCol.w *= 0.85f;
        const float h = std::max(2.0f, glyphSize.y * 0.2f);
        drawList->AddRectFilled(ImVec2(x, y + glyphSize.y - h), ImVec2(x + glyphSize.x, y + glyphSize.y),
                                ImGui::ColorConvertFloat4ToU32(cursorCol));
      }
      else
      {
        cursorCol.w *= 0.55f;
        drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + glyphSize.x, y + glyphSize.y),
                                ImGui::ColorConvertFloat4ToU32(cursorCol));
      }
    }
  }
}

//-----------------------------------------------------------------------------
// LANGUAGE SERVER

#ifdef _WIN32
std::wstring quoteWindowsArgument(const std::span<const wchar_t> input)
{
  // This is susprisingly complex due to the odd logic Windows has for
  // backslashes!
  // `a\b` becomes `a\b`, but `a\"b` becomes `a\\\b`.
  // See https://learn.microsoft.com/en-us/cpp/cpp/main-function-command-line-args?view=msvc-170&redirectedfrom=MSDN#parsing-c-command-line-arguments .
  std::wstring result = L"\"";

  uint32_t backslashCount  = 0;
  auto     emitBackslashes = [&](bool doubleEscape) {
    const uint32_t count = backslashCount * (doubleEscape ? 2 : 1);
    if(count != 0)
    {
      result += std::wstring(count, L'\\');
    }
    backslashCount = 0;
  };

  for(const wchar_t wc : input)
  {
    if(wc == L'\"')
    {
      // " -> \"; \" -> \\\", \\" -> \\\\\", etc.
      emitBackslashes(true);
      result += L"\\\"";
    }
    else if(wc == L'\\')
    {
      backslashCount++;  // Don't emit it yet
    }
    else
    {
      // Pop off regular backslashes
      emitBackslashes(false);
      result += wc;
    }
  }

  // Any remaining backslashes (not used in this sample, but just so this
  // function does what it says it does in general).
  // Doubled, then followed by a regular quote.
  emitBackslashes(true);
  result += L"\"";
  return result;
}

void closeHandle(HANDLE& h)
{
  if(h != NULL)
  {
    CloseHandle(h);
    h = NULL;
  }
}
#else
void closeHandle(int& h)
{
  if(h != -1)
  {
    close(h);
    h = -1;
  }
}
#endif

#ifdef RECORD_SLANGD
struct RecordMessage
{
  bool        received = false;  // false == sent
  double      when     = 0.0;    // In seconds since server start
  std::string message;
};
void to_json(nlohmann::json& j, const RecordMessage& m)
{
  j = nlohmann::json{{"received", m.received}, {"when", m.when}, {"message", m.message}};
}
#endif

// A process that can automatically restart itself.
struct RestartableSubprocess
{
  static constexpr float kDefaultRestartInterval = 10.0f;

private:
#ifdef _WIN32
  PROCESS_INFORMATION m_slangdInfo          = {};
  HANDLE              m_slangd_stdin_read   = NULL;
  HANDLE              m_slangd_stdin_write  = NULL;
  HANDLE              m_slangd_stdout_read  = NULL;
  HANDLE              m_slangd_stdout_write = NULL;
#else
  pid_t m_slangdPid           = -1;
  int   m_slangd_stdin_read   = -1;
  int   m_slangd_stdin_write  = -1;
  int   m_slangd_stdout_read  = -1;
  int   m_slangd_stdout_write = -1;
#endif

  std::filesystem::path m_path;
  using clock                           = std::chrono::system_clock;
  clock::time_point m_lastShutdownTime  = clock::time_point::min();
  float             m_restartInterval   = kDefaultRestartInterval;
  bool              m_permanentlyFailed = false;

#ifdef RECORD_SLANGD
  clock::time_point m_recordStartTime = clock::time_point::min();

  std::vector<RecordMessage> m_recordMessages;
  void                       initRecording()
  {
    m_recordStartTime = std::chrono::system_clock::now();
    m_recordMessages.clear();
  }
  void saveRecording()
  {
    const nlohmann::json json         = m_recordMessages;
    const std::string    fileContents = json.dump(2);
    const std::string    filename     = "slangd-crash-" + pathSafeTimeString() + ".json";
    std::ofstream        crashFile(filename);
    crashFile.write(fileContents.c_str(), fileContents.size());
    crashFile.close();
    LOGI("Wrote slangd recording to %s\n", filename.c_str());
  }
#endif

  bool tooSoonToRestart() const
  {
    const auto now = clock::now();
    // Note the form here -- if we instead did (now - m_lastShutdownTime) we'd
    // run into internal overflow issues.
    const auto minRestart = m_lastShutdownTime + std::chrono::duration<double>(m_restartInterval);
    return (now < minRestart);
  }

  // Creates the slangd subprocess.
  // If already created, does nothing and returns true.
  // On failure (including if trying to restart it too soon), returns false.
  // On permanent failure, logs, sets m_permanentlyFailed, and returns false.
  [[nodiscard]] bool getSubprocess()
  {
    if(m_permanentlyFailed)
    {
      return false;
    }
#ifdef _WIN32
    if(m_slangdInfo.hProcess)
    {
      return true;  // Running normally
    }
#else
    if(m_slangdPid != -1)
    {
      return true;
    }
#endif

    if(tooSoonToRestart())
    {
      LOGI(
          "Too soon to restart slangd: less than %f seconds have passed since it exited. Rate-limiting so we avoid "
          "rapid server restarts, just in case.\n",
          m_restartInterval);
      return false;
    }

    // We need to create the subprocess and its handles.
    // For brevity, mark m_permanentlyFailed until we reach the success state.
    m_permanentlyFailed = true;
    if(m_path.empty())
    {
      LOGW("Could not find slangd. Language server will not run.\n");
      return false;
    }

#ifdef _WIN32
    // Reference: https://learn.microsoft.com/en-us/windows/win32/procthread/creating-a-child-process-with-redirected-input-and-output
    SECURITY_ATTRIBUTES securityAttributes{
        .nLength        = sizeof(SECURITY_ATTRIBUTES),  //
        .bInheritHandle = TRUE,                         // so pipe handles are inherited
    };

    // In addition to the stdout routing to our app and stdin routing into slangd,
    // we also need to create slangd's stdin input and stdout output .
    if(!CreatePipe(&m_slangd_stdin_read, &m_slangd_stdin_write, &securityAttributes, 0))
    {
      LOGW("Could not create stdin pipes for slangd; GetLastError == %lu. Language server will not run.\n", GetLastError());
      return false;
    }
    // And make sure our write handle for stdin isn't inherited:
    if(!SetHandleInformation(m_slangd_stdin_write, HANDLE_FLAG_INHERIT, 0))
    {
      LOGW(
          "Could not mark slangd stdin write handle as uninheritable; GetLastError == %lu. Language server will "
          "not run.\n",
          GetLastError());
      return false;
    }
    // Similarly for stdout:
    if(!CreatePipe(&m_slangd_stdout_read, &m_slangd_stdout_write, &securityAttributes, 0))
    {
      LOGW("Could not create stdin pipes for slangd; GetLastError == %lu. Language server will not run.\n", GetLastError());
      return false;
    }
    if(!SetHandleInformation(m_slangd_stdout_read, HANDLE_FLAG_INHERIT, 0))
    {
      LOGW(
          "Could not mark slangd stdin write handle as uninheritable; GetLastError == %lu. Language server will "
          "not run.\n",
          GetLastError());
      return false;
    }

    // Create the slangd subprocess.
    STARTUPINFOW startupInfo{
        .cb         = sizeof(STARTUPINFOW),
        .dwFlags    = STARTF_USESTDHANDLES,
        .hStdInput  = m_slangd_stdin_read,
        .hStdOutput = m_slangd_stdout_write,
        .hStdError  = m_slangd_stdout_write,
    };

    // Quote the path and add arguments
    std::wstring commandLine = quoteWindowsArgument(m_path.native());
    commandLine += L" --stdio";
    if(!CreateProcessW(NULL,                // lpApplicationName
                       commandLine.data(),  // lpCommandLine
                       NULL,                // Process security attributes
                       NULL,                // Primary thread security attributes
                       TRUE,                // Handles are inherited
                       0,                   // Creation flags
                       NULL,                // Use parent's environment variables
                       NULL,                // Use parent's current working directory
                       &startupInfo,        // Startup info
                       &m_slangdInfo        // Output process info
                       ))
    {
      LOGW("CreateProcessW failed; GetLastError == %lu. Language server will not run.\n", GetLastError());
      return false;
    }

    // Close handles to the stdin and stdout pipes no longer needed by the child process.
    // If they are not explicitly closed, there is no way to recognize that the child process has ended.
    closeHandle(m_slangd_stdin_read);
    closeHandle(m_slangd_stdout_write);

#else
    // Mark slangd as executable
    {
      struct stat exeProps;
      if(stat(m_path.c_str(), &exeProps) == -1)
      {
        LOGW("Calling stat() on the slangd executable at `%s` failed with errno %d. Language server will not run.\n",
             m_path.c_str(), errno);
        return false;
      }

      // Figure out which permissions the user needs to execute the file
      const uid_t uid              = getuid();
      mode_t      requiredModeFlag = S_IXOTH;
      if(exeProps.st_uid == uid)
      {
        requiredModeFlag = S_IXUSR;
      }
      else
      {
        struct passwd* pw = getpwuid(uid);
        if(pw)
        {
          int ngroups = 0;
          getgrouplist(pw->pw_name, pw->pw_gid, nullptr, &ngroups);
          if(ngroups >= 0)
          {
            std::vector<gid_t> groups(static_cast<size_t>(ngroups));
            // In case the number of groups somehow changes in between these calls:
            int newNgroups = ngroups;
            getgrouplist(pw->pw_name, pw->pw_gid, groups.data(), &newNgroups);
            for(int i = 0; i < std::min(ngroups, newNgroups); i++)
            {
              if(groups[i] == exeProps.st_gid)
              {
                requiredModeFlag = S_IXGRP;
              }
            }
          }
        }
      }

      if((exeProps.st_mode & requiredModeFlag) == 0)
      {
        LOGI("Attempting to set permissions flag %u on %s.\n", static_cast<unsigned>(requiredModeFlag), m_path.c_str());
        if(chmod(m_path.c_str(), exeProps.st_mode | requiredModeFlag) == 0)
        {
          LOGOK("Successfully set permissions flag.\n");
        }
        else
        {
          LOGW(
              "Calling chmod() on the slangd executable at `%s` failed with errno %d. The language server will not "
              "run, because executable permissions have not been set. To fix this, please run `chmod u+x slangd` and "
              "fix any errors that occur.\n",
              m_path.c_str(), errno);
          return false;
        }
      }
    }

    // Create pipes for stdin and stdout
    {
      int stdin_pipe[2];
      if(pipe(stdin_pipe) != 0)
      {
        LOGW(
            "Could not create stdin pipe for slangd; errno == %d. Language "
            "server will not run.\n",
            errno);
        return false;
      }
      m_slangd_stdin_read  = stdin_pipe[0];
      m_slangd_stdin_write = stdin_pipe[1];
    }

    {
      int stdout_pipe[2];
      if(pipe(stdout_pipe) != 0)
      {
        LOGW(
            "Could not create stdout pipe for slangd; errno == %d. Language "
            "server will not run.\n",
            errno);
        return false;
      }
      m_slangd_stdout_read  = stdout_pipe[0];
      m_slangd_stdout_write = stdout_pipe[1];
    }

    // Reset SIGPIPE to the default so that the fork doesn't inherit the ignore
    // instruction below if this is a restart
    signal(SIGPIPE, SIG_DFL);
    // Fork the process
    m_slangdPid = fork();
    if(m_slangdPid == -1)
    {
      LOGW("fork() failed; errno == %d. Language server will not run.\n", errno);
      return false;
    }

    if(m_slangdPid == 0)
    {
      // Child process
      // Redirect our stdin and stdout to the pipes given to us
      dup2(m_slangd_stdin_read, STDIN_FILENO);
      dup2(m_slangd_stdout_write, STDOUT_FILENO);

      // Then close all 4 handles so we don't keep around references to them
      closeHandle(m_slangd_stdin_read);
      closeHandle(m_slangd_stdin_write);
      closeHandle(m_slangd_stdout_read);
      closeHandle(m_slangd_stdout_write);

      // Replace our currently running process with slangd
      unsetenv("LD_LIBRARY_PATH");  // Workaround for Vulkan SDK conflict
      execl(m_path.c_str(), m_path.c_str(), "--stdio", nullptr);

      // If we get here, execl failed. Since we're the child process, we should exit.
      // Note that it is *not* wise to print to stdout here; that will go to
      // the parent process.
      const int err = errno;
      std::cerr << "execl failed; errno == " << err << ".Language server will not run.\n ";
      exit(EXIT_FAILURE);
    }
    else
    {
      // Parent process
      // Close the ends we don't need
      closeHandle(m_slangd_stdin_read);
      closeHandle(m_slangd_stdout_write);

      // Don't crash if the child process closes all its handles
      signal(SIGPIPE, SIG_IGN);
    }
#endif

#ifdef RECORD_SLANGD
    initRecording();
#endif

    m_permanentlyFailed = false;
    return true;
  }

  // Shuts down the slangd subprocess.
  void shutdownSubprocess(bool crash = true)
  {
#ifdef RECORD_SLANGD
    if(crash)
    {
      saveRecording();
    }
#endif

    closeHandle(m_slangd_stdin_read);
    closeHandle(m_slangd_stdin_write);
    closeHandle(m_slangd_stdout_read);
    closeHandle(m_slangd_stdout_write);

#ifdef _WIN32
    if(m_slangdInfo.hProcess)
    {
      TerminateProcess(m_slangdInfo.hProcess, 0);
    }

    closeHandle(m_slangdInfo.hThread);
    closeHandle(m_slangdInfo.hProcess);
    m_slangdInfo = {};
#else
    if(m_slangdPid > 0)
    {
      kill(m_slangdPid, SIGTERM);
      m_slangdPid = -1;
    }
#endif

    m_lastShutdownTime = clock::now();
  }

public:
  // Try to write to slangd's stdin. Returns true on success.
  [[nodiscard]] bool sendStringData(const std::string& data)
  {
    if(!getSubprocess())
    {
      shutdownSubprocess();
      return false;
    }

#ifdef SERVER_VERBOSE
    LOGI("Sending message to slangd:\n%s\n\n", data.c_str());
#endif
#ifdef RECORD_SLANGD
    m_recordMessages.push_back(RecordMessage{
        .received = false,
        .when     = std::chrono::duration_cast<std::chrono::duration<double>>(clock::now() - m_recordStartTime).count(),
        .message  = data});
#endif

#ifdef _WIN32
    DWORD bytesWritten = 0;
    if(!WriteFile(m_slangd_stdin_write, data.c_str(), static_cast<DWORD>(data.size()), &bytesWritten, NULL))
    {
      LOGW("Sending data to slangd failed! GetLastError == %lu. Shutting down slangd.\n", GetLastError());
      shutdownSubprocess();
      return false;
    }
#else
    ssize_t bytesWritten = write(m_slangd_stdin_write, data.c_str(), data.size());
    if(bytesWritten == -1)
    {
      LOGW("Sending data to slangd failed! errno == %d. Shutting down slangd.\n", errno);
      shutdownSubprocess();
      return false;
    }
#endif

    if(static_cast<size_t>(bytesWritten) != data.size())
    {
      LOGW("Partial write to slangd: wrote %zu of %zu bytes. Shutting down slangd.\n",
           static_cast<size_t>(bytesWritten), data.size());
      shutdownSubprocess();
      return false;
    }

    return true;
  }

  // Try to read from slangd's output. Returns a string (possibly empty if it
  // has nothing to report) on success, logs and returns no value on
  // system failure.
  [[nodiscard]] std::optional<std::string> receiveStringData()
  {
    if(!getSubprocess())
    {
      shutdownSubprocess();
      return {};
    }

    std::string result;

#ifdef _WIN32
    // See if there's new data to read, and if so, a minimum on how much
    DWORD bytesAvailable = 0;
    if(!PeekNamedPipe(m_slangd_stdout_read, NULL, 0, NULL, &bytesAvailable, NULL))
    {
      LOGW("PeekNamedPipe failed! GetLastError == %lu. Shutting down slangd.\n", GetLastError());
      shutdownSubprocess();
      return {};
    }

    if(bytesAvailable == 0)
    {
      return {""};
    }

    result.resize(bytesAvailable);
    DWORD bytesRead = 0;
    if(!ReadFile(m_slangd_stdout_read, result.data(), bytesAvailable, &bytesRead, NULL))
    {
      LOGW("Reading data from slangd failed! GetLastError == %lu. Shutting down slangd.\n", GetLastError());
      shutdownSubprocess();
      return {};
    }

    if(bytesRead != bytesAvailable)
    {
      LOGE(
          "bytesRead (%lu) wasn't the same as the number of bytes Windows said were available (%lu). This should never "
          "happen.\n",
          bytesRead, bytesAvailable);
      shutdownSubprocess();
      return {};
    }
#else
    // See if there's new data to read, and if so, a minimum on how much
    int bytesAvailable = 0;
    if(ioctl(m_slangd_stdout_read, FIONREAD, &bytesAvailable) < 0)
    {
      LOGW("ioctl(..., FIONREAD, ...) failed! errno == %d. Shutting down slangd.\n", errno);
      shutdownSubprocess();
      return {};
    }

    if(bytesAvailable == 0)
    {
      return {""};  // No data available
    }

    result.resize(bytesAvailable);
    ssize_t bytesRead = read(m_slangd_stdout_read, result.data(), result.size());
    if(bytesRead == -1)
    {
      LOGW("Reading data from slangd failed! errno == %d. Shutting down slangd.\n", errno);
      shutdownSubprocess();
      return {};
    }

    if(bytesRead != bytesAvailable)
    {
      LOGE(
          "bytesRead (%zd) wasn't the same as the number of bytes ioctl said were available (%d). This should never "
          "happen.\n",
          bytesRead, bytesAvailable);
      shutdownSubprocess();
      return {};
    }
#endif

#ifdef SERVER_VERBOSE
    LOGI("Received message from slangd:\n%s\n\n", result.c_str());
#endif
#ifdef RECORD_SLANGD
    m_recordMessages.push_back(RecordMessage{
        .received = true,
        .when     = std::chrono::duration_cast<std::chrono::duration<double>>(clock::now() - m_recordStartTime).count(),
        .message  = result});
#endif

    return {result};
  }

  void init(std::filesystem::path&& path) { m_path = std::forward<std::filesystem::path>(path); }
  void deinit()
  {
    shutdownSubprocess(false);
    m_lastShutdownTime = {};
  }
  bool isRunning() const
  {
#ifdef _WIN32
    return !!m_slangdInfo.hProcess;
#else
    return m_slangdPid != -1;
#endif
  }
  bool canTryStart() const { return !m_permanentlyFailed && !tooSoonToRestart(); }
  void setRestartInterval(float interval) { m_restartInterval = interval; }
};

// Iterates over the UTF-8 codepoints of a line.
// Computes both a position in columns, and a position in UTF-16 codepoints.
// This might be buggy; I wrote it pretty quickly.
struct Utf8Iterator
{
  // We could implement these if we want to be a proper C++ iterator,
  // but we don't need that right now.
  /*
  using iterator_category = std::bidirectional_iterator_tag;
  using difference_type   = std::ptrdiff_t;
  using value_type        = char; 
  using pointer           = char*;
  using reference         = char&;
  */

  Utf8Iterator() {}
  // Constructs an iterator at the start of a string.
  Utf8Iterator(const char* data, size_t size, size_t tabSize)
  {
    m_data    = reinterpret_cast<const uint8_t*>(data);
    m_size    = size;
    m_tabSize = tabSize;
  }
  Utf8Iterator(const std::string_view& data, size_t tabSize)
  {
    m_data    = reinterpret_cast<const uint8_t*>(data.data());
    m_size    = data.size();
    m_tabSize = tabSize;
  }

  // Basic getters
  size_t byteIndex() const { return m_bytes; }
  size_t utf8Index() const { return m_utf8Codepoints; }
  size_t utf16Index() const { return m_utf16CodeUnits; }
  size_t columnIndex() const { return m_column; }

  // Becomes false if we run into invalid UTF-8. Still valid if we're in
  // the string or 1 past the end.
  operator bool() const { return m_data != nullptr; }

#if 0  // Not needed
  // Returns true if we are past the last character in the data buffer.
  bool isAtEnd() const { return m_bytes >= m_size; }
  static Utf8Iterator makeAtEnd(const std::string_view& data, size_t tabSize)
  {
    Utf8Iterator result(data, tabSize);
    while(!result.isAtEnd())
    {
      ++result;
    }
    return result;
  }
#endif

  // Prefix increment
  Utf8Iterator& operator++()
  {
    if(m_data)
    {
      const size_t   cbBytes   = codepointBytes();
      const uint32_t codepoint = decode();
      if(codepoint == IM_UNICODE_CODEPOINT_INVALID)
      {
        m_data = nullptr;
      }
      else
      {
        // Note that we might be 1 past the end now; that's fine
        m_bytes += cbBytes;
        m_utf8Codepoints += 1;
        m_utf16CodeUnits += countUtf16Codepoints(codepoint);
        m_column += (codepoint == '\t' ? m_tabSize : 1);
      }
    }
    return *this;
  }

  // Prefix decrement
  Utf8Iterator& operator--()
  {
    if(m_data)
    {
      // Go backwards until we reach a 1-byte or non-continuation codepoint
      // (or go too far, in which case invalidate the iterator)
      while(true)
      {
        if(m_bytes == 0)
        {
          m_data = nullptr;
          return *this;
        }
        m_bytes--;
        const uint8_t b = m_data[m_bytes];
        // First case is 1-byte codepoint; second case is non-continuation
        if((b & 0b1000'0000) == 0 || (b & 0b1100'0000) == 0b1100'0000)
        {
          break;
        }
      }
      // Count how many UTF-16 codepoints and columns this counts as
      assert(m_utf8Codepoints != 0);
      assert(m_utf16CodeUnits != 0);
      assert(m_column != 0);
      const uint32_t codepoint = decode();
      m_utf8Codepoints -= 1;
      m_utf16CodeUnits -= countUtf16Codepoints(codepoint);
      m_column -= (codepoint == '\t' ? m_tabSize : 1);
    }
    return *this;
  }

  // Reads the codepoint at the current position as UTF-32.
  // Returns IM_UNICODE_CODEPOINT_INVALID on failure.
  uint32_t decode() const
  {
    const size_t numBytes = codepointBytes();
    if(m_bytes + numBytes > m_size)
    {
      return IM_UNICODE_CODEPOINT_INVALID;
    }

    if(numBytes == 1)
    {
      return m_data[m_bytes];
    }
    else if(numBytes == 2)
    {
      return ((m_data[m_bytes] & 0b0001'1111u) << 6)  //
             | (m_data[m_bytes + 1] & 0b0011'1111u);
    }
    else if(numBytes == 3)
    {
      return ((m_data[m_bytes] & 0b0000'1111u) << 12)       //
             | ((m_data[m_bytes + 1] & 0b0011'1111u) << 6)  //
             | (m_data[m_bytes + 2] & 0b0011'1111u);
    }
    else if(numBytes == 4)
    {
      return ((m_data[m_bytes] & 0b0000'0111u) << 18)        //
             | ((m_data[m_bytes + 1] & 0b0011'1111u) << 12)  //
             | ((m_data[m_bytes + 2] & 0b0011'1111u) << 6)   //
             | (m_data[m_bytes + 3] & 0b0011'1111u);
    }
    return IM_UNICODE_CODEPOINT_INVALID;
  }

private:
  // Returns the number of bytes the codepoint at the current position uses.
  size_t codepointBytes() const
  {
    const uint8_t c = m_data[m_bytes];
    if((c & 0b1000'0000u) == 0)
    {
      return 1;
    }
    else if((c & 0b1110'0000u) == 0b1100'0000u)
    {
      return 2;
    }
    else if((c & 0b1111'0000u) == 0b1110'0000u)
    {
      return 3;
    }
    else
    {
      return 4;  // Note: Includes impossible 0b1111'1... bytes
    }
  }

  size_t countUtf16Codepoints(uint32_t utf32)
  {
    if(utf32 < 65536u)
    {
      return 1;
    }
    else
    {
      return 2;
    }
  }

  const uint8_t* m_data    = nullptr;
  size_t         m_size    = 0;
  size_t         m_tabSize = 0;

  size_t m_bytes          = 0;
  size_t m_utf16CodeUnits = 0;
  size_t m_utf8Codepoints = 0;
  size_t m_column         = 0;
};

// Return a Position as JSON corresponding to
// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#position.
// Lines and characters are 0-based.
static nlohmann::json makePositionJson(size_t line, size_t utf16CodeUnits)
{
  return {{"line", line}, {"character", utf16CodeUnits}};
}

static bool isNewline(char c)
{
  return (c == '\r') || (c == '\n');
}

constexpr size_t kCharTableSize = 128;
template <class Codepoint>
static bool testCodepoint(std::array<bool, kCharTableSize>& table, Codepoint c, bool ifOutsideTable)
{
  if(c >= table.size())
  {
    return ifOutsideTable;
  }
  return table[c];
}

struct SignatureInformation
{
  std::string                label;
  std::optional<std::string> documentation;
  // We don't handle parameters yet
  // TODO: Format Markdown documentation
};
// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#completionItemKind
enum class CompletionItemKind
{
  eText          = 1,
  eMethod        = 2,
  eFunction      = 3,
  eConstructor   = 4,
  eField         = 5,
  eVariable      = 6,
  eClass         = 7,
  eInterface     = 8,
  eModule        = 9,
  eProperty      = 10,
  eUnit          = 11,
  eValue         = 12,
  eEnum          = 13,
  eKeyword       = 14,
  eSnippet       = 15,
  eColor         = 16,
  eFile          = 17,
  eReference     = 18,
  eFolder        = 19,
  eEnumMember    = 20,
  eConstant      = 21,
  eStruct        = 22,
  eEvent         = 23,
  eOperator      = 24,
  eTypeParameter = 25
};
const char* completionItemKindToString(CompletionItemKind kind)
{
  switch(kind)
  {
    case CompletionItemKind::eText:
      return "text";
    case CompletionItemKind::eMethod:
      return "method";
    case CompletionItemKind::eFunction:
      return "function";
    case CompletionItemKind::eConstructor:
      return "constructor";
    case CompletionItemKind::eField:
      return "field";
    case CompletionItemKind::eVariable:
      return "variable";
    case CompletionItemKind::eClass:
      return "class";
    case CompletionItemKind::eInterface:
      return "interface";
    case CompletionItemKind::eModule:
      return "module";
    case CompletionItemKind::eProperty:
      return "property";
    case CompletionItemKind::eUnit:
      return "unit";
    case CompletionItemKind::eValue:
      return "value";
    case CompletionItemKind::eEnum:
      return "enum";
    case CompletionItemKind::eKeyword:
      return "keyword";
    case CompletionItemKind::eSnippet:
      return "snippet";
    case CompletionItemKind::eColor:
      return "color";
    case CompletionItemKind::eFile:
      return "file";
    case CompletionItemKind::eReference:
      return "reference";
    case CompletionItemKind::eFolder:
      return "folder";
    case CompletionItemKind::eEnumMember:
      return "enum member";
    case CompletionItemKind::eConstant:
      return "constant";
    case CompletionItemKind::eStruct:
      return "struct";
    case CompletionItemKind::eEvent:
      return "event";
    case CompletionItemKind::eOperator:
      return "operator";
    case CompletionItemKind::eTypeParameter:
      return "type parameter";
    default:
      return "<unknown>";
  }
}
struct CompletionItem
{
  std::string        label;
  CompletionItemKind kind = CompletionItemKind::eText;
};

struct LanguageServer::Implementation
{
private:
  // Objects:
  TextEditorEx*         m_editor = nullptr;
  RestartableSubprocess m_slangd;

  // Communication state
  struct CommunicationState
  {
    // Just for stacking up messages until `initialize` is responded to
    std::queue<nlohmann::json>                  pendingSends;
    std::unordered_map<int32_t, nlohmann::json> pendingRequests;
    int32_t                                     nextMessageId         = 0;
    bool                                        initializeRespondedTo = false;
    bool                                        permanentlyFailed     = false;
    std::deque<char>                            receivedText;
  } m_ioState;

  // Mental model of the document between the client and server
  struct DocumentState
  {
  private:
    std::filesystem::path m_path;
    std::string           m_uri;

  public:
    std::vector<std::string> lines;
    int32_t                  version            = 0;  // Monotonically increases
    size_t                   lastFrameUndoIndex = 0;  // Used to detect when we need to re-scan
    bool                     currentlyOpen      = false;

    DocumentState();
    void                         setPath(const std::filesystem::path& path);
    const std::filesystem::path& getPath() const { return m_path; }
    const std::string&           getUri() const { return m_uri; }
  } m_documentState;

  // Info received back from the language server
  enum class SyncKind
  {
    eNone,
    eFull,
    eIncremental
  };
  struct ServerInfo
  {
    // For wide characters >= 128, we default to true or false when looking things up
    std::array<bool, kCharTableSize> completionTriggers{};  // start completion
    std::array<bool, kCharTableSize> completionEnds{};      // end completion
    std::array<bool, kCharTableSize> identifierCharacters{};
    std::array<bool, kCharTableSize> signatureHelpTriggers{};  // start signature help
    std::array<bool, kCharTableSize> signatureHelpEnds{};      // end signature help

    SyncKind syncKind         = SyncKind::eNone;
    bool     canFormat        = false;  // TODO
    bool     canFormatRange   = false;  // TODO
    bool     canTokenizeFull  = false;
    bool     canTokenizeRange = false;
  } m_serverInfo;

  struct CompletionActive
  {
    static constexpr int32_t kDisplayMax = 10;
    int32_t                  index       = 0;
    int32_t                  uiScroll    = 0;
  };

  struct UiState
  {
    // Copied from ImGui::GetIO().InputQueueCharacters, as TextEditor clears this
    ImVector<ImWchar> inputQueueCharacters;

    std::vector<CompletionItem>     completions;
    std::optional<CompletionActive> completionActive;

    std::vector<SignatureInformation> signatures;
    std::optional<uint32_t>           signatureActive;

    bool editorWasFocused      = false;
    bool wasShowingCompletions = false;
    bool wasShowingSignatures  = false;
  } m_uiState;

  // Settings
  LanguageServer::Settings m_settings{.restartInterval = 10.0f, .enabled = true};

  void resetState()
  {
    m_ioState       = {};
    m_documentState = {};
    m_serverInfo    = {};
    m_uiState       = {};
  }

  // Processes the queue of JSON-RPC requests to slangd.
  // `initialize` requests are sent immediately. Other requests wait until
  // slangd replies it was initialized.
  // On error, returns false and clears pending requests; the server crashed.
  bool processSendQueue();

  // Queues a JSON-RPC message to slangd.
  // You only need to fill out `method` and `params` (and `id` if not a
  // notification); these functions will set `jsonrpc` and `Content-Length`
  // for you.
  void sendJsonRpc(nlohmann::json& request)
  {
    request["jsonrpc"] = "2.0";
    m_ioState.pendingSends.push(request);
  }

  // Cancels previously created requests for a particular method.
  // That way if requests pile up (e.g. autocompletion), slangd doesn't spend
  // time going through them one by one.
  // See https://github.com/shader-slang/slang/blob/b282c88d9743fc9bb60ef27cfa5d9cf58cccd60b/source/slang/slang-language-server.cpp#L2707.
  void cancelPreviousRequestsForMethod(const char* method);

  [[nodiscard]] int32_t newMessageId() { return m_ioState.nextMessageId++; }

  // Tries to initialize the language server, if it isn't running.
  // Returns true on success; on false, don't try to talk to it.
  [[nodiscard]] bool getLanguageServer();

  // Processes pending messages; sets up state for doUI to read.
  void processMessageQueue();

  // Processes a single message.
  void processMessage(const nlohmann::json& message);

  // Iterate over the UTF-8 codepoints in the line for the main cursor to
  // get (1) the current character in bytes from the column, and (2) the
  // current character in UTF-16 indices.
  // All fields are outputs.
  Utf8Iterator getMainCursorIterator(int& mainCursorLine);

  // Extract a string for the identifier before the cursor.
  std::string getAutocompletionIdentifierGivenCursor(const Utf8Iterator& cursor, int cursorLine, Utf8Iterator& start);

  // This can be empty if none matched.
  std::span<CompletionItem> getMatchedCompletions(const std::string& search);

public:
  void init(TextEditorEx& codeEditor);

  void deinit()
  {
    resetState();
    m_slangd.deinit();
  }

  void interceptKeys();

  void doUI(bool codeEditorFocused);

  void notifyDocumentClosed();
  void notifyDocumentName(const std::filesystem::path& path);

  LanguageServer::Settings& settings(LanguageServer::Settings* newSettings);
};

LanguageServer::Implementation::DocumentState::DocumentState()
{
  setPath({});
}

void LanguageServer::Implementation::DocumentState::setPath(const std::filesystem::path& path)
{
  if(path.empty())
  {
    // On Linux, slangd won't produce autocompletions inside functions unless
    // the uri points to a real, readable file that exists on disk. It doesn't
    // have to be a Slang file, though! So we use the executable path.
    // (nbickford thinks this is because the autocompletion code in Slang
    // calls Path::getCanonical, which calls realpath, which returns an empty
    // string; this then messes up getOrLoadModule() somehow).
    m_path = nvutils::getExecutablePath();
  }
  else
  {
    m_path = path;
  }

  const std::string utf8 = nvutils::utf8FromPath(m_path);
  // Hand-written from https://en.wikipedia.org/wiki/Percent-encoding;
  // see also https://en.wikipedia.org/wiki/File_URI_scheme
  m_uri = "file:///";  // No host
  for(const char c : utf8)
  {
    if(c == '\\' || c == '/')  // Directory separators
    {
      m_uri.push_back('/');
    }
    else if(('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z') || ('0' <= c && c <= '9') || (c == '-') || (c == '_')
            || (c == '~') || (c == '.'))
    {
      m_uri.push_back(c);
    }
    else
    {
      static const std::array<char, 16> hextable = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
      m_uri.push_back('%');
      m_uri.push_back(hextable[static_cast<uint8_t>(c) >> 4]);
      m_uri.push_back(hextable[static_cast<uint8_t>(c) & 15]);
    }
  }
}

bool LanguageServer::Implementation::processSendQueue()
{
  while(!m_ioState.pendingSends.empty())
  {
    nlohmann::json& send = m_ioState.pendingSends.front();
    if(!(m_ioState.initializeRespondedTo || send.at("method") == "initialize"))
    {
      // Can't send this message yet
      break;
    }

    std::stringstream fullRequest;
    // slangd appears to want two \r\ns at the end
    const std::string jsonStr = send.dump();
    fullRequest << "Content-Length: " << jsonStr.size() + 4 << "\r\n\r\n" << jsonStr << "\r\n\r\n";
    const bool sendOk = m_slangd.sendStringData(fullRequest.str());

    if(!sendOk)
    {
      LOGW("Language server appears to have crashed. Clearing pending requests.\n");
      resetState();
      return false;
    }

    const auto& it = send.find("id");
    if(it != send.end())
    {
      const int id                  = it->template get<int>();
      m_ioState.pendingRequests[id] = send;
    }
    m_ioState.pendingSends.pop();
  }
  return true;
}

void LanguageServer::Implementation::cancelPreviousRequestsForMethod(const char* method)
{
  // Cancel messages that haven't been sent yet
  std::queue<nlohmann::json> oldPendingSends = std::move(m_ioState.pendingSends);
  while(oldPendingSends.size() > 0)
  {
    nlohmann::json json = std::move(oldPendingSends.front());
    oldPendingSends.pop();

    if(json.value("method", "") == method && json.contains("id"))
    {
      // Just don't copy it over; no need to send a cancellation
    }
    else
    {
      m_ioState.pendingSends.push(std::move(json));
    }
  }

  // Cancel messages we're waiting on that haven't been responded to yet
  std::vector<int32_t> idsToRemove;
  for(const auto& kvp : m_ioState.pendingRequests)
  {
    if(kvp.second.value("method", "") == method)
    {
      idsToRemove.push_back(kvp.first);
    }
  }

  for(int32_t id : idsToRemove)
  {
    m_ioState.pendingRequests.erase(id);
    nlohmann::json cancellation = {{"method", "$/cancelRequest"}, {"params", {{"id", id}}}};
    sendJsonRpc(cancellation);
  }

  if(!idsToRemove.empty())
  {
    processSendQueue();
  }
}

[[nodiscard]] bool LanguageServer::Implementation::getLanguageServer()
{
  if(m_ioState.permanentlyFailed)
  {
    return false;
  }
  if(m_slangd.isRunning())
  {
    return true;
  }
  if(!m_slangd.canTryStart())
  {
    return false;
  }

  // Send our initialization message. This will also try to start slangd.
  LOGI("Sending slangd an initialization message...\n");
  // https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#initialize
  m_ioState.initializeRespondedTo = false;

#ifdef _WIN32
  const DWORD pid = GetCurrentProcessId();
#else
  const pid_t pid = getpid();
#endif

  // This is hacky but seems to be enough for now for slangd; ideally we'd
  // advertise completion capabilities as well but this is fine
  nlohmann::json json{{"id", newMessageId()},    //
                      {"method", "initialize"},  //
                      {"params",
                       {
                           //
                           {"processId", pid},                       //
                           {"clientInfo", {{"name", TARGET_NAME}}},  //
                       }}};
  sendJsonRpc(json);
  processSendQueue();
  return true;
}

void LanguageServer::Implementation::processMessageQueue()
{
  if(!getLanguageServer())
  {
    return;
  }

  std::optional<std::string> responseText = m_slangd.receiveStringData();
  if(!responseText.has_value())
  {
    LOGW("Language server appears to have crashed. Clearing pending requests.\n");
    resetState();
    return;
  }

  auto& receivedText = m_ioState.receivedText;
  receivedText.insert(receivedText.end(), responseText.value().begin(), responseText.value().end());
  // Pop as many full responses off as we can.
  while(true)
  {
    // First, start by removing newlines
    {
      size_t marker = 0;
      while(marker < receivedText.size() && isNewline(receivedText[marker]))
      {
        marker++;
      }
      receivedText.erase(receivedText.begin(), receivedText.begin() + marker);
    }

    // Now, see if we've got a complete header part and block.
    // We should have a series of lines ending with \r\n, then an empty line and \r\n.
    std::optional<size_t> contentLength  = {};
    bool                  foundHeaderEnd = false;
    size_t                contentStart   = 0;
    {
      bool   lastLineWasEmpty = false;
      size_t lineStart        = 0;
      size_t lineEnd          = 0;
      while(lineEnd < receivedText.size())
      {
        lineEnd = lineStart;
        // Advance lineEnd until we reach EOF or a newline
        while(lineEnd < receivedText.size() && !isNewline(receivedText[lineEnd]))
        {
          lineEnd++;
        }
        // [lineStart, lineEnd) is a line. If it's empty, it might be the
        // last one of the header block. Also, parse it for Content-Length.
        const size_t lineLength = lineEnd - lineStart;
        // This isn't very efficient, but should be OK for now
        std::string line;
        line.resize(lineLength);
        for(size_t i = lineStart; i < lineEnd; i++)
        {
          line[i - lineStart] = receivedText[i];
        }
        std::istringstream lineStream(line);
        try
        {
          std::string prefix;
          size_t      len = 0;
          lineStream >> prefix >> len;
          if(prefix == "Content-Length:")
          {
            contentLength = {len};
          }
        }
        catch(const std::exception& /* unused */)
        {
        }

        // Now advance lineEnd past one \r\n.
        if(lineEnd + 2 > receivedText.size())
        {
          // Not enough characters; partial read.
          break;
        }
        if(receivedText[lineEnd] != '\r' || receivedText[lineEnd + 1] != '\n')
        {
          LOGW("Incorrect line endings!\n");

          LOGW("receivedText buffer contains %zu bytes. Hex: ", receivedText.size());

          std::stringstream hexData;
          for(size_t i = 0; i < receivedText.size(); i++)
          {
            const char  c     = receivedText[i];
            const char* chars = "0123456789ABCDEF";
            hexData << chars[uint8_t(c) >> 4] << chars[c & 15] << " ";
          }
          LOGW("%s\n", hexData.str().c_str());

          LOGE(
              "Clearing received text buffer. Language server might recover, but it should be considered unstable. "
              "Please create an issue on https://github.com/nvpro-samples/" TARGET_NAME
              " with the hex contents above so we can help diagnose this issue.\n");
          receivedText.clear();

          break;
        }
        lineEnd += 2;
        // lineEnd now points at the start of a new line:
        lineStart = lineEnd;
        // And if this line was empty, then we're at the start of the content
        // and should break:
        if(lineLength <= 1)
        {
          contentStart   = lineStart;
          foundHeaderEnd = true;
          break;
        }
      }
    }

    // If we didn't find the end of a header, we got a partial read.
    // No more messages to process.
    if(!foundHeaderEnd)
    {
      break;
    }

    // If we found the end of a header, we should have gotten Content-Length.
    // Check for this; if we didn't, bail; probably programmer error.
    if(!contentLength.has_value())
    {
      LOGE("Read a header block without a Content-Length field! This shouldn't happen.\n");
      break;
    }

    // We should now have at least Content-Length characters remaining.
    // If we didn't, then we got a partial read.
    const size_t charactersRemaining = receivedText.size() - contentStart;
    if(charactersRemaining < contentLength.value())
    {
      break;
    }

    // We have a full message! Pop off the header, then pop off the message.
    receivedText.erase(receivedText.begin(), receivedText.begin() + contentStart);

    // Also not very efficient but should do OK for now
    std::string jsonStr;
    jsonStr.resize(contentLength.value());
    for(size_t i = 0; i < contentLength.value(); i++)
    {
      jsonStr[i] = receivedText.front();
      receivedText.pop_front();
    }

    // Can we parse it as JSON?
    nlohmann::json json;
    try
    {
      json = nlohmann::json::parse(jsonStr);
    }
    catch(const std::exception& e)
    {
      // Most likely programmer error
      LOGE("Could not parse JSON: %s. String follows:\n%s\n", e.what(), jsonStr.c_str());
      continue;  // Try recovering with the next one, maybe?
    }

    // And handle it:
    processMessage(json);
  }
}

void LanguageServer::Implementation::processMessage(const nlohmann::json& message)
{
  // This is wrapped in a try/catch because that's nlohmann::json's
  // preferred method of error handling; there's still some manual error
  // checking here but that can probably be removed.
  // This could also be more robust; at the moment it only needs to
  // handle slangd.
  try
  {
    // Get the ID field so we can match this message up to where it came from
    int32_t id = 0;
    {
      const auto& it = message.find("id");
      if(it == message.end())
      {
        // This happens for textDocument/publishDiagnostics.
        // We don't currently handle this.
        return;
      }

      if(!it->is_number_integer())
      {
        // This shouldn't happen since we only send IDs that are integers.
        LOGE("Somehow got an ID that was not an integer; this should not happen.\n");
        return;
      }

      id = it->template get<int32_t>();
    }

    // Get the method this is calling. If the server's requesting us to do
    // something, `message` will have a `method` field. Otherwise, look it
    // up in our pending requests.
    std::string    method = "";
    nlohmann::json request;
    if(message.contains("method"))
    {
      method = message.at("method");
    }
    else
    {
      const auto& it = m_ioState.pendingRequests.find(id);
      if(it == m_ioState.pendingRequests.end())
      {
        // Most likely a cancelled request; ignore it.
        return;
      }
      request = std::move(it->second);
      m_ioState.pendingRequests.erase(it);

      method = request.at("method");
    }

    // If the result failed, log that.
    if(message.contains("error"))
    {
      const auto& error = message.at("error");
      LOGW("Response to message %d (method %s) produced error %d: %s\n", id, method.c_str(),
           error.value<int32_t>("code", 0), error.value("message", "").c_str());
      return;
    }

    // Awesome! Now we can respond to it.
    if(method == "initialize")
    {
      LOGI("slangd initialized.\n");
      m_ioState.initializeRespondedTo = true;

      // We currently don't handle the following fields:
      // - hoverProvider (need to add hook to TextEditor to get coordinate
      // under mouse cursor)
      // - inlayHintProvider (that's where it displays arguments and types)
      // - we could handle documentOnTypeFormattingProvider, but I know people who really don't like it so I'll avoid it
      // - definitionProvider (go to definition)
      // - documentSymbolProvider (get symbols in document)
      const auto& result       = message.at("result");
      const auto& capabilities = result.at("capabilities");

      const auto& textDocumentSync = capabilities.at("textDocumentSync");
      if(textDocumentSync.is_object())
      {
        m_serverInfo.syncKind = static_cast<SyncKind>(textDocumentSync.value<uint32_t>("change", 0u));
      }
      else
      {
        m_serverInfo.syncKind = static_cast<SyncKind>(textDocumentSync.template get<uint32_t>());
      }

      // slangd always uses eIncremental. But check for it anyways,
      // just in case we need to check it.
      if(m_serverInfo.syncKind != SyncKind::eFull && m_serverInfo.syncKind != SyncKind::eIncremental)
      {
        LOGW(
            "Server specified syncKind %u; this isn't one of the kinds we know -- Full (1) or Incremental (2) -- and "
            "so syncs will not happen.\n",
            static_cast<unsigned>(m_serverInfo.syncKind));
      }

      m_serverInfo.canFormat      = capabilities.value<bool>("documentFormattingProvider", false);
      m_serverInfo.canFormatRange = capabilities.value<bool>("documentRangeFormattingProvider", false);

      const auto& semanticTokens    = capabilities.at("semanticTokensProvider");
      m_serverInfo.canTokenizeFull  = semanticTokens.value<bool>("full", false);
      m_serverInfo.canTokenizeRange = semanticTokens.value<bool>("range", false);

      // Set up character tables
      for(size_t i = 0; i < kCharTableSize; i++)
      {
        m_serverInfo.completionEnds[i] = true;
      }
      for(ImWchar c : {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '_'})
      {
        m_serverInfo.identifierCharacters[c] = true;
        m_serverInfo.completionEnds[c]       = false;
      }
      for(ImWchar c = 'A'; c <= 'Z'; c++)
      {
        m_serverInfo.identifierCharacters[c] = true;
        m_serverInfo.completionTriggers[c]   = true;
        m_serverInfo.completionEnds[c]       = false;
      }
      for(ImWchar c = 'a'; c <= 'z'; c++)
      {
        m_serverInfo.identifierCharacters[c] = true;
        m_serverInfo.completionTriggers[c]   = true;
        m_serverInfo.completionEnds[c]       = false;
      }

      const auto& compTriggerChars = capabilities.at("completionProvider").at("triggerCharacters");
      for(const auto& [key, value] : compTriggerChars.items())
      {
        if(value.is_string())
        {
          const std::string& v = value.template get<std::string>();
          assert(v.size() == 1);
          if(v.size() > 0)
          {
            const char c                       = v[0];
            m_serverInfo.completionTriggers[c] = true;
            m_serverInfo.completionEnds[c]     = false;
          }
        }
      }

      const auto& signatureHelpChars = capabilities.at("signatureHelpProvider").at("triggerCharacters");
      for(const auto& [key, value] : signatureHelpChars.items())
      {
        if(value.is_string())
        {
          const std::string& v = value.template get<std::string>();
          assert(v.size() == 1);
          if(v.size() > 0)
          {
            const char c                          = v[0];
            m_serverInfo.signatureHelpTriggers[c] = true;
          }
        }
      }

#if 0  // Or not, to match how VS Code does things
      // slangd wants space to be a completion trigger. It should actually be
      // a stop, in my opinion.
      m_serverInfo.completionTriggers[' '] = false;
      m_serverInfo.completionEnds[' ']     = true;
#endif

      // It doesn't seem like the JSON gives us help with this
      m_serverInfo.signatureHelpEnds[')'] = true;
      m_serverInfo.signatureHelpEnds[';'] = true;
      m_serverInfo.signatureHelpEnds[27]  = true;  // escape

      // Finish the handshake by sending https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#initialized
      nlohmann::json json{
          {"id", newMessageId()},     //
          {"method", "initialized"},  //
      };
      sendJsonRpc(json);

#if 0  // This doesn't work!                                                                                           \
       // Make the server send us a lot of trace messages
      json = {{"method", "$/setTrace"}, {"params", {{"value", "verbose"}}}};
      sendJsonRpc(json);
#endif
    }
    else if(method == "textDocument/completion")
    {
      // Slang might have given us a lot of data here! I'm not sure it
      // filters at all. Maybe we shouldn't be doing this on every character.

      // If the UI code cleared completionActive (so we're no longer showing
      // completions), throw this away.
      if(!m_uiState.completionActive.has_value())
      {
        return;
      }
      m_uiState.completions.clear();

      // Result should be an array of CompletionItem objects. We don't support
      // the case where this returns an object instead of an array yet.
      const nlohmann::json& result = message.at("result");
      if(result.size() == 0)
      {
        // Empty object, so no completions.
        return;
      }

      if(!result.is_array())
      {
        LOGW("Received an object other than an array for textDocument/completion; cannot handle this.\n");
        return;
      }

      // Load them:
      m_uiState.completions.reserve(result.size());
      for(const auto& [keyUnused, value] : result.items())
      {
        CompletionItem item{.label = value.value("label", "<missing label>"),
                            .kind  = static_cast<CompletionItemKind>(value.value<uint32_t>("kind", 0))};
        m_uiState.completions.push_back(std::move(item));
      }

      // Then sort them alphabetically, case insensitive, so they display
      // nicely and we can filter them.
      // We use a stable sort here because e.g. `texture2d` compares equal to
      // `Texture2D`.
      std::stable_sort(m_uiState.completions.begin(), m_uiState.completions.end(),
                       [](const CompletionItem& a, const CompletionItem& b) { return stricompare(a.label, b.label) < 0; });

      // Reset the active completion index. We can probably do better here
      // but setting it to 0 matches what Slang Playground does.
      m_uiState.completionActive = CompletionActive();
    }
    else if(method == "textDocument/signatureHelp")
    {
      m_uiState.signatures.clear();

      // See https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#signatureHelp.
      const nlohmann::json& result = message.at("result");
      if(result.size() == 0)
      {
        return;
      }

      // We don't use activeSignature or activeParameter yet.
      // For now, just loop over SignatureInformation.
      const nlohmann::json& signatures = result.at("signatures");
      if(signatures.size() == 0)
      {
        return;
      }

      m_uiState.signatures.reserve(signatures.size());
      // We also reverse signatures here; the reason is that for some reason,
      // slangd sends the last signature that appears in `core` first, but
      // the first signature is usually the one with the most documentation
      // (and thus the one we want to show).
      for(auto sigIt = signatures.crbegin(); sigIt != signatures.crend(); ++sigIt)
      {
        const nlohmann::json& value = *sigIt;
        SignatureInformation  info{.label = value.value("label", "<missing label>")};
        const auto&           docIt = value.find("documentation");
        if(docIt != value.end())
        {
          const auto& docObjectOrString = *docIt;
          if(docObjectOrString.is_string())
          {
            info.documentation = docObjectOrString.template get<std::string>();
          }
          else
          {
            assert(docObjectOrString.is_object());
            info.documentation = docObjectOrString.at("value").template get<std::string>();
          }
        }
        m_uiState.signatures.push_back(std::move(info));
      }

      // The documentation could usually use some formatting. There are extra
      // newlines, and for some reason paths are absolute to the exe directory.
      // As a quick sanitization pass:
      // - trim leading and trailing newlines
      // - if we see the current working directory, remove it
      const std::filesystem::path& cwd    = std::filesystem::current_path();
      std::string                  cwdStr = nvutils::utf8FromPath(cwd);
      if(cwdStr.back() != '/' && cwdStr.back() != '\\')
      {
        cwdStr += static_cast<char>(cwd.preferred_separator);
      }

      for(SignatureInformation& info : m_uiState.signatures)
      {
        if(!info.documentation.has_value())
        {
          continue;
        }

        std::string& doc             = info.documentation.value();
        const size_t firstNonNewline = doc.find_first_not_of("\r\n");
        if(firstNonNewline != doc.npos)
        {
          const size_t lastNonNewline = doc.find_last_not_of("\r\n");  // non-npos
          doc                         = doc.substr(firstNonNewline, lastNonNewline - firstNonNewline + 1);
        }

        while(true)
        {
          const size_t exeDirIdx = doc.find(cwdStr);
          if(exeDirIdx == doc.npos)
          {
            break;
          }
          doc.erase(exeDirIdx, cwdStr.size());
        }
      }

      // May as well keep the signature index if we already have it
      if(!m_uiState.signatureActive.has_value())
      {
        m_uiState.signatureActive = {0};
      }
    }
    else if(method == "client/registerCapability" || method == "workspace/configuration")
    {
      // Not supported; nothing to do
    }
    else
    {
      LOGW("Unknown method: %s.\n", method.c_str());
    }
  }
  catch(const std::exception& e)
  {
    LOGW("Caught exception while processing message: %s\n", e.what());
  }
}

Utf8Iterator LanguageServer::Implementation::getMainCursorIterator(int& mainCursorLine)
{
  int mainCursorColumn = 0;
  m_editor->GetMainCursor(mainCursorLine, mainCursorColumn);
  Utf8Iterator mainCursorIt = Utf8Iterator(m_documentState.lines[mainCursorLine], static_cast<size_t>(m_editor->GetTabSize()));
  while(mainCursorIt && mainCursorIt.columnIndex() < mainCursorColumn)
  {
    ++mainCursorIt;
  }
  return mainCursorIt;
}

std::string LanguageServer::Implementation::getAutocompletionIdentifierGivenCursor(const Utf8Iterator& cursor,
                                                                                   int                 cursorLine,
                                                                                   Utf8Iterator&       start)
{
  start = cursor;
  while(true)
  {
    Utf8Iterator prev = start;
    --prev;
    // "If OOB or not an identifier any more, break
    if(!prev || !testCodepoint(m_serverInfo.identifierCharacters, prev.decode(), true))
    {
      break;
    }
    start = prev;
  }

  // Don't march forward; this makes it so things like (| denoting the cursor here)
  // float4 output = colo|float4(2.0);
  // will suggest `color`.

  const auto&       line = m_documentState.lines[cursorLine];
  const std::string identifier(line.begin() + start.byteIndex(), line.begin() + cursor.byteIndex());
  return identifier;
}

std::span<CompletionItem> LanguageServer::Implementation::getMatchedCompletions(const std::string& search)
{
  // Find the first entry where label >= search.
  auto firstCompletionIt = std::lower_bound(m_uiState.completions.begin(), m_uiState.completions.end(), search,
                                            [](const CompletionItem& completion, const std::string& search) {
                                              return stricompare(completion.label.c_str(), search.c_str()) < 0;
                                            });
  if(firstCompletionIt == m_uiState.completions.end())
  {
    return {};
  }

  // Now in the remainder, find the first one that doesn't start with `search`.
  auto exclusiveEndIt = std::lower_bound(firstCompletionIt, m_uiState.completions.end(), search,
                                         [](const CompletionItem& completion, const std::string& search) {
                                           return startsWithI(completion.label.c_str(), search.c_str());
                                         });

  // If we can, march forward until we find one that matches exactly.
  // This avoids cases where e.g. "Texture2D" gives you "texture2d".
  // The copy in the first clause of this `for` loop is intentional.
  for(auto testIt = firstCompletionIt; testIt != exclusiveEndIt; ++testIt)
  {
    if(testIt->label == search)
    {
      firstCompletionIt = testIt;
      break;
    }
    if(!strieq(testIt->label.c_str(), search.c_str()))
    {
      break;
    }
  }

  return std::span<CompletionItem>(firstCompletionIt, exclusiveEndIt);
}

void LanguageServer::Implementation::init(TextEditorEx& codeEditor)
{
  m_editor = &codeEditor;

  // Try to find slangd
  const std::filesystem::path        exeDir = nvutils::getExecutablePath().parent_path();
  std::vector<std::filesystem::path> searchPaths{exeDir};
#ifdef _WIN32
  std::filesystem::path slangdPath = nvutils::findFile(L"slangd.exe", searchPaths, false);
#else
  std::filesystem::path slangdPath = nvutils::findFile("slangd", searchPaths, false);
#endif
  m_slangd.init(std::move(slangdPath));
}


void LanguageServer::Implementation::notifyDocumentClosed()
{
  if(!m_settings.enabled)
  {
    return;
  }

  if(!getLanguageServer())
  {
    return;
  }

  if(m_documentState.currentlyOpen)
  {
    nlohmann::json json = {{"method", "textDocument/didClose"},
                           {"params",
                            {//
                             {"textDocument", {"uri", m_documentState.getUri()}}}}};
    sendJsonRpc(json);
    m_documentState.currentlyOpen = false;
    m_documentState.setPath("");
  }
}


void LanguageServer::Implementation::notifyDocumentName(const std::filesystem::path& path)
{
  m_documentState.setPath(path);
}


LanguageServer::Settings& LanguageServer::Implementation::settings(Settings* newSettings)
{
  if(newSettings)
  {
    if(m_settings.enabled && !newSettings->enabled)
    {
      deinit();
    }

    m_settings = *newSettings;

    m_slangd.setRestartInterval(m_settings.restartInterval);
  }

  return m_settings;
}


void LanguageServer::Implementation::interceptKeys()
{
  if(!m_settings.enabled)
  {
    return;
  }

  if(m_uiState.editorWasFocused)
  {
    // This ID is the top 4 bytes of the SHA-3 hash of
    // "vk_slang_editor::interceptKeys()"
    const ImGuiID myId = 0xe70e871au;

    if(m_uiState.wasShowingCompletions && m_uiState.completionActive.has_value())
    {
      // Take ownership of some keys so that the code editor doesn't react
      // to them.

      for(const auto key : {ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_Tab, ImGuiKey_Enter})
      {
        ImGui::SetKeyOwner(key, myId, ImGuiInputFlags_LockThisFrame);
      }

      const bool ctrlOnly =
          ImGui::IsKeyDown(ImGuiMod_Ctrl) && !ImGui::IsKeyDown(ImGuiMod_Shift) && !ImGui::IsKeyDown(ImGuiMod_Alt);
      if(ctrlOnly)
      {
        ImGui::SetKeyOwner(ImGuiKey_J, myId, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_K, myId, ImGuiInputFlags_LockThisFrame);
      }

      if(ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, myId)
         || (ctrlOnly && ImGui::IsKeyPressed(ImGuiKey_K, ImGuiInputFlags_Repeat, myId)))
      {
        m_uiState.completionActive->index--;
      }

      if(ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, myId)
         || (ctrlOnly && ImGui::IsKeyPressed(ImGuiKey_J, ImGuiInputFlags_Repeat, myId)))
      {
        m_uiState.completionActive->index++;
      }

      if(ImGui::IsKeyPressed(ImGuiKey_Tab, ImGuiInputFlags_None, myId)
         || ImGui::IsKeyPressed(ImGuiKey_Enter, ImGuiInputFlags_None, myId))
      {
        // Try to insert an autocompletion
        // Note: This code is pretty much the same as in the UI -- we can
        // probably simplify things more.
        int                mainCursorLine = 0;
        const Utf8Iterator mainCursorIt   = getMainCursorIterator(mainCursorLine);
        Utf8Iterator       identifierStart;
        const std::string identifier = getAutocompletionIdentifierGivenCursor(mainCursorIt, mainCursorLine, identifierStart);
        std::span<const CompletionItem> completions = getMatchedCompletions(identifier);
        const int32_t                   uiIndex     = m_uiState.completionActive->index;
        if(0 <= uiIndex && uiIndex < completions.size())
        {
          m_editor->ReplaceRange(mainCursorLine, static_cast<int>(identifierStart.columnIndex()), mainCursorLine,
                                 static_cast<int>(mainCursorIt.columnIndex()), completions[uiIndex].label);
          m_uiState.completionActive = {};
        }
      }
    }

    if(m_uiState.wasShowingSignatures)
    {
      // Similarly for signature help, but using left/right + only taking
      // ownership if Shift is pressed
      if(ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_LeftArrow))
      {
        ImGui::SetKeyOwner(ImGuiKey_LeftArrow, myId, ImGuiInputFlags_LockThisFrame);
        m_uiState.signatureActive = std::max(1u, m_uiState.signatureActive.value()) - 1;
      }

      if(ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_RightArrow))
      {
        ImGui::SetKeyOwner(ImGuiKey_RightArrow, myId, ImGuiInputFlags_LockThisFrame);
        m_uiState.signatureActive = m_uiState.signatureActive.value() + 1;
      }
    }

    // Copy the input characters before TextEditor gets to them so we can
    // react to new characters below (only when in standard or Vim Insert/Replace mode)
    if(!m_editor || m_editor->IsVimInsertOrReplaceMode())
    {
      m_uiState.inputQueueCharacters = ImGui::GetIO().InputQueueCharacters;
    }
    else
    {
      m_uiState.inputQueueCharacters.clear();
    }
  }
}

void LanguageServer::Implementation::doUI(bool codeEditorFocused)
{
  if(!m_settings.enabled)
  {
    return;
  }

  if(!getLanguageServer())
  {
    return;
  }

  // Track changes to the document and send updates.
  const size_t thisFrameUndoIndex = m_editor->GetUndoIndex();
  const bool needUpdate = !m_documentState.currentlyOpen || (m_documentState.lastFrameUndoIndex != thisFrameUndoIndex);
  if(needUpdate)
  {
    // Get lines
    const int                newLineCount = static_cast<size_t>(m_editor->GetLineCount());
    std::vector<std::string> newLines(newLineCount);
    for(int newLine = 0; newLine < newLineCount; newLine++)
    {
      newLines[newLine] = m_editor->GetLineText(newLine);
    }

    if(!m_documentState.currentlyOpen)
    {
      // New document
      // Note: We can do this faster by using '\n'.join(newLines), but this should be ok for now
      const std::string text = m_editor->GetText();
      nlohmann::json    json = {{"method", "textDocument/didOpen"},
                                {"params",
                                 {//
                               {"textDocument",
                                   {//
                                 {"uri", m_documentState.getUri()},
                                 {"languageId", "slang"},
                                 {"version", ++m_documentState.version},
                                 {"text", text}}}}}};
         sendJsonRpc(json);
      m_documentState.currentlyOpen = true;
    }
    else if(m_serverInfo.syncKind == SyncKind::eIncremental || m_serverInfo.syncKind == SyncKind::eFull)
    {
      nlohmann::json changeEvent;
      if(m_serverInfo.syncKind == SyncKind::eIncremental)
      {
        // We could do more sophisticated diffing - but keep it simple for now;
        // just find the range of lines that were replaced.
        const auto& oldLines = m_documentState.lines;

        // First, find how many lines at the start match:
        size_t       startLinesMatch = 0;
        const size_t maxCommonLines  = std::min(oldLines.size(), newLines.size());
        for(size_t i = 0; i < maxCommonLines && oldLines[i] == newLines[i]; i++)
        {
          startLinesMatch++;
        }

        // Slang seems to delete its copy of the document if we specify a
        // starting coordinate at the end of the document; this happens if we
        // insert a newline at the end. I suspect we're not reaching the
        // `startOffset != -1` case in https://github.com/shader-slang/slang/blob/b282c88d9743fc9bb60ef27cfa5d9cf58cccd60b/source/slang/slang-workspace-version.cpp#L58.
        // To work around this, if all the starting lines matched but there's
        // at least one new line in the document, we re-send the full document.
        if(startLinesMatch == oldLines.size() && newLines.size() > oldLines.size())
        {
          startLinesMatch = 0;
        }

        // We can also run into the `startLinesMatch` case if the text editor
        // created an undo index but didn't actually change the content.
        // Avoid this.
        if(startLinesMatch != oldLines.size() || oldLines.size() == 0)
        {
          // Now count backwards to see how many lines at the end match,
          // making sure not to overlap with those that we already matched:
          size_t endLinesMatch = 0;
          {
            const size_t endMaxIterLength = maxCommonLines - startLinesMatch;
            for(size_t i = 0;         //
                i < endMaxIterLength  //
                && oldLines[oldLines.size() - i - 1] == newLines[newLines.size() - i - 1];
                i++)
            {
              endLinesMatch++;
            }
          }
          // So the range of lines that changed in the new state starts at
          // `startLinesMatch`, and has length `newLines.size() - startLinesMatch - endLinesMatch`.
          // I.e. our range is [startLinesMatch, newLines.size() - endLinesMatch).
          // Note the half-open interval syntax.
          std::string newText;
          for(size_t i = startLinesMatch; i < newLines.size() - endLinesMatch; i++)
          {
            newText += newLines[i];
            // GetLineText ignores the last character.
            // The text editor standardizes newlines, so we can put in \n here.
            newText += '\n';
          }

          // Note that `end` specifies the start of the line after the change here.
          changeEvent = {{"range",
                          {{"start", makePositionJson(startLinesMatch, 0)},
                           {"end", makePositionJson(oldLines.size() - endLinesMatch, 0)}}},
                         {"text", std::move(newText)}};

          nlohmann::json json = {{"method", "textDocument/didChange"},
                                 {"params",
                                  {//
                                   {"textDocument",
                                    {//
                                     {"uri", m_documentState.getUri()},
                                     {"version", ++m_documentState.version}}},
                                   {"contentChanges", {changeEvent}}}}};
          sendJsonRpc(json);
        }
      }
      if(m_serverInfo.syncKind == SyncKind::eFull)
      {
        // Replace the full document.
        std::string newText = m_editor->GetText();
        changeEvent = {{"range", {{"start", makePositionJson(0, 0)}, {"end", makePositionJson(m_documentState.lines.size(), 0)}}},
                       {"text", std::move(newText)}};

        nlohmann::json json = {{"method", "textDocument/didChange"},
                               {"params",
                                {//
                                 {"textDocument",
                                  {//
                                   {"uri", m_documentState.getUri()},
                                   {"version", ++m_documentState.version}}},
                                 {"contentChanges", {changeEvent}}}}};
        sendJsonRpc(json);
      }
    }

    m_documentState.lastFrameUndoIndex = thisFrameUndoIndex;
    m_documentState.lines              = std::move(newLines);
  }

  int                mainCursorLine = 0;
  const Utf8Iterator mainCursorIt   = getMainCursorIterator(mainCursorLine);
  Utf8Iterator       identifierStart;
  const std::string  identifier = getAutocompletionIdentifierGivenCursor(mainCursorIt, mainCursorLine, identifierStart);

  // If the user's typing into the text editor, poll the keyboard to see if
  // they pressed any keys that would lead to auto-completion or signature help.
  // If any modifier keys are pressed, that doesn't affect things.
  // Some keys also deactivate these; I'm not sure what the specification for this is.
  if(codeEditorFocused)
  {
    if(m_editor && !m_editor->IsVimInsertOrReplaceMode())
    {
      m_uiState.completionActive = {};
      m_uiState.signatureActive  = {};
      m_uiState.completions.clear();
      m_uiState.signatures.clear();
    }
    else if(!ImGui::IsKeyDown(ImGuiMod_Ctrl) && !ImGui::IsKeyDown(ImGuiMod_Alt))
    {
      const bool wasSignatureActive   = m_uiState.signatureActive.has_value();
      const bool wasCompletionActive  = m_uiState.completionActive.has_value();
      bool       pressedNonIdentifier = false;

      std::optional<ImWchar> completionTriggerChar = {};
      std::optional<ImWchar> signatureTriggerChar  = {};
      for(int i = 0; i < m_uiState.inputQueueCharacters.size(); i++)
      {
        const ImWchar c = m_uiState.inputQueueCharacters[i];
        if(testCodepoint(m_serverInfo.completionTriggers, c, true) && !m_uiState.completionActive.has_value())
        {
          completionTriggerChar      = {c};
          m_uiState.completionActive = CompletionActive();  // Start completion; default to the first one
        }
        if(testCodepoint(m_serverInfo.completionEnds, c, false))
        {
          m_uiState.completionActive = {};  // End completion
        }
        if(testCodepoint(m_serverInfo.signatureHelpTriggers, c, false) && !m_uiState.signatureActive.has_value())
        {
          signatureTriggerChar      = {c};
          m_uiState.signatureActive = {0};
        }
        if(testCodepoint(m_serverInfo.signatureHelpEnds, c, false))
        {
          m_uiState.signatureActive = {};
        }
        if(!testCodepoint(m_serverInfo.identifierCharacters, c, true))
        {
          pressedNonIdentifier = true;
        }
      }
      bool completionChanged = !m_uiState.inputQueueCharacters.empty();

      // As special cases, stop completion if:
      // - Escape was pressed
      // As a special case, stop completion if text was deleted or a new word was started and now the identifier is empty.
      if(ImGui::IsKeyPressed(ImGuiKey_Escape)
         || (identifier.empty() && (ImGui::IsKeyPressed(ImGuiKey_Backspace) || ImGui::IsKeyPressed(ImGuiKey_Delete))))
      {
        m_uiState.completionActive = {};
        m_uiState.signatureActive  = {};
        completionChanged          = true;
      }

      // Also treat cursor keys (that weren't captured by interceptKeys)
      // as completion ends
      if(ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)
         || ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow))
      {
        m_uiState.completionActive = {};
      }

      // And as another special case, on space (which usually marks a new term),
      // pre-emptively clear completions so that we don't get flickering windows.
      if(ImGui::IsKeyPressed(ImGuiKey_Space))
      {
        m_uiState.completions.clear();
      }

      // Did things change at all?
      if(completionChanged)
      {
        // Throw away previous completions/signatures if we reached an end.
        if(!m_uiState.completionActive.has_value())
        {
          m_uiState.completions.clear();
        }
        if(!m_uiState.signatureActive.has_value())
        {
          m_uiState.signatures.clear();
        }

        // Update completion / signature help.
        // This extra clause at the end is a small optimization; if we're
        // already showing completions, we only re-request if the user typed
        // something that wasn't an identifier character.
        // Otherwise we get swamped parsing the massive 'everything in the file'
        // JSON that slangd sends back
        if(m_uiState.completionActive.has_value() && mainCursorIt && (!wasCompletionActive || pressedNonIdentifier))
        {
          const char* method = "textDocument/completion";
          // If there are previous completion requests, cancel them and
          // remove them from the queue.
          cancelPreviousRequestsForMethod(method);

          // https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#completionContext
          nlohmann::json completionContext = nlohmann::json::object();
          if(completionTriggerChar.has_value()
             && !testCodepoint(m_serverInfo.identifierCharacters, completionTriggerChar.value(), true))
          {
            // Completion started by a dedicated trigger character
            completionContext["triggerKind"]      = 2;
            completionContext["triggerCharacter"] = std::string{static_cast<char>(completionTriggerChar.value())};
          }
          else
          {
            completionContext["triggerKind"] = 1;  // Invoked by identifier
          }

          nlohmann::json json{{"id", newMessageId()},
                              {"method", method},
                              {"params",
                               {//
                                {"context", completionContext},
                                {"textDocument", {{"uri", m_documentState.getUri()}}},
                                {"position", {{"line", mainCursorLine}, {"character", mainCursorIt.utf16Index()}}}}}};
          sendJsonRpc(json);
        };

        // Similar to above
        if(m_uiState.signatureActive.has_value() && mainCursorIt && (!wasSignatureActive || pressedNonIdentifier))
        {
          const char* method = "textDocument/signatureHelp";
          cancelPreviousRequestsForMethod(method);

          // https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#signatureHelpContext
          nlohmann::json signatureHelpContext = nlohmann::json::object();
          if(signatureTriggerChar.has_value())
          {
            signatureHelpContext["triggerKind"]      = 2;
            signatureHelpContext["triggerCharacter"] = std::string{static_cast<char>(signatureTriggerChar.value())};
          }
          else
          {
            signatureHelpContext["triggerKind"] = 1;
          }
          signatureHelpContext["isRetrigger"] = wasSignatureActive;
          // TODO: Serialize signature help so we can set activeSignatureHelp

          nlohmann::json json{{"id", newMessageId()},
                              {"method", method},
                              {"params",
                               {//
                                {"context", signatureHelpContext},
                                {"textDocument", {{"uri", m_documentState.getUri()}}},
                                {"position", {{"line", mainCursorLine}, {"character", mainCursorIt.utf16Index()}}}}}};
          sendJsonRpc(json);
        }
      }
    }
  }

  if(!processSendQueue())
  {
    return;
  }
  processMessageQueue();

  m_uiState.wasShowingCompletions = m_uiState.wasShowingSignatures = false;
  if(codeEditorFocused)
  {
    const ImGuiWindowFlags tooltipStyleFlags = ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings
                                               | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking;
    // Semitransparent background effect
    ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
    background.w *= 0.95f;
    ImGui::PushStyleColor(ImGuiCol_PopupBg, background);

    // Autocompletion tooltip
    if(m_uiState.completionActive.has_value() && !m_uiState.completions.empty())
    {
      // If there's nothing under the cursor, don't bother displaying things.
      std::span<const CompletionItem> completions = getMatchedCompletions(identifier);
      if(!completions.empty())
      {
        m_uiState.wasShowingCompletions = true;
        ImVec2 screenPos;
        if(m_editor->GetCharacterScreenPos(screenPos, mainCursorLine, static_cast<int>(identifierStart.columnIndex())))
        {
          ImGui::SetNextWindowPos(screenPos + ImVec2(0, m_editor->GetLineHeight()), ImGuiCond_Always);
          // See https://github.com/ocornut/imgui/issues/1345#issuecomment-333338986
          if(ImGui::Begin("##autocompletion", nullptr, tooltipStyleFlags))
          {
            if(ImGui::IsWindowHovered())
            {
              m_uiState.completionActive->index -= static_cast<int32_t>(roundf(ImGui::GetIO().MouseWheel));
            }
            // If our index is out of bounds, wrap it back to the start or end.
            // This matches what VS Code does.
            if(m_uiState.completionActive->index < 0)
            {
              m_uiState.completionActive->index = static_cast<int32_t>(completions.size()) - 1;
            }
            else if(m_uiState.completionActive->index >= completions.size())
            {
              m_uiState.completionActive->index = 0;
            }
            // Then clamp the scroll so we display as many of the kDisplayMax
            // completions as we can while keeping the index onscreen.
            m_uiState.completionActive->uiScroll =
                std::clamp(m_uiState.completionActive->uiScroll,                                   //
                           m_uiState.completionActive->index - CompletionActive::kDisplayMax + 1,  //
                           m_uiState.completionActive->index);
            m_uiState.completionActive->uiScroll =
                std::clamp(m_uiState.completionActive->uiScroll,  //
                           0,                                     //
                           std::max(0, static_cast<int32_t>(completions.size()) - CompletionActive::kDisplayMax));

            // Draw GUI
            ImGui::PushFont(nvgui::getMonospaceFont());
            const int32_t completionsDisplayed =
                std::min(CompletionActive::kDisplayMax, static_cast<int32_t>(completions.size()));
            for(int32_t i = 0; i < completionsDisplayed; i++)
            {
              const int32_t viewedIndex = m_uiState.completionActive->uiScroll + i;
              const auto&   completion  = completions[viewedIndex];
              if(ImGui::Selectable(completion.label.c_str(), (viewedIndex == m_uiState.completionActive->index),
                                   ImGuiSelectableFlags_SelectOnClick))
              {
                m_editor->ReplaceRange(mainCursorLine, static_cast<int>(identifierStart.columnIndex()), mainCursorLine,
                                       static_cast<int>(mainCursorIt.columnIndex()), completion.label);
              }
            }
            ImGui::PopFont();
            if(CompletionActive::kDisplayMax < completions.size())
            {
              ImGui::Text("...");
            }
            ImGui::End();
          }
        }
      }
    }

    // Signature tooltip
    if(m_uiState.signatureActive.has_value() && !m_uiState.signatures.empty())
    {
      m_uiState.wasShowingSignatures = true;
      // Clamp signatureActive to valid range
      // TODO: Make use of the signatureActive field so that slangd tracks
      // this for us and does something nicer
      m_uiState.signatureActive =
          std::min(m_uiState.signatureActive.value(), static_cast<uint32_t>(m_uiState.signatures.size()) - 1);
      const SignatureInformation& signature = m_uiState.signatures[m_uiState.signatureActive.value()];

      ImVec2 screenPos;
      if(m_editor->GetCharacterScreenPos(screenPos, mainCursorLine, static_cast<int>(mainCursorIt.columnIndex())))
      {
        ImGui::SetNextWindowPos(screenPos - ImVec2(0, m_editor->GetLineHeight()), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        // Size the tooltip so that we can see the full function.
        // Documentation will then be wrapped.
        ImGui::PushFont(nvgui::getMonospaceFont());
        const ImVec2 labelSize = ImGui::CalcTextSize(signature.label.c_str());
        ImGui::PopFont();
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGui::SetNextWindowSize(ImVec2(labelSize.x + style.WindowPadding.x * 2.0f, 0.0f), ImGuiCond_Always);

        if(ImGui::Begin("##signature", nullptr, tooltipStyleFlags))
        {
          ImVec4 dimmedTextColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
          dimmedTextColor.w      = 0.5f;
          ImGui::PushStyleColor(ImGuiCol_Text, dimmedTextColor);
          ImGui::Text("< %u/%zu >", m_uiState.signatureActive.value() + 1, m_uiState.signatures.size());
          ImGui::PopStyleColor();
          nvgui::tooltip("Navigate between function definitions using Shift+Left / Shift+Right.", false, 0.0f);

          if(signature.documentation.has_value())
          {
            ImGui::TextWrapped("%s", signature.documentation.value().c_str());
          }

          ImGui::Separator();

          ImGui::PushFont(nvgui::getMonospaceFont());
          ImGui::Text("%s", signature.label.c_str());
          ImGui::PopFont();

          ImGui::End();
        }
      }
    }

    ImGui::PopStyleColor();  // background

    // If the mouse was just clicked, they either moved the cursor or selected
    // something from the dialogs; in any case, we should deactivate them.
    if(ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
      m_uiState.completionActive = {};
      m_uiState.signatureActive  = {};
    }

    // While our popups are up, take ownership of the Escape key so ImGui
    // doesn't use it to exit the code editor.
    if(m_uiState.wasShowingCompletions || m_uiState.wasShowingSignatures)
    {
      ImGui::SetKeyOwner(ImGuiKey_Escape, 0x64264264);
    }
  }

  m_uiState.editorWasFocused = codeEditorFocused;
}


LanguageServer::~LanguageServer()
{
  assert(!m_pImpl);
}

void LanguageServer::init(TextEditorEx& textEditor)
{
  m_pImpl = new Implementation;
  m_pImpl->init(textEditor);
}

void LanguageServer::deinit()
{
  if(m_pImpl)
  {
    m_pImpl->deinit();
    delete m_pImpl;
    m_pImpl = nullptr;
  }
}

void LanguageServer::interceptKeys()
{
  m_pImpl->interceptKeys();
}

void LanguageServer::doUI(bool codeEditorFocused)
{
  m_pImpl->doUI(codeEditorFocused);
}

void LanguageServer::notifyDocumentClosed()
{
  m_pImpl->notifyDocumentClosed();
}

void LanguageServer::notifyDocumentName(const std::filesystem::path& path)
{
  m_pImpl->notifyDocumentName(path);
}

LanguageServer::Settings& LanguageServer::settings(Settings* newSettings)
{
  return m_pImpl->settings(newSettings);
}
