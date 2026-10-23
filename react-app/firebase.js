import { initializeApp } from 'firebase/app';
import { getDatabase } from 'firebase/database';

const firebaseConfig = {
  apiKey: "jFDOKRsisKLcOHyNFk0CCa6Na1hcJIm43WlVA1o2",
  authDomain: "ucf-jr.firebaseapp.com",
  databaseURL: "https://ucf-jr-default-rtdb.firebaseio.com",
  projectId: "ucf-jr",
};

const app = initializeApp(firebaseConfig);
export const db = getDatabase(app);
