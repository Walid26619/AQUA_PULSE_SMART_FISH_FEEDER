export interface FarmProfile {
  species: string;
  fishCount: number;
  feedingsPerDay: number;
  gramsPerPortion: number;
}

export const DEFAULT_FARM_PROFILE: FarmProfile = {
  species: "nile_tilapia",
  fishCount: 0,
  feedingsPerDay: 2,
  gramsPerPortion: 5,
};

// Approximate feed rates, grams per fish per day. These are rough estimates
// for common Uganda aquaculture species, not certified extension data.
// Refine these numbers if better local guidance is available.
export const SPECIES_OPTIONS: { value: string; label: string; gramsPerFishPerDay: number }[] = [
  { value: "nile_tilapia", label: "Nile tilapia", gramsPerFishPerDay: 4 },
  { value: "african_catfish", label: "African catfish", gramsPerFishPerDay: 7 },
  { value: "nile_perch", label: "Nile perch", gramsPerFishPerDay: 10 },
  { value: "silver_fish", label: "Silver fish (mukene)", gramsPerFishPerDay: 1 },
  { value: "other", label: "Other / unspecified", gramsPerFishPerDay: 5 },
];

export function getSpeciesRate(species: string): number {
  return SPECIES_OPTIONS.find((s) => s.value === species)?.gramsPerFishPerDay ?? 5;
}

export function getTotalDailyGrams(profile: FarmProfile): number {
  return getSpeciesRate(profile.species) * Math.max(0, profile.fishCount);
}

// The number of stepper portions to dispense per feeding, computed from the
// farm profile. This is what replaces manual portion entry everywhere.
export function computePortionsPerFeeding(profile: FarmProfile): number {
  if (profile.fishCount <= 0 || profile.gramsPerPortion <= 0 || profile.feedingsPerDay <= 0) {
    return 1; // sensible fallback if the profile isn't set up yet
  }
  const totalDailyGrams = getTotalDailyGrams(profile);
  const gramsPerFeeding = totalDailyGrams / profile.feedingsPerDay;
  const portions = Math.round(gramsPerFeeding / profile.gramsPerPortion);
  return Math.min(5, Math.max(1, portions)); // firmware caps portions at 1-5 per dispense
}
