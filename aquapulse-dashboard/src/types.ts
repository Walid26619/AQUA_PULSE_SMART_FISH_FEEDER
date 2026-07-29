export interface DeviceHealthEntry {
  [key: string]: any;
}

export interface DeviceDoc {
  connectivity?: string;
  lastCommunication?: { seconds: number; nanoseconds: number } | null;
  healthSummary?: string;
  capacity?: number;
  lastFeedTime?: { seconds: number; nanoseconds: number } | null;
  lastFeedQuantity?: number;
  config?: {
    stepperSpeed?: number;
  };
  health?: {
    esp32Board?: { responding?: boolean; uptime?: number; lastResetReason?: string };
    network?: { wifiSignal?: string; networkStatus?: string; ipAddress?: string };
    buttons?: { lastPressed?: string; functional?: boolean };
    lcdScreen?: { working?: boolean; lastMessage?: string };
    ultrasonicSensor?: { working?: boolean; rawDistanceCm?: number; lastMeasuredLevel?: string };
    rtcModule?: { synced?: boolean; deviceTime?: string };
    stepperMotor?: { status?: string; lastActuation?: string; configuredSpeed?: number };
  };
  farmProfile?: {
    species?: string;
    fishCount?: number;
    feedingsPerDay?: number;
    gramsPerPortion?: number;
  };
}

export interface HistoryItem {
  id: string;
  quantity?: number;
  source?: string;
  timestamp?: { seconds: number; nanoseconds: number } | null;
  status?: string;
}

export interface ScheduleItem {
  id: string;
  time?: string; // "HH:MM"
  enabled?: boolean;
  portion?: number;
  days?: string[]; // "Sun".."Sat"
}

export const DAY_ORDER = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
