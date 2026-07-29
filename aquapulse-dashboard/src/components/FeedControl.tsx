import { useState } from "react";
import { sendFeedCommand, sendRebootCommand, sendSelftestCommand } from "../lib/commands";
import { computePortionsPerFeeding, type FarmProfile } from "../lib/feedingCalculator";

interface Props {
  onAction: (message: string) => void;
  disabled?: boolean;
  farmProfile?: FarmProfile;
}

export function FeedControl({ onAction, disabled, farmProfile }: Props) {
  const [sending, setSending] = useState(false);

  const portion = farmProfile ? computePortionsPerFeeding(farmProfile) : 1;
  const profileNotSet = !farmProfile || farmProfile.fishCount <= 0;

  async function handleFeed() {
    setSending(true);
    try {
      await sendFeedCommand(portion);
      onAction(`Feed command sent, ${portion} portion${portion > 1 ? "s" : ""}.`);
    } finally {
      setSending(false);
    }
  }

  async function handleReboot() {
    if (!confirm("Reboot the feeder now? Any dispensing in progress will finish first.")) return;
    await sendRebootCommand();
    onAction("Reboot command sent.");
  }

  async function handleSelftest() {
    await sendSelftestCommand();
    onAction("Self test command sent. Check the device serial log for results.");
  }

  return (
    <div className="panel">
      <h2>Manual control</h2>

      {disabled && (
        <p style={{ fontSize: 13, color: "var(--barn-red)", marginBottom: 12 }}>
          Feeder is offline. Controls are disabled until it reconnects.
        </p>
      )}

      {profileNotSet && (
        <p style={{ fontSize: 13, color: "var(--text-muted)", marginBottom: 12 }}>
          Set up your farm profile below for an automatically calculated
          amount. Using a default of 1 portion until then.
        </p>
      )}

      <p style={{ fontSize: 13, color: "var(--text-muted)", marginBottom: 12 }}>
        Feed now will dispense <strong>{portion} portion{portion > 1 ? "s" : ""}</strong>, based on your farm profile.
      </p>

      <div className="button-row">
        <button className="btn sage" onClick={handleFeed} disabled={sending || disabled}>
          {sending ? "Sending..." : "Feed now"}
        </button>
        <button className="btn secondary" onClick={handleSelftest} disabled={disabled}>
          Run self test
        </button>
        <button className="btn danger" onClick={handleReboot} disabled={disabled}>
          Reboot feeder
        </button>
      </div>
    </div>
  );
}
