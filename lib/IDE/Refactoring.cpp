//===--- Refactoring.cpp ---------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/IDE/Refactoring.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticsRefactoring.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Types.h"
#include "swift/AST/USRGeneration.h"
#include "swift/Basic/Edit.h"
#include "swift/Basic/StringExtras.h"
#include "swift/Frontend/Frontend.h"
#include "swift/IDE/IDERequests.h"
#include "swift/Index/Index.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/Parse/Lexer.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Subsystems.h"
#include "clang/Rewrite/Core/RewriteBuffer.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSet.h"

using namespace swift;
using namespace swift::ide;
using namespace swift::index;

namespace {

class ContextFinder : public SourceEntityWalker {
  SourceFile &SF;
  ASTContext &Ctx;
  SourceManager &SM;
  SourceRange Target;
  function_ref<bool(ASTNode)> IsContext;
  SmallVector<ASTNode, 4> AllContexts;
  bool contains(ASTNode Enclosing) {
    auto Result = SM.rangeContains(Enclosing.getSourceRange(), Target);
    if (Result && IsContext(Enclosing))
      AllContexts.push_back(Enclosing);
    return Result;
  }
public:
  ContextFinder(SourceFile &SF, ASTNode TargetNode,
                function_ref<bool(ASTNode)> IsContext =
                  [](ASTNode N) { return true; }) :
                  SF(SF), Ctx(SF.getASTContext()), SM(Ctx.SourceMgr),
                  Target(TargetNode.getSourceRange()), IsContext(IsContext) {}
  ContextFinder(SourceFile &SF, SourceLoc TargetLoc,
                function_ref<bool(ASTNode)> IsContext =
                  [](ASTNode N) { return true; }) :
                  SF(SF), Ctx(SF.getASTContext()), SM(Ctx.SourceMgr),
                  Target(TargetLoc), IsContext(IsContext) {
                    assert(TargetLoc.isValid() && "Invalid loc to find");
                  }
  bool walkToDeclPre(Decl *D, CharSourceRange Range) override { return contains(D); }
  bool walkToStmtPre(Stmt *S) override { return contains(S); }
  bool walkToExprPre(Expr *E) override { return contains(E); }
  void resolve() { walk(SF); }
  ArrayRef<ASTNode> getContexts() const {
    return llvm::makeArrayRef(AllContexts);
  }
};

class Renamer {
protected:
  const SourceManager &SM;

protected:
  Renamer(const SourceManager &SM, StringRef OldName) : SM(SM), Old(OldName) {}

  // Implementor's interface.
  virtual void doRenameLabel(CharSourceRange Label,
                             RefactoringRangeKind RangeKind,
                             unsigned NameIndex) = 0;
  virtual void doRenameBase(CharSourceRange Range,
                            RefactoringRangeKind RangeKind) = 0;

public:
  const DeclNameViewer Old;

public:
  virtual ~Renamer() {}

  /// Adds a replacement to rename the given base name range
  /// \return true if the given range does not match the old name
  bool renameBase(CharSourceRange Range, RefactoringRangeKind RangeKind) {
    assert(Range.isValid());

    if (stripBackticks(Range).str() != Old.base())
      return true;
    doRenameBase(Range, RangeKind);
    return false;
  }

  /// Adds replacements to rename the given label ranges
  /// \return true if the label ranges do not match the old name
  bool renameLabels(ArrayRef<CharSourceRange> LabelRanges,
                    Optional<unsigned> FirstTrailingLabel,
                    LabelRangeType RangeType, bool isCallSite) {
    if (isCallSite)
      return renameLabelsLenient(LabelRanges, FirstTrailingLabel, RangeType);

    assert(!FirstTrailingLabel);
    ArrayRef<StringRef> OldLabels = Old.args();

    if (OldLabels.size() != LabelRanges.size())
      return true;

    size_t Index = 0;
    for (const auto &LabelRange : LabelRanges) {
      assert(LabelRange.isValid());
      if (!labelRangeMatches(LabelRange, RangeType, OldLabels[Index]))
        return true;
      splitAndRenameLabel(LabelRange, RangeType, Index++);
    }
    return false;
  }

  bool isOperator() const { return Lexer::isOperator(Old.base()); }

private:

  /// Returns the range of the  (possibly escaped) identifier at the start of
  /// \p Range and updates \p IsEscaped to indicate whether it's escaped or not.
  CharSourceRange getLeadingIdentifierRange(CharSourceRange Range, bool &IsEscaped) {
    assert(Range.isValid() && Range.getByteLength());
    IsEscaped = Range.str().front() == '`';
    SourceLoc Start = Range.getStart();
    if (IsEscaped)
      Start = Start.getAdvancedLoc(1);
    return Lexer::getCharSourceRangeFromSourceRange(SM, Start);
  }

  CharSourceRange stripBackticks(CharSourceRange Range) {
    StringRef Content = Range.str();
    if (Content.size() < 3 || Content.front() != '`' || Content.back() != '`') {
      return Range;
    }
    return CharSourceRange(Range.getStart().getAdvancedLoc(1),
                           Range.getByteLength() - 2);
  }

  void splitAndRenameLabel(CharSourceRange Range, LabelRangeType RangeType,
                           size_t NameIndex) {
    switch (RangeType) {
    case LabelRangeType::CallArg:
      return splitAndRenameCallArg(Range, NameIndex);
    case LabelRangeType::Param:
      return splitAndRenameParamLabel(Range, NameIndex, /*IsCollapsible=*/true);
    case LabelRangeType::NoncollapsibleParam:
      return splitAndRenameParamLabel(Range, NameIndex, /*IsCollapsible=*/false);
    case LabelRangeType::Selector:
      return doRenameLabel(
          Range, RefactoringRangeKind::SelectorArgumentLabel, NameIndex);
    case LabelRangeType::None:
      llvm_unreachable("expected a label range");
    }
  }

  void splitAndRenameParamLabel(CharSourceRange Range, size_t NameIndex, bool IsCollapsible) {
    // Split parameter range foo([a b]: Int) into decl argument label [a] and
    // parameter name [b] or noncollapsible parameter name [b] if IsCollapsible
    // is false (as for subscript decls). If we have only foo([a]: Int), then we
    // add an empty range for the local name, or for the decl argument label if
    // IsCollapsible is false.
    StringRef Content = Range.str();
    size_t ExternalNameEnd = Content.find_first_of(" \t\n\v\f\r/");

    if (ExternalNameEnd == StringRef::npos) { // foo([a]: Int)
      if (IsCollapsible) {
        doRenameLabel(Range, RefactoringRangeKind::DeclArgumentLabel, NameIndex);
        doRenameLabel(CharSourceRange{Range.getEnd(), 0},
                      RefactoringRangeKind::ParameterName, NameIndex);
      } else {
        doRenameLabel(CharSourceRange{Range.getStart(), 0},
                      RefactoringRangeKind::DeclArgumentLabel, NameIndex);
        doRenameLabel(Range, RefactoringRangeKind::NoncollapsibleParameterName,
                      NameIndex);
      }
    } else { // foo([a b]: Int)
      CharSourceRange Ext{Range.getStart(), unsigned(ExternalNameEnd)};

      // Note: we consider the leading whitespace part of the parameter name
      // if the parameter is collapsible, since if the parameter is collapsed
      // into a matching argument label, we want to remove the whitespace too.
      // FIXME: handle comments foo(a /*...*/b: Int).
      size_t LocalNameStart = Content.find_last_of(" \t\n\v\f\r/");
      assert(LocalNameStart != StringRef::npos);
      if (!IsCollapsible)
        ++LocalNameStart;
      auto LocalLoc = Range.getStart().getAdvancedLocOrInvalid(LocalNameStart);
      CharSourceRange Local{LocalLoc, unsigned(Content.size() - LocalNameStart)};

      doRenameLabel(Ext, RefactoringRangeKind::DeclArgumentLabel, NameIndex);
      if (IsCollapsible) {
        doRenameLabel(Local, RefactoringRangeKind::ParameterName, NameIndex);
      } else {
        doRenameLabel(Local, RefactoringRangeKind::NoncollapsibleParameterName, NameIndex);
      }
    }
  }

  void splitAndRenameCallArg(CharSourceRange Range, size_t NameIndex) {
    // Split call argument foo([a: ]1) into argument name [a] and the remainder
    // [: ].
    StringRef Content = Range.str();
    size_t Colon = Content.find(':'); // FIXME: leading whitespace?
    if (Colon == StringRef::npos) {
      assert(Content.empty());
      doRenameLabel(Range, RefactoringRangeKind::CallArgumentCombined,
                    NameIndex);
      return;
    }

    // Include any whitespace before the ':'.
    assert(Colon == Content.substr(0, Colon).size());
    Colon = Content.substr(0, Colon).rtrim().size();

    CharSourceRange Arg{Range.getStart(), unsigned(Colon)};
    doRenameLabel(Arg, RefactoringRangeKind::CallArgumentLabel, NameIndex);

    auto ColonLoc = Range.getStart().getAdvancedLocOrInvalid(Colon);
    assert(ColonLoc.isValid());
    CharSourceRange Rest{ColonLoc, unsigned(Content.size() - Colon)};
    doRenameLabel(Rest, RefactoringRangeKind::CallArgumentColon, NameIndex);
  }

  bool labelRangeMatches(CharSourceRange Range, LabelRangeType RangeType, StringRef Expected) {
    if (Range.getByteLength()) {
      bool IsEscaped = false;
      CharSourceRange ExistingLabelRange = getLeadingIdentifierRange(Range, IsEscaped);
      StringRef ExistingLabel = ExistingLabelRange.str();
      bool IsSingleName = Range == ExistingLabelRange ||
        (IsEscaped && Range.getByteLength() == ExistingLabel.size() + 2);

      switch (RangeType) {
      case LabelRangeType::NoncollapsibleParam:
        if (IsSingleName && Expected.empty()) // subscript([x]: Int)
          return true;
        LLVM_FALLTHROUGH;
      case LabelRangeType::CallArg:
      case LabelRangeType::Param:
      case LabelRangeType::Selector:
        return ExistingLabel == (Expected.empty() ? "_" : Expected);
      case LabelRangeType::None:
        llvm_unreachable("Unhandled label range type");
      }
    }
    return Expected.empty();
  }

  bool renameLabelsLenient(ArrayRef<CharSourceRange> LabelRanges,
                           Optional<unsigned> FirstTrailingLabel,
                           LabelRangeType RangeType) {

    ArrayRef<StringRef> OldNames = Old.args();

    // First, match trailing closure arguments in reverse
    if (FirstTrailingLabel) {
      auto TrailingLabels = LabelRanges.drop_front(*FirstTrailingLabel);
      LabelRanges = LabelRanges.take_front(*FirstTrailingLabel);

      for (auto LabelIndex: llvm::reverse(indices(TrailingLabels))) {
        CharSourceRange Label = TrailingLabels[LabelIndex];

        if (Label.getByteLength()) {
          if (OldNames.empty())
            return true;

          while (!labelRangeMatches(Label, LabelRangeType::Selector,
                                    OldNames.back())) {
            if ((OldNames = OldNames.drop_back()).empty())
              return true;
          }
          splitAndRenameLabel(Label, LabelRangeType::Selector,
                              OldNames.size() - 1);
          OldNames = OldNames.drop_back();
          continue;
        }

        // empty labelled trailing closure label
        if (LabelIndex) {
          if (OldNames.empty())
            return true;

          while (!OldNames.back().empty()) {
            if ((OldNames = OldNames.drop_back()).empty())
              return true;
          }
          splitAndRenameLabel(Label, LabelRangeType::Selector,
                              OldNames.size() - 1);
          OldNames = OldNames.drop_back();
          continue;
        }

        // unlabelled trailing closure label
        OldNames = OldNames.drop_back();
        continue;
      }
    }

    // Next, match the non-trailing arguments.
    size_t NameIndex = 0;

    for (CharSourceRange Label : LabelRanges) {
      // empty label
      if (!Label.getByteLength()) {

        // first name pos
        if (!NameIndex) {
          while (!OldNames[NameIndex].empty()) {
            if (++NameIndex >= OldNames.size())
              return true;
          }
          splitAndRenameLabel(Label, RangeType, NameIndex++);
          continue;
        }

        // other name pos
        if (NameIndex >= OldNames.size() || !OldNames[NameIndex].empty()) {
          // FIXME: only allow one variadic param
          continue; // allow for variadic
        }
        splitAndRenameLabel(Label, RangeType, NameIndex++);
        continue;
      }

      // non-empty label
      if (NameIndex >= OldNames.size())
        return true;

      while (!labelRangeMatches(Label, RangeType, OldNames[NameIndex])) {
        if (++NameIndex >= OldNames.size())
          return true;
      };
      splitAndRenameLabel(Label, RangeType, NameIndex++);
    }
    return false;
  }

  static RegionType getSyntacticRenameRegionType(const ResolvedLoc &Resolved) {
    if (Resolved.Node.isNull())
      return RegionType::Comment;

    if (Expr *E = Resolved.Node.getAsExpr()) {
      if (isa<StringLiteralExpr>(E))
        return RegionType::String;
    }
    if (Resolved.IsInSelector)
      return RegionType::Selector;
    if (Resolved.IsActive)
      return RegionType::ActiveCode;
    return RegionType::InactiveCode;
  }

public:
  RegionType addSyntacticRenameRanges(const ResolvedLoc &Resolved,
                                      const RenameLoc &Config) {

    if (!Resolved.Range.isValid())
      return RegionType::Unmatched;

    auto RegionKind = getSyntacticRenameRegionType(Resolved);
    // Don't include unknown references coming from active code; if we don't
    // have a semantic NameUsage for them, then they're likely unrelated symbols
    // that happen to have the same name.
    if (RegionKind == RegionType::ActiveCode &&
        Config.Usage == NameUsage::Unknown)
      return RegionType::Unmatched;

    assert(Config.Usage != NameUsage::Call || Config.IsFunctionLike);

    // FIXME: handle escaped keyword names `init`
    bool IsSubscript = Old.base() == "subscript" && Config.IsFunctionLike;
    bool IsInit = Old.base() == "init" && Config.IsFunctionLike;

    // FIXME: this should only be treated specially for instance methods.
    bool IsCallAsFunction = Old.base() == "callAsFunction" &&
        Config.IsFunctionLike;

    bool IsSpecialBase = IsInit || IsSubscript || IsCallAsFunction;
    
    // Filter out non-semantic special basename locations with no labels.
    // We've already filtered out those in active code, so these are
    // any appearance of just 'init', 'subscript', or 'callAsFunction' in
    // strings, comments, and inactive code.
    if (IsSpecialBase && (Config.Usage == NameUsage::Unknown &&
                          Resolved.LabelType == LabelRangeType::None))
      return RegionType::Unmatched;

    if (!Config.IsFunctionLike || !IsSpecialBase) {
      if (renameBase(Resolved.Range, RefactoringRangeKind::BaseName))
        return RegionType::Mismatch;

    } else if (IsInit || IsCallAsFunction) {
      if (renameBase(Resolved.Range, RefactoringRangeKind::KeywordBaseName)) {
        // The base name doesn't need to match (but may) for calls, but
        // it should for definitions and references.
        if (Config.Usage == NameUsage::Definition ||
            Config.Usage == NameUsage::Reference) {
          return RegionType::Mismatch;
        }
      }
    } else if (IsSubscript && Config.Usage == NameUsage::Definition) {
      if (renameBase(Resolved.Range, RefactoringRangeKind::KeywordBaseName))
        return RegionType::Mismatch;
    }

    bool HandleLabels = false;
    if (Config.IsFunctionLike) {
      switch (Config.Usage) {
      case NameUsage::Call:
        HandleLabels = !isOperator();
        break;
      case NameUsage::Definition:
        HandleLabels = true;
        break;
      case NameUsage::Reference:
        HandleLabels = Resolved.LabelType == LabelRangeType::Selector || IsSubscript;
        break;
      case NameUsage::Unknown:
        HandleLabels = Resolved.LabelType != LabelRangeType::None;
        break;
      }
    } else if (Resolved.LabelType != LabelRangeType::None &&
               !Config.IsNonProtocolType &&
               // FIXME: Workaround for enum case labels until we support them
               Config.Usage != NameUsage::Definition) {
      return RegionType::Mismatch;
    }

    if (HandleLabels) {
      bool isCallSite = Config.Usage != NameUsage::Definition &&
                        (Config.Usage != NameUsage::Reference || IsSubscript) &&
                        Resolved.LabelType == LabelRangeType::CallArg;

      if (renameLabels(Resolved.LabelRanges, Resolved.FirstTrailingLabel,
                       Resolved.LabelType, isCallSite))
        return Config.Usage == NameUsage::Unknown ?
            RegionType::Unmatched : RegionType::Mismatch;
    }

    return RegionKind;
  }
};

class RenameRangeDetailCollector : public Renamer {
  void doRenameLabel(CharSourceRange Label, RefactoringRangeKind RangeKind,
                     unsigned NameIndex) override {
    Ranges.push_back({Label, RangeKind, NameIndex});
  }
  void doRenameBase(CharSourceRange Range,
                    RefactoringRangeKind RangeKind) override {
    Ranges.push_back({Range, RangeKind, None});
  }

public:
  RenameRangeDetailCollector(const SourceManager &SM, StringRef OldName)
      : Renamer(SM, OldName) {}
  std::vector<RenameRangeDetail> Ranges;
};

class TextReplacementsRenamer : public Renamer {
  llvm::StringSet<> &ReplaceTextContext;
  std::vector<Replacement> Replacements;

public:
  const DeclNameViewer New;

private:
  StringRef registerText(StringRef Text) {
    if (Text.empty())
      return Text;
    return ReplaceTextContext.insert(Text).first->getKey();
  }

  StringRef getCallArgLabelReplacement(StringRef OldLabelRange,
                                       StringRef NewLabel) {
    return NewLabel.empty() ? "" : NewLabel;
  }

  StringRef getCallArgColonReplacement(StringRef OldLabelRange,
                                       StringRef NewLabel) {
    // Expected OldLabelRange: foo( []3, a[: ]2,  b[ : ]3 ...)
    // FIXME: Preserve comments: foo([a/*:*/ : /*:*/ ]2, ...)
    if (NewLabel.empty())
      return "";
    if (OldLabelRange.empty())
      return ": ";
    return registerText(OldLabelRange);
  }

  StringRef getCallArgCombinedReplacement(StringRef OldArgLabel,
                                          StringRef NewArgLabel) {
    // This case only happens when going from foo([]1) to foo([a: ]1).
    assert(OldArgLabel.empty());
    if (NewArgLabel.empty())
      return "";
    return registerText((Twine(NewArgLabel) + ": ").str());
  }

  StringRef getParamNameReplacement(StringRef OldParam, StringRef OldArgLabel,
                                    StringRef NewArgLabel) {
    // We don't want to get foo(a a: Int), so drop the parameter name if the
    // argument label will match the original name.
    // Note: the leading whitespace is part of the parameter range.
    if (!NewArgLabel.empty() && OldParam.ltrim() == NewArgLabel)
      return "";

    // If we're renaming foo(x: Int) to foo(_:), then use the original argument
    // label as the parameter name so as to not break references in the body.
    if (NewArgLabel.empty() && !OldArgLabel.empty() && OldParam.empty())
      return registerText((Twine(" ") + OldArgLabel).str());

    return registerText(OldParam);
  }

  StringRef getDeclArgumentLabelReplacement(StringRef OldLabelRange,
                                            StringRef NewArgLabel) {
      // OldLabelRange is subscript([]a: Int), foo([a]: Int) or foo([a] b: Int)
      if (NewArgLabel.empty())
        return OldLabelRange.empty() ? "" : "_";

      if (OldLabelRange.empty())
        return registerText((Twine(NewArgLabel) + " ").str());
      return registerText(NewArgLabel);
  }

  StringRef getReplacementText(StringRef LabelRange,
                               RefactoringRangeKind RangeKind,
                               StringRef OldLabel, StringRef NewLabel) {
    switch (RangeKind) {
    case RefactoringRangeKind::CallArgumentLabel:
      return getCallArgLabelReplacement(LabelRange, NewLabel);
    case RefactoringRangeKind::CallArgumentColon:
      return getCallArgColonReplacement(LabelRange, NewLabel);
    case RefactoringRangeKind::CallArgumentCombined:
      return getCallArgCombinedReplacement(LabelRange, NewLabel);
    case RefactoringRangeKind::ParameterName:
      return getParamNameReplacement(LabelRange, OldLabel, NewLabel);
    case RefactoringRangeKind::NoncollapsibleParameterName:
      return LabelRange;
    case RefactoringRangeKind::DeclArgumentLabel:
      return getDeclArgumentLabelReplacement(LabelRange, NewLabel);
    case RefactoringRangeKind::SelectorArgumentLabel:
      return NewLabel.empty() ? "_" : registerText(NewLabel);
    default:
      llvm_unreachable("label range type is none but there are labels");
    }
  }

  void addReplacement(CharSourceRange LabelRange,
                      RefactoringRangeKind RangeKind, StringRef OldLabel,
                      StringRef NewLabel) {
    StringRef ExistingLabel = LabelRange.str();
    StringRef Text =
        getReplacementText(ExistingLabel, RangeKind, OldLabel, NewLabel);
    if (Text != ExistingLabel)
      Replacements.push_back({LabelRange, Text, {}});
  }

  void doRenameLabel(CharSourceRange Label, RefactoringRangeKind RangeKind,
                     unsigned NameIndex) override {
    addReplacement(Label, RangeKind, Old.args()[NameIndex],
                   New.args()[NameIndex]);
  }

  void doRenameBase(CharSourceRange Range, RefactoringRangeKind) override {
    if (Old.base() != New.base())
      Replacements.push_back({Range, registerText(New.base()), {}});
  }

public:
  TextReplacementsRenamer(const SourceManager &SM, StringRef OldName,
                          StringRef NewName,
                          llvm::StringSet<> &ReplaceTextContext)
      : Renamer(SM, OldName), ReplaceTextContext(ReplaceTextContext),
        New(NewName) {
    assert(Old.isValid() && New.isValid());
    assert(Old.partsCount() == New.partsCount());
  }

  std::vector<Replacement> getReplacements() const {
    return std::move(Replacements);
  }
};

static const ValueDecl *getRelatedSystemDecl(const ValueDecl *VD) {
  if (VD->getModuleContext()->isSystemModule())
    return VD;
  for (auto *Req : VD->getSatisfiedProtocolRequirements()) {
    if (Req->getModuleContext()->isSystemModule())
      return Req;
  }
  for (auto Over = VD->getOverriddenDecl(); Over;
       Over = Over->getOverriddenDecl()) {
    if (Over->getModuleContext()->isSystemModule())
      return Over;
  }
  return nullptr;
}

static Optional<RefactoringKind>
getAvailableRenameForDecl(const ValueDecl *VD,
                          Optional<RenameRefInfo> RefInfo) {
  SmallVector<RenameAvailabilityInfo, 2> Infos;
  collectRenameAvailabilityInfo(VD, RefInfo, Infos);
  for (auto &Info : Infos) {
    if (Info.AvailableKind == RenameAvailableKind::Available)
      return Info.Kind;
  }
  return None;
}

class RenameRangeCollector : public IndexDataConsumer {
public:
  RenameRangeCollector(StringRef USR, StringRef newName)
      : USR(USR.str()), newName(newName.str()) {}

  RenameRangeCollector(const ValueDecl *D, StringRef newName)
      : newName(newName.str()) {
    llvm::raw_string_ostream OS(USR);
    printValueDeclUSR(D, OS);
  }

  ArrayRef<RenameLoc> results() const { return locations; }

private:
  bool indexLocals() override { return true; }
  void failed(StringRef error) override {}
  bool startDependency(StringRef name, StringRef path, bool isClangModule, bool isSystem) override {
    return true;
  }
  bool finishDependency(bool isClangModule) override { return true; }

  Action startSourceEntity(const IndexSymbol &symbol) override {
    if (symbol.USR == USR) {
      if (auto loc = indexSymbolToRenameLoc(symbol, newName)) {
        locations.push_back(std::move(*loc));
      }
    }
    return IndexDataConsumer::Continue;
  }

  bool finishSourceEntity(SymbolInfo symInfo, SymbolRoleSet roles) override {
    return true;
  }

  Optional<RenameLoc> indexSymbolToRenameLoc(const index::IndexSymbol &symbol,
                                             StringRef NewName);

private:
  std::string USR;
  std::string newName;
  StringScratchSpace stringStorage;
  std::vector<RenameLoc> locations;
};

Optional<RenameLoc>
RenameRangeCollector::indexSymbolToRenameLoc(const index::IndexSymbol &symbol,
                                             StringRef newName) {
  if (symbol.roles & (unsigned)index::SymbolRole::Implicit) {
    return None;
  }

  NameUsage usage = NameUsage::Unknown;
  if (symbol.roles & (unsigned)index::SymbolRole::Call) {
    usage = NameUsage::Call;
  } else if (symbol.roles & (unsigned)index::SymbolRole::Definition) {
    usage = NameUsage::Definition;
  } else if (symbol.roles & (unsigned)index::SymbolRole::Reference) {
    usage = NameUsage::Reference;
  } else {
    llvm_unreachable("unexpected role");
  }

  bool isFunctionLike = false;
  bool isNonProtocolType = false;

  switch (symbol.symInfo.Kind) {
  case index::SymbolKind::EnumConstant:
  case index::SymbolKind::Function:
  case index::SymbolKind::Constructor:
  case index::SymbolKind::ConversionFunction:
  case index::SymbolKind::InstanceMethod:
  case index::SymbolKind::ClassMethod:
  case index::SymbolKind::StaticMethod:
    isFunctionLike = true;
    break;
  case index::SymbolKind::Class:
  case index::SymbolKind::Enum:
  case index::SymbolKind::Struct:
    isNonProtocolType = true;
    break;
  default:
    break;
  }
  StringRef oldName = stringStorage.copyString(symbol.name);
  return RenameLoc{symbol.line,    symbol.column,    usage, oldName, newName,
                   isFunctionLike, isNonProtocolType};
}

ArrayRef<SourceFile*>
collectSourceFiles(ModuleDecl *MD, SmallVectorImpl<SourceFile *> &Scratch) {
  for (auto Unit : MD->getFiles()) {
    if (auto SF = dyn_cast<SourceFile>(Unit)) {
      Scratch.push_back(SF);
    }
  }
  return llvm::makeArrayRef(Scratch);
}

/// Get the source file that contains the given range and belongs to the module.
SourceFile *getContainingFile(ModuleDecl *M, RangeConfig Range) {
  SmallVector<SourceFile*, 4> Files;
  for (auto File : collectSourceFiles(M, Files)) {
    if (File->getBufferID()) {
      if (File->getBufferID().getValue() == Range.BufferId) {
        return File;
      }
    }
  }
  return nullptr;
}

class RefactoringAction {
protected:
  ModuleDecl *MD;
  SourceFile *TheFile;
  SourceEditConsumer &EditConsumer;
  ASTContext &Ctx;
  SourceManager &SM;
  DiagnosticEngine DiagEngine;
  SourceLoc StartLoc;
  StringRef PreferredName;
public:
  RefactoringAction(ModuleDecl *MD, RefactoringOptions &Opts,
                    SourceEditConsumer &EditConsumer,
                    DiagnosticConsumer &DiagConsumer);
  virtual ~RefactoringAction() = default;
  virtual bool performChange() = 0;
};

RefactoringAction::
RefactoringAction(ModuleDecl *MD, RefactoringOptions &Opts,
                  SourceEditConsumer &EditConsumer,
                  DiagnosticConsumer &DiagConsumer): MD(MD),
    TheFile(getContainingFile(MD, Opts.Range)),
    EditConsumer(EditConsumer), Ctx(MD->getASTContext()),
    SM(MD->getASTContext().SourceMgr), DiagEngine(SM),
    StartLoc(Lexer::getLocForStartOfToken(SM, Opts.Range.getStart(SM))),
    PreferredName(Opts.PreferredName) {
  DiagEngine.addConsumer(DiagConsumer);
}

/// Different from RangeBasedRefactoringAction, TokenBasedRefactoringAction takes
/// the input of a given token, e.g., a name or an "if" key word. Contextual
/// refactoring kinds can suggest applicable refactorings on that token, e.g.
/// rename or reverse if statement.
class TokenBasedRefactoringAction : public RefactoringAction {
protected:
  ResolvedCursorInfo CursorInfo;
public:
  TokenBasedRefactoringAction(ModuleDecl *MD, RefactoringOptions &Opts,
                              SourceEditConsumer &EditConsumer,
                              DiagnosticConsumer &DiagConsumer) :
  RefactoringAction(MD, Opts, EditConsumer, DiagConsumer) {
  // Resolve the sema token and save it for later use.
  CursorInfo = evaluateOrDefault(TheFile->getASTContext().evaluator,
                          CursorInfoRequest{ CursorInfoOwner(TheFile, StartLoc)},
                                 ResolvedCursorInfo());
  }
};

#define CURSOR_REFACTORING(KIND, NAME, ID)                                    \
class RefactoringAction##KIND: public TokenBasedRefactoringAction {           \
  public:                                                                     \
  RefactoringAction##KIND(ModuleDecl *MD, RefactoringOptions &Opts,           \
                          SourceEditConsumer &EditConsumer,                   \
                          DiagnosticConsumer &DiagConsumer) :                 \
    TokenBasedRefactoringAction(MD, Opts, EditConsumer, DiagConsumer) {}      \
  bool performChange() override;                                              \
  static bool isApplicable(const ResolvedCursorInfo &Info,                    \
                           DiagnosticEngine &Diag);                           \
  bool isApplicable() {                                                       \
    return RefactoringAction##KIND::isApplicable(CursorInfo, DiagEngine) ;    \
  }                                                                           \
};
#include "swift/IDE/RefactoringKinds.def"

class RangeBasedRefactoringAction : public RefactoringAction {
protected:
  ResolvedRangeInfo RangeInfo;
public:
  RangeBasedRefactoringAction(ModuleDecl *MD, RefactoringOptions &Opts,
                              SourceEditConsumer &EditConsumer,
                              DiagnosticConsumer &DiagConsumer) :
  RefactoringAction(MD, Opts, EditConsumer, DiagConsumer),
  RangeInfo(evaluateOrDefault(MD->getASTContext().evaluator,
    RangeInfoRequest(RangeInfoOwner(TheFile, Opts.Range.getStart(SM), Opts.Range.getEnd(SM))),
                              ResolvedRangeInfo())) {}
};

#define RANGE_REFACTORING(KIND, NAME, ID)                                     \
class RefactoringAction##KIND: public RangeBasedRefactoringAction {           \
  public:                                                                     \
  RefactoringAction##KIND(ModuleDecl *MD, RefactoringOptions &Opts,           \
                          SourceEditConsumer &EditConsumer,                   \
                          DiagnosticConsumer &DiagConsumer) :                 \
    RangeBasedRefactoringAction(MD, Opts, EditConsumer, DiagConsumer) {}      \
  bool performChange() override;                                              \
  static bool isApplicable(const ResolvedRangeInfo &Info,                     \
                           DiagnosticEngine &Diag);                           \
  bool isApplicable() {                                                       \
    return RefactoringAction##KIND::isApplicable(RangeInfo, DiagEngine) ;     \
  }                                                                           \
};
#include "swift/IDE/RefactoringKinds.def"

bool RefactoringActionLocalRename::
isApplicable(const ResolvedCursorInfo &CursorInfo, DiagnosticEngine &Diag) {
  if (CursorInfo.Kind != CursorInfoKind::ValueRef)
    return false;

  Optional<RenameRefInfo> RefInfo;
  if (CursorInfo.IsRef)
    RefInfo = {CursorInfo.SF, CursorInfo.Loc, CursorInfo.IsKeywordArgument};

  auto RenameOp = getAvailableRenameForDecl(CursorInfo.ValueD, RefInfo);
  return RenameOp.hasValue() &&
    RenameOp.getValue() == RefactoringKind::LocalRename;
}

static void analyzeRenameScope(ValueDecl *VD, Optional<RenameRefInfo> RefInfo,
                               DiagnosticEngine &Diags,
                               SmallVectorImpl<DeclContext *> &Scopes) {
  Scopes.clear();
  if (!getAvailableRenameForDecl(VD, RefInfo).hasValue()) {
    Diags.diagnose(SourceLoc(), diag::value_decl_no_loc, VD->getName());
    return;
  }

  auto *Scope = VD->getDeclContext();
  // If the context is a top-level code decl, there may be other sibling
  // decls that the renamed symbol is visible from
  if (isa<TopLevelCodeDecl>(Scope))
    Scope = Scope->getParent();

  Scopes.push_back(Scope);
}

bool RefactoringActionLocalRename::performChange() {
  if (StartLoc.isInvalid()) {
    DiagEngine.diagnose(SourceLoc(), diag::invalid_location);
    return true;
  }
  if (!DeclNameViewer(PreferredName).isValid()) {
    DiagEngine.diagnose(SourceLoc(), diag::invalid_name, PreferredName);
    return true;
  }
  if (!TheFile) {
    DiagEngine.diagnose(StartLoc, diag::location_module_mismatch,
                        MD->getNameStr());
    return true;
  }
  CursorInfo = evaluateOrDefault(TheFile->getASTContext().evaluator,
                          CursorInfoRequest{CursorInfoOwner(TheFile, StartLoc)},
                                 ResolvedCursorInfo());
  if (CursorInfo.isValid() && CursorInfo.ValueD) {
    ValueDecl *VD = CursorInfo.typeOrValue();
    SmallVector<DeclContext *, 8> Scopes;

    Optional<RenameRefInfo> RefInfo;
    if (CursorInfo.IsRef)
      RefInfo = {CursorInfo.SF, CursorInfo.Loc, CursorInfo.IsKeywordArgument};

    analyzeRenameScope(VD, RefInfo, DiagEngine, Scopes);
    if (Scopes.empty())
      return true;
    RenameRangeCollector rangeCollector(VD, PreferredName);
    for (DeclContext *DC : Scopes)
      indexDeclContext(DC, rangeCollector);

    auto consumers = DiagEngine.takeConsumers();
    assert(consumers.size() == 1);
    return syntacticRename(TheFile, rangeCollector.results(), EditConsumer,
                           *consumers[0]);
  } else {
    DiagEngine.diagnose(StartLoc, diag::unresolved_location);
    return true;
  }
}

StringRef getDefaultPreferredName(RefactoringKind Kind) {
  switch(Kind) {
    case RefactoringKind::None:
      llvm_unreachable("Should be a valid refactoring kind");
    case RefactoringKind::GlobalRename:
    case RefactoringKind::LocalRename:
      return "newName";
    case RefactoringKind::ExtractExpr:
    case RefactoringKind::ExtractRepeatedExpr:
      return "extractedExpr";
    case RefactoringKind::ExtractFunction:
      return "extractedFunc";
    default:
      return "";
  }
}

enum class CannotExtractReason {
  Literal,
  VoidType,
};

class ExtractCheckResult {
  bool KnownFailure;
  SmallVector<CannotExtractReason, 2> AllReasons;

public:
  ExtractCheckResult(): KnownFailure(true) {}
  ExtractCheckResult(ArrayRef<CannotExtractReason> AllReasons):
    KnownFailure(false), AllReasons(AllReasons.begin(), AllReasons.end()) {}
  bool success() { return success({}); }
  bool success(ArrayRef<CannotExtractReason> ExpectedReasons) {
    if (KnownFailure)
      return false;
    bool Result = true;

    // Check if any reasons aren't covered by the list of expected reasons
    // provided by the client.
    for (auto R: AllReasons) {
      Result &= llvm::is_contained(ExpectedReasons, R);
    }
    return Result;
  }
};

/// Check whether a given range can be extracted.
/// Return true on successful condition checking,.
/// Return false on failed conditions.
ExtractCheckResult checkExtractConditions(const ResolvedRangeInfo &RangeInfo,
                                          DiagnosticEngine &DiagEngine) {
  SmallVector<CannotExtractReason, 2> AllReasons;
  // If any declared declaration is refered out of the given range, return false.
  auto Declared = RangeInfo.DeclaredDecls;
  auto It = std::find_if(Declared.begin(), Declared.end(),
                         [](DeclaredDecl DD) { return DD.ReferredAfterRange; });
  if (It != Declared.end()) {
    DiagEngine.diagnose(It->VD->getLoc(),
                        diag::value_decl_referenced_out_of_range,
                        It->VD->getName());
    return ExtractCheckResult();
  }

  // We cannot extract a range with multi entry points.
  if (!RangeInfo.HasSingleEntry) {
    DiagEngine.diagnose(SourceLoc(), diag::multi_entry_range);
    return ExtractCheckResult();
  }

  // We cannot extract code that is not sure to exit or not.
  if (RangeInfo.exit() == ExitState::Unsure) {
    return ExtractCheckResult();
  }

  // We cannot extract expressions of l-value type.
  if (auto Ty = RangeInfo.getType()) {
    if (Ty->hasLValueType() || Ty->is<InOutType>())
      return ExtractCheckResult();

    // Disallow extracting error type expressions/statements
    // FIXME: diagnose what happened?
    if (Ty->hasError())
      return ExtractCheckResult();

    if (Ty->isVoid()) {
      AllReasons.emplace_back(CannotExtractReason::VoidType);
    }
  }

  // We cannot extract a range with orphaned loop keyword.
  switch (RangeInfo.Orphan) {
  case swift::ide::OrphanKind::Continue:
    DiagEngine.diagnose(SourceLoc(), diag::orphan_loop_keyword, "continue");
    return ExtractCheckResult();
  case swift::ide::OrphanKind::Break:
    DiagEngine.diagnose(SourceLoc(), diag::orphan_loop_keyword, "break");
    return ExtractCheckResult();
  case swift::ide::OrphanKind::None:
    break;
  }

  // Guard statement can not be extracted.
  if (llvm::any_of(RangeInfo.ContainedNodes,
                  [](ASTNode N) { return N.isStmt(StmtKind::Guard); })) {
    return ExtractCheckResult();
  }

  // Disallow extracting certain kinds of statements.
  if (RangeInfo.Kind == RangeKind::SingleStatement) {
    Stmt *S = RangeInfo.ContainedNodes[0].get<Stmt *>();

    // These aren't independent statement.
    if (isa<BraceStmt>(S) || isa<CaseStmt>(S))
      return ExtractCheckResult();
  }

  // Disallow extracting literals.
  if (RangeInfo.Kind == RangeKind::SingleExpression) {
    Expr *E = RangeInfo.ContainedNodes[0].get<Expr*>();

    // Until implementing the performChange() part of extracting trailing
    // closures, we disable them for now.
    if (isa<AbstractClosureExpr>(E))
      return ExtractCheckResult();

    if (isa<LiteralExpr>(E))
      AllReasons.emplace_back(CannotExtractReason::Literal);
  }

  switch (RangeInfo.RangeContext->getContextKind()) {
  case swift::DeclContextKind::Initializer:
  case swift::DeclContextKind::SubscriptDecl:
  case swift::DeclContextKind::EnumElementDecl:
  case swift::DeclContextKind::AbstractFunctionDecl:
  case swift::DeclContextKind::AbstractClosureExpr:
  case swift::DeclContextKind::TopLevelCodeDecl:
    break;

  case swift::DeclContextKind::SerializedLocal:
  case swift::DeclContextKind::Module:
  case swift::DeclContextKind::FileUnit:
  case swift::DeclContextKind::GenericTypeDecl:
  case swift::DeclContextKind::ExtensionDecl:
    return ExtractCheckResult();
  }
  return ExtractCheckResult(AllReasons);
}

bool RefactoringActionExtractFunction::
isApplicable(const ResolvedRangeInfo &Info, DiagnosticEngine &Diag) {
  switch (Info.Kind) {
  case RangeKind::PartOfExpression:
  case RangeKind::SingleDecl:
  case RangeKind::MultiTypeMemberDecl:
  case RangeKind::Invalid:
    return false;
  case RangeKind::SingleExpression:
  case RangeKind::SingleStatement:
  case RangeKind::MultiStatement: {
    return checkExtractConditions(Info, Diag).
      success({CannotExtractReason::VoidType});
  }
  }
  llvm_unreachable("unhandled kind");
}

static StringRef correctNameInternal(ASTContext &Ctx, StringRef Name,
                                     ArrayRef<ValueDecl*> AllVisibles) {
  // If we find the collision.
  bool FoundCollision = false;

  // The suffixes we cannot use by appending to the original given name.
  llvm::StringSet<> UsedSuffixes;
  for (auto VD : AllVisibles) {
    StringRef S = VD->getBaseName().userFacingName();
    if (!S.startswith(Name))
      continue;
    StringRef Suffix = S.substr(Name.size());
    if (Suffix.empty())
      FoundCollision = true;
    else
      UsedSuffixes.insert(Suffix);
  }
  if (!FoundCollision)
    return Name;

  // Find the first suffix we can use.
  std::string SuffixToUse;
  for (unsigned I = 1; ; I ++) {
    SuffixToUse = std::to_string(I);
    if (UsedSuffixes.count(SuffixToUse) == 0)
      break;
  }
  return Ctx.getIdentifier((llvm::Twine(Name) + SuffixToUse).str()).str();
}

static StringRef correctNewDeclName(DeclContext *DC, StringRef Name) {

  // Collect all visible decls in the decl context.
  llvm::SmallVector<ValueDecl*, 16> AllVisibles;
  VectorDeclConsumer Consumer(AllVisibles);
  ASTContext &Ctx = DC->getASTContext();
  lookupVisibleDecls(Consumer, DC, true);
  return correctNameInternal(Ctx, Name, AllVisibles);
}

static Type sanitizeType(Type Ty) {
  // Transform lvalue type to inout type so that we can print it properly.
  return Ty.transform([](Type Ty) {
    if (Ty->is<LValueType>()) {
      return Type(InOutType::get(Ty->getRValueType()->getCanonicalType()));
    }
    return Ty;
  });
}

static SourceLoc
getNewFuncInsertLoc(DeclContext *DC, DeclContext*& InsertToContext) {
  if (auto D = DC->getInnermostDeclarationDeclContext()) {

    // If extracting from a getter/setter, we should skip both the immediate
    // getter/setter function and the individual var decl. The pattern binding
    // decl is the position before which we should insert the newly extracted
    // function.
    if (auto *FD = dyn_cast<AccessorDecl>(D)) {
      ValueDecl *SD = FD->getStorage();
      switch (SD->getKind()) {
      case DeclKind::Var:
        if (auto *PBD = cast<VarDecl>(SD)->getParentPatternBinding())
          D = PBD;
        break;
      case DeclKind::Subscript:
        D = SD;
        break;
      default:
        break;
      }
    }

    auto Result = D->getStartLoc();
    assert(Result.isValid());

    // The insert loc should be before every decl attributes.
    for (auto Attr : D->getAttrs()) {
      auto Loc = Attr->getRangeWithAt().Start;
      if (Loc.isValid() &&
          Loc.getOpaquePointerValue() < Result.getOpaquePointerValue())
        Result = Loc;
    }

    // The insert loc should be before the doc comments associated with this decl.
    if (!D->getRawComment().Comments.empty()) {
      auto Loc = D->getRawComment().Comments.front().Range.getStart();
      if (Loc.isValid() &&
          Loc.getOpaquePointerValue() < Result.getOpaquePointerValue()) {
        Result = Loc;
      }
    }
    InsertToContext = D->getDeclContext();
    return Result;
  }
  return SourceLoc();
}

static std::vector<NoteRegion>
getNotableRegions(StringRef SourceText, unsigned NameOffset, StringRef Name,
                    bool IsFunctionLike = false, bool IsNonProtocolType = false) {
  auto InputBuffer = llvm::MemoryBuffer::getMemBufferCopy(SourceText,"<extract>");

  CompilerInvocation Invocation{};

  Invocation.getFrontendOptions().InputsAndOutputs.addInput(
      InputFile("<extract>", true, InputBuffer.get(), file_types::TY_Swift));
  Invocation.getFrontendOptions().ModuleName = "extract";
  Invocation.getLangOptions().DisablePoundIfEvaluation = true;

  auto Instance = std::make_unique<swift::CompilerInstance>();
  if (Instance->setup(Invocation))
    llvm_unreachable("Failed setup");

  unsigned BufferId = Instance->getPrimarySourceFile()->getBufferID().getValue();
  SourceManager &SM = Instance->getSourceMgr();
  SourceLoc NameLoc = SM.getLocForOffset(BufferId, NameOffset);
  auto LineAndCol = SM.getLineAndColumnInBuffer(NameLoc);

  UnresolvedLoc UnresoledName{NameLoc, true};

  NameMatcher Matcher(*Instance->getPrimarySourceFile());
  auto Resolved = Matcher.resolve(llvm::makeArrayRef(UnresoledName), None);
  assert(!Resolved.empty() && "Failed to resolve generated func name loc");

  RenameLoc RenameConfig = {
    LineAndCol.first, LineAndCol.second,
    NameUsage::Definition, /*OldName=*/Name, /*NewName=*/"",
    IsFunctionLike, IsNonProtocolType
  };
  RenameRangeDetailCollector Renamer(SM, Name);
  Renamer.addSyntacticRenameRanges(Resolved.back(), RenameConfig);
  auto Ranges = Renamer.Ranges;

  std::vector<NoteRegion> NoteRegions(Renamer.Ranges.size());
  llvm::transform(
      Ranges, NoteRegions.begin(),
      [&SM](RenameRangeDetail &Detail) -> NoteRegion {
        auto Start = SM.getLineAndColumnInBuffer(Detail.Range.getStart());
        auto End = SM.getLineAndColumnInBuffer(Detail.Range.getEnd());
        return {Detail.RangeKind, Start.first, Start.second,
                End.first,        End.second,  Detail.Index};
      });

  return NoteRegions;
}

bool RefactoringActionExtractFunction::performChange() {
  // Check if the new name is ok.
  if (!Lexer::isIdentifier(PreferredName)) {
    DiagEngine.diagnose(SourceLoc(), diag::invalid_name, PreferredName);
    return true;
  }
  DeclContext *DC = RangeInfo.RangeContext;
  DeclContext *InsertToDC = nullptr;
  SourceLoc InsertLoc = getNewFuncInsertLoc(DC, InsertToDC);

  // Complain about no inserting position.
  if (InsertLoc.isInvalid()) {
    DiagEngine.diagnose(SourceLoc(), diag::no_insert_position);
    return true;
  }

  // Correct the given name if collision happens.
  PreferredName = correctNewDeclName(InsertToDC, PreferredName);

  // Collect the paramters to pass down to the new function.
  std::vector<ReferencedDecl> Parameters;
  for (auto &RD: RangeInfo.ReferencedDecls) {
    // If the referenced decl is declared elsewhere, no need to pass as parameter
    if (RD.VD->getDeclContext() != DC)
      continue;

    // We don't need to pass down implicitly declared variables, e.g. error in
    // a catch block.
    if (RD.VD->isImplicit()) {
      SourceLoc Loc = RD.VD->getStartLoc();
      if (Loc.isValid() &&
          SM.isBeforeInBuffer(RangeInfo.ContentRange.getStart(), Loc) &&
          SM.isBeforeInBuffer(Loc, RangeInfo.ContentRange.getEnd()))
        continue;
    }

    // If the referenced decl is declared inside the range, no need to pass
    // as parameter.
    if (RangeInfo.DeclaredDecls.end() !=
      std::find_if(RangeInfo.DeclaredDecls.begin(), RangeInfo.DeclaredDecls.end(),
        [RD](DeclaredDecl DD) { return RD.VD == DD.VD; }))
      continue;

    // We don't need to pass down self.
    if (auto PD = dyn_cast<ParamDecl>(RD.VD)) {
      if (PD->isSelfParameter()) {
        continue;
      }
    }

    Parameters.emplace_back(RD.VD, sanitizeType(RD.Ty));
  }
  SmallString<64> Buffer;
  unsigned FuncBegin = Buffer.size();
  unsigned FuncNameOffset;
  {
    llvm::raw_svector_ostream OS(Buffer);

    if (!InsertToDC->isLocalContext()) {
      // Default to be file private.
      OS << tok::kw_fileprivate << " ";
    }

    // Inherit static if the containing function is.
    if (DC->getContextKind() == DeclContextKind::AbstractFunctionDecl) {
      if (auto FD = dyn_cast<FuncDecl>(static_cast<AbstractFunctionDecl*>(DC))) {
        if (FD->isStatic()) {
          OS << tok::kw_static << " ";
        }
      }
    }

    OS << tok::kw_func << " ";
    FuncNameOffset = Buffer.size() - FuncBegin;
    OS << PreferredName;
    OS << "(";
    for (auto &RD : Parameters) {
      OS << "_ " << RD.VD->getBaseName().userFacingName() << ": ";
      RD.Ty->reconstituteSugar(/*Recursive*/true)->print(OS);
      if (&RD != &Parameters.back())
        OS << ", ";
    }
    OS << ")";

    if (RangeInfo.ThrowingUnhandledError)
      OS << " " << tok::kw_throws;

    bool InsertedReturnType = false;
    if (auto Ty = RangeInfo.getType()) {
      // If the type of the range is not void, specify the return type.
      if (!Ty->isVoid()) {
        OS << " " << tok::arrow << " ";
        sanitizeType(Ty)->reconstituteSugar(/*Recursive*/true)->print(OS);
        InsertedReturnType = true;
      }
    }

    OS << " {\n";

    // Add "return" if the extracted entity is an expression.
    if (RangeInfo.Kind == RangeKind::SingleExpression && InsertedReturnType)
      OS << tok::kw_return << " ";
    OS << RangeInfo.ContentRange.str() << "\n}\n\n";
  }
  unsigned FuncEnd = Buffer.size();

  unsigned ReplaceBegin = Buffer.size();
  unsigned CallNameOffset;
  {
    llvm::raw_svector_ostream OS(Buffer);
    if (RangeInfo.exit() == ExitState::Positive)
      OS << tok::kw_return <<" ";
    CallNameOffset = Buffer.size() - ReplaceBegin;
    OS << PreferredName << "(";
    for (auto &RD : Parameters) {

      // Inout argument needs "&".
      if (RD.Ty->is<InOutType>())
        OS << "&";
      OS << RD.VD->getBaseName().userFacingName();
      if (&RD != &Parameters.back())
        OS << ", ";
    }
    OS << ")";
  }
  unsigned ReplaceEnd = Buffer.size();

  std::string ExtractedFuncName = PreferredName.str() + "(";
  for (size_t i = 0; i < Parameters.size(); ++i) {
    ExtractedFuncName += "_:";
  }
  ExtractedFuncName += ")";

  StringRef DeclStr(Buffer.begin() + FuncBegin, FuncEnd - FuncBegin);
  auto NotableFuncRegions = getNotableRegions(DeclStr, FuncNameOffset,
                                              ExtractedFuncName,
                                              /*IsFunctionLike=*/true);

  StringRef CallStr(Buffer.begin() + ReplaceBegin, ReplaceEnd - ReplaceBegin);
  auto NotableCallRegions = getNotableRegions(CallStr, CallNameOffset,
                                              ExtractedFuncName,
                                              /*IsFunctionLike=*/true);

  // Insert the new function's declaration.
  EditConsumer.accept(SM, InsertLoc, DeclStr, NotableFuncRegions);

  // Replace the code to extract with the function call.
  EditConsumer.accept(SM, RangeInfo.ContentRange, CallStr, NotableCallRegions);

  return false;
}

class RefactoringActionExtractExprBase {
  SourceFile *TheFile;
  ResolvedRangeInfo RangeInfo;
  DiagnosticEngine &DiagEngine;
  const bool ExtractRepeated;
  StringRef PreferredName;
  SourceEditConsumer &EditConsumer;

  ASTContext &Ctx;
  SourceManager &SM;

public:
  RefactoringActionExtractExprBase(SourceFile *TheFile,
                                   ResolvedRangeInfo RangeInfo,
                                   DiagnosticEngine &DiagEngine,
                                   bool ExtractRepeated,
                                   StringRef PreferredName,
                                   SourceEditConsumer &EditConsumer) :
    TheFile(TheFile), RangeInfo(RangeInfo), DiagEngine(DiagEngine),
    ExtractRepeated(ExtractRepeated), PreferredName(PreferredName),
    EditConsumer(EditConsumer), Ctx(TheFile->getASTContext()),
    SM(Ctx.SourceMgr){}
  bool performChange();
};

/// This is to ensure all decl references in two expressions are identical.
struct ReferenceCollector: public SourceEntityWalker {
  SmallVector<ValueDecl*, 4> References;

  ReferenceCollector(Expr *E) { walk(E); }
  bool visitDeclReference(ValueDecl *D, CharSourceRange Range,
                          TypeDecl *CtorTyRef, ExtensionDecl *ExtTyRef,
                          Type T, ReferenceMetaData Data) override {
    References.emplace_back(D);
    return true;
  }
  bool operator==(const ReferenceCollector &Other) const {
    if (References.size() != Other.References.size())
      return false;
    return std::equal(References.begin(), References.end(),
                      Other.References.begin());
  }
};

struct SimilarExprCollector: public SourceEntityWalker {
  SourceManager &SM;

  /// The expression under selection.
  Expr *SelectedExpr;
  ArrayRef<Token> AllTokens;
  llvm::SetVector<Expr*> &Bucket;

  /// The tokens included in the expression under selection.
  ArrayRef<Token> SelectedTokens;

  /// The referenced decls in the expression under selection.
  ReferenceCollector SelectedReferences;

  bool compareTokenContent(ArrayRef<Token> Left, ArrayRef<Token> Right) {
    if (Left.size() != Right.size())
      return false;
    return std::equal(Left.begin(), Left.end(), Right.begin(),
                      [](const Token &L, const Token& R) {
                        return L.getText() == R.getText();
                      });
  }

  /// Find all tokens included by an expression.
  ArrayRef<Token> getExprSlice(Expr *E) {
    return slice_token_array(AllTokens, E->getStartLoc(), E->getEndLoc());
  }

  SimilarExprCollector(SourceManager &SM, Expr *SelectedExpr,
                       ArrayRef<Token> AllTokens,
    llvm::SetVector<Expr*> &Bucket): SM(SM), SelectedExpr(SelectedExpr),
    AllTokens(AllTokens), Bucket(Bucket),
    SelectedTokens(getExprSlice(SelectedExpr)),
    SelectedReferences(SelectedExpr){}

  bool walkToExprPre(Expr *E) override {
    // We don't extract implicit expressions.
    if (E->isImplicit())
      return true;
    if (E->getKind() != SelectedExpr->getKind())
      return true;

    // First check the underlying token arrays have the same content.
    if (compareTokenContent(getExprSlice(E), SelectedTokens)) {
      ReferenceCollector CurrentReferences(E);

      // Next, check the referenced decls are same.
      if (CurrentReferences == SelectedReferences)
        Bucket.insert(E);
    }
    return true;
  }
};

bool RefactoringActionExtractExprBase::performChange() {
  // Check if the new name is ok.
  if (!Lexer::isIdentifier(PreferredName)) {
    DiagEngine.diagnose(SourceLoc(), diag::invalid_name, PreferredName);
    return true;
  }

  // Find the enclosing brace statement;
  ContextFinder Finder(*TheFile, RangeInfo.ContainedNodes.front(),
                       [](ASTNode N) { return N.isStmt(StmtKind::Brace); });

  auto *SelectedExpr = RangeInfo.ContainedNodes[0].get<Expr*>();
  Finder.resolve();
  SourceLoc InsertLoc;
  llvm::SetVector<ValueDecl*> AllVisibleDecls;
  struct DeclCollector: public SourceEntityWalker {
    llvm::SetVector<ValueDecl*> &Bucket;
    DeclCollector(llvm::SetVector<ValueDecl*> &Bucket): Bucket(Bucket) {}
    bool walkToDeclPre(Decl *D, CharSourceRange Range) override {
      if (auto *VD = dyn_cast<ValueDecl>(D))
        Bucket.insert(VD);
      return true;
    }
  } Collector(AllVisibleDecls);

  llvm::SetVector<Expr*> AllExpressions;

  if (!Finder.getContexts().empty()) {

    // Get the innermost brace statement.
    auto BS = static_cast<BraceStmt*>(Finder.getContexts().back().get<Stmt*>());

    // Collect all value decls inside the brace statement.
    Collector.walk(BS);

    if (ExtractRepeated) {
      // Collect all expressions we are going to extract.
      SimilarExprCollector(SM, SelectedExpr,
                           slice_token_array(TheFile->getAllTokens(),
                                             BS->getStartLoc(),
                                             BS->getEndLoc()),
                           AllExpressions).walk(BS);
    } else {
      AllExpressions.insert(SelectedExpr);
    }

    assert(!AllExpressions.empty() && "at least one expression is extracted.");
    for (auto Ele : BS->getElements()) {
      // Find the element that encloses the first expression under extraction.
      if (SM.rangeContains(Ele.getSourceRange(),
                           (*AllExpressions.begin())->getSourceRange())) {

        // Insert before the enclosing element.
        InsertLoc = Ele.getStartLoc();
      }
    }
  }

  // Complain about no inserting position.
  if (InsertLoc.isInvalid()) {
    DiagEngine.diagnose(SourceLoc(), diag::no_insert_position);
    return true;
  }

  // Correct name if collision happens.
  PreferredName = correctNameInternal(TheFile->getASTContext(), PreferredName,
                                      AllVisibleDecls.getArrayRef());

  // Print the type name of this expression.
  SmallString<16> TyBuffer;

  // We are not sure about the type of repeated expressions.
  if (!ExtractRepeated) {
    if (auto Ty = RangeInfo.getType()) {
      llvm::raw_svector_ostream OS(TyBuffer);
      OS << ": ";
      Ty->getRValueType()->reconstituteSugar(true)->print(OS);
    }
  }

  SmallString<64> DeclBuffer;
  llvm::raw_svector_ostream OS(DeclBuffer);
  unsigned StartOffset, EndOffset;
  OS << tok::kw_let << " ";
  StartOffset = DeclBuffer.size();
  OS << PreferredName;
  EndOffset = DeclBuffer.size();
  OS << TyBuffer.str() <<  " = " << RangeInfo.ContentRange.str() << "\n";

  NoteRegion DeclNameRegion{
    RefactoringRangeKind::BaseName,
    /*StartLine=*/1, /*StartColumn=*/StartOffset + 1,
    /*EndLine=*/1, /*EndColumn=*/EndOffset + 1,
    /*ArgIndex*/None
  };

  // Perform code change.
  EditConsumer.accept(SM, InsertLoc, DeclBuffer.str(), {DeclNameRegion});

  // Replace all occurrences of the extracted expression.
  for (auto *E : AllExpressions) {
    EditConsumer.accept(SM,
      Lexer::getCharSourceRangeFromSourceRange(SM, E->getSourceRange()),
      PreferredName,
      {{
        RefactoringRangeKind::BaseName,
        /*StartLine=*/1, /*StartColumn-*/1, /*EndLine=*/1,
        /*EndColumn=*/static_cast<unsigned int>(PreferredName.size() + 1),
        /*ArgIndex*/None
      }});
  }
  return false;
}

bool RefactoringActionExtractExpr::
isApplicable(const ResolvedRangeInfo &Info, DiagnosticEngine &Diag) {
  switch (Info.Kind) {
    case RangeKind::SingleExpression:
      // We disallow extract literal expression for two reasons:
      // (1) since we print the type for extracted expression, the type of a
      // literal may print as "int2048" where it is not typically users' choice;
      // (2) Extracting one literal provides little value for users.
      return checkExtractConditions(Info, Diag).success();
    case RangeKind::PartOfExpression:
    case RangeKind::SingleDecl:
    case RangeKind::MultiTypeMemberDecl:
    case RangeKind::SingleStatement:
    case RangeKind::MultiStatement:
    case RangeKind::Invalid:
      return false;
  }
  llvm_unreachable("unhandled kind");
}

bool RefactoringActionExtractExpr::performChange() {
  return RefactoringActionExtractExprBase(TheFile, RangeInfo,
                                          DiagEngine, false, PreferredName,
                                          EditConsumer).performChange();
}

bool RefactoringActionExtractRepeatedExpr::
isApplicable(const ResolvedRangeInfo &Info, DiagnosticEngine &Diag) {
  switch (Info.Kind) {
    case RangeKind::SingleExpression:
      return checkExtractConditions(Info, Diag).
        success({CannotExtractReason::Literal});
    case RangeKind::PartOfExpression:
    case RangeKind::SingleDecl:
    case RangeKind::MultiTypeMemberDecl:
    case RangeKind::SingleStatement:
    case RangeKind::MultiStatement:
    case RangeKind::Invalid:
      return false;
  }
  llvm_unreachable("unhandled kind");
}
bool RefactoringActionExtractRepeatedExpr::performChange() {
  return RefactoringActionExtractExprBase(TheFile, RangeInfo,
                                          DiagEngine, true, PreferredName,
                                          EditConsumer).performChange();
}


bool RefactoringActionMoveMembersToExtension::isApplicable(
    const ResolvedRangeInfo &Info, DiagnosticEngine &Diag) {
  switch (Info.Kind) {
  case RangeKind::SingleDecl:
  case RangeKind::MultiTypeMemberDecl: {
    DeclContext *DC = Info.RangeContext;

    // The the common decl context is not a nomial type, we cannot create an
    // extension for it
    if (!DC || !DC->getInnermostDeclarationDeclContext() ||
        !isa<NominalTypeDecl>(DC->getInnermostDeclarationDeclContext()))
      return false;


    // Members of types not declared at top file level cannot be extracted
    // to an extension at top file level
    if (DC->getParent()->getContextKind() != DeclContextKind::FileUnit)
      return false;

    // Check if contained nodes are all allowed decls.
    for (auto Node : Info.ContainedNodes) {
      Decl *D = Node.dyn_cast<Decl*>();
      if (!D)
        return false;

      if (isa<AccessorDecl>(D) || isa<DestructorDecl>(D) ||
          isa<EnumCaseDecl>(D) || isa<EnumElementDecl>(D))
        return false;
    }

    // We should not move instance variables with storage into the extension
    // because they are not allowed to be declared there
    for (auto DD : Info.DeclaredDecls) {
      if (auto ASD = dyn_cast<AbstractStorageDecl>(DD.VD)) {
        // Only disallow storages in the common decl context, allow them in
        // any subtypes
        if (ASD->hasStorage() && ASD->getDeclContext() == DC) {
          return false;
        }
      }
    }

    return true;
  }
  case RangeKind::SingleExpression:
  case RangeKind::PartOfExpression:
  case RangeKind::SingleStatement:
  case RangeKind::MultiStatement:
  case RangeKind::Invalid:
    return false;
  }
  llvm_unreachable("unhandled kind");
}

bool RefactoringActionMoveMembersToExtension::performChange() {
  DeclContext *DC = RangeInfo.RangeContext;

  auto CommonTypeDecl =
      dyn_cast<NominalTypeDecl>(DC->getInnermostDeclarationDeclContext());
  assert(CommonTypeDecl && "Not applicable if common parent is no nomial type");

  SmallString<64> Buffer;
  llvm::raw_svector_ostream OS(Buffer);
  OS << "\n\n";
  OS << "extension " << CommonTypeDecl->getName() << " {\n";
  OS << RangeInfo.ContentRange.str().trim();
  OS << "\n}";

  // Insert extension after the type declaration
  EditConsumer.insertAfter(SM, CommonTypeDecl->getEndLoc(), Buffer);
  EditConsumer.remove(SM, RangeInfo.ContentRange);

  return false;
}

namespace {
// A SingleDecl range may not include all decls actually declared in that range:
// a var decl has accessors that aren't included. This will find those missing
// decls.
class FindAllSubDecls : public SourceEntityWalker {
  SmallPtrSetImpl<Decl *> &Found;
  public:
  FindAllSubDecls(SmallPtrSetImpl<Decl *> &found)
    : Found(found) {}

  bool walkToDeclPre(Decl *D, CharSourceRange range) override {
    // Record this Decl, and skip its contents if we've already touched it.
    if (!Found.insert(D).second)
      return false;

    if (auto ASD = dyn_cast<AbstractStorageDecl>(D)) {
      ASD->visitParsedAccessors([&](AccessorDecl *accessor) {
        Found.insert(accessor);
      });
    }
    return true;
  }
};
}
bool RefactoringActionReplaceBodiesWithFatalError::isApplicable(
  const ResolvedRangeInfo &Info, DiagnosticEngine &Diag) {
  switch (Info.Kind) {
  case RangeKind::SingleDecl:
  case RangeKind::MultiTypeMemberDecl: {
    SmallPtrSet<Decl *, 16> Found;
    for (auto decl : Info.DeclaredDecls) {
      FindAllSubDecls(Found).walk(decl.VD);
    }
    for (auto decl : Found) {
      auto AFD = dyn_cast<AbstractFunctionDecl>(decl);
      if (AFD && !AFD->isImplicit())
        return true;
    }

    return false;
 }
  case RangeKind::SingleExpression:
  case RangeKind::PartOfExpression:
  case RangeKind::SingleStatement:
  case RangeKind::MultiStatement:
  case RangeKind::Invalid:
    return false;
  }
  llvm_unreachable("unhandled kind");
}

bool RefactoringActionReplaceBodiesWithFatalError::performChange() {
  const StringRef replacement = "{\nfatalError()\n}";
  SmallPtrSet<Decl *, 16> Found;
  for (auto decl : RangeInfo.DeclaredDecls) {
    FindAllSubDecls(Found).walk(decl.VD);
  }
  for (auto decl : Found) {
    auto AFD = dyn_cast<AbstractFunctionDecl>(decl);
    if (!AFD || AFD->isImplicit())
      continue;

    auto range = AFD->getBodySourceRange();
    // If we're in replacement mode (i.e. have an edit consumer), we can
    // rewrite the function body.
    auto charRange = Lexer::getCharSourceRangeFromSourceRange(SM, range);
    EditConsumer.accept(SM, charRange, replacement);

  }
  return false;
}

static std::pair<IfStmt *, IfStmt *>
findCollapseNestedIfTarget(const ResolvedCursorInfo &CursorInfo) {
  if (CursorInfo.Kind != CursorInfoKind::StmtStart)
    return {};

  // Ensure the statement is 'if' statement. It must not have 'else' clause.
  IfStmt *OuterIf = dyn_cast<IfStmt>(CursorInfo.TrailingStmt);
  if (!OuterIf)
    return {};
  if (OuterIf->getElseStmt())
    return {};

  // The body must contain a sole inner 'if' statement.
  auto Body = dyn_cast_or_null<BraceStmt>(OuterIf->getThenStmt());
  if (!Body || Body->getNumElements() != 1)
    return {};

  IfStmt *InnerIf =
      dyn_cast_or_null<IfStmt>(Body->getFirstElement().dyn_cast<Stmt *>());
  if (!InnerIf)
    return {};

  // Inner 'if' statement also cannot have 'else' clause.
  if (InnerIf->getElseStmt())
    return {};

  return {OuterIf, InnerIf};
}

bool RefactoringActionCollapseNestedIfStmt::
isApplicable(const ResolvedCursorInfo &CursorInfo, DiagnosticEngine &Diag) {
  return findCollapseNestedIfTarget(CursorInfo).first;
}

bool RefactoringActionCollapseNestedIfStmt::performChange() {
  auto Target = findCollapseNestedIfTarget(CursorInfo);
  if (!Target.first)
    return true;
  auto OuterIf = Target.first;
  auto InnerIf = Target.second;

  EditorConsumerInsertStream OS(
      EditConsumer, SM,
      Lexer::getCharSourceRangeFromSourceRange(SM, OuterIf->getSourceRange()));

  OS << tok::kw_if << " "; 

  // Emit conditions.
  bool first = true;
  for (auto &C : llvm::concat<StmtConditionElement>(OuterIf->getCond(),
                                                    InnerIf->getCond())) {
    if (first)
      first = false;
    else
      OS << ", ";
    OS << Lexer::getCharSourceRangeFromSourceRange(SM, C.getSourceRange())
              .str();
  }

  // Emit body.
  OS << " ";
  OS << Lexer::getCharSourceRangeFromSourceRange(
            SM, InnerIf->getThenStmt()->getSourceRange())
            .str();
  return false;
}

static std::unique_ptr<llvm::SetVector<Expr*>>
findConcatenatedExpressions(const ResolvedRangeInfo &Info, ASTContext &Ctx) {
  Expr *E = nullptr;

  switch (Info.Kind) {
  case RangeKind::SingleExpression:
    E = Info.ContainedNodes[0].get<Expr*>();
    break;
  case RangeKind::PartOfExpression:
    E = Info.CommonExprParent;
    break;
  default:
    return nullptr;
  }

  assert(E);

  struct StringInterpolationExprFinder: public SourceEntityWalker {
    std::unique_ptr<llvm::SetVector<Expr *>> Bucket =
        std::make_unique<llvm::SetVector<Expr *>>();
    ASTContext &Ctx;

    bool IsValidInterpolation = true;
    StringInterpolationExprFinder(ASTContext &Ctx): Ctx(Ctx) {}

    bool isConcatenationExpr(DeclRefExpr* Expr) {
      if (!Expr)
        return false;
      auto *FD = dyn_cast<FuncDecl>(Expr->getDecl());
      if (FD == nullptr || (FD != Ctx.getPlusFunctionOnString() &&
          FD != Ctx.getPlusFunctionOnRangeReplaceableCollection())) {
        return false;
      }
      return true;
    }

    bool walkToExprPre(Expr *E) override {
      if (E->isImplicit())
        return true;
      // FIXME: we should have ErrorType instead of null.
      if (E->getType().isNull())
        return true;

      //Only binary concatenation operators should exist in expression
      if (E->getKind() == ExprKind::Binary) {
        auto *BE = dyn_cast<BinaryExpr>(E);
        auto *OperatorDeclRef = BE->getSemanticFn()->getMemberOperatorRef();
        if (!(isConcatenationExpr(OperatorDeclRef) &&
            E->getType()->isString())) {
          IsValidInterpolation = false;
          return false;
        }
        return true;
      }
      // Everything that evaluates to string should be gathered.
      if (E->getType()->isString()) {
        Bucket->insert(E);
        return false;
      }
      if (auto *DR = dyn_cast<DeclRefExpr>(E)) {
        // Checks whether all function references in expression are concatenations.
        auto *FD = dyn_cast<FuncDecl>(DR->getDecl());
        auto IsConcatenation = isConcatenationExpr(DR);
        if (FD && IsConcatenation) {
          return false;
        }
      }
      // There was non-expected expression, it's not valid interpolation then.
      IsValidInterpolation = false;
      return false;
    }
  } Walker(Ctx);
  Walker.walk(E);

  // There should be two or more expressions to convert.
  if (!Walker.IsValidInterpolation || Walker.Bucket->size() < 2)
    return nullptr;

  return std::move(Walker.Bucket);
}

static void interpolatedExpressionForm(Expr *E, SourceManager &SM,
                                              llvm::raw_ostream &OS) {
  if (auto *Literal = dyn_cast<StringLiteralExpr>(E)) {
    OS << Literal->getValue();
    return;
  }
  auto ExpStr = Lexer::getCharSourceRangeFromSourceRange(SM,
    E->getSourceRange()).str().str();
  if (isa<InterpolatedStringLiteralExpr>(E)) {
    ExpStr.erase(0, 1);
    ExpStr.pop_back();
    OS << ExpStr;
    return;
  }
  OS << "\\(" << ExpStr << ")";
}

bool RefactoringActionConvertStringsConcatenationToInterpolation::
isApplicable(const ResolvedRangeInfo &Info, DiagnosticEngine &Diag) {
  auto RangeContext = Info.RangeContext;
  if (RangeContext) {
    auto &Ctx = Info.RangeContext->getASTContext();
    return findConcatenatedExpressions(Info, Ctx) != nullptr;
  }
  return false;
}

bool RefactoringActionConvertStringsConcatenationToInterpolation::performChange() {
  auto Expressions = findConcatenatedExpressions(RangeInfo, Ctx);
  if (!Expressions)
    return true;
  EditorConsumerInsertStream OS(EditConsumer, SM, RangeInfo.ContentRange);
  OS << "\"";
  for (auto It = Expressions->begin(); It != Expressions->end(); ++It) {
    interpolatedExpressionForm(*It, SM, OS);
  }
  OS << "\"";
  return false;
}

/// Abstract helper class containing info about an IfExpr
/// that can be expanded into an IfStmt.
class ExpandableTernaryExprInfo {

public:
  virtual ~ExpandableTernaryExprInfo() {}

  virtual IfExpr *getIf() = 0;

  virtual SourceRange getNameRange() = 0;

  virtual Type getType() = 0;

  virtual bool shouldDeclareNameAndType() {
    return !getType().isNull();
  }

  virtual bool isValid() {

    //Ensure all public properties are non-nil and valid
    if (!getIf() || !getNameRange().isValid())
      return false;
    if (shouldDeclareNameAndType() && getType().isNull())
      return false;

    return true; //valid
  }

  CharSourceRange getNameCharRange(const SourceManager &SM) {
    return Lexer::getCharSourceRangeFromSourceRange(SM, getNameRange());
  }
};

/// Concrete subclass containing info about an AssignExpr
/// where the source is the expandable IfExpr.
class ExpandableAssignTernaryExprInfo: public ExpandableTernaryExprInfo {

public:
  ExpandableAssignTernaryExprInfo(AssignExpr *Assign): Assign(Assign) {}

  IfExpr *getIf() override {
    if (!Assign)
      return nullptr;
    return dyn_cast_or_null<IfExpr>(Assign->getSrc());
  }

  SourceRange getNameRange() override {
    auto Invalid = SourceRange();

    if (!Assign)
      return Invalid;

    if (auto dest = Assign->getDest())
      return dest->getSourceRange();

    return Invalid;
  }

  Type getType() override {
    return nullptr;
  }

private:
  AssignExpr *Assign = nullptr;
};

/// Concrete subclass containing info about a PatternBindingDecl
/// where the pattern initializer is the expandable IfExpr.
class ExpandableBindingTernaryExprInfo: public ExpandableTernaryExprInfo {

public:
  ExpandableBindingTernaryExprInfo(PatternBindingDecl *Binding):
  Binding(Binding) {}

  IfExpr *getIf() override {
    if (Binding && Binding->getNumPatternEntries() == 1) {
      if (auto *Init = Binding->getInit(0)) {
        return dyn_cast<IfExpr>(Init);
      }
    }

    return nullptr;
  }

  SourceRange getNameRange() override {
    if (auto Pattern = getNamePattern())
      return Pattern->getSourceRange();

    return SourceRange();
  }

  Type getType() override {
    if (auto Pattern = getNamePattern())
      return Pattern->getType();

    return nullptr;
  }

private:
  Pattern *getNamePattern() {
    if (!Binding || Binding->getNumPatternEntries() != 1)
      return nullptr;

    auto Pattern = Binding->getPattern(0);

    if (!Pattern)
      return nullptr;

    if (auto TyPattern = dyn_cast<TypedPattern>(Pattern))
      Pattern = TyPattern->getSubPattern();

    return Pattern;
  }

  PatternBindingDecl *Binding = nullptr;
};

std::unique_ptr<ExpandableTernaryExprInfo>
findExpandableTernaryExpression(const ResolvedRangeInfo &Info) {

  if (Info.Kind != RangeKind::SingleDecl
      && Info.Kind != RangeKind:: SingleExpression)
    return nullptr;

  if (Info.ContainedNodes.size() != 1)
    return nullptr;

  if (auto D = Info.ContainedNodes[0].dyn_cast<Decl*>())
    if (auto Binding = dyn_cast<PatternBindingDecl>(D))
      return std::make_unique<ExpandableBindingTernaryExprInfo>(Binding);

  if (auto E = Info.ContainedNodes[0].dyn_cast<Expr*>())
    if (auto Assign = dyn_cast<AssignExpr>(E))
      return std::make_unique<ExpandableAssignTernaryExprInfo>(Assign);

  return nullptr;
}

bool RefactoringActionExpandTernaryExpr::
isApplicable(const ResolvedRangeInfo &Info, DiagnosticEngine &Diag) {
  auto Target = findExpandableTernaryExpression(Info);
  return Target && Target->isValid();
}

bool RefactoringActionExpandTernaryExpr::performChange() {
  auto Target = findExpandableTernaryExpression(RangeInfo);

  if (!Target || !Target->isValid())
    return true; //abort

  auto NameCharRange = Target->getNameCharRange(SM);

  auto IfRange = Target->getIf()->getSourceRange();
  auto IfCharRange = Lexer::getCharSourceRangeFromSourceRange(SM, IfRange);

  auto CondRange = Target->getIf()->getCondExpr()->getSourceRange();
  auto CondCharRange = Lexer::getCharSourceRangeFromSourceRange(SM, CondRange);

  auto ThenRange = Target->getIf()->getThenExpr()->getSourceRange();
  auto ThenCharRange = Lexer::getCharSourceRangeFromSourceRange(SM, ThenRange);

  auto ElseRange = Target->getIf()->getElseExpr()->getSourceRange();
  auto ElseCharRange = Lexer::getCharSourceRangeFromSourceRange(SM, ElseRange);

  SmallString<64> DeclBuffer;
  llvm::raw_svector_ostream OS(DeclBuffer);

  StringRef Space = " ";
  StringRef NewLine = "\n";

  if (Target->shouldDeclareNameAndType()) {
    //Specifier will not be replaced; append after specifier
    OS << NameCharRange.str() << tok::colon << Space;
    OS << Target->getType() << NewLine;
  }

  OS << tok::kw_if << Space;
  OS << CondCharRange.str() << Space;
  OS << tok::l_brace << NewLine;

  OS << NameCharRange.str() << Space;
  OS << tok::equal << Space;
  OS << ThenCharRange.str() << NewLine;

  OS << tok::r_brace << Space;
  OS << tok::kw_else << Space;
  OS << tok::l_brace << NewLine;

  OS << NameCharRange.str() << Space;
  OS << tok::equal << Space;
  OS << ElseCharRange.str() << NewLine;

  OS << tok::r_brace;

  //Start replacement with name range, skip the specifier
  auto ReplaceRange(NameCharRange);
  ReplaceRange.widen(IfCharRange);

  EditConsumer.accept(SM, ReplaceRange, DeclBuffer.str());

  return false; //don't abort
}

bool RefactoringActionConvertIfLetExprToGuardExpr::
  isApplicable(const ResolvedRangeInfo &Info, DiagnosticEngine &Diag) {

  if (Info.Kind != RangeKind::SingleStatement
      && Info.Kind != RangeKind::MultiStatement)
    return false;

  if (Info.ContainedNodes.empty())
    return false;

  IfStmt *If = nullptr;

  if (Info.ContainedNodes.size() == 1) {
    if (auto S = Info.ContainedNodes[0].dyn_cast<Stmt*>()) {
      If = dyn_cast<IfStmt>(S);
    }
  }

  if (!If)
    return false;

  auto CondList = If->getCond();

  if (CondList.size() == 1) {
    auto E = CondList[0];
    auto P = E.getKind();
    if (P == swift::StmtConditionElement::CK_PatternBinding) {
      auto Body = dyn_cast_or_null<BraceStmt>(If->getThenStmt());
      if (Body)
        return true;
    }
  }

  return false;
}

bool RefactoringActionConvertIfLetExprToGuardExpr::performChange() {

  auto S = RangeInfo.ContainedNodes[0].dyn_cast<Stmt*>();
  IfStmt *If = dyn_cast<IfStmt>(S);
  auto CondList = If->getCond();

  // Get if-let condition
  SourceRange range = CondList[0].getSourceRange();
  SourceManager &SM = RangeInfo.RangeContext->getASTContext().SourceMgr;
  auto CondCharRange = Lexer::getCharSourceRangeFromSourceRange(SM, range);
  
  auto Body = dyn_cast_or_null<BraceStmt>(If->getThenStmt());
  
  // Get if-let then body.
  auto firstElement = Body->getFirstElement();
  auto lastElement = Body->getLastElement();
  SourceRange bodyRange = firstElement.getSourceRange();
  bodyRange.widen(lastElement.getSourceRange());
  auto BodyCharRange = Lexer::getCharSourceRangeFromSourceRange(SM, bodyRange);
  
  SmallString<64> DeclBuffer;
  llvm::raw_svector_ostream OS(DeclBuffer);
  
  StringRef Space = " ";
  StringRef NewLine = "\n";
  
  OS << tok::kw_guard << Space;
  OS << CondCharRange.str().str() << Space;
  OS << tok::kw_else << Space;
  OS << tok::l_brace << NewLine;
  
  // Get if-let else body.
  if (auto *ElseBody = dyn_cast_or_null<BraceStmt>(If->getElseStmt())) {
    auto firstElseElement = ElseBody->getFirstElement();
    auto lastElseElement = ElseBody->getLastElement();
    SourceRange elseBodyRange = firstElseElement.getSourceRange();
    elseBodyRange.widen(lastElseElement.getSourceRange());
    auto ElseBodyCharRange = Lexer::getCharSourceRangeFromSourceRange(SM, elseBodyRange);
    OS << ElseBodyCharRange.str().str() << NewLine;
  }
  
  OS << tok::kw_return << NewLine;
  OS << tok::r_brace << NewLine;
  OS << BodyCharRange.str().str();
  
  // Replace if-let to guard
  auto ReplaceRange = RangeInfo.ContentRange;
  EditConsumer.accept(SM, ReplaceRange, DeclBuffer.str());

  return false;
}

bool RefactoringActionConvertGuardExprToIfLetExpr::
isApplicable(const ResolvedRangeInfo &Info, DiagnosticEngine &Diag) {
  if (Info.Kind != RangeKind::SingleStatement
      && Info.Kind != RangeKind::MultiStatement)
    return false;

  if (Info.ContainedNodes.empty())
    return false;

  GuardStmt *guardStmt = nullptr;

  if (Info.ContainedNodes.size() > 0) {
    if (auto S = Info.ContainedNodes[0].dyn_cast<Stmt*>()) {
      guardStmt = dyn_cast<GuardStmt>(S);
    }
  }

  if (!guardStmt)
    return false;

  auto CondList = guardStmt->getCond();

  if (CondList.size() == 1) {
    auto E = CondList[0];
    auto P = E.getPatternOrNull();
    if (P && E.getKind() == swift::StmtConditionElement::CK_PatternBinding)
      return true;
  }

  return false;
}

bool RefactoringActionConvertGuardExprToIfLetExpr::performChange() {

  // Get guard stmt
  auto S = RangeInfo.ContainedNodes[0].dyn_cast<Stmt*>();
  GuardStmt *Guard = dyn_cast<GuardStmt>(S);

  // Get guard condition
  auto CondList = Guard->getCond();

  // Get guard condition source
  SourceRange range = CondList[0].getSourceRange();
  SourceManager &SM = RangeInfo.RangeContext->getASTContext().SourceMgr;
  auto CondCharRange = Lexer::getCharSourceRangeFromSourceRange(SM, range);
  
  SmallString<64> DeclBuffer;
  llvm::raw_svector_ostream OS(DeclBuffer);
  
  StringRef Space = " ";
  StringRef NewLine = "\n";
  
  OS << tok::kw_if << Space;
  OS << CondCharRange.str().str() << Space;
  OS << tok::l_brace << NewLine;

  // Get nodes after guard to place them at if-let body
  if (RangeInfo.ContainedNodes.size() > 1) {
    auto S = RangeInfo.ContainedNodes[1].getSourceRange();
    S.widen(RangeInfo.ContainedNodes.back().getSourceRange());
    auto BodyCharRange = Lexer::getCharSourceRangeFromSourceRange(SM, S);
    OS << BodyCharRange.str().str() << NewLine;
  }
  OS << tok::r_brace;

  // Get guard body
  auto Body = dyn_cast_or_null<BraceStmt>(Guard->getBody());
  
  if (Body && Body->getNumElements() > 1) {
    auto firstElement = Body->getFirstElement();
    auto lastElement = Body->getLastElement();
    SourceRange bodyRange = firstElement.getSourceRange();
    bodyRange.widen(lastElement.getSourceRange());
    auto BodyCharRange = Lexer::getCharSourceRangeFromSourceRange(SM, bodyRange);
    OS << Space << tok::kw_else << Space << tok::l_brace << NewLine;
    OS << BodyCharRange.str().str() << NewLine;
    OS << tok::r_brace;
  }
  
  // Replace guard to if-let
  auto ReplaceRange = RangeInfo.ContentRange;
  EditConsumer.accept(SM, ReplaceRange, DeclBuffer.str());
  
  return false;
}

bool RefactoringActionConvertToSwitchStmt::
isApplicable(const ResolvedRangeInfo &Info, DiagnosticEngine &Diag) {

  class ConditionalChecker : public ASTWalker {
  public:
    bool ParamsUseSameVars = true;
    bool ConditionUseOnlyAllowedFunctions = false;
    StringRef ExpectName;

    Expr *walkToExprPost(Expr *E) override {
      if (E->getKind() != ExprKind::DeclRef)
        return E;
      auto D = dyn_cast<DeclRefExpr>(E)->getDecl();
      if (D->getKind() == DeclKind::Var || D->getKind() == DeclKind::Param)
        ParamsUseSameVars = checkName(dyn_cast<VarDecl>(D));
      if (D->getKind() == DeclKind::Func)
        ConditionUseOnlyAllowedFunctions = checkName(dyn_cast<FuncDecl>(D));
      if (allCheckPassed())
        return E;
      return nullptr;
    }

    bool allCheckPassed() {
      return ParamsUseSameVars && ConditionUseOnlyAllowedFunctions;
    }

  private:
    bool checkName(VarDecl *VD) {
      auto Name = VD->getName().str();
      if (ExpectName.empty())
        ExpectName = Name;
      return Name == ExpectName;
    }

    bool checkName(FuncDecl *FD) {
      const auto Name = FD->getBaseIdentifier().str();
      return Name == "~="
      || Name == "=="
      || Name == "__derived_enum_equals"
      || Name == "__derived_struct_equals"
      || Name == "||"
      || Name == "...";
    }
  };

  class SwitchConvertable {
  public:
    SwitchConvertable(const ResolvedRangeInfo &Info) : Info(Info) { }

    bool isApplicable() {
      if (Info.Kind != RangeKind::SingleStatement)
        return false;
      if (!findIfStmt())
        return false;
      return checkEachCondition();
    }

  private:
    const ResolvedRangeInfo &Info;
    IfStmt *If = nullptr;
    ConditionalChecker checker;

    bool findIfStmt() {
      if (Info.ContainedNodes.size() != 1)
        return false;
      if (auto S = Info.ContainedNodes.front().dyn_cast<Stmt*>())
        If = dyn_cast<IfStmt>(S);
      return If != nullptr;
    }

    bool checkEachCondition() {
      checker = ConditionalChecker();
      do {
        if (!checkEachElement())
          return false;
      } while ((If = dyn_cast_or_null<IfStmt>(If->getElseStmt())));
      return true;
    }

    bool checkEachElement() {
      bool result = true;
      auto ConditionalList = If->getCond();
      for (auto Element : ConditionalList) {
        result &= check(Element);
      }
      return result;
    }

    bool check(StmtConditionElement ConditionElement) {
      if (ConditionElement.getKind() == StmtConditionElement::CK_Availability)
        return false;
      if (ConditionElement.getKind() == StmtConditionElement::CK_PatternBinding)
        checker.ConditionUseOnlyAllowedFunctions = true;
      ConditionElement.walk(checker);
      return checker.allCheckPassed();
    }
  };
  return SwitchConvertable(Info).isApplicable();
}

bool RefactoringActionConvertToSwitchStmt::performChange() {

  class VarNameFinder : public ASTWalker {
  public:
    std::string VarName;

    Expr *walkToExprPost(Expr *E) override {
      if (E->getKind() != ExprKind::DeclRef)
        return E;
      auto D = dyn_cast<DeclRefExpr>(E)->getDecl();
      if (D->getKind() != DeclKind::Var && D->getKind() != DeclKind::Param)
        return E;
      VarName = dyn_cast<VarDecl>(D)->getName().str().str();
      return nullptr;
    }
  };

  class ConditionalPatternFinder : public ASTWalker {
  public:
    ConditionalPatternFinder(SourceManager &SM) : SM(SM) {}

    SmallString<64> ConditionalPattern = SmallString<64>();

    Expr *walkToExprPost(Expr *E) override {
      if (E->getKind() != ExprKind::Binary)
        return E;
      auto BE = dyn_cast<BinaryExpr>(E);
      if (isFunctionNameAllowed(BE))
        appendPattern(dyn_cast<BinaryExpr>(E)->getArg());
      return E;
    }

    std::pair<bool, Pattern*> walkToPatternPre(Pattern *P) override {
      ConditionalPattern.append(Lexer::getCharSourceRangeFromSourceRange(SM, P->getSourceRange()).str());
      if (P->getKind() == PatternKind::OptionalSome)
        ConditionalPattern.append("?");
      return { true, nullptr };
    }

  private:

    SourceManager &SM;

    bool isFunctionNameAllowed(BinaryExpr *E) {
      auto FunctionBody = dyn_cast<DotSyntaxCallExpr>(E->getFn())->getFn();
      auto FunctionDeclaration = dyn_cast<DeclRefExpr>(FunctionBody)->getDecl();
      const auto FunctionName = dyn_cast<FuncDecl>(FunctionDeclaration)
          ->getBaseIdentifier().str();
      return FunctionName == "~="
      || FunctionName == "=="
      || FunctionName == "__derived_enum_equals"
      || FunctionName == "__derived_struct_equals";
    }

    void appendPattern(TupleExpr *Tuple) {
      auto PatternArgument = Tuple->getElements().back();
      if (PatternArgument->getKind() == ExprKind::DeclRef)
        PatternArgument = Tuple->getElements().front();
      if (ConditionalPattern.size() > 0)
        ConditionalPattern.append(", ");
      ConditionalPattern.append(Lexer::getCharSourceRangeFromSourceRange(SM, PatternArgument->getSourceRange()).str());
    }
  };

  class ConverterToSwitch {
  public:
    ConverterToSwitch(const ResolvedRangeInfo &Info,
                      SourceManager &SM) : Info(Info), SM(SM) { }

    void performConvert(SmallString<64> &Out) {
      If = findIf();
      OptionalLabel = If->getLabelInfo().Name.str().str();
      ControlExpression = findControlExpression();
      findPatternsAndBodies(PatternsAndBodies);
      DefaultStatements = findDefaultStatements();
      makeSwitchStatement(Out);
    }

  private:
    const ResolvedRangeInfo &Info;
    SourceManager &SM;

    IfStmt *If;
    IfStmt *PreviousIf;

    std::string OptionalLabel;
    std::string ControlExpression;
    SmallVector<std::pair<std::string, std::string>, 16> PatternsAndBodies;
    std::string DefaultStatements;

    IfStmt *findIf() {
      auto S = Info.ContainedNodes[0].dyn_cast<Stmt*>();
      return dyn_cast<IfStmt>(S);
    }

    std::string findControlExpression() {
      auto ConditionElement = If->getCond().front();
      auto Finder = VarNameFinder();
      ConditionElement.walk(Finder);
      return Finder.VarName;
    }

    void findPatternsAndBodies(SmallVectorImpl<std::pair<std::string, std::string>> &Out) {
      do {
        auto pattern = findPattern();
        auto body = findBodyStatements();
        Out.push_back(std::make_pair(pattern, body));
        PreviousIf = If;
      } while ((If = dyn_cast_or_null<IfStmt>(If->getElseStmt())));
    }

    std::string findPattern() {
      auto ConditionElement = If->getCond().front();
      auto Finder = ConditionalPatternFinder(SM);
      ConditionElement.walk(Finder);
      return Finder.ConditionalPattern.str().str();
    }

    std::string findBodyStatements() {
      return findBodyWithoutBraces(If->getThenStmt());
    }

    std::string findDefaultStatements() {
      auto ElseBody = dyn_cast_or_null<BraceStmt>(PreviousIf->getElseStmt());
      if (!ElseBody)
        return getTokenText(tok::kw_break).str();
      return findBodyWithoutBraces(ElseBody);
    }

    std::string findBodyWithoutBraces(Stmt *body) {
      auto BS = dyn_cast<BraceStmt>(body);
      if (!BS)
        return Lexer::getCharSourceRangeFromSourceRange(SM, body->getSourceRange()).str().str();
      if (BS->getElements().empty())
        return getTokenText(tok::kw_break).str();
      SourceRange BodyRange = BS->getElements().front().getSourceRange();
      BodyRange.widen(BS->getElements().back().getSourceRange());
      return Lexer::getCharSourceRangeFromSourceRange(SM, BodyRange).str().str();
    }

    void makeSwitchStatement(SmallString<64> &Out) {
      StringRef Space = " ";
      StringRef NewLine = "\n";
      llvm::raw_svector_ostream OS(Out);
      if (OptionalLabel.size() > 0)
        OS << OptionalLabel << ":" << Space;
      OS << tok::kw_switch << Space << ControlExpression << Space << tok::l_brace << NewLine;
      for (auto &pair : PatternsAndBodies) {
        OS << tok::kw_case << Space << pair.first << tok::colon << NewLine;
        OS << pair.second << NewLine;
      }
      OS << tok::kw_default << tok::colon << NewLine;
      OS << DefaultStatements << NewLine;
      OS << tok::r_brace;
    }

  };

  SmallString<64> result;
  ConverterToSwitch(RangeInfo, SM).performConvert(result);
  EditConsumer.accept(SM, RangeInfo.ContentRange, result.str());
  return false;
}

/// Struct containing info about an IfStmt that can be converted into an IfExpr.
struct ConvertToTernaryExprInfo {
  ConvertToTernaryExprInfo() {}

  Expr *AssignDest() {

    if (!Then || !Then->getDest() || !Else || !Else->getDest())
      return nullptr;

    auto ThenDest = Then->getDest();
    auto ElseDest = Else->getDest();

    if (ThenDest->getKind() != ElseDest->getKind())
      return nullptr;

    switch (ThenDest->getKind()) {
      case ExprKind::DeclRef: {
        auto ThenRef = dyn_cast<DeclRefExpr>(Then->getDest());
        auto ElseRef = dyn_cast<DeclRefExpr>(Else->getDest());

        if (!ThenRef || !ThenRef->getDecl() || !ElseRef || !ElseRef->getDecl())
          return nullptr;

        const auto ThenName = ThenRef->getDecl()->getName();
        const auto ElseName = ElseRef->getDecl()->getName();

        if (ThenName.compare(ElseName) != 0)
          return nullptr;

        return Then->getDest();
      }
      case ExprKind::Tuple: {
        auto ThenTuple = dyn_cast<TupleExpr>(Then->getDest());
        auto ElseTuple = dyn_cast<TupleExpr>(Else->getDest());

        if (!ThenTuple || !ElseTuple)
          return nullptr;

        auto ThenNames = ThenTuple->getElementNames();
        auto ElseNames = ElseTuple->getElementNames();

        if (!ThenNames.equals(ElseNames))
          return nullptr;

        return ThenTuple;
      }
      default:
        return nullptr;
    }
  }

  Expr *ThenSrc() {
    if (!Then)
      return nullptr;
    return Then->getSrc();
  }

  Expr *ElseSrc() {
    if (!Else)
      return nullptr;
    return Else->getSrc();
  }

  bool isValid() {
    if (!Cond || !AssignDest() || !ThenSrc() || !ElseSrc()
        || !IfRange.isValid())
      return false;

    return true;
  }

  PatternBindingDecl *Binding = nullptr; //optional

  Expr *Cond = nullptr; //required
  AssignExpr *Then = nullptr; //required
  AssignExpr *Else = nullptr; //required
  SourceRange IfRange;
};

ConvertToTernaryExprInfo
findConvertToTernaryExpression(const ResolvedRangeInfo &Info) {

  auto notFound = ConvertToTernaryExprInfo();

  if (Info.Kind != RangeKind::SingleStatement
      && Info.Kind != RangeKind::MultiStatement)
    return notFound;

  if (Info.ContainedNodes.empty())
    return notFound;

  struct AssignExprFinder: public SourceEntityWalker {

    AssignExpr *Assign = nullptr;

    AssignExprFinder(Stmt* S) {
      if (S)
        walk(S);
    }

    virtual bool walkToExprPre(Expr *E) override {
      Assign = dyn_cast<AssignExpr>(E);
      return false;
    }
  };

  ConvertToTernaryExprInfo Target;

  IfStmt *If = nullptr;

  if (Info.ContainedNodes.size() == 1) {
    if (auto S = Info.ContainedNodes[0].dyn_cast<Stmt*>())
      If = dyn_cast<IfStmt>(S);
  }

  if (Info.ContainedNodes.size() == 2) {
    if (auto D = Info.ContainedNodes[0].dyn_cast<Decl*>())
      Target.Binding = dyn_cast<PatternBindingDecl>(D);
    if (auto S = Info.ContainedNodes[1].dyn_cast<Stmt*>())
      If = dyn_cast<IfStmt>(S);
  }

  if (!If)
    return notFound;

  auto CondList = If->getCond();

  if (CondList.size() != 1)
    return notFound;

  Target.Cond = CondList[0].getBooleanOrNull();
  Target.IfRange = If->getSourceRange();

  Target.Then = AssignExprFinder(If->getThenStmt()).Assign;
  Target.Else = AssignExprFinder(If->getElseStmt()).Assign;

  return Target;
}

bool RefactoringActionConvertToTernaryExpr::
isApplicable(const ResolvedRangeInfo &Info, DiagnosticEngine &Diag) {
  return findConvertToTernaryExpression(Info).isValid();
}

bool RefactoringActionConvertToTernaryExpr::performChange() {
  auto Target = findConvertToTernaryExpression(RangeInfo);

  if (!Target.isValid())
    return true; //abort

  SmallString<64> DeclBuffer;
  llvm::raw_svector_ostream OS(DeclBuffer);

  StringRef Space = " ";

  auto IfRange = Target.IfRange;
  auto ReplaceRange = Lexer::getCharSourceRangeFromSourceRange(SM, IfRange);

  auto CondRange = Target.Cond->getSourceRange();
  auto CondCharRange = Lexer::getCharSourceRangeFromSourceRange(SM, CondRange);

  auto ThenRange = Target.ThenSrc()->getSourceRange();
  auto ThenCharRange = Lexer::getCharSourceRangeFromSourceRange(SM, ThenRange);

  auto ElseRange = Target.ElseSrc()->getSourceRange();
  auto ElseCharRange = Lexer::getCharSourceRangeFromSourceRange(SM, ElseRange);

  CharSourceRange DestCharRange;

  if (Target.Binding) {
    auto DestRange = Target.Binding->getSourceRange();
    DestCharRange = Lexer::getCharSourceRangeFromSourceRange(SM, DestRange);
    ReplaceRange.widen(DestCharRange);
  } else {
    auto DestRange = Target.AssignDest()->getSourceRange();
    DestCharRange = Lexer::getCharSourceRangeFromSourceRange(SM, DestRange);
  }

  OS << DestCharRange.str() << Space << tok::equal << Space;
  OS << CondCharRange.str() << Space << tok::question_postfix << Space;
  OS << ThenCharRange.str() << Space << tok::colon << Space;
  OS << ElseCharRange.str();

  EditConsumer.accept(SM, ReplaceRange, DeclBuffer.str());

  return false; //don't abort
}

/// The helper class analyzes a given nominal decl or an extension decl to
/// decide whether stubs are required to filled in and the context in which
/// these stubs should be filled.
class FillProtocolStubContext {

  std::vector<ValueDecl*>
  getUnsatisfiedRequirements(const IterableDeclContext *IDC);

  /// Context in which the content should be filled; this could be either a
  /// nominal type declaraion or an extension declaration.
  DeclContext *DC;

  /// The type that adopts the required protocol stubs. For nominal type decl, this
  /// should be the declared type itself; for extension decl, this should be the
  /// extended type at hand.
  Type Adopter;

  /// The start location of the decl, either nominal type or extension, for the
  /// printer to figure out the right indentation.
  SourceLoc StartLoc;

  /// The location of '{' for the decl, thus we know where to insert the filling
  /// stubs.
  SourceLoc BraceStartLoc;

  /// The value decls that should be satisfied; this could be either function
  /// decls, property decls, or required type alias.
  std::vector<ValueDecl*> FillingContents;

public:
  FillProtocolStubContext(ExtensionDecl *ED) : DC(ED),
    Adopter(ED->getExtendedType()), StartLoc(ED->getStartLoc()),
    BraceStartLoc(ED->getBraces().Start),
    FillingContents(getUnsatisfiedRequirements(ED)) {};

  FillProtocolStubContext(NominalTypeDecl *ND) : DC(ND),
    Adopter(ND->getDeclaredType()), StartLoc(ND->getStartLoc()),
    BraceStartLoc(ND->getBraces().Start),
    FillingContents(getUnsatisfiedRequirements(ND)) {};

  FillProtocolStubContext() : DC(nullptr), Adopter(), FillingContents({}) {};

  static FillProtocolStubContext getContextFromCursorInfo(
      const ResolvedCursorInfo &Tok);

  ArrayRef<ValueDecl*> getFillingContents() const {
    return llvm::makeArrayRef(FillingContents);
  }

  DeclContext *getFillingContext() const { return DC; }

  bool canProceed() const {
    return StartLoc.isValid() && BraceStartLoc.isValid() &&
      !getFillingContents().empty();
  }

  Type getAdopter() const { return Adopter; }
  SourceLoc getContextStartLoc() const { return StartLoc; }
  SourceLoc getBraceStartLoc() const { return BraceStartLoc; }
};

FillProtocolStubContext FillProtocolStubContext::
getContextFromCursorInfo(const ResolvedCursorInfo &CursorInfo) {
  if(!CursorInfo.isValid())
    return FillProtocolStubContext();
  if (!CursorInfo.IsRef) {
    // If the type name is on the declared nominal, e.g. "class A {}"
    if (auto ND = dyn_cast<NominalTypeDecl>(CursorInfo.ValueD)) {
      return FillProtocolStubContext(ND);
    }
  } else if (auto *ED = CursorInfo.ExtTyRef) {
    // If the type ref is on a declared extension, e.g. "extension A {}"
    return FillProtocolStubContext(ED);
  }
  return FillProtocolStubContext();
}

std::vector<ValueDecl*> FillProtocolStubContext::
getUnsatisfiedRequirements(const IterableDeclContext *IDC) {
  // The results to return.
  std::vector<ValueDecl*> NonWitnessedReqs;

  // For each conformance of the extended nominal.
  for(ProtocolConformance *Con : IDC->getLocalConformances()) {

    // Collect non-witnessed requirements.
    Con->forEachNonWitnessedRequirement(
      [&](ValueDecl *VD) { NonWitnessedReqs.push_back(VD); });
  }

  return NonWitnessedReqs;
}

bool RefactoringActionFillProtocolStub::
isApplicable(const ResolvedCursorInfo &Tok, DiagnosticEngine &Diag) {
  return FillProtocolStubContext::getContextFromCursorInfo(Tok).canProceed();
};

bool RefactoringActionFillProtocolStub::performChange() {
  // Get the filling protocol context from the input token.
  FillProtocolStubContext Context = FillProtocolStubContext::
    getContextFromCursorInfo(CursorInfo);

  assert(Context.canProceed());
  assert(!Context.getFillingContents().empty());
  assert(Context.getFillingContext());
  SmallString<128> Text;
  {
    llvm::raw_svector_ostream SS(Text);
    Type Adopter = Context.getAdopter();
    SourceLoc Loc = Context.getContextStartLoc();
    auto Contents = Context.getFillingContents();

    // For each unsatisfied requirement, print the stub to the buffer.
    std::for_each(Contents.begin(), Contents.end(), [&](ValueDecl *VD) {
      printRequirementStub(VD, Context.getFillingContext(), Adopter, Loc, SS);
    });
  }

  // Insert all stubs after '{' in the extension/nominal type decl.
  EditConsumer.insertAfter(SM, Context.getBraceStartLoc(), Text);
  return false;
}

static void collectAvailableRefactoringsAtCursor(
    SourceFile *SF, unsigned Line, unsigned Column,
    SmallVectorImpl<RefactoringKind> &Kinds,
    ArrayRef<DiagnosticConsumer *> DiagConsumers) {
  // Prepare the tool box.
  ASTContext &Ctx = SF->getASTContext();
  SourceManager &SM = Ctx.SourceMgr;
  DiagnosticEngine DiagEngine(SM);
  std::for_each(DiagConsumers.begin(), DiagConsumers.end(),
                [&](DiagnosticConsumer *Con) { DiagEngine.addConsumer(*Con); });
  SourceLoc Loc = SM.getLocForLineCol(SF->getBufferID().getValue(), Line, Column);
  if (Loc.isInvalid())
    return;

  ResolvedCursorInfo Tok = evaluateOrDefault(SF->getASTContext().evaluator,
    CursorInfoRequest{CursorInfoOwner(SF, Lexer::getLocForStartOfToken(SM, Loc))},
                                             ResolvedCursorInfo());
  collectAvailableRefactorings(Tok, Kinds, /*Exclude rename*/ false);
}

static EnumDecl* getEnumDeclFromSwitchStmt(SwitchStmt *SwitchS) {
  if (auto SubjectTy = SwitchS->getSubjectExpr()->getType()) {
    // FIXME: Support more complex subject like '(Enum1, Enum2)'.
    return dyn_cast_or_null<EnumDecl>(SubjectTy->getAnyNominal());
  }
  return nullptr;
}

static bool performCasesExpansionInSwitchStmt(SwitchStmt *SwitchS,
                                              DiagnosticEngine &DiagEngine,
                                              SourceLoc ExpandedStmtLoc,
                                              EditorConsumerInsertStream &OS
                                              ) {
  // Assume enum elements are not handled in the switch statement.
  auto EnumDecl = getEnumDeclFromSwitchStmt(SwitchS);
  assert(EnumDecl);
  llvm::DenseSet<EnumElementDecl*> UnhandledElements;
  EnumDecl->getAllElements(UnhandledElements);
  for (auto Current : SwitchS->getCases()) {
    if (Current->isDefault()) {
      continue;
    }
    // For each handled enum element, remove it from the bucket.
    for (auto Item : Current->getCaseLabelItems()) {
      if (auto *EEP = dyn_cast_or_null<EnumElementPattern>(Item.getPattern())) {
        UnhandledElements.erase(EEP->getElementDecl());
      }
    }
  }

  // If all enum elements are handled in the switch statement, issue error.
  if (UnhandledElements.empty()) {
    DiagEngine.diagnose(ExpandedStmtLoc, diag::no_remaining_cases);
    return true;
  }

  printEnumElementsAsCases(UnhandledElements, OS);
  return false;
}

// Finds SwitchStmt that contains given CaseStmt.
static SwitchStmt* findEnclosingSwitchStmt(CaseStmt *CS,
                                           SourceFile *SF,
                                           DiagnosticEngine &DiagEngine) {
  auto IsSwitch = [](ASTNode Node) {
    return Node.is<Stmt*>() &&
    Node.get<Stmt*>()->getKind() == StmtKind::Switch;
  };
  ContextFinder Finder(*SF, CS, IsSwitch);
  Finder.resolve();

  // If failed to find the switch statement, issue error.
  if (Finder.getContexts().empty()) {
    DiagEngine.diagnose(CS->getStartLoc(), diag::no_parent_switch);
    return nullptr;
  }
  auto *SwitchS = static_cast<SwitchStmt*>(Finder.getContexts().back().
                                           get<Stmt*>());
  // Make sure that CaseStmt is included in switch that was found.
  auto Cases = SwitchS->getCases();
  auto Default = std::find(Cases.begin(), Cases.end(), CS);
  if (Default == Cases.end()) {
    DiagEngine.diagnose(CS->getStartLoc(), diag::no_parent_switch);
    return nullptr;
  }
  return SwitchS;
}

bool RefactoringActionExpandDefault::
isApplicable(const ResolvedCursorInfo &CursorInfo, DiagnosticEngine &Diag) {
  auto Exit = [&](bool Applicable) {
    if (!Applicable)
      Diag.diagnose(SourceLoc(), diag::invalid_default_location);
    return Applicable;
  };
  if (CursorInfo.Kind != CursorInfoKind::StmtStart)
    return Exit(false);
  if (auto *CS = dyn_cast<CaseStmt>(CursorInfo.TrailingStmt)) {
    auto EnclosingSwitchStmt = findEnclosingSwitchStmt(CS,
                                                       CursorInfo.SF,
                                                       Diag);
    if (!EnclosingSwitchStmt)
      return false;
    auto EnumD = getEnumDeclFromSwitchStmt(EnclosingSwitchStmt);
    auto IsApplicable = CS->isDefault() && EnumD != nullptr;
    return IsApplicable;
  }
  return Exit(false);
}

bool RefactoringActionExpandDefault::performChange() {
  // If we've not seen the default statement inside the switch statement, issue
  // error.
  auto *CS = static_cast<CaseStmt*>(CursorInfo.TrailingStmt);
  auto *SwitchS = findEnclosingSwitchStmt(CS, TheFile, DiagEngine);
  assert(SwitchS);
  EditorConsumerInsertStream OS(EditConsumer, SM,
                                Lexer::getCharSourceRangeFromSourceRange(SM,
                                  CS->getLabelItemsRange()));
  return performCasesExpansionInSwitchStmt(SwitchS,
                                           DiagEngine,
                                           CS->getStartLoc(),
                                           OS);
}

bool RefactoringActionExpandSwitchCases::
isApplicable(const ResolvedCursorInfo &CursorInfo, DiagnosticEngine &DiagEngine) {
  if (!CursorInfo.TrailingStmt)
    return false;
  if (auto *Switch = dyn_cast<SwitchStmt>(CursorInfo.TrailingStmt)) {
    return getEnumDeclFromSwitchStmt(Switch);
  }
  return false;
}

bool RefactoringActionExpandSwitchCases::performChange() {
  auto *SwitchS = dyn_cast<SwitchStmt>(CursorInfo.TrailingStmt);
  assert(SwitchS);

  auto InsertRange = CharSourceRange();
  auto Cases = SwitchS->getCases();
  auto Default = std::find_if(Cases.begin(), Cases.end(), [](CaseStmt *Stmt) {
    return Stmt->isDefault();
  });
  if (Default != Cases.end()) {
    auto DefaultRange = (*Default)->getLabelItemsRange();
    InsertRange = Lexer::getCharSourceRangeFromSourceRange(SM, DefaultRange);
  } else {
    auto RBraceLoc = SwitchS->getRBraceLoc();
    InsertRange = CharSourceRange(SM, RBraceLoc, RBraceLoc);
  }
  EditorConsumerInsertStream OS(EditConsumer, SM, InsertRange);
  if (SM.getLineAndColumnInBuffer(SwitchS->getLBraceLoc()).first ==
      SM.getLineAndColumnInBuffer(SwitchS->getRBraceLoc()).first) {
    OS << "\n";
  }
  auto Result = performCasesExpansionInSwitchStmt(SwitchS,
                                           DiagEngine,
                                           SwitchS->getStartLoc(),
                                           OS);
  return Result;
}

static Expr *findLocalizeTarget(const ResolvedCursorInfo &CursorInfo) {
  if (CursorInfo.Kind != CursorInfoKind::ExprStart)
    return nullptr;
  struct StringLiteralFinder: public SourceEntityWalker {
    SourceLoc StartLoc;
    Expr *Target;
    StringLiteralFinder(SourceLoc StartLoc): StartLoc(StartLoc), Target(nullptr) {}
    bool walkToExprPre(Expr *E) override {
      if (E->getStartLoc() != StartLoc)
        return false;
      if (E->getKind() == ExprKind::InterpolatedStringLiteral)
        return false;
      if (E->getKind() == ExprKind::StringLiteral) {
        Target = E;
        return false;
      }
      return true;
    }
  } Walker(CursorInfo.TrailingExpr->getStartLoc());
  Walker.walk(CursorInfo.TrailingExpr);
  return Walker.Target;
}

bool RefactoringActionLocalizeString::
isApplicable(const ResolvedCursorInfo &Tok, DiagnosticEngine &Diag) {
  return findLocalizeTarget(Tok);
}

bool RefactoringActionLocalizeString::performChange() {
  Expr* Target = findLocalizeTarget(CursorInfo);
   if (!Target)
    return true;
  EditConsumer.accept(SM, Target->getStartLoc(), "NSLocalizedString(");
  EditConsumer.insertAfter(SM, Target->getEndLoc(), ", comment: \"\")");
  return false;
}

struct MemberwiseParameter {
  Identifier Name;
  Type MemberType;
  Expr *DefaultExpr;

  MemberwiseParameter(Identifier name, Type type, Expr *initialExpr)
      : Name(name), MemberType(type), DefaultExpr(initialExpr) {}
};

static void generateMemberwiseInit(SourceEditConsumer &EditConsumer,
                                   SourceManager &SM,
                                   ArrayRef<MemberwiseParameter> memberVector,
                                   SourceLoc targetLocation) {
  
  assert(!memberVector.empty());

  EditConsumer.accept(SM, targetLocation, "\ninternal init(");
  auto insertMember = [&SM](const MemberwiseParameter &memberData,
                            raw_ostream &OS, bool wantsSeparator) {
    {
      OS << memberData.Name << ": ";
      // Unconditionally print '@escaping' if we print out a function type -
      // the assignments we generate below will escape this parameter.
      if (isa<AnyFunctionType>(memberData.MemberType->getCanonicalType())) {
        OS << "@" << TypeAttributes::getAttrName(TAK_escaping) << " ";
      }
      OS << memberData.MemberType.getString();
    }

    if (auto *expr = memberData.DefaultExpr) {
      if (isa<NilLiteralExpr>(expr)) {
        OS << " = nil";
      } else if (expr->getSourceRange().isValid()) {
        auto range =
          Lexer::getCharSourceRangeFromSourceRange(
            SM, expr->getSourceRange());
        OS << " = " << SM.extractText(range);
      }
    }

    if (wantsSeparator) {
      OS << ", ";
    }
  };

  // Process the initial list of members, inserting commas as appropriate.
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  for (const auto &memberData : memberVector.drop_back()) {
    insertMember(memberData, OS, /*wantsSeparator*/ true);
  }

  // Process the last (or perhaps, only) member.
  insertMember(memberVector.back(), OS, /*wantsSeparator*/ false);

  // Synthesize the body.
  OS << ") {\n";
  for (auto &member : memberVector) {
    // self.<property> = <property>
    OS << "self." << member.Name << " = " << member.Name << "\n";
  }
  OS << "}\n";

  // Accept the entire edit.
  EditConsumer.accept(SM, targetLocation, OS.str());
}

static SourceLoc
collectMembersForInit(const ResolvedCursorInfo &CursorInfo,
                      SmallVectorImpl<MemberwiseParameter> &memberVector) {

  if (!CursorInfo.ValueD)
    return SourceLoc();
  
  NominalTypeDecl *nominalDecl = dyn_cast<NominalTypeDecl>(CursorInfo.ValueD);
  if (!nominalDecl || nominalDecl->getStoredProperties().empty() ||
      CursorInfo.IsRef) {
    return SourceLoc();
  }

  SourceLoc bracesStart = nominalDecl->getBraces().Start;
  if (!bracesStart.isValid())
    return SourceLoc();
  
  SourceLoc targetLocation = bracesStart.getAdvancedLoc(1);
  if (!targetLocation.isValid())
    return SourceLoc();

  for (auto varDecl : nominalDecl->getStoredProperties()) {
    auto patternBinding = varDecl->getParentPatternBinding();
    if (!patternBinding)
      continue;

    if (!varDecl->isMemberwiseInitialized(/*preferDeclaredProperties=*/true)) {
      continue;
    }

    const auto i = patternBinding->getPatternEntryIndexForVarDecl(varDecl);
    Expr *defaultInit = nullptr;
    if (patternBinding->isExplicitlyInitialized(i) ||
        patternBinding->isDefaultInitializable()) {
      defaultInit = varDecl->getParentInitializer();
    }

    memberVector.emplace_back(varDecl->getName(),
                              varDecl->getType(), defaultInit);
  }
  
  if (memberVector.empty()) {
    return SourceLoc();
  }
  
  return targetLocation;
}

bool RefactoringActionMemberwiseInitLocalRefactoring::
isApplicable(const ResolvedCursorInfo &Tok, DiagnosticEngine &Diag) {
  
  SmallVector<MemberwiseParameter, 8> memberVector;
  return collectMembersForInit(Tok, memberVector).isValid();
}
    
bool RefactoringActionMemberwiseInitLocalRefactoring::performChange() {
  
  SmallVector<MemberwiseParameter, 8> memberVector;
  SourceLoc targetLocation = collectMembersForInit(CursorInfo, memberVector);
  if (targetLocation.isInvalid())
    return true;
  
  generateMemberwiseInit(EditConsumer, SM, memberVector, targetLocation);
  
  return false;
}

class AddEquatableContext {

  /// Declaration context
  DeclContext *DC;

  /// Adopter type
  Type Adopter;

  /// Start location of declaration context brace
  SourceLoc StartLoc;

  /// Array of all inherited protocols' locations
  ArrayRef<TypeLoc> ProtocolsLocations;

  /// Array of all conformed protocols
  SmallVector<swift::ProtocolDecl *, 2> Protocols;

  /// Start location of declaration,
  /// a place to write protocol name
  SourceLoc ProtInsertStartLoc;

  /// Stored properties of extending adopter
  ArrayRef<VarDecl *> StoredProperties;

  /// Range of internal members in declaration
  DeclRange Range;

  bool conformsToEquatableProtocol() {
    for (ProtocolDecl *Protocol : Protocols) {
      if (Protocol->getKnownProtocolKind() == KnownProtocolKind::Equatable) {
        return true;
      }
    }
    return false;
  }

  bool isRequirementValid() {
    auto Reqs = getProtocolRequirements();
    if (Reqs.empty()) {
      return false;
    }
    auto Req = dyn_cast<FuncDecl>(Reqs[0]);
    return Req && Req->getParameters()->size() == 2;
  }

  bool isPropertiesListValid() {
    return !getUserAccessibleProperties().empty();
  }

  void printFunctionBody(ASTPrinter &Printer, StringRef ExtraIndent,
  ParameterList *Params);

  std::vector<ValueDecl *> getProtocolRequirements();

  std::vector<VarDecl *> getUserAccessibleProperties();

public:

  AddEquatableContext(NominalTypeDecl *Decl) : DC(Decl),
  Adopter(Decl->getDeclaredType()), StartLoc(Decl->getBraces().Start),
  ProtocolsLocations(Decl->getInherited()),
  Protocols(Decl->getAllProtocols()), ProtInsertStartLoc(Decl->getNameLoc()),
  StoredProperties(Decl->getStoredProperties()), Range(Decl->getMembers()) {};

  AddEquatableContext(ExtensionDecl *Decl) : DC(Decl),
  Adopter(Decl->getExtendedType()), StartLoc(Decl->getBraces().Start),
  ProtocolsLocations(Decl->getInherited()),
  Protocols(Decl->getExtendedNominal()->getAllProtocols()),
  ProtInsertStartLoc(Decl->getExtendedTypeRepr()->getEndLoc()),
  StoredProperties(Decl->getExtendedNominal()->getStoredProperties()), Range(Decl->getMembers()) {};

  AddEquatableContext() : DC(nullptr), Adopter(), ProtocolsLocations(),
  Protocols(), StoredProperties(), Range(nullptr, nullptr) {};

  static AddEquatableContext getDeclarationContextFromInfo(ResolvedCursorInfo Info);

  std::string getInsertionTextForProtocol();

  std::string getInsertionTextForFunction(SourceManager &SM);

  bool isValid() {
    // FIXME: Allow to generate explicit == method for declarations which already have
    // compiler-generated == method
    return StartLoc.isValid() && ProtInsertStartLoc.isValid() &&
    !conformsToEquatableProtocol() && isPropertiesListValid() &&
    isRequirementValid();
  }

  SourceLoc getStartLocForProtocolDecl() {
    if (ProtocolsLocations.empty()) {
      return ProtInsertStartLoc;
    }
    return ProtocolsLocations.back().getSourceRange().Start;
  }

  bool isMembersRangeEmpty() {
    return Range.empty();
  }

  SourceLoc getInsertStartLoc();
};

SourceLoc AddEquatableContext::
getInsertStartLoc() {
  SourceLoc MaxLoc = StartLoc;
  for (auto Mem : Range) {
    if (Mem->getEndLoc().getOpaquePointerValue() >
        MaxLoc.getOpaquePointerValue()) {
      MaxLoc = Mem->getEndLoc();
    }
  }
  return MaxLoc;
}

std::string AddEquatableContext::
getInsertionTextForProtocol() {
  StringRef ProtocolName = getProtocolName(KnownProtocolKind::Equatable);
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  if (ProtocolsLocations.empty()) {
    OS << ": " << ProtocolName;
    return Buffer;
  }
  OS << ", " << ProtocolName;
  return Buffer;
}

std::string AddEquatableContext::
getInsertionTextForFunction(SourceManager &SM) {
  auto Reqs = getProtocolRequirements();
  auto Req = dyn_cast<FuncDecl>(Reqs[0]);
  auto Params = Req->getParameters();
  StringRef ExtraIndent;
  StringRef CurrentIndent =
    Lexer::getIndentationForLine(SM, getInsertStartLoc(), &ExtraIndent);
  std::string Indent;
  if (isMembersRangeEmpty()) {
    Indent = (CurrentIndent + ExtraIndent).str();
  } else {
    Indent = CurrentIndent.str();
  }
  PrintOptions Options = PrintOptions::printVerbose();
  Options.PrintDocumentationComments = false;
  Options.setBaseType(Adopter);
  Options.FunctionBody = [&](const ValueDecl *VD, ASTPrinter &Printer) {
    Printer << " {";
    Printer.printNewline();
    printFunctionBody(Printer, ExtraIndent, Params);
    Printer.printNewline();
    Printer << "}";
  };
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  ExtraIndentStreamPrinter Printer(OS, Indent);
  Printer.printNewline();
  if (!isMembersRangeEmpty()) {
    Printer.printNewline();
  }
  Reqs[0]->print(Printer, Options);
  return Buffer;
}

std::vector<VarDecl *> AddEquatableContext::
getUserAccessibleProperties() {
  std::vector<VarDecl *> PublicProperties;
  for (VarDecl *Decl : StoredProperties) {
    if (Decl->Decl::isUserAccessible()) {
      PublicProperties.push_back(Decl);
    }
  }
  return PublicProperties;
}

std::vector<ValueDecl *> AddEquatableContext::
getProtocolRequirements() {
  std::vector<ValueDecl *> Collection;
  auto Proto = DC->getASTContext().getProtocol(KnownProtocolKind::Equatable);
  for (auto Member : Proto->getMembers()) {
    auto Req = dyn_cast<ValueDecl>(Member);
    if (!Req || Req->isInvalid() || !Req->isProtocolRequirement()) {
      continue;
    }
    Collection.push_back(Req);
  }
  return Collection;
}

AddEquatableContext AddEquatableContext::
getDeclarationContextFromInfo(ResolvedCursorInfo Info) {
  if (Info.isInvalid()) {
    return AddEquatableContext();
  }
  if (!Info.IsRef) {
    if (auto *NomDecl = dyn_cast<NominalTypeDecl>(Info.ValueD)) {
      return AddEquatableContext(NomDecl);
    }
  } else if (auto *ExtDecl = Info.ExtTyRef) {
    if (ExtDecl->getExtendedNominal()) {
      return AddEquatableContext(ExtDecl);
    }
  }
  return AddEquatableContext();
}

void AddEquatableContext::
printFunctionBody(ASTPrinter &Printer, StringRef ExtraIndent, ParameterList *Params) {
  SmallString<128> Return;
  llvm::raw_svector_ostream SS(Return);
  SS << tok::kw_return;
  StringRef Space = " ";
  StringRef AdditionalSpace = "       ";
  StringRef Point = ".";
  StringRef Join = " == ";
  StringRef And = " &&";
  auto Props = getUserAccessibleProperties();
  auto FParam = Params->get(0)->getName();
  auto SParam = Params->get(1)->getName();
  auto Prop = Props[0]->getName();
  Printer << ExtraIndent << Return << Space
  << FParam << Point << Prop << Join << SParam << Point << Prop;
  if (Props.size() > 1) {
    std::for_each(Props.begin() + 1, Props.end(), [&](VarDecl *VD){
      auto Name = VD->getName();
      Printer << And;
      Printer.printNewline();
      Printer << ExtraIndent << AdditionalSpace << FParam << Point
      << Name << Join << SParam << Point << Name;
    });
  }
}

bool RefactoringActionAddEquatableConformance::
isApplicable(const ResolvedCursorInfo &Tok, DiagnosticEngine &Diag) {
  return AddEquatableContext::getDeclarationContextFromInfo(Tok).isValid();
}

bool RefactoringActionAddEquatableConformance::
performChange() {
  auto Context = AddEquatableContext::getDeclarationContextFromInfo(CursorInfo);
  EditConsumer.insertAfter(SM, Context.getStartLocForProtocolDecl(),
                           Context.getInsertionTextForProtocol());
  EditConsumer.insertAfter(SM, Context.getInsertStartLoc(),
                           Context.getInsertionTextForFunction(SM));
  return false;
}

static CharSourceRange
findSourceRangeToWrapInCatch(const ResolvedCursorInfo &CursorInfo,
                             SourceFile *TheFile,
                             SourceManager &SM) {
  Expr *E = CursorInfo.TrailingExpr;
  if (!E)
    return CharSourceRange();
  auto Node = ASTNode(E);
  auto NodeChecker = [](ASTNode N) { return N.isStmt(StmtKind::Brace); };
  ContextFinder Finder(*TheFile, Node, NodeChecker);
  Finder.resolve();
  auto Contexts = Finder.getContexts();
  if (Contexts.empty())
    return CharSourceRange();
  auto TargetNode = Contexts.back();
  BraceStmt *BStmt = dyn_cast<BraceStmt>(TargetNode.dyn_cast<Stmt*>());
  auto ConvertToCharRange = [&SM](SourceRange SR) {
    return Lexer::getCharSourceRangeFromSourceRange(SM, SR);
  };
  assert(BStmt);
  auto ExprRange = ConvertToCharRange(E->getSourceRange());
  // Check elements of the deepest BraceStmt, pick one that covers expression.
  for (auto Elem: BStmt->getElements()) {
    auto ElemRange = ConvertToCharRange(Elem.getSourceRange());
    if (ElemRange.contains(ExprRange))
      TargetNode = Elem;
  }
  return ConvertToCharRange(TargetNode.getSourceRange());
}

bool RefactoringActionConvertToDoCatch::
isApplicable(const ResolvedCursorInfo &Tok, DiagnosticEngine &Diag) {
  if (!Tok.TrailingExpr)
    return false;
  return isa<ForceTryExpr>(Tok.TrailingExpr);
}

bool RefactoringActionConvertToDoCatch::performChange() {
  auto *TryExpr = dyn_cast<ForceTryExpr>(CursorInfo.TrailingExpr);
  assert(TryExpr);
  auto Range = findSourceRangeToWrapInCatch(CursorInfo, TheFile, SM);
  if (!Range.isValid())
    return true;
  // Wrap given range in do catch block.
  EditConsumer.accept(SM, Range.getStart(), "do {\n");
  EditorConsumerInsertStream OS(EditConsumer, SM, Range.getEnd());
  OS << "\n} catch {\n" << getCodePlaceholder() << "\n}";

  // Delete ! from try! expression
  auto ExclaimLen = getKeywordLen(tok::exclaim_postfix);
  auto ExclaimRange = CharSourceRange(TryExpr->getExclaimLoc(), ExclaimLen);
  EditConsumer.remove(SM, ExclaimRange);
  return false;
}

/// Given a cursor position, this function tries to collect a number literal
/// expression immediately following the cursor.
static NumberLiteralExpr *getTrailingNumberLiteral(
    const ResolvedCursorInfo &Tok) {
  // This cursor must point to the start of an expression.
  if (Tok.Kind != CursorInfoKind::ExprStart)
    return nullptr;

  // For every sub-expression, try to find the literal expression that matches
  // our criteria.
  class FindLiteralNumber : public ASTWalker {
    Expr * const parent;

  public:
    NumberLiteralExpr *found = nullptr;

    explicit FindLiteralNumber(Expr *parent) : parent(parent) { }

    std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {
      if (auto *literal = dyn_cast<NumberLiteralExpr>(expr)) {
        // The sub-expression must have the same start loc with the outermost
        // expression, i.e. the cursor position.
        if (!found &&
            parent->getStartLoc().getOpaquePointerValue() ==
              expr->getStartLoc().getOpaquePointerValue()) {
          found = literal;
        }
      }

      return { found == nullptr, expr };
    }
  };

  auto parent = Tok.TrailingExpr;
  FindLiteralNumber finder(parent);
  parent->walk(finder);
  return finder.found;
}

static std::string insertUnderscore(StringRef Text) {
  SmallString<64> Buffer;
  llvm::raw_svector_ostream OS(Buffer);
  for (auto It = Text.begin(); It != Text.end(); ++It) {
    unsigned Distance = It - Text.begin();
    if (Distance && !(Distance % 3)) {
      OS << '_';
    }
    OS << *It;
  }
  return OS.str().str();
}

void insertUnderscoreInDigits(StringRef Digits,
                              raw_ostream &OS) {
  StringRef BeforePointRef, AfterPointRef;
  std::tie(BeforePointRef, AfterPointRef) = Digits.split('.');

  std::string BeforePoint(BeforePointRef);
  std::string AfterPoint(AfterPointRef);

  // Insert '_' for the part before the decimal point.
  std::reverse(BeforePoint.begin(), BeforePoint.end());
  BeforePoint = insertUnderscore(BeforePoint);
  std::reverse(BeforePoint.begin(), BeforePoint.end());
  OS << BeforePoint;

  // Insert '_' for the part after the decimal point, if necessary.
  if (!AfterPoint.empty()) {
    OS << '.';
    OS << insertUnderscore(AfterPoint);
  }
}

bool RefactoringActionSimplifyNumberLiteral::
isApplicable(const ResolvedCursorInfo &Tok, DiagnosticEngine &Diag) {
  if (auto *Literal = getTrailingNumberLiteral(Tok)) {
    SmallString<64> Buffer;
    llvm::raw_svector_ostream OS(Buffer);
    StringRef Digits = Literal->getDigitsText();
    insertUnderscoreInDigits(Digits, OS);

    // If inserting '_' results in a different digit sequence, this refactoring
    // is applicable.
    return OS.str() != Digits;
  }
  return false;
}

bool RefactoringActionSimplifyNumberLiteral::performChange() {
  if (auto *Literal = getTrailingNumberLiteral(CursorInfo)) {

    EditorConsumerInsertStream OS(EditConsumer, SM,
                                  CharSourceRange(SM, Literal->getDigitsLoc(),
                                  Lexer::getLocForEndOfToken(SM,
                                    Literal->getEndLoc())));
    StringRef Digits = Literal->getDigitsText();
    insertUnderscoreInDigits(Digits, OS);
    return false;
  }
  return true;
}

static CallExpr *findTrailingClosureTarget(
    SourceManager &SM, const ResolvedCursorInfo &CursorInfo) {
  if (CursorInfo.Kind == CursorInfoKind::StmtStart)
    // StmtStart postion can't be a part of CallExpr.
    return nullptr;

  // Find inner most CallExpr
  ContextFinder
  Finder(*CursorInfo.SF, CursorInfo.Loc,
         [](ASTNode N) {
           return N.isStmt(StmtKind::Brace) || N.isExpr(ExprKind::Call);
         });
  Finder.resolve();
  auto contexts = Finder.getContexts();
  if (contexts.empty())
    return nullptr;

  // If the innermost context is a statement (which will be a BraceStmt per
  // the filtering condition above), drop it.
  if (contexts.back().is<Stmt *>()) {
    contexts = contexts.drop_back();
  }

  if (contexts.empty() || !contexts.back().is<Expr*>())
    return nullptr;
  CallExpr *CE = cast<CallExpr>(contexts.back().get<Expr*>());

  if (CE->hasTrailingClosure())
    // Call expression already has a trailing closure.
    return nullptr;

  // The last argument is a closure?
  Expr *Args = CE->getArg();
  if (!Args)
    return nullptr;
  Expr *LastArg;
  if (auto *PE = dyn_cast<ParenExpr>(Args)) {
    LastArg = PE->getSubExpr();
  } else {
    auto *TE = cast<TupleExpr>(Args);
    if (TE->getNumElements() == 0)
      return nullptr;
    LastArg = TE->getElements().back();
  }

  if (auto *ICE = dyn_cast<ImplicitConversionExpr>(LastArg))
    LastArg = ICE->getSyntacticSubExpr();

  if (isa<ClosureExpr>(LastArg) || isa<CaptureListExpr>(LastArg))
    return CE;
  return nullptr;
}

bool RefactoringActionTrailingClosure::
isApplicable(const ResolvedCursorInfo &CursorInfo, DiagnosticEngine &Diag) {
  SourceManager &SM = CursorInfo.SF->getASTContext().SourceMgr;
  return findTrailingClosureTarget(SM, CursorInfo);
}

bool RefactoringActionTrailingClosure::performChange() {
  auto *CE = findTrailingClosureTarget(SM, CursorInfo);
  if (!CE)
    return true;
  Expr *Arg = CE->getArg();

  Expr *ClosureArg = nullptr;
  Expr *PrevArg = nullptr;

  OriginalArgumentList ArgList = getOriginalArgumentList(Arg);

  auto NumArgs = ArgList.args.size();
  if (NumArgs == 0)
    return true;
  ClosureArg = ArgList.args[NumArgs - 1];
  if (NumArgs > 1)
    PrevArg = ArgList.args[NumArgs - 2];

  if (auto *ICE = dyn_cast<ImplicitConversionExpr>(ClosureArg))
    ClosureArg = ICE->getSyntacticSubExpr();

  if (ArgList.lParenLoc.isInvalid() || ArgList.rParenLoc.isInvalid())
    return true;

  // Replace:
  //   * Open paren with ' ' if the closure is sole argument.
  //   * Comma with ') ' otherwise.
  if (PrevArg) {
    CharSourceRange PreRange(
        SM,
        Lexer::getLocForEndOfToken(SM, PrevArg->getEndLoc()),
        ClosureArg->getStartLoc());
    EditConsumer.accept(SM, PreRange, ") ");
  } else {
    CharSourceRange PreRange(
        SM, ArgList.lParenLoc, ClosureArg->getStartLoc());
    EditConsumer.accept(SM, PreRange, " ");
  }
  // Remove original closing paren.
  CharSourceRange PostRange(
      SM,
      Lexer::getLocForEndOfToken(SM, ClosureArg->getEndLoc()),
      Lexer::getLocForEndOfToken(SM, ArgList.rParenLoc));
  EditConsumer.remove(SM, PostRange);
  return false;
}

static bool rangeStartMayNeedRename(const ResolvedRangeInfo &Info) {
  switch(Info.Kind) {
    case RangeKind::SingleExpression: {
      Expr *E = Info.ContainedNodes[0].get<Expr*>();
      // We should show rename for the selection of "foo()"
      if (auto *CE = dyn_cast<CallExpr>(E)) {
        if (CE->getFn()->getKind() == ExprKind::DeclRef)
          return true;

        // When callling an instance method inside another instance method,
        // we have a dot syntax call whose dot and base are both implicit. We
        // need to explicitly allow the specific case here.
        if (auto *DSC = dyn_cast<DotSyntaxCallExpr>(CE->getFn())) {
          if (DSC->getBase()->isImplicit() &&
              DSC->getFn()->getStartLoc() == Info.TokensInRange.front().getLoc())
            return true;
        }
      }
      return false;
    }
    case RangeKind::PartOfExpression: {
      if (auto *CE = dyn_cast<CallExpr>(Info.CommonExprParent)) {
        if (auto *DSC = dyn_cast<DotSyntaxCallExpr>(CE->getFn())) {
          if (DSC->getFn()->getStartLoc() == Info.TokensInRange.front().getLoc())
            return true;
        }
      }
      return false;
    }
    case RangeKind::SingleDecl:
    case RangeKind::MultiTypeMemberDecl:
    case RangeKind::SingleStatement:
    case RangeKind::MultiStatement:
    case RangeKind::Invalid:
      return false;
  }
  llvm_unreachable("unhandled kind");
}
    
bool RefactoringActionConvertToComputedProperty::
isApplicable(const ResolvedRangeInfo &Info, DiagnosticEngine &Diag) {
  if (Info.Kind != RangeKind::SingleDecl) {
    return false;
  }
  
  if (Info.ContainedNodes.size() != 1) {
    return false;
  }
  
  auto D = Info.ContainedNodes[0].dyn_cast<Decl*>();
  if (!D) {
    return false;
  }
  
  auto Binding = dyn_cast<PatternBindingDecl>(D);
  if (!Binding) {
    return false;
  }

  auto SV = Binding->getSingleVar();
  if (!SV) {
    return false;
  }

  // willSet, didSet cannot be provided together with a getter
  for (auto AD : SV->getAllAccessors()) {
    if (AD->isObservingAccessor()) {
      return false;
    }
  }
  
  // 'lazy' must not be used on a computed property
  // NSCopying and IBOutlet attribute requires property to be mutable
  auto Attributies = SV->getAttrs();
  if (Attributies.hasAttribute<LazyAttr>() ||
      Attributies.hasAttribute<NSCopyingAttr>() ||
      Attributies.hasAttribute<IBOutletAttr>()) {
    return false;
  }

  // Property wrapper cannot be applied to a computed property
  if (SV->hasAttachedPropertyWrapper()) {
    return false;
  }

  // has an initializer
  return Binding->hasInitStringRepresentation(0);
}

bool RefactoringActionConvertToComputedProperty::performChange() {
  // Get an initialization
  auto D = RangeInfo.ContainedNodes[0].dyn_cast<Decl*>();
  auto Binding = dyn_cast<PatternBindingDecl>(D);
  SmallString<128> scratch;
  auto Init = Binding->getInitStringRepresentation(0, scratch);
  
  // Get type
  auto SV = Binding->getSingleVar();
  auto SVType = SV->getType();
  auto TR = SV->getTypeReprOrParentPatternTypeRepr();
  
  SmallString<64> DeclBuffer;
  llvm::raw_svector_ostream OS(DeclBuffer);
  StringRef Space = " ";
  StringRef NewLine = "\n";
  
  OS << tok::kw_var << Space;
  // Add var name
  OS << SV->getNameStr().str() << ":" << Space;
  // For computed property must write a type of var
  if (TR) {
    OS << Lexer::getCharSourceRangeFromSourceRange(SM, TR->getSourceRange()).str();
  } else {
    SVType.print(OS);
  }

  OS << Space << tok::l_brace << NewLine;
  // Add an initialization
  OS << tok::kw_return << Space << Init.str() << NewLine;
  OS << tok::r_brace;
  
  // Replace initializer to computed property
  auto ReplaceStartLoc = Binding->getLoc();
  auto ReplaceEndLoc = Binding->getSourceRange().End;
  auto ReplaceRange = SourceRange(ReplaceStartLoc, ReplaceEndLoc);
  auto ReplaceCharSourceRange = Lexer::getCharSourceRangeFromSourceRange(SM, ReplaceRange);
  EditConsumer.accept(SM, ReplaceCharSourceRange, DeclBuffer.str());
  return false; // success
}

namespace asyncrefactorings {

// TODO: Should probably split the refactorings into separate files

/// Whether the given type is (or conforms to) the stdlib Error type
bool isErrorType(Type Ty, ModuleDecl *MD) {
  if (!Ty)
    return false;
  return !MD->conformsToProtocol(Ty, Ty->getASTContext().getErrorDecl())
              .isInvalid();
}

// The single Decl* subject of a switch statement, or nullptr if none
Decl *singleSwitchSubject(const SwitchStmt *Switch) {
  if (auto *DRE = dyn_cast<DeclRefExpr>(Switch->getSubjectExpr()))
    return DRE->getDecl();
  return nullptr;
}

// Wrapper to make dealing with single elements easier (ie. for Paren|TupleExpr
// arguments)
template <typename T>
class PtrArrayRef {
  bool IsSingle = false;
  union Storage {
    ArrayRef<T> ManyElements;
    T SingleElement;
  } Storage = {ArrayRef<T>()};

public:
  PtrArrayRef() {}
  PtrArrayRef(T Element) : IsSingle(true) { Storage.SingleElement = Element; }
  PtrArrayRef(ArrayRef<T> Ref) : IsSingle(Ref.size() == 1), Storage({Ref}) {
    if (IsSingle)
      Storage.SingleElement = Ref[0];
  }

  ArrayRef<T> ref() {
    if (IsSingle)
      return ArrayRef<T>(Storage.SingleElement);
    return Storage.ManyElements;
  }
};

PtrArrayRef<Expr *> callArgs(const ApplyExpr *AE) {
  if (auto *PE = dyn_cast<ParenExpr>(AE->getArg())) {
    return PtrArrayRef<Expr *>(PE->getSubExpr());
  } else if (auto *TE = dyn_cast<TupleExpr>(AE->getArg())) {
    return PtrArrayRef<Expr *>(TE->getElements());
  }
  return PtrArrayRef<Expr *>();
}

FuncDecl *getUnderlyingFunc(const Expr *Fn) {
  Fn = Fn->getSemanticsProvidingExpr();
  if (auto *DRE = dyn_cast<DeclRefExpr>(Fn))
    return dyn_cast_or_null<FuncDecl>(DRE->getDecl());
  if (auto ApplyE = dyn_cast<SelfApplyExpr>(Fn))
    return getUnderlyingFunc(ApplyE->getFn());
  if (auto *ACE = dyn_cast<AutoClosureExpr>(Fn)) {
    if (auto *Unwrapped = ACE->getUnwrappedCurryThunkExpr())
      return getUnderlyingFunc(Unwrapped);
  }
  return nullptr;
}

/// Find the outermost call of the given location
CallExpr *findOuterCall(const ResolvedCursorInfo &CursorInfo) {
  auto IncludeInContext = [](ASTNode N) {
    if (auto *E = N.dyn_cast<Expr *>())
      return !E->isImplicit();
    return false;
  };

  // TODO: Bit pointless using the "ContextFinder" here. Ideally we would have
  //       already generated a slice of the AST for anything that contains
  //       the cursor location
  ContextFinder Finder(*CursorInfo.SF, CursorInfo.Loc, IncludeInContext);
  Finder.resolve();
  auto Contexts = Finder.getContexts();
  if (Contexts.empty())
    return nullptr;

  CallExpr *CE = dyn_cast<CallExpr>(Contexts[0].get<Expr *>());
  if (!CE)
    return nullptr;

  SourceManager &SM = CursorInfo.SF->getASTContext().SourceMgr;
  if (!SM.rangeContains(CE->getFn()->getSourceRange(), CursorInfo.Loc))
    return nullptr;
  return CE;
}

/// Find the function matching the given location if it is not an accessor and
/// either has a body or is a member of a protocol
FuncDecl *findFunction(const ResolvedCursorInfo &CursorInfo) {
  auto IncludeInContext = [](ASTNode N) {
    if (auto *D = N.dyn_cast<Decl *>())
      return !D->isImplicit();
    return false;
  };

  ContextFinder Finder(*CursorInfo.SF, CursorInfo.Loc, IncludeInContext);
  Finder.resolve();

  auto Contexts = Finder.getContexts();
  if (Contexts.empty())
    return nullptr;

  if (Contexts.back().isDecl(DeclKind::Param))
    Contexts = Contexts.drop_back();

  auto *FD = dyn_cast_or_null<FuncDecl>(Contexts.back().get<Decl *>());
  if (!FD || isa<AccessorDecl>(FD))
    return nullptr;

  auto *Body = FD->getBody();
  if (!Body && !isa<ProtocolDecl>(FD->getDeclContext()))
    return nullptr;

  SourceManager &SM = CursorInfo.SF->getASTContext().SourceMgr;
  SourceLoc DeclEnd = Body ? Body->getLBraceLoc() : FD->getEndLoc();
  if (!SM.rangeContains(SourceRange(FD->getStartLoc(), DeclEnd),
                        CursorInfo.Loc))
    return nullptr;

  return FD;
}

FuncDecl *isOperator(const BinaryExpr *BE) {
  auto *AE = dyn_cast<ApplyExpr>(BE->getFn());
  if (AE) {
    auto *Callee = AE->getCalledValue();
    if (Callee && Callee->isOperator() && isa<FuncDecl>(Callee))
      return cast<FuncDecl>(Callee);
  }
  return nullptr;
}

/// Describes the expressions to be kept from a call to the handler in a
/// function that has (or will have ) and async alternative. Eg.
/// ```
/// func toBeAsync(completion: (String?, Error?) -> Void) {
///   ...
///   completion("something", nil) // Result = ["something"], IsError = false
///   ...
///   completion(nil, MyError.Bad) // Result = [MyError.Bad], IsError = true
/// }
class HandlerResult {
  PtrArrayRef<Expr *> Results;
  bool IsError = false;

public:
  HandlerResult() {}

  HandlerResult(ArrayRef<Expr *> Results)
      : Results(PtrArrayRef<Expr *>(Results)) {}

  HandlerResult(Expr *Result, bool IsError)
      : Results(PtrArrayRef<Expr *>(Result)), IsError(IsError) {}

  bool isError() { return IsError; }

  ArrayRef<Expr *> args() { return Results.ref(); }
};

/// The type of the handler, ie. whether it takes regular parameters or a
/// single parameter of `Result` type.
enum class HandlerType { INVALID, PARAMS, RESULT };

/// Given a function with an async alternative (or one that *could* have an
/// async alternative), stores information about the handler parameter.
struct AsyncHandlerDesc {
  const ParamDecl *Handler = nullptr;
  int Index = -1;
  HandlerType Type = HandlerType::INVALID;
  bool HasError = false;

  static AsyncHandlerDesc find(const FuncDecl *FD, bool ignoreName) {
    if (!FD || FD->hasAsync() || FD->hasThrows())
      return AsyncHandlerDesc();

    // Require at least one parameter and void return type
    auto *Params = FD->getParameters();
    if (Params->size() == 0 || !FD->getResultInterfaceType()->isVoid())
      return AsyncHandlerDesc();

    AsyncHandlerDesc HandlerDesc;

    // Assume the handler is the last parameter for now
    HandlerDesc.Index = Params->size() - 1;
    HandlerDesc.Handler = Params->get(HandlerDesc.Index);

    // Callback must not be attributed with @autoclosure
    if (HandlerDesc.Handler->isAutoClosure())
      return AsyncHandlerDesc();

    // Callback must have a completion-like name (if we're not ignoring it)
    if (!ignoreName &&
        !isCompletionHandlerParamName(HandlerDesc.Handler->getNameStr()))
      return AsyncHandlerDesc();

    // Callback must be a function type and return void. Doesn't need to have
    // any parameters - may just be a "I'm done" callback
    auto *HandlerTy = HandlerDesc.Handler->getType()->getAs<AnyFunctionType>();
    if (!HandlerTy || !HandlerTy->getResult()->isVoid())
      return AsyncHandlerDesc();

    // Find the type of result in the handler (eg. whether it's a Result<...>,
    // just parameters, or nothing).
    auto HandlerParams = HandlerTy->getParams();
    if (HandlerParams.size() == 1) {
      auto ParamTy =
          HandlerParams.back().getPlainType()->getAs<BoundGenericType>();
      if (ParamTy && ParamTy->isResult()) {
        auto GenericArgs = ParamTy->getGenericArgs();
        assert(GenericArgs.size() == 2 && "Result should have two params");
        HandlerDesc.Type = HandlerType::RESULT;
        HandlerDesc.HasError = !GenericArgs.back()->isUninhabited();
      }
    }

    if (HandlerDesc.Type != HandlerType::RESULT) {
      // Only handle non-result parameters
      for (auto &Param : HandlerParams) {
        if (Param.getPlainType() && Param.getPlainType()->isResult())
          return AsyncHandlerDesc();
      }

      HandlerDesc.Type = HandlerType::PARAMS;
      if (!HandlerParams.empty()) {
        auto LastParamTy = HandlerParams.back().getParameterType();
        HandlerDesc.HasError = isErrorType(LastParamTy->getOptionalObjectType(),
                                           FD->getModuleContext());
      }
    }

    return HandlerDesc;
  }

  bool isValid() const { return Type != HandlerType::INVALID; }

  ArrayRef<AnyFunctionType::Param> params() const {
    auto Ty = Handler->getType()->getAs<AnyFunctionType>();
    assert(Ty && "Type must be a function type");
    return Ty->getParams();
  }

  /// The `CallExpr` if the given node is a call to the `Handler`
  CallExpr *getAsHandlerCall(ASTNode Node) const {
    if (!isValid())
      return nullptr;

    if (Node.isExpr(swift::ExprKind::Call)) {
      CallExpr *CE = cast<CallExpr>(Node.dyn_cast<Expr *>());
      if (CE->getFn()->getReferencedDecl().getDecl() == Handler)
        return CE;
    }
    return nullptr;
  }

  /// Given a call to the `Handler`, extract the expressions to be returned or
  /// thrown, taking care to remove the `.success`/`.failure` if it's a
  /// `RESULT` handler type.
  HandlerResult extractResultArgs(const CallExpr *CE) const {
    auto ArgList = callArgs(CE);
    auto Args = ArgList.ref();

    if (Type == HandlerType::PARAMS) {
      if (!HasError)
        return HandlerResult(Args);

      if (!isa<NilLiteralExpr>(Args.back()))
        return HandlerResult(Args.back(), true);

      return HandlerResult(Args.drop_back());
    } else if (Type == HandlerType::RESULT) {
      if (Args.size() != 1)
        return HandlerResult(Args);

      auto *ResultCE = dyn_cast<CallExpr>(Args[0]);
      if (!ResultCE)
        return HandlerResult(Args);

      auto *DSC = dyn_cast<DotSyntaxCallExpr>(ResultCE->getFn());
      if (!DSC)
        return HandlerResult(Args);

      auto *D = dyn_cast<EnumElementDecl>(
          DSC->getFn()->getReferencedDecl().getDecl());
      if (!D)
        return HandlerResult(Args);

      auto ResultArgList = callArgs(ResultCE);
      return HandlerResult(ResultArgList.ref()[0],
                           D->getNameStr() == StringRef("failure"));
    }

    llvm_unreachable("Unhandled result type");
  }
};

enum class ConditionType { INVALID, NIL, NOT_NIL };

/// Finds the `Subject` being compared to in various conditions. Also finds any
/// pattern that may have a bound name.
struct CallbackCondition {
  ConditionType Type = ConditionType::INVALID;
  const Decl *Subject = nullptr;
  const Pattern *BindPattern = nullptr;
  // Bit of a hack. When the `Subject` is a `Result` type we use this to
  // distinguish between the `.success` and `.failure` case (as opposed to just
  // checking whether `Subject` == `TheErrDecl`)
  bool ErrorCase = false;

  CallbackCondition() = default;

  /// Initializes a `CallbackCondition` with a `!=` or `==` comparison of
  /// an `Optional` typed `Subject` to `nil`, ie.
  ///   - `<Subject> != nil`
  ///   - `<Subject> == nil`
  CallbackCondition(const BinaryExpr *BE, const FuncDecl *Operator) {
    bool FoundNil = false;
    for (auto *Operand : BE->getArg()->getElements()) {
      if (isa<NilLiteralExpr>(Operand)) {
        FoundNil = true;
      } else if (auto *DRE = dyn_cast<DeclRefExpr>(Operand)) {
        Subject = DRE->getDecl();
      }
    }

    if (Subject && FoundNil) {
      if (Operator->getBaseName() == "==") {
        Type = ConditionType::NIL;
      } else if (Operator->getBaseName() == "!=") {
        Type = ConditionType::NOT_NIL;
      }
    }
  }

  /// Initializes a `CallbackCondition` with binding of an `Optional` or
  /// `Result` typed `Subject`, ie.
  ///   - `let bind = <Subject>`
  ///   - `case .success(let bind) = <Subject>`
  ///   - `case .failure(let bind) = <Subject>`
  ///   - `let bind = try? <Subject>.get()`
  CallbackCondition(const Pattern *P, const Expr *Init) {
    if (auto *DRE = dyn_cast<DeclRefExpr>(Init)) {
      if (auto *OSP = dyn_cast<OptionalSomePattern>(P)) {
        // `let bind = <Subject>`
        Type = ConditionType::NOT_NIL;
        Subject = DRE->getDecl();
        BindPattern = OSP->getSubPattern();
      } else if (auto *EEP = dyn_cast<EnumElementPattern>(P)) {
        // `case .<func>(let <bind>) = <Subject>`
        initFromEnumPattern(DRE->getDecl(), EEP);
      }
    } else if (auto *OTE = dyn_cast<OptionalTryExpr>(Init)) {
      // `let bind = try? <Subject>.get()`
      if (auto *OSP = dyn_cast<OptionalSomePattern>(P))
        initFromOptionalTry(OSP->getSubPattern(), OTE);
    }
  }

  /// Initializes a `CallbackCondtion` from a case statement inside a switch
  /// on `Subject` with `Result` type, ie.
  /// ```
  /// switch <Subject> {
  /// case .success(let bind):
  /// case .failure(let bind):
  /// }
  /// ```
  CallbackCondition(const Decl *Subject, const CaseLabelItem *CaseItem) {
    if (auto *EEP = dyn_cast<EnumElementPattern>(CaseItem->getPattern())) {
      // `case .<func>(let <bind>)`
      initFromEnumPattern(Subject, EEP);
    }
  }

  bool isValid() const { return Type != ConditionType::INVALID; }

  /// Given an `if` condition `Cond` and a set of `Decls`, find any
  /// `CallbackCondition`s in `Cond` that use one of those `Decls` and add them
  /// to the map `AddTo`. Return `true` if all elements in the condition are
  /// "handled", ie. every condition can be mapped to a single `Decl` in
  /// `Decls`.
  static bool all(StmtCondition Cond, llvm::DenseSet<const Decl *> Decls,
                  llvm::DenseMap<const Decl *, CallbackCondition> &AddTo) {
    bool Handled = true;
    for (auto &CondElement : Cond) {
      if (auto *BoolExpr = CondElement.getBooleanOrNull()) {
        SmallVector<Expr *, 1> Exprs;
        Exprs.push_back(BoolExpr);

        while (!Exprs.empty()) {
          auto *Next = Exprs.pop_back_val();
          if (auto *ACE = dyn_cast<AutoClosureExpr>(Next))
            Next = ACE->getSingleExpressionBody();

          if (auto *BE = dyn_cast_or_null<BinaryExpr>(Next)) {
            auto *Operator = isOperator(BE);
            if (Operator) {
              if (Operator->getBaseName() == "&&") {
                auto Args = BE->getArg()->getElements();
                Exprs.insert(Exprs.end(), Args.begin(), Args.end());
              } else {
                addCond(CallbackCondition(BE, Operator), Decls, AddTo, Handled);
              }
              continue;
            }
          }

          Handled = false;
        }
      } else if (auto *P = CondElement.getPatternOrNull()) {
        addCond(CallbackCondition(P, CondElement.getInitializer()), Decls,
                AddTo, Handled);
      }
    }
    return Handled && !AddTo.empty();
  }

private:
  static void addCond(const CallbackCondition &CC,
                      llvm::DenseSet<const Decl *> Decls,
                      llvm::DenseMap<const Decl *, CallbackCondition> &AddTo,
                      bool &Handled) {
    if (!CC.isValid() || !Decls.count(CC.Subject) ||
        !AddTo.try_emplace(CC.Subject, CC).second)
      Handled = false;
  }

  void initFromEnumPattern(const Decl *D, const EnumElementPattern *EEP) {
    if (auto *EED = EEP->getElementDecl()) {
      auto eedTy = EED->getParentEnum()->getDeclaredType();
      if (!eedTy || !eedTy->isResult())
        return;
      if (EED->getNameStr() == StringRef("failure"))
        ErrorCase = true;
      Type = ConditionType::NOT_NIL;
      Subject = D;
      BindPattern = EEP->getSubPattern();
    }
  }

  void initFromOptionalTry(const class Pattern *P, const OptionalTryExpr *OTE) {
    auto *ICE = dyn_cast<ImplicitConversionExpr>(OTE->getSubExpr());
    if (!ICE)
      return;
    auto *CE = dyn_cast<CallExpr>(ICE->getSyntacticSubExpr());
    if (!CE)
      return;
    auto *DSC = dyn_cast<DotSyntaxCallExpr>(CE->getFn());
    if (!DSC)
      return;

    auto *BaseDRE = dyn_cast<DeclRefExpr>(DSC->getBase());
    if (!BaseDRE->getType() || !BaseDRE->getType()->isResult())
      return;

    auto *FnDRE = dyn_cast<DeclRefExpr>(DSC->getFn());
    if (!FnDRE)
      return;
    auto *FD = dyn_cast<FuncDecl>(FnDRE->getDecl());
    if (!FD || FD->getNameStr() != StringRef("get"))
      return;

    Type = ConditionType::NOT_NIL;
    Subject = BaseDRE->getDecl();
    BindPattern = P;
  }
};

/// The statements within the closure of call to a function taking a callback
/// are split into a `SuccessBlock` and `ErrorBlock` (`ClassifiedBlocks`).
/// This class stores the nodes for each block, as well as a mapping of
/// decls to any patterns they are used in.
class ClassifiedBlock {
  SmallVector<ASTNode, 0> Nodes;
  // closure param -> name
  llvm::DenseMap<const Decl *, StringRef> BoundNames;
  bool AllLet = true;

public:
  ArrayRef<ASTNode> nodes() const { return llvm::makeArrayRef(Nodes); }

  StringRef boundName(const Decl *D) const { return BoundNames.lookup(D); }

  bool allLet() const { return AllLet; }

  void addAllNodes(ArrayRef<ASTNode> Nodes) {
    for (auto Node : Nodes) {
      addNode(Node);
    }
  }

  void addNode(const ASTNode Node) {
    if (!Node.isDecl(DeclKind::Var))
      Nodes.push_back(Node);
  }

  void addBinding(const CallbackCondition &FromCondition,
                  DiagnosticEngine &DiagEngine) {
    if (!FromCondition.BindPattern)
      return;

    if (auto *BP =
            dyn_cast_or_null<BindingPattern>(FromCondition.BindPattern)) {
      if (!BP->isLet())
        AllLet = false;
    }

    StringRef Name = FromCondition.BindPattern->getBoundName().str();
    if (Name.empty())
      return;

    auto Res = BoundNames.try_emplace(FromCondition.Subject, Name);
    if (Res.second)
      return;

    // Already inserted, only handle cases where the name is the same
    // TODO: This wouldn't be that hard to handle, just need to keep track
    //       of the decl and replace its name with the same as the original
    StringRef OldName = Res.first->second;
    if (OldName != Name) {
      DiagEngine.diagnose(FromCondition.BindPattern->getLoc(),
                          diag::callback_multiple_bound_names,
                          StringRef(OldName), Name);
    }
  }

  void addAllBindings(
      const llvm::DenseMap<const Decl *, CallbackCondition> &FromConditions,
      DiagnosticEngine &DiagEngine) {
    for (auto &Entry : FromConditions) {
      addBinding(Entry.second, DiagEngine);
      if (DiagEngine.hadAnyError())
        return;
    }
  }
};

struct ClassifiedBlocks {
  ClassifiedBlock SuccessBlock;
  ClassifiedBlock ErrorBlock;
};

/// Classifer of callback closure statements that that have either multiple
/// non-Result parameters or a single Result parameter and return Void.
///
/// It performs a (possibly incorrect) best effort and may give up in certain
/// cases. Aims to cover the idiomatic cases of either having no error
/// parameter at all, or having success/error code wrapped in ifs/guards/switch
/// using either pattern binding or nil checks.
///
/// Code outside any clear conditions is assumed to be solely part of the
/// success block for now, though some heuristics could be added to classify
/// these better in the future.
struct CallbackClassifier {
  /// Updates the success and error block of `Blocks` with nodes and bound
  /// names from `Body`. Errors are added through `DiagEngine`, possibly
  /// resulting in partially filled out blocks.
  static void classifyInto(ClassifiedBlocks &Blocks,
                           DiagnosticEngine &DiagEngine,
                           ArrayRef<const ParamDecl *> SuccessParams,
                           const ParamDecl *ErrParam, HandlerType ResultType,
                           ArrayRef<ASTNode> Body) {
    assert(!Body.empty() && "Cannot classify empty body");

    auto ParamsSet = llvm::DenseSet<const Decl *>(SuccessParams.begin(),
                                                  SuccessParams.end());
    if (ErrParam)
      ParamsSet.insert(ErrParam);

    CallbackClassifier Classifier(Blocks, DiagEngine, ParamsSet, ErrParam,
                                  ResultType == HandlerType::RESULT);
    Classifier.classifyNodes(Body);
  }

private:
  ClassifiedBlocks &Blocks;
  DiagnosticEngine &DiagEngine;
  ClassifiedBlock *CurrentBlock;
  llvm::DenseSet<const Decl *> ParamsSet;
  const ParamDecl *ErrParam;
  bool IsResultParam;

  CallbackClassifier(ClassifiedBlocks &Blocks, DiagnosticEngine &DiagEngine,
                     llvm::DenseSet<const Decl *> ParamsSet,
                     const ParamDecl *ErrParam, bool IsResultParam)
      : Blocks(Blocks), DiagEngine(DiagEngine),
        CurrentBlock(&Blocks.SuccessBlock), ParamsSet(ParamsSet),
        ErrParam(ErrParam), IsResultParam(IsResultParam) {}

  void classifyNodes(ArrayRef<ASTNode> Nodes) {
    for (auto I = Nodes.begin(), E = Nodes.end(); I < E; ++I) {
      auto *Statement = I->dyn_cast<Stmt *>();
      if (auto *IS = dyn_cast_or_null<IfStmt>(Statement)) {
        ArrayRef<ASTNode> TempNodes;
        if (auto *BS = dyn_cast<BraceStmt>(IS->getThenStmt())) {
          TempNodes = BS->getElements();
        } else {
          TempNodes = ArrayRef<ASTNode>(IS->getThenStmt());
        }

        classifyConditional(IS, IS->getCond(), TempNodes, IS->getElseStmt());
      } else if (auto *GS = dyn_cast_or_null<GuardStmt>(Statement)) {
        classifyConditional(GS, GS->getCond(), ArrayRef<ASTNode>(),
                            GS->getBody());
      } else if (auto *SS = dyn_cast_or_null<SwitchStmt>(Statement)) {
        classifySwitch(SS);
      } else {
        CurrentBlock->addNode(*I);
      }

      if (DiagEngine.hadAnyError())
        return;
    }
  }

  void classifyConditional(Stmt *Statement, StmtCondition Condition,
                           ArrayRef<ASTNode> ThenNodes, Stmt *ElseStmt) {
    llvm::DenseMap<const Decl *, CallbackCondition> CallbackConditions;
    bool UnhandledConditions =
        !CallbackCondition::all(Condition, ParamsSet, CallbackConditions);
    CallbackCondition ErrCondition = CallbackConditions.lookup(ErrParam);

    if (UnhandledConditions) {
      // Some unknown conditions. If there's an else, assume we can't handle
      // and use the fallback case. Otherwise add to either the success or
      // error block depending on some heuristics, known conditions will have
      // placeholders added (ideally we'd remove them)
      // TODO: Remove known conditions and split the `if` statement

      if (CallbackConditions.empty()) {
        // Technically this has a similar problem, ie. the else could have
        // conditions that should be in either success/error
        CurrentBlock->addNode(Statement);
      } else if (ElseStmt) {
        DiagEngine.diagnose(Statement->getStartLoc(),
                            diag::unknown_callback_conditions);
      } else if (ErrCondition.isValid() &&
                 ErrCondition.Type == ConditionType::NOT_NIL) {
        Blocks.ErrorBlock.addNode(Statement);
      } else {
        for (auto &Entry : CallbackConditions) {
          if (Entry.second.Type == ConditionType::NIL) {
            Blocks.ErrorBlock.addNode(Statement);
            return;
          }
        }
        Blocks.SuccessBlock.addNode(Statement);
      }
      return;
    }

    ClassifiedBlock *ThenBlock = &Blocks.SuccessBlock;
    ClassifiedBlock *ElseBlock = &Blocks.ErrorBlock;

    if (ErrCondition.isValid() && (!IsResultParam || ErrCondition.ErrorCase) &&
        ErrCondition.Type == ConditionType::NOT_NIL) {
      ClassifiedBlock *TempBlock = ThenBlock;
      ThenBlock = ElseBlock;
      ElseBlock = TempBlock;
    } else {
      ConditionType CondType = ConditionType::INVALID;
      for (auto &Entry : CallbackConditions) {
        if (IsResultParam || Entry.second.Subject != ErrParam) {
          if (CondType == ConditionType::INVALID) {
            CondType = Entry.second.Type;
          } else if (CondType != Entry.second.Type) {
            // Similar to the unknown conditions case. Add the whole if unless
            // there's an else, in which case use the fallback instead.
            // TODO: Split the `if` statement

            if (ElseStmt) {
              DiagEngine.diagnose(Statement->getStartLoc(),
                                  diag::mixed_callback_conditions);
            } else {
              CurrentBlock->addNode(Statement);
            }
            return;
          }
        }
      }

      if (CondType == ConditionType::NIL) {
        ClassifiedBlock *TempBlock = ThenBlock;
        ThenBlock = ElseBlock;
        ElseBlock = TempBlock;
      }
    }

    ThenBlock->addAllBindings(CallbackConditions, DiagEngine);
    if (DiagEngine.hadAnyError())
      return;

    // TODO: Handle nested ifs
    setNodes(ThenBlock, ElseBlock, ThenNodes);

    if (ElseStmt) {
      if (auto *BS = dyn_cast<BraceStmt>(ElseStmt)) {
        setNodes(ElseBlock, ThenBlock, BS->getElements());
      } else {
        classifyNodes(ArrayRef<ASTNode>(ElseStmt));
      }
    }
  }

  void setNodes(ClassifiedBlock *Block, ClassifiedBlock *OtherBlock,
                ArrayRef<ASTNode> Nodes) {
    if (Nodes.empty())
      return;
    if ((Nodes.back().isStmt(StmtKind::Return) ||
         Nodes.back().isStmt(StmtKind::Break)) &&
        !Nodes.back().isImplicit()) {
      CurrentBlock = OtherBlock;
      Block->addAllNodes(Nodes.drop_back());
    } else {
      Block->addAllNodes(Nodes);
    }
  }

  void classifySwitch(SwitchStmt *SS) {
    if (!IsResultParam || singleSwitchSubject(SS) != ErrParam) {
      CurrentBlock->addNode(SS);
    }

    for (auto *CS : SS->getCases()) {
      if (CS->hasFallthroughDest()) {
        DiagEngine.diagnose(CS->getLoc(), diag::callback_with_fallthrough);
        return;
      }

      if (CS->isDefault()) {
        DiagEngine.diagnose(CS->getLoc(), diag::callback_with_default);
        return;
      }

      auto Items = CS->getCaseLabelItems();
      if (Items.size() > 1) {
        DiagEngine.diagnose(CS->getLoc(), diag::callback_multiple_case_items);
        return;
      }

      if (Items[0].getWhereLoc().isValid()) {
        DiagEngine.diagnose(CS->getLoc(), diag::callback_where_case_item);
        return;
      }

      CallbackCondition CC(ErrParam, &Items[0]);
      ClassifiedBlock *Block = &Blocks.SuccessBlock;
      ClassifiedBlock *OtherBlock = &Blocks.ErrorBlock;
      if (CC.ErrorCase) {
        Block = &Blocks.ErrorBlock;
        OtherBlock = &Blocks.SuccessBlock;
      }

      setNodes(Block, OtherBlock, CS->getBody()->getElements());
      Block->addBinding(CC, DiagEngine);
      if (DiagEngine.hadAnyError())
        return;
    }
  }
};

/// Builds up async-converted code for an AST node.
///
/// If it is a function, its declaration will have `async` added. If a
/// completion handler is present, it will be removed and the return type of
/// the function will reflect the parameters of the handler, including an
/// added `throws` if necessary.
///
/// Calls to the completion handler are replaced with either a `return` or
/// `throws` depending on the arguments.
///
/// Calls to functions with an async alternative will be replaced with a call
/// to the alternative, possibly wrapped in a do/catch. The do/catch is skipped
/// if the the closure either:
///   1. Has no error
///   2. Has an error but no error handling (eg. just ignores)
///   3. Has error handling that only calls the containing function's handler
///      with an error matching the error argument
///
/// (2) is technically not the correct translation, but in practice it's likely
/// the code a user would actually want.
///
/// If the success vs error handling split inside the closure cannot be
/// determined and the closure takes regular parameters (ie. not a Result), a
/// fallback translation is used that keeps all the same variable names and
/// simply moves the code within the closure out.
///
/// The fallback is generally avoided, however, since it's quite unlikely to be
/// the code the user intended. In most cases the refactoring will continue,
/// with any unhandled decls wrapped in placeholders instead.
class AsyncConverter : private SourceEntityWalker {
  SourceManager &SM;
  DiagnosticEngine &DiagEngine;

  // Node to convert
  ASTNode StartNode;

  // Completion handler of `StartNode` (if it's a function with an async
  // alternative)
  const AsyncHandlerDesc &TopHandler;
  SmallString<0> Buffer;
  llvm::raw_svector_ostream OS;

  // Decls where any force-unwrap of that decl should be unwrapped, eg. for a
  // previously optional closure paramter has become a non-optional local
  llvm::DenseSet<const Decl *> Unwraps;
  // Decls whose references should be replaced with, either because they no
  // longer exist or are a different type. Any replaced code should ideally be
  // handled by the refactoring properly, but that's not possible in all cases
  llvm::DenseSet<const Decl *> Placeholders;
  // Mapping from decl -> name, used as both the name of possibly new local
  // declarations of old parameters, as well as the replacement for any
  // references to it
  llvm::DenseMap<const Decl *, std::string> Names;

  // These are per-node (ie. are saved and restored on each convertNode call)
  SourceLoc LastAddedLoc;
  int NestedExprCount = 0;

public:
  AsyncConverter(SourceManager &SM, DiagnosticEngine &DiagEngine,
                 ASTNode StartNode, const AsyncHandlerDesc &TopHandler)
      : SM(SM), DiagEngine(DiagEngine), StartNode(StartNode),
        TopHandler(TopHandler), Buffer(), OS(Buffer) {
    Placeholders.insert(TopHandler.Handler);
  }

  bool convert() {
    if (!Buffer.empty())
      return !DiagEngine.hadAnyError();

    if (auto *FD = dyn_cast_or_null<FuncDecl>(StartNode.dyn_cast<Decl *>())) {
      addFuncDecl(FD);
      if (FD->getBody()) {
        convertNode(FD->getBody());
      }
    } else {
      convertNode(StartNode);
    }
    return !DiagEngine.hadAnyError();
  }

  void replace(ASTNode Node, SourceEditConsumer &EditConsumer,
               SourceLoc StartOverride = SourceLoc()) {
    SourceRange Range = Node.getSourceRange();
    if (StartOverride.isValid()) {
      Range = SourceRange(StartOverride, Range.End);
    }
    CharSourceRange CharRange =
        Lexer::getCharSourceRangeFromSourceRange(SM, Range);
    EditConsumer.accept(SM, CharRange, Buffer.str());
    Buffer.clear();
  }

  void insertAfter(ASTNode Node, SourceEditConsumer &EditConsumer) {
    EditConsumer.insertAfter(SM, Node.getEndLoc(), "\n\n");
    EditConsumer.insertAfter(SM, Node.getEndLoc(), Buffer.str());
    Buffer.clear();
  }

private:
  void convertNodes(ArrayRef<ASTNode> Nodes) {
    for (auto Node : Nodes) {
      OS << "\n";
      convertNode(Node);
    }
  }

  void convertNode(ASTNode Node, SourceLoc StartOverride = {},
                   bool ConvertCalls = true) {
    if (!StartOverride.isValid())
      StartOverride = Node.getStartLoc();

    llvm::SaveAndRestore<SourceLoc> RestoreLoc(LastAddedLoc, StartOverride);
    llvm::SaveAndRestore<int> RestoreCount(NestedExprCount,
                                           ConvertCalls ? 0 : 1);
    walk(Node);
    addRange(LastAddedLoc, Node.getEndLoc(), /*ToEndOfToken=*/true);
  }

  bool walkToDeclPre(Decl *D, CharSourceRange Range) override {
    if (isa<PatternBindingDecl>(D)) {
      NestedExprCount++;
      return true;
    }
    return false;
  }

  bool walkToDeclPost(Decl *D) override {
    NestedExprCount--;
    return true;
  }

#define PLACEHOLDER_START "<#"
#define PLACEHOLDER_END "#>"
  bool walkToExprPre(Expr *E) override {
    // TODO: Handle Result.get as well
    if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
      if (auto *D = DRE->getDecl()) {
        bool AddPlaceholder = Placeholders.count(D);
        StringRef Name = newNameFor(D, false);
        if (AddPlaceholder || !Name.empty())
          return addCustom(DRE->getStartLoc(),
                           Lexer::getLocForEndOfToken(SM, DRE->getEndLoc()),
                           [&]() {
                             if (AddPlaceholder)
                               OS << PLACEHOLDER_START;
                             if (!Name.empty())
                               OS << Name;
                             else
                               D->getName().print(OS);
                             if (AddPlaceholder)
                               OS << PLACEHOLDER_END;
                           });
      }
    } else if (auto *FTE = dyn_cast<ForceValueExpr>(E)) {
      if (auto *D = FTE->getReferencedDecl().getDecl()) {
        if (Unwraps.count(D))
          return addCustom(FTE->getStartLoc(),
                           FTE->getEndLoc().getAdvancedLoc(1),
                           [&]() { OS << newNameFor(D, true); });
      }
    } else if (NestedExprCount == 0) {
      if (CallExpr *CE = TopHandler.getAsHandlerCall(E))
        return addCustom(CE->getStartLoc(), CE->getEndLoc().getAdvancedLoc(1),
                         [&]() { addHandlerCall(CE); });

      if (auto *CE = dyn_cast<CallExpr>(E)) {
        auto HandlerDesc = AsyncHandlerDesc::find(
            getUnderlyingFunc(CE->getFn()), StartNode.dyn_cast<Expr *>() == CE);
        if (HandlerDesc.isValid())
          return addCustom(CE->getStartLoc(), CE->getEndLoc().getAdvancedLoc(1),
                           [&]() { addAsyncAlternativeCall(CE, HandlerDesc); });
      }
    }

    NestedExprCount++;
    return true;
  }
#undef PLACEHOLDER_START
#undef PLACEHOLDER_END

  bool walkToExprPost(Expr *E) override {
    NestedExprCount--;
    return true;
  }

  bool addCustom(SourceLoc End, SourceLoc NextAddedLoc,
                 std::function<void()> Custom = {}) {
    addRange(LastAddedLoc, End);
    Custom();
    LastAddedLoc = NextAddedLoc;
    return false;
  }

  void addRange(SourceLoc Start, SourceLoc End, bool ToEndOfToken = false) {
    if (ToEndOfToken) {
      OS << Lexer::getCharSourceRangeFromSourceRange(SM,
                                                     SourceRange(Start, End))
                .str();
    } else {
      OS << CharSourceRange(SM, Start, End).str();
    }
  }

  void addRange(SourceRange Range, bool ToEndOfToken = false) {
    addRange(Range.Start, Range.End, ToEndOfToken);
  }

  void addFuncDecl(const FuncDecl *FD) {
    auto *Params = FD->getParameters();

    // First chunk: start -> the parameter to remove (if any)
    SourceLoc LeftEndLoc = Params->getLParenLoc().getAdvancedLoc(1);
    if (TopHandler.Index - 1 >= 0) {
      LeftEndLoc = Lexer::getLocForEndOfToken(
          SM, Params->get(TopHandler.Index - 1)->getEndLoc());
    }
    addRange(FD->getSourceRangeIncludingAttrs().Start, LeftEndLoc);

    // Second chunk: end of the parameter to remove -> right parenthesis
    SourceLoc MidStartLoc = LeftEndLoc;
    SourceLoc MidEndLoc = Params->getRParenLoc().getAdvancedLoc(1);
    if (TopHandler.isValid()) {
      if ((size_t)(TopHandler.Index + 1) < Params->size()) {
        MidStartLoc = Params->get(TopHandler.Index + 1)->getStartLoc();
      } else {
        MidStartLoc = Params->getRParenLoc();
      }
    }
    addRange(MidStartLoc, MidEndLoc);

    // Third chunk: add in async and throws if necessary
    OS << " async";
    if (FD->hasThrows() || TopHandler.HasError)
      // TODO: Add throws if converting a function and it has a converted call
      //       without a do/catch
      OS << " " << tok::kw_throws;

    // Fourth chunk: if no parent handler (ie. not adding an async
    // alternative), the rest of the decl. Otherwise, add in the new return
    // type
    if (!TopHandler.isValid()) {
      SourceLoc RightStartLoc = MidEndLoc;
      if (FD->hasThrows()) {
        RightStartLoc = Lexer::getLocForEndOfToken(SM, FD->getThrowsLoc());
      }
      SourceLoc RightEndLoc =
          FD->getBody() ? FD->getBody()->getLBraceLoc() : FD->getEndLoc();
      addRange(RightStartLoc, RightEndLoc);
      return;
    }

    auto HandlerParams = TopHandler.params();
    if (TopHandler.Type == HandlerType::PARAMS && TopHandler.HasError) {
      HandlerParams = HandlerParams.drop_back();
    }

    if (HandlerParams.empty()) {
      OS << " ";
      return;
    }

    OS << " -> ";

    if (HandlerParams.size() > 1) {
      OS << "(";
    }
    for (size_t I = 0, E = HandlerParams.size(); I < E; ++I) {
      if (I > 0) {
        OS << ", ";
      }

      auto &Param = HandlerParams[I];
      if (TopHandler.Type == HandlerType::PARAMS) {
        Type ToPrint = Param.getPlainType();
        if (TopHandler.HasError)
          ToPrint = ToPrint->lookThroughSingleOptionalType();
        ToPrint->print(OS);
      } else if (TopHandler.Type == HandlerType::RESULT) {
        auto ResultTy = Param.getPlainType()->getAs<BoundGenericType>();
        assert(ResultTy && "Result must have generic type");
        ResultTy->getGenericArgs()[0]->print(OS);
      } else {
        llvm_unreachable("Unhandled handler type");
      }
    }
    if (HandlerParams.size() > 1) {
      OS << ")";
    }

    if (FD->hasBody())
      OS << " ";

    // TODO: Should remove the generic param and where clause for the error
    //       param if it exists (and no other parameter uses that type)
    TrailingWhereClause *TWC = FD->getTrailingWhereClause();
    if (TWC && TWC->getWhereLoc().isValid()) {
      auto Range = TWC->getSourceRange();
      OS << Lexer::getCharSourceRangeFromSourceRange(SM, Range).str();
      if (FD->hasBody())
        OS << " ";
    }
  }

  void addFallbackVars(ArrayRef<const ParamDecl *> FallbackParams,
                       ClassifiedBlocks &Blocks) {
    for (auto Param : FallbackParams) {
      OS << tok::kw_var << " " << newNameFor(Param) << ": ";
      auto Ty = Param->getType();
      Ty->print(OS);
      if (!Ty->getOptionalObjectType())
        OS << "?";

      OS << " = " << tok::kw_nil << "\n";
    }
  }

  void addDo() { OS << tok::kw_do << " " << tok::l_brace << "\n"; }

  void addHandlerCall(const CallExpr *CE) {
    auto Exprs = TopHandler.extractResultArgs(CE);

    if (!Exprs.isError()) {
      OS << tok::kw_return;
    } else {
      OS << tok::kw_throw;
    }

    ArrayRef<Expr *> Args = Exprs.args();
    if (!Args.empty()) {
      OS << " ";
      if (Args.size() > 1)
        OS << tok::l_paren;
      for (size_t I = 0, E = Args.size(); I < E; ++I) {
        if (I > 0)
          OS << tok::comma << " ";
        // Can't just add the range as we need to perform replacements
        convertNode(Args[I], /*StartOverride=*/CE->getArgumentLabelLoc(I),
                    /*ConvertCalls=*/false);
      }
      if (Args.size() > 1)
        OS << tok::r_paren;
    }
  }

  void addAsyncAlternativeCall(const CallExpr *CE,
                               const AsyncHandlerDesc &HandlerDesc) {
    auto ArgList = callArgs(CE);
    if ((size_t)HandlerDesc.Index >= ArgList.ref().size()) {
      DiagEngine.diagnose(CE->getStartLoc(), diag::missing_callback_arg);
      return;
    }

    auto Callback = dyn_cast<ClosureExpr>(ArgList.ref()[HandlerDesc.Index]);
    auto Capture = dyn_cast<CaptureListExpr>(ArgList.ref()[HandlerDesc.Index]);
    if (Capture) {
      Callback = Capture->getClosureBody();
    }
    if (!Callback) {
      DiagEngine.diagnose(CE->getStartLoc(), diag::missing_callback_arg);
      return;
    }

    ParameterList *CallbackParams = Callback->getParameters();
    ArrayRef<ASTNode> CallbackBody = Callback->getBody()->getElements();
    if (HandlerDesc.params().size() != CallbackParams->size()) {
      DiagEngine.diagnose(CE->getStartLoc(), diag::mismatched_callback_args);
      return;
    }

    // Note that the `ErrParam` may be a Result (in which case it's also the
    // only element in `SuccessParams`)
    ArrayRef<const ParamDecl *> SuccessParams = CallbackParams->getArray();
    const ParamDecl *ErrParam = nullptr;
    if (HandlerDesc.HasError) {
      ErrParam = SuccessParams.back();
      if (HandlerDesc.Type == HandlerType::PARAMS)
        SuccessParams = SuccessParams.drop_back();
    }
    ArrayRef<const ParamDecl *> ErrParams;
    if (ErrParam)
      ErrParams = llvm::makeArrayRef(ErrParam);

    ClassifiedBlocks Blocks;
    if (!HandlerDesc.HasError) {
      Blocks.SuccessBlock.addAllNodes(CallbackBody);
    } else if (!CallbackBody.empty()) {
      CallbackClassifier::classifyInto(Blocks, DiagEngine, SuccessParams,
                                       ErrParam, HandlerDesc.Type,
                                       CallbackBody);
      if (DiagEngine.hadAnyError()) {
        // Can only fallback when the results are params, in which case only
        // the names are used (defaulted to the names of the params if none)
        if (HandlerDesc.Type != HandlerType::PARAMS)
          return;
        DiagEngine.resetHadAnyError();

        setNames(ClassifiedBlock(), CallbackParams->getArray());

        addFallbackVars(CallbackParams->getArray(), Blocks);
        addDo();
        addAwaitCall(CE, ArgList.ref(), Blocks.SuccessBlock, SuccessParams,
                     /*HasError=*/HandlerDesc.HasError,
                     /*AddDeclarations=*/!HandlerDesc.HasError);
        addFallbackCatch(ErrParam);
        OS << "\n";
        convertNodes(CallbackBody);

        clearParams(CallbackParams->getArray());
        return;
      }
    }

    bool RequireDo = !Blocks.ErrorBlock.nodes().empty();
    // Check if we *actually* need a do/catch (see class comment)
    if (Blocks.ErrorBlock.nodes().size() == 1) {
      auto Node = Blocks.ErrorBlock.nodes()[0];
      if (auto *HandlerCall = TopHandler.getAsHandlerCall(Node)) {
        auto Res = TopHandler.extractResultArgs(HandlerCall);
        if (Res.args().size() == 1) {
          // Skip if we have the param itself or the name it's bound to
          auto *SingleDecl = Res.args()[0]->getReferencedDecl().getDecl();
          auto ErrName = Blocks.ErrorBlock.boundName(ErrParam);
          RequireDo = SingleDecl != ErrParam &&
                      !(Res.isError() && SingleDecl &&
                        SingleDecl->getName().isSimpleName(ErrName));
        }
      }
    }

    if (RequireDo) {
      addDo();
    }

    setNames(Blocks.SuccessBlock, SuccessParams);
    addAwaitCall(CE, ArgList.ref(), Blocks.SuccessBlock, SuccessParams,
                 /*HasError=*/HandlerDesc.HasError,
                 /*AddDeclarations=*/true);

    prepareNamesForBody(HandlerDesc.Type, SuccessParams, ErrParams);
    convertNodes(Blocks.SuccessBlock.nodes());

    if (RequireDo) {
      clearParams(SuccessParams);
      // Always use the ErrParam name if none is bound
      setNames(Blocks.ErrorBlock, ErrParams,
               HandlerDesc.Type != HandlerType::RESULT);
      addCatch(ErrParam);

      prepareNamesForBody(HandlerDesc.Type, ErrParams, SuccessParams);
      addCatchBody(ErrParam, Blocks.ErrorBlock);
    }

    clearParams(CallbackParams->getArray());
  }

  void addAwaitCall(const CallExpr *CE, ArrayRef<Expr *> Args,
                    const ClassifiedBlock &SuccessBlock,
                    ArrayRef<const ParamDecl *> SuccessParams, bool HasError,
                    bool AddDeclarations) {
    if (!SuccessParams.empty()) {
      if (AddDeclarations) {
        if (SuccessBlock.allLet()) {
          OS << tok::kw_let;
        } else {
          OS << tok::kw_var;
        }
        OS << " ";
      }
      if (SuccessParams.size() > 1)
        OS << tok::l_paren;
      OS << newNameFor(SuccessParams.front());
      for (const auto Param : SuccessParams.drop_front()) {
        OS << tok::comma << " ";
        OS << newNameFor(Param);
      }
      if (SuccessParams.size() > 1) {
        OS << tok::r_paren;
      }
      OS << " " << tok::equal << " ";
    }

    if (HasError) {
      OS << tok::kw_try << " ";
    }
    OS << "await ";
    addRange(CE->getStartLoc(), CE->getFn()->getEndLoc(),
             /*ToEndOfToken=*/true);

    OS << tok::l_paren;
    size_t realArgCount = 0;
    for (size_t I = 0, E = Args.size() - 1; I < E; ++I) {
      if (isa<DefaultArgumentExpr>(Args[I]))
        continue;

      if (realArgCount > 0)
        OS << tok::comma << " ";
      // Can't just add the range as we need to perform replacements
      convertNode(Args[I], /*StartOverride=*/CE->getArgumentLabelLoc(I),
                  /*ConvertCalls=*/false);
      realArgCount++;
    }
    OS << tok::r_paren;
  }

  void addFallbackCatch(const ParamDecl *ErrParam) {
    auto ErrName = newNameFor(ErrParam);
    OS << "\n"
       << tok::r_brace << " " << tok::kw_catch << " " << tok::l_brace << "\n"
       << ErrName << " = error\n"
       << tok::r_brace;
  }

  void addCatch(const ParamDecl *ErrParam) {
    OS << "\n" << tok::r_brace << " " << tok::kw_catch << " ";
    auto ErrName = newNameFor(ErrParam, false);
    if (!ErrName.empty()) {
      OS << tok::kw_let << " " << ErrName << " ";
    }
    OS << tok::l_brace;
  }

  void addCatchBody(const ParamDecl *ErrParam,
                    const ClassifiedBlock &ErrorBlock) {
    convertNodes(ErrorBlock.nodes());
    OS << "\n" << tok::r_brace;
  }

  void prepareNamesForBody(HandlerType ResultType,
                           ArrayRef<const ParamDecl *> CurrentParams,
                           ArrayRef<const ParamDecl *> OtherParams) {
    switch (ResultType) {
    case HandlerType::PARAMS:
      for (auto *Param : CurrentParams) {
        if (Param->getType()->getOptionalObjectType()) {
          Unwraps.insert(Param);
          Placeholders.insert(Param);
        }
      }
      // Use of the other params is invalid within the current body
      Placeholders.insert(OtherParams.begin(), OtherParams.end());
      break;
    case HandlerType::RESULT:
      // Any uses of the result parameter in the current body (that
      // isn't replaced) are invalid, so replace them with a placeholder
      Placeholders.insert(CurrentParams.begin(), CurrentParams.end());
      break;
    default:
      llvm_unreachable("Unhandled handler type");
    }
  }

  // TODO: Check for clashes with existing names
  void setNames(const ClassifiedBlock &Block,
                ArrayRef<const ParamDecl *> Params, bool AddIfMissing = true) {
    for (auto *Param : Params) {
      StringRef Name = Block.boundName(Param);
      if (!Name.empty()) {
        Names[Param] = Name.str();
        continue;
      }

      if (!AddIfMissing)
        continue;

      auto ParamName = Param->getNameStr();
      if (ParamName.startswith("$")) {
        Names[Param] = "val" + ParamName.drop_front().str();
      } else {
        Names[Param] = ParamName.str();
      }
    }
  }

  StringRef newNameFor(const Decl *D, bool Required = true) {
    auto Res = Names.find(D);
    if (Res == Names.end()) {
      assert(!Required && "Missing name for decl when one was required");
      return StringRef();
    }
    return StringRef(Res->second);
  }

  void clearParams(ArrayRef<const ParamDecl *> Params) {
    for (auto *Param : Params) {
      Unwraps.erase(Param);
      Placeholders.erase(Param);
      Names.erase(Param);
    }
  }
};
} // namespace asyncrefactorings

bool RefactoringActionConvertCallToAsyncAlternative::isApplicable(
    const ResolvedCursorInfo &CursorInfo, DiagnosticEngine &Diag) {
  using namespace asyncrefactorings;

  // Currently doesn't check that the call is in an async context. This seems
  // possibly useful in some situations, so we'll see what the feedback is.
  // May need to change in the future
  auto *CE = findOuterCall(CursorInfo);
  if (!CE)
    return false;

  auto HandlerDesc = AsyncHandlerDesc::find(getUnderlyingFunc(CE->getFn()),
                                            /*ignoreName=*/true);
  return HandlerDesc.isValid();
}

/// Converts a call of a function with a possible async alternative, to use it
/// instead. Currently this is any function that
///   1. has a void return type,
///   2. has a void returning closure as its last parameter, and
///   3. is not already async
///
/// For now the call need not be in an async context, though this may change
/// depending on feedback.
bool RefactoringActionConvertCallToAsyncAlternative::performChange() {
  using namespace asyncrefactorings;

  auto *CE = findOuterCall(CursorInfo);
  assert(CE &&
         "Should not run performChange when refactoring is not applicable");

  AsyncHandlerDesc TempDesc;
  AsyncConverter Converter(SM, DiagEngine, CE, TempDesc);
  if (!Converter.convert())
    return true;

  Converter.replace(CE, EditConsumer);
  return false;
}

bool RefactoringActionConvertToAsync::isApplicable(
    const ResolvedCursorInfo &CursorInfo, DiagnosticEngine &Diag) {
  using namespace asyncrefactorings;

  // As with the call refactoring, should possibly only apply if there's
  // actually calls to async alternatives. At the moment this will just add
  // `async` if there are no calls, which is probably fine.
  return findFunction(CursorInfo);
}

/// Converts a whole function to async, converting any calls to functions with
/// async alternatives as above.
bool RefactoringActionConvertToAsync::performChange() {
  using namespace asyncrefactorings;

  auto *FD = findFunction(CursorInfo);
  assert(FD &&
         "Should not run performChange when refactoring is not applicable");

  AsyncHandlerDesc TempDesc;
  AsyncConverter Converter(SM, DiagEngine, FD, TempDesc);
  if (!Converter.convert())
    return true;

  Converter.replace(FD, EditConsumer, FD->getSourceRangeIncludingAttrs().Start);
  return false;
}

bool RefactoringActionAddAsyncAlternative::isApplicable(
    const ResolvedCursorInfo &CursorInfo, DiagnosticEngine &Diag) {
  using namespace asyncrefactorings;

  auto *FD = findFunction(CursorInfo);
  if (!FD)
    return false;

  auto HandlerDesc = AsyncHandlerDesc::find(FD, /*ignoreName=*/true);
  return HandlerDesc.isValid();
}

/// Adds an async alternative and marks the current function as deprecated.
/// Equivalent to the conversion but
///   1. only works on functions that themselves are a possible async
///      alternative, and
///   2. has extra handling to convert the completion/handler/callback closure
///      parameter to either `return`/`throws`
bool RefactoringActionAddAsyncAlternative::performChange() {
  using namespace asyncrefactorings;

  auto *FD = findFunction(CursorInfo);
  assert(FD &&
         "Should not run performChange when refactoring is not applicable");

  auto HandlerDesc = AsyncHandlerDesc::find(FD, /*ignoreName=*/true);
  assert(HandlerDesc.isValid() &&
         "Should not run performChange when refactoring is not applicable");

  AsyncConverter Converter(SM, DiagEngine, FD, HandlerDesc);
  if (!Converter.convert())
    return true;

  EditConsumer.accept(SM, FD->getAttributeInsertionLoc(false),
                      "@available(*, deprecated, message: \"Prefer async "
                      "alternative instead\")\n");
  Converter.insertAfter(FD, EditConsumer);

  return false;
}
} // end of anonymous namespace

StringRef swift::ide::
getDescriptiveRefactoringKindName(RefactoringKind Kind) {
    switch(Kind) {
      case RefactoringKind::None:
        llvm_unreachable("Should be a valid refactoring kind");
#define REFACTORING(KIND, NAME, ID) case RefactoringKind::KIND: return NAME;
#include "swift/IDE/RefactoringKinds.def"
    }
    llvm_unreachable("unhandled kind");
  }

  StringRef swift::ide::
  getDescriptiveRenameUnavailableReason(RenameAvailableKind Kind) {
    switch(Kind) {
      case RenameAvailableKind::Available:
        return "";
      case RenameAvailableKind::Unavailable_system_symbol:
        return "symbol from system module cannot be renamed";
      case RenameAvailableKind::Unavailable_has_no_location:
        return "symbol without a declaration location cannot be renamed";
      case RenameAvailableKind::Unavailable_has_no_name:
        return "cannot find the name of the symbol";
      case RenameAvailableKind::Unavailable_has_no_accessibility:
        return "cannot decide the accessibility of the symbol";
      case RenameAvailableKind::Unavailable_decl_from_clang:
        return "cannot rename a Clang symbol from its Swift reference";
    }
    llvm_unreachable("unhandled kind");
  }

SourceLoc swift::ide::RangeConfig::getStart(SourceManager &SM) {
  return SM.getLocForLineCol(BufferId, Line, Column);
}

SourceLoc swift::ide::RangeConfig::getEnd(SourceManager &SM) {
  return getStart(SM).getAdvancedLoc(Length);
}

struct swift::ide::FindRenameRangesAnnotatingConsumer::Implementation {
  std::unique_ptr<SourceEditConsumer> pRewriter;
  Implementation(SourceManager &SM, unsigned BufferId, raw_ostream &OS)
  : pRewriter(new SourceEditOutputConsumer(SM, BufferId, OS)) {}
  static StringRef tag(RefactoringRangeKind Kind) {
    switch (Kind) {
      case RefactoringRangeKind::BaseName:
        return "base";
      case RefactoringRangeKind::KeywordBaseName:
        return "keywordBase";
      case RefactoringRangeKind::ParameterName:
        return "param";
      case RefactoringRangeKind::NoncollapsibleParameterName:
        return "noncollapsibleparam";
      case RefactoringRangeKind::DeclArgumentLabel:
        return "arglabel";
      case RefactoringRangeKind::CallArgumentLabel:
        return "callarg";
      case RefactoringRangeKind::CallArgumentColon:
        return "callcolon";
      case RefactoringRangeKind::CallArgumentCombined:
        return "callcombo";
      case RefactoringRangeKind::SelectorArgumentLabel:
        return "sel";
    }
    llvm_unreachable("unhandled kind");
  }
  void accept(SourceManager &SM, const RenameRangeDetail &Range) {
    std::string NewText;
    llvm::raw_string_ostream OS(NewText);
    StringRef Tag = tag(Range.RangeKind);
    OS << "<" << Tag;
    if (Range.Index.hasValue())
      OS << " index=" << *Range.Index;
    OS << ">" << Range.Range.str() << "</" << Tag << ">";
    pRewriter->accept(SM, {Range.Range, OS.str(), {}});
  }
};

swift::ide::FindRenameRangesAnnotatingConsumer::
FindRenameRangesAnnotatingConsumer(SourceManager &SM, unsigned BufferId,
                                   raw_ostream &OS) :
    Impl(*new Implementation(SM, BufferId, OS)) {}

swift::ide::FindRenameRangesAnnotatingConsumer::~FindRenameRangesAnnotatingConsumer() {
  delete &Impl;
}

void swift::ide::FindRenameRangesAnnotatingConsumer::
accept(SourceManager &SM, RegionType RegionType,
       ArrayRef<RenameRangeDetail> Ranges) {
  if (RegionType == RegionType::Mismatch || RegionType == RegionType::Unmatched)
    return;
  for (const auto &Range : Ranges) {
    Impl.accept(SM, Range);
  }
}

void swift::ide::collectRenameAvailabilityInfo(
    const ValueDecl *VD, Optional<RenameRefInfo> RefInfo,
    SmallVectorImpl<RenameAvailabilityInfo> &Infos) {
  RenameAvailableKind AvailKind = RenameAvailableKind::Available;
  if (getRelatedSystemDecl(VD)){
    AvailKind = RenameAvailableKind::Unavailable_system_symbol;
  } else if (VD->getClangDecl()) {
    AvailKind = RenameAvailableKind::Unavailable_decl_from_clang;
  } else if (VD->getStartLoc().isInvalid()) {
    AvailKind = RenameAvailableKind::Unavailable_has_no_location;
  } else if (!VD->hasName()) {
    AvailKind = RenameAvailableKind::Unavailable_has_no_name;
  }

  if (isa<AbstractFunctionDecl>(VD)) {
    // Disallow renaming accessors.
    if (isa<AccessorDecl>(VD))
      return;

    // Disallow renaming deinit.
    if (isa<DestructorDecl>(VD))
      return;

    // Disallow renaming init with no arguments.
    if (auto CD = dyn_cast<ConstructorDecl>(VD)) {
      if (!CD->getParameters()->size())
        return;

      if (RefInfo && !RefInfo->IsArgLabel) {
        NameMatcher Matcher(*(RefInfo->SF));
        auto Resolved = Matcher.resolve({RefInfo->Loc, /*ResolveArgs*/true});
        if (Resolved.LabelRanges.empty())
          return;
      }
    }

    // Disallow renaming 'callAsFunction' method with no arguments.
    if (auto FD = dyn_cast<FuncDecl>(VD)) {
      // FIXME: syntactic rename can only decide by checking the spelling, not
      // whether it's an instance method, so we do the same here for now.
      if (FD->getBaseIdentifier() == FD->getASTContext().Id_callAsFunction) {
        if (!FD->getParameters()->size())
          return;

        if (RefInfo && !RefInfo->IsArgLabel) {
          NameMatcher Matcher(*(RefInfo->SF));
          auto Resolved = Matcher.resolve({RefInfo->Loc, /*ResolveArgs*/true});
          if (Resolved.LabelRanges.empty())
            return;
        }
      }
    }
  }

  // Always return local rename for parameters.
  // FIXME: if the cursor is on the argument, we should return global rename.
  if (isa<ParamDecl>(VD)) {
    Infos.emplace_back(RefactoringKind::LocalRename, AvailKind);
    return;
  }

  // If the indexer considers VD a global symbol, then we apply global rename.
  if (index::isLocalSymbol(VD))
    Infos.emplace_back(RefactoringKind::LocalRename, AvailKind);
  else
    Infos.emplace_back(RefactoringKind::GlobalRename, AvailKind);
}

void swift::ide::collectAvailableRefactorings(
    const ResolvedCursorInfo &CursorInfo,
    SmallVectorImpl<RefactoringKind> &Kinds, bool ExcludeRename) {
  DiagnosticEngine DiagEngine(CursorInfo.SF->getASTContext().SourceMgr);

  if (!ExcludeRename) {
    if (RefactoringActionLocalRename::isApplicable(CursorInfo, DiagEngine))
      Kinds.push_back(RefactoringKind::LocalRename);

    switch (CursorInfo.Kind) {
    case CursorInfoKind::ModuleRef:
    case CursorInfoKind::Invalid:
    case CursorInfoKind::StmtStart:
    case CursorInfoKind::ExprStart:
      break;
    case CursorInfoKind::ValueRef: {
      Optional<RenameRefInfo> RefInfo;
      if (CursorInfo.IsRef)
        RefInfo = {CursorInfo.SF, CursorInfo.Loc, CursorInfo.IsKeywordArgument};
      auto RenameOp = getAvailableRenameForDecl(CursorInfo.ValueD, RefInfo);
      if (RenameOp.hasValue() &&
          RenameOp.getValue() == RefactoringKind::GlobalRename)
        Kinds.push_back(RenameOp.getValue());
    }
    }
  }

#define CURSOR_REFACTORING(KIND, NAME, ID)                                     \
  if (RefactoringKind::KIND != RefactoringKind::LocalRename &&                 \
      RefactoringAction##KIND::isApplicable(CursorInfo, DiagEngine))           \
    Kinds.push_back(RefactoringKind::KIND);
#include "swift/IDE/RefactoringKinds.def"
}

void swift::ide::collectAvailableRefactorings(
    SourceFile *SF, RangeConfig Range, bool &RangeStartMayNeedRename,
    SmallVectorImpl<RefactoringKind> &Kinds,
    ArrayRef<DiagnosticConsumer *> DiagConsumers) {
  if (Range.Length == 0) {
    return collectAvailableRefactoringsAtCursor(SF, Range.Line, Range.Column,
                                                Kinds, DiagConsumers);
  }
  // Prepare the tool box.
  ASTContext &Ctx = SF->getASTContext();
  SourceManager &SM = Ctx.SourceMgr;
  DiagnosticEngine DiagEngine(SM);
  std::for_each(DiagConsumers.begin(), DiagConsumers.end(),
    [&](DiagnosticConsumer *Con) { DiagEngine.addConsumer(*Con); });
  ResolvedRangeInfo Result = evaluateOrDefault(SF->getASTContext().evaluator,
    RangeInfoRequest(RangeInfoOwner({SF,
                      Range.getStart(SF->getASTContext().SourceMgr),
                      Range.getEnd(SF->getASTContext().SourceMgr)})),
                                               ResolvedRangeInfo());

  bool enableInternalRefactoring = getenv("SWIFT_ENABLE_INTERNAL_REFACTORING_ACTIONS");

#define RANGE_REFACTORING(KIND, NAME, ID)                                      \
  if (RefactoringAction##KIND::isApplicable(Result, DiagEngine))               \
    Kinds.push_back(RefactoringKind::KIND);
#define INTERNAL_RANGE_REFACTORING(KIND, NAME, ID)                            \
  if (enableInternalRefactoring)                                              \
    RANGE_REFACTORING(KIND, NAME, ID)
#include "swift/IDE/RefactoringKinds.def"

  RangeStartMayNeedRename = rangeStartMayNeedRename(Result);
}

bool swift::ide::
refactorSwiftModule(ModuleDecl *M, RefactoringOptions Opts,
                    SourceEditConsumer &EditConsumer,
                    DiagnosticConsumer &DiagConsumer) {
  assert(Opts.Kind != RefactoringKind::None && "should have a refactoring kind.");

  // Use the default name if not specified.
  if (Opts.PreferredName.empty()) {
    Opts.PreferredName = getDefaultPreferredName(Opts.Kind).str();
  }

  switch (Opts.Kind) {
#define SEMANTIC_REFACTORING(KIND, NAME, ID)                                   \
case RefactoringKind::KIND: {                                                  \
      RefactoringAction##KIND Action(M, Opts, EditConsumer, DiagConsumer);     \
      if (RefactoringKind::KIND == RefactoringKind::LocalRename ||             \
          Action.isApplicable())                                               \
        return Action.performChange();                                         \
      return true;                                                             \
  }
#include "swift/IDE/RefactoringKinds.def"
    case RefactoringKind::GlobalRename:
    case RefactoringKind::FindGlobalRenameRanges:
    case RefactoringKind::FindLocalRenameRanges:
      llvm_unreachable("not a valid refactoring kind");
    case RefactoringKind::None:
      llvm_unreachable("should not enter here.");
  }
  llvm_unreachable("unhandled kind");
}

static std::vector<ResolvedLoc>
resolveRenameLocations(ArrayRef<RenameLoc> RenameLocs, SourceFile &SF,
                       DiagnosticEngine &Diags) {
  SourceManager &SM = SF.getASTContext().SourceMgr;
  unsigned BufferID = SF.getBufferID().getValue();

  std::vector<UnresolvedLoc> UnresolvedLocs;
  for (const RenameLoc &RenameLoc : RenameLocs) {
    DeclNameViewer OldName(RenameLoc.OldName);
    SourceLoc Location = SM.getLocForLineCol(BufferID, RenameLoc.Line,
                                             RenameLoc.Column);

    if (!OldName.isValid()) {
      Diags.diagnose(Location, diag::invalid_name, RenameLoc.OldName);
      return {};
    }

    if (!RenameLoc.NewName.empty()) {
      DeclNameViewer NewName(RenameLoc.NewName);
      ArrayRef<StringRef> ParamNames = NewName.args();
      bool newOperator = Lexer::isOperator(NewName.base());
      bool NewNameIsValid = NewName.isValid() &&
        (Lexer::isIdentifier(NewName.base()) || newOperator) &&
        std::all_of(ParamNames.begin(), ParamNames.end(), [](StringRef Label) {
          return Label.empty() || Lexer::isIdentifier(Label);
        });

      if (!NewNameIsValid) {
        Diags.diagnose(Location, diag::invalid_name, RenameLoc.NewName);
        return {};
      }

      if (NewName.partsCount() != OldName.partsCount()) {
        Diags.diagnose(Location, diag::arity_mismatch, RenameLoc.NewName,
                       RenameLoc.OldName);
        return {};
      }

      if (RenameLoc.Usage == NameUsage::Call && !RenameLoc.IsFunctionLike) {
        Diags.diagnose(Location, diag::name_not_functionlike, RenameLoc.NewName);
        return {};
      }
    }

    bool isOperator = Lexer::isOperator(OldName.base());
    UnresolvedLocs.push_back({
      Location,
      (RenameLoc.Usage == NameUsage::Unknown ||
       (RenameLoc.Usage == NameUsage::Call && !isOperator))
    });
  }

  NameMatcher Resolver(SF);
  return Resolver.resolve(UnresolvedLocs, SF.getAllTokens());
}

int swift::ide::syntacticRename(SourceFile *SF, ArrayRef<RenameLoc> RenameLocs,
                                SourceEditConsumer &EditConsumer,
                                DiagnosticConsumer &DiagConsumer) {

  assert(SF && "null source file");

  SourceManager &SM = SF->getASTContext().SourceMgr;
  DiagnosticEngine DiagEngine(SM);
  DiagEngine.addConsumer(DiagConsumer);

  auto ResolvedLocs = resolveRenameLocations(RenameLocs, *SF, DiagEngine);
  if (ResolvedLocs.size() != RenameLocs.size())
    return true; // Already diagnosed.

  size_t index = 0;
  llvm::StringSet<> ReplaceTextContext;
  for(const RenameLoc &Rename: RenameLocs) {
    ResolvedLoc &Resolved = ResolvedLocs[index++];
    TextReplacementsRenamer Renamer(SM, Rename.OldName, Rename.NewName,
                                    ReplaceTextContext);
    RegionType Type = Renamer.addSyntacticRenameRanges(Resolved, Rename);
    if (Type == RegionType::Mismatch) {
      DiagEngine.diagnose(Resolved.Range.getStart(), diag::mismatched_rename,
                          Rename.NewName);
      EditConsumer.accept(SM, Type, None);
    } else {
      EditConsumer.accept(SM, Type, Renamer.getReplacements());
    }
  }

  return false;
}

int swift::ide::findSyntacticRenameRanges(
    SourceFile *SF, ArrayRef<RenameLoc> RenameLocs,
    FindRenameRangesConsumer &RenameConsumer,
    DiagnosticConsumer &DiagConsumer) {
  assert(SF && "null source file");

  SourceManager &SM = SF->getASTContext().SourceMgr;
  DiagnosticEngine DiagEngine(SM);
  DiagEngine.addConsumer(DiagConsumer);

  auto ResolvedLocs = resolveRenameLocations(RenameLocs, *SF, DiagEngine);
  if (ResolvedLocs.size() != RenameLocs.size())
    return true; // Already diagnosed.

  size_t index = 0;
  for (const RenameLoc &Rename : RenameLocs) {
    ResolvedLoc &Resolved = ResolvedLocs[index++];
    RenameRangeDetailCollector Renamer(SM, Rename.OldName);
    RegionType Type = Renamer.addSyntacticRenameRanges(Resolved, Rename);
    if (Type == RegionType::Mismatch) {
      DiagEngine.diagnose(Resolved.Range.getStart(), diag::mismatched_rename,
                          Rename.NewName);
      RenameConsumer.accept(SM, Type, None);
    } else {
      RenameConsumer.accept(SM, Type, Renamer.Ranges);
    }
  }

  return false;
}

int swift::ide::findLocalRenameRanges(
    SourceFile *SF, RangeConfig Range,
    FindRenameRangesConsumer &RenameConsumer,
    DiagnosticConsumer &DiagConsumer) {
  assert(SF && "null source file");

  SourceManager &SM = SF->getASTContext().SourceMgr;
  DiagnosticEngine Diags(SM);
  Diags.addConsumer(DiagConsumer);

  auto StartLoc = Lexer::getLocForStartOfToken(SM, Range.getStart(SM));
  ResolvedCursorInfo CursorInfo =
    evaluateOrDefault(SF->getASTContext().evaluator,
                      CursorInfoRequest{CursorInfoOwner(SF, StartLoc)},
                      ResolvedCursorInfo());
  if (!CursorInfo.isValid() || !CursorInfo.ValueD) {
    Diags.diagnose(StartLoc, diag::unresolved_location);
    return true;
  }
  ValueDecl *VD = CursorInfo.typeOrValue();
  Optional<RenameRefInfo> RefInfo;
  if (CursorInfo.IsRef)
    RefInfo = {CursorInfo.SF, CursorInfo.Loc, CursorInfo.IsKeywordArgument};

  llvm::SmallVector<DeclContext *, 8> Scopes;
  analyzeRenameScope(VD, RefInfo, Diags, Scopes);
  if (Scopes.empty())
    return true;
  RenameRangeCollector RangeCollector(VD, StringRef());
  for (DeclContext *DC : Scopes)
    indexDeclContext(DC, RangeCollector);

  return findSyntacticRenameRanges(SF, RangeCollector.results(), RenameConsumer,
                                   DiagConsumer);
}
