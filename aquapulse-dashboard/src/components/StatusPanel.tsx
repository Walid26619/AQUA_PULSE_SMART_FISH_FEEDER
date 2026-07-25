import { useEffect, useState } from "react";
import type { DeviceDoc } from "../types";
import { CapacityGauge } from "./CapacityGauge";

interface Props {
  device: DeviceDoc;
}

const STALE_THRESHOLD_MS = 25000;

function formatTimestamp(ts?: { seconds: number } | null) {
  if (!ts) return "never";
  const date = new Date(ts.seconds * 1000);
  return date.toLocaleString();
}

export function computeIsOnline(device: DeviceDoc, now: number): boolean {
  if (!device.lastCommunication) return false;
  const lastMs = device.lastCommunication.seconds * 1000;
  return now - lastMs < STALE_THRESHOLD_MS;
}

export function StatusPanel({ device }: Props) {
  const [now, setNow] = useState(Date.now());

  useEffect(() => {
    const interval = setInterval(() => setNow(Date.now()), 5000);
    return () => clearInterval(interval);
  }, []);

  const isOnline = computeIsOnline(device, now);
  const capacity = device.capacity ?? 0;
  const hasAlert = isOnline && device.healthSummary?.toLowerCase().startsWith("alert");

  return (
    <div className="panel">
      <h2>Feeder status</h2>
      <div className="status-row" style={{ marginBottom: 16 }}>
        <span className={`pill ${isOnline ? "ok" : "bad"}`}>
          <span className="dot" />
          {isOnline ? "Online" : "Offline"}
        </span>
        {isOnline && (
          <span className={`pill ${hasAlert ? "bad" : "ok"}`}>
            {hasAlert ? "Alert" : "Normal"}
          </span>
        )}
        <span className="pill neutral">
          Last contact: {formatTimestamp(device.lastCommunication)}
        </span>
      </div>

      <CapacityGauge capacity={capacity} />

      <p style={{ marginTop: 14, fontSize: 13, color: "var(--text-muted)" }}>
        {isOnline
          ? device.healthSummary ?? "No status reported yet."
          : "Feeder has not reported in recently. It may be powered off or disconnected."}
      </p>

      {device.lastFeedTime && (
        <p style={{ fontSize: 13, color: "var(--text-muted)" }}>
          Last feed: {formatTimestamp(device.lastFeedTime)}, quantity {device.lastFeedQuantity ?? 1}
        </p>
      )}
    </div>
  );
}