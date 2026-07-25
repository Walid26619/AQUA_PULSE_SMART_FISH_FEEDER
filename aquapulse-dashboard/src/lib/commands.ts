import { addDoc, collection } from "firebase/firestore";
import { db, DEVICE_ID } from "../firebaseConfig";

export async function sendFeedCommand(portion: number) {
  const ref = collection(db, "devices", DEVICE_ID, "commands");
  await addDoc(ref, { action: "feed", portion, status: "pending" });
}

export async function sendRebootCommand() {
  const ref = collection(db, "devices", DEVICE_ID, "commands");
  await addDoc(ref, { action: "reboot", status: "pending" });
}

export async function sendSelftestCommand() {
  const ref = collection(db, "devices", DEVICE_ID, "commands");
  await addDoc(ref, { action: "selftest", status: "pending" });
}
