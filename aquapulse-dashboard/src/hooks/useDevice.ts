import { useEffect, useState } from "react";
import { doc, onSnapshot } from "firebase/firestore";
import { db, DEVICE_ID } from "../firebaseConfig";
import type { DeviceDoc } from "../types";

export function useDevice() {
  const [device, setDevice] = useState<DeviceDoc | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    const ref = doc(db, "devices", DEVICE_ID);
    const unsubscribe = onSnapshot(
      ref,
      (snap) => {
        setDevice(snap.exists() ? (snap.data() as DeviceDoc) : null);
        setLoading(false);
      },
      (err) => {
        setError(err.message);
        setLoading(false);
      }
    );
    return () => unsubscribe();
  }, []);

  return { device, loading, error };
}
