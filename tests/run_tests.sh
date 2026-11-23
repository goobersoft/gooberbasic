#!/bin/sh
# Runs every tests/*.gb that has a matching .expected file and diffs the output.
cd "$(dirname "$0")/.." || exit 1
GOOBER=${GOOBER:-./goober}
pass=0; fail=0
for t in tests/*.gb; do
    exp="${t%.gb}.expected"
    [ -f "$exp" ] || continue
    if $GOOBER "$t" 2>&1 < /dev/null | diff -u "$exp" - > /dev/null; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1)); echo "FAIL: $t"; $GOOBER "$t" 2>&1 < /dev/null | diff -u "$exp" - | head -20
    fi
done
# compile errors, one per file: the first line is "' expect: <text the error must contain>"
# (scope_errors: what an included file keeps to itself; their includes are in scope_errors/lib)
for f in tests/superstrict/*.gb tests/scope_errors/*.gb tests/data_errors/*.gb; do
    want=$(sed -n "1s/^' expect: //p" "$f")
    got=$($GOOBER --check "$f" 2>&1)
    if printf '%s' "$got" | grep -qF -- "$want"; then pass=$((pass + 1))
    else fail=$((fail + 1)); echo "FAIL: $f: wanted '$want'"; printf '%s\n' "$got" | head -5; fi
done
for f in examples/*.gb; do
    if ! $GOOBER --check "$f" > /dev/null; then fail=$((fail + 1)); echo "FAIL: $f does not compile"; else pass=$((pass + 1)); fi
done
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
