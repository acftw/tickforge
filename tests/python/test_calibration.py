import numpy as np
import tickforge as tf
from tickforge.calibration import calibrate, estimate_sigma, fit_intensity


def test_fit_recovers_known_intensity():
    d = np.array([0.5, 1.0, 1.5, 2.0, 2.5])
    A, k = fit_intensity(d, 7.0 * np.exp(-2.5 * d))
    assert np.isclose(A, 7.0) and np.isclose(k, 2.5)


def test_sigma_of_random_walk():
    rng = np.random.default_rng(0)
    t = np.arange(0, 3600, 0.1)
    mid = np.cumsum(rng.normal(0, 0.4 * np.sqrt(0.1), len(t)))
    assert abs(estimate_sigma(t, mid) / 0.4 - 1) < 0.05


def test_calibrate_on_simulator():
    m = tf.MarketConfig()
    m.duration = 900
    c = calibrate(m, [0, 1])
    assert c["sigma"] > 0 and c["A"] > 0 and c["kappa"] > 0
    rates = np.array(c["trade_through_rates"])
    assert np.all(np.diff(rates) <= 0)  # fewer trades reach further from the mid
