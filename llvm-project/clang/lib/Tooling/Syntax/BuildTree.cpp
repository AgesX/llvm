//===- BuildTree.cpp ------------------------------------------*- C++ -*-=====//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "clang/Tooling/Syntax/BuildTree.h"
#include "clang/AST/ASTFwd.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclarationName.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/TypeLoc.h"
#include "clang/AST/TypeLocVisitor.h"
#include "clang/Basic/LLVM.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/LiteralSupport.h"
#include "clang/Tooling/Syntax/Nodes.h"
#include "clang/Tooling/Syntax/Tokens.h"
#include "clang/Tooling/Syntax/Tree.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <cstddef>
#include <map>

using namespace clang;

LLVM_ATTRIBUTE_UNUSED
static bool isImplicitExpr(Expr *E) { return E->IgnoreImplicit() != E; }

namespace {
/// Get start location of the Declarator from the TypeLoc.
/// E.g.:
///   loc of `(` in `int (a)`
///   loc of `*` in `int *(a)`
///   loc of the first `(` in `int (*a)(int)`
///   loc of the `*` in `int *(a)(int)`
///   loc of the first `*` in `const int *const *volatile a;`
///
/// It is non-trivial to get the start location because TypeLocs are stored
/// inside out. In the example above `*volatile` is the TypeLoc returned
/// by `Decl.getTypeSourceInfo()`, and `*const` is what `.getPointeeLoc()`
/// returns.
struct GetStartLoc : TypeLocVisitor<GetStartLoc, SourceLocation> {
  SourceLocation VisitParenTypeLoc(ParenTypeLoc T) {
    auto L = Visit(T.getInnerLoc());
    if (L.isValid())
      return L;
    return T.getLParenLoc();
  }

  // Types spelled in the prefix part of the declarator.
  SourceLocation VisitPointerTypeLoc(PointerTypeLoc T) {
    return HandlePointer(T);
  }

  SourceLocation VisitMemberPointerTypeLoc(MemberPointerTypeLoc T) {
    return HandlePointer(T);
  }

  SourceLocation VisitBlockPointerTypeLoc(BlockPointerTypeLoc T) {
    return HandlePointer(T);
  }

  SourceLocation VisitReferenceTypeLoc(ReferenceTypeLoc T) {
    return HandlePointer(T);
  }

  SourceLocation VisitObjCObjectPointerTypeLoc(ObjCObjectPointerTypeLoc T) {
    return HandlePointer(T);
  }

  // All other cases are not important, as they are either part of declaration
  // specifiers (e.g. inheritors of TypeSpecTypeLoc) or introduce modifiers on
  // existing declarators (e.g. QualifiedTypeLoc). They cannot start the
  // declarator themselves, but their underlying type can.
  SourceLocation VisitTypeLoc(TypeLoc T) {
    auto N = T.getNextTypeLoc();
    if (!N)
      return SourceLocation();
    return Visit(N);
  }

  SourceLocation VisitFunctionProtoTypeLoc(FunctionProtoTypeLoc T) {
    if (T.getTypePtr()->hasTrailingReturn())
      return SourceLocation(); // avoid recursing into the suffix of declarator.
    return VisitTypeLoc(T);
  }

private:
  template <class PtrLoc> SourceLocation HandlePointer(PtrLoc T) {
    auto L = Visit(T.getPointeeLoc());
    if (L.isValid())
      return L;
    return T.getLocalSourceRange().getBegin();
  }
};
} // namespace

static syntax::NodeKind getOperatorNodeKind(const CXXOperatorCallExpr &E) {
  switch (E.getOperator()) {
  // Comparison
  case OO_EqualEqual:
  case OO_ExclaimEqual:
  case OO_Greater:
  case OO_GreaterEqual:
  case OO_Less:
  case OO_LessEqual:
  case OO_Spaceship:
  // Assignment
  case OO_Equal:
  case OO_SlashEqual:
  case OO_PercentEqual:
  case OO_CaretEqual:
  case OO_PipeEqual:
  case OO_LessLessEqual:
  case OO_GreaterGreaterEqual:
  case OO_PlusEqual:
  case OO_MinusEqual:
  case OO_StarEqual:
  case OO_AmpEqual:
  // Binary computation
  case OO_Slash:
  case OO_Percent:
  case OO_Caret:
  case OO_Pipe:
  case OO_LessLess:
  case OO_GreaterGreater:
  case OO_AmpAmp:
  case OO_PipePipe:
  case OO_ArrowStar:
  case OO_Comma:
    return syntax::NodeKind::BinaryOperatorExpression;
  case OO_Tilde:
  case OO_Exclaim:
    return syntax::NodeKind::PrefixUnaryOperatorExpression;
  // Prefix/Postfix increment/decrement
  case OO_PlusPlus:
  case OO_MinusMinus:
    switch (E.getNumArgs()) {
    case 1:
      return syntax::NodeKind::PrefixUnaryOperatorExpression;
    case 2:
      return syntax::NodeKind::PostfixUnaryOperatorExpression;
    default:
      llvm_unreachable("Invalid number of arguments for operator");
    }
  // Operators that can be unary or binary
  case OO_Plus:
  case OO_Minus:
  case OO_Star:
  case OO_Amp:
    switch (E.getNumArgs()) {
    case 1:
      return syntax::NodeKind::PrefixUnaryOperatorExpression;
    case 2:
      return syntax::NodeKind::BinaryOperatorExpression;
    default:
      llvm_unreachable("Invalid number of arguments for operator");
    }
    return syntax::NodeKind::BinaryOperatorExpression;
  // Not yet supported by SyntaxTree
  case OO_New:
  case OO_Delete:
  case OO_Array_New:
  case OO_Array_Delete:
  case OO_Coawait:
  case OO_Call:
  case OO_Subscript:
  case OO_Arrow:
    return syntax::NodeKind::UnknownExpression;
  case OO_Conditional: // not overloadable
  case NUM_OVERLOADED_OPERATORS:
  case OO_None:
    llvm_unreachable("Not an overloadable operator");
  }
  llvm_unreachable("Unknown OverloadedOperatorKind enum");
}

/// Gets the range of declarator as defined by the C++ grammar. E.g.
///     `int a;` -> range of `a`,
///     `int *a;` -> range of `*a`,
///     `int a[10];` -> range of `a[10]`,
///     `int a[1][2][3];` -> range of `a[1][2][3]`,
///     `int *a = nullptr` -> range of `*a = nullptr`.
/// FIMXE: \p Name must be a source range, e.g. for `operator+`.
static SourceRange getDeclaratorRange(const SourceManager &SM, TypeLoc T,
                                      SourceLocation Name,
                                      SourceRange Initializer) {
  SourceLocation Start = GetStartLoc().Visit(T);
  SourceLocation End = T.getSourceRange().getEnd();
  assert(End.isValid());
  if (Name.isValid()) {
    if (Start.isInvalid())
      Start = Name;
    if (SM.isBeforeInTranslationUnit(End, Name))
      End = Name;
  }
  if (Initializer.isValid()) {
    auto InitializerEnd = Initializer.getEnd();
    assert(SM.isBeforeInTranslationUnit(End, InitializerEnd) ||
           End == InitializerEnd);
    End = InitializerEnd;
  }
  return SourceRange(Start, End);
}

namespace {
/// All AST hierarchy roots that can be represented as pointers.
using ASTPtr = llvm::PointerUnion<Stmt *, Decl *>;
/// Maintains a mapping from AST to syntax tree nodes. This class will get more
/// complicated as we support more kinds of AST nodes, e.g. TypeLocs.
/// FIXME: expose this as public API.
class ASTToSyntaxMapping {
public:
  void add(ASTPtr From, syntax::Tree *To) {
    assert(To != nullptr);
    assert(!From.isNull());

    bool Added = Nodes.insert({From, To}).second;
    (void)Added;
    assert(Added && "mapping added twice");
  }

  void add(NestedNameSpecifierLoc From, syntax::Tree *To) {
    assert(To != nullptr);
    assert(From.hasQualifier());

    bool Added = NNSNodes.insert({From, To}).second;
    (void)Added;
    assert(Added && "mapping added twice");
  }

  syntax::Tree *find(ASTPtr P) const { return Nodes.lookup(P); }

  syntax::Tree *find(NestedNameSpecifierLoc P) const {
    return NNSNodes.lookup(P);
  }

private:
  llvm::DenseMap<ASTPtr, syntax::Tree *> Nodes;
  llvm::DenseMap<NestedNameSpecifierLoc, syntax::Tree *> NNSNodes;
};
} // namespace

/// A helper class for constructing the syntax tree while traversing a clang
/// AST.
///
/// At each point of the traversal we maintain a list of pending nodes.
/// Initially all tokens are added as pending nodes. When processing a clang AST
/// node, the clients need to:
///   - create a corresponding syntax node,
///   - assign roles to all pending child nodes with 'markChild' and
///     'markChildToken',
///   - replace the child nodes with the new syntax node in the pending list
///     with 'foldNode'.
///
/// Note that all children are expected to be processed when building a node.
///
/// Call finalize() to finish building the tree and consume the root node.
class syntax::TreeBuilder {
public:
  TreeBuilder(syntax::Arena &Arena) : Arena(Arena), Pending(Arena) {
    for (const auto &T : Arena.tokenBuffer().expandedTokens())
      LocationToToken.insert({T.location().getRawEncoding(), &T});
  }

  llvm::BumpPtrAllocator &allocator() { return Arena.allocator(); }
  const SourceManager &sourceManager() const { return Arena.sourceManager(); }

  /// Populate children for \p New node, assuming it covers tokens from \p
  /// Range.
  void foldNode(ArrayRef<syntax::Token> Range, syntax::Tree *New, ASTPtr From) {
    assert(New);
    Pending.foldChildren(Arena, Range, New);
    if (From)
      Mapping.add(From, New);
  }

  void foldNode(ArrayRef<syntax::Token> Range, syntax::Tree *New, TypeLoc L) {
    // FIXME: add mapping for TypeLocs
    foldNode(Range, New, nullptr);
  }

  void foldNode(llvm::ArrayRef<syntax::Token> Range, syntax::Tree *New,
                NestedNameSpecifierLoc From) {
    assert(New);
    Pending.foldChildren(Arena, Range, New);
    if (From)
      Mapping.add(From, New);
  }

  /// Notifies that we should not consume trailing semicolon when computing
  /// token range of \p D.
  void noticeDeclWithoutSemicolon(Decl *D);

  /// Mark the \p Child node with a corresponding \p Role. All marked children
  /// should be consumed by foldNode.
  /// When called on expressions (clang::Expr is derived from clang::Stmt),
  /// wraps expressions into expression statement.
  void markStmtChild(Stmt *Child, NodeRole Role);
  /// Should be called for expressions in non-statement position to avoid
  /// wrapping into expression statement.
  void markExprChild(Expr *Child, NodeRole Role);
  /// Set role for a token starting at \p Loc.
  void markChildToken(SourceLocation Loc, NodeRole R);
  /// Set role for \p T.
  void markChildToken(const syntax::Token *T, NodeRole R);

  /// Set role for \p N.
  void markChild(syntax::Node *N, NodeRole R);
  /// Set role for the syntax node matching \p N.
  void markChild(ASTPtr N, NodeRole R);
  /// Set role for the syntax node matching \p N.
  void markChild(NestedNameSpecifierLoc N, NodeRole R);

  /// Finish building the tree and consume the root node.
  syntax::TranslationUnit *finalize() && {
    auto Tokens = Arena.tokenBuffer().expandedTokens();
    assert(!Tokens.empty());
    assert(Tokens.back().kind() == tok::eof);

    // Build the root of the tree, consuming all the children.
    Pending.foldChildren(Arena, Tokens.drop_back(),
                         new (Arena.allocator()) syntax::TranslationUnit);

    auto *TU = cast<syntax::TranslationUnit>(std::move(Pending).finalize());
    TU->assertInvariantsRecursive();
    return TU;
  }

  /// Finds a token starting at \p L. The token must exist if \p L is valid.
  const syntax::Token *findToken(SourceLocation L) const;

  /// Finds the syntax tokens corresponding to the \p SourceRange.
  ArrayRef<syntax::Token> getRange(SourceRange Range) const {
    assert(Range.isValid());
    return getRange(Range.getBegin(), Range.getEnd());
  }

  /// Finds the syntax tokens corresponding to the passed source locations.
  /// \p First is the start position of the first token and \p Last is the start
  /// position of the last token.
  ArrayRef<syntax::Token> getRange(SourceLocation First,
                                   SourceLocation Last) const {
    assert(First.isValid());
    assert(Last.isValid());
    assert(First == Last ||
           Arena.sourceManager().isBeforeInTranslationUnit(First, Last));
    return llvm::makeArrayRef(findToken(First), std::next(findToken(Last)));
  }

  ArrayRef<syntax::Token>
  getTemplateRange(const ClassTemplateSpecializationDecl *D) const {
    auto Tokens = getRange(D->getSourceRange());
    return maybeAppendSemicolon(Tokens, D);
  }

  /// Returns true if \p D is the last declarator in a chain and is thus
  /// reponsible for creating SimpleDeclaration for the whole chain.
  template <class T>
  bool isResponsibleForCreatingDeclaration(const T *D) const {
    static_assert((std::is_base_of<DeclaratorDecl, T>::value ||
                   std::is_base_of<TypedefNameDecl, T>::value),
                  "only DeclaratorDecl and TypedefNameDecl are supported.");

    const Decl *Next = D->getNextDeclInContext();

    // There's no next sibling, this one is responsible.
    if (Next == nullptr) {
      return true;
    }
    const auto *NextT = dyn_cast<T>(Next);

    // Next sibling is not the same type, this one is responsible.
    if (NextT == nullptr) {
      return true;
    }
    // Next sibling doesn't begin at the same loc, it must be a different
    // declaration, so this declarator is responsible.
    if (NextT->getBeginLoc() != D->getBeginLoc()) {
      return true;
    }

    // NextT is a member of the same declaration, and we need the last member to
    // create declaration. This one is not responsible.
    return false;
  }

  ArrayRef<syntax::Token> getDeclarationRange(Decl *D) {
    ArrayRef<syntax::Token> Tokens;
    // We want to drop the template parameters for specializations.
    if (const auto *S = dyn_cast<TagDecl>(D))
      Tokens = getRange(S->TypeDecl::getBeginLoc(), S->getEndLoc());
    else
      Tokens = getRange(D->getSourceRange());
    return maybeAppendSemicolon(Tokens, D);
  }

  ArrayRef<syntax::Token> getExprRange(const Expr *E) const {
    return getRange(E->getSourceRange());
  }

  /// Find the adjusted range for the statement, consuming the trailing
  /// semicolon when needed.
  ArrayRef<syntax::Token> getStmtRange(const Stmt *S) const {
    auto Tokens = getRange(S->getSourceRange());
    if (isa<CompoundStmt>(S))
      return Tokens;

    // Some statements miss a trailing semicolon, e.g. 'return', 'continue' and
    // all statements that end with those. Consume this semicolon here.
    if (Tokens.back().kind() == tok::semi)
      return Tokens;
    return withTrailingSemicolon(Tokens);
  }

private:
  ArrayRef<syntax::Token> maybeAppendSemicolon(ArrayRef<syntax::Token> Tokens,
                                               const Decl *D) const {
    if (isa<NamespaceDecl>(D))
      return Tokens;
    if (DeclsWithoutSemicolons.count(D))
      return Tokens;
    // FIXME: do not consume trailing semicolon on function definitions.
    // Most declarations own a semicolon in syntax trees, but not in clang AST.
    return withTrailingSemicolon(Tokens);
  }

  ArrayRef<syntax::Token>
  withTrailingSemicolon(ArrayRef<syntax::Token> Tokens) const {
    assert(!Tokens.empty());
    assert(Tokens.back().kind() != tok::eof);
    // We never consume 'eof', so looking at the next token is ok.
    if (Tokens.back().kind() != tok::semi && Tokens.end()->kind() == tok::semi)
      return llvm::makeArrayRef(Tokens.begin(), Tokens.end() + 1);
    return Tokens;
  }

  void setRole(syntax::Node *N, NodeRole R) {
    assert(N->role() == NodeRole::Detached);
    N->setRole(R);
  }

  /// A collection of trees covering the input tokens.
  /// When created, each tree corresponds to a single token in the file.
  /// Clients call 'foldChildren' to attach one or more subtrees to a parent
  /// node and update the list of trees accordingly.
  ///
  /// Ensures that added nodes properly nest and cover the whole token stream.
  struct Forest {
    Forest(syntax::Arena &A) {
      assert(!A.tokenBuffer().expandedTokens().empty());
      assert(A.tokenBuffer().expandedTokens().back().kind() == tok::eof);
      // Create all leaf nodes.
      // Note that we do not have 'eof' in the tree.
      for (auto &T : A.tokenBuffer().expandedTokens().drop_back()) {
        auto *L = new (A.allocator()) syntax::Leaf(&T);
        L->Original = true;
        L->CanModify = A.tokenBuffer().spelledForExpanded(T).hasValue();
        Trees.insert(Trees.end(), {&T, L});
      }
    }

    void assignRole(ArrayRef<syntax::Token> Range, syntax::NodeRole Role) {
      assert(!Range.empty());
      auto It = Trees.lower_bound(Range.begin());
      assert(It != Trees.end() && "no node found");
      assert(It->first == Range.begin() && "no child with the specified range");
      assert((std::next(It) == Trees.end() ||
              std::next(It)->first == Range.end()) &&
             "no child with the specified range");
      assert(It->second->role() == NodeRole::Detached &&
             "re-assigning role for a child");
      It->second->setRole(Role);
    }

    /// Add \p Node to the forest and attach child nodes based on \p Tokens.
    void foldChildren(const syntax::Arena &A, ArrayRef<syntax::Token> Tokens,
                      syntax::Tree *Node) {
      // Attach children to `Node`.
      assert(Node->firstChild() == nullptr && "node already has children");

      auto *FirstToken = Tokens.begin();
      auto BeginChildren = Trees.lower_bound(FirstToken);

      assert((BeginChildren == Trees.end() ||
              BeginChildren->first == FirstToken) &&
             "fold crosses boundaries of existing subtrees");
      auto EndChildren = Trees.lower_bound(Tokens.end());
      assert(
          (EndChildren == Trees.end() || EndChildren->first == Tokens.end()) &&
          "fold crosses boundaries of existing subtrees");

      // We need to go in reverse order, because we can only prepend.
      for (auto It = EndChildren; It != BeginChildren; --It) {
        auto *C = std::prev(It)->second;
        if (C->role() == NodeRole::Detached)
          C->setRole(NodeRole::Unknown);
        Node->prependChildLowLevel(C);
      }

      // Mark that this node came from the AST and is backed by the source code.
      Node->Original = true;
      Node->CanModify = A.tokenBuffer().spelledForExpanded(Tokens).hasValue();

      Trees.erase(BeginChildren, EndChildren);
      Trees.insert({FirstToken, Node});
    }

    // EXPECTS: all tokens were consumed and are owned by a single root node.
    syntax::Node *finalize() && {
      assert(Trees.size() == 1);
      auto *Root = Trees.begin()->second;
      Trees = {};
      return Root;
    }

    std::string str(const syntax::Arena &A) const {
      std::string R;
      for (auto It = Trees.begin(); It != Trees.end(); ++It) {
        unsigned CoveredTokens =
            It != Trees.end()
                ? (std::next(It)->first - It->first)
                : A.tokenBuffer().expandedTokens().end() - It->first;

        R += std::string(
            formatv("- '{0}' covers '{1}'+{2} tokens\n", It->second->kind(),
                    It->first->text(A.sourceManager()), CoveredTokens));
        R += It->second->dump(A);
      }
      return R;
    }

  private:
    /// Maps from the start token to a subtree starting at that token.
    /// Keys in the map are pointers into the array of expanded tokens, so
    /// pointer order corresponds to the order of preprocessor tokens.
    std::map<const syntax::Token *, syntax::Node *> Trees;
  };

  /// For debugging purposes.
  std::string str() { return Pending.str(Arena); }

  syntax::Arena &Arena;
  /// To quickly find tokens by their start location.
  llvm::DenseMap</*SourceLocation*/ unsigned, const syntax::Token *>
      LocationToToken;
  Forest Pending;
  llvm::DenseSet<Decl *> DeclsWithoutSemicolons;
  ASTToSyntaxMapping Mapping;
};

namespace {
class BuildTreeVisitor : public RecursiveASTVisitor<BuildTreeVisitor> {
public:
  explicit BuildTreeVisitor(ASTContext &Context, syntax::TreeBuilder &Builder)
      : Builder(Builder), Context(Context) {}

  bool shouldTraversePostOrder() const { return true; }

  bool WalkUpFromDeclaratorDecl(DeclaratorDecl *DD) {
    return processDeclaratorAndDeclaration(DD);
  }

  bool WalkUpFromTypedefNameDecl(TypedefNameDecl *TD) {
    return processDeclaratorAndDeclaration(TD);
  }

  bool VisitDecl(Decl *D) {
    assert(!D->isImplicit());
    Builder.foldNode(Builder.getDeclarationRange(D),
                     new (allocator()) syntax::UnknownDeclaration(), D);
    return true;
  }

  // RAV does not call WalkUpFrom* on explicit instantiations, so we have to
  // override Traverse.
  // FIXME: make RAV call WalkUpFrom* instead.
  bool
  TraverseClassTemplateSpecializationDecl(ClassTemplateSpecializationDecl *C) {
    if (!RecursiveASTVisitor::TraverseClassTemplateSpecializationDecl(C))
      return false;
    if (C->isExplicitSpecialization())
      return true; // we are only interested in explicit instantiations.
    auto *Declaration =
        cast<syntax::SimpleDeclaration>(handleFreeStandingTagDecl(C));
    foldExplicitTemplateInstantiation(
        Builder.getTemplateRange(C), Builder.findToken(C->getExternLoc()),
        Builder.findToken(C->getTemplateKeywordLoc()), Declaration, C);
    return true;
  }

  bool WalkUpFromTemplateDecl(TemplateDecl *S) {
    foldTemplateDeclaration(
        Builder.getDeclarationRange(S),
        Builder.findToken(S->getTemplateParameters()->getTemplateLoc()),
        Builder.getDeclarationRange(S->getTemplatedDecl()), S);
    return true;
  }

  bool WalkUpFromTagDecl(TagDecl *C) {
    // FIXME: build the ClassSpecifier node.
    if (!C->isFreeStanding()) {
      assert(C->getNumTemplateParameterLists() == 0);
      return true;
    }
    handleFreeStandingTagDecl(C);
    return true;
  }

  syntax::Declaration *handleFreeStandingTagDecl(TagDecl *C) {
    assert(C->isFreeStanding());
    // Class is a declaration specifier and needs a spanning declaration node.
    auto DeclarationRange = Builder.getDeclarationRange(C);
    syntax::Declaration *Result = new (allocator()) syntax::SimpleDeclaration;
    Builder.foldNode(DeclarationRange, Result, nullptr);

    // Build TemplateDeclaration nodes if we had template parameters.
    auto ConsumeTemplateParameters = [&](const TemplateParameterList &L) {
      const auto *TemplateKW = Builder.findToken(L.getTemplateLoc());
      auto R = llvm::makeArrayRef(TemplateKW, DeclarationRange.end());
      Result =
          foldTemplateDeclaration(R, TemplateKW, DeclarationRange, nullptr);
      DeclarationRange = R;
    };
    if (auto *S = dyn_cast<ClassTemplatePartialSpecializationDecl>(C))
      ConsumeTemplateParameters(*S->getTemplateParameters());
    for (unsigned I = C->getNumTemplateParameterLists(); 0 < I; --I)
      ConsumeTemplateParameters(*C->getTemplateParameterList(I - 1));
    return Result;
  }

  bool WalkUpFromTranslationUnitDecl(TranslationUnitDecl *TU) {
    // We do not want to call VisitDecl(), the declaration for translation
    // unit is built by finalize().
    return true;
  }

  bool WalkUpFromCompoundStmt(CompoundStmt *S) {
    using NodeRole = syntax::NodeRole;

    Builder.markChildToken(S->getLBracLoc(), NodeRole::OpenParen);
    for (auto *Child : S->body())
      Builder.markStmtChild(Child, NodeRole::CompoundStatement_statement);
    Builder.markChildToken(S->getRBracLoc(), NodeRole::CloseParen);

    Builder.foldNode(Builder.getStmtRange(S),
                     new (allocator()) syntax::CompoundStatement, S);
    return true;
  }

  // Some statements are not yet handled by syntax trees.
  bool WalkUpFromStmt(Stmt *S) {
    Builder.foldNode(Builder.getStmtRange(S),
                     new (allocator()) syntax::UnknownStatement, S);
    return true;
  }

  bool TraverseCXXForRangeStmt(CXXForRangeStmt *S) {
    // We override to traverse range initializer as VarDecl.
    // RAV traverses it as a statement, we produce invalid node kinds in that
    // case.
    // FIXME: should do this in RAV instead?
    bool Result = [&, this]() {
      if (S->getInit() && !TraverseStmt(S->getInit()))
        return false;
      if (S->getLoopVariable() && !TraverseDecl(S->getLoopVariable()))
        return false;
      if (S->getRangeInit() && !TraverseStmt(S->getRangeInit()))
        return false;
      if (S->getBody() && !TraverseStmt(S->getBody()))
        return false;
      return true;
    }();
    WalkUpFromCXXForRangeStmt(S);
    return Result;
  }

  bool TraverseStmt(Stmt *S) {
    if (auto *DS = dyn_cast_or_null<DeclStmt>(S)) {
      // We want to consume the semicolon, make sure SimpleDeclaration does not.
      for (auto *D : DS->decls())
        Builder.noticeDeclWithoutSemicolon(D);
    } else if (auto *E = dyn_cast_or_null<Expr>(S)) {
      return RecursiveASTVisitor::TraverseStmt(E->IgnoreImplicit());
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  // Some expressions are not yet handled by syntax trees.
  bool WalkUpFromExpr(Expr *E) {
    assert(!isImplicitExpr(E) && "should be handled by TraverseStmt");
    Builder.foldNode(Builder.getExprRange(E),
                     new (allocator()) syntax::UnknownExpression, E);
    return true;
  }

  bool TraverseUserDefinedLiteral(UserDefinedLiteral *S) {
    // The semantic AST node `UserDefinedLiteral` (UDL) may have one child node
    // referencing the location of the UDL suffix (`_w` in `1.2_w`). The
    // UDL suffix location does not point to the beginning of a token, so we
    // can't represent the UDL suffix as a separate syntax tree node.

    return WalkUpFromUserDefinedLiteral(S);
  }

  syntax::UserDefinedLiteralExpression *
  buildUserDefinedLiteral(UserDefinedLiteral *S) {
    switch (S->getLiteralOperatorKind()) {
    case UserDefinedLiteral::LOK_Integer:
      return new (allocator()) syntax::IntegerUserDefinedLiteralExpression;
    case UserDefinedLiteral::LOK_Floating:
      return new (allocator()) syntax::FloatUserDefinedLiteralExpression;
    case UserDefinedLiteral::LOK_Character:
      return new (allocator()) syntax::CharUserDefinedLiteralExpression;
    case UserDefinedLiteral::LOK_String:
      return new (allocator()) syntax::StringUserDefinedLiteralExpression;
    case UserDefinedLiteral::LOK_Raw:
    case UserDefinedLiteral::LOK_Template:
      // For raw literal operator and numeric literal operator template we
      // cannot get the type of the operand in the semantic AST. We get this
      // information from the token. As integer and floating point have the same
      // token kind, we run `NumericLiteralParser` again to distinguish them.
      auto TokLoc = S->getBeginLoc();
      auto TokSpelling =
          Builder.findToken(TokLoc)->text(Context.getSourceManager());
      auto Literal =
          NumericLiteralParser(TokSpelling, TokLoc, Context.getSourceManager(),
                               Context.getLangOpts(), Context.getTargetInfo(),
                               Context.getDiagnostics());
      if (Literal.isIntegerLiteral())
        return new (allocator()) syntax::IntegerUserDefinedLiteralExpression;
      else {
        assert(Literal.isFloatingLiteral());
        return new (allocator()) syntax::FloatUserDefinedLiteralExpression;
      }
    }
    llvm_unreachable("Unknown literal operator kind.");
  }

  bool WalkUpFromUserDefinedLiteral(UserDefinedLiteral *S) {
    Builder.markChildToken(S->getBeginLoc(), syntax::NodeRole::LiteralToken);
    Builder.foldNode(Builder.getExprRange(S), buildUserDefinedLiteral(S), S);
    return true;
  }

  // FIXME: Fix `NestedNameSpecifierLoc::getLocalSourceRange` for the
  // `DependentTemplateSpecializationType` case.
  /// Given a nested-name-specifier return the range for the last name
  /// specifier.
  ///
  /// e.g. `std::T::template X<U>::` => `template X<U>::`
  SourceRange getLocalSourceRange(const NestedNameSpecifierLoc &NNSLoc) {
    auto SR = NNSLoc.getLocalSourceRange();

    // The method `NestedNameSpecifierLoc::getLocalSourceRange` *should*
    // return the desired `SourceRange`, but there is a corner case. For a
    // `DependentTemplateSpecializationType` this method returns its
    // qualifiers as well, in other words in the example above this method
    // returns `T::template X<U>::` instead of only `template X<U>::`
    if (auto TL = NNSLoc.getTypeLoc()) {
      if (auto DependentTL =
              TL.getAs<DependentTemplateSpecializationTypeLoc>()) {
        // The 'template' keyword is always present in dependent template
        // specializations. Except in the case of incorrect code
        // TODO: Treat the case of incorrect code.
        SR.setBegin(DependentTL.getTemplateKeywordLoc());
      }
    }

    return SR;
  }

  syntax::NodeKind getNameSpecifierKind(const NestedNameSpecifier &NNS) {
    switch (NNS.getKind()) {
    case NestedNameSpecifier::Global:
      return syntax::NodeKind::GlobalNameSpecifier;
    case NestedNameSpecifier::Namespace:
    case NestedNameSpecifier::NamespaceAlias:
    case NestedNameSpecifier::Identifier:
      return syntax::NodeKind::IdentifierNameSpecifier;
    case NestedNameSpecifier::TypeSpecWithTemplate:
      return syntax::NodeKind::SimpleTemplateNameSpecifier;
    case NestedNameSpecifier::TypeSpec: {
      const auto *NNSType = NNS.getAsType();
      assert(NNSType);
      if (isa<DecltypeType>(NNSType))
        return syntax::NodeKind::DecltypeNameSpecifier;
      if (isa<TemplateSpecializationType, DependentTemplateSpecializationType>(
              NNSType))
        return syntax::NodeKind::SimpleTemplateNameSpecifier;
      return syntax::NodeKind::IdentifierNameSpecifier;
    }
    default:
      // FIXME: Support Microsoft's __super
      llvm::report_fatal_error("We don't yet support the __super specifier",
                               true);
    }
  }

  syntax::NameSpecifier *
  BuildNameSpecifier(const NestedNameSpecifierLoc &NNSLoc) {
    assert(NNSLoc.hasQualifier());
    auto NameSpecifierTokens =
        Builder.getRange(getLocalSourceRange(NNSLoc)).drop_back();
    switch (getNameSpecifierKind(*NNSLoc.getNestedNameSpecifier())) {
    case syntax::NodeKind::GlobalNameSpecifier:
      return new (allocator()) syntax::GlobalNameSpecifier;
    case syntax::NodeKind::IdentifierNameSpecifier: {
      assert(NameSpecifierTokens.size() == 1);
      Builder.markChildToken(NameSpecifierTokens.begin(),
                             syntax::NodeRole::Unknown);
      auto *NS = new (allocator()) syntax::IdentifierNameSpecifier;
      Builder.foldNode(NameSpecifierTokens, NS, nullptr);
      return NS;
    }
    case syntax::NodeKind::SimpleTemplateNameSpecifier: {
      // TODO: Build `SimpleTemplateNameSpecifier` children and implement
      // accessors to them.
      // Be aware, we cannot do that simply by calling `TraverseTypeLoc`,
      // some `TypeLoc`s have inside them the previous name specifier and
      // we want to treat them independently.
      auto *NS = new (allocator()) syntax::SimpleTemplateNameSpecifier;
      Builder.foldNode(NameSpecifierTokens, NS, nullptr);
      return NS;
    }
    case syntax::NodeKind::DecltypeNameSpecifier: {
      const auto TL = NNSLoc.getTypeLoc().castAs<DecltypeTypeLoc>();
      if (!RecursiveASTVisitor::TraverseDecltypeTypeLoc(TL))
        return nullptr;
      auto *NS = new (allocator()) syntax::DecltypeNameSpecifier;
      // TODO: Implement accessor to `DecltypeNameSpecifier` inner
      // `DecltypeTypeLoc`.
      // For that add mapping from `TypeLoc` to `syntax::Node*` then:
      // Builder.markChild(TypeLoc, syntax::NodeRole);
      Builder.foldNode(NameSpecifierTokens, NS, nullptr);
      return NS;
    }
    default:
      llvm_unreachable("getChildKind() does not return this value");
    }
  }

  // To build syntax tree nodes for NestedNameSpecifierLoc we override
  // Traverse instead of WalkUpFrom because we want to traverse the children
  // ourselves and build a list instead of a nested tree of name specifier
  // prefixes.
  bool TraverseNestedNameSpecifierLoc(NestedNameSpecifierLoc QualifierLoc) {
    if (!QualifierLoc)
      return true;
    for (auto it = QualifierLoc; it; it = it.getPrefix()) {
      auto *NS = BuildNameSpecifier(it);
      if (!NS)
        return false;
      Builder.markChild(NS, syntax::NodeRole::List_element);
      Builder.markChildToken(it.getEndLoc(), syntax::NodeRole::List_delimiter);
    }
    Builder.foldNode(Builder.getRange(QualifierLoc.getSourceRange()),
                     new (allocator()) syntax::NestedNameSpecifier,
                     QualifierLoc);
    return true;
  }

  syntax::IdExpression *buildIdExpression(NestedNameSpecifierLoc QualifierLoc,
                                          SourceLocation TemplateKeywordLoc,
                                          SourceRange UnqualifiedIdLoc,
                                          ASTPtr From) {
    if (QualifierLoc) {
      Builder.markChild(QualifierLoc, syntax::NodeRole::IdExpression_qualifier);
      if (TemplateKeywordLoc.isValid())
        Builder.markChildToken(TemplateKeywordLoc,
                               syntax::NodeRole::TemplateKeyword);
    }

    auto *TheUnqualifiedId = new (allocator()) syntax::UnqualifiedId;
    Builder.foldNode(Builder.getRange(UnqualifiedIdLoc), TheUnqualifiedId,
                     nullptr);
    Builder.markChild(TheUnqualifiedId, syntax::NodeRole::IdExpression_id);

    auto IdExpressionBeginLoc =
        QualifierLoc ? QualifierLoc.getBeginLoc() : UnqualifiedIdLoc.getBegin();

    auto *TheIdExpression = new (allocator()) syntax::IdExpression;
    Builder.foldNode(
        Builder.getRange(IdExpressionBeginLoc, UnqualifiedIdLoc.getEnd()),
        TheIdExpression, From);

    return TheIdExpression;
  }

  bool WalkUpFromMemberExpr(MemberExpr *S) {
    // For `MemberExpr` with implicit `this->` we generate a simple
    // `id-expression` syntax node, beacuse an implicit `member-expression` is
    // syntactically undistinguishable from an `id-expression`
    if (S->isImplicitAccess()) {
      buildIdExpression(S->getQualifierLoc(), S->getTemplateKeywordLoc(),
                        SourceRange(S->getMemberLoc(), S->getEndLoc()), S);
      return true;
    }

    auto *TheIdExpression = buildIdExpression(
        S->getQualifierLoc(), S->getTemplateKeywordLoc(),
        SourceRange(S->getMemberLoc(), S->getEndLoc()), nullptr);

    Builder.markChild(TheIdExpression,
                      syntax::NodeRole::MemberExpression_member);

    Builder.markExprChild(S->getBase(),
                          syntax::NodeRole::MemberExpression_object);
    Builder.markChildToken(S->getOperatorLoc(),
                           syntax::NodeRole::MemberExpression_accessToken);

    Builder.foldNode(Builder.getExprRange(S),
                     new (allocator()) syntax::MemberExpression, S);
    return true;
  }

  bool WalkUpFromDeclRefExpr(DeclRefExpr *S) {
    buildIdExpression(S->getQualifierLoc(), S->getTemplateKeywordLoc(),
                      SourceRange(S->getLocation(), S->getEndLoc()), S);

    return true;
  }

  // Same logic as DeclRefExpr.
  bool WalkUpFromDependentScopeDeclRefExpr(DependentScopeDeclRefExpr *S) {
    buildIdExpression(S->getQualifierLoc(), S->getTemplateKeywordLoc(),
                      SourceRange(S->getLocation(), S->getEndLoc()), S);

    return true;
  }

  bool WalkUpFromCXXThisExpr(CXXThisExpr *S) {
    if (!S->isImplicit()) {
      Builder.markChildToken(S->getLocation(),
                             syntax::NodeRole::IntroducerKeyword);
      Builder.foldNode(Builder.getExprRange(S),
                       new (allocator()) syntax::ThisExpression, S);
    }
    return true;
  }

  bool WalkUpFromParenExpr(ParenExpr *S) {
    Builder.markChildToken(S->getLParen(), syntax::NodeRole::OpenParen);
    Builder.markExprChild(S->getSubExpr(),
                          syntax::NodeRole::ParenExpression_subExpression);
    Builder.markChildToken(S->getRParen(), syntax::NodeRole::CloseParen);
    Builder.foldNode(Builder.getExprRange(S),
                     new (allocator()) syntax::ParenExpression, S);
    return true;
  }

  bool WalkUpFromIntegerLiteral(IntegerLiteral *S) {
    Builder.markChildToken(S->getLocation(), syntax::NodeRole::LiteralToken);
    Builder.foldNode(Builder.getExprRange(S),
                     new (allocator()) syntax::IntegerLiteralExpression, S);
    return true;
  }

  bool WalkUpFromCharacterLiteral(CharacterLiteral *S) {
    Builder.markChildToken(S->getLocation(), syntax::NodeRole::LiteralToken);
    Builder.foldNode(Builder.getExprRange(S),
                     new (allocator()) syntax::CharacterLiteralExpression, S);
    return true;
  }

  bool WalkUpFromFloatingLiteral(FloatingLiteral *S) {
    Builder.markChildToken(S->getLocation(), syntax::NodeRole::LiteralToken);
    Builder.foldNode(Builder.getExprRange(S),
                     new (allocator()) syntax::FloatingLiteralExpression, S);
    return true;
  }

  bool WalkUpFromStringLiteral(StringLiteral *S) {
    Builder.markChildToken(S->getBeginLoc(), syntax::NodeRole::LiteralToken);
    Builder.foldNode(Builder.getExprRange(S),
                     new (allocator()) syntax::StringLiteralExpression, S);
    return true;
  }

  bool WalkUpFromCXXBoolLiteralExpr(CXXBoolLiteralExpr *S) {
    Builder.markChildToken(S->getLocation(), syntax::NodeRole::LiteralToken);
    Builder.foldNode(Builder.getExprRange(S),
                     new (allocator()) syntax::BoolLiteralExpression, S);
    return true;
  }

  bool WalkUpFromCXXNullPtrLiteralExpr(CXXNullPtrLiteralExpr *S) {
    Builder.markChildToken(S->getLocation(), syntax::NodeRole::LiteralToken);
    Builder.foldNode(Builder.getExprRange(S),
                     new (allocator()) syntax::CxxNullPtrExpression, S);
    return true;
  }

  bool WalkUpFromUnaryOperator(UnaryOperator *S) {
    Builder.markChildToken(S->getOperatorLoc(),
                           syntax::NodeRole::OperatorExpression_operatorToken);
    Builder.markExprChild(S->getSubExpr(),
                          syntax::NodeRole::UnaryOperatorExpression_operand);

    if (S->isPostfix())
      Builder.foldNode(Builder.getExprRange(S),
                       new (allocator()) syntax::PostfixUnaryOperatorExpression,
                       S);
    else
      Builder.foldNode(Builder.getExprRange(S),
                       new (allocator()) syntax::PrefixUnaryOperatorExpression,
                       S);

    return true;
  }

  bool WalkUpFromBinaryOperator(BinaryOperator *S) {
    Builder.markExprChild(
        S->getLHS(), syntax::NodeRole::BinaryOperatorExpression_leftHandSide);
    Builder.markChildToken(S->getOperatorLoc(),
                           syntax::NodeRole::OperatorExpression_operatorToken);
    Builder.markExprChild(
        S->getRHS(), syntax::NodeRole::BinaryOperatorExpression_rightHandSide);
    Builder.foldNode(Builder.getExprRange(S),
                     new (allocator()) syntax::BinaryOperatorExpression, S);
    return true;
  }

  bool TraverseCXXOperatorCallExpr(CXXOperatorCallExpr *S) {
    // To construct a syntax tree of the same shape for calls to built-in and
    // user-defined operators, ignore the `DeclRefExpr` that refers to the
    // operator and treat it as a simple token. Do that by traversing
    // arguments instead of children.
    for (auto *child : S->arguments()) {
      // A postfix unary operator is declared as taking two operands. The
      // second operand is used to distinguish from its prefix counterpart. In
      // the semantic AST this "phantom" operand is represented as a
      // `IntegerLiteral` with invalid `SourceLocation`. We skip visiting this
      // operand because it does not correspond to anything written in source
      // code.
      if (child->getSourceRange().isInvalid()) {
        assert(getOperatorNodeKind(*S) ==
               syntax::NodeKind::PostfixUnaryOperatorExpression);
        continue;
      }
      if (!TraverseStmt(child))
        return false;
    }
    return WalkUpFromCXXOperatorCallExpr(S);
  }

  bool WalkUpFromCXXOperatorCallExpr(CXXOperatorCallExpr *S) {
    switch (getOperatorNodeKind(*S)) {
    case syntax::NodeKind::BinaryOperatorExpression:
      Builder.markExprChild(
          S->getArg(0),
          syntax::NodeRole::BinaryOperatorExpression_leftHandSide);
      Builder.markChildToken(
          S->getOperatorLoc(),
          syntax::NodeRole::OperatorExpression_operatorToken);
      Builder.markExprChild(
          S->getArg(1),
          syntax::NodeRole::BinaryOperatorExpression_rightHandSide);
      Builder.foldNode(Builder.getExprRange(S),
                       new (allocator()) syntax::BinaryOperatorExpression, S);
      return true;
    case syntax::NodeKind::PrefixUnaryOperatorExpression:
      Builder.markChildToken(
          S->getOperatorLoc(),
          syntax::NodeRole::OperatorExpression_operatorToken);
      Builder.markExprChild(S->getArg(0),
                            syntax::NodeRole::UnaryOperatorExpression_operand);
      Builder.foldNode(Builder.getExprRange(S),
                       new (allocator()) syntax::PrefixUnaryOperatorExpression,
                       S);
      return true;
    case syntax::NodeKind::PostfixUnaryOperatorExpression:
      Builder.markChildToken(
          S->getOperatorLoc(),
          syntax::NodeRole::OperatorExpression_operatorToken);
      Builder.markExprChild(S->getArg(0),
                            syntax::NodeRole::UnaryOperatorExpression_operand);
      Builder.foldNode(Builder.getExprRange(S),
                       new (allocator()) syntax::PostfixUnaryOperatorExpression,
                       S);
      return true;
    case syntax::NodeKind::UnknownExpression:
      return RecursiveASTVisitor::WalkUpFromCXXOperatorCallExpr(S);
    default:
      llvm_unreachable("getOperatorNodeKind() does not return this value");
    }
  }

  bool WalkUpFromNamespaceDecl(NamespaceDecl *S) {
    auto Tokens = Builder.getDeclarationRange(S);
    if (Tokens.front().kind() == tok::coloncolon) {
      // Handle nested namespace definitions. Those start at '::' token, e.g.
      // namespace a^::b {}
      // FIXME: build corresponding nodes for the name of this namespace.
      return true;
    }
    Builder.foldNode(Tokens, new (allocator()) syntax::NamespaceDefinition, S);
    return true;
  }

  // FIXME: Deleting the `TraverseParenTypeLoc` override doesn't change test
  // results. Find test coverage or remove it.
  bool TraverseParenTypeLoc(ParenTypeLoc L) {
    // We reverse order of traversal to get the proper syntax structure.
    if (!WalkUpFromParenTypeLoc(L))
      return false;
    return TraverseTypeLoc(L.getInnerLoc());
  }

  bool WalkUpFromParenTypeLoc(ParenTypeLoc L) {
    Builder.markChildToken(L.getLParenLoc(), syntax::NodeRole::OpenParen);
    Builder.markChildToken(L.getRParenLoc(), syntax::NodeRole::CloseParen);
    Builder.foldNode(Builder.getRange(L.getLParenLoc(), L.getRParenLoc()),
                     new (allocator()) syntax::ParenDeclarator, L);
    return true;
  }

  // Declarator chunks, they are produced by type locs and some clang::Decls.
  bool WalkUpFromArrayTypeLoc(ArrayTypeLoc L) {
    Builder.markChildToken(L.getLBracketLoc(), syntax::NodeRole::OpenParen);
    Builder.markExprChild(L.getSizeExpr(),
                          syntax::NodeRole::ArraySubscript_sizeExpression);
    Builder.markChildToken(L.getRBracketLoc(), syntax::NodeRole::CloseParen);
    Builder.foldNode(Builder.getRange(L.getLBracketLoc(), L.getRBracketLoc()),
                     new (allocator()) syntax::ArraySubscript, L);
    return true;
  }

  bool WalkUpFromFunctionTypeLoc(FunctionTypeLoc L) {
    Builder.markChildToken(L.getLParenLoc(), syntax::NodeRole::OpenParen);
    for (auto *P : L.getParams()) {
      Builder.markChild(P, syntax::NodeRole::ParametersAndQualifiers_parameter);
    }
    Builder.markChildToken(L.getRParenLoc(), syntax::NodeRole::CloseParen);
    Builder.foldNode(Builder.getRange(L.getLParenLoc(), L.getEndLoc()),
                     new (allocator()) syntax::ParametersAndQualifiers, L);
    return true;
  }

  bool WalkUpFromFunctionProtoTypeLoc(FunctionProtoTypeLoc L) {
    if (!L.getTypePtr()->hasTrailingReturn())
      return WalkUpFromFunctionTypeLoc(L);

    auto *TrailingReturnTokens = BuildTrailingReturn(L);
    // Finish building the node for parameters.
    Builder.markChild(TrailingReturnTokens,
                      syntax::NodeRole::ParametersAndQualifiers_trailingReturn);
    return WalkUpFromFunctionTypeLoc(L);
  }

  bool TraverseMemberPointerTypeLoc(MemberPointerTypeLoc L) {
    // In the source code "void (Y::*mp)()" `MemberPointerTypeLoc` corresponds
    // to "Y::*" but it points to a `ParenTypeLoc` that corresponds to
    // "(Y::*mp)" We thus reverse the order of traversal to get the proper
    // syntax structure.
    if (!WalkUpFromMemberPointerTypeLoc(L))
      return false;
    return TraverseTypeLoc(L.getPointeeLoc());
  }

  bool WalkUpFromMemberPointerTypeLoc(MemberPointerTypeLoc L) {
    auto SR = L.getLocalSourceRange();
    Builder.foldNode(Builder.getRange(SR),
                     new (allocator()) syntax::MemberPointer, L);
    return true;
  }

  // The code below is very regular, it could even be generated with some
  // preprocessor magic. We merely assign roles to the corresponding children
  // and fold resulting nodes.
  bool WalkUpFromDeclStmt(DeclStmt *S) {
    Builder.foldNode(Builder.getStmtRange(S),
                     new (allocator()) syntax::DeclarationStatement, S);
    return true;
  }

  bool WalkUpFromNullStmt(NullStmt *S) {
    Builder.foldNode(Builder.getStmtRange(S),
                     new (allocator()) syntax::EmptyStatement, S);
    return true;
  }

  bool WalkUpFromSwitchStmt(SwitchStmt *S) {
    Builder.markChildToken(S->getSwitchLoc(),
                           syntax::NodeRole::IntroducerKeyword);
    Builder.markStmtChild(S->getBody(), syntax::NodeRole::BodyStatement);
    Builder.foldNode(Builder.getStmtRange(S),
                     new (allocator()) syntax::SwitchStatement, S);
    return true;
  }

  bool WalkUpFromCaseStmt(CaseStmt *S) {
    Builder.markChildToken(S->getKeywordLoc(),
                           syntax::NodeRole::IntroducerKeyword);
    Builder.markExprChild(S->getLHS(), syntax::NodeRole::CaseStatement_value);
    Builder.markStmtChild(S->getSubStmt(), syntax::NodeRole::BodyStatement);
    Builder.foldNode(Builder.getStmtRange(S),
                     new (allocator()) syntax::CaseStatement, S);
    return true;
  }

  bool WalkUpFromDefaultStmt(DefaultStmt *S) {
    Builder.markChildToken(S->getKeywordLoc(),
                           syntax::NodeRole::IntroducerKeyword);
    Builder.markStmtChild(S->getSubStmt(), syntax::NodeRole::BodyStatement);
    Builder.foldNode(Builder.getStmtRange(S),
                     new (allocator()) syntax::DefaultStatement, S);
    return true;
  }

  bool WalkUpFromIfStmt(IfStmt *S) {
    Builder.markChildToken(S->getIfLoc(), syntax::NodeRole::IntroducerKeyword);
    Builder.markStmtChild(S->getThen(),
                          syntax::NodeRole::IfStatement_thenStatement);
    Builder.markChildToken(S->getElseLoc(),
                           syntax::NodeRole::IfStatement_elseKeyword);
    Builder.markStmtChild(S->getElse(),
                          syntax::NodeRole::IfStatement_elseStatement);
    Builder.foldNode(Builder.getStmtRange(S),
                     new (allocator()) syntax::IfStatement, S);
    return true;
  }

  bool WalkUpFromForStmt(ForStmt *S) {
    Builder.markChildToken(S->getForLoc(), syntax::NodeRole::IntroducerKeyword);
    Builder.markStmtChild(S->getBody(), syntax::NodeRole::BodyStatement);
    Builder.foldNode(Builder.getStmtRange(S),
                     new (allocator()) syntax::ForStatement, S);
    return true;
  }

  bool WalkUpFromWhileStmt(WhileStmt *S) {
    Builder.markChildToken(S->getWhileLoc(),
                           syntax::NodeRole::IntroducerKeyword);
    Builder.markStmtChild(S->getBody(), syntax::NodeRole::BodyStatement);
    Builder.foldNode(Builder.getStmtRange(S),
                     new (allocator()) syntax::WhileStatement, S);
    return true;
  }

  bool WalkUpFromContinueStmt(ContinueStmt *S) {
    Builder.markChildToken(S->getContinueLoc(),
                           syntax::NodeRole::IntroducerKeyword);
    Builder.foldNode(Builder.getStmtRange(S),
                     new (allocator()) syntax::ContinueStatement, S);
    return true;
  }

  bool WalkUpFromBreakStmt(BreakStmt *S) {
    Builder.markChildToken(S->getBreakLoc(),
                           syntax::NodeRole::IntroducerKeyword);
    Builder.foldNode(Builder.getStmtRange(S),
                     new (allocator()) syntax::BreakStatement, S);
    return true;
  }

  bool WalkUpFromReturnStmt(ReturnStmt *S) {
    Builder.markChildToken(S->getReturnLoc(),
                           syntax::NodeRole::IntroducerKeyword);
    Builder.markExprChild(S->getRetValue(),
                          syntax::NodeRole::ReturnStatement_value);
    Builder.foldNode(Builder.getStmtRange(S),
                     new (allocator()) syntax::ReturnStatement, S);
    return true;
  }

  bool WalkUpFromCXXForRangeStmt(CXXForRangeStmt *S) {
    Builder.markChildToken(S->getForLoc(), syntax::NodeRole::IntroducerKeyword);
    Builder.markStmtChild(S->getBody(), syntax::NodeRole::BodyStatement);
    Builder.foldNode(Builder.getStmtRange(S),
                     new (allocator()) syntax::RangeBasedForStatement, S);
    return true;
  }

  bool WalkUpFromEmptyDecl(EmptyDecl *S) {
    Builder.foldNode(Builder.getDeclarationRange(S),
                     new (allocator()) syntax::EmptyDeclaration, S);
    return true;
  }

  bool WalkUpFromStaticAssertDecl(StaticAssertDecl *S) {
    Builder.markExprChild(S->getAssertExpr(),
                          syntax::NodeRole::StaticAssertDeclaration_condition);
    Builder.markExprChild(S->getMessage(),
                          syntax::NodeRole::StaticAssertDeclaration_message);
    Builder.foldNode(Builder.getDeclarationRange(S),
                     new (allocator()) syntax::StaticAssertDeclaration, S);
    return true;
  }

  bool WalkUpFromLinkageSpecDecl(LinkageSpecDecl *S) {
    Builder.foldNode(Builder.getDeclarationRange(S),
                     new (allocator()) syntax::LinkageSpecificationDeclaration,
                     S);
    return true;
  }

  bool WalkUpFromNamespaceAliasDecl(NamespaceAliasDecl *S) {
    Builder.foldNode(Builder.getDeclarationRange(S),
                     new (allocator()) syntax::NamespaceAliasDefinition, S);
    return true;
  }

  bool WalkUpFromUsingDirectiveDecl(UsingDirectiveDecl *S) {
    Builder.foldNode(Builder.getDeclarationRange(S),
                     new (allocator()) syntax::UsingNamespaceDirective, S);
    return true;
  }

  bool WalkUpFromUsingDecl(UsingDecl *S) {
    Builder.foldNode(Builder.getDeclarationRange(S),
                     new (allocator()) syntax::UsingDeclaration, S);
    return true;
  }

  bool WalkUpFromUnresolvedUsingValueDecl(UnresolvedUsingValueDecl *S) {
    Builder.foldNode(Builder.getDeclarationRange(S),
                     new (allocator()) syntax::UsingDeclaration, S);
    return true;
  }

  bool WalkUpFromUnresolvedUsingTypenameDecl(UnresolvedUsingTypenameDecl *S) {
    Builder.foldNode(Builder.getDeclarationRange(S),
                     new (allocator()) syntax::UsingDeclaration, S);
    return true;
  }

  bool WalkUpFromTypeAliasDecl(TypeAliasDecl *S) {
    Builder.foldNode(Builder.getDeclarationRange(S),
                     new (allocator()) syntax::TypeAliasDeclaration, S);
    return true;
  }

private:
  template <class T> SourceLocation getQualifiedNameStart(T *D) {
    static_assert((std::is_base_of<DeclaratorDecl, T>::value ||
                   std::is_base_of<TypedefNameDecl, T>::value),
                  "only DeclaratorDecl and TypedefNameDecl are supported.");

    auto DN = D->getDeclName();
    bool IsAnonymous = DN.isIdentifier() && !DN.getAsIdentifierInfo();
    if (IsAnonymous)
      return SourceLocation();

    if (const auto *DD = dyn_cast<DeclaratorDecl>(D)) {
      if (DD->getQualifierLoc()) {
        return DD->getQualifierLoc().getBeginLoc();
      }
    }

    return D->getLocation();
  }

  SourceRange getInitializerRange(Decl *D) {
    if (auto *V = dyn_cast<VarDecl>(D)) {
      auto *I = V->getInit();
      // Initializers in range-based-for are not part of the declarator
      if (I && !V->isCXXForRangeDecl())
        return I->getSourceRange();
    }

    return SourceRange();
  }

  /// Folds SimpleDeclarator node (if present) and in case this is the last
  /// declarator in the chain it also folds SimpleDeclaration node.
  template <class T> bool processDeclaratorAndDeclaration(T *D) {
    SourceRange Initializer = getInitializerRange(D);
    auto Range = getDeclaratorRange(Builder.sourceManager(),
                                    D->getTypeSourceInfo()->getTypeLoc(),
                                    getQualifiedNameStart(D), Initializer);

    // There doesn't have to be a declarator (e.g. `void foo(int)` only has
    // declaration, but no declarator).
    if (Range.getBegin().isValid()) {
      auto *N = new (allocator()) syntax::SimpleDeclarator;
      Builder.foldNode(Builder.getRange(Range), N, nullptr);
      Builder.markChild(N, syntax::NodeRole::SimpleDeclaration_declarator);
    }

    if (Builder.isResponsibleForCreatingDeclaration(D)) {
      Builder.foldNode(Builder.getDeclarationRange(D),
                       new (allocator()) syntax::SimpleDeclaration, D);
    }
    return true;
  }

  /// Returns the range of the built node.
  syntax::TrailingReturnType *BuildTrailingReturn(FunctionProtoTypeLoc L) {
    assert(L.getTypePtr()->hasTrailingReturn());

    auto ReturnedType = L.getReturnLoc();
    // Build node for the declarator, if any.
    auto ReturnDeclaratorRange =
        getDeclaratorRange(this->Builder.sourceManager(), ReturnedType,
                           /*Name=*/SourceLocation(),
                           /*Initializer=*/SourceLocation());
    syntax::SimpleDeclarator *ReturnDeclarator = nullptr;
    if (ReturnDeclaratorRange.isValid()) {
      ReturnDeclarator = new (allocator()) syntax::SimpleDeclarator;
      Builder.foldNode(Builder.getRange(ReturnDeclaratorRange),
                       ReturnDeclarator, nullptr);
    }

    // Build node for trailing return type.
    auto Return = Builder.getRange(ReturnedType.getSourceRange());
    const auto *Arrow = Return.begin() - 1;
    assert(Arrow->kind() == tok::arrow);
    auto Tokens = llvm::makeArrayRef(Arrow, Return.end());
    Builder.markChildToken(Arrow, syntax::NodeRole::ArrowToken);
    if (ReturnDeclarator)
      Builder.markChild(ReturnDeclarator,
                        syntax::NodeRole::TrailingReturnType_declarator);
    auto *R = new (allocator()) syntax::TrailingReturnType;
    Builder.foldNode(Tokens, R, L);
    return R;
  }

  void foldExplicitTemplateInstantiation(
      ArrayRef<syntax::Token> Range, const syntax::Token *ExternKW,
      const syntax::Token *TemplateKW,
      syntax::SimpleDeclaration *InnerDeclaration, Decl *From) {
    assert(!ExternKW || ExternKW->kind() == tok::kw_extern);
    assert(TemplateKW && TemplateKW->kind() == tok::kw_template);
    Builder.markChildToken(ExternKW, syntax::NodeRole::ExternKeyword);
    Builder.markChildToken(TemplateKW, syntax::NodeRole::IntroducerKeyword);
    Builder.markChild(
        InnerDeclaration,
        syntax::NodeRole::ExplicitTemplateInstantiation_declaration);
    Builder.foldNode(
        Range, new (allocator()) syntax::ExplicitTemplateInstantiation, From);
  }

  syntax::TemplateDeclaration *foldTemplateDeclaration(
      ArrayRef<syntax::Token> Range, const syntax::Token *TemplateKW,
      ArrayRef<syntax::Token> TemplatedDeclaration, Decl *From) {
    assert(TemplateKW && TemplateKW->kind() == tok::kw_template);
    Builder.markChildToken(TemplateKW, syntax::NodeRole::IntroducerKeyword);

    auto *N = new (allocator()) syntax::TemplateDeclaration;
    Builder.foldNode(Range, N, From);
    Builder.markChild(N, syntax::NodeRole::TemplateDeclaration_declaration);
    return N;
  }

  /// A small helper to save some typing.
  llvm::BumpPtrAllocator &allocator() { return Builder.allocator(); }

  syntax::TreeBuilder &Builder;
  const ASTContext &Context;
};
} // namespace

void syntax::TreeBuilder::noticeDeclWithoutSemicolon(Decl *D) {
  DeclsWithoutSemicolons.insert(D);
}

void syntax::TreeBuilder::markChildToken(SourceLocation Loc, NodeRole Role) {
  if (Loc.isInvalid())
    return;
  Pending.assignRole(*findToken(Loc), Role);
}

void syntax::TreeBuilder::markChildToken(const syntax::Token *T, NodeRole R) {
  if (!T)
    return;
  Pending.assignRole(*T, R);
}

void syntax::TreeBuilder::markChild(syntax::Node *N, NodeRole R) {
  assert(N);
  setRole(N, R);
}

void syntax::TreeBuilder::markChild(ASTPtr N, NodeRole R) {
  auto *SN = Mapping.find(N);
  assert(SN != nullptr);
  setRole(SN, R);
}
void syntax::TreeBuilder::markChild(NestedNameSpecifierLoc NNSLoc, NodeRole R) {
  auto *SN = Mapping.find(NNSLoc);
  assert(SN != nullptr);
  setRole(SN, R);
}

void syntax::TreeBuilder::markStmtChild(Stmt *Child, NodeRole Role) {
  if (!Child)
    return;

  syntax::Tree *ChildNode;
  if (Expr *ChildExpr = dyn_cast<Expr>(Child)) {
    // This is an expression in a statement position, consume the trailing
    // semicolon and form an 'ExpressionStatement' node.
    markExprChild(ChildExpr, NodeRole::ExpressionStatement_expression);
    ChildNode = new (allocator()) syntax::ExpressionStatement;
    // (!) 'getStmtRange()' ensures this covers a trailing semicolon.
    Pending.foldChildren(Arena, getStmtRange(Child), ChildNode);
  } else {
    ChildNode = Mapping.find(Child);
  }
  assert(ChildNode != nullptr);
  setRole(ChildNode, Role);
}

void syntax::TreeBuilder::markExprChild(Expr *Child, NodeRole Role) {
  if (!Child)
    return;
  Child = Child->IgnoreImplicit();

  syntax::Tree *ChildNode = Mapping.find(Child);
  assert(ChildNode != nullptr);
  setRole(ChildNode, Role);
}

const syntax::Token *syntax::TreeBuilder::findToken(SourceLocation L) const {
  if (L.isInvalid())
    return nullptr;
  auto It = LocationToToken.find(L.getRawEncoding());
  assert(It != LocationToToken.end());
  return It->second;
}

syntax::TranslationUnit *
syntax::buildSyntaxTree(Arena &A, const TranslationUnitDecl &TU) {
  TreeBuilder Builder(A);
  BuildTreeVisitor(TU.getASTContext(), Builder).TraverseAST(TU.getASTContext());
  return std::move(Builder).finalize();
}
