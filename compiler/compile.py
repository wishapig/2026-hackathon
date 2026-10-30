#!/usr/bin/env python3
"""Compile a deliberately restricted structured C++ kernel driver to Metadata.

No compiler dependency is needed. Unsupported syntax is rejected, never skipped.
See compiler/README.md for the supported subset and trusted contract boundary.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys
import zlib
from dataclasses import dataclass

MAGIC = b'DAGMETA1'
COMPRESSED_MAGIC = b'DAGMETA2'
COMPACT_MAGIC = b'DAGMETA3'
COMPACT_COMPRESSED_MAGIC = b'DAGMETA4'
BINARY_MAGIC = b'DAGMETA5'
BINARY_COMPRESSED_MAGIC = b'DAGMETA6'
INT_MIN, INT_MAX = -(1 << 63), (1 << 63) - 1
SOURCE_INT_MIN, SOURCE_INT_MAX = -(1 << 31), (1 << 31) - 1
TRUSTED_INCLUDES = {'"kernels.h"', '<algorithm>', '<cmath>', '<cstddef>', '<stdexcept>', '<vector>'}
OPS = {'+': 2, '-': 2, '*': 2, '/': 2, '%': 2, '<': 2, '<=': 2,
       '>': 2, '>=': 2, '==': 2, '!=': 2, '&&': 2, '||': 2,
       'neg': 1, '!': 1}
PRECEDENCE = {'||': 1, '&&': 2, '==': 3, '!=': 3, '<': 4, '<=': 4,
              '>': 4, '>=': 4, '+': 5, '-': 5, '*': 6, '/': 6, '%': 6}


class CompileError(ValueError):
    pass


@dataclass(frozen=True)
class Token:
    text: str
    line: int


def tokenize(source: str) -> list[Token]:
    # C++ performs physical-line splicing before comments/preprocessing. This
    # frontend does not implement that phase, so reject it rather than parse a
    # different program (especially a continued // comment).
    if re.search(r'\\[ \t]*(?:\r\n|\n|\r)', source):
        raise CompileError('C++ backslash-newline line splicing is unsupported')
    tokens = []
    i, line = 0, 1
    pattern = re.compile(r'(?:[A-Za-z_][A-Za-z_0-9]*|(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?|"(?:\\.|[^"\\\n])*"|::|\+\+|\+=|-=|\*=|/=|<=|>=|==|!=|&&|\|\||[{}()\[\];,+*/%<>=!&-])')
    while i < len(source):
        if source[i].isspace():
            line += source[i] == '\n'
            i += 1
            continue
        if source.startswith('//', i):
            end = source.find('\n', i)
            i = len(source) if end == -1 else end
            continue
        if source.startswith('/*', i):
            end = source.find('*/', i + 2)
            if end == -1:
                raise CompileError(f'line {line}: unterminated comment')
            line += source[i:end + 2].count('\n')
            i = end + 2
            continue
        if source[i] == '#':
            end = source.find('\n', i)
            end = len(source) if end == -1 else end
            directive = source[i:end]
            if source[source.rfind('\n', 0, i) + 1:i].strip() or tokens:
                raise CompileError(f'line {line}: includes must precede code and start a logical line')
            match = re.fullmatch(r'#\s*include\s*("[^"\n]+"|<[^>\n]+>)\s*', directive)
            if not match:
                raise CompileError(f'line {line}: only trusted #include directives are supported; macros/conditionals are rejected')
            if match.group(1) not in TRUSTED_INCLUDES:
                raise CompileError(f'line {line}: untrusted/unsupported include {match.group(1)!r}; header macros cannot be ignored')
            i = end
            continue
        m = pattern.match(source, i)
        if not m:
            raise CompileError(f'line {line}: unsupported token starting at {source[i:i+20]!r}')
        tokens.append(Token(m.group(), line))
        i = m.end()
    tokens.append(Token('<eof>', line))
    return tokens


def fnv1a(data: bytes) -> int:
    h = 14695981039346656037
    for b in data:
        h = ((h ^ b) * 1099511628211) & ((1 << 64) - 1)
    return h


def expr_variables(expr):
    if isinstance(expr, str):
        return {expr}
    if isinstance(expr, list):
        result = set()
        for child in expr[1:]:
            result |= expr_variables(child)
        return result
    return set()


def validate_expr(expr, allowed: set[str], depth=0):
    if depth > 64:
        raise CompileError('expression exceeds maximum nesting depth')
    if type(expr) is int:
        if not INT_MIN <= expr <= INT_MAX:
            raise CompileError('integer literal outside signed 64-bit range')
    elif isinstance(expr, str):
        if expr not in allowed:
            raise CompileError(f'unknown expression variable {expr!r}')
    elif isinstance(expr, list) and expr and isinstance(expr[0], str):
        op = expr[0]
        if op not in OPS or len(expr) != OPS[op] + 1:
            raise CompileError(f'invalid expression operator/arity: {expr!r}')
        for arg in expr[1:]:
            validate_expr(arg, allowed, depth + 1)
    else:
        raise CompileError(f'invalid expression: {expr!r}')


def substitute(expr, args):
    if isinstance(expr, str) and re.fullmatch(r'arg[0-9]+', expr):
        return args[int(expr[3:])]
    if isinstance(expr, list):
        return [expr[0], *(substitute(arg, args) for arg in expr[1:])]
    return expr


def load_catalog(data: bytes):
    def unique(items):
        result = {}
        for k, v in items:
            if k in result:
                raise CompileError(f'duplicate catalog key {k!r}')
            result[k] = v
        return result
    try:
        catalog = json.loads(data, object_pairs_hook=unique)
    except (json.JSONDecodeError, UnicodeDecodeError) as ex:
        raise CompileError(f'invalid catalog JSON: {ex}') from ex
    if not isinstance(catalog, dict) or catalog.get('schema_version') != 1:
        raise CompileError('catalog requires schema_version=1')
    operators = catalog.get('operators')
    if not isinstance(operators, dict):
        raise CompileError('catalog operators must be an object keyed by symbol')
    for symbol, contract in operators.items():
        if (not re.fullmatch('[A-Za-z_][A-Za-z_0-9]*', symbol) or
                symbol.startswith('__') or symbol == 'scalar_expression'):
            raise CompileError(f'invalid/reserved operator symbol {symbol!r}')
        if not isinstance(contract, dict):
            raise CompileError(f'{symbol}: contract must be an object')
        arguments = contract.get('arguments')
        if not isinstance(arguments, list) or not all(x in ('pointer:L', 'int') for x in arguments):
            raise CompileError(f'{symbol}: arguments must list int or pointer:L types')
        if contract.get('completion') != 'synchronous' or contract.get('reentrant') is not True:
            raise CompileError(f'{symbol}: synchronous, reentrant contract required for parallel lowering')
        if contract.get('internal_threads') != 1:
            raise CompileError(f'{symbol}: this backend requires declared internal_threads=1')
        allowed = {'n', 'b'} | {f'arg{i}' for i in range(len(arguments))}
        for mode in ('reads', 'writes'):
            regions = contract.get(mode)
            if not isinstance(regions, list):
                raise CompileError(f'{symbol}: missing {mode} region list')
            for region in regions:
                if not isinstance(region, dict) or set(region) != {'buffer', 'offset', 'rows', 'cols', 'stride'}:
                    raise CompileError(f'{symbol}: malformed region')
                if region['buffer'] != 'L':
                    raise CompileError(f'{symbol}: call regions must refer to L in this adapter')
                for field in ('offset', 'rows', 'cols', 'stride'):
                    validate_expr(region[field], allowed)
    return catalog


class Parser:
    def __init__(self, source, catalog, entry):
        self.tokens = tokenize(source)
        self.pos = 0
        self.catalog = catalog['operators']
        self.entry = entry
        self.scopes = [set(['n'])]
        self.used = set()
        self.requirements = []
        self.work_seen = False
        self.return_seen = False
        self.source = source
        self.depth = 0

    @property
    def token(self):
        return self.tokens[self.pos].text

    def error(self, msg):
        raise CompileError(f'line {self.tokens[self.pos].line}: {msg} (at {self.token!r})')

    def accept(self, token):
        if self.token == token:
            self.pos += 1
            return True
        return False

    def expect(self, token):
        if not self.accept(token):
            self.error(f'expected {token!r}')

    def ident(self):
        if not re.fullmatch('[A-Za-z_][A-Za-z_0-9]*', self.token):
            self.error('expected identifier')
        value = self.token
        self.pos += 1
        return value

    def variables(self):
        return set().union(*self.scopes)

    def define(self, name):
        if name in self.variables() or name in {'A', 'L'}:
            self.error(f'shadowing/redefinition of {name!r} is unsupported')
        self.scopes[-1].add(name)

    def expr(self, minimum=0):
        if self.accept('-'):
            left = ['neg', self.expr(7)]
        elif self.accept('!'):
            left = ['!', self.expr(7)]
        elif self.accept('+'):
            left = self.expr(7)
        elif self.accept('('):
            left = self.expr()
            self.expect(')')
        elif re.fullmatch('[0-9]+', self.token):
            if len(self.token) > 1 and self.token.startswith('0'):
                self.error('leading-zero/octal source integer literals are unsupported')
            left = int(self.token)
            self.pos += 1
            if left > SOURCE_INT_MAX:
                self.error('source integer literal outside supported signed 32-bit int range')
        else:
            left = self.ident()
            if left not in self.variables():
                self.error(f'unknown or noninteger variable {left!r}')
        while self.token in PRECEDENCE and PRECEDENCE[self.token] >= minimum:
            op = self.token
            prec = PRECEDENCE[op]
            self.pos += 1
            right = self.expr(prec + 1)
            left = [op, left, right]
        self.check_constant_int(left)
        return left

    def check_constant_int(self, expression):
        """Reject known source-int overflow; dynamic checks belong to binding."""
        if isinstance(expression, int):
            return expression
        if isinstance(expression, str):
            return None
        op = expression[0]
        a = self.check_constant_int(expression[1])
        if a is None:
            return None
        if op == 'neg':
            result = -a
        elif op == '!':
            result = int(not a)
        elif op == '&&' and not a:
            return 0
        elif op == '||' and a:
            return 1
        else:
            b = self.check_constant_int(expression[2])
            if b is None:
                return None
            if op in ('/', '%') and b == 0:
                self.error('constant division by zero in source expression')
            if op in ('/', '%') and a == SOURCE_INT_MIN and b == -1:
                self.error('constant signed int division overflow')
            if op == '+': result = a + b
            elif op == '-': result = a - b
            elif op == '*': result = a * b
            elif op in ('/', '%'):
                quotient = abs(a) // abs(b) * (-1 if (a < 0) != (b < 0) else 1)
                result = quotient if op == '/' else a - quotient * b
            elif op == '<': result = int(a < b)
            elif op == '<=': result = int(a <= b)
            elif op == '>': result = int(a > b)
            elif op == '>=': result = int(a >= b)
            elif op == '==': result = int(a == b)
            elif op == '!=': result = int(a != b)
            elif op == '&&': result = int(bool(a) and bool(b))
            elif op == '||': result = int(bool(a) or bool(b))
            else: self.error('invalid constant operator')
        if not SOURCE_INT_MIN <= result <= SOURCE_INT_MAX:
            self.error('constant source expression outside signed 32-bit int range')
        return result

    def floating_expr(self, minimum=0):
        """Parse scalar arithmetic without reassociation or implicit algorithms."""
        if self.accept('-'):
            value = self.floating_expr(7)
            if value[0] == 'integer':
                left = ('integer', self.check_constant_int(['neg', value[1]]))
            else:
                left = ('neg', value)
        elif self.accept('+'):
            left = self.floating_expr(7)
        elif self.accept('('):
            left = self.floating_expr()
            self.expect(')')
        elif self.token in ('A', 'L'):
            buffer = self.token
            self.pos += 1
            self.expect('[')
            offset = self.expr()
            self.expect(']')
            left = ('load', buffer, offset)
        elif self.token in ('std', 'sqrt'):
            if self.accept('std'):
                self.expect('::')
            self.expect('sqrt'); self.expect('(')
            left = ('sqrt', self.floating_expr())
            self.expect(')')
        elif re.fullmatch(r'(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?', self.token):
            literal = self.token
            # Unsuffixed integer operands retain the frontend's int policy.
            if re.fullmatch('[0-9]+', literal):
                if len(literal) > 1 and literal.startswith('0'):
                    self.error('leading-zero/octal source integer literals are unsupported')
                if int(literal) > SOURCE_INT_MAX:
                    self.error('source integer literal outside supported signed 32-bit int range')
                left = ('integer', int(literal))
            else:
                left = ('literal', float(literal))
            value = left[1]
            if not math.isfinite(value):
                self.error('non-finite scalar literal')
            self.pos += 1
        else:
            self.error('scalar expressions support matrix elements, literals, arithmetic and sqrt; local floating variables are unsupported')
        precedence = {'+': 5, '-': 5, '*': 6, '/': 6}
        while self.token in precedence and precedence[self.token] >= minimum:
            op = self.token
            self.pos += 1
            right = self.floating_expr(precedence[op] + 1)
            if left[0] == right[0] == 'integer':
                left = ('integer', self.check_constant_int([op, left[1], right[1]]))
            else:
                left = (op, left, right)
        return left

    def lower_scalar(self, nodes):
        """Use a generic scalar expression call with exact element contracts.

        Postfix ABI: destination, then loadA=0/index, loadL=1/index,
        literal=2/four little-endian uint16 words, add=3, sub=4, mul=5,
        div=6, sqrt=7, negate=8. Words avoid source-int32 narrowing of bits.
        """
        result = []
        for node in nodes:
            for field in ('body', 'then', 'else'):
                if field in node:
                    node[field] = self.lower_scalar(node[field])
            if node['kind'] != '_store':
                result.append(node)
                continue
            reads, bytecode = [], []
            def emit(expression):
                op = expression[0]
                if op == 'load':
                    _, buffer, offset = expression
                    bytecode.extend([0 if buffer == 'A' else 1, offset])
                    region = {'buffer': buffer, 'offset': offset,
                              'rows': 1, 'cols': 1, 'stride': 'n'}
                    if region not in reads:
                        reads.append(region)
                elif op in ('literal', 'integer'):
                    words = struct.unpack('<4H', struct.pack('<d', expression[1]))
                    bytecode.extend([2, *words])
                else:
                    for child in expression[1:]:
                        emit(child)
                    bytecode.append({'+': 3, '-': 4, '*': 5, '/': 6,
                                     'sqrt': 7, 'neg': 8}[op])
            emit(node['value'])
            self.used.add('scalar_expression')
            result.append({'kind': 'call', 'op': 'scalar_expression',
                           'args': [node['offset'], *bytecode], 'reads': reads,
                           'writes': [{'buffer': 'L', 'offset': node['offset'],
                                       'rows': 1, 'cols': 1, 'stride': 'n'}]})
        return result

    def block(self):
        self.depth += 1
        if self.depth > 64:
            self.error('statement nesting exceeds limit')
        self.expect('{')
        self.scopes.append(set())
        nodes = []
        while not self.accept('}'):
            if self.token == '<eof>':
                self.error('unterminated block')
            nodes.extend(self.statement())
        self.scopes.pop()
        self.depth -= 1
        return nodes

    def parse(self):
        namespaces = 0
        while self.accept('namespace'):
            self.ident()
            self.expect('{')
            namespaces += 1
        self.expect('int')
        name = self.ident()
        if name != self.entry:
            self.error(f'entry function must be {self.entry!r}, got {name!r}')
        expected = ['(', 'const', 'double', '*', 'A', ',', 'double', '*', 'L', ',', 'int', 'n']
        for token in expected:
            self.expect(token)
        if self.accept(','):
            self.expect('int'); self.expect('b')
            self.scopes[0].add('b')
        self.expect(')')
        nodes = self.block()
        if not self.return_seen:
            self.error('entry must end with return 0')
        for _ in range(namespaces):
            self.expect('}')
        self.expect('<eof>')
        # Collapse the recognized zeroing idiom only after its syntax/semantics
        # have been parsed completely. Names may differ; arithmetic may not.
        nodes = self.lower_zero(nodes)
        return self.lower_scalar(nodes)

    def lower_zero(self, nodes):
        output = []
        for node in nodes:
            matched = False
            if node['kind'] == 'for' and node['start'] == 0 and node['stop'] == 'n' and node['step'] == 1 and len(node['body']) == 1:
                r = node['var']; inner = node['body'][0]
                if inner['kind'] == 'for' and inner['start'] == ['+', r, 1] and inner['stop'] == 'n' and inner['step'] == 1 and len(inner['body']) == 1:
                    c = inner['var']; store = inner['body'][0]
                    if (store.get('kind') == '_store' and
                            store.get('offset') == ['+', ['*', r, 'n'], c] and
                            store.get('value') in (('literal', 0.0), ('integer', 0))):
                        output.append({'kind': 'zero_upper'})
                        matched = True
            if matched:
                continue
            for field in ('body', 'then', 'else'):
                if field in node:
                    node[field] = self.lower_zero(node[field])
            output.append(node)
        return output

    def statement(self):
        if self.return_seen:
            self.error('statements after return are unsupported')
        if self.token == 'for':
            self.work_seen = True
            return [self.for_loop()]
        if self.accept('if'):
            self.expect('('); condition = self.expr(); self.expect(')')
            # Parameter guards can be removed only when the shared binder
            # already guarantees their negation, not arbitrary throw paths.
            saved = self.pos
            self.accept('{')
            is_throw = self.token == 'throw'
            self.pos = saved
            if is_throw:
                if self.depth != 1 or self.work_seen:
                    self.error('parameter guards must precede work at entry level')
                known = [['<=', 'b', 0], ['<=', 'n', 0], ['>', 'b', 'n'], ['!=', ['%', 'n', 'b'], 0]]
                if condition not in known:
                    self.error('throw guard is not guaranteed by runtime parameter validation')
                braced = self.accept('{')
                self.expect('throw'); self.expect('std'); self.expect('::')
                if self.ident() not in ('invalid_argument', 'runtime_error'):
                    self.error('unsupported exception type')
                self.expect('(')
                if not self.token.startswith('"'):
                    self.error('exception message must be a literal')
                self.pos += 1; self.expect(')'); self.expect(';')
                if braced: self.expect('}')
                if self.token == 'else': self.error('guard else branch is unsupported')
                self.requirements.append(['!', condition])
                return []
            self.work_seen = True
            yes = self.block()
            no = self.block() if self.accept('else') else []
            return [{'kind': 'if', 'condition': condition, 'then': yes, 'else': no}]
        if self.token in ('const', 'int'):
            self.accept('const'); self.expect('int'); name = self.ident(); self.expect('=')
            value = self.expr(); self.expect(';'); self.define(name)
            return [{'kind': 'let', 'name': name, 'value': value}]
        if self.accept('return'):
            if self.depth != 1:
                self.error('conditional/loop return is unsupported')
            self.expect('0'); self.expect(';')
            if self.token != '}': self.error('return must be the last entry statement')
            self.return_seen = True
            return []
        if self.accept('std'):
            self.expect('::'); self.expect('copy'); self.expect('(')
            self.expect('A'); self.expect(','); self.expect('A'); self.expect('+')
            # Exactly the full matrix copy from the real baseline. The cast is
            # checked syntactically and does not authorize dropping other code.
            if self.accept('static_cast'):
                for t in ['<', 'std', '::', 'size_t', '>', '(', 'n', ')', '*', 'n']:
                    self.expect(t)
            else:
                value = self.expr()
                if value != ['*', 'n', 'n']:
                    self.error('only a full n*n A-to-L copy is supported')
            self.expect(','); self.expect('L'); self.expect(')'); self.expect(';')
            self.work_seen = True
            return [{'kind': 'copy'}]
        if self.accept('L'):
            self.expect('['); offset = self.expr(); self.expect(']')
            operation = self.token
            if operation not in ('=', '+=', '-=', '*=', '/='):
                self.error('expected scalar assignment')
            self.pos += 1
            value = self.floating_expr()
            if operation != '=':
                value = (operation[0], ('load', 'L', offset), value)
            self.expect(';'); self.work_seen = True
            return [{'kind': '_store', 'offset': offset, 'value': value}]
        symbol = self.ident()
        if symbol not in self.catalog:
            self.error(f'call to {symbol!r} lacks an operator contract (or statement is unsupported)')
        contract = self.catalog[symbol]
        self.expect('(')
        args = []
        for idx, argtype in enumerate(contract['arguments']):
            if idx: self.expect(',')
            if argtype == 'pointer:L':
                self.expect('&'); self.expect('L'); self.expect('[')
                args.append(self.expr()); self.expect(']')
            else:
                args.append(self.expr())
        self.expect(')'); self.expect(';')
        self.used.add(symbol); self.work_seen = True
        node = {'kind': 'call', 'op': symbol, 'args': args}
        for mode in ('reads', 'writes'):
            node[mode] = [{field: value if field == 'buffer' else substitute(value, args)
                           for field, value in region.items()} for region in contract[mode]]
        return [node]

    def for_loop(self):
        self.expect('for'); self.expect('('); self.expect('int')
        name = self.ident(); self.expect('='); start = self.expr(); self.expect(';')
        self.scopes.append(set()); self.define(name)
        self.expect(name)
        inclusive = self.accept('<=')
        if not inclusive: self.expect('<')
        stop = self.expr()
        if name in expr_variables(stop): self.error('loop bound cannot depend on its own induction variable')
        if inclusive: stop = ['+', stop, 1]
        self.expect(';')
        if self.accept('++'):
            self.expect(name); step = 1
        else:
            self.expect(name)
            if self.accept('++'): step = 1
            else:
                self.expect('+='); step = self.expr()
        if name in expr_variables(step): self.error('loop step cannot depend on its induction variable')
        if isinstance(step, int) and step <= 0:
            self.error('loop step must be positive')
        self.expect(')'); body = self.block(); self.scopes.pop()
        return {'kind': 'for', 'var': name, 'start': start, 'stop': stop, 'step': step, 'body': body}


def compile_source(source: str, catalog_bytes: bytes, entry='block_cholesky', include_provenance=False):
    if len(source.encode()) > 4 * 1024 * 1024 or len(catalog_bytes) > 4 * 1024 * 1024:
        raise CompileError('source/catalog exceeds 4 MiB limit')
    catalog = load_catalog(catalog_bytes)
    parser = Parser(source, catalog, entry)
    program = parser.parse()
    document = {
        'schema_version': 1,
        'operators': sorted(parser.used),
        'program': program,
        'parameter_guards': parser.requirements,
        'frontend': 'strict-structured-cpp-v1',
    }
    if include_provenance:
        document['source_digest'] = hashlib.sha256(source.encode()).hexdigest()
        document['contract_digest'] = hashlib.sha256(catalog_bytes).hexdigest()
    return document


def binary_value(value):
    """Encode only schema values: signed int64, ASCII names and arrays."""
    def header(kind, argument):
        if argument < 24:
            return bytes([(kind << 5) | argument])
        width = 1 if argument < 256 else 2 if argument < 65536 else 4 if argument < (1 << 32) else 8
        return bytes([(kind << 5) | {1: 24, 2: 25, 4: 26, 8: 27}[width]]) + argument.to_bytes(width, 'big')

    if type(value) is int:
        if not INT_MIN <= value <= INT_MAX:
            raise CompileError('binary metadata integer outside signed 64-bit range')
        return header(0, value) if value >= 0 else header(1, -1 - value)
    if isinstance(value, str):
        try:
            data = value.encode('ascii')
        except UnicodeEncodeError as error:
            raise CompileError('binary metadata names must be ASCII') from error
        if len(data) > 128:
            raise CompileError('binary metadata name exceeds identifier limit')
        return header(3, len(data)) + data
    if isinstance(value, list):
        return header(4, len(value)) + b''.join(binary_value(item) for item in value)
    raise CompileError('binary metadata only supports integers, strings and arrays')


def compact_payload(document, binary=False):
    """Losslessly encode schema field positions; expressions remain unchanged."""
    allowed = {'schema_version', 'source_digest', 'contract_digest', 'operators',
               'program', 'parameter_guards', 'frontend'}
    if set(document) - allowed or document.get('schema_version') != 1:
        raise CompileError('compact metadata requires schema_version=1')
    frontend = document.get('frontend')
    if frontend not in (None, 'strict-structured-cpp-v1'):
        raise CompileError('unsupported compact frontend semantics')

    def regions(items):
        fields = ('buffer', 'offset', 'rows', 'cols', 'stride')
        result = []
        for item in items:
            if set(item) != set(fields):
                raise CompileError('unexpected compact region fields')
            result.append([item[field] for field in fields])
        return result

    def nodes(items):
        result = []
        for item in items:
            kind = item['kind']
            if kind == 'let':
                fields = {'kind', 'name', 'value'}
                packed = [0, item['name'], item['value']]
            elif kind == 'for':
                fields = {'kind', 'var', 'start', 'stop', 'step', 'body'}
                packed = [1, item['var'], item['start'], item['stop'], item['step'],
                          nodes(item['body'])]
            elif kind == 'if':
                fields = {'kind', 'condition', 'then', 'else'}
                packed = [2, item['condition'], nodes(item['then']), nodes(item.get('else', []))]
            elif kind == 'call':
                fields = {'kind', 'op', 'args', 'reads', 'writes'}
                packed = [3, item['op'], item['args'], regions(item['reads']), regions(item['writes'])]
            elif kind in ('copy', 'zero_upper'):
                fields = {'kind'}
                packed = [4 if kind == 'copy' else 5]
            else:
                raise CompileError(f'unsupported compact node: {kind}')
            if set(item) - fields:
                raise CompileError('unexpected compact node fields')
            result.append(packed)
        return result

    digests = bytearray()
    for key in ('source_digest', 'contract_digest'):
        value = document.get(key)
        if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
            raise CompileError('compact metadata requires lowercase SHA-256 input digests')
        digests.extend(bytes.fromhex(value))
    packed = [document['operators'], document.get('parameter_guards', []),
              int(frontend is not None), nodes(document['program'])]
    body = binary_value(packed) if binary else json.dumps(
        packed, ensure_ascii=True, separators=(',', ':')).encode()
    return bytes(digests) + body


# Execution-only format: numeric lexical references, no hashes or audit fields.
LEAN_OPS = ['neg', '!', '+', '-', '*', '/', '%', '<', '<=', '>', '>=', '==', '!=', '&&', '||']


def lean_payload(document):
    operators = list(document['operators'])
    buffers = []
    next_slot = 2

    def expression(e, scope):
        if type(e) is int:
            return e
        if isinstance(e, str):
            return [0, scope[e]]
        return [LEAN_OPS.index(e[0]) + 1, *(expression(a, scope) for a in e[1:])]

    def regions(items, scope):
        result = []
        for r in items:
            if r['buffer'] not in buffers:
                buffers.append(r['buffer'])
            result.append([buffers.index(r['buffer']),
                           *(expression(r[k], scope) for k in ('offset', 'rows', 'cols', 'stride'))])
        return result

    def nodes(items, incoming):
        nonlocal next_slot
        scope = dict(incoming)
        result = []
        for node in items:
            kind = node['kind']
            if kind == 'let':
                rhs = expression(node['value'], scope)
                slot = next_slot; next_slot += 1
                scope[node['name']] = slot
                result.append([0, slot, rhs])
            elif kind == 'for':
                args = [expression(node[k], scope) for k in ('start', 'stop', 'step')]
                slot = next_slot; next_slot += 1
                inner = dict(scope); inner[node['var']] = slot
                result.append([1, slot, *args, nodes(node['body'], inner)])
            elif kind == 'if':
                result.append([2, expression(node['condition'], scope),
                               nodes(node['then'], scope), nodes(node.get('else', []), scope)])
            elif kind == 'call':
                result.append([3, operators.index(node['op']),
                               [expression(a, scope) for a in node['args']],
                               regions(node['reads'], scope), regions(node['writes'], scope)])
            elif kind in ('copy', 'zero_upper'):
                result.append([4 if kind == 'copy' else 5])
            else:
                raise CompileError('unsupported lean statement: ' + kind)
        return result

    scope = {'n': 0, 'b': 1}
    # Omit only guards implied by the runtime's mandatory parameter constraints.
    guaranteed = [['!', ['<=', 'b', 0]], ['!', ['<=', 'n', 0]],
                  ['!', ['>', 'b', 'n']], ['!', ['!=', ['%', 'n', 'b'], 0]]]
    guards = [expression(g, scope) for g in document.get('parameter_guards', [])
              if g not in guaranteed]
    program = nodes(document['program'], scope)
    frontend = document.get('frontend')
    if frontend not in (None, 'strict-structured-cpp-v1'):
        raise CompileError('unsupported lean integer semantics')
    return binary_value([int(frontend is not None), operators, buffers, guards, program])


def encode_lean(document, encoding='auto'):
    payload = lean_payload(document)
    size = len(payload)
    length = bytearray()
    while size >= 128:
        length.append((size & 127) | 128)
        size >>= 7
    length.append(size)
    raw = bytes([0xa7]) + length + payload
    compressor = zlib.compressobj(level=9, wbits=-15)
    compressed = bytes([0xa8]) + length + compressor.compress(payload) + compressor.flush()
    if encoding == 'lean':
        return raw
    if encoding == 'lean-deflate':
        return compressed
    return min((raw, compressed), key=len)


def encode_metadata(document, encoding='auto'):
    if encoding in ('auto', 'lean', 'lean-deflate'):
        return encode_lean(document, encoding)
    if encoding not in ('auto', 'raw', 'zlib', 'compact', 'compact-zlib', 'binary', 'binary-zlib'):
        raise CompileError(f'unknown metadata encoding: {encoding}')
    candidates = []

    def append(magic, payload, compressed):
        stored = zlib.compress(payload, level=9) if compressed else payload
        candidates.append(magic + struct.pack('<QQ', len(payload), fnv1a(payload)) + stored)

    if encoding in ('auto', 'raw', 'zlib'):
        payload = json.dumps(document, ensure_ascii=True, sort_keys=True, separators=(',', ':')).encode()
        if encoding != 'zlib':
            append(MAGIC, payload, False)
        if encoding != 'raw':
            append(COMPRESSED_MAGIC, payload, True)
    if encoding in ('auto', 'compact', 'compact-zlib'):
        payload = compact_payload(document)
        if encoding != 'compact-zlib':
            append(COMPACT_MAGIC, payload, False)
        if encoding != 'compact':
            append(COMPACT_COMPRESSED_MAGIC, payload, True)
    if encoding in ('auto', 'binary', 'binary-zlib'):
        payload = compact_payload(document, binary=True)
        if encoding != 'binary-zlib':
            append(BINARY_MAGIC, payload, False)
        if encoding != 'binary':
            append(BINARY_COMPRESSED_MAGIC, payload, True)
    return min(candidates, key=len)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--catalog', required=True, type=Path)
    parser.add_argument('-o', '--output', required=True, type=Path)
    parser.add_argument('--entry', default='block_cholesky')
    parser.add_argument('--json-debug', type=Path)
    parser.add_argument('--encoding', choices=('auto', 'raw', 'zlib', 'compact', 'compact-zlib',
                                               'binary', 'binary-zlib', 'lean', 'lean-deflate'),
                        default='auto', help='metadata container; auto chooses the smallest representation')
    opts = parser.parse_args(argv)
    try:
        document = compile_source(opts.source.read_text(), opts.catalog.read_bytes(), opts.entry,
                                  include_provenance=bool(opts.json_debug) or
                                  opts.encoding not in ('auto', 'lean', 'lean-deflate'))
        artifact = encode_metadata(document, opts.encoding)
        opts.output.parent.mkdir(parents=True, exist_ok=True)
        temp = opts.output.with_name(opts.output.name + '.tmp')
        temp.write_bytes(artifact)
        temp.replace(opts.output)
        if opts.json_debug:
            opts.json_debug.parent.mkdir(parents=True, exist_ok=True)
            opts.json_debug.write_text(json.dumps(document, indent=2, sort_keys=True) + '\n')
        print(f'compiled {opts.source}: {len(artifact)} bytes, operators={",".join(document["operators"])}', file=sys.stderr)
        return 0
    except (OSError, UnicodeError, CompileError, RecursionError) as ex:
        print(f'compiler: {ex}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
