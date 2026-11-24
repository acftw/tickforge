import random

import pytest
import tickforge as tf
from tickforge.reference_book import PyBook

BUY, SELL = tf.Side.BUY, tf.Side.SELL


def test_price_time_priority():
    b = tf.OrderBook(1, 1000)
    a1 = b.add_limit(SELL, 101, 5)
    a2 = b.add_limit(SELL, 100, 3)
    a3 = b.add_limit(SELL, 100, 4)
    r = b.add_limit(BUY, 101, 9)
    assert [(f.maker_id, f.price, f.qty) for f in b.fills()] == [(a2.id, 100, 3), (a3.id, 100, 4), (a1.id, 101, 2)]
    assert r.filled == 9 and r.resting == 0
    assert b.order_qty(a1.id) == 3


def test_reduce_keeps_priority_and_stale_ids_are_rejected():
    b = tf.OrderBook(1, 1000)
    o1 = b.add_limit(BUY, 50, 10)
    o2 = b.add_limit(BUY, 50, 10)
    assert b.reduce(o1.id, 4)
    assert b.queue_ahead(o2.id) == 4
    b.add_market(SELL, 5)
    assert b.fills()[0].maker_id == o1.id
    assert not b.cancel(o1.id)  # already filled
    assert b.levels(BUY, 5) == [(50, 9, 1)]


@pytest.mark.parametrize("seed", [11, 12, 13])
def test_matches_independent_python_book(seed):
    """C++ engine vs a pure-Python book written separately: identical fills and state."""
    rng = random.Random(seed)
    fast, ref = tf.OrderBook(1, 400), PyBook()
    id_map = {}  # C++ id -> Python id
    live = []
    n_fills = 0
    for _ in range(20000):
        if rng.random() < 0.6 or not live:
            side = rng.choice(["buy", "sell"])
            px = 200 + rng.randint(-6, 4) * (1 if side == "buy" else -1)
            qty = rng.randint(1, 8)
            ioc = rng.random() < 0.1
            r = fast.add_limit(BUY if side == "buy" else SELL, px, qty, 0, tf.Tif.IOC if ioc else tf.Tif.GTC)
            pid = ref.add(side, px, qty, ioc)
            got = [(id_map[f.maker_id], f.price, f.qty) for f in fast.fills()]
            assert got == ref.fills
            n_fills += len(got)
            if r.resting:
                id_map[r.id] = pid
                live.append(r.id)
        else:
            oid = live.pop(rng.randrange(len(live)))
            assert fast.cancel(oid) == ref.cancel(id_map[oid])
        assert (fast.best_bid() if fast.has_bid() else None) == ref.best_bid()
        assert (fast.best_ask() if fast.has_ask() else None) == ref.best_ask()
    assert n_fills > 2000
