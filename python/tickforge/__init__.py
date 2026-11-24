"""tickforge: C++ matching engine, market simulator and market-making study."""

from tickforge._core import (  # noqa: F401
    AddResult,
    Fill,
    MakerConfig,
    MarketConfig,
    OrderBook,
    Quotes,
    Side,
    Strategy,
    Tif,
    compute_quotes,
    simulate,
)

__version__ = "0.1.0"
