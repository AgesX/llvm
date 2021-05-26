//===--- Parsing.cpp - Parsing function implementations ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Tooling/Transformer/Parsing.h"
#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/Transformer/RangeSelector.h"
#include "clang/Tooling/Transformer/SourceCode.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include <string>
#include <utility>
#include <vector>

using namespace clang;
using namespace transformer;

// FIXME: This implementation is entirely separate from that of the AST
// matchers. Given the similarity of the languages and uses of the two parsers,
// the two should share a common parsing infrastructure, as should other
// Transformer types. We intend to unify this implementation soon to share as
// much as possible with the AST Matchers parsing.

namespace {
using llvm::Error;
using llvm::Expected;

template <typename... Ts> using RangeSelectorOp = RangeSelector (*)(Ts...);

struct ParseState {
  // The remaining input to be processed.
  StringRef Input;
  // The original input. Not modified during parsing; only for reference in
  // error reporting.
  StringRef OriginalInput;
};

// Represents an intermediate result returned by a parsing function. Functions
// that don't generate values should use `llvm::None`
template <typename ResultType> struct ParseProgress {
  ParseState State;
  // Intermediate result generated by the Parser.
  ResultType Value;
};

template <typename T> using ExpectedProgress = llvm::Expected<ParseProgress<T>>;
template <typename T> using ParseFunction = ExpectedProgress<T> (*)(ParseState);

class ParseError : public llvm::ErrorInfo<ParseError> {
public:
  // Required field for all ErrorInfo derivatives.
  static char ID;

  ParseError(size_t Pos, std::string ErrorMsg, std::string InputExcerpt)
      : Pos(Pos), ErrorMsg(std::move(ErrorMsg)),
        Excerpt(std::move(InputExcerpt)) {}

  void log(llvm::raw_ostream &OS) const override {
    OS << "parse error at position (" << Pos << "): " << ErrorMsg
       << ": " + Excerpt;
  }

  std::error_code convertToErrorCode() const override {
    return llvm::inconvertibleErrorCode();
  }

  // Position of the error in the input string.
  size_t Pos;
  std::string ErrorMsg;
  // Excerpt of the input starting at the error position.
  std::string Excerpt;
};

char ParseError::ID;
} // namespace

static const llvm::StringMap<RangeSelectorOp<std::string>> &
getUnaryStringSelectors() {
  static const llvm::StringMap<RangeSelectorOp<std::string>> M = {
      {"name", name},
      {"node", node},
      {"statement", statement},
      {"statements", statements},
      {"member", member},
      {"callArgs", callArgs},
      {"elseBranch", elseBranch},
      {"initListElements", initListElements}};
  return M;
}

static const llvm::StringMap<RangeSelectorOp<RangeSelector>> &
getUnaryRangeSelectors() {
  static const llvm::StringMap<RangeSelectorOp<RangeSelector>> M = {
      {"before", before}, {"after", after}, {"expansion", expansion}};
  return M;
}

static const llvm::StringMap<RangeSelectorOp<std::string, std::string>> &
getBinaryStringSelectors() {
  static const llvm::StringMap<RangeSelectorOp<std::string, std::string>> M = {
      {"encloseNodes", encloseNodes}};
  return M;
}

static const llvm::StringMap<RangeSelectorOp<RangeSelector, RangeSelector>> &
getBinaryRangeSelectors() {
  static const llvm::StringMap<RangeSelectorOp<RangeSelector, RangeSelector>>
      M = {{"enclose", enclose}, {"between", between}};
  return M;
}

template <typename Element>
llvm::Optional<Element> findOptional(const llvm::StringMap<Element> &Map,
                                     llvm::StringRef Key) {
  auto it = Map.find(Key);
  if (it == Map.end())
    return llvm::None;
  return it->second;
}

template <typename ResultType>
ParseProgress<ResultType> makeParseProgress(ParseState State,
                                            ResultType Result) {
  return ParseProgress<ResultType>{State, std::move(Result)};
}

static llvm::Error makeParseError(const ParseState &S, std::string ErrorMsg) {
  size_t Pos = S.OriginalInput.size() - S.Input.size();
  return llvm::make_error<ParseError>(Pos, std::move(ErrorMsg),
                                      S.OriginalInput.substr(Pos, 20).str());
}

// Returns a new ParseState that advances \c S by \c N characters.
static ParseState advance(ParseState S, size_t N) {
  S.Input = S.Input.drop_front(N);
  return S;
}

static StringRef consumeWhitespace(StringRef S) {
  return S.drop_while([](char c) { return c >= 0 && isWhitespace(c); });
}

// Parses a single expected character \c c from \c State, skipping preceding
// whitespace.  Error if the expected character isn't found.
static ExpectedProgress<llvm::NoneType> parseChar(char c, ParseState State) {
  State.Input = consumeWhitespace(State.Input);
  if (State.Input.empty() || State.Input.front() != c)
    return makeParseError(State,
                          ("expected char not found: " + llvm::Twine(c)).str());
  return makeParseProgress(advance(State, 1), llvm::None);
}

// Parses an identitifer "token" -- handles preceding whitespace.
static ExpectedProgress<std::string> parseId(ParseState State) {
  State.Input = consumeWhitespace(State.Input);
  auto Id = State.Input.take_while(
      [](char c) { return c >= 0 && isIdentifierBody(c); });
  if (Id.empty())
    return makeParseError(State, "failed to parse name");
  return makeParseProgress(advance(State, Id.size()), Id.str());
}

// For consistency with the AST matcher parser and C++ code, node ids are
// written as strings. However, we do not support escaping in the string.
static ExpectedProgress<std::string> parseStringId(ParseState State) {
  State.Input = consumeWhitespace(State.Input);
  if (State.Input.empty())
    return makeParseError(State, "unexpected end of input");
  if (!State.Input.consume_front("\""))
    return makeParseError(
        State,
        "expecting string, but encountered other character or end of input");

  StringRef Id = State.Input.take_until([](char c) { return c == '"'; });
  if (State.Input.size() == Id.size())
    return makeParseError(State, "unterminated string");
  // Advance past the trailing quote as well.
  return makeParseProgress(advance(State, Id.size() + 1), Id.str());
}

// Parses a single element surrounded by parens. `Op` is applied to the parsed
// result to create the result of this function call.
template <typename T>
ExpectedProgress<RangeSelector> parseSingle(ParseFunction<T> ParseElement,
                                            RangeSelectorOp<T> Op,
                                            ParseState State) {
  auto P = parseChar('(', State);
  if (!P)
    return P.takeError();

  auto E = ParseElement(P->State);
  if (!E)
    return E.takeError();

  P = parseChar(')', E->State);
  if (!P)
    return P.takeError();

  return makeParseProgress(P->State, Op(std::move(E->Value)));
}

// Parses a pair of elements surrounded by parens and separated by comma. `Op`
// is applied to the parsed results to create the result of this function call.
template <typename T>
ExpectedProgress<RangeSelector> parsePair(ParseFunction<T> ParseElement,
                                          RangeSelectorOp<T, T> Op,
                                          ParseState State) {
  auto P = parseChar('(', State);
  if (!P)
    return P.takeError();

  auto Left = ParseElement(P->State);
  if (!Left)
    return Left.takeError();

  P = parseChar(',', Left->State);
  if (!P)
    return P.takeError();

  auto Right = ParseElement(P->State);
  if (!Right)
    return Right.takeError();

  P = parseChar(')', Right->State);
  if (!P)
    return P.takeError();

  return makeParseProgress(P->State,
                           Op(std::move(Left->Value), std::move(Right->Value)));
}

// Parses input for a stencil operator(single arg ops like AsValue, MemberOp or
// Id operator). Returns StencilType representing the operator on success and
// error if it fails to parse input for an operator.
static ExpectedProgress<RangeSelector>
parseRangeSelectorImpl(ParseState State) {
  auto Id = parseId(State);
  if (!Id)
    return Id.takeError();

  std::string OpName = std::move(Id->Value);
  if (auto Op = findOptional(getUnaryStringSelectors(), OpName))
    return parseSingle(parseStringId, *Op, Id->State);

  if (auto Op = findOptional(getUnaryRangeSelectors(), OpName))
    return parseSingle(parseRangeSelectorImpl, *Op, Id->State);

  if (auto Op = findOptional(getBinaryStringSelectors(), OpName))
    return parsePair(parseStringId, *Op, Id->State);

  if (auto Op = findOptional(getBinaryRangeSelectors(), OpName))
    return parsePair(parseRangeSelectorImpl, *Op, Id->State);

  return makeParseError(State, "unknown selector name: " + OpName);
}

Expected<RangeSelector> transformer::parseRangeSelector(llvm::StringRef Input) {
  ParseState State = {Input, Input};
  ExpectedProgress<RangeSelector> Result = parseRangeSelectorImpl(State);
  if (!Result)
    return Result.takeError();
  State = Result->State;
  // Discard any potentially trailing whitespace.
  State.Input = consumeWhitespace(State.Input);
  if (State.Input.empty())
    return Result->Value;
  return makeParseError(State, "unexpected input after selector");
}