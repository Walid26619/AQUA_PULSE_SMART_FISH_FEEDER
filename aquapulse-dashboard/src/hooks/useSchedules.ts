import { useEffect, useState } from "react";
import {
  addDoc,
  collection,
  deleteDoc,
  doc,
  onSnapshot,
  updateDoc,
} from "firebase/firestore";
import { db, DEVICE_ID } from "../firebaseConfig";
import type { ScheduleItem } from "../types";

export function useSchedules() {
  const [schedules, setSchedules] = useState<ScheduleItem[]>([]);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    const ref = collection(db, "devices", DEVICE_ID, "schedules");
    const unsubscribe = onSnapshot(ref, (snap) => {
      setSchedules(
        snap.docs.map((d) => ({ id: d.id, ...d.data() } as ScheduleItem))
      );
      setLoading(false);
    });
    return () => unsubscribe();
  }, []);

  async function addSchedule(time: string, portion: number, days: string[]) {
    const ref = collection(db, "devices", DEVICE_ID, "schedules");
    await addDoc(ref, { time, portion, days, enabled: true });
  }

  async function toggleSchedule(id: string, enabled: boolean) {
    const ref = doc(db, "devices", DEVICE_ID, "schedules", id);
    await updateDoc(ref, { enabled });
  }

  async function removeSchedule(id: string) {
    const ref = doc(db, "devices", DEVICE_ID, "schedules", id);
    await deleteDoc(ref);
  }

  return { schedules, loading, addSchedule, toggleSchedule, removeSchedule };
}
