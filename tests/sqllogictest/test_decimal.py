"""Exact decimal arithmetic and comparisons against Python's decimal module."""
from decimal import Decimal, localcontext
import random
import sys
from run import Core


def main():
    rng = random.Random(260913)
    core = Core(sys.argv[1])
    checked = 0
    try:
        with localcontext() as context:
            context.prec = 200
            for _ in range(400):
                p, q = rng.randint(1, 38), rng.randint(1, 38)
                s, t = rng.randint(0, p), rng.randint(0, q)
                a = Decimal(rng.randint(-10**p+1, 10**p-1)).scaleb(-s)
                b = Decimal(rng.randint(-10**q+1, 10**q-1)).scaleb(-t)
                left, right = f"CAST('{a:f}' AS DECIMAL({p},{s}))", f"CAST('{b:f}' AS DECIMAL({q},{t}))"
                for op, wanted in [('+', a+b), ('-', a-b), ('*', a*b)]:
                    scale = s+t if op == '*' else max(s, t)
                    precision = min(38, p+q if op == '*' else max(p-s, q-t)+scale+1)
                    result = core.execute(f'SELECT {left}{op}{right}')
                    if scale > 38 or abs(wanted) >= Decimal(10)**(precision-scale):
                        assert result[0] == 'error', (left, op, right, result, wanted)
                    else:
                        assert result[0] == 'ok' and result[1] == [[wanted]], (left, op, right, result, wanted)
                        assert isinstance(result[1][0][0], Decimal), result
                    checked += 1
                result = core.execute(f'SELECT {left}={right},{left}<{right},{left}>{right}')
                assert result == ('ok', [[int(a == b), int(a < b), int(a > b)]]), (a, b, result)
                checked += 1
        print(f'{checked} exact decimal statements matched Python Decimal')
    finally:
        core.close()


if __name__ == '__main__':
    main()
