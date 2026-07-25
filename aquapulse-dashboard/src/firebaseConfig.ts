import { initializeApp } from "firebase/app";
import { getFirestore } from "firebase/firestore";

// These values are safe to expose in a client app. They identify the
// Firebase project, they are not secret keys. Actual access control comes
// from your Firestore security rules, not from hiding this object.
const firebaseConfig = {
  apiKey: "AIzaSyDy_2nKdJISxgKskblyVuCAQYdujH1Uc0E",
  authDomain: "smartfishfeeder-25c86.firebaseapp.com",
  projectId: "smartfishfeeder-25c86",
  storageBucket: "smartfishfeeder-25c86.firebasestorage.app",
  messagingSenderId: "174600467570",
  appId: "1:174600467570:web:1081a606335e062ac9beeb",
};

// The device document ID this dashboard talks to. Must match DEVICE_ID in
// the ESP32 firmware exactly.
export const DEVICE_ID = "feeder_01";

const app = initializeApp(firebaseConfig);
export const db = getFirestore(app);
