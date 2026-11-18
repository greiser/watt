// The JSON body's lexer, plugged into Scintilla as an ILexer5 (see http_editor_lexer.cpp).
#pragma once

namespace watt::app::http_editor {

// A new lexer instance for wxStyledTextCtrl::SetILexer (Scintilla owns it and releases it when
// the lexer is replaced). Styles with lex_json (LexJSON's style numbers, so the colours stay)
// and sets fold levels like LexJSON: an unquoted {{var}} no longer turns the rest of its line
// into an error, and Scintilla runs it inside its own paint / idle styling like a built-in lexer.
void* make_json_lexer();

}  // namespace watt::app::http_editor
