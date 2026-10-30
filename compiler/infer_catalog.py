#!/usr/bin/env python3
"""Infer parameterized operator accesses from LLVM/Clang's C++ AST, offline.

No operator names, argument positions, or block dimensions are built in.  The
supported kernel subset uses integer-bounded loops and affine matrix accesses;
wrappers and address helpers are resolved across the supplied translation units.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path


class AnalysisError(ValueError):
    pass


def children(node):
    return node.get('inner', [])


def unwrap(node):
    while node.get('kind') in ('ImplicitCastExpr', 'ParenExpr', 'CStyleCastExpr',
                               'CXXStaticCastExpr', 'ExprWithCleanups',
                               'MaterializeTemporaryExpr', 'ConstantExpr'):
        node = children(node)[0]
    return node


def expression(op, a, b):
    if a is None or b is None:
        return None
    if isinstance(a, int) and isinstance(b, int):
        if op == '+': return a + b
        if op == '-': return a - b
        if op == '*': return a * b
    if op == '-':
        if a == b: return 0
        if isinstance(b, int): return expression('+', a, -b)
    if op == '+':
        if a == 0: return b
        if b == 0: return a
        if isinstance(a, int) and not isinstance(b, int): a, b = b, a
        if isinstance(b, int) and isinstance(a, tuple) and a[0] == '+' and isinstance(a[2], int):
            return expression('+', a[1], a[2] + b)
    if op == '*':
        if a == 0 or b == 0: return 0
        if a == 1: return b
        if b == 1: return a
    return (op, a, b)


def variables(value):
    if isinstance(value, str): return {value}
    if isinstance(value, tuple): return variables(value[1]) | variables(value[2])
    return set()


def json_expr(value):
    return [value[0], json_expr(value[1]), json_expr(value[2])] if isinstance(value, tuple) else value


def polynomial(value):
    if isinstance(value, int): return {(): value} if value else {}
    if isinstance(value, str): return {(value,): 1}
    op, a, b = value
    left, right = polynomial(a), polynomial(b)
    result = dict(left) if op != '*' else {}
    if op == '*':
        for x, v in left.items():
            for y, w in right.items():
                key = tuple(sorted(x + y))
                result[key] = result.get(key, 0) + v * w
    else:
        for key, coefficient in right.items():
            result[key] = result.get(key, 0) + (coefficient if op == '+' else -coefficient)
    return {k: v for k, v in result.items() if v}


def contains(outer, inner):
    if outer['buffer'] != inner['buffer'] or outer['stride'] != inner['stride']: return False
    delta = polynomial(('-', inner['offset'], outer['offset']))
    stride = outer['stride']
    # For the strided matrix representation, compare constant row/column
    # translations symbolically. Other cases are retained, not guessed.
    row = delta.pop((stride,), 0) if isinstance(stride, str) else 0
    col = delta.pop((), 0)
    if delta or row < 0 or col < 0: return False
    for field, shift in (('rows', row), ('cols', col)):
        extent = polynomial(('-', outer[field], ('+', inner[field], shift)))
        constant = extent.pop((), 0)
        if extent or constant < 0: return False
    return True


def minimal_regions(regions):
    result = []
    for region in regions:
        if any(contains(old, region) for old in result): continue
        result = [old for old in result if not contains(region, old)]
        result.append(region)
    return sorted(result, key=lambda r: json.dumps(r, sort_keys=True))


@dataclass(frozen=True)
class Pointer:
    argument: int
    offset: object = 0


@dataclass
class Function:
    unit: int
    node: dict
    public: bool

    @property
    def params(self):
        return [n for n in children(self.node) if n.get('kind') == 'ParmVarDecl']

    @property
    def body(self):
        return next(n for n in children(self.node) if n.get('kind') == 'CompoundStmt')


class Analyzer:
    def __init__(self, sources, compiler=None, includes=(), flags=()):
        compiler = compiler or shutil.which('clang++') or shutil.which('clang')
        if not compiler:
            raise AnalysisError('Clang is required for offline operator analysis; set --compiler or DAG_LLVM_COMPILER')
        self.functions = {}
        self.declarations = {}
        self.stack = []
        self.accesses = []
        for unit, source in enumerate(sources):
            # The JSON AST can be large for standard headers. Keep it in a
            # temporary file, retain only function definitions, then release it.
            with tempfile.TemporaryFile(mode='w+') as output:
                command = [str(compiler), '-std=c++17', '-fsyntax-only', '-Xclang', '-ast-dump=json']
                for include in includes: command += ['-I', str(include)]
                command += list(flags) + [str(source)]
                run = subprocess.run(command, stdout=output, stderr=subprocess.PIPE, text=True)
                if run.returncode:
                    raise AnalysisError(f'Clang failed for {source}:\n{run.stderr.strip()}')
                output.seek(0)
                self.collect(json.load(output), unit)

    def collect(self, node, unit, namespace=()):
        kind = node.get('kind')
        if kind == 'NamespaceDecl':
            name = node.get('name', '')
            if name in ('std', '__gnu_cxx', '__cxxabiv1'): return
            namespace += (name,)
        if kind == 'FunctionDecl':
            symbol = node.get('mangledName', node['name'])
            self.declarations[(unit, node['id'])] = symbol
            if any(n.get('kind') == 'CompoundStmt' for n in children(node)):
                public = (not node.get('loc', {}).get('includedFrom') and '' not in namespace
                          and node.get('storageClass') != 'static' and not node['name'].startswith('__'))
                self.functions[symbol] = Function(unit, node, public)
            return
        for child in children(node): self.collect(child, unit, namespace)

    def target(self, node, unit):
        ref = unwrap(children(node)[0]).get('referencedDecl', {})
        symbol = self.declarations.get((unit, ref.get('id')))
        return self.functions.get(symbol)

    def call_env(self, node, env, unit, target):
        return {p['id']: (self.address(a, env, unit) if '&' in p['type']['qualType']
                         else self.value(a, env, unit))
                for p, a in zip(target.params, children(node)[1:])}

    def invalidate_mutations(self, node, env):
        """Loop-carried address state has no affine substitution in this subset."""
        if (node.get('kind') in ('BinaryOperator', 'CompoundAssignOperator', 'UnaryOperator')
                and node.get('opcode') in ('=', '+=', '-=', '*=', '/=', '++', '--')):
            lhs = unwrap(children(node)[0])
            if lhs.get('kind') == 'DeclRefExpr':
                key = lhs['referencedDecl']['id']
                old = env.get(key)
                if old is not None:
                    env[key] = Pointer(old.argument, None) if isinstance(old, Pointer) else None
        for child in children(node): self.invalidate_mutations(child, env)

    def value(self, node, env, unit):
        node = unwrap(node)
        kind, inner = node.get('kind'), children(node)
        if kind == 'IntegerLiteral': return int(node['value'])
        if kind == 'DeclRefExpr': return env.get(node['referencedDecl']['id'])
        if kind == 'UnaryOperator':
            op = node['opcode']
            if op == '&': return self.address(inner[0], env, unit)
            if op == '+': return self.value(inner[0], env, unit)
            if op == '-': return expression('-', 0, self.value(inner[0], env, unit))
        if kind == 'BinaryOperator' and node['opcode'] in ('+', '-', '*'):
            a, b = (self.value(x, env, unit) for x in inner)
            if isinstance(b, Pointer) and node['opcode'] == '+': a, b = b, a
            if isinstance(a, Pointer):
                if node['opcode'] not in ('+', '-') or isinstance(b, Pointer): return None
                offset = expression(node['opcode'], a.offset, b)
                return Pointer(a.argument, offset)
            return expression(node['opcode'], a, b)
        if kind == 'CallExpr':
            target = self.target(node, unit)
            if target and len(children(target.body)) == 1 and children(target.body)[0].get('kind') == 'ReturnStmt':
                if target in self.stack: raise AnalysisError('recursive address helper')
                self.stack.append(target)
                try:
                    return self.value(children(children(target.body)[0])[0],
                                      self.call_env(node, env, unit, target), target.unit)
                finally: self.stack.pop()
            if target and '*' in target.node['type']['qualType'].split('(')[0]:
                if any(isinstance(self.value(a, env, unit), Pointer) for a in inner[1:]):
                    raise AnalysisError('pointer-returning helper requires a symbolic return expression')
        return None

    def address(self, node, env, unit):
        node = unwrap(node)
        kind, inner = node.get('kind'), children(node)
        if kind == 'ArraySubscriptExpr':
            base = self.value(inner[0], env, unit)
            if isinstance(base, Pointer):
                return Pointer(base.argument, expression('+', base.offset, self.value(inner[1], env, unit)))
        if kind == 'UnaryOperator' and node['opcode'] == '*': return self.value(inner[0], env, unit)
        if kind == 'DeclRefExpr':
            # Only reference variables denote a memory lvalue here; a pointer
            # variable on an assignment's lhs is a local address update.
            ref = node['referencedDecl']
            if '&' in ref.get('type', {}).get('qualType', ''): return env.get(ref['id'])
        if kind == 'CallExpr':
            target = self.target(node, unit)
            if target and len(children(target.body)) == 1 and children(target.body)[0].get('kind') == 'ReturnStmt':
                if target in self.stack: raise AnalysisError('recursive address helper')
                self.stack.append(target)
                try:
                    return self.address(children(children(target.body)[0])[0],
                                        self.call_env(node, env, unit, target), target.unit)
                finally: self.stack.pop()
        return None

    def record(self, address, mode, bounds):
        if isinstance(address, Pointer):
            if address.offset is None: raise AnalysisError('data-dependent memory subscript is unsupported')
            self.accesses.append((address, mode, dict(bounds)))

    def walk(self, node, env, bounds, unit, mode='read'):
        kind, inner = node.get('kind'), children(node)
        if not kind: return
        if kind == 'ForStmt':
            init, _, condition, increment, body = inner
            local = dict(env)
            self.walk(init, local, bounds, unit)
            cond = unwrap(condition)
            if cond.get('kind') != 'BinaryOperator' or cond.get('opcode') not in ('<', '<='):
                raise AnalysisError('operator loops require increasing integer bounds')
            index = unwrap(children(cond)[0]).get('referencedDecl', {}).get('id')
            step = unwrap(increment)
            increasing = (step.get('opcode') == '++' or
                          (step.get('opcode') == '+=' and isinstance(self.value(children(step)[1], local, unit), int)
                           and self.value(children(step)[1], local, unit) > 0))
            if not index or not increasing: raise AnalysisError('unsupported operator induction variable')
            start = local.get(index)
            stop = self.value(children(cond)[1], local, unit)
            if start is None or stop is None: raise AnalysisError('cannot infer operator loop bounds')
            key = f'loop:{unit}:{index}'
            local[index] = key
            nested = dict(bounds)
            # Preserve a parameterized rectangular envelope for triangular
            # loops too. It is a conflict footprint, not a full-overwrite claim.
            lo = self.bound(start, bounds, False)
            hi = self.bound(stop, bounds, True)
            if not (variables(stop) & bounds.keys()) and cond['opcode'] == '<':
                hi = expression('-', hi, 1)
            nested[key] = (lo, hi)
            self.invalidate_mutations(body, local)
            local[index] = key
            self.walk(body, local, nested, unit)
            return
        if kind in ('WhileStmt', 'DoStmt', 'CXXForRangeStmt'):
            raise AnalysisError('operator loops require explicit integer for bounds')
        if kind == 'VarDecl':
            if inner:
                ty = node.get('type', {}).get('qualType', '')
                env[node['id']] = (self.address(inner[-1], env, unit) if '&' in ty
                                   else self.value(inner[-1], env, unit))
                self.walk(inner[-1], env, bounds, unit)
            return
        if kind in ('BinaryOperator', 'CompoundAssignOperator') and node.get('opcode') in ('=', '+=', '-=', '*=', '/='):
            lhs, rhs = inner
            self.walk(lhs, env, bounds, unit, 'write')
            if node['opcode'] != '=': self.walk(lhs, env, bounds, unit, 'read')
            self.walk(rhs, env, bounds, unit)
            bare = unwrap(lhs)
            if bare.get('kind') == 'DeclRefExpr':
                ref = bare['referencedDecl']
                if '&' not in ref.get('type', {}).get('qualType', ''):
                    old, new = env.get(ref['id']), self.value(rhs, env, unit)
                    env[ref['id']] = new if node['opcode'] == '=' else expression(node['opcode'][0], old, new)
            return
        if kind == 'UnaryOperator' and node.get('opcode') in ('++', '--'):
            self.walk(inner[0], env, bounds, unit, 'read')
            self.walk(inner[0], env, bounds, unit, 'write')
            bare = unwrap(inner[0])
            if bare.get('kind') == 'DeclRefExpr': env[bare['referencedDecl']['id']] = None
            return
        if kind == 'UnaryOperator' and node.get('opcode') == '&': return
        if kind == 'CallExpr':
            address = self.address(node, env, unit)
            if isinstance(address, Pointer):
                self.record(address, mode, bounds)
                return
            target = self.target(node, unit)
            args = children(node)[1:]
            for arg in args: self.walk(arg, env, bounds, unit)
            actuals = self.call_env(node, env, unit, target) if target else {}
            ref = unwrap(inner[0]).get('referencedDecl', {})
            related = (any(isinstance(a, Pointer) for a in actuals.values()) or
                       any(isinstance(self.value(a, env, unit), Pointer) for a in args) or
                       (not target and '&' in ref.get('type', {}).get('qualType', '') and
                        any(isinstance(self.address(a, env, unit), Pointer) for a in args)))
            if related:
                if not target:
                    raise AnalysisError(f'missing operator/helper definition: {ref.get("name", "indirect call")}')
                if target in self.stack: raise AnalysisError('recursive operator call')
                self.stack.append(target)
                try: self.walk(target.body, actuals, bounds, target.unit)
                finally: self.stack.pop()
            return
        if kind == 'ArraySubscriptExpr' or (kind == 'UnaryOperator' and node.get('opcode') == '*'):
            self.record(self.address(node, env, unit), mode, bounds)
            # Indices may themselves read memory. Such indices subsequently
            # fail symbolic inference, but ordinary integer arithmetic is free.
            for child in inner: self.walk(child, env, bounds, unit)
            return
        if kind == 'DeclRefExpr':
            self.record(self.address(node, env, unit), mode, bounds)
            return
        if kind == 'IfStmt':
            # Both branches contribute accesses; don't leak branch-local values.
            for child in inner: self.walk(child, dict(env), bounds, unit)
            self.invalidate_mutations(node, env)
            return
        for child in inner: self.walk(child, env, bounds, unit, mode)

    def bound(self, value, bounds, upper):
        if isinstance(value, str): return bounds[value][int(upper)] if value in bounds else value
        if isinstance(value, int): return value
        if isinstance(value, tuple):
            op, a, b = value
            if op == '-': return expression('-', self.bound(a, bounds, upper), self.bound(b, bounds, not upper))
            if op == '*' and ((isinstance(a, int) and a < 0) or (isinstance(b, int) and b < 0)):
                factor, other = (a, b) if isinstance(a, int) and a < 0 else (b, a)
                return expression('*', factor, self.bound(other, bounds, not upper))
            return expression(op, self.bound(a, bounds, upper), self.bound(b, bounds, upper))
        raise AnalysisError('cannot bound operator memory access')

    def region(self, address, bounds):
        # Decompose row*stride + column using dependence on loop variables;
        # parameter spelling and ordering do not affect this decision.
        def terms(value):
            if isinstance(value, tuple) and value[0] == '+': return terms(value[1]) + terms(value[2])
            return [value]
        parts = terms(address.offset)
        candidate = None
        for at, term in enumerate(parts):
            if isinstance(term, tuple) and term[0] == '*':
                a, b = term[1:]
                if variables(a) & bounds.keys() and not variables(b) & bounds.keys(): candidate = (at, a, b)
                elif variables(b) & bounds.keys() and not variables(a) & bounds.keys(): candidate = (at, b, a)
                if candidate: break
        if candidate:
            at, row, stride = candidate
            col = 0
            for i, part in enumerate(parts):
                if i != at: col = expression('+', col, part)
            rlo, rhi = self.bound(row, bounds, False), self.bound(row, bounds, True)
        else:
            col, stride, rlo, rhi = address.offset, 1, 0, 0
        clo, chi = self.bound(col, bounds, False), self.bound(col, bounds, True)
        offset = expression('+', f'arg{address.argument}', expression('+', expression('*', rlo, stride), clo))
        rows = expression('+', expression('-', rhi, rlo), 1)
        cols = expression('+', expression('-', chi, clo), 1)
        if not candidate: stride = cols
        return {'buffer': 'L', 'offset': json_expr(offset), 'rows': json_expr(rows),
                'cols': json_expr(cols), 'stride': json_expr(stride)}

    def contract(self, target):
        kinds, env = [], {}
        for index, param in enumerate(target.params):
            ty = param['type'].get('desugaredQualType', param['type']['qualType'])
            if re.fullmatch(r'(?:const )?double\s*\*(?:const)?', ty.strip()):
                kinds.append('pointer:L'); env[param['id']] = Pointer(index)
            elif re.fullmatch(r'(?:(?:const|unsigned|signed) )*(?:int|long|long long|short)', ty.strip()):
                kinds.append('int'); env[param['id']] = f'arg{index}'
            else: raise AnalysisError(f'{target.node["name"]}: unsupported ABI parameter {ty}')
        self.accesses = []
        self.stack = [target]
        self.walk(target.body, env, {}, target.unit)
        result = {'arguments': kinds, 'completion': 'synchronous', 'reentrant': True,
                  'internal_threads': 1, 'reads': [], 'writes': []}
        for address, mode, bounds in self.accesses:
            region = self.region(address, bounds)
            if region not in result[mode + 's']: result[mode + 's'].append(region)
        for mode in ('reads', 'writes'): result[mode] = minimal_regions(result[mode])
        return result

    def infer(self, operators=()):
        selected = {}
        for target in self.functions.values():
            name = target.node['name']
            if (name in operators if operators else target.public and
                    target.node['type']['qualType'].startswith('void (') and
                    any('*' in p['type']['qualType'] for p in target.params)):
                if name in selected: raise AnalysisError(f'overloaded operator {name}: select an unambiguous public wrapper')
                selected[name] = target
        missing = set(operators) - selected.keys()
        if missing: raise AnalysisError('operator definitions not found: ' + ', '.join(sorted(missing)))
        if not selected: raise AnalysisError('no operator definitions found in supplied sources')
        return {'schema_version': 1, 'operators': {name: self.contract(target) for name, target in sorted(selected.items())}}


def infer_catalog(sources, compiler=None, includes=(), flags=(), operators=()):
    return Analyzer(sources, compiler, includes, flags).infer(operators)


def add_arguments(parser):
    parser.add_argument('--operator-source', action='append', type=Path, default=[], help='operator/helper translation unit; repeat for multiple files')
    parser.add_argument('--compiler', help='Clang executable used only for offline analysis')
    parser.add_argument('--include', action='append', type=Path, default=[])
    parser.add_argument('--clang-arg', action='append', default=[], help='extra Clang argument, e.g. --clang-arg=-DMODE=1')
    parser.add_argument('--operator', action='append', default=[], help='optional public operator selection')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    add_arguments(parser)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not args.operator_source: parser.error('at least one --operator-source is required')
    try:
        catalog = infer_catalog(args.operator_source, args.compiler, args.include, args.clang_arg, args.operator)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(catalog, indent=2) + '\n')
        print('inferred operators=' + ','.join(catalog['operators']))
    except (OSError, AnalysisError) as error:
        parser.exit(1, f'operator analysis: {error}\n')


if __name__ == '__main__':
    main()
