"""Split one simple SELECT into ctid page ranges, so N rings can scan it at once.

`Cursor.aggregate()` uses this to fan a single aggregate out over the ring pool.
Each range is appended to the query as

    ctid >= '(lo,0)'::tid AND ctid < '(hi,0)'::tid

which PostgreSQL 18 plans as a `Tid Range Scan`; the ranges are disjoint and
together cover pages [0, relpages), so the relation is scanned once across all
of them. The partial aggregates are exact, so combining them is arithmetic and
not an approximation — unlike a sample, the answer is the same one a single
scan gives.

Only a plain single-table SELECT is splittable:

    SELECT <anything> FROM [[schema.]table | alias] [WHERE <condition>]

The WHERE condition is arbitrary — it is parenthesised and AND-ed with the
range — but everything else that shapes the result is refused: a join, grouping,
HAVING, ordering, LIMIT/OFFSET, a set operation, a CTE, DISTINCT, a window, a
locking clause, FETCH FIRST/NEXT, a subquery or a function (`LATERAL`,
set-returning) in FROM, more than one FROM item, more than one statement. The
caller then keeps the single-ring path. Doubt is answered with None: being too
timid only costs speed, while splitting a query whose rows we do not fully
understand would cost rows.

The scanner is deliberately a scanner and not a parser. It walks the text once,
tracks parenthesis depth, and skips quoted text, quoted identifiers, `--` and
`/* */` (nested) comments and dollar quotes wholesale, so a keyword in a literal
is data and a clause inside a function call is not top level. That is enough to
recognise the shape above and to refuse everything else.

Pure stdlib: importing this module must not need the native extension.
"""

__all__ = ["table_of", "partition_sql"]


class _Token:
    """One token at parenthesis depth 0.

    ``kind`` is ``word`` (bare identifier or keyword, original case kept),
    ``qident`` (a double-quoted identifier, quotes and doubled quotes as
    written), or one of ``.`` ``,`` ``(`` ``)`` ``;`` ``other`` (an operator, a
    number — anything the shape does not need to name).
    """

    __slots__ = ("kind", "text", "start", "end")

    def __init__(self, kind, text, start, end):
        self.kind = kind
        self.text = text
        self.start = start
        self.end = end

    def __repr__(self):
        return f"_Token({self.kind!r}, {self.text!r})"


# Words that mean the query is not a shape we can split. Checked at depth 0
# only, so a LIMIT inside a subquery of the WHERE clause stays allowed.
#
# The join and set-operation words are here because rows would then come from
# more than the one relation; the grouping/ordering/slicing words because the
# result is not a function of the whole scan; DISTINCT/OVER/WINDOW because the
# answer depends on rows the other partitions may own; WITH/FOR/FETCH/INTO
# because the statement is not a plain read.
_BANNED = frozenset(
    """join inner left right full cross natural
    group having order limit offset
    union intersect except
    distinct over window
    with for fetch into returning""".split()
)

# Words that may not stand where a relation or an alias goes: reserved words, the
# FROM-item modifiers we do not handle, and the clause words we treat as the end
# of the FROM item. A bare word here means "not a simple base table", so refuse.
_NOT_A_NAME = _BANNED | frozenset(
    """lateral only select from where as on using tablesample""".split()
)


def _tokens(sql):
    """The tokens at parenthesis depth 0, and whether the text was delimited.

    The second element is False on an unterminated quote, comment or dollar
    quote, or an unbalanced ``)``, because the rest of the text then cannot be
    read as code. Nothing is guessed after that: the caller refuses.
    """
    out = []
    depth = 0
    i = 0
    n = len(sql)
    while i < n:
        ch = sql[i]
        if ch.isspace():
            i += 1
            continue

        if ch == "-" and sql.startswith("--", i):  # line comment: runs to the newline
            j = sql.find("\n", i + 2)
            i = n if j < 0 else j + 1
            continue

        if ch == "/" and sql.startswith("/*", i):
            level = 0  # PostgreSQL's block comments nest
            j = i
            while j < n:
                if sql.startswith("/*", j):
                    level += 1
                    j += 2
                elif sql.startswith("*/", j):
                    level -= 1
                    j += 2
                    if level == 0:
                        break
                else:
                    j += 1
            else:
                return out, False
            i = j
            continue

        if ch == "'":  # a string literal: '' is an escaped quote
            j = i + 1
            while j < n:
                if sql[j] == "'":
                    if j + 1 < n and sql[j + 1] == "'":
                        j += 2
                        continue
                    j += 1
                    break
                j += 1
            else:
                return out, False
            i = j
            continue

        if ch == '"':  # a quoted identifier: "" is an escaped quote
            j = i + 1
            while j < n:
                if sql[j] == '"':
                    if j + 1 < n and sql[j + 1] == '"':
                        j += 2
                        continue
                    j += 1
                    break
                j += 1
            else:
                return out, False
            if depth == 0:
                out.append(_Token("qident", sql[i:j], i, j))
            i = j
            continue

        if ch == "$":  # $tag$...$tag$ — a body could hold anything, so skip it
            j = i + 1
            while j < n and (sql[j].isalnum() or sql[j] == "_"):
                j += 1
            if j < n and sql[j] == "$":
                tag = sql[i : j + 1]
                close = sql.find(tag, j + 1)
                if close < 0:
                    return out, False
                i = close + len(tag)
                continue
            # Not a tag: a positional parameter ($1) or an operator's dollar sign.
            if depth == 0:
                out.append(_Token("other", ch, i, i + 1))
            i += 1
            continue

        if ch == "(":
            if depth == 0:
                out.append(_Token("(", ch, i, i + 1))
            depth += 1
            i += 1
            continue

        if ch == ")":
            depth -= 1
            if depth < 0:
                return out, False
            if depth == 0:
                out.append(_Token(")", ch, i, i + 1))
            i += 1
            continue

        if ch in ".,;" and depth == 0:
            out.append(_Token(ch, ch, i, i + 1))
            i += 1
            continue

        if ch.isalpha() or ch == "_":  # an identifier or a keyword
            j = i + 1
            while j < n and (sql[j].isalnum() or sql[j] in "_$"):
                j += 1
            if depth == 0:
                out.append(_Token("word", sql[i:j], i, j))
            i = j
            continue

        if ch.isdigit():  # a number, including its fractional part
            j = i + 1
            while j < n and (sql[j].isdigit() or sql[j] == "."):
                j += 1
            if depth == 0:
                out.append(_Token("other", sql[i:j], i, j))
            i = j
            continue

        # Any other operator character: kept as "other" so that a stray one in
        # the FROM item (say `t *`) is noticed instead of silently dropped.
        if depth == 0:
            out.append(_Token("other", ch, i, i + 1))
        i += 1

    if depth != 0:
        return out, False
    return out, True


class _Shape:
    """The pieces of a splittable SELECT, and where the rewrite may cut it.

    ``table`` is the relation as written (`public.t`, `"My Table"`), so it can be
    handed to ``::regclass`` unchanged. ``where`` is the top-level WHERE token,
    or None. ``cut`` is where the statement ends — the trailing ``;`` and
    anything after it, which the rewrite leaves in place.
    """

    __slots__ = ("table", "where", "cut")

    def __init__(self, table, where, cut):
        self.table = table
        self.where = where
        self.cut = cut


def _analyse(sql):
    """The shape of `sql`, or None when it is not a simple single-table SELECT."""
    if not isinstance(sql, str):
        return None
    toks, delimited = _tokens(sql)
    if not delimited or not toks:
        return None

    # One statement: a ';' is allowed only as the terminator.
    cut = len(sql)
    for k, tok in enumerate(toks):
        if tok.kind == ";":
            if k != len(toks) - 1:
                return None
            cut = tok.start
            toks = toks[:-1]
            break
    if not toks:
        return None

    head = toks[0]
    if head.kind != "word" or head.text.lower() != "select":
        return None

    # Refuse every clause we cannot carry across a page range. This runs before
    # the FROM item is parsed, so `FROM t JOIN u` and `FROM t WHERE x LIMIT 1`
    # are both caught by the same rule.
    for tok in toks:
        if tok.kind == "word" and tok.text.lower() in _BANNED:
            return None

    # Exactly one top-level FROM, so exactly one relation: a second FROM is a
    # subquery that got flattened out of its parentheses or invalid SQL.
    froms = [k for k, tok in enumerate(toks) if tok.kind == "word" and tok.text.lower() == "from"]
    if len(froms) != 1:
        return None
    i = froms[0] + 1

    # The relation name: an identifier, optionally schema-qualified and/or
    # quoted, dotted as far as it goes (`db.schema.table`).
    parts = []
    while True:
        if i >= len(toks):
            return None
        tok = toks[i]
        if tok.kind == "qident":
            pass
        elif tok.kind == "word" and tok.text.lower() not in _NOT_A_NAME:
            pass
        else:
            # `FROM (`, `FROM LATERAL`, `FROM ONLY t`, a missing name ...
            return None
        parts.append(tok.text)
        i += 1
        if i < len(toks) and toks[i].kind == ".":
            i += 1
            continue
        break

    # A function call or a subquery in FROM is not a base relation. `(` also
    # covers the alias's column list (`FROM t AS x(a, b)`), refused below.
    if i < len(toks) and toks[i].kind == "(":
        return None

    # An alias, `AS x` or a bare `x`; a clause word is left for the tail.
    if i < len(toks) and toks[i].kind == "word" and toks[i].text.lower() == "as":
        i += 1
        if i >= len(toks):
            return None
        tok = toks[i]
        if tok.kind == "qident":
            i += 1
        elif tok.kind == "word" and tok.text.lower() not in _NOT_A_NAME:
            i += 1
        else:
            return None
    elif i < len(toks) and toks[i].kind in ("word", "qident"):
        if toks[i].kind == "qident" or toks[i].text.lower() not in _NOT_A_NAME:
            i += 1

    # What is left is the WHERE clause or nothing: the condition itself is not
    # parsed (it may be any expression), and the banned words above already
    # ruled out the clauses that would make a page split wrong.
    where = None
    if i < len(toks):
        tok = toks[i]
        if tok.kind != "word" or tok.text.lower() != "where":
            return None  # a second FROM item, a comma, a stray operator, ...
        i += 1
        if i >= len(toks):
            return None  # WHERE with no condition
        where = tok

    return _Shape(".".join(parts), where, cut)


def table_of(sql):
    """The single relation `sql` reads, or None when the shape is not splittable.

    A splittable query is one SELECT over one base relation with no join,
    grouping, ordering, limit, set operation, CTE, DISTINCT, window, locking
    clause, FETCH FIRST/NEXT, or subquery/function in FROM. The name is returned
    as written, so it feeds ``::regclass`` directly.
    """
    shape = _analyse(sql)
    return None if shape is None else shape.table


def _ctid_range(lo, hi):
    """One page range, as the predicate that pins a Tid Range Scan to it.

    ``hi`` is exclusive and points at the smallest ctid of the next page, so
    every tuple on page ``hi - 1`` is inside the range.
    """
    return f"ctid >= '({lo},0)'::tid AND ctid < '({hi},0)'::tid"


def partition_sql(sql, parts, relpages):
    """`sql` split into `parts` statements, each restricted to a page range.

    None when the statement is not splittable, when ``parts < 2`` (there is
    nothing to fan out), or when ``relpages < parts`` (fewer than one page per
    slice: a tiny table is not worth N rings). ``relpages`` is the relation's
    page count, and the ranges are contiguous over ``[0, relpages)``, roughly
    equal, with empty ranges skipped.

    The query is otherwise preserved as written: a top-level WHERE has its
    condition wrapped in parentheses and AND-ed with the range, and a query
    without one gains a WHERE. A trailing ``;`` (and any comment after it) stays
    where it was.
    """
    shape = _analyse(sql)
    if shape is None:
        return None
    try:
        parts = int(parts)
        relpages = int(relpages)
    except (TypeError, ValueError):
        return None
    if parts < 2 or relpages < parts:
        return None

    step = -(-relpages // parts)  # ceil: at least one page per slice
    text, tail = sql[: shape.cut], sql[shape.cut:]
    sqls = []
    lo = 0
    while lo < relpages:
        hi = min(lo + step, relpages)  # the last slice takes the remainder
        cond = _ctid_range(lo, hi)
        if shape.where is None:
            sqls.append(f"{text} WHERE {cond}{tail}")
        else:
            # Keep the caller's condition, but bind it tighter than the AND so
            # `a OR b` stays one condition instead of swallowing the range.
            condition = text[shape.where.end :]
            sqls.append(f"{text[: shape.where.end]} ({condition}) AND {cond}{tail}")
        lo = hi
    return sqls
