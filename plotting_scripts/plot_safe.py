import pandas as pd
import numpy as np
import matplotlib
matplotlib.use('Agg')  # Forces background rendering - NO CHROME CRASHES
import matplotlib.pyplot as plt

# ---------------------------------------------------------------------
# Paper color theme (companions of the tikz soft-fill colors used in
# the architecture diagrams: softblue, softgreen, softorange, softgray,
# softpink). These are darker/saturated versions of the same hues so
# they stay legible as line colors while matching the paper's palette.
# ---------------------------------------------------------------------
PAPER_GREEN  = "#4F8F6B"   # companion of softgreen  {224,239,230}
PAPER_PINK   = "#B85C7A"   # companion of softpink   {245,225,232}
PAPER_BLUE   = "#3B6EA5"   # companion of softblue   {221,232,244}
PAPER_ORANGE = "#C98A3D"   # companion of softorange {245,233,216}
PAPER_GRAY   = "#54595F"   # companion of softgray   {238,240,242}

print("Loading CSV data in background...")
df = pd.read_csv("sim_output.csv")

# Downsample to 1500 points to keep memory footprint tiny
if len(df) > 1500:
    df = df.iloc[::len(df) // 1500].reset_index(drop=True)

FACTOR = 10000.0  # Converts 10,000 integer units to $1.00 USD
df["time_sec"] = (df["timestamp"] - df["timestamp"].iloc[0]) / 1e9

# --- DYNAMIC WARMUP SLICE ---
warmup_cutoff = len(df) // 10
df = df.iloc[warmup_cutoff:].reset_index(drop=True)

# Downsample to 1500 points to keep memory footprint tiny
if len(df) > 1500:
    df = df.iloc[::len(df) // 1500].reset_index(drop=True)

price_cols = ["best_bid", "best_ask", "mid_price", "last_trade_price"]
df[price_cols] = df[price_cols].replace(0, np.nan) / FACTOR
df["spread_usd"] = df["spread"] / FACTOR

fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(14, 10), sharex=True)
fig.patch.set_facecolor("white")
fig.suptitle("LOB Market State (Clean USD Scale)", fontsize=16, fontweight="bold")

ax1.plot(df["time_sec"], df["best_bid"], label="Best Bid", color=PAPER_GREEN, alpha=0.8, linewidth=1)
ax1.plot(df["time_sec"], df["best_ask"], label="Best Ask", color=PAPER_PINK, alpha=0.8, linewidth=1)
ax1.plot(df["time_sec"], df["mid_price"], label="Mid Price", color=PAPER_BLUE, linewidth=1.5)
ax1.set_ylabel("Price ($ USD)")
ax1.legend(loc="upper left", framealpha=0.9)
ax1.grid(True, linestyle="--", alpha=0.4, color=PAPER_GRAY)

ax2.plot(df["time_sec"], df["spread_usd"], label="Spread ($ USD)", color=PAPER_ORANGE, linewidth=1.2)
ax2.set_ylabel("Spread ($ USD)", color=PAPER_ORANGE)
ax2.grid(True, linestyle="--", alpha=0.4, color=PAPER_GRAY)

ax3.plot(df["time_sec"], df["total_volume"], label="Total Traded Volume", color=PAPER_GRAY, linewidth=1.5)
ax3.set_ylabel("Cumulative Shares Traded")
ax3.set_xlabel("Simulation Time (Seconds)")
ax3.grid(True, linestyle="--", alpha=0.4, color=PAPER_GRAY)

plt.tight_layout()
plt.savefig("market_state_stabilized.png", dpi=300)
print("SUCCESS: Image generated safely and saved as 'market_state_stabilized.png'")