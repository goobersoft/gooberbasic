#!/usr/bin/env python3
"""Generates syntaxes/gooberbasic.tmLanguage.json from the compiler's sources.

The keyword table (src/lexer.c), the built-in commands (src/builtins.c,
src/gfx.c), the list commands (src/compiler.c) and the KEY_/MOUSE_ constants
(src/gfx.c) are read straight from the C code, so the highlighting can't drift
out of date when the language changes. Run it from anywhere:

    python3 editors/vscode/generate.py

A program that embeds GooberBASIC with its own native table (GooberEngine's
src/api.c, say) can add those commands and constants to the highlighting:

    python3 editors/vscode/generate.py --with ../gooberengine/src/api.c
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "src")


def read(name):
    with open(os.path.join(SRC, name)) as f:
        return f.read()


lexer, builtins, gfx, compiler = read("lexer.c"), read("builtins.c"), read("gfx.c"), read("compiler.c")
net = read("net.c")

# ---- names from the sources ----
# only the keywords[] table: the "end data" / "end if" merge table holds words that aren't keywords alone
kwtable = re.search(r'keywords\[\] = \{(.*?)\{NULL', lexer, re.S).group(1)
keywords = set(re.findall(r'\{"([a-z]+)", T_[A-Z]+\}', kwtable))

natives = set()
for src in (builtins, gfx, net):
    natives |= set(n for n in re.findall(r'^\s*\{"([a-z0-9_]+)", \'[a-z]\'', src, re.M) if not n.startswith("__"))

m = re.search(r'LIST_FNS\[\] = \{(.*?)NULL', compiler, re.S)
natives |= set(re.findall(r'"([a-z_]+)"', m.group(1)))

# intrinsics handled directly by the compiler (see intrinsic() in compiler.c)
intrinsics = {"len", "str", "string", "abs", "sgn", "min", "max"}

consts = ["KEY_" + k for k in re.findall(r'K\(([A-Z0-9]+), [A-Z0-9_]+\)', gfx)]
consts += re.findall(r'\{"(MOUSE_[A-Z]+)", \d+\}', gfx)
consts += re.findall(r'\{"(NET_[A-Z_]+)", ', net)

# --with FILE.c: more native tables ({"name", 'r', ...}) and constants ({"NAME", ...}, K(...))
args = sys.argv[1:]
while args:
    if args[0] != "--with" or len(args) < 2:
        raise SystemExit("usage: generate.py [--with file.c ...]")
    with open(args[1]) as f:
        extra = f.read()
    natives |= set(n for n in re.findall(r'^\s*\{"([a-z0-9_]+)", \'[a-z]\'', extra, re.M) if not n.startswith("__"))
    consts += ["KEY_" + k for k in re.findall(r'K\(([A-Z0-9]+), [A-Z0-9_]+\)', extra)]
    consts += re.findall(r'\{"([A-Z][A-Z0-9_]*)", [^}]*\}', extra)
    args = args[2:]
consts = list(dict.fromkeys(consts))

# the built-in type names come from NUM_TYPES in compiler.c (each one is also a conversion function)
types = re.findall(r'\{"([a-z0-9]+)", NT_[A-Z0-9]+\}', compiler) + ["string"]

# keywords the compiler rejects with a "was removed" message
removed = {"delete", "each"}

control = {"if", "then", "else", "elseif", "endif", "while", "wend", "endwhile", "repeat", "until",
           "forever", "for", "to", "step", "next", "eachin", "select", "case", "default",
           "endselect", "exit", "continue", "return", "end"}
declare = {"function", "endfunction", "type", "endtype", "field", "local", "global", "const",
           "dim", "strict", "superstrict", "include", "new", "defdata", "enddata"}
wordops = {"and", "or", "not", "mod", "shl", "shr", "sar"}
literals = {"true", "false", "pi", "null"}   # true/false/pi are built-in constants, null a keyword

unknown = keywords - control - declare - wordops - literals - removed - {"rem", "endrem"}
if unknown:
    raise SystemExit("generate.py: keywords with no highlighting group: " + ", ".join(sorted(unknown)))


def alt(words):
    # longest first so that e.g. 'elseif' wins over 'else'
    return "|".join(sorted((re.escape(w) for w in words), key=lambda w: (-len(w), w)))


ID = r"[A-Za-z_][A-Za-z0-9_]*"
TYPE = r"(?:" + alt(types) + r"|" + ID + r")"

grammar = {
    "$schema": "https://raw.githubusercontent.com/martinring/tmlanguage/master/tmlanguage.json",
    "name": "GooberBASIC",
    "scopeName": "source.gooberbasic",
    "fileTypes": ["gb"],
    "patterns": [
        {"include": "#comments"},
        {"include": "#strings"},
        {"include": "#continuation"},
        {"include": "#definitions"},
        {"include": "#annotations"},
        {"include": "#keywords"},
        {"include": "#numbers"},
        {"include": "#builtins"},
        {"include": "#constants"},
        {"include": "#calls"},
        {"include": "#variables"},
        {"include": "#operators"},
    ],
    "repository": {
        "comments": {"patterns": [
            {"name": "comment.line.apostrophe.gooberbasic", "match": "'.*$"},
            # rem starts a block that runs to a line starting with endrem / end rem
            {"name": "comment.block.rem.gooberbasic",
             "begin": r"(?i)\brem\b", "end": r"(?i)^\s*end\s*rem\b"},
        ]},
        "strings": {"patterns": [{
            "name": "string.quoted.double.gooberbasic",
            "begin": '"', "end": '"|$',
            "patterns": [
                {"name": "constant.character.escape.gooberbasic", "match": r"~[ntrq0~]"},
                {"name": "invalid.illegal.escape.gooberbasic", "match": r"~."},
            ],
        }]},
        # `..` joins the next line; the rest of this line is ignored by the lexer
        "continuation": {"patterns": [{
            "match": r"(\.\.)(.*)$",
            "captures": {"1": {"name": "punctuation.separator.continuation.gooberbasic"},
                         "2": {"name": "comment.line.continuation.gooberbasic"}},
        }]},
        "definitions": {"patterns": [
            {"match": r"(?i)\b(function)\s+(" + ID + r")([%#$])?",
             "captures": {"1": {"name": "storage.type.function.gooberbasic"},
                          "2": {"name": "entity.name.function.gooberbasic"},
                          "3": {"name": "storage.type.suffix.gooberbasic"}}},
            {"match": r"(?i)\b(type)\s+(" + ID + r")",
             "captures": {"1": {"name": "storage.type.type.gooberbasic"},
                          "2": {"name": "entity.name.type.gooberbasic"}}},
            {"match": r"(?i)\b(end\s*(?:function|type))\b",
             "captures": {"1": {"name": "storage.type.end.gooberbasic"}}},
        ]},
        # `:int`, `:t_enemy`, `:list<float>`, `:string[]`, `new t_enemy` and `new list<int>`
        "annotations": {"patterns": [
            # written with no spaces (`x:int`), which tells it apart from the `:` statement separator
            {"match": r"(?i)(?<=[A-Za-z0-9_%#$])(:)(?:(list)\s*(<)\s*(" + TYPE + r")?|(" + TYPE + r"))(?![A-Za-z0-9_])",
             "captures": {"1": {"name": "punctuation.separator.type.gooberbasic"},
                          "2": {"name": "storage.type.list.gooberbasic"},
                          "3": {"name": "punctuation.definition.list.gooberbasic"},
                          "4": {"name": "storage.type.gooberbasic"},
                          "5": {"name": "storage.type.gooberbasic"}}},
            {"match": r"(?i)\b(new)\s+(?:(list)\s*(<)\s*(" + TYPE + r")?|(" + TYPE + r"))",
             "captures": {"1": {"name": "keyword.other.new.gooberbasic"},
                          "2": {"name": "storage.type.list.gooberbasic"},
                          "3": {"name": "punctuation.definition.list.gooberbasic"},
                          "4": {"name": "storage.type.gooberbasic"},
                          "5": {"name": "storage.type.gooberbasic"}}},
            # the element type of a list<...> anywhere else: function parameters, list<int(int)>
            {"match": r"(?i)\b(list)\s*(<)\s*(" + TYPE + r")",
             "captures": {"1": {"name": "storage.type.list.gooberbasic"},
                          "2": {"name": "punctuation.definition.list.gooberbasic"},
                          "3": {"name": "storage.type.gooberbasic"}}},
        ]},
        "keywords": {"patterns": [
            {"name": "keyword.control.gooberbasic",
             "match": r"(?i)\b(?:end\s+(?:if|select|while)|else\s+if)\b"},
            {"name": "invalid.deprecated.gooberbasic", "match": r"(?i)\b(?:" + alt(removed) + r")\b"},
            {"name": "keyword.control.gooberbasic", "match": r"(?i)\b(?:" + alt(control) + r")\b"},
            {"name": "storage.modifier.gooberbasic", "match": r"(?i)\b(?:" + alt(declare) + r")\b"},
            {"name": "keyword.operator.word.gooberbasic", "match": r"(?i)\b(?:" + alt(wordops) + r")\b"},
            {"name": "constant.language.gooberbasic", "match": r"(?i)\b(?:" + alt(literals) + r")\b"},
        ]},
        "numbers": {"patterns": [
            {"name": "constant.numeric.hex.gooberbasic", "match": r"(?<![A-Za-z0-9_])\$[0-9A-Fa-f]+"},
            {"name": "constant.numeric.binary.gooberbasic", "match": r"(?<![A-Za-z0-9_])%[01]+"},
            {"name": "constant.numeric.float.gooberbasic",
             "match": r"(?<![A-Za-z0-9_])(?:\d+\.\d*(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?|\d+[eE][+-]?\d+)"},
            {"name": "constant.numeric.integer.gooberbasic", "match": r"(?<![A-Za-z0-9_])\d+"},
        ]},
        "builtins": {"patterns": [
            {"name": "support.function.builtin.gooberbasic",
             "match": r"(?i)\b(?:" + alt(intrinsics) + r")(?=\s*\()"},
            {"name": "support.function.builtin.gooberbasic",
             "match": r"(?i)\b(?:" + alt(natives) + r")\b(?![%#$])"},
            {"name": "storage.type.gooberbasic", "match": r"(?i)\b(?:" + alt(types) + r")\b"},
        ]},
        "constants": {"patterns": [
            {"name": "support.constant.gooberbasic", "match": r"(?i)\b(?:" + alt(consts) + r")\b"},
            # by convention constants are UPPER_SNAKE_CASE
            {"name": "constant.other.gooberbasic", "match": r"\b[A-Z][A-Z0-9_]*\b"},
        ]},
        "calls": {"patterns": [{
            "match": r"\b(" + ID + r")([%#$])?(?=\s*\()",
            "captures": {"1": {"name": "entity.name.function.call.gooberbasic"},
                         "2": {"name": "storage.type.suffix.gooberbasic"}},
        }]},
        "variables": {"patterns": [
            {"match": r"(\.)\s*(" + ID + r")",
             "captures": {"1": {"name": "punctuation.accessor.gooberbasic"},
                          "2": {"name": "variable.other.member.gooberbasic"}}},
            {"match": r"\b(" + ID + r")([%#$])",
             "captures": {"1": {"name": "variable.other.gooberbasic"},
                          "2": {"name": "storage.type.suffix.gooberbasic"}}},
        ]},
        "operators": {"patterns": [
            {"name": "keyword.operator.assignment.compound.gooberbasic", "match": r":[+\-*/]"},
            {"name": "keyword.operator.comparison.gooberbasic", "match": r"<>|<=|>=|<|>"},
            {"name": "keyword.operator.assignment.gooberbasic", "match": r"="},
            {"name": "keyword.operator.arithmetic.gooberbasic", "match": r"[-+*/\\^]"},
            {"name": "keyword.operator.bitwise.gooberbasic", "match": r"[&|~]"},
            {"name": "punctuation.separator.statement.gooberbasic", "match": r"[:;]"},
            {"name": "punctuation.separator.comma.gooberbasic", "match": r","},
        ]},
    },
}

out = os.path.join(HERE, "syntaxes", "gooberbasic.tmLanguage.json")
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, "w") as f:
    json.dump(grammar, f, indent=2)
    f.write("\n")
print("wrote %s (%d keywords, %d built-in commands, %d constants)" % (
    os.path.relpath(out), len(keywords), len(natives) + len(intrinsics), len(consts)))
