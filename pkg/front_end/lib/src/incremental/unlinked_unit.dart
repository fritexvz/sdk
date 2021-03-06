// Copyright (c) 2017, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:typed_data';

import '../base/api_signature.dart';
import '../fasta/parser.dart' show Listener, Parser, optional;
import '../fasta/parser/top_level_parser.dart';
import '../fasta/scanner.dart';
import '../fasta/scanner/token_constants.dart' show STRING_TOKEN;
import '../fasta/source/directive_listener.dart';
import 'format.dart';

/// Compute the [UnlinkedUnitBuilder] for the [content].
UnlinkedUnitBuilder computeUnlinkedUnit(List<int> salt, List<int> content) {
  // Scan the content.
  ScannerResult scanResult = _scan(content);
  Token token = scanResult.tokens;

  // Parse directives.
  var listener = new DirectiveListener();
  new TopLevelParser(listener).parseUnit(token);

  // Parse to record function bodies.
  var parser = new _BodySkippingParser();
  parser.parseUnit(token);

  ApiSignature apiSignature = new ApiSignature();
  apiSignature.addBytes(salt);

  // Iterate over tokens and skip bodies.
  Iterator<_BodyRange> bodyIterator = parser.bodyRanges.iterator;
  bodyIterator.moveNext();
  for (; token.kind != EOF_TOKEN; token = token.next) {
    // Move to the body range that ends after the token.
    while (bodyIterator.current != null &&
        bodyIterator.current.last < token.charOffset) {
      bodyIterator.moveNext();
    }
    // If the current body range starts before or at the token, skip it.
    if (bodyIterator.current != null &&
        bodyIterator.current.first <= token.charOffset) {
      continue;
    }
    // The token is outside of a function body, add it.
    if (token is! ErrorToken) {
      apiSignature.addString(token.lexeme);
    }
  }

  return new UnlinkedUnitBuilder(
      apiSignature: apiSignature.toByteList(),
      imports: listener.imports.map(_toUnlinkedNamespaceDirective).toList(),
      exports: listener.exports.map(_toUnlinkedNamespaceDirective).toList(),
      parts: listener.parts.toList(),
      hasMixinApplication: parser.hasMixin);
}

/// Exclude all `native 'xyz';` token sequences.
void _excludeNativeClauses(Token token) {
  while (token.kind != EOF_TOKEN) {
    Token next = token.next;
    if (optional('native', next) &&
        next.next.kind == STRING_TOKEN &&
        optional(';', next.next.next)) {
      next = next.next.next;
      token.setNext(next);
    }
    token = next;
  }
}

/// Scan the content of the file.
ScannerResult _scan(List<int> content) {
  var zeroTerminatedBytes = new Uint8List(content.length + 1);
  zeroTerminatedBytes.setRange(0, content.length, content);
  ScannerResult result = scan(zeroTerminatedBytes);
  _excludeNativeClauses(result.tokens);
  return result;
}

/// Convert [NamespaceCombinator] into [UnlinkedCombinatorBuilder].
UnlinkedCombinatorBuilder _toUnlinkedCombinator(NamespaceCombinator c) =>
    new UnlinkedCombinatorBuilder(isShow: c.isShow, names: c.names.toList());

/// Convert [NamespaceDirective] into [UnlinkedNamespaceDirectiveBuilder].
UnlinkedNamespaceDirectiveBuilder _toUnlinkedNamespaceDirective(
        NamespaceDirective directive) =>
    new UnlinkedNamespaceDirectiveBuilder(
        uri: directive.uri,
        combinators: directive.combinators.map(_toUnlinkedCombinator).toList());

/// The char range of a function body.
class _BodyRange {
  /// The char offset of the first token in the range.
  final int first;

  /// The char offset of the last token in the range.
  final int last;

  _BodyRange(this.first, this.last);

  @override
  String toString() => '[$first, $last]';
}

/// The [Parser] that skips function bodies and remembers their token ranges.
class _BodySkippingParser extends Parser {
  bool hasMixin = false;
  final List<_BodyRange> bodyRanges = [];

  _BodySkippingParser() : super(new Listener());

  @override
  Token parseFunctionBody(
      Token token, bool ofFunctionExpression, bool allowAbstract) {
    Token next = token.next;
    if (identical('{', next.lexeme)) {
      Token close = skipBlock(token);
      bodyRanges.add(new _BodyRange(next.charOffset, close.charOffset));
      return close;
    }
    return super.parseFunctionBody(token, ofFunctionExpression, allowAbstract);
  }

  @override
  Token parseInvalidBlock(Token token) => skipBlock(token);

  Token parseMixinApplicationRest(Token token) {
    hasMixin = true;
    return super.parseMixinApplicationRest(token);
  }
}
