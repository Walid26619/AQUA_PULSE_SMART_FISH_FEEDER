interface Props {
  capacity: number;
}

export function CapacityGauge({ capacity }: Props) {
  const bars = 20;
  const filledBars = Math.round((capacity / 100) * bars);

  return (
    <div>
      <div className="status-row">
        <span className="capacity-number">{capacity}%</span>
        <span className="gauge-label" style={{ marginLeft: 8 }}>
          hopper level
        </span>
      </div>
      <div className="gauge">
        {Array.from({ length: bars }).map((_, i) => (
          <div
            key={i}
            className={`gauge-bar ${i < filledBars ? "filled" : ""}`}
            style={{ height: `${((i + 1) / bars) * 100}%` }}
          />
        ))}
      </div>
      <div className="gauge-label">
        <span>empty</span>
        <span>full</span>
      </div>
    </div>
  );
}
