import pandas as pd
import matplotlib
matplotlib.use('Agg')  # Headless background render
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
PAPER_PURPLE = "#7C6089"   # blue+pink blend, used for net/derived series

print("Loading simulation output for liquidity analysis...")
df = pd.read_csv("sim_output.csv")

# Downsample to 1500 points for fast, clean rendering
if len(df) > 1500:
    df = df.iloc[::len(df) // 1500].reset_index(drop=True)

df["time_sec"] = (df["timestamp"] - df["timestamp"].iloc[0]) / 1e9

# --- DYNAMIC WARMUP SLICE ---
# Safely skip the first 10% of the simulation to bypass the empty-book phase
warmup_cutoff = len(df) // 10
df = df.iloc[warmup_cutoff:].reset_index(drop=True)

# Downsample to 1500 points for fast, clean rendering
if len(df) > 1500:
    df = df.iloc[::len(df) // 1500].reset_index(drop=True)

# Create 2-panel liquidity chart
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(14, 8), sharex=True)
fig.patch.set_facecolor("white")
fig.suptitle("LOB Resting Liquidity & Order Book Depth Evolution", fontsize=16, fontweight="bold")

# --- Panel 1: Total Resting Shares on Bid vs Ask Sides ---
ax1.plot(df["time_sec"], df["buy_shares"], label="Total Resting Bid Shares",
          color=PAPER_GREEN, linewidth=1.2, alpha=0.85)
ax1.plot(df["time_sec"], df["sell_shares"], label="Total Resting Ask Shares",
          color=PAPER_PINK, linewidth=1.2, alpha=0.85)
ax1.set_ylabel("Resting Volume (Shares)")
ax1.legend(loc="upper left", framealpha=0.9)
ax1.grid(True, linestyle="--", alpha=0.4, color=PAPER_GRAY)

# --- Panel 2: Net Book Imbalance (Buy Shares - Sell Shares) ---
net_shares = df["buy_shares"] - df["sell_shares"]
ax2.plot(df["time_sec"], net_shares, label="Net Book Liquidity (Bid Shares - Ask Shares)",
          color=PAPER_PURPLE, linewidth=1)
ax2.axhline(0, color=PAPER_GRAY, linestyle=":", linewidth=1)
ax2.set_ylabel("Net Share Delta")
ax2.set_xlabel("Simulation Time (Seconds)")
ax2.legend(loc="upper left", framealpha=0.9)
ax2.grid(True, linestyle="--", alpha=0.4, color=PAPER_GRAY)

plt.tight_layout()
plt.savefig("liquidity_depth_analysis.png", dpi=300)
print("SUCCESS: Liquidity analysis saved to 'liquidity_depth_analysis.png'")