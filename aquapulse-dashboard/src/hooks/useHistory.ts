import { useEffect, useState } from "react";
import { collection, onSnapshot, orderBy, query, limit } from "firebase/firestore";
import { db, DEVICE_ID } from "../firebaseConfig";
import type { HistoryItem } from "../types";

export function useHistory(maxItems = 25) {
  const [history, setHistory] = useState<HistoryItem[]>([]);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    const ref = collection(db, "devices", DEVICE_ID, "history");
    const q = query(ref, orderBy("timestamp", "desc"), limit(maxItems));
    const unsubscribe = onSnapshot(q, (snap) => {
      setHistory(
        snap.docs.map((d) => ({ id: d.id, ...d.data() } as HistoryItem))
      );
      setLoading(false);
    });
    return () => unsubscribe();
  }, [maxItems]);

  return { history, loading };
}
