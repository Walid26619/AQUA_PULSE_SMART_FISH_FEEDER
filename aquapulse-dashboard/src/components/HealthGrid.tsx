import type { DeviceDoc } from "../types";

interface Props {
  device: DeviceDoc;
}

function Item({ label, ok, value }: { label: string; ok?: boolean; value: string }) {
  return (
    <div className="health-item">
      <div className="label">{label}</div>
      <div className="value" style={{ color: ok === false ? "var(--barn-red)" : "var(--soil)" }}>
        {value}
      </div>
    </div>
  );
}

export function HealthGrid({ device }: Props) {
  const h = device.health ?? {};

  return (
    <div className="panel">
      <h2>System health</h2>
      <div className="health-grid">
        <Item
          label="LCD display"
          ok={h.lcdScreen?.working}
          value={h.lcdScreen?.working ? "Working" : "Not responding"}
        />
        <Item
          label="RTC clock"
          ok={h.rtcModule?.synced}
          value={h.rtcModule?.synced ? `Synced, ${h.rtcModule?.deviceTime ?? ""}` : "Not synced"}
        />
        <Item
          label="Hopper sensor"
          ok={h.ultrasonicSensor?.working}
          value={
            h.ultrasonicSensor?.working
              ? `${h.ultrasonicSensor?.lastMeasuredLevel ?? "unknown"}`
              : "Not responding"
          }
        />
        <Item
          label="Stepper motor"
          ok={h.stepperMotor?.status !== "error"}
          value={h.stepperMotor?.status ?? "unknown"}
        />
        <Item
          label="Button"
          ok={h.buttons?.functional}
          value={h.buttons?.lastPressed ?? "never"}
        />
        <Item
          label="WiFi signal"
          ok={h.network?.wifiSignal !== "none"}
          value={`${h.network?.wifiSignal ?? "unknown"}, ${h.network?.ipAddress ?? ""}`}
        />
      </div>
    </div>
  );
}
