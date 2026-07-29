import { useState, useEffect } from "react";
import { doc, updateDoc } from "firebase/firestore";
import { db, DEVICE_ID } from "../firebaseConfig";
import {
  SPECIES_OPTIONS,
  DEFAULT_FARM_PROFILE,
  computePortionsPerFeeding,
  getTotalDailyGrams,
  type FarmProfile as FarmProfileType,
} from "../lib/feedingCalculator";
import type { DeviceDoc } from "../types";

interface Props {
  device: DeviceDoc;
}

export function FarmProfile({ device }: Props) {
  const saved = device.farmProfile;
  const [species, setSpecies] = useState(saved?.species ?? DEFAULT_FARM_PROFILE.species);
  const [fishCount, setFishCount] = useState(saved?.fishCount ?? DEFAULT_FARM_PROFILE.fishCount);
  const [feedingsPerDay, setFeedingsPerDay] = useState(saved?.feedingsPerDay ?? DEFAULT_FARM_PROFILE.feedingsPerDay);
  const [gramsPerPortion, setGramsPerPortion] = useState(saved?.gramsPerPortion ?? DEFAULT_FARM_PROFILE.gramsPerPortion);
  const [saving, setSaving] = useState(false);
  const [savedMessage, setSavedMessage] = useState(false);

  useEffect(() => {
    if (saved) {
      setSpecies(saved.species ?? DEFAULT_FARM_PROFILE.species);
      setFishCount(saved.fishCount ?? DEFAULT_FARM_PROFILE.fishCount);
      setFeedingsPerDay(saved.feedingsPerDay ?? DEFAULT_FARM_PROFILE.feedingsPerDay);
      setGramsPerPortion(saved.gramsPerPortion ?? DEFAULT_FARM_PROFILE.gramsPerPortion);
    }
  }, [saved?.species, saved?.fishCount, saved?.feedingsPerDay, saved?.gramsPerPortion]);

  const profile: FarmProfileType = { species, fishCount, feedingsPerDay, gramsPerPortion };
  const totalDailyGrams = getTotalDailyGrams(profile);
  const portionsPerFeeding = computePortionsPerFeeding(profile);

  async function handleSave() {
    setSaving(true);
    try {
      const ref = doc(db, "devices", DEVICE_ID);
      await updateDoc(ref, {
        farmProfile: { species, fishCount, feedingsPerDay, gramsPerPortion },
      });
      setSavedMessage(true);
      setTimeout(() => setSavedMessage(false), 2500);
    } finally {
      setSaving(false);
    }
  }

  return (
    <div className="panel">
      <h2>Farm profile</h2>
      <p style={{ fontSize: 13, color: "var(--text-muted)", marginBottom: 16 }}>
        Tell the feeder about your fish, and it works out how much to dispense
        each feeding on its own. No need to set a portion count yourself.
      </p>

      <div className="schedule-form" style={{ flexWrap: "wrap" }}>
        <div className="field">
          <label htmlFor="species">Fish species</label>
          <select
            id="species"
            value={species}
            onChange={(e) => setSpecies(e.target.value)}
            style={{
              border: "1px solid var(--border)",
              borderRadius: 3,
              padding: "7px 8px",
              fontSize: 14,
              fontFamily: "Helvetica, Arial, sans-serif",
              minWidth: 170,
            }}
          >
            {SPECIES_OPTIONS.map((s) => (
              <option key={s.value} value={s.value}>
                {s.label}
              </option>
            ))}
          </select>
        </div>

        <div className="field">
          <label htmlFor="fishCount">Number of fish</label>
          <input
            id="fishCount"
            type="number"
            min={0}
            value={fishCount}
            onChange={(e) => setFishCount(Math.max(0, Number(e.target.value)))}
            style={{ width: 100 }}
          />
        </div>

        <div className="field">
          <label htmlFor="feedingsPerDay">Feedings per day</label>
          <input
            id="feedingsPerDay"
            type="number"
            min={1}
            max={6}
            value={feedingsPerDay}
            onChange={(e) => setFeedingsPerDay(Math.min(6, Math.max(1, Number(e.target.value))))}
            style={{ width: 100 }}
          />
        </div>

        <div className="field">
          <label htmlFor="gramsPerPortion">Grams per portion</label>
          <input
            id="gramsPerPortion"
            type="number"
            min={1}
            value={gramsPerPortion}
            onChange={(e) => setGramsPerPortion(Math.max(1, Number(e.target.value)))}
            style={{ width: 100 }}
          />
        </div>

        <button className="btn sage" onClick={handleSave} disabled={saving}>
          {saving ? "Saving..." : "Save profile"}
        </button>
      </div>

      <p style={{ fontSize: 12, color: "var(--text-muted)", marginBottom: 14 }}>
        Grams per portion is how much food one dispense (one stepper rotation)
        actually releases. Weigh one manual feed to find this, feeding rates
        by species are approximate estimates, adjust if you have better data.
      </p>

      <div className="status-row">
        <span className="pill neutral">Total daily feed: {totalDailyGrams}g</span>
        <span className="pill ok">{portionsPerFeeding} portion(s) per feeding</span>
        {savedMessage && <span className="pill ok">Saved</span>}
      </div>
    </div>
  );
}
