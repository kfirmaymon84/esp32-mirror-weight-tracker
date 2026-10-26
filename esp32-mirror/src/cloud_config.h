#pragma once

// Firebase project every device talks to (Firebase console > Project settings >
// General > "Your apps" > Web app). These are PUBLIC identifiers, not secrets:
// Firebase web apps ship them to every browser. Access is controlled by each
// user's sign-in plus the Firestore rules in cloud/firestore.rules.
// Empty = accounts disabled in this build (the device works offline).
#ifndef FIREBASE_API_KEY
#define FIREBASE_API_KEY    "AIzaSyCZfBDHRfuuW7_mjWbvsF3mT_TOrpVmcdA"
#endif
#ifndef FIREBASE_PROJECT_ID
#define FIREBASE_PROJECT_ID "smart-mirror-9c760"
#endif
