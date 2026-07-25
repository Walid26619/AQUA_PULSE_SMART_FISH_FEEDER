import { useEffect, useState } from "react";
import { useDevice } from "./hooks/useDevice";
import { StatusPanel, computeIsOnline } from "./components/StatusPanel";
import { HealthGrid } from "./components/HealthGrid";
import { FeedControl } from "./components/FeedControl";
import { ScheduleManager } from "./components/ScheduleManager";
import { HistoryLog } from "./components/HistoryLog";

export default function App() {
  const { device, loading, error } = useDevice();
  const [toast, setToast] = useState<string | null>(null);
  const [now, setNow] = useState(Date.now());

  useEffect(() => {
    const interval = setInterval(() => setNow(Date.now()), 5000);
    return () => clearInterval(interval);
  }, []);

  function showToast(message: string) {
    setToast(message);
    setTimeout(() => setToast(null), 3000);
  }

  const isOnline = device ? computeIsOnline(device, now) : false;

  return (
    <div className="app-shell">
      <div className="app-header">
        <h1>AquaPulse Feeder</h1>
        <span className="subtitle">feeder_01</span>
      </div>

      {error && (
        <div className="error-banner">
          Could not read device data. This usually means Firestore security rules
          are blocking reads. Details: {error}
        </div>
      )}

      {loading && <p className="empty-state">Loading feeder data.</p>}

      {!loading && !device && !error && (
        <div className="error-banner">
          No device document found for "feeder_01". Check that the ESP32 has
          connected at least once and that the device ID matches.
        </div>
      )}

      {device && (
        <>
          <StatusPanel device={device} />
          <FeedControl onAction={showToast} disabled={!isOnline} />
          <ScheduleManager />
          <HealthGrid device={device} />
          <HistoryLog />
        </>
      )}

      {toast && <div className="toast">{toast}</div>}
    </div>
  );
}