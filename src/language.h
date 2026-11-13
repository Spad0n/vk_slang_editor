/*
 * Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
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
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

// Provides Slang language and language server support for the text editor.

#include <ImGuiColorTextEdit/TextEditor.h>

#include <filesystem>

const TextEditor::Language* getSlangLanguageInfo();
const TextEditor::Language* getSpirVLanguageInfo();

// Extended version of the text editor, since we need to do things like text
// insertion and finding character screen positions
class TextEditorEx : public TextEditor
{
public:
  enum class VimMode
  {
    eNormal,
    eInsert,
    eReplace,
    eVisual,
    eVisualLine
  };

  void Render(const char* title, const ImVec2& size = ImVec2(), bool border = false);

  // Gets the screen position of the top-left corner of the glyph at the given
  // line and column (0-indexed).
  // Returns whether the glyph is visible onscreen.
  bool GetCharacterScreenPos(ImVec2& screenPos, int line, int column);

  // Replaces text starting at a given line and column (0-indexed) and
  // ending at a given line and column with the given text.
  void ReplaceRange(int startLine, int startColumn, int endLine, int endColumn, const std::string_view& text);

  // Vim modal editing support
  void        SetVimModeEnabled(bool enabled);
  bool        IsVimModeEnabled() const { return m_vimEnabled; }
  VimMode     GetVimMode() const { return m_vimMode; }
  bool        IsVimInsertOrReplaceMode() const { return !m_vimEnabled || m_vimMode == VimMode::eInsert || m_vimMode == VimMode::eReplace; }
  std::string GetVimStatusText() const;

protected:
  void handleKeyboardInputs() override;
  void renderCursors() override;

private:
  ImVec2     m_screenPosOnRender{};
  bool       m_vimEnabled      = false;
  VimMode    m_vimMode         = VimMode::eNormal;
  int        m_vimCount        = 0;
  char       m_vimPendingOp    = 0;
  char       m_vimPendingSubOp = 0;
  bool       m_vimLastYankLine = false;
  Coordinate m_vimVisualAnchor{0, 0};
  Coordinate m_vimVisualHead{0, 0};

  void       resetVimPendingState();
  void       enterVimNormalMode(bool moveLeftOnExitInsert = false);
  void       enterVimInsertMode();
  Coordinate clampVimNormalCoord(Coordinate c) const;
  Coordinate getVimCharEndOnLine(Coordinate c) const;
  Coordinate getVimFirstNonBlank(int line) const;
  Coordinate getVimWordForward(Coordinate from, bool bigWord = false) const;
  Coordinate getVimWordBackward(Coordinate from, bool bigWord = false) const;
  Coordinate getVimWordEnd(Coordinate from, bool bigWord = false) const;
  Coordinate getVimMatchingBracket(Coordinate from) const;
  void       updateVimVisualSelection();
  void       yankVimRange(Coordinate start, Coordinate end, bool linewise);
  void       deleteVimRange(Coordinate start, Coordinate end, bool linewise);
  void       pasteVim(bool after);
  bool       resolveVimTextObject(char modifier, ImWchar obj, Coordinate cur, Coordinate& outStart, Coordinate& outEnd) const;
  bool       resolveVimMotion(ImWchar motion, ImWchar arg, int count, Coordinate cur,
                              Coordinate& outTarget, bool& outLinewise, bool& outInclusive) const;
  void       applyVimOperatorToRange(char op, Coordinate start, Coordinate end, bool linewise);
  void       handleVimKeyInNormalOrVisual(ImWchar ch);
};

struct LanguageServer
{
  ~LanguageServer();
  void init(TextEditorEx& codeEditor);
  void deinit();

  // Intercepts keyboard commands that should go to the tooltips instead.
  // Call this before rendering the TextEditor.
  void interceptKeys();

  // Checks for changes, adds tooltips, etc.
  // Call this after rendering the TextEditor.
  void doUI(bool codeEditorFocused);

  // Call this before opening a new file (telling it that we closed the old one)
  void notifyDocumentClosed();

  // Call this if you know the name of the new file, after calling notifyDocumentClosed().
  void notifyDocumentName(const std::filesystem::path& path);

  struct Settings
  {
    float restartInterval{};
    bool  enabled{};
  };
  // Gets or sets the current language server settings.
  Settings& settings(Settings* newSettings = nullptr);

private:
  struct Implementation;
  Implementation* m_pImpl = nullptr;
};
