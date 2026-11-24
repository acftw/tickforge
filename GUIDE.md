# tickforge: a guide for explaining it in interviews

This guide walks through the project in the order an interviewer usually asks
about it. Read it with the code open. Each section ends with questions you
should be able to answer without notes.

---

## 1. The one-minute pitch

> "I built a limit order book matching engine in C++ that processes about 20
> million messages a second, and used it to run a market-making study. I first
> replicated the Avellaneda–Stoikov paper's published results to prove my
> simulation code was right. Then I built an agent-based market with informed
> traders and tested four market-making strategies out of sample. The main
> findings: the textbook fill model overstates fills 7.5x because it ignores
> queues, inventory-aware quoting turns a strategy that lost in all 200 test
> hours into one that never lost, and latency decides everything. A plain
> touch-joining maker stops making money at about 6 ms of delay, while one that
> reacts to a leading signal survives up to roughly 900 ms."

---

## 2. What a limit order book is

* A **limit order** says "buy 5 lots at 100 or better". If nobody sells at 100
  or lower, it **rests** in the book and waits.
* The **best bid** is the highest resting buy price, the **best ask** the lowest
  resting sell price. Together they are **the touch**; the gap is the **spread**.
* A **market order** (or a marketable limit order) trades immediately against
  resting orders. The resting side is the **maker**, the aggressive side the
  **taker**. Trades happen at the maker's price.
* **Price-time priority**: better price first; at the same price, whoever
  arrived first is filled first. Your place in line is your **queue position**.

Questions: Why does the maker set the trade price? What happens to the part of
an IOC order that can't fill? Why does modifying an order's price lose its
queue position but reducing its size doesn't?

---

## 3. The matching engine (`include/tickforge/order_book.hpp`)

Four design choices make it fast. Know why each one matters:

1. **Prices are array indices.** Prices are integer ticks inside a fixed range,
   so finding the level at price `p` is `levels[p - min_price]`: O(1), no tree.
2. **Orders live in a preallocated pool**, linked into their level's queue by
   *indices* (an intrusive doubly linked list). Add, cancel and fill are O(1)
   and allocate nothing once the pool is warm. Memory allocation is the
   slowest thing a hot path can do.
3. **Order ids encode the pool slot plus a generation counter.** Looking up an
   order needs no hash map. When a slot is reused, its generation increases, so
   an old id (for an order that already filled) can't cancel the new order in
   that slot. That bug would be catastrophic in a real exchange.
4. **A two-level bitmap** (`level_bitmap.hpp`) marks which levels are non-empty.
   When the best level empties, we find the next one by scanning 64 levels per
   machine instruction (`countr_zero` / `countl_zero`) and skipping empty
   words with a summary bitmap.

**How we know it is correct:** `reference_book.hpp` is a deliberately simple
book (std::map + std::deque) that shares no code with the fast one. The test
feeds both books 800,000 random orders, cancels and reductions, and checks that
every fill and the whole book state match after every single step. A second,
pure-Python book (`python/tickforge/reference_book.py`) does the same check from
Python. Tests also run under AddressSanitizer and UndefinedBehaviorSanitizer
in CI.

**Benchmark** (`bench/bench_order_book.cpp`): 5 million messages (55% adds,
38% cancels, 7% market orders). Throughput is the best of 3 runs; latency
percentiles come from timing each message. The std::map book is the baseline.

Questions: Why is a std::map book slower even though both are "fast enough"
asymptotically? (Pointer chasing, allocation per order, cache misses.) What
does p99 latency tell you that the average doesn't? Why do the timing numbers
include ~20 ns of clock overhead?

---

## 4. The Avellaneda–Stoikov replication (`python/tickforge/as_model.py`)

The 2008 paper models a market maker who quotes a bid and an ask around the
mid price and wants to avoid holding inventory.

* **Reservation price** `r = s − q·γ·σ²·(T − t)`: if you are long (q > 0),
  your personal "fair value" is below the mid, so you quote lower to sell more
  and buy less.
* **Optimal spread** `γσ²(T − t) + (2/γ)·ln(1 + γ/k)`.
* **Fill model**: a quote δ away from the mid fills with intensity
  `A·exp(−kδ)`: the further away, the rarer the fill.

We rerun their exact experiment (1,000 simulations per row) and compare with
their Tables 1–3. Every mean profit is within 1.3 standard errors of the
published value. That proves the simulation code, not the paper.

Key insight to say out loud: the inventory strategy earns slightly *less*
than the symmetric one but with about half the risk. Market making is a
risk-management problem, not a prediction problem.

Questions: What does γ (gamma) represent? What happens to the spread as γ → 0?
Why do we use **common random numbers** (the same price paths for both
strategies)?

---

## 5. GLFT: the version used in the simulator

Guéant, Lehalle & Fernandez-Tapia (2013) solved the same problem without a
fixed end time and gave a closed-form approximation
(`compute_quotes` in `src/market_sim.cpp`):

    δ_bid = c1 + (2q + 1)/2 · ω
    δ_ask = c1 − (2q − 1)/2 · ω
    c1 = ln(1 + γ/κ) / γ
    ω  = sqrt( σ²γ / (2κA) · (1 + γ/κ)^(1 + κ/γ) )

`c1` is the base half-spread, and `ω` is how far quotes shift per lot of
inventory. Higher γ means bigger shifts and tighter inventory control.

---

## 6. The simulated market (`src/market_sim.cpp`)

* A hidden **fundamental value V** jumps ±1 tick at random (0.1 jumps/second).
* **Liquidity providers** post limit orders near the touch and cancel them at
  random. They don't know V.
* **Noise traders** send random market orders. They are who market makers
  earn from.
* **Informed traders** know V. If the best ask is below V they buy everything
  under V, and symmetrically for sells. They arrive 5 times a second (a 200 ms
  mean reaction time). They are who market makers lose to: **adverse
  selection**.
* **The market maker** decides every 10 ms. Its orders reach the book after a
  **latency** L, and it joins the back of the queue. It may watch a **lead
  signal**: V as seen in a leading market, like NIFTY futures moving before a
  stock.

Time is continuous. Background events are exact Poisson processes
(Gillespie's method): draw the time to the next event from an exponential
distribution, then pick which event by its share of the total rate. Scheduled
events (timer, delayed orders) interleave with them. Throwing away a drawn
exponential time when a scheduled event comes first is exact because Poisson
arrivals are **memoryless**.

The market was tuned so informed traders take about 80% of what noise
traders lose, leaving the background liquidity providers a thin margin. That
is close to the competitive (Glosten–Milgrom) benchmark where liquidity
providers earn nothing, so a strategy only profits if it is better than
average.

Questions: Why do informed traders hurt market makers? What does "memoryless"
mean, and why does it matter here? Why did we tune the market so liquidity
providers earn only a thin margin?

---

## 7. The study protocol (`python/tickforge/study.py`)

Same discipline as honest-alpha:

1. **Calibrate** σ, A and κ from sessions with no market maker, using only
   the public tape (`calibration.py`).
2. **Tune** γ and the imbalance weight on **training** seeds only.
3. **Test** on 200 unseen one-hour sessions with bootstrap 95% confidence
   intervals and **paired** comparisons (every strategy faces the same seeds).
4. **Sweep latency** on 100 more unseen sessions.

Metrics you must be able to define:

* **Markout(h)**: `side × (mid h seconds after the fill − fill price)` per
  lot. At h = 0 it's the spread you captured. If it falls as h grows, prices
  moved against you after you traded: adverse selection.
* **Informed share**: the fraction of your filled lots that traded against
  informed traders.
* **Queue haircut**: fills the A·exp(−κδ) model predicts for your quotes vs
  fills you actually got. The model assumes any trade at your price fills
  you. In a queue you are filled only after everyone ahead of you.

---

## 8. Results and how to explain them

The README has the exact numbers. Explain the mechanisms:

* **The textbook fill model overstates fills 7.5x.** Calibrated trade-through
  rates assume you're first in line. You're usually last, and the orders that
  reach the back of the queue are disproportionately informed sweeps.
* **Symmetric quoting loses at 20 ms latency; GLFT wins.** Joining the touch
  on both sides with no inventory control lets informed flow push you into
  big losing positions. GLFT skews quotes away from inventory, cutting
  per-minute P&L volatility about 7x.
* **The lead signal adds about 55% to GLFT's profit.** The maker moves its
  quotes before the arbitrageurs arrive, so only 14% of its lots trade
  against informed traders (25% for plain GLFT).
* **Latency is the strategy.** Each strategy has a latency where it stops
  making money. With delay, the maker acts on a book that has already
  changed: it re-posts stale prices and churns orders, losing queue priority.
  That's the HFT "arms race" (Budish, Cramton & Shim 2015) in one chart.

**Limitations (say these before you're asked):** the market is stylized
(unit tick, ±1-tick jumps, one fast maker, no fees or rebates, no competing
HFTs), so absolute P&L and Sharpe numbers are not realistic. The comparisons
and mechanisms are the result. Hourly Sharpe ratios are very high because a
single fast maker faces slow background liquidity. Real markets compete
that edge away.

---

## 9. Likely interview questions

1. Walk me through what happens inside `add_limit` when a buy order crosses
   two price levels.
2. How would you make the engine handle multiple instruments? (One book per
   instrument; route by symbol id; keep books on separate cores.)
3. What would you change to run this on real NSE data? (Replay historical
   order messages, estimate queue position from the feed, add fees/STT, model
   your own latency.)
4. Why is high γ optimal here when the paper uses γ = 0.1? (Units differ:
   prices are in ticks and positions in lots. And adverse selection makes
   holding inventory more dangerous than the paper's random-walk world.)
5. Is the queue-imbalance signal useful in real markets? (Yes, it's
   documented, e.g. Cartea, Donnelly & Jaimungal. Here it adds a little, because
   price moves come from hidden jumps the book can't foresee.)
6. How would you check the simulator itself is right? (Accounting identities,
   determinism tests, informed traders only trade when profitable, reproducing
   a known paper.)
