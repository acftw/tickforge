"""A pure-Python price-time priority order book, written independently of the C++ code.

It exists only to cross-check the C++ engine from Python (tests/python). It is
slow and simple on purpose: dicts of price -> list of [id, qty], linear scans.
"""

from __future__ import annotations


class PyBook:
    def __init__(self):
        self.bids: dict[int, list[list[int]]] = {}
        self.asks: dict[int, list[list[int]]] = {}
        self.where: dict[int, tuple[str, int]] = {}
        self.next_id = 1
        self.fills: list[tuple[int, int, int]] = []  # (maker_id, price, qty)

    def add(self, side: str, price: int, qty: int, ioc: bool = False) -> int:
        self.fills = []
        oid = self.next_id
        self.next_id += 1
        book, crosses = (self.asks, lambda p: p <= price) if side == "buy" else (self.bids, lambda p: p >= price)
        while qty > 0 and book:
            best = min(book) if side == "buy" else max(book)
            if not crosses(best):
                break
            queue = book[best]
            while qty > 0 and queue:
                maker = queue[0]
                take = min(qty, maker[1])
                self.fills.append((maker[0], best, take))
                maker[1] -= take
                qty -= take
                if maker[1] == 0:
                    queue.pop(0)
                    del self.where[maker[0]]
            if not queue:
                del book[best]
        if qty > 0 and not ioc:
            own = self.bids if side == "buy" else self.asks
            own.setdefault(price, []).append([oid, qty])
            self.where[oid] = (side, price)
        return oid

    def cancel(self, oid: int) -> bool:
        if oid not in self.where:
            return False
        side, price = self.where.pop(oid)
        book = self.bids if side == "buy" else self.asks
        book[price] = [o for o in book[price] if o[0] != oid]
        if not book[price]:
            del book[price]
        return True

    def best_bid(self):
        return max(self.bids) if self.bids else None

    def best_ask(self):
        return min(self.asks) if self.asks else None

    def depth(self, side: str, price: int) -> int:
        book = self.bids if side == "buy" else self.asks
        return sum(o[1] for o in book.get(price, []))
