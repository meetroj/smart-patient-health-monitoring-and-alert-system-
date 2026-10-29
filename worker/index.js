import { alertMessages, isFresh } from "./alert.js";

const DB = "https://patient-76db1-default-rtdb.firebaseio.com";
const API_KEY = "AIzaSyB65xdbKWK3eN2Bne-KG4I570cgtMuKA3U";   // public Firebase web key
const WORKER_EMAIL = "worker@patient-monitor.app";   // its uid is hard-coded in database.rules.json
const FROM = "Patient Monitor <onboarding@resend.dev>";      // only delivers to your Resend account email until a domain is verified
const COOLDOWN_MS = 10 * 60 * 1000;                          // max one alert email per 10 min

async function run(env) {
  const login = await (await fetch(`https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=${API_KEY}`, {
    method: "POST",
    body: JSON.stringify({ email: WORKER_EMAIL, password: env.WORKER_PASSWORD, returnSecureToken: true }),
  })).json();
  if (!login.idToken) throw new Error("Firebase login failed: " + JSON.stringify(login));
  const get = async (p) => (await fetch(`${DB}/${p}.json?auth=${login.idToken}`)).json();

  const live = await get("live");
  if (!isFresh(live)) return;

  // history: one row per run, keyed by reading time (same reading twice = same row)
  // ponytail: grows ~1440 rows/day, add pruning of old keys when it matters
  const msgs = alertMessages(live);
  const { bpm, bpmStatus, temperature, tempStatus, ts } = live;
  await fetch(`${DB}/history/${ts}.json?auth=${login.idToken}`, {
    method: "PUT",
    body: JSON.stringify({ bpm, bpmStatus, temperature: temperature ?? null, tempStatus: tempStatus ?? null, ts, alert: msgs.length > 0 }),
  });
  if (!msgs.length) return;

  const last = (await get("alertState/lastSent")) || 0;
  if (Date.now() - last < COOLDOWN_MS) return;
  const put = (v) => fetch(`${DB}/alertState/lastSent.json?auth=${login.idToken}`, { method: "PUT", body: JSON.stringify(v) });
  await put(Date.now());   // claim the slot before sending

  const users = (await get("users")) || {};
  const to = [...new Set(Object.values(users).map((u) => u && u.emergencyEmail).filter(Boolean))];
  if (!to.length) return;

  const res = await fetch("https://api.resend.com/emails", {
    method: "POST",
    headers: { Authorization: `Bearer ${env.RESEND_API_KEY}`, "Content-Type": "application/json" },
    body: JSON.stringify({
      from: FROM, to,
      subject: "⚠ Patient Monitor emergency alert",
      html: `<h2>Emergency alert</h2><ul>${msgs.map((m) => `<li>${m}</li>`).join("")}</ul><p>${new Date().toUTCString()}</p>`,
    }),
  });
  if (!res.ok) { await put(last); throw new Error("Resend failed " + res.status + " " + await res.text()); }   // retry next minute
}

export default { scheduled: (_e, env, ctx) => ctx.waitUntil(run(env)) };
