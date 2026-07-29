import { useState } from "react";
import { useSchedules } from "../hooks/useSchedules";
import { DAY_ORDER } from "../types";
import { computePortionsPerFeeding, type FarmProfile } from "../lib/feedingCalculator";

interface Props {
  farmProfile?: FarmProfile;
}

export function ScheduleManager({ farmProfile }: Props) {
  const { schedules, loading, addSchedule, toggleSchedule, removeSchedule } = useSchedules();
  const [time, setTime] = useState("08:00");
  const [days, setDays] = useState<string[]>(["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]);
  const [saving, setSaving] = useState(false);

  const portion = farmProfile ? computePortionsPerFeeding(farmProfile) : 1;

  function toggleDay(day: string) {
    setDays((prev) => (prev.includes(day) ? prev.filter((d) => d !== day) : [...prev, day]));
  }

  async function handleAdd() {
    if (days.length === 0) return;
    setSaving(true);
    try {
      await addSchedule(time, portion, days);
      setTime("08:00");
    } finally {
      setSaving(false);
    }
  }

  const sorted = [...schedules].sort((a, b) => (a.time ?? "").localeCompare(b.time ?? ""));

  return (
    <div className="panel">
      <h2>Feeding schedule</h2>

      <p style={{ fontSize: 13, color: "var(--text-muted)", marginBottom: 12 }}>
        Each scheduled feeding dispenses <strong>{portion} portion{portion > 1 ? "s" : ""}</strong>, calculated from your farm profile below.
      </p>

      <div className="schedule-form">
        <div className="field">
          <label htmlFor="sched-time">Time</label>
          <input id="sched-time" type="time" value={time} onChange={(e) => setTime(e.target.value)} />
        </div>
        <div className="field">
          <label>Days</label>
          <div className="day-toggles">
            {DAY_ORDER.map((day) => (
              <button
                key={day}
                type="button"
                className={`day-toggle ${days.includes(day) ? "active" : ""}`}
                onClick={() => toggleDay(day)}
              >
                {day[0]}
              </button>
            ))}
          </div>
        </div>
        <button className="btn sage" onClick={handleAdd} disabled={saving}>
          {saving ? "Adding..." : "Add schedule"}
        </button>
      </div>

      {loading && <p className="empty-state">Loading schedules.</p>}
      {!loading && sorted.length === 0 && (
        <p className="empty-state">No feeding schedules set yet. Add one above.</p>
      )}

      {sorted.map((s) => (
        <div className="schedule-row" key={s.id}>
          <div>
            <div className="schedule-time">{s.time}</div>
            <div className="schedule-meta">
              {s.portion ?? 1} portion{(s.portion ?? 1) > 1 ? "s" : ""}, {(s.days ?? []).join(", ")}
            </div>
          </div>
          <div className="schedule-actions">
            <label style={{ fontSize: 12, display: "flex", alignItems: "center", gap: 6 }}>
              <input
                type="checkbox"
                checked={s.enabled ?? true}
                onChange={(e) => toggleSchedule(s.id, e.target.checked)}
              />
              Enabled
            </label>
            <button className="btn secondary" onClick={() => removeSchedule(s.id)}>
              Remove
            </button>
          </div>
        </div>
      ))}
    </div>
  );
}
