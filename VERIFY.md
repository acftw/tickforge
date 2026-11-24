# Verifying tickforge on your own Mac

Takes about 10 minutes, most of it one-time setup. Works on Apple Silicon and Intel
Macs. Run every command in **Terminal**.

## 1. One-time setup

Install Apple's C++ compiler. Click **Install** in the pop-up, and skip this if it's
already installed:

```bash
xcode-select --install
```

Install uv, which sets up the right Python version and build tools for you:

```bash
curl -LsSf https://astral.sh/uv/install.sh | sh
```

Close Terminal and open a new window so `uv` is found.

You don't need Homebrew, and you don't need to install CMake yourself: the install step
fetches it automatically.

## 2. Get the project and install it

Unzip the project and go into its folder. If Safari already unzipped it into a
`tickforge` folder, skip the `unzip` line.

```bash
cd ~/Downloads
unzip tickforge.zip
cd tickforge
```

Create a Python 3.12 environment (uv downloads Python if needed), switch it on, and
install the project. The last step compiles the C++ engine and takes about a minute
the first time.

```bash
uv venv --python 3.12
source .venv/bin/activate
uv pip install ".[dev]"
```

After `source`, you should see `(tickforge)` or `(.venv)` at the start of the prompt.

If you open a new Terminal later, run `cd ~/Downloads/tickforge && source .venv/bin/activate` again.

## 3. Run the checks

| Command | What it checks | You should see |
|---|---|---|
| `pytest` | 20 Python tests, including the C++ engine vs. an independent Python order book | `20 passed` |
| `ruff check python tests/python` | Code style and common bugs | `All checks passed!` |
| `tickforge replicate-as` | The Avellaneda & Stoikov replication | six rows, every `z` between -3 and 3 |
| `tickforge all --quick` | The whole pipeline end to end on a small study | finishes in seconds; charts go to `results/img_quick/` (the README's charts are untouched) |

**C++ tests with memory-error checking (AddressSanitizer + UndefinedBehaviorSanitizer):**

```bash
uv pip install cmake ninja
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTICKFORGE_BUILD_TESTS=ON -DTICKFORGE_SANITIZE=ON
cmake --build build -j
UBSAN_OPTIONS=halt_on_error=1 ./build/tickforge_tests
```

Expect `[doctest] Status: SUCCESS!` with about 6.86 million assertions passed.

**Benchmark on your machine:**

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release -DTICKFORGE_BUILD_BENCH=ON
cmake --build build-rel -j
./build-rel/bench_order_book
```

An Apple Silicon Mac will likely be faster than the README's cloud machine. Quote
*your* numbers in interviews, and say which machine they came from.

## 4. Reproduce the full study (optional, ~2 minutes)

This runs the replication, the full study and the charts:

```bash
tickforge all
```

On Linux this reproduces `results/study.json` bit for bit. On a Mac the numbers may differ
very slightly, because macOS's math library can round the last digit of `log()`
differently, and a simulation amplifies tiny differences over an hour. The conclusions
(the order of the strategies, the latency thresholds, the 7.5x queue haircut) should
hold to within the confidence intervals. This command overwrites `results/` and
`docs/img/`, so copy those two folders somewhere first if you want to compare.

## 5. After you push to GitHub

The **Actions** tab runs the same checks automatically on every push:

* C++ tests with sanitizers on Linux (GCC and Clang) and on a real Apple Silicon Mac;
* Python 3.10, 3.12 and 3.13 on Linux and macOS.

When all of them are green, the CI badge at the top of the README turns green.

## If something fails

* `xcrun: error: invalid active developer path`: step 1's `xcode-select --install`
  hasn't finished. Run it again and wait for the installer.
* `command not found: uv`: open a new Terminal window after installing uv.
* `command not found: tickforge` or `pytest`: you're not in the virtual environment.
  Run `source .venv/bin/activate` inside the `tickforge` folder.
* `ERROR: Interceptors are not working` when running `./build/tickforge_tests`: the
  code built fine; macOS's memory checker failed to start, which happens with older
  Command Line Tools on newer macOS. Start it explicitly:

  ```bash
  DYLD_INSERT_LIBRARIES=$(clang -print-file-name=libclang_rt.asan_osx_dynamic.dylib) UBSAN_OPTIONS=halt_on_error=1 ./build/tickforge_tests
  ```

  If that still fails, run the same tests without the memory checker:

  ```bash
  cmake -S . -B build-plain -DCMAKE_BUILD_TYPE=Debug -DTICKFORGE_BUILD_TESTS=ON
  cmake --build build-plain -j
  ./build-plain/tickforge_tests
  ```

  The permanent fix is updating the Command Line Tools in **System Settings > General >
  Software Update**. GitHub's Mac runners check the memory-checker build on every push.
* Anything else: copy the full error message and send it to me.
