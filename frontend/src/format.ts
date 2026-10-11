export const fmt = {
  int: (x: number | null | undefined) => (x == null || !Number.isFinite(x) ? "–" : Math.round(x).toLocaleString()),
  num: (x: number | null | undefined, digits = 1) =>
    x == null || !Number.isFinite(x) ? "–" : x.toLocaleString(undefined, { minimumFractionDigits: digits, maximumFractionDigits: digits }),
  secs: (x: number | null | undefined) => (x == null || !Number.isFinite(x) ? "–" : `${x.toFixed(1)} s`),
  clock: (t: number) => {
    const m = Math.floor(t / 60);
    const s = Math.floor(t % 60);
    return `${m}:${s.toString().padStart(2, "0")}`;
  },
  pct: (x: number | null | undefined) => (x == null || !Number.isFinite(x) ? "n/a" : `${x > 0 ? "+" : ""}${x.toFixed(0)}%`),
  signed: (x: number, digits = 1) => `${x > 0 ? "+" : x < 0 ? "−" : ""}${Math.abs(x).toFixed(digits)}`,
};

export const controllerNames: Record<string, string> = {
  fixed_time: "Fixed-time",
  adaptive: "Adaptive",
  max_pressure: "Max-pressure",
};
export const controllerName = (c: string) => controllerNames[c] ?? c;
