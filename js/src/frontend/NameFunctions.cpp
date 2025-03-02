/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/NameFunctions.h"

#include "mozilla/MemoryChecking.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Sprintf.h"

#include "frontend/BytecodeCompiler.h"
#include "frontend/CompilationInfo.h"
#include "frontend/ParseNode.h"
#include "frontend/ParseNodeVisitor.h"
#include "frontend/SharedContext.h"
#include "util/Poison.h"
#include "util/StringBuffer.h"
#include "vm/JSFunction.h"

using namespace js;
using namespace js::frontend;

namespace {

class NameResolver : public ParseNodeVisitor<NameResolver> {
  using Base = ParseNodeVisitor;

  static const size_t MaxParents = 100;

  CompilationInfo& compilationInfo_;
  const ParserAtom* prefix_;

  // Number of nodes in the parents array.
  size_t nparents_;

  // Stack of ParseNodes from the root to the current node.
  // Only elements 0..nparents_ are initialized.
  MOZ_INIT_OUTSIDE_CTOR
  ParseNode* parents_[MaxParents];

  // When naming a function, the buffer where the name is built.
  // When we are not under resolveFun, buf_ is empty.
  StringBuffer buf_;

  /* Test whether a ParseNode represents a function invocation */
  bool isCall(ParseNode* pn) {
    return pn && pn->isKind(ParseNodeKind::CallExpr);
  }

  /*
   * Append a reference to a property named |name| to |buf_|. If |name| is
   * a proper identifier name, then we append '.name'; otherwise, we
   * append '["name"]'.
   *
   * Note that we need the IsIdentifier check for atoms from both
   * ParseNodeKind::Name nodes and ParseNodeKind::String nodes:
   * given code like a["b c"], the front end will produce a ParseNodeKind::Dot
   * with a ParseNodeKind::Name child whose name contains spaces.
   */
  bool appendPropertyReference(const ParserAtom* name) {
    if (IsIdentifier(name)) {
      return buf_.append('.') && buf_.append(name);
    }

    /* Quote the string as needed. */
    UniqueChars source = QuoteString(cx_, name, '"');
    return source && buf_.append('[') &&
           buf_.append(source.get(), strlen(source.get())) && buf_.append(']');
  }

  /* Append a number to buf_. */
  bool appendNumber(double n) {
    char number[30];
    int digits = SprintfLiteral(number, "%g", n);
    return buf_.append(number, digits);
  }

  // Append "[<n>]" to buf_, referencing a property named by a numeric literal.
  bool appendNumericPropertyReference(double n) {
    return buf_.append("[") && appendNumber(n) && buf_.append(']');
  }

  /*
   * Walk over the given ParseNode, attempting to convert it to a stringified
   * name that respresents where the function is being assigned to.
   *
   * |*foundName| is set to true if a name is found for the expression.
   */
  bool nameExpression(ParseNode* n, bool* foundName) {
    switch (n->getKind()) {
      case ParseNodeKind::DotExpr: {
        PropertyAccess* prop = &n->as<PropertyAccess>();
        if (!nameExpression(&prop->expression(), foundName)) {
          return false;
        }
        if (!*foundName) {
          return true;
        }
        return appendPropertyReference(prop->right()->as<NameNode>().atom());
      }

      case ParseNodeKind::Name:
      case ParseNodeKind::PrivateName:
        *foundName = true;
        return buf_.append(n->as<NameNode>().atom());

      case ParseNodeKind::ThisExpr:
        *foundName = true;
        return buf_.append("this");

      case ParseNodeKind::ElemExpr: {
        PropertyByValue* elem = &n->as<PropertyByValue>();
        if (!nameExpression(&elem->expression(), foundName)) {
          return false;
        }
        if (!*foundName) {
          return true;
        }
        if (!buf_.append('[') || !nameExpression(elem->right(), foundName)) {
          return false;
        }
        if (!*foundName) {
          return true;
        }
        return buf_.append(']');
      }

      case ParseNodeKind::NumberExpr:
        *foundName = true;
        return appendNumber(n->as<NumericLiteral>().value());

      default:
        // We're confused as to what to call this function.
        *foundName = false;
        return true;
    }
  }

  /*
   * When naming an anonymous function, the process works loosely by walking
   * up the AST and then translating that to a string. The stringification
   * happens from some far-up assignment and then going back down the parse
   * tree to the function definition point.
   *
   * This function will walk up the parse tree, gathering relevant nodes used
   * for naming, and return the assignment node if there is one. The provided
   * array and size will be filled in, and the returned node could be nullptr
   * if no assignment is found. The first element of the array will be the
   * innermost node relevant to naming, and the last element will be the
   * outermost node.
   */
  ParseNode* gatherNameable(ParseNode** nameable, size_t* size) {
    MOZ_ASSERT(nparents_ > 0);
    MOZ_ASSERT(parents_[nparents_ - 1]->is<FunctionNode>());

    *size = 0;

    for (int pos = nparents_ - 2; pos >= 0; pos--) {
      ParseNode* cur = parents_[pos];
      if (cur->is<AssignmentNode>()) {
        return cur;
      }

      switch (cur->getKind()) {
        case ParseNodeKind::PrivateName:
        case ParseNodeKind::Name:
          return cur;  // found the initialized declaration
        case ParseNodeKind::ThisExpr:
          return cur;  // setting a property of 'this'
        case ParseNodeKind::Function:
          return nullptr;  // won't find an assignment or declaration

        case ParseNodeKind::ReturnStmt:
          // Normally the relevant parent of a node is its direct parent, but
          // sometimes with code like:
          //
          //    var foo = (function() { return function() {}; })();
          //
          // the outer function is just a helper to create a scope for the
          // returned function. Hence the name of the returned function should
          // actually be 'foo'.  This loop sees if the current node is a
          // ParseNodeKind::Return, and if there is a direct function
          // call we skip to that.
          for (int tmp = pos - 1; tmp > 0; tmp--) {
            if (isDirectCall(tmp, cur)) {
              pos = tmp;
              break;
            }
            if (isCall(cur)) {
              // Don't skip too high in the tree.
              break;
            }
            cur = parents_[tmp];
          }
          break;

        case ParseNodeKind::PropertyDefinition:
        case ParseNodeKind::Shorthand:
          // Record the ParseNodeKind::PropertyDefinition/Shorthand but skip the
          // ParseNodeKind::Object so we're not flagged as a contributor.
          pos--;
          [[fallthrough]];

        default:
          // Save any other nodes we encounter on the way up.
          MOZ_ASSERT(*size < MaxParents);
          nameable[(*size)++] = cur;
          break;
      }
    }

    return nullptr;
  }

  /*
   * Resolve the name of a function. If the function already has a name
   * listed, then it is skipped. Otherwise an intelligent name is guessed to
   * assign to the function's displayAtom field.
   */
  MOZ_MUST_USE bool resolveFun(FunctionNode* funNode,
                               const ParserAtom** retId) {
    MOZ_ASSERT(funNode != nullptr);

    FunctionBox* funbox = funNode->funbox();

    MOZ_ASSERT(buf_.empty());
    auto resetBuf = mozilla::MakeScopeExit([&] { buf_.clear(); });

    *retId = nullptr;

    // If the function already has a name, use that.
    if (funbox->displayAtom()) {
      if (!prefix_) {
        *retId = funbox->displayAtom();
        return true;
      }
      if (!buf_.append(prefix_) || !buf_.append('/') ||
          !buf_.append(funbox->displayAtom())) {
        return false;
      }
      *retId = buf_.finishParserAtom(compilationInfo_);
      return !!*retId;
    }

    // If a prefix is specified, then it is a form of namespace.
    if (prefix_) {
      if (!buf_.append(prefix_) || !buf_.append('/')) {
        return false;
      }
    }

    // Gather all nodes relevant to naming.
    ParseNode* toName[MaxParents];
    size_t size;
    ParseNode* assignment = gatherNameable(toName, &size);

    // If the function is assigned to something, then that is very relevant.
    if (assignment) {
      if (assignment->is<AssignmentNode>()) {
        assignment = assignment->as<AssignmentNode>().left();
      }
      bool foundName = false;
      if (!nameExpression(assignment, &foundName)) {
        return false;
      }
      if (!foundName) {
        return true;
      }
    }

    // Other than the actual assignment, other relevant nodes to naming are
    // those in object initializers and then particular nodes marking a
    // contribution.
    for (int pos = size - 1; pos >= 0; pos--) {
      ParseNode* node = toName[pos];

      if (node->isKind(ParseNodeKind::PropertyDefinition) ||
          node->isKind(ParseNodeKind::Shorthand)) {
        ParseNode* left = node->as<BinaryNode>().left();
        if (left->isKind(ParseNodeKind::ObjectPropertyName) ||
            left->isKind(ParseNodeKind::StringExpr)) {
          if (!appendPropertyReference(left->as<NameNode>().atom())) {
            return false;
          }
        } else if (left->isKind(ParseNodeKind::NumberExpr)) {
          if (!appendNumericPropertyReference(
                  left->as<NumericLiteral>().value())) {
            return false;
          }
        } else {
          MOZ_ASSERT(left->isKind(ParseNodeKind::ComputedName) ||
                     left->isKind(ParseNodeKind::BigIntExpr));
        }
      } else {
        // Don't have consecutive '<' characters, and also don't start
        // with a '<' character.
        if (!buf_.empty() && buf_.getChar(buf_.length() - 1) != '<' &&
            !buf_.append('<')) {
          return false;
        }
      }
    }

    // functions which are "genuinely anonymous" but are contained in some
    // other namespace are rather considered as "contributing" to the outer
    // function, so give them a contribution symbol here.
    if (!buf_.empty() && buf_.getChar(buf_.length() - 1) == '/' &&
        !buf_.append('<')) {
      return false;
    }

    if (buf_.empty()) {
      return true;
    }

    *retId = buf_.finishParserAtom(compilationInfo_);
    if (!*retId) {
      return false;
    }

    // Skip assigning the guessed name if the function has a (dynamically)
    // computed inferred name.
    if (!funNode->isDirectRHSAnonFunction()) {
      funbox->setGuessedAtom(*retId);
    }
    return true;
  }

  /*
   * Tests whether parents_[pos] is a function call whose callee is cur.
   * This is the case for functions which do things like simply create a scope
   * for new variables and then return an anonymous function using this scope.
   */
  bool isDirectCall(int pos, ParseNode* cur) {
    return pos >= 0 && isCall(parents_[pos]) &&
           parents_[pos]->as<BinaryNode>().left() == cur;
  }

 public:
  MOZ_MUST_USE bool visitFunction(FunctionNode* pn) {
    const ParserAtom* savedPrefix = prefix_;
    const ParserAtom* newPrefix = nullptr;
    if (!resolveFun(pn, &newPrefix)) {
      return false;
    }

    // If a function looks like (function(){})() where the parent node
    // of the definition of the function is a call, then it shouldn't
    // contribute anything to the namespace, so don't bother updating
    // the prefix to whatever was returned.
    if (!isDirectCall(nparents_ - 2, pn)) {
      prefix_ = newPrefix;
    }

    bool ok = Base::visitFunction(pn);

    prefix_ = savedPrefix;
    return ok;
  }

  // Skip this type of node. It never contains functions.
  MOZ_MUST_USE bool visitCallSiteObj(CallSiteNode* callSite) {
    // This node only contains internal strings or undefined and an array -- no
    // user-controlled expressions.
    return true;
  }

  // Skip walking the list of string parts, which never contains functions.
  MOZ_MUST_USE bool visitTaggedTemplateExpr(BinaryNode* taggedTemplate) {
    ParseNode* tag = taggedTemplate->left();

    // The leading expression, e.g. |tag| in |tag`foo`|,
    // that might contain functions.
    if (!visit(tag)) {
      return false;
    }

    // The callsite object node is first.  This node only contains
    // internal strings or undefined and an array -- no user-controlled
    // expressions.
    CallSiteNode* element =
        &taggedTemplate->right()->as<ListNode>().head()->as<CallSiteNode>();
#ifdef DEBUG
    {
      ListNode* rawNodes = &element->head()->as<ListNode>();
      MOZ_ASSERT(rawNodes->isKind(ParseNodeKind::ArrayExpr));
      for (ParseNode* raw : rawNodes->contents()) {
        MOZ_ASSERT(raw->isKind(ParseNodeKind::TemplateStringExpr));
      }
      for (ParseNode* cooked : element->contentsFrom(rawNodes->pn_next)) {
        MOZ_ASSERT(cooked->isKind(ParseNodeKind::TemplateStringExpr) ||
                   cooked->isKind(ParseNodeKind::RawUndefinedExpr));
      }
    }
#endif

    // Next come any interpolated expressions in the tagged template.
    ParseNode* interpolated = element->pn_next;
    for (; interpolated; interpolated = interpolated->pn_next) {
      if (!visit(interpolated)) {
        return false;
      }
    }

    return true;
  }

 private:
  // Speed hack: this type of node can't contain functions, so skip walking it.
  MOZ_MUST_USE bool internalVisitSpecList(ListNode* pn) {
    // Import/export spec lists contain import/export specs containing
    // only pairs of names. Alternatively, an export spec list may
    // contain a single export batch specifier.
#ifdef DEBUG
    bool isImport = pn->isKind(ParseNodeKind::ImportSpecList);
    ParseNode* item = pn->head();
    if (!isImport && item && item->isKind(ParseNodeKind::ExportBatchSpecStmt)) {
      MOZ_ASSERT(item->is<NullaryNode>());
    } else {
      for (ParseNode* item : pn->contents()) {
        BinaryNode* spec = &item->as<BinaryNode>();
        MOZ_ASSERT(spec->isKind(isImport ? ParseNodeKind::ImportSpec
                                         : ParseNodeKind::ExportSpec));
        MOZ_ASSERT(spec->left()->isKind(ParseNodeKind::Name));
        MOZ_ASSERT(spec->right()->isKind(ParseNodeKind::Name));
      }
    }
#endif
    return true;
  }

 public:
  MOZ_MUST_USE bool visitImportSpecList(ListNode* pn) {
    return internalVisitSpecList(pn);
  }

  MOZ_MUST_USE bool visitExportSpecList(ListNode* pn) {
    return internalVisitSpecList(pn);
  }

  explicit NameResolver(JSContext* cx, CompilationInfo& compilationInfo)
      : ParseNodeVisitor(cx),
        compilationInfo_(compilationInfo),
        prefix_(nullptr),
        nparents_(0),
        buf_(cx) {}

  /*
   * Resolve names for all anonymous functions in the given ParseNode tree.
   */
  MOZ_MUST_USE bool visit(ParseNode* pn) {
    // Push pn to the parse node stack.
    if (nparents_ >= MaxParents) {
      // Silently skip very deeply nested functions.
      return true;
    }
    auto initialParents = nparents_;
    parents_[initialParents] = pn;
    nparents_++;

    bool ok = Base::visit(pn);

    // Pop pn from the parse node stack.
    nparents_--;
    MOZ_ASSERT(initialParents == nparents_, "nparents imbalance detected");
    MOZ_ASSERT(parents_[initialParents] == pn,
               "pushed child shouldn't change underneath us");
    AlwaysPoison(&parents_[initialParents], JS_OOB_PARSE_NODE_PATTERN,
                 sizeof(parents_[initialParents]), MemCheckKind::MakeUndefined);

    return ok;
  }
};

} /* anonymous namespace */

bool frontend::NameFunctions(JSContext* cx, CompilationInfo& compilationInfo,
                             ParseNode* pn) {
  AutoTraceLog traceLog(TraceLoggerForCurrentThread(cx),
                        TraceLogger_BytecodeNameFunctions);
  NameResolver nr(cx, compilationInfo);
  return nr.visit(pn);
}
