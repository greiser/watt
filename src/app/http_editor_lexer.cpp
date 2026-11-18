// The JSON body's lexer as a Scintilla ILexer5.
//
// Scintilla's LexJSON styles everything after an unquoted {{var}} up to the end of the line as an
// error. A container lexer (wxEVT_STC_STYLENEEDED) could fix that, but it has to set fold levels
// through the editor API, which repaints the whole fold margin per changed line (~13 us a line:
// seconds of idle styling for a 960k-line body). An ILexer5 sets them through IDocument, the way
// the built-in lexers do, and runs inside Scintilla's own paint / idle styling.
//
// IDocument / ILexer4 / ILexer5 below are declared exactly as in Scintilla's include/ILexer.h:
// that interface is Scintilla's stable ABI for external lexers (Lexilla), and wxWidgets' headers
// do not ship it.
#include "app/http_editor_lexer.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "app/http_editor_util.h"

namespace watt::app::http_editor {

namespace {

using Sci_Position = std::ptrdiff_t;
using Sci_PositionU = std::size_t;
#define WATT_SCI_METHOD __stdcall

class IDocument {
 public:
  virtual int WATT_SCI_METHOD Version() const = 0;
  virtual void WATT_SCI_METHOD SetErrorStatus(int status) = 0;
  virtual Sci_Position WATT_SCI_METHOD Length() const = 0;
  virtual void WATT_SCI_METHOD GetCharRange(char* buffer, Sci_Position position, Sci_Position lengthRetrieve) const = 0;
  virtual char WATT_SCI_METHOD StyleAt(Sci_Position position) const = 0;
  virtual Sci_Position WATT_SCI_METHOD LineFromPosition(Sci_Position position) const = 0;
  virtual Sci_Position WATT_SCI_METHOD LineStart(Sci_Position line) const = 0;
  virtual int WATT_SCI_METHOD GetLevel(Sci_Position line) const = 0;
  virtual int WATT_SCI_METHOD SetLevel(Sci_Position line, int level) = 0;
  virtual int WATT_SCI_METHOD GetLineState(Sci_Position line) const = 0;
  virtual int WATT_SCI_METHOD SetLineState(Sci_Position line, int state) = 0;
  virtual void WATT_SCI_METHOD StartStyling(Sci_Position position) = 0;
  virtual bool WATT_SCI_METHOD SetStyleFor(Sci_Position length, char style) = 0;
  virtual bool WATT_SCI_METHOD SetStyles(Sci_Position length, const char* styles) = 0;
  virtual void WATT_SCI_METHOD DecorationSetCurrentIndicator(int indicator) = 0;
  virtual void WATT_SCI_METHOD DecorationFillRange(Sci_Position position, int value, Sci_Position fillLength) = 0;
  virtual void WATT_SCI_METHOD ChangeLexerState(Sci_Position start, Sci_Position end) = 0;
  virtual int WATT_SCI_METHOD CodePage() const = 0;
  virtual bool WATT_SCI_METHOD IsDBCSLeadByte(char ch) const = 0;
  virtual const char* WATT_SCI_METHOD BufferPointer() = 0;
  virtual int WATT_SCI_METHOD GetLineIndentation(Sci_Position line) = 0;
  virtual Sci_Position WATT_SCI_METHOD LineEnd(Sci_Position line) const = 0;
  virtual Sci_Position WATT_SCI_METHOD GetRelativePosition(Sci_Position positionStart, Sci_Position characterOffset) const = 0;
  virtual int WATT_SCI_METHOD GetCharacterAndWidth(Sci_Position position, Sci_Position* pWidth) const = 0;
};

class ILexer4 {
 public:
  virtual int WATT_SCI_METHOD Version() const = 0;
  virtual void WATT_SCI_METHOD Release() = 0;
  virtual const char* WATT_SCI_METHOD PropertyNames() = 0;
  virtual int WATT_SCI_METHOD PropertyType(const char* name) = 0;
  virtual const char* WATT_SCI_METHOD DescribeProperty(const char* name) = 0;
  virtual Sci_Position WATT_SCI_METHOD PropertySet(const char* key, const char* val) = 0;
  virtual const char* WATT_SCI_METHOD DescribeWordListSets() = 0;
  virtual Sci_Position WATT_SCI_METHOD WordListSet(int n, const char* wl) = 0;
  virtual void WATT_SCI_METHOD Lex(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, IDocument* pAccess) = 0;
  virtual void WATT_SCI_METHOD Fold(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, IDocument* pAccess) = 0;
  virtual void* WATT_SCI_METHOD PrivateCall(int operation, void* pointer) = 0;
  virtual int WATT_SCI_METHOD LineEndTypesSupported() = 0;
  virtual int WATT_SCI_METHOD AllocateSubStyles(int styleBase, int numberStyles) = 0;
  virtual int WATT_SCI_METHOD SubStylesStart(int styleBase) = 0;
  virtual int WATT_SCI_METHOD SubStylesLength(int styleBase) = 0;
  virtual int WATT_SCI_METHOD StyleFromSubStyle(int subStyle) = 0;
  virtual int WATT_SCI_METHOD PrimaryStyleFromStyle(int style) = 0;
  virtual void WATT_SCI_METHOD FreeSubStyles() = 0;
  virtual void WATT_SCI_METHOD SetIdentifiers(int style, const char* identifiers) = 0;
  virtual int WATT_SCI_METHOD DistanceToSecondaryStyles() = 0;
  virtual const char* WATT_SCI_METHOD GetSubStyleBases() = 0;
  virtual int WATT_SCI_METHOD NamedStyles() = 0;
  virtual const char* WATT_SCI_METHOD NameOfStyle(int style) = 0;
  virtual const char* WATT_SCI_METHOD TagsOfStyle(int style) = 0;
  virtual const char* WATT_SCI_METHOD DescriptionOfStyle(int style) = 0;
};

class ILexer5 : public ILexer4 {
 public:
  virtual const char* WATT_SCI_METHOD GetName() = 0;
  virtual int WATT_SCI_METHOD GetIdentifier() = 0;
  virtual const char* WATT_SCI_METHOD PropertyGet(const char* key) = 0;
};

constexpr int kLexerRelease5 = 3;  // lvRelease5
constexpr int kFoldBase = 0x400, kFoldNumberMask = 0x0FFF, kFoldHeader = 0x2000;

class JsonLexer final : public ILexer5 {
 public:
  int WATT_SCI_METHOD Version() const override { return kLexerRelease5; }
  void WATT_SCI_METHOD Release() override { delete this; }
  const char* WATT_SCI_METHOD PropertyNames() override { return ""; }
  int WATT_SCI_METHOD PropertyType(const char*) override { return 0; }
  const char* WATT_SCI_METHOD DescribeProperty(const char*) override { return ""; }
  Sci_Position WATT_SCI_METHOD PropertySet(const char*, const char*) override { return -1; }
  const char* WATT_SCI_METHOD DescribeWordListSets() override { return ""; }
  Sci_Position WATT_SCI_METHOD WordListSet(int, const char*) override { return -1; }

  // Styles and folds in one pass, from the start of the line holding startPos (Scintilla always
  // starts there) to the end of the range; the depth at a line start is its fold level and the
  // "inside /* */" state its line state, so the next call picks up from there.
  void WATT_SCI_METHOD Lex(Sci_PositionU startPos, Sci_Position lengthDoc, int, IDocument* doc) override {
    const Sci_Position docLen = doc->Length();
    Sci_Position start = static_cast<Sci_Position>(startPos);
    Sci_Position end = std::min(docLen, start + lengthDoc);
    Sci_Position line = doc->LineFromPosition(start);
    start = doc->LineStart(line);
    if (end <= start) return;
    // Whole lines: the last one is read to its end (only [start, end) is styled).
    const Sci_Position readEnd = std::min(docLen, doc->LineStart(doc->LineFromPosition(end - 1) + 1));
    JsonLexState st;
    if (line > 0) {
      st.depth = std::max(0, (doc->GetLevel(line) & kFoldNumberMask) - kFoldBase);
      st.in_comment = (doc->GetLineState(line) & 1) != 0;
    }
    text_.resize(static_cast<size_t>(readEnd - start));
    doc->GetCharRange(text_.data(), start, readEnd - start);
    lines_.clear();
    lex_json(text_, st, &styles_, &lines_);
    doc->StartStyling(start);
    doc->SetStyles(end - start, styles_.data());
    for (const JsonLexLine& l : lines_) {
      put(doc, line, (kFoldBase + l.depth) | (l.header ? kFoldHeader : 0), l.in_comment);
      ++line;
    }
    // The state the next line starts in (its header flag stays until it is lexed itself).
    if (line < doc->LineFromPosition(docLen) + 1) {
      const int old = doc->GetLevel(line);
      const int level = kFoldBase + st.depth;
      put(doc, line, (old & kFoldNumberMask) == level ? old : level, st.in_comment);
    }
  }
  void WATT_SCI_METHOD Fold(Sci_PositionU, Sci_Position, int, IDocument*) override {}  // done in Lex
  void* WATT_SCI_METHOD PrivateCall(int, void*) override { return nullptr; }
  int WATT_SCI_METHOD LineEndTypesSupported() override { return 0; }
  int WATT_SCI_METHOD AllocateSubStyles(int, int) override { return -1; }
  int WATT_SCI_METHOD SubStylesStart(int) override { return -1; }
  int WATT_SCI_METHOD SubStylesLength(int) override { return 0; }
  int WATT_SCI_METHOD StyleFromSubStyle(int subStyle) override { return subStyle; }
  int WATT_SCI_METHOD PrimaryStyleFromStyle(int style) override { return style; }
  void WATT_SCI_METHOD FreeSubStyles() override {}
  void WATT_SCI_METHOD SetIdentifiers(int, const char*) override {}
  int WATT_SCI_METHOD DistanceToSecondaryStyles() override { return 0; }
  const char* WATT_SCI_METHOD GetSubStyleBases() override { return ""; }
  int WATT_SCI_METHOD NamedStyles() override { return 0; }
  const char* WATT_SCI_METHOD NameOfStyle(int) override { return ""; }
  const char* WATT_SCI_METHOD TagsOfStyle(int) override { return ""; }
  const char* WATT_SCI_METHOD DescriptionOfStyle(int) override { return ""; }
  const char* WATT_SCI_METHOD GetName() override { return "watt-json"; }
  int WATT_SCI_METHOD GetIdentifier() override { return 0; }
  const char* WATT_SCI_METHOD PropertyGet(const char*) override { return ""; }

 private:
  // Only what changed: each change makes Scintilla repaint part of the fold margin, and a line
  // state change repaints the whole editor (set only inside / right after a block comment).
  static void put(IDocument* doc, Sci_Position line, int level, bool comment) {
    if (doc->GetLevel(line) != level) doc->SetLevel(line, level);
    if (comment || doc->GetLineState(line) != 0) {
      if ((doc->GetLineState(line) & 1) != (comment ? 1 : 0)) doc->SetLineState(line, comment ? 1 : 0);
    }
  }

  std::string text_, styles_;
  std::vector<JsonLexLine> lines_;
};

#undef WATT_SCI_METHOD

}  // namespace

void* make_json_lexer() { return static_cast<ILexer5*>(new JsonLexer()); }

}  // namespace watt::app::http_editor
